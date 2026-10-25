// Serial Port classes in a Juce stylee, by graffiti
// see juce_serialport.h for details
//
// Updated for current Juce API 8/1/13 Marc Lindahl
//

#ifdef JUCE_SERIALPORT_H_INCLUDED
 /* When you add this cpp file to your project, you mustn't include it in a file where you've
    already included any other headers - just put it inside a file on its own, possibly with your config
    flags preceding it, but don't include anything else. That also includes avoiding any automatic prefix
    header files that the compiler may be using.
 */
 #error "Incorrect use of JUCE cpp file"
#endif

#include <juce_core/system/juce_TargetPlatform.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#elif JUCE_MAC || JUCE_LINUX || JUCE_BSD
 #include <cerrno>
 #include <climits>
 #include <cstdlib>
 #include <fcntl.h>
 #include <poll.h>
 #include <sys/ioctl.h>
 #include <sys/select.h>
 #include <termios.h>
 #include <unistd.h>
 #if JUCE_MAC
  #include <CoreFoundation/CoreFoundation.h>
  #include <IOKit/IOKitLib.h>
  #include <IOKit/IOBSD.h>
  #include <IOKit/serial/IOSerialKeys.h>
  #include <IOKit/serial/ioss.h>
 #endif
#endif

#include "juce_serialport.h"

#if JUCE_ANDROID
 #include <juce_core/native/juce_JNIHelpers_android.h>
#endif

#include <cstring>
#include <vector>

#if JUCE_WINDOWS
 #include "native/juce_serialport_windows.cpp"
#elif JUCE_MAC || JUCE_LINUX || JUCE_BSD
 #include "native/juce_serialport_posix.cpp"
#elif JUCE_ANDROID
 #include "native/juce_serialport_android.cpp"
#else
 // iOS, and anything else, has no serial port support
 #include "native/juce_serialport_unsupported.cpp"
#endif

namespace
{
    // how long the stream threads block in the platform I/O before checking whether they should exit. the I/O
    // returns as soon as there is data, or the write has completed, so these do not add latency
    constexpr auto kReadTimeoutMs { 100 };
    constexpr auto kWriteTimeoutMs { 1000 };
    constexpr auto kReadChunkSize { 4096 };
    constexpr auto kMaxWriteChunkSize { 4096 };

    // about 125ms of data at the port's rate, well inside the write timeout, so a write is only ever cancelled when
    // the device has stalled. some drivers do not report the progress of a cancelled write accurately
    int getWriteChunkSize (uint32_t bps)
    {
        if (bps == 0)
            return kMaxWriteChunkSize;
        return juce::jlimit (16, kMaxWriteChunkSize, static_cast<int> (bps / 80));
    }
}

//////////////////////////////////////////////////////////////////
// SerialPortByteQueue
//////////////////////////////////////////////////////////////////
void SerialPortByteQueue::reserve (size_t minimumCapacity)
{
    if (minimumCapacity <= storageSize)
        return;

    // a power of two, so the indexes wrap with a mask
    size_t newSize { 256 };
    while (newSize < minimumCapacity)
        newSize *= 2;

    juce::HeapBlock<char> newStorage { newSize };
    peek (newStorage.get (), numBytes);
    storage.swapWith (newStorage);
    storageSize = newSize;
    readIndex = 0;
}

void SerialPortByteQueue::push (const void* source, size_t numBytesToPush)
{
    if (numBytesToPush == 0)
        return;

    reserve (numBytes + numBytesToPush);
    const auto writeIndex { (readIndex + numBytes) & (storageSize - 1) };
    const auto firstPart { juce::jmin (numBytesToPush, storageSize - writeIndex) };
    std::memcpy (storage.get () + writeIndex, source, firstPart);
    std::memcpy (storage.get (), static_cast<const char*> (source) + firstPart, numBytesToPush - firstPart);
    numBytes += numBytesToPush;
}

