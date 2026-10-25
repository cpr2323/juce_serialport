// functional tests for juce_serialport. see tests/README.md for the modes, and what each one needs
#include "Links.h"

#include <iostream>
#include <thread>

namespace
{
    int numPassed { 0 };
    int numFailed { 0 };
    int numSkipped { 0 };

    void check (bool condition, const juce::String& description)
    {
        if (condition)
            ++numPassed;
        else
            ++numFailed;
        std::cout << (condition ? "    ok    " : "    FAIL  ") << description << std::endl;
    }

    void info (const juce::String& text)
    {
        std::cout << "    info  " << text << std::endl;
    }

    void section (const juce::String& name)
    {
        std::cout << std::endl << "  " << name << std::endl;
    }

    void skip (const juce::String& name, const juce::String& reason)
    {
        ++numSkipped;
        std::cout << std::endl << "  " << name << " - skipped, " << reason << std::endl;
    }

    double ticksToMs (juce::int64 ticks)
    {
        return juce::Time::highResolutionTicksToSeconds (ticks) * 1000.0;
    }

    juce::MemoryBlock makeTestData (size_t numBytes, int seed)
    {
        juce::MemoryBlock data { numBytes };
        juce::Random random { seed };
        for (size_t i { 0 }; i < numBytes; ++i)
            data[i] = static_cast<char> (random.nextInt (256));
        return data;
    }

    // how long numBytes takes on the wire at bps, 10 bits per byte, plus some slack
    int transferTimeMs (size_t numBytes, uint32_t bps)
    {
        return 2000 + static_cast<int> (static_cast<double> (numBytes) * 10.0 * 1000.0 / juce::jmax (1u, bps) * 1.5);
    }

    struct Options
    {
        uint32_t bps { 115200 };
        bool unplugTest { false };
    };

    // the port under test, with its streams
    struct Fixture
    {
        explicit Fixture (Link& link, const Options& options, SerialPortInputStream::DataReceivedCallback callback = nullptr, bool alsoBuffer = false)
            : port { [] (juce::String, juce::String) {} }
        {
            // data from the previous test may still be arriving at the far end, wait for it to stop, and discard it
            if (auto received { link.getFarEndReceived () })
            {
                const auto endTime { juce::Time::getMillisecondCounter () + 5000 };
                auto lastSize { received->getSize () };
                while (juce::Time::getMillisecondCounter () < endTime)
                {
                    juce::Thread::sleep (300);
                    if (received->getSize () == lastSize)
                        break;
                    lastSize = received->getSize ();
                }
                received->take ();
            }

            if (! port.open (link.getPortUnderTest ()))
                return;
            SerialPortConfig config;
            config.bps = options.bps;
            port.setConfig (config);
            link.setFarEndBps (options.bps);
            const auto usesCallback { callback != nullptr };
            if (usesCallback)
                input = std::make_unique<SerialPortInputStream> (&port, std::move (callback), alsoBuffer);
            else
                input = std::make_unique<SerialPortInputStream> (&port);
            output = std::make_unique<SerialPortOutputStream> (&port);
            // let the device start up, and anything left over from a previous test arrive, then discard it
            juce::Thread::sleep (juce::jmax (100, link.getSettleTimeMs ()));
            if (! usesCallback)
            {
                char discard [256];
                while (input->read (discard, sizeof (discard)) > 0) {}
            }
            if (auto received { link.getFarEndReceived () })
                received->take ();
        }

        ~Fixture ()
        {
            output = nullptr;
            input = nullptr;
            port.close ();
        }

        bool isOpen () const { return port.exists (); }

        SerialPort port;
        std::unique_ptr<SerialPortInputStream> input;
        std::unique_ptr<SerialPortOutputStream> output;
    };

