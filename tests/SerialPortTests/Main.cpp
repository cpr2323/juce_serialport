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
            {
                // the test's callback only sees data which arrives once the fixture is ready
                input = std::make_unique<SerialPortInputStream> (&port, [isReady = ready, testCallback = std::move (callback)] (const void* data, int numBytes)
                {
                    if (isReady->load ())
                        testCallback (data, numBytes);
                }, alsoBuffer);
            }
            else
            {
                input = std::make_unique<SerialPortInputStream> (&port);
            }
            output = std::make_unique<SerialPortOutputStream> (&port);

            // let the device start up, and anything left over from a previous test arrive (a USB serial bridge, eg. an
            // Uno's 16U2, can replay a few bytes once the port is reopened), then discard it
            juce::Thread::sleep (juce::jmax (100, link.getSettleTimeMs ()));
            if (! usesCallback || alsoBuffer)
            {
                char discard [256];
                while (input->read (discard, sizeof (discard)) > 0) {}
            }
            if (auto received { link.getFarEndReceived () })
                received->take ();
            ready->store (true);
        }

        ~Fixture ()
        {
            output = nullptr;
            input = nullptr;
            port.close ();
        }

        bool isOpen () const { return port.exists (); }

        std::shared_ptr<std::atomic<bool>> ready { std::make_shared<std::atomic<bool>> (false) };
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

        // the count includes anything the fixture discarded while the device started
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
    // sends to the port under test, from the far end, or through the port itself when the device echoes
    void sendToPort (Link& link, Fixture& fixture, const void* data, size_t numBytes)
    {
        if (link.hasFarEnd ())
            link.sendFromFarEnd (data, numBytes);
        else
            fixture.output->write (data, numBytes);
    }

    void sendToPort (Link& link, Fixture& fixture, const char* text)
    {
        sendToPort (link, fixture, text, std::strlen (text));
    }

    bool waitForBuffered (SerialPortInputStream& input, juce::int64 numBytes, int timeoutMs)
    {
        const auto endTime { juce::Time::getMillisecondCounter () + static_cast<juce::uint32> (timeoutMs) };
        while (input.getTotalLength () < numBytes)
        {
            if (juce::Time::getMillisecondCounter () >= endTime)
                return false;
            juce::Thread::sleep (2);
        }
        return true;
    }

    juce::MemoryBlock readAll (SerialPortInputStream& input)
    {
        juce::MemoryBlock result;
        char buffer [256];
        for (auto numRead { input.read (buffer, sizeof (buffer)) }; numRead > 0; numRead = input.read (buffer, sizeof (buffer)))
            result.append (buffer, static_cast<size_t> (numRead));
        return result;
    }

    // the tests run on their own thread, while main () runs the message loop, which delivers the change notifications
    bool waitUntil (std::function<bool ()> condition, int timeoutMs)
    {
        const auto endTime { juce::Time::getMillisecondCounter () + static_cast<juce::uint32> (timeoutMs) };
        while (! condition ())
        {
            if (juce::Time::getMillisecondCounter () >= endTime)
                return false;
            juce::Thread::sleep (2);
        }
        return true;
    }

    //==============================================================================
    void testFlush (Link& link, const Options& options)
    {
        section ("flush () and waitUntilSent ()");

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        // slow callbacks, so flush returning before they have run would be caught
        constexpr auto kNumWrites { 4 };
        std::atomic<int> numCompleted { 0 };
        const auto data { makeTestData (1024, 11) };
        for (auto writeIndex { 0 }; writeIndex < kNumWrites; ++writeIndex)
            fixture.output->write (data.getData (), data.getSize (), [&numCompleted] (bool)
            {
                juce::Thread::sleep (20);
                ++numCompleted;
            });
        fixture.output->flush ();
        check (numCompleted == kNumWrites, "flush returns after every completion callback has returned (" + juce::String (numCompleted.load ()) + " of " + juce::String (kNumWrites) + ")");
        check (fixture.output->getNumBytesPending () == 0 && fixture.output->getNumBytesSent () == data.getSize () * kNumWrites, "and everything has been sent");

        // from a completion callback, waiting for the callbacks would wait for itself
        auto* output { fixture.output.get () };
        std::atomic<bool> innerCalled { false };
        std::atomic<bool> innerResult { false };
        const auto startTicks { juce::Time::getHighResolutionTicks () };
        output->write ("x", 1, [output, &innerCalled, &innerResult] (bool)
        {
            innerResult = output->waitUntilSent (2000);
            innerCalled = true;
        });
        check (output->waitUntilSent (5000) && innerCalled.load () && innerResult.load (), "waitUntilSent called from a completion callback returns true");
        check (ticksToMs (juce::Time::getHighResolutionTicks () - startTicks) < 1000.0, "without waiting for itself");
    }

    //==============================================================================
    void testBulkPolling (Link& link, const Options& options)
    {
        section ("Polling reads of bulk data");

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        const auto numBytes { static_cast<size_t> (link.echoes () ? 16384 : 262144) };
        const auto data { makeTestData (numBytes, 12) };
        auto sender { std::thread ([&] { sendToPort (link, fixture, data.getData (), data.getSize ()); }) };

        // an odd size, so the reads don't line up with the stream's ring buffer
        juce::MemoryBlock gathered;
        char buffer [333];
        const auto endTime { juce::Time::getMillisecondCounter () + static_cast<juce::uint32> (transferTimeMs (numBytes, options.bps) * (link.echoes () ? 2 : 1)) };
        while (gathered.getSize () < numBytes && juce::Time::getMillisecondCounter () < endTime)
        {
            const auto numRead { fixture.input->read (buffer, sizeof (buffer)) };
            if (numRead > 0)
                gathered.append (buffer, static_cast<size_t> (numRead));
            else
                juce::Thread::sleep (1);
        }
        sender.join ();
        check (gathered == data, juce::String (numBytes / 1024) + " KB read intact, 333 bytes at a time, while it was arriving");
        check (fixture.input->isExhausted (), "then the stream is exhausted");
    }

    //==============================================================================
    void testCanReadString (Link& link, const Options& options)
    {
        section ("canReadString ()");

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        sendToPort (link, fixture, "abc");
        waitForBuffered (*fixture.input, 3, 3000);
        check (! fixture.input->canReadString (), "no string until its terminator arrives");

        const char rest [] { '\0', 'd', 'e', 'f' };
        sendToPort (link, fixture, rest, sizeof (rest));
        waitForBuffered (*fixture.input, 7, 3000);
        check (fixture.input->canReadString (), "a string once its terminator arrives");
        char received [4] {};
        check (fixture.input->read (received, 4) == 4 && std::memcmp (received, "abc", 4) == 0, "the string can be read");
        check (! fixture.input->canReadString (), "no string in what is left");
        check (readAll (*fixture.input) == juce::MemoryBlock ("def", 3), "the rest can be read");
    }

    //==============================================================================
    void testCallbackAndBuffer (Link& link, const Options& options)
    {
        section ("Callback and buffer together");

        Collector received;
        Fixture fixture { link, options, [&received] (const void* data, int numBytes) { received.append (data, static_cast<size_t> (numBytes)); }, true };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        const auto data { makeTestData (4096, 13) };
        sendToPort (link, fixture, data.getData (), data.getSize ());
        const auto timeoutMs { transferTimeMs (data.getSize (), options.bps) * (link.echoes () ? 2 : 1) };
        check (received.waitForSize (data.getSize (), timeoutMs), "the callback received the data");
        check (waitForBuffered (*fixture.input, static_cast<juce::int64> (data.getSize ()), timeoutMs), "and it was buffered too");
        check (received.take () == data, "the callback's copy is intact");
        check (readAll (*fixture.input) == data, "the buffered copy is intact");
    }

    //==============================================================================
    void testNotifications (Link& link, const Options& options)
    {
        section ("Change notifications on the message thread");

        struct Listener : public juce::ChangeListener
        {
            void changeListenerCallback (juce::ChangeBroadcaster*) override
            {
                ++numNotifications;
                if (! juce::MessageManager::getInstance ()->isThisTheMessageThread ())
                    ++numOffMessageThread;
            }
            std::atomic<int> numNotifications { 0 };
            std::atomic<int> numOffMessageThread { 0 };
        };
        Listener listener;

        Fixture fixture { link, options };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }
        // listeners are added and removed on the message thread, or with it locked
        {
            const juce::MessageManagerLock messageManagerLock;
            fixture.input->addChangeListener (&listener);
        }

        sendToPort (link, fixture, "off\n");
        waitForBuffered (*fixture.input, 4, 3000);
        juce::Thread::sleep (300);
        check (listener.numNotifications == 0, "NOTIFY_OFF, the default, sends nothing");
        readAll (*fixture.input);

        fixture.input->setNotify (SerialPortInputStream::NOTIFY_ON_CHAR, '\n');
        sendToPort (link, fixture, "abc");
        waitForBuffered (*fixture.input, 3, 3000);
        juce::Thread::sleep (300);
        check (listener.numNotifications == 0, "NOTIFY_ON_CHAR sends nothing until the character arrives");
        sendToPort (link, fixture, "def\n");
        check (waitUntil ([&listener] { return listener.numNotifications > 0; }, 3000), "and a notification once it does");
        check (fixture.input->readNextLine () == "abcdef", "the line can be read in the listener");

        fixture.input->setNotify (SerialPortInputStream::NOTIFY_ALWAYS);
        listener.numNotifications = 0;
        sendToPort (link, fixture, "z");
        check (waitUntil ([&listener] { return listener.numNotifications > 0; }, 3000), "NOTIFY_ALWAYS sends a notification for any data");
        readAll (*fixture.input);

        fixture.input->setNotify (SerialPortInputStream::NOTIFY_OFF);
        juce::Thread::sleep (100);
        listener.numNotifications = 0;
        sendToPort (link, fixture, "q");
        waitForBuffered (*fixture.input, 1, 3000);
        juce::Thread::sleep (300);
        check (listener.numNotifications == 0, "turning them off again stops them");
        check (listener.numOffMessageThread == 0, "every notification arrived on the message thread");

        {
            const juce::MessageManagerLock messageManagerLock;
            fixture.input->removeChangeListener (&listener);
        }
    }

    //==============================================================================
    void testSwitchingModes (Link& link, const Options& options)
    {
        section ("Switching between callback and polling");

        Collector first;
        Collector second;
        Collector third;
        Fixture fixture { link, options, [&first] (const void* data, int numBytes) { first.append (data, static_cast<size_t> (numBytes)); } };
        if (! fixture.isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        sendToPort (link, fixture, "one");
        check (first.waitForSize (3, 3000) && first.take () == juce::MemoryBlock ("one", 3) && fixture.input->isExhausted (),
               "callback only: the callback gets the data, and nothing is buffered");

        fixture.input->setDataReceivedCallback (nullptr);
        sendToPort (link, fixture, "two");
        check (waitForBuffered (*fixture.input, 3, 3000) && readAll (*fixture.input) == juce::MemoryBlock ("two", 3) && first.getSize () == 0,
               "no callback: the data is buffered for polling, and the old callback is not called");

        fixture.input->setDataReceivedCallback ([&second] (const void* data, int numBytes) { second.append (data, static_cast<size_t> (numBytes)); }, true);
        sendToPort (link, fixture, "three");
        check (second.waitForSize (5, 3000) && waitForBuffered (*fixture.input, 5, 3000)
               && second.take () == juce::MemoryBlock ("three", 5) && readAll (*fixture.input) == juce::MemoryBlock ("three", 5),
               "a callback which also buffers: both get the data");

        fixture.input->setDataReceivedCallback ([&third] (const void* data, int numBytes) { third.append (data, static_cast<size_t> (numBytes)); });
        sendToPort (link, fixture, "four");
        const auto thirdArrived { third.waitForSize (4, 3000) };
        juce::Thread::sleep (100);
        check (thirdArrived && third.take () == juce::MemoryBlock ("four", 4) && fixture.input->isExhausted () && second.getSize () == 0,
               "back to callback only: only the new callback gets the data");
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
    // what tests/daisy_echo reports, the raw CDC values (see daisy_echo.cpp)
    struct DeviceState
    {
        bool valid { false };
        uint32_t rate { 0 };
        int dataBits { 0 };
        int parity { 0 };   // 0 none, 1 odd, 2 even, 3 mark, 4 space
        int stopBits { 0 }; // 0 = 1, 1 = 1.5, 2 = 2
        bool dtr { false };
        bool rts { false };
        int numDtrDrops { 0 };
        int numOverflows { 0 };

        juce::String describe () const
        {
            if (! valid)
                return "no reply";
            return juce::String (rate) + " " + juce::String (dataBits) + juce::String::charToString ("NOEMS"[juce::jlimit (0, 4, parity)])
                 + juce::StringArray { "1", "1.5", "2" }[juce::jlimit (0, 2, stopBits)] + ", DTR " + (dtr ? "on" : "off") + ", RTS " + (rts ? "on" : "off");
        }
    };

    const uint8_t kDeviceCommandPrefix[] { 0xF5, 0x5A, 0xA5, 0x0F };

    void sendDeviceCommand (Fixture& fixture, char command)
    {
        uint8_t message[5];
        std::memcpy (message, kDeviceCommandPrefix, 4);
        message[4] = static_cast<uint8_t> (command);
        fixture.output->write (message, sizeof (message));
    }

    // the fixture must be polling, ie. have no receive callback
    DeviceState queryDevice (Fixture& fixture)
    {
        sendDeviceCommand (fixture, 'S');

        // the echoed command comes first, then the reply
        juce::MemoryBlock received;
        const auto endTime { juce::Time::getMillisecondCounter () + 2000 };
        juce::String line;
        while (juce::Time::getMillisecondCounter () < endTime)
        {
            char buffer [256];
            const auto numRead { fixture.input->read (buffer, sizeof (buffer)) };
            if (numRead > 0)
                received.append (buffer, static_cast<size_t> (numRead));
            else
                juce::Thread::sleep (2);

            // the echoed prefix isn't valid UTF-8, so search the raw bytes
            const std::string text { static_cast<const char*> (received.getData ()), received.getSize () };
            const auto start { text.find ("\x02STATE ") };
            const auto end { start != std::string::npos ? text.find ('\n', start) : std::string::npos };
            if (end != std::string::npos)
            {
                line = juce::String (text.substr (start + 7, end - start - 7));
                break;
            }
        }

        DeviceState state;
        juce::StringPairArray values;
        for (const auto& token : juce::StringArray::fromTokens (line, " ", ""))
            values.set (token.upToFirstOccurrenceOf ("=", false, false), token.fromFirstOccurrenceOf ("=", false, false));
        state.valid = values.containsKey ("rate");
        state.rate = static_cast<uint32_t> (values["rate"].getLargeIntValue ());
        state.dataBits = values["data"].getIntValue ();
        state.parity = values["parity"].getIntValue ();
        state.stopBits = values["stop"].getIntValue ();
        state.dtr = values["dtr"] == "1";
        state.rts = values["rts"] == "1";
        state.numDtrDrops = values["dtrdrops"].getIntValue ();
        state.numOverflows = values["overflows"].getIntValue ();
        return state;
    }

    bool containsByte (const juce::MemoryBlock& block, uint8_t byte)
    {
        return block.getSize () > 0 && std::memchr (block.getData (), byte, block.getSize ()) != nullptr;
    }

    // sends data through the echo device, and returns how long it took to come back, or -1 if it didn't
    double echoTimeMs (Fixture& fixture, const juce::MemoryBlock& data, int timeoutMs)
    {
        readAll (*fixture.input);
        const auto startTicks { juce::Time::getHighResolutionTicks () };
        fixture.output->write (data.getData (), data.getSize ());
        if (! waitForBuffered (*fixture.input, static_cast<juce::int64> (data.getSize ()), timeoutMs))
            return -1.0;
        const auto elapsedMs { ticksToMs (juce::Time::getHighResolutionTicks () - startTicks) };
        return readAll (*fixture.input) == data ? elapsedMs : -1.0;
    }

    void testDeviceLineState (Link& link, const Options& options)
    {
        section ("Line settings and control lines, as the device sees them");

        auto fixture { std::make_unique<Fixture> (link, options) };
        if (! fixture->isOpen ())
        {
            check (false, "the port opens");
            return;
        }

        auto state { queryDevice (*fixture) };
        check (state.valid, "the device reports its state (" + state.describe () + ")");
        if (! state.valid)
            return;
        check (state.rate == options.bps && state.dataBits == 8 && state.parity == 0 && state.stopBits == 0, "opening the port sent " + juce::String (options.bps) + " 8N1");
        check (state.dtr && state.rts, "DTR and RTS are on");

        // the rate is passed through as a number, so a CDC device sees any rate, standard or not
        juce::StringArray rateFailures;
        for (const uint32_t bps : { 300u, 1200u, 9600u, 31250u, 57600u, 115200u, 230400u, 250000u, 460800u, 921600u,
                                    1000000u, 2000000u, 3000000u, 12345u })
        {
            SerialPortConfig config;
            config.bps = bps;
            SerialPortConfig readBack;
            const auto set { fixture->port.setConfig (config) && fixture->port.getConfig (readBack) && readBack.bps == bps };
            state = queryDevice (*fixture);
            if (! set || state.rate != bps)
                rateFailures.add (juce::String (bps) + (set ? "" : " (setConfig)") + " -> " + state.describe ());
        }
        check (rateFailures.isEmpty (), "14 rates, from 300 to 3000000 and 12345, reach the device" + (rateFailures.isEmpty () ? juce::String () : ": " + rateFailures.joinIntoString (", ")));

        // every data bits, parity and stop bits combination. 1.5 stop bits is only valid with 5 data bits
        struct Framing { uint32_t dataBits; SerialPortConfig::SerialPortParity parity; SerialPortConfig::SerialPortStopBits stopBits; int cdcParity; int cdcStopBits; };
        const SerialPortConfig::SerialPortParity parities[] { SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::SERIALPORT_PARITY_ODD,
                                                              SerialPortConfig::SERIALPORT_PARITY_EVEN, SerialPortConfig::SERIALPORT_PARITY_MARK,
                                                              SerialPortConfig::SERIALPORT_PARITY_SPACE };
        std::vector<Framing> framings;
        for (const uint32_t dataBits : { 5u, 6u, 7u, 8u })
            for (auto parityIndex { 0 }; parityIndex < 5; ++parityIndex)
            {
                framings.push_back ({ dataBits, parities[parityIndex], SerialPortConfig::STOPBITS_1, parityIndex, 0 });
                framings.push_back ({ dataBits, parities[parityIndex], SerialPortConfig::STOPBITS_2, parityIndex, 2 });
            }
        for (auto parityIndex { 0 }; parityIndex < 5; ++parityIndex)
            framings.push_back ({ 5u, parities[parityIndex], SerialPortConfig::STOPBITS_1ANDHALF, parityIndex, 1 });

        juce::StringArray framingFailures;
        for (const auto& framing : framings)
        {
            SerialPortConfig config { options.bps, framing.dataBits, framing.parity, framing.stopBits, SerialPortConfig::FLOWCONTROL_NONE };
            SerialPortConfig readBack;
            const auto set { fixture->port.setConfig (config) && fixture->port.getConfig (readBack)
                             && readBack.databits == framing.dataBits && readBack.parity == framing.parity && readBack.stopbits == framing.stopBits };
            state = queryDevice (*fixture);
            if (! set || state.dataBits != static_cast<int> (framing.dataBits) || state.parity != framing.cdcParity || state.stopBits != framing.cdcStopBits)
            {
                DeviceState wanted;
                wanted.valid = true;
                wanted.rate = options.bps;
                wanted.dataBits = static_cast<int> (framing.dataBits);
                wanted.parity = framing.cdcParity;
                wanted.stopBits = framing.cdcStopBits;
                framingFailures.add (wanted.describe ().upToFirstOccurrenceOf (",", false, false).fromFirstOccurrenceOf (" ", false, false)
                                     + (set ? "" : " (setConfig/getConfig)") + " -> " + state.describe ().upToFirstOccurrenceOf (",", false, false));
            }
        }
        check (framingFailures.isEmpty (), juce::String (static_cast<int> (framings.size ())) + " data bits/parity/stop bits combinations reach the device, and read back"
                                           + (framingFailures.isEmpty () ? juce::String () : ": " + framingFailures.joinIntoString (", ")));

        // a USB device is sent bytes whatever the framing, so 5 data bits doesn't mask the top 3 bits off
        {
            SerialPortConfig config { options.bps, 5, SerialPortConfig::SERIALPORT_PARITY_MARK, SerialPortConfig::STOPBITS_1ANDHALF, SerialPortConfig::FLOWCONTROL_NONE };
            fixture->port.setConfig (config);
            const auto data { makeTestData (4096, 14) };
            check (echoTimeMs (*fixture, data, 3000) >= 0.0, "data is echoed unchanged at 5M1.5, all 8 bits of each byte");
        }

        // flow control. with none or XON/XOFF, DTR and RTS are simply on. with RTS/CTS the driver drives RTS
        for (const auto flowControl : { SerialPortConfig::FLOWCONTROL_XONXOFF, SerialPortConfig::FLOWCONTROL_HARDWARE, SerialPortConfig::FLOWCONTROL_NONE })
        {
            const juce::String name { flowControl == SerialPortConfig::FLOWCONTROL_NONE ? "no flow control"
                                    : flowControl == SerialPortConfig::FLOWCONTROL_XONXOFF ? "XON/XOFF" : "RTS/CTS" };
            SerialPortConfig config { options.bps, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1, flowControl };
            SerialPortConfig readBack;
            check (fixture->port.setConfig (config) && fixture->port.getConfig (readBack) && readBack.flowcontrol == flowControl, name + ": set and read back");
            state = queryDevice (*fixture);
            if (flowControl == SerialPortConfig::FLOWCONTROL_HARDWARE)
            {
                check (state.dtr, name + ": DTR is on");
                info (name + ": RTS is " + (state.rts ? "on" : "off") + ", driven by the driver's handshake");
                // CDC ACM has no CTS line, so whether data flows depends on what the driver assumes CTS is
                const auto ms { echoTimeMs (*fixture, makeTestData (256, 15), 3000) };
                info (name + ": " + (ms >= 0.0 ? "data still flows, the driver treats the missing CTS as on" : "data does not flow, the driver treats the missing CTS as off"));
            }
            else
            {
                check (state.dtr && state.rts, name + ": DTR and RTS are on (" + state.describe () + ")");
                check (echoTimeMs (*fixture, makeTestData (256, 15), 3000) >= 0.0, name + ": data flows");
            }
        }

        // XON/XOFF: the device sends XOFF, then XON a second later
        {
            SerialPortConfig config { options.bps, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1, SerialPortConfig::FLOWCONTROL_XONXOFF };
            fixture->port.setConfig (config);
            readAll (*fixture->input);
            sendDeviceCommand (*fixture, 'X');
            juce::Thread::sleep (200);
            const auto xoffPassedOn { containsByte (readAll (*fixture->input), 0x13) };

            // the XON may arrive with the echo, so look for the echo in whatever arrives
            const auto startTicks { juce::Time::getHighResolutionTicks () };
            fixture->output->write ("paused", 6);
            juce::MemoryBlock received;
            auto echoMs { -1.0 };
            while (ticksToMs (juce::Time::getHighResolutionTicks () - startTicks) < 3000.0)
            {
                const auto more { readAll (*fixture->input) };
                received.append (more.getData (), more.getSize ());
                if (std::string (static_cast<const char*> (received.getData ()), received.getSize ()).find ("paused") != std::string::npos)
                {
                    echoMs = ticksToMs (juce::Time::getHighResolutionTicks () - startTicks);
                    break;
                }
                juce::Thread::sleep (2);
            }
            juce::Thread::sleep (1000);
            const auto rest { readAll (*fixture->input) };
            received.append (rest.getData (), rest.getSize ());
            const auto xonPassedOn { containsByte (received, 0x11) };
            info ("XON/XOFF: after the device sent XOFF, an echo took " + (echoMs >= 0.0 ? juce::String (echoMs, 1) + " ms" : juce::String ("over 3 s"))
                  + (echoMs > 500.0 ? ", so the driver paused sending until XON" : ", so the driver did not pause sending"));
            info (juce::String ("XON/XOFF: the driver ") + (xoffPassedOn || xonPassedOn ? "passed the XOFF/XON characters on to the application" : "kept the XOFF/XON characters to itself"));
            config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;
            fixture->port.setConfig (config);
        }

        state = queryDevice (*fixture);
        check (state.numOverflows == 0, "the device's receive buffer never overflowed while the port was open");

        // closing the port drops DTR, which tells a device the application has gone
        const auto dtrDropsBefore { state.numDtrDrops };
        fixture = nullptr;
        juce::Thread::sleep (200);
        fixture = std::make_unique<Fixture> (link, options);
        state = queryDevice (*fixture);
        check (state.valid && state.numDtrDrops > dtrDropsBefore && state.dtr, "closing the port turned DTR off, reopening it turned DTR on again");
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
        testFlush (link, options);
        testDeleteWithPendingWrites (link, options);
        testWriteNeverBlocks (link, options);

        if (link.hasFarEnd () || link.echoes ())
        {
            testReceiveCallback (link, options);
            testPolling (link, options);
            testBulkPolling (link, options);
            testCanReadString (link, options);
            testCallbackAndBuffer (link, options);
            testNotifications (link, options);
            testSwitchingModes (link, options);
        }
        else
        {
            for (const auto* name : { "Receive callback", "Polling reads", "Polling reads of bulk data", "canReadString ()",
                                      "Callback and buffer together", "Change notifications on the message thread",
                                      "Switching between callback and polling" })
                skip (name, "needs a far end, or an echo device");
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

        if (link.reportsLineState ())
            testDeviceLineState (link, options);
        else
            skip ("Line settings and control lines, as the device sees them", "needs a device which reports them (tests/daisy_echo)");

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
                     "  --daisy PORT         as --echo, with a Daisy Seed running tests/daisy_echo, which also reports\n"
                     "                       the line settings and control lines it receives\n"
                     "  --write-only PORT    only the send tests, for a port with nothing connected\n"
                     "  --list               list the serial ports\n"
                     "  --baud N             the rate to test at, default 115200\n"
                     "  --unplug             with --pair or --echo, also test unplugging the device\n";
    }
}

static int runAllTests (const juce::StringArray& args)
{
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
    else if (const auto daisyIndex { args.indexOf ("--daisy") }; daisyIndex >= 0)
    {
        link = std::make_unique<DaisyLink> (args[daisyIndex + 1]);
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

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::StringArray args;
    for (auto index { 1 }; index < argc; ++index)
        args.add (juce::String::fromUTF8 (argv[index]));

    // the tests run on their own thread, so this one can run the message loop, as in an app, and deliver the
    // change notifications while the tests wait for them
    class TestThread : public juce::Thread
    {
    public:
        explicit TestThread (const juce::StringArray& testArgs) : Thread ("Tests"), args { testArgs } {}

        void run () override
        {
            result = runAllTests (args);
            juce::MessageManager::getInstance ()->stopDispatchLoop ();
        }

        juce::StringArray args;
        std::atomic<int> result { 1 };
    };

    TestThread testThread { args };
    testThread.startThread ();
   #if JUCE_MAC
    // in a console program on macOS, runDispatchLoop returns at once, as there is no Cocoa application for it to run,
    // so the run loop is pumped directly (which needs JUCE_MODAL_LOOPS_PERMITTED, see CMakeLists.txt)
    while (testThread.isThreadRunning ())
        juce::MessageManager::getInstance ()->runDispatchLoopUntil (20);
   #else
    juce::MessageManager::getInstance ()->runDispatchLoop ();
   #endif
    testThread.waitForThreadToExit (-1);
    return testThread.result;
}
