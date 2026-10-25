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
    // how long the stream threads block in the platform I/O before checking whether they should exit
    constexpr auto kReadTimeoutMs { 100 };
    constexpr auto kWriteTimeoutMs { 250 };
    constexpr auto kReadChunkSize { 1024 };
    constexpr auto kWriteChunkSize { 256 };
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

void SerialPort::reportConnectionLost (const juce::String& prefix, const juce::String& msg)
{
    // only the first thread to notice reports it
    if (! connectionLost.exchange (true))
        DebugLog (prefix, "connection lost: " + msg);
}

//////////////////////////////////////////////////////////////////
// SerialPortInputStream
//////////////////////////////////////////////////////////////////
SerialPortInputStream::SerialPortInputStream (SerialPort* newPort)
    : Thread ("SerialInThread"), port { newPort }
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

void SerialPortInputStream::setNotify (notifyflag newNotify, char c)
{
    notifyChar = c;
    notify = newNotify;
}

bool SerialPortInputStream::canReadString ()
{
    const juce::ScopedLock sl { bufferCriticalSection };
    return std::memchr (buffer.getData (), 0, buffer.getSize ()) != nullptr;
}

bool SerialPortInputStream::canReadLine ()
{
    const juce::ScopedLock sl { bufferCriticalSection };
    return std::memchr (buffer.getData (), '\n', buffer.getSize ()) != nullptr;
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

        {
            const juce::ScopedLock sl { bufferCriticalSection };
            buffer.append (chunk.get (), static_cast<size_t> (bytesRead));
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
    if (buffer.getSize () == 0 && ! port->exists ())
        return -1;

    const auto bytesToRead { juce::jlimit (0, static_cast<int> (buffer.getSize ()), maxBytesToRead) };
    if (bytesToRead > 0)
    {
        std::memcpy (destBuffer, buffer.getData (), static_cast<size_t> (bytesToRead));
        buffer.removeSection (0, static_cast<size_t> (bytesToRead));
    }
    return bytesToRead;
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
    return static_cast<juce::int64> (buffer.getSize ());
}

bool SerialPortInputStream::isExhausted ()
{
    const juce::ScopedLock sl { bufferCriticalSection };
    return buffer.getSize () == 0;
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
    // the platform writes time out, so the thread will exit, and it must have before the port can be released
    waitForThreadToExit (-1);

    if (port != nullptr)
        --port->numAttachedStreams;
}

void SerialPortOutputStream::run ()
{
    if (port == nullptr)
        return;

    juce::HeapBlock<char> chunk { kWriteChunkSize };

    while (! threadShouldExit () && port->exists ())
    {
        auto bytesToWrite { 0 };
        {
            const juce::ScopedLock sl { bufferCriticalSection };
            bytesToWrite = juce::jmin (kWriteChunkSize, static_cast<int> (buffer.getSize ()));
            if (bytesToWrite > 0)
                std::memcpy (chunk.get (), buffer.getData (), static_cast<size_t> (bytesToWrite));
        }

        if (bytesToWrite == 0)
        {
            triggerWrite.wait (100);
            continue;
        }

        // the lock is not held while writing, so write () never waits on the device
        const auto bytesWritten { port->writeBytes (chunk.get (), bytesToWrite, kWriteTimeoutMs) };
        if (bytesWritten < 0)
        {
            port->reportConnectionLost ("SerialPortOutputStream::run", "write failed");
            break;
        }
        if (bytesWritten > 0)
        {
            const juce::ScopedLock sl { bufferCriticalSection };
            buffer.removeSection (0, static_cast<size_t> (bytesWritten));
        }
    }
}

void SerialPortOutputStream::cancel ()
{
    triggerWrite.signal ();
    if (port != nullptr && port->isOpen ())
        port->cancel ();
}

bool SerialPortOutputStream::write (const void* dataToWrite, size_t howManyBytes)
{
    if (port == nullptr || ! port->exists ())
        return false;

    {
        const juce::ScopedLock sl { bufferCriticalSection };
        buffer.append (dataToWrite, howManyBytes);
    }
    triggerWrite.signal ();
    return true;
}
