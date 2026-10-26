// echoes everything it receives over the Daisy Seed's micro USB port, like arduino_echo, for SerialPortTests --daisy.
// it also records the CDC requests the host's driver sends, which a real UART never sees: the line coding (rate,
// data bits, parity, stop bits) and the DTR and RTS lines. SerialPortTests asks for them in band, with a 5 byte
// command, which is echoed like any other data, and followed by the reply:
//
//   F5 5A A5 0F 'S'   reply "\x02STATE rate=115200 data=8 parity=0 stop=0 dtr=1 rts=1 ...\n"
//   F5 5A A5 0F 'X'   send XOFF (0x13), then XON (0x11) one second later
//
// the numbers are the raw CDC values: parity 0 none, 1 odd, 2 even, 3 mark, 4 space; stop 0 = 1, 1 = 1.5, 2 = 2
#include "daisy_seed.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace daisy;

namespace
{
    DaisySeed hw;

    // received data, written in the USB interrupt, read by the main loop
    constexpr uint32_t kFifoSize { 65536 };
    uint8_t fifo[kFifoSize];
    volatile uint32_t fifoWrite { 0 };
    volatile uint32_t fifoRead { 0 };
    volatile uint32_t numOverflows { 0 };

    // the CDC requests, written in the USB interrupt
    struct LineState
    {
        uint32_t rate { 0 };
        uint8_t stopBits { 0 };
        uint8_t parity { 0 };
        uint8_t dataBits { 0 };
        bool dtr { false };
        bool rts { false };
        uint32_t numLineCodings { 0 };
        uint32_t numControlLineStates { 0 };
        uint32_t numDtrDrops { 0 };
        uint32_t numBreaks { 0 };
    };
    volatile LineState lineState;
    volatile bool dtrRaised { false };

    int8_t (*libDaisyControl) (uint8_t, uint8_t*, uint16_t) { nullptr };

    int8_t control (uint8_t cmd, uint8_t* pbuf, uint16_t length)
    {
        switch (cmd)
        {
            case CDC_SET_LINE_CODING:
                if (length >= 7)
                {
                    lineState.rate = static_cast<uint32_t> (pbuf[0]) | (static_cast<uint32_t> (pbuf[1]) << 8)
                                   | (static_cast<uint32_t> (pbuf[2]) << 16) | (static_cast<uint32_t> (pbuf[3]) << 24);
                    lineState.stopBits = pbuf[4];
                    lineState.parity = pbuf[5];
                    lineState.dataBits = pbuf[6];
                    lineState.numLineCodings = lineState.numLineCodings + 1;
                }
                break;

            case CDC_SET_CONTROL_LINE_STATE:
            {
                // a request with no data stage is passed the setup packet, wValue holds the lines
                const auto* request { reinterpret_cast<const USBD_SetupReqTypedef*> (pbuf) };
                const auto dtr { (request->wValue & 0x01) != 0 };
                if (lineState.dtr && ! dtr)
                    lineState.numDtrDrops = lineState.numDtrDrops + 1;
                if (! lineState.dtr && dtr)
                    dtrRaised = true;
                lineState.dtr = dtr;
                lineState.rts = (request->wValue & 0x02) != 0;
                lineState.numControlLineStates = lineState.numControlLineStates + 1;
                break;
            }

            case CDC_SEND_BREAK:
                lineState.numBreaks = lineState.numBreaks + 1;
                break;

            default:
                break;
        }
        // libDaisy keeps the line coding, for GET_LINE_CODING
        return libDaisyControl (cmd, pbuf, length);
    }

    void received (uint8_t* buffer, uint32_t* length)
    {
        auto write { fifoWrite };
        for (uint32_t i { 0 }; i < *length; ++i)
        {
            const auto next { (write + 1) % kFifoSize };
            if (next == fifoRead)
            {
                numOverflows = numOverflows + 1;
                break;
            }
            fifo[write] = buffer[i];
            write = next;
        }
        fifoWrite = write;
    }

    // two buffers, so one can be filled while the other is being sent. a transmit is only accepted once the
    // previous one has finished, so the buffer being filled is never in use
    uint8_t txBuffers[2][512];
    int txIndex { 0 };
    uint32_t txLength { 0 };

    void queueTransmit (const uint8_t* data, uint32_t length)
    {
        while (length > 0)
        {
            const auto numToCopy { std::min (length, static_cast<uint32_t> (sizeof (txBuffers[0])) - txLength) };
            std::memcpy (txBuffers[txIndex] + txLength, data, numToCopy);
            txLength += numToCopy;
            data += numToCopy;
            length -= numToCopy;
            if (txLength == sizeof (txBuffers[0]))
            {
                // full, wait for the previous buffer to go. if the host is not reading, it never will, so give up
                const auto start { System::GetNow () };
                while (hw.usb_handle.TransmitInternal (txBuffers[txIndex], txLength) != UsbHandle::Result::OK)
                    if (System::GetNow () - start > 100)
                        break;
                txIndex ^= 1;
                txLength = 0;
            }
        }
    }