    //==============================================================================
    void testByteQueue ()
    {
        section ("SerialPortByteQueue");

        SerialPortByteQueue queue;
        juce::MemoryBlock expected;
        juce::Random random { 1 };
        auto ok { true };
        juce::HeapBlock<char> temp { 10000 };
        for (auto iteration { 0 }; iteration < 2000 && ok; ++iteration)
        {
            const auto numToPush { static_cast<size_t> (random.nextInt (700)) };
            const auto data { makeTestData (numToPush, iteration) };
            queue.push (data.getData (), data.getSize ());
            expected.append (data.getData (), data.getSize ());

            const auto numToRead { static_cast<size_t> (random.nextInt (700)) };
            const auto numRead { queue.read (temp.get (), numToRead) };
            ok = numRead == juce::jmin (numToRead, expected.getSize ())
              && std::memcmp (temp.get (), expected.getData (), numRead) == 0;
            expected.removeSection (0, numRead);
            ok = ok && queue.size () == expected.getSize ();
        }
        check (ok, "random pushes and reads, with wrap around and growth, keep the data in order");

        SerialPortByteQueue lines;
        lines.reserve (256);
        juce::HeapBlock<char> filler { 250 };
        std::memset (filler.get (), 'x', 250);
        lines.push (filler.get (), 250);
        lines.pop (250);
        lines.push ("abcdefgh\nij", 11); // wraps the end of the storage
        check (lines.capacity () == 256 && lines.contains ('\n') && lines.contains ('j') && ! lines.contains ('z'), "contains () searches across the wrap");
    }

    //==============================================================================
    void testOpenFailures ()
    {
        section ("Open failures");

        SerialPort port { nullptr };
        check (! port.open ("this port does not exist"), "opening a missing port fails");
        check (! port.exists (), "exists () is false");
        SerialPortInputStream input { &port };
        SerialPortOutputStream output { &port };
        auto callbackCalled { false };
        check (! output.write ("x", 1, [&callbackCalled] (bool) { callbackCalled = true; }), "writing fails");
        check (input.read (nullptr, 0) == -1, "reading fails");
        check (output.waitUntilSent (100), "waitUntilSent has nothing to wait for");
        juce::Thread::sleep (50);
        check (! callbackCalled, "the completion of a rejected write is never called");
    }

    //==============================================================================
    void testConfig (Link& link, const Options& options)
    {
        section ("Configuration");

        Fixture fixture { link, options };
        check (fixture.isOpen (), "the port opens");
        if (! fixture.isOpen ())
            return;

        for (const uint32_t bps : { 9600u, 28800u, 115200u, options.bps })
        {
            SerialPortConfig config;
            config.bps = bps;
            SerialPortConfig readBack;
            const auto ok { fixture.port.setConfig (config) && fixture.port.getConfig (readBack) && readBack.bps == bps };
            check (ok, "set and read back " + juce::String (bps) + " bps (read " + juce::String (readBack.bps) + ")");
        }

        SerialPortConfig config;
        config.bps = options.bps;
        if (link.supportsLineSettings ())
        {
            config.parity = SerialPortConfig::SERIALPORT_PARITY_EVEN;
            config.stopbits = SerialPortConfig::STOPBITS_2;
            SerialPortConfig readBack;
            check (fixture.port.setConfig (config) && fixture.port.getConfig (readBack)
                   && readBack.parity == SerialPortConfig::SERIALPORT_PARITY_EVEN && readBack.stopbits == SerialPortConfig::STOPBITS_2,
                   "even parity and 2 stop bits");
        }
        else
        {
            info ("parity and stop bits not tested, " + link.getName () + " ignores them");
        }
        config = {};
        config.bps = options.bps;
        check (fixture.port.setConfig (config), "back to 8N1");
    }

    //==============================================================================
    void testWriteCompletion (Link& link, const Options& options)
    {
        section ("Write completion callbacks");

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        constexpr auto kNumWrites { 50 };
        constexpr size_t kWriteSize { 64 };
        juce::CriticalSection lock;
        juce::Array<int> completionOrder;
        std::atomic<int> numSucceeded { 0 };
        std::atomic<int> numOnCallingThread { 0 };
        const auto callingThread { juce::Thread::getCurrentThreadId () };

        const auto data { makeTestData (kNumWrites * kWriteSize, 2) };
        auto allQueued { true };
        for (auto writeIndex { 0 }; writeIndex < kNumWrites; ++writeIndex)
        {
            allQueued = fixture.output->write (static_cast<const char*> (data.getData ()) + static_cast<size_t> (writeIndex) * kWriteSize, kWriteSize,
                                              [&, writeIndex] (bool sent)
            {
                if (sent)
                    ++numSucceeded;
                if (juce::Thread::getCurrentThreadId () == callingThread)
                    ++numOnCallingThread;
                const juce::ScopedLock sl { lock };
                completionOrder.add (writeIndex);
            }) && allQueued;
        }
        check (allQueued, "all writes were queued");
        check (fixture.output->waitUntilSent (transferTimeMs (data.getSize (), options.bps)), "waitUntilSent returns once everything has been sent");
        check (numSucceeded == kNumWrites, "every completion reported success (" + juce::String (numSucceeded.load ()) + " of " + juce::String (kNumWrites) + ")");
        check (numOnCallingThread == 0, "completions are called on the writer thread");
        auto inOrder { completionOrder.size () == kNumWrites };
        for (auto index { 0 }; index < completionOrder.size (); ++index)
            inOrder = inOrder && completionOrder[index] == index;
        check (inOrder, "completions are called in the order the writes were made");
        check (fixture.output->getNumBytesSent () == data.getSize () && fixture.output->getNumBytesPending () == 0, "getNumBytesSent and getNumBytesPending agree");

        std::atomic<bool> emptyWriteCompleted { false };
        check (fixture.output->write (nullptr, 0, [&] (bool sent) { emptyWriteCompleted = sent; }), "an empty write is queued");
        fixture.output->waitUntilSent (1000);
        check (emptyWriteCompleted.load (), "an empty write completes too");

        if (auto received { link.getFarEndReceived () })
        {
            received->waitForSize (data.getSize (), 5000);
            check (received->take () == data, "the far end received the data intact");
        }
        else if (link.echoes ())
        {
            juce::Thread::sleep (500);
        }
    }