size_t SerialPortByteQueue::peek (void* dest, size_t maxBytes) const noexcept
{
    const auto numToCopy { juce::jmin (maxBytes, numBytes) };
    if (numToCopy == 0)
        return 0;

    const auto firstPart { juce::jmin (numToCopy, storageSize - readIndex) };
    std::memcpy (dest, storage.get () + readIndex, firstPart);
    std::memcpy (static_cast<char*> (dest) + firstPart, storage.get (), numToCopy - firstPart);
    return numToCopy;
}

void SerialPortByteQueue::pop (size_t numBytesToPop) noexcept
{
    const auto numToPop { juce::jmin (numBytesToPop, numBytes) };
    numBytes -= numToPop;
    readIndex = numBytes == 0 ? 0 : (readIndex + numToPop) & (storageSize - 1);
}

bool SerialPortByteQueue::contains (char c) const noexcept
{
    if (numBytes == 0)
        return false;

    const auto firstPart { juce::jmin (numBytes, storageSize - readIndex) };
    return std::memchr (storage.get () + readIndex, c, firstPart) != nullptr
        || std::memchr (storage.get (), c, numBytes - firstPart) != nullptr;
}

//////////////////////////////////////////////////////////////////
// SerialPort
//////////////////////////////////////////////////////////////////
SerialPort::SerialPort (DebugFunction theDebugLog)
    : native { std::make_unique<Native> () },
      DebugLogInternal { theDebugLog }
{
}

SerialPort::SerialPort (const juce::String& newPortPath, DebugFunction theDebugLog)
    : SerialPort (theDebugLog)
{
    open (newPortPath);
}

SerialPort::SerialPort (const juce::String& newPortPath, const SerialPortConfig& config, DebugFunction theDebugLog)
    : SerialPort (theDebugLog)
{
    if (open (newPortPath))
        setConfig (config);
}

SerialPort::~SerialPort ()
{
    close ();
}

bool SerialPort::exists () const
{
    return isOpen () && ! connectionLost.load ();
}

void SerialPort::setConnectionLostCallback (std::function<void ()> newCallback)
{
    const juce::ScopedLock sl { connectionLostCallbackLock };
    connectionLostCallback = std::move (newCallback);
}

void SerialPort::reportConnectionLost (const juce::String& prefix, const juce::String& msg)
{
    // only the first thread to notice reports it
    if (connectionLost.exchange (true))
        return;

    DebugLog (prefix, "connection lost: " + msg);

    const juce::ScopedLock sl { connectionLostCallbackLock };
    if (connectionLostCallback != nullptr)
        connectionLostCallback ();
}

//////////////////////////////////////////////////////////////////
// SerialPortInputStream
//////////////////////////////////////////////////////////////////
SerialPortInputStream::SerialPortInputStream (SerialPort* newPort)
    : SerialPortInputStream (newPort, nullptr, true)
{
}

SerialPortInputStream::SerialPortInputStream (SerialPort* newPort, DataReceivedCallback newDataReceivedCallback, bool alsoBufferData)
    : Thread ("SerialInThread"),
      port { newPort },
      dataReceivedCallback { std::move (newDataReceivedCallback) },
      bufferData { dataReceivedCallback == nullptr || alsoBufferData }
{
    if (port == nullptr)
        return;

    ++port->numAttachedStreams;
    startThread ();
}

SerialPortInputStream::~SerialPortInputStream ()
{
    signalThreadShouldExit ();
    cancel ();
    // the platform reads time out, so the thread will exit, and it must have before the port can be released
    waitForThreadToExit (-1);

    if (port != nullptr)
        --port->numAttachedStreams;
}

void SerialPortInputStream::setDataReceivedCallback (DataReceivedCallback newCallback, bool alsoBufferData)
{
    const juce::ScopedLock sl { callbackLock };
    bufferData = newCallback == nullptr || alsoBufferData;
    dataReceivedCallback = std::move (newCallback);
}

void SerialPortInputStream::setNotify (notifyflag newNotify, char c)
{
    notifyChar = c;
    notify = newNotify;
}

bool SerialPortInputStream::canReadString ()
{
    const juce::ScopedLock sl { bufferCriticalSection };
    return buffer.contains (0);
}

