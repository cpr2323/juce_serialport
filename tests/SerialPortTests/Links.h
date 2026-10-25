#pragma once

#include <juce_serialport/juce_serialport.h>

#if JUCE_MAC || JUCE_LINUX
 #include <fcntl.h>
 #include <poll.h>
 #include <termios.h>
 #include <unistd.h>
 #if JUCE_MAC
  #include <util.h>
 #else
  #include <pty.h>
 #endif
#endif

// collects bytes from another thread, and lets the test wait for them
class Collector
{
public:
    void append (const void* data, size_t numBytes)
    {
        {
            const juce::ScopedLock sl { lock };
            bytes.append (data, numBytes);
        }
        dataArrived.signal ();
    }

    // waits until at least numBytes have been collected
    bool waitForSize (size_t numBytes, int timeoutMs)
    {
        const auto endTime { juce::Time::getMillisecondCounter () + static_cast<juce::uint32> (timeoutMs) };
        while (getSize () < numBytes)
        {
            const auto now { juce::Time::getMillisecondCounter () };
            if (now >= endTime)
                return false;
            dataArrived.wait (static_cast<int> (juce::jmin (endTime - now, 50u)));
        }
        return true;
    }

    size_t getSize () const
    {
        const juce::ScopedLock sl { lock };
        return bytes.getSize ();
    }

    juce::MemoryBlock take ()
    {
        const juce::ScopedLock sl { lock };
        juce::MemoryBlock result;
        result.swapWith (bytes);
        return result;
    }

private:
    juce::CriticalSection lock;
    juce::MemoryBlock bytes;
    juce::WaitableEvent dataArrived;
};

// how a test reaches the far end of the port under test
class Link
{
public:
    virtual ~Link () = default;

    virtual juce::String getName () const = 0;
    virtual juce::String getPortUnderTest () const = 0;

    // true if data can be sent from the far end, and what the port under test sends arrives at the far end
    virtual bool hasFarEnd () const { return false; }
    virtual bool sendFromFarEnd (const void* /*data*/, size_t /*numBytes*/) { return false; }
    // everything the far end has received
    virtual Collector* getFarEndReceived () { return nullptr; }
    virtual void setFarEndBps (uint32_t /*bps*/) {}

    // true if the device sends back everything it receives
    virtual bool echoes () const { return false; }

    // false if the driver ignores parity and stop bits (pseudo terminals)
    virtual bool supportsLineSettings () const { return true; }

    // true if the test can make the device go away
    virtual bool canHangUp () const { return false; }
    virtual void hangUp () {}
};

#if JUCE_MAC || JUCE_LINUX
// a pseudo terminal. the port under test opens the slave, and the master is the far end
class PtyLink : public Link, private juce::Thread
{
public:
    PtyLink () : Thread ("PtyFarEnd")
    {
        int slave { -1 };
        char slaveName [256] {};
        if (openpty (&master, &slave, slaveName, nullptr, nullptr) != 0)
            return;
        // the master is raw, so data passes through unchanged
        termios options {};
        tcgetattr (master, &options);
        cfmakeraw (&options);
        tcsetattr (master, TCSANOW, &options);
        fcntl (master, F_SETFL, fcntl (master, F_GETFL) | O_NONBLOCK);
        ::close (slave);
        slavePath = slaveName;
        startThread ();
    }

    ~PtyLink () override
    {
        hangUp ();
    }

    bool isValid () const { return slavePath.isNotEmpty (); }
    juce::String getName () const override { return "pty " + slavePath; }
    juce::String getPortUnderTest () const override { return slavePath; }
    bool hasFarEnd () const override { return true; }
    Collector* getFarEndReceived () override { return &received; }
    bool canHangUp () const override { return true; }
    // the Linux pty driver forces 8 data bits and no parity
    bool supportsLineSettings () const override { return false; }

    bool sendFromFarEnd (const void* data, size_t numBytes) override
    {
        size_t sent { 0 };
        while (sent < numBytes && master != -1)
        {
            pollfd pfd { master, POLLOUT, 0 };
            if (poll (&pfd, 1, 1000) <= 0)
                return false;
            const auto result { ::write (master, static_cast<const char*> (data) + sent, numBytes - sent) };
            if (result > 0)
                sent += static_cast<size_t> (result);
            else if (errno != EAGAIN && errno != EINTR)
                return false;
        }
        return sent == numBytes;
    }

    void hangUp () override
    {
        stopThread (2000);
        if (master != -1)
        {
            ::close (master);
            master = -1;
        }
    }

private:
    int master { -1 };
    juce::String slavePath;
    Collector received;

    void run () override
    {
        char buffer [4096];
        while (! threadShouldExit ())
        {
            pollfd pfd { master, POLLIN, 0 };
            if (poll (&pfd, 1, 50) > 0)
            {
                const auto numRead { ::read (master, buffer, sizeof (buffer)) };
                if (numRead > 0)
                    received.append (buffer, static_cast<size_t> (numRead));
            }
        }
    }
};
#endif

// two ports connected to each other, eg. a com0com pair, or two USB serial adapters with a null modem cable.
// the far end uses this library too
class PairLink : public Link
{
public:
    PairLink (const juce::String& portUnderTestPath, const juce::String& farEndPath)
        : portUnderTest { portUnderTestPath }, farEndPort { nullptr }
    {
        if (farEndPort.open (farEndPath))
        {
            farEndInput = std::make_unique<SerialPortInputStream> (&farEndPort, [this] (const void* data, int numBytes)
            {
                received.append (data, static_cast<size_t> (numBytes));
            });
            farEndOutput = std::make_unique<SerialPortOutputStream> (&farEndPort);
        }
    }

    ~PairLink () override
    {
        farEndOutput = nullptr;
        farEndInput = nullptr;
        farEndPort.close ();
    }

    bool isValid () const { return farEndPort.exists (); }
    juce::String getName () const override { return "pair " + portUnderTest + " <-> " + farEndPort.getPortPath (); }
    juce::String getPortUnderTest () const override { return portUnderTest; }
    bool hasFarEnd () const override { return true; }
    Collector* getFarEndReceived () override { return &received; }

    bool sendFromFarEnd (const void* data, size_t numBytes) override
    {
        return farEndOutput != nullptr && farEndOutput->write (data, numBytes);
    }

    void setFarEndBps (uint32_t bps) override
    {
        SerialPortConfig config;
        config.bps = bps;
        farEndPort.setConfig (config);
    }

private:
    juce::String portUnderTest;
    SerialPort farEndPort;
    std::unique_ptr<SerialPortInputStream> farEndInput;
    std::unique_ptr<SerialPortOutputStream> farEndOutput;
    Collector received;
};

// a device which echoes everything back, eg. tests/arduino_echo
class EchoLink : public Link
{
public:
    explicit EchoLink (const juce::String& path) : portUnderTest { path } {}
    juce::String getName () const override { return "echo device " + portUnderTest; }
    juce::String getPortUnderTest () const override { return portUnderTest; }
    bool echoes () const override { return true; }

private:
    juce::String portUnderTest;
};

// a port with nothing connected (eg. a motherboard COM port), only sending can be tested
class WriteOnlyLink : public Link
{
public:
    explicit WriteOnlyLink (const juce::String& path) : portUnderTest { path } {}
    juce::String getName () const override { return "write only " + portUnderTest; }
    juce::String getPortUnderTest () const override { return portUnderTest; }

private:
    juce::String portUnderTest;
};