    //==============================================================================
    void testDeleteWithPendingWrites (Link& link, const Options& options)
    {
        section ("Deleting the stream with writes pending");

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        // more than can possibly be sent before the stream is deleted
        const auto numBytes { static_cast<size_t> (juce::jmax (65536u, options.bps / 2)) };
        const auto data { makeTestData (numBytes, 3) };
        std::atomic<int> numCalls { 0 };
        std::atomic<bool> lastResult { true };
        fixture.output->write (data.getData (), data.getSize (), [&] (bool sent) { ++numCalls; lastResult = sent; });

        const auto startTicks { juce::Time::getHighResolutionTicks () };
        fixture.output = nullptr;
        const auto deleteMs { ticksToMs (juce::Time::getHighResolutionTicks () - startTicks) };
        check (numCalls == 1, "the completion is called exactly once");
        check (! lastResult, "and reports that the data was not sent");
        check (deleteMs < 2000.0, "deleting the stream took " + juce::String (deleteMs, 1) + " ms");
        // the far end may have received some of it, which the next fixture discards
    }

    //==============================================================================
    void testWriteNeverBlocks (Link& link, const Options& options)
    {
        section ("write () never blocks the calling thread");

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }
        // room for the 1 MB write and the sequencer's writes, so the queue never has to grow during the test
        fixture.output->setQueueSize (2 << 20);

        // a 1 MB write is just a copy, however long it takes to send
        const auto big { makeTestData (1 << 20, 4) };
        auto startTicks { juce::Time::getHighResolutionTicks () };
        fixture.output->write (big.getData (), big.getSize ());
        const auto bigWriteMs { ticksToMs (juce::Time::getHighResolutionTicks () - startTicks) };
        check (bigWriteMs < 50.0, "queueing 1 MB took " + juce::String (bigWriteMs, 2) + " ms");

        // a sequencer style thread, writing a small message every millisecond, while the queue is busy
        struct Sequencer : public juce::Thread
        {
            explicit Sequencer (SerialPortOutputStream& o) : Thread ("Sequencer"), output { o } {}
            void run () override
            {
                const uint8_t message [3] { 0x90, 60, 100 };
                while (! threadShouldExit () && numWrites < 1000)
                {
                    const auto before { juce::Time::getHighResolutionTicks () };
                    output.write (message, sizeof (message));
                    worstTicks = juce::jmax (worstTicks, juce::Time::getHighResolutionTicks () - before);
                    ++numWrites;
                    sleep (1);
                }
            }
            SerialPortOutputStream& output;
            juce::int64 worstTicks { 0 };
            int numWrites { 0 };
        };
        Sequencer sequencer { *fixture.output };
        sequencer.startThread (juce::Thread::Priority::highest);
        sequencer.waitForThreadToExit (10000);
        check (sequencer.numWrites == 1000, "the sequencer thread made its 1000 writes");
        // a write is a short copy under a lock, anything near a millisecond would be the OS preempting the thread
        check (ticksToMs (sequencer.worstTicks) < 2.0, "the slowest small write took " + juce::String (ticksToMs (sequencer.worstTicks), 3) + " ms");