    // sends what is buffered, if the previous transmit has finished
    void pumpTransmit ()
    {
        if (txLength > 0 && hw.usb_handle.TransmitInternal (txBuffers[txIndex], txLength) == UsbHandle::Result::OK)
        {
            txIndex ^= 1;
            txLength = 0;
        }
    }

    // finds the command prefix in the echoed data. the prefix has no repeated bytes, so a mismatch only has to check
    // whether the byte starts a new prefix
    constexpr uint8_t kPrefix[] { 0xF5, 0x5A, 0xA5, 0x0F };
    size_t prefixMatched { 0 };
    bool awaitingCommand { false };
    uint32_t xonDueMs { 0 };

    void sendState ()
    {
        char reply[256];
        const auto length { std::snprintf (reply, sizeof (reply),
            "\x02STATE rate=%lu data=%u parity=%u stop=%u dtr=%u rts=%u codings=%lu controls=%lu dtrdrops=%lu breaks=%lu overflows=%lu\n",
            static_cast<unsigned long> (lineState.rate), lineState.dataBits, lineState.parity, lineState.stopBits,
            lineState.dtr ? 1u : 0u, lineState.rts ? 1u : 0u,
            static_cast<unsigned long> (lineState.numLineCodings), static_cast<unsigned long> (lineState.numControlLineStates),
            static_cast<unsigned long> (lineState.numDtrDrops), static_cast<unsigned long> (lineState.numBreaks),
            static_cast<unsigned long> (numOverflows)) };
        queueTransmit (reinterpret_cast<const uint8_t*> (reply), static_cast<uint32_t> (length));
    }

    void runCommand (uint8_t command)
    {
        if (command == 'S')
        {
            sendState ();
        }
        else if (command == 'X')
        {
            const uint8_t xoff { 0x13 };
            queueTransmit (&xoff, 1);
            xonDueMs = System::GetNow () + 1000;
        }
    }

    // the commands follow the bytes echoed before them
    void scanForCommands (const uint8_t* data, uint32_t length, uint32_t& numEchoed)
    {
        for (uint32_t i { 0 }; i < length; ++i)
        {
            const auto byte { data[i] };
            if (awaitingCommand)
            {
                awaitingCommand = false;
                queueTransmit (data + numEchoed, i + 1 - numEchoed);
                numEchoed = i + 1;
                runCommand (byte);
                continue;
            }
            if (byte == kPrefix[prefixMatched])
            {
                if (++prefixMatched == sizeof (kPrefix))
                {
                    prefixMatched = 0;
                    awaitingCommand = true;
                }
            }
            else
            {
                prefixMatched = byte == kPrefix[0] ? 1 : 0;
            }
        }
    }
}

int main (void)
{
    hw.Init ();

    // intercept the CDC requests before USB starts, so the ones made while the host enumerates are seen too
    libDaisyControl = USBD_Interface_fops_FS.Control;
    USBD_Interface_fops_FS.Control = control;
    hw.usb_handle.Init (UsbHandle::FS_INTERNAL);
    System::Delay (100);
    hw.usb_handle.SetReceiveCallback (received, UsbHandle::FS_INTERNAL);

    uint32_t lastActivityMs { 0 };
    for (;;)
    {
        // a new connection, forget whatever the last one sent which was never read
        if (dtrRaised)
        {
            dtrRaised = false;
            fifoRead = fifoWrite;
            prefixMatched = 0;
            awaitingCommand = false;
        }

        const auto write { fifoWrite };
        const auto read { fifoRead };
        if (write != read)
        {
            // the contiguous part
            const auto end { write > read ? write : kFifoSize };
            const auto length { end - read };
            uint32_t numEchoed { 0 };
            scanForCommands (fifo + read, length, numEchoed);
            queueTransmit (fifo + read + numEchoed, length - numEchoed);
            fifoRead = end % kFifoSize;
            lastActivityMs = System::GetNow ();
        }

        if (xonDueMs != 0 && System::GetNow () >= xonDueMs)
        {
            xonDueMs = 0;
            const uint8_t xon { 0x11 };
            queueTransmit (&xon, 1);
        }

        pumpTransmit ();

        // the LED shows data passing, or the DTR line when idle
        hw.SetLed (System::GetNow () - lastActivityMs < 50 ? true : lineState.dtr);
    }
}