bool SerialPortInputStream::canReadLine ()
{
    const juce::ScopedLock sl { bufferCriticalSection };
    return buffer.contains ('\n');
}

void SerialPortInputStream::run ()
{
    if (port == nullptr)
        return;

    juce::HeapBlock<char> chunk { kReadChunkSize };

    while (! threadShouldExit () && port->exists ())
    {
        const auto bytesRead { port->readBytes (chunk.get (), kReadChunkSize, kReadTimeoutMs) };
        if (bytesRead < 0)
        {
            port->reportConnectionLost ("SerialPortInputStream::run", "read failed");
            break;
        }
        if (bytesRead == 0)
            continue;

        numBytesReceived += static_cast<juce::uint64> (bytesRead);

        {
            // held while calling back, so setDataReceivedCallback can guarantee the old callback is not running
            const juce::ScopedLock sl { callbackLock };
            if (bufferData)
            {
                const juce::ScopedLock bsl { bufferCriticalSection };
                buffer.push (chunk.get (), static_cast<size_t> (bytesRead));
            }
            if (dataReceivedCallback != nullptr)
                dataReceivedCallback (chunk.get (), bytesRead);
        }

        const auto curNotify { notify.load () };
        if (curNotify == NOTIFY_ALWAYS || (curNotify == NOTIFY_ON_CHAR && std::memchr (chunk.get (), notifyChar.load (), static_cast<size_t> (bytesRead)) != nullptr))
            sendChangeMessage ();
    }
}

void SerialPortInputStream::cancel ()
{
    if (port != nullptr && port->isOpen ())
        port->cancel ();
}

int SerialPortInputStream::read (void* destBuffer, int maxBytesToRead)
{
    if (port == nullptr)
        return -1;

    const juce::ScopedLock sl { bufferCriticalSection };

    // data received before the device went away can still be read
    if (buffer.size () == 0 && ! port->exists ())
        return -1;

    if (maxBytesToRead <= 0)
        return 0;

    return static_cast<int> (buffer.read (destBuffer, static_cast<size_t> (maxBytesToRead)));
}

juce::String SerialPortInputStream::readNextLine ()
{
    juce::MemoryOutputStream line;
    char c;
    while (read (&c, 1) == 1 && c != '\n')
        line.writeByte (c);
    return line.toString ().trim ();
}

juce::int64 SerialPortInputStream::getTotalLength ()
{
    const juce::ScopedLock sl { bufferCriticalSection };
    return static_cast<juce::int64> (buffer.size ());
}

bool SerialPortInputStream::isExhausted ()
{
    const juce::ScopedLock sl { bufferCriticalSection };
    return buffer.size () == 0;
}

//////////////////////////////////////////////////////////////////
// SerialPortOutputStream
//////////////////////////////////////////////////////////////////
SerialPortOutputStream::SerialPortOutputStream (SerialPort* newPort)
    : Thread ("SerialOutThread"), port { newPort }
{
    if (port == nullptr)
        return;

    ++port->numAttachedStreams;
    startThread ();
}

SerialPortOutputStream::~SerialPortOutputStream ()
{
    signalThreadShouldExit ();
    cancel ();
    // the platform writes time out, so the thread will exit, and it must have before the port can be released.
    // the thread fails any writes still pending before it exits
    waitForThreadToExit (-1);

    if (port != nullptr)
        --port->numAttachedStreams;
}