        // don't wait for 1 MB to be sent at low rates, deleting the stream discards it
        startTicks = juce::Time::getHighResolutionTicks ();
        fixture.output = nullptr;
        info ("deleting the stream with " + juce::String (big.getSize () / 1024) + " KB queued took "
              + juce::String (ticksToMs (juce::Time::getHighResolutionTicks () - startTicks), 1) + " ms");
    }

    //==============================================================================
    void testReceiveCallback (Link& link, const Options& options)
    {
        section ("Receive callback");

        Collector received;
        std::atomic<int> numOnMainThread { 0 };
        std::atomic<juce::int64> firstArrivalTicks { 0 };
        const auto mainThread { juce::Thread::getCurrentThreadId () };
        Fixture fixture { link, options, [&] (const void* data, int numBytes)
        {
            if (juce::Thread::getCurrentThreadId () == mainThread)
                ++numOnMainThread;
            auto expected { juce::int64 { 0 } };
            firstArrivalTicks.compare_exchange_strong (expected, juce::Time::getHighResolutionTicks ());
            received.append (data, static_cast<size_t> (numBytes));
        } };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        // a USB serial bridge (eg. an Uno's 16U2) can replay data from a previous test once the port is reopened,
        // which arrives while the fixture waits for the device to start. discard it, and measure from here
        const auto discarded { received.take ().getSize () };
        if (discarded > 0)
            info ("discarded " + juce::String (discarded) + " bytes left over from a previous test");
        firstArrivalTicks = 0;
        const auto bytesReceivedBefore { fixture.input->getNumBytesReceived () };

        // latency of a single short message
        const char ping [] { "ping" };
        const auto sendTicks { juce::Time::getHighResolutionTicks () };
        if (link.hasFarEnd ())
            link.sendFromFarEnd (ping, 4);
        else
            fixture.output->write (ping, 4);
        check (received.waitForSize (4, 2000), "a short message arrived");
        const auto latencyMs { ticksToMs (firstArrivalTicks.load () - sendTicks) };
        info ((link.echoes () ? "round trip latency: " : "latency: ") + juce::String (latencyMs, 2) + " ms");
        check (received.take () == juce::MemoryBlock (ping, 4), "it arrived intact");
        check (numOnMainThread == 0, "the callback is called on the reader thread");
        check (fixture.input->isExhausted (), "data only passed to the callback is not buffered");

        // bulk
        const auto numBytes { static_cast<size_t> (link.echoes () ? 16384 : 262144) };
        const auto data { makeTestData (numBytes, 5) };
        const auto startTicks { juce::Time::getHighResolutionTicks () };
        if (link.hasFarEnd ())
            link.sendFromFarEnd (data.getData (), data.getSize ());
        else
            fixture.output->write (data.getData (), data.getSize ());
        const auto arrived { received.waitForSize (numBytes, transferTimeMs (numBytes, options.bps) * (link.echoes () ? 2 : 1)) };
        const auto seconds { juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks () - startTicks) };
        check (arrived, juce::String (numBytes / 1024) + " KB arrived");
        check (received.take () == data, "it arrived intact");
        info ("throughput: " + juce::String (static_cast<double> (numBytes) / seconds / 1024.0, 1) + " KB/s");
        check (fixture.input->getNumBytesReceived () - bytesReceivedBefore == numBytes + 4, "getNumBytesReceived counts everything");
    }

    //==============================================================================
    void testPolling (Link& link, const Options& options)
    {
        section ("Polling reads");

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        const juce::String lines { "first line\nsecond line\r\nthird" };
        if (link.hasFarEnd ())
            link.sendFromFarEnd (lines.toRawUTF8 (), lines.getNumBytesAsUTF8 ());
        else
            fixture.output->write (lines.toRawUTF8 (), lines.getNumBytesAsUTF8 ());

        const auto endTime { juce::Time::getMillisecondCounter () + 3000 };
        while (fixture.input->getTotalLength () < static_cast<juce::int64> (lines.getNumBytesAsUTF8 ()) && juce::Time::getMillisecondCounter () < endTime)
            juce::Thread::sleep (5);
        check (fixture.input->canReadLine (), "canReadLine");
        check (fixture.input->readNextLine () == "first line", "first line");
        check (fixture.input->readNextLine () == "second line", "second line, \\r\\n trimmed");
        check (! fixture.input->canReadLine (), "an incomplete line is not reported as a line");
        char rest [5] {};
        check (fixture.input->read (rest, 5) == 5 && juce::String (rest, 5) == "third", "the rest can still be read");
        check (fixture.input->isExhausted () && fixture.input->read (rest, 5) == 0, "then the stream is exhausted");
    }

    //==============================================================================
    void testBothDirections (Link& link, const Options& options)
    {
        section ("Both directions at once");

        Collector received;
        Fixture fixture { link, options, [&] (const void* data, int numBytes) { received.append (data, static_cast<size_t> (numBytes)); } };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        const auto numBytes { static_cast<size_t> (262144) };
        const auto outgoing { makeTestData (numBytes, 6) };
        const auto incoming { makeTestData (numBytes, 7) };
        std::atomic<bool> outgoingSent { false };
        const auto startTicks { juce::Time::getHighResolutionTicks () };
        fixture.output->write (outgoing.getData (), outgoing.getSize (), [&] (bool sent) { outgoingSent = sent; });
        auto farEndSender { std::thread ([&] { link.sendFromFarEnd (incoming.getData (), incoming.getSize ()); }) };

        const auto timeoutMs { transferTimeMs (numBytes, options.bps) };
        check (received.waitForSize (numBytes, timeoutMs), "the port under test received everything");
        check (link.getFarEndReceived ()->waitForSize (numBytes, timeoutMs), "the far end received everything");
        farEndSender.join ();
        fixture.output->waitUntilSent (timeoutMs);
        const auto seconds { juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks () - startTicks) };
        check (outgoingSent.load (), "the write completed");
        check (received.take () == incoming, "incoming data intact");
        check (link.getFarEndReceived ()->take () == outgoing, "outgoing data intact");
        info ("throughput: " + juce::String (2.0 * static_cast<double> (numBytes) / seconds / 1024.0, 1) + " KB/s, both directions combined");
    }

    //==============================================================================
    void testCallbackSwap (Link& link, const Options& options)
    {
        section ("Changing the receive callback while data is arriving");

        Fixture fixture { link, options, [] (const void*, int) {} };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        std::atomic<bool> stopSending { false };
        auto sender { std::thread ([&]
        {
            const auto data { makeTestData (256, 8) };
            while (! stopSending)
            {
                link.sendFromFarEnd (data.getData (), data.getSize ());
                juce::Thread::sleep (1);
            }
        }) };

        std::atomic<int> callsAfterReplacement { 0 };
        auto ok { true };
        for (auto iteration { 0 }; iteration < 50; ++iteration)
        {
            std::atomic<bool> replaced { false };
            fixture.input->setDataReceivedCallback ([&replaced, &callsAfterReplacement] (const void*, int)
            {
                if (replaced)
                    ++callsAfterReplacement;
                juce::Thread::sleep (1);
            });
            juce::Thread::sleep (5);
            fixture.input->setDataReceivedCallback ([] (const void*, int) {});
            replaced = true;
            juce::Thread::sleep (2);
            ok = ok && callsAfterReplacement == 0;
        }
        stopSending = true;
        sender.join ();
        check (ok, "a replaced callback is never called after setDataReceivedCallback returns");
    }

    //==============================================================================
    void testConnectionLost (Link& link, const Options& options, bool interactive)
    {
        section (interactive ? "Unplugging the device" : "Connection lost");

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        std::atomic<int> numLostCalls { 0 };
        juce::WaitableEvent lost;
        fixture.port.setConnectionLostCallback ([&] { ++numLostCalls; lost.signal (); });

        // keep writing, so a pending write is in flight when the device goes away
        std::atomic<int> numFailedCompletions { 0 };
        const auto data { makeTestData (1024, 9) };
        for (auto i { 0 }; i < 64; ++i)
            fixture.output->write (data.getData (), data.getSize (), [&] (bool sent) { if (! sent) ++numFailedCompletions; });

        const auto startTicks { juce::Time::getHighResolutionTicks () };
        if (interactive)
        {
            std::cout << "    >>>>  unplug the device now (waiting 30 seconds)" << std::endl;
            lost.wait (30000);
        }
        else
        {
            link.hangUp ();
            lost.wait (5000);
        }
        const auto detectMs { ticksToMs (juce::Time::getHighResolutionTicks () - startTicks) };
        check (numLostCalls == 1, "the connection lost callback was called once");
        if (! interactive)
            info ("noticed after " + juce::String (detectMs, 1) + " ms");
        check (! fixture.port.exists (), "exists () is false");
        check (! fixture.output->write ("x", 1), "writes are rejected");
        juce::Thread::sleep (200);
        check (numFailedCompletions > 0 || fixture.output->getNumBytesPending () == 0, "pending writes completed with failure");
        check (fixture.input->read (nullptr, 0) == -1 || ! fixture.input->isExhausted (), "reads report the port has gone, once the buffer is empty");
    }

    //==============================================================================
    void listPorts ()
    {
        const auto ports { SerialPort::getSerialPortPaths () };
        std::cout << "Serial ports (" << ports.size () << "):" << std::endl;
        for (auto index { 0 }; index < ports.size (); ++index)
            std::cout << "  " << ports.getAllKeys ()[index] << "  ->  " << ports.getAllValues ()[index] << std::endl;
    }

    void runTests (Link& link, const Options& options)
    {
        std::cout << std::endl << "Testing " << link.getName () << " at " << options.bps << " bps" << std::endl;

        testConfig (link, options);
        testWriteCompletion (link, options);
        testDeleteWithPendingWrites (link, options);
        testWriteNeverBlocks (link, options);

        if (link.hasFarEnd () || link.echoes ())
        {
            testReceiveCallback (link, options);
            testPolling (link, options);
        }
        else
        {
            skip ("Receive callback", "needs a far end, or an echo device");
            skip ("Polling reads", "needs a far end, or an echo device");
        }

        if (link.hasFarEnd ())
        {
            testBothDirections (link, options);
            testCallbackSwap (link, options);
        }
        else
        {
            skip ("Both directions at once", "needs a far end");
            skip ("Changing the receive callback while data is arriving", "needs a far end");
        }

        if (link.canHangUp ())
            testConnectionLost (link, options, false);
        else if (options.unplugTest)
            testConnectionLost (link, options, true);
        else
            skip ("Connection lost", "run with --unplug to test it by unplugging the device");
    }

    void printUsage ()
    {
        std::cout << "SerialPortTests [options]\n"
                     "  (no mode)            macOS/Linux: test over a pseudo terminal pair. Windows: list the ports\n"
                     "  --pair PORT1 PORT2   test over two ports connected to each other (com0com, null modem cable)\n"
                     "  --echo PORT          test with a device which echoes everything back (tests/arduino_echo)\n"
                     "  --write-only PORT    only the send tests, for a port with nothing connected\n"
                     "  --list               list the serial ports\n"
                     "  --baud N             the rate to test at, default 115200\n"
                     "  --unplug             with --pair or --echo, also test unplugging the device\n";
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::StringArray args;
    for (auto index { 1 }; index < argc; ++index)
        args.add (juce::String::fromUTF8 (argv[index]));

    if (args.contains ("--help") || args.contains ("-h"))
    {
        printUsage ();
        return 0;
    }

    Options options;
    if (const auto index { args.indexOf ("--baud") }; index >= 0)
        options.bps = static_cast<uint32_t> (args[index + 1].getIntValue ());
    options.unplugTest = args.contains ("--unplug");

    listPorts ();
    if (args.contains ("--list"))
        return 0;

    testByteQueue ();
    testOpenFailures ();

    std::unique_ptr<Link> link;
    if (const auto index { args.indexOf ("--pair") }; index >= 0)
    {
        auto pairLink { std::make_unique<PairLink> (args[index + 1], args[index + 2]) };
        if (! pairLink->isValid ())
        {
            std::cout << "could not open " << args[index + 2] << std::endl;
            return 1;
        }
        link = std::move (pairLink);
    }
    else if (const auto echoIndex { args.indexOf ("--echo") }; echoIndex >= 0)
    {
        link = std::make_unique<EchoLink> (args[echoIndex + 1]);
    }
    else if (const auto writeOnlyIndex { args.indexOf ("--write-only") }; writeOnlyIndex >= 0)
    {
        link = std::make_unique<WriteOnlyLink> (args[writeOnlyIndex + 1]);
    }
    else
    {
       #if JUCE_MAC || JUCE_LINUX
        auto ptyLink { std::make_unique<PtyLink> () };
        if (! ptyLink->isValid ())
        {
            std::cout << "could not create a pseudo terminal" << std::endl;
            return 1;
        }
        link = std::move (ptyLink);
       #else
        std::cout << std::endl << "There is no built in loopback on this platform, use --pair, --echo or --write-only" << std::endl;
       #endif
    }

    if (link != nullptr)
        runTests (*link, options);

    std::cout << std::endl << numPassed << " passed, " << numFailed << " failed, " << numSkipped << " skipped" << std::endl;
    return numFailed == 0 ? 0 : 1;
}