void SerialPortOutputStream::run ()
{
    if (port == nullptr)
        return;

    juce::HeapBlock<char> chunk { kMaxWriteChunkSize };
    std::vector<WriteCompleteCallback> completed;
    completed.reserve (16);

    // moves the callbacks of the writes which have been sent, so they can be called without holding the lock
    auto collectCompleted = [this, &completed] ()
    {
        const auto sent { numBytesSent.load () };
        while (! pendingCompletions.empty () && pendingCompletions.front ().endPosition <= sent)
        {
            completed.push_back (std::move (pendingCompletions.front ().callback));
            pendingCompletions.pop_front ();
        }
    };
    auto callCompleted = [&completed] ()
    {
        for (auto& callback : completed)
            callback (true);
        completed.clear ();
    };

    while (! threadShouldExit () && port->exists ())
    {
        auto bytesToWrite { 0 };
        {
            const juce::ScopedLock sl { bufferCriticalSection };
            // copied out, so the lock is not held while writing, and write () never waits for the device
            bytesToWrite = static_cast<int> (buffer.peek (chunk.get (), static_cast<size_t> (getWriteChunkSize (port->configuredBps.load ()))));
            collectCompleted ();
        }
        callCompleted ();

        if (bytesToWrite == 0)
        {
            sentEvent.signal ();
            triggerWrite.wait (100);
            continue;
        }

        const auto bytesWritten { port->writeBytes (chunk.get (), bytesToWrite, kWriteTimeoutMs) };
        if (bytesWritten < 0)
        {
            port->reportConnectionLost ("SerialPortOutputStream::run", "write failed");
            break;
        }
        if (bytesWritten > 0)
        {
            const juce::ScopedLock sl { bufferCriticalSection };
            buffer.pop (static_cast<size_t> (bytesWritten));
            numBytesSent += static_cast<juce::uint64> (bytesWritten);
            collectCompleted ();
        }
        callCompleted ();
        sentEvent.signal ();
    }

    // nothing more can be queued after this, so these are the last callbacks
    std::deque<PendingCompletion> failed;
    {
        const juce::ScopedLock sl { bufferCriticalSection };
        acceptingWrites = false;
        collectCompleted ();
        failed.swap (pendingCompletions);
    }
    callCompleted ();
    for (auto& pendingCompletion : failed)
        pendingCompletion.callback (false);
    sentEvent.signal ();
}

void SerialPortOutputStream::cancel ()
{
    triggerWrite.signal ();
    if (port != nullptr && port->isOpen ())
        port->cancel ();
}

bool SerialPortOutputStream::queueWrite (const void* dataToWrite, size_t howManyBytes, WriteCompleteCallback onComplete)
{
    if (port == nullptr || ! port->exists ())
        return false;

    {
        const juce::ScopedLock sl { bufferCriticalSection };
        if (! acceptingWrites)
            return false;

        buffer.push (dataToWrite, howManyBytes);
        numBytesQueued += howManyBytes;
        if (onComplete != nullptr)
            pendingCompletions.push_back ({ numBytesQueued, std::move (onComplete) });
    }
    triggerWrite.signal ();
    return true;
}

bool SerialPortOutputStream::write (const void* dataToWrite, size_t howManyBytes)
{
    return queueWrite (dataToWrite, howManyBytes, nullptr);
}

bool SerialPortOutputStream::write (const void* dataToWrite, size_t howManyBytes, WriteCompleteCallback onComplete)
{
    return queueWrite (dataToWrite, howManyBytes, std::move (onComplete));
}

bool SerialPortOutputStream::waitUntilSent (int timeoutMs)
{
    const auto startTime { juce::Time::getMillisecondCounter () };
    for (;;)
    {
        {
            const juce::ScopedLock sl { bufferCriticalSection };
            if (buffer.size () == 0 && pendingCompletions.empty ())
                return true;
            if (! acceptingWrites)
                return false;
        }

        auto waitTime { 100 };
        if (timeoutMs >= 0)
        {
            const auto remaining { timeoutMs - static_cast<int> (juce::Time::getMillisecondCounter () - startTime) };
            if (remaining <= 0)
                return false;
            waitTime = juce::jmin (waitTime, remaining);
        }
        sentEvent.wait (waitTime);
    }
}

void SerialPortOutputStream::setQueueSize (size_t numBytes)
{
    const juce::ScopedLock sl { bufferCriticalSection };
    buffer.reserve (numBytes);
}

size_t SerialPortOutputStream::getNumBytesPending () const
{
    const juce::ScopedLock sl { bufferCriticalSection };
    return buffer.size ();
}
