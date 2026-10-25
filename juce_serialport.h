/***********************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 juce_serialport
  vendor:             om ohm productions
  version:            3.1.0
  name:               JUCE Serial Port classes
  description:        Access hardware serial ports or USB virtual serial ports.
  website:            https://github.com/cpr2323/juce_serialport
  license:

  minimumCppStandard: 17

  dependencies:       juce_core juce_events
  OSXFrameworks:      IOKit CoreFoundation
  iOSFrameworks:
  linuxLibs:
  windowsLibs:        advapi32
  mingwLibs:          advapi32

 END_JUCE_MODULE_DECLARATION
***********************************************************************************/
/*  Serial port classes for accessing serial ports in an asynchronous, buffered manner

    contributed by graffiti
    Updated for current Juce API 8/1/12 Marc Lindahl

    Platforms:
        Windows - Win32 comm API, overlapped I/O
        macOS   - termios, ports enumerated with IOKit
        Linux   - termios, ports enumerated from /sys/class/tty
        Android - USB serial devices, through com/artiphon/juce_serial/UsbSerialHelper.java and the
                  usb-serial-for-android library (see README.md for how to add them to an app)
        iOS     - not supported, the functions compile but always fail

    Threading:
        SerialPortInputStream and SerialPortOutputStream each own a thread which does the actual I/O, so no call
        made by the application ever waits for the device:
        - write () copies the data into the output queue and returns. It only allocates if the queue has to grow
          (see setQueueSize ()), or if a completion callback is given
        - read () copies data which has already been received
        - received data can be delivered by a callback on the reader thread, as soon as it arrives
        - write completions are delivered by a callback on the writer thread, once the data has been handed to
          the operating system's driver

        Callbacks run on the stream threads, so they should be quick. They must not delete the stream, or the port,
        which would deadlock. Post that work to another thread instead.

        The port must outlive the streams attached to it, and must not be opened, closed or reconfigured while
        streams are attached to it. If the device goes away, the stream threads stop, exists () returns false and
        the port's connection lost callback is called. The owner should then delete the streams and close the port.

    Receiving, by callback (called on the reader thread, as soon as data arrives):

        SerialPortInputStream input { &serialPort, [] (const void* data, int numBytes) { ... } };

    Receiving, by polling (from any thread):

        char buffer [256];
        const auto numRead { input.read (buffer, sizeof (buffer)) };
        while (input.canReadLine ())
            processLine (input.readNextLine ());

    Receiving, with a notification on the message thread:

        input.addChangeListener (this);
        input.setNotify (SerialPortInputStream::NOTIFY_ON_CHAR, '\n');

    Sending (never blocks):

        output.write (data, numBytes);
        output.write (data, numBytes, [] (bool sent) { ... }); // called on the writer thread when it has been sent
        output.flush ();                                        // blocks until everything queued has been sent
*/

#pragma once
#define JUCE_SERIALPORT_H_INCLUDED

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>

using DebugFunction = std::function<void (juce::String, juce::String)>;

//////////////////////////////////////////////////////////////////
class JUCE_API SerialPortConfig
{
public:
    enum SerialPortStopBits { STOPBITS_1, STOPBITS_1ANDHALF, STOPBITS_2 };
    enum SerialPortFlowControl { FLOWCONTROL_NONE, FLOWCONTROL_HARDWARE, FLOWCONTROL_XONXOFF };
    enum SerialPortParity { SERIALPORT_PARITY_NONE, SERIALPORT_PARITY_ODD, SERIALPORT_PARITY_EVEN, SERIALPORT_PARITY_SPACE, SERIALPORT_PARITY_MARK };

    SerialPortConfig () = default;
    SerialPortConfig (uint32_t newBps, uint32_t newDatabits, SerialPortParity newParity, SerialPortStopBits newStopbits, SerialPortFlowControl newFlowcontrol)
        : bps { newBps }, databits { newDatabits }, parity { newParity }, stopbits { newStopbits }, flowcontrol { newFlowcontrol }
    {
    }

    uint32_t bps { 9600 };
    uint32_t databits { 8 };
    SerialPortParity parity { SERIALPORT_PARITY_NONE };
    SerialPortStopBits stopbits { STOPBITS_1 };
    SerialPortFlowControl flowcontrol { FLOWCONTROL_NONE };
};

//////////////////////////////////////////////////////////////////
// a growable ring buffer of bytes. not thread safe, the streams lock around it
class JUCE_API SerialPortByteQueue
{
public:
    size_t size () const noexcept { return numBytes; }
    size_t capacity () const noexcept { return storageSize; }
    void reserve (size_t minimumCapacity);
    void push (const void* source, size_t numBytesToPush);
    // copies up to maxBytes from the front, without removing them
    size_t peek (void* dest, size_t maxBytes) const noexcept;
    void pop (size_t numBytesToPop) noexcept;
    size_t read (void* dest, size_t maxBytes) noexcept { const auto numRead { peek (dest, maxBytes) }; pop (numRead); return numRead; }
    bool contains (char c) const noexcept;
    void clear () noexcept { readIndex = 0; numBytes = 0; }

private:
    juce::HeapBlock<char> storage;
    size_t storageSize { 0 };
    size_t readIndex { 0 };
    size_t numBytes { 0 };
};

//////////////////////////////////////////////////////////////////
class JUCE_API SerialPort
{
public:
    SerialPort (DebugFunction theDebugLog);
    SerialPort (const juce::String& portPath, DebugFunction theDebugLog);
    SerialPort (const juce::String& portPath, const SerialPortConfig& config, DebugFunction theDebugLog);
    virtual ~SerialPort ();

    // opening a port that is already open closes it first
    bool open (const juce::String& portPath);
    void close ();
    bool setConfig (const SerialPortConfig& config);
    bool getConfig (SerialPortConfig& config);
    juce::String getPortPath () const { return portPath; }
    // returns friendly name/port path pairs. safe to call from any thread
    static juce::StringPairArray getSerialPortPaths ();
    // true while the port is open and the device has not gone away. safe to call from any thread
    bool exists () const;
    // aborts any I/O that the stream threads are blocked on
    virtual void cancel ();
    void DebugLog (juce::String prefix, juce::String msg) const { if (DebugLogInternal != nullptr) DebugLogInternal (prefix, msg); }
    // the debug function is called from the stream threads, so it must be set before any streams are created
    void setDebugLogFunction (DebugFunction theDebugLog) { DebugLogInternal = theDebugLog; }

    // called once, on the stream thread which notices that the device has gone away. it must not delete the streams
    // or close the port, as that would deadlock, so post that to another thread. it can be set from any thread, and
    // once the call returns, the previous callback is not running, and will not be called
    void setConnectionLostCallback (std::function<void ()> newCallback);

private:
    friend class SerialPortInputStream;
    friend class SerialPortOutputStream;

    // the platform specific state, defined in native/juce_serialport_<platform>.cpp
    struct Native;
    std::unique_ptr<Native> native;
    juce::String portPath;
    DebugFunction DebugLogInternal;
    // set by the stream threads when the device stops responding, cleared by open ()
    std::atomic<bool> connectionLost { false };
    std::atomic<int> numAttachedStreams { 0 };
    // set by setConfig, used to size the writes
    std::atomic<uint32_t> configuredBps { 0 };
    juce::CriticalSection connectionLostCallbackLock;
    std::function<void ()> connectionLostCallback;

    bool isOpen () const;
    // these are only called by the stream threads, one thread for each direction.
    // they return the number of bytes transferred, 0 if the timeout expired, or -1 if the device has gone away
    int readBytes (void* destBuffer, int maxBytesToRead, int timeoutMs);
    int writeBytes (const void* sourceBuffer, int numBytesToWrite, int timeoutMs);
    void reportConnectionLost (const juce::String& prefix, const juce::String& msg);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SerialPort)
};

//////////////////////////////////////////////////////////////////
class JUCE_API SerialPortInputStream : public juce::InputStream, public juce::ChangeBroadcaster, private juce::Thread
{
public:
    // called on the reader thread, with each block of data as it arrives
    using DataReceivedCallback = std::function<void (const void* data, int numBytes)>;

    explicit SerialPortInputStream (SerialPort* port);
    // the callback is set before the reader thread starts, so it receives all of the data.
    // if alsoBufferData is false, the data is only passed to the callback, and read () will not return it
    SerialPortInputStream (SerialPort* port, DataReceivedCallback dataReceivedCallback, bool alsoBufferData = false);
    ~SerialPortInputStream () override;

    // can be called from any thread. once it returns, the previous callback is not running, and will not be called
    void setDataReceivedCallback (DataReceivedCallback newCallback, bool alsoBufferData = false);

    enum notifyflag { NOTIFY_OFF = 0, NOTIFY_ON_CHAR, NOTIFY_ALWAYS };
    // sends a change message, which listeners receive on the message thread
    void setNotify (notifyflag newNotify = NOTIFY_ON_CHAR, char c = 0);

    bool canReadString ();
    bool canReadLine ();

    int read (void* destBuffer, int maxBytesToRead) override;
    // InputStream::readNextLine isn't compatible with serial ports, as it uses setPosition
    juce::String readNextLine () override;
    juce::int64 getTotalLength () override;
    bool isExhausted () override;
    juce::int64 getPosition () override { return 0; }
    bool setPosition (juce::int64 /*newPosition*/) override { return false; }

    // the total number of bytes received, including any only passed to the callback
    juce::uint64 getNumBytesReceived () const noexcept { return numBytesReceived.load (); }

    virtual void cancel ();
    SerialPort* getPort () { return port; }
    void setReaderPriority (juce::Thread::Priority newPriority) { setPriority (newPriority); }

private:
    SerialPort* const port;
    SerialPortByteQueue buffer;
    juce::CriticalSection bufferCriticalSection;
    juce::CriticalSection callbackLock;
    DataReceivedCallback dataReceivedCallback;
    bool bufferData { true };
    std::atomic<notifyflag> notify { NOTIFY_OFF };
    std::atomic<char> notifyChar { 0 };
    std::atomic<juce::uint64> numBytesReceived { 0 };

    void run () override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SerialPortInputStream)
};

//////////////////////////////////////////////////////////////////
class JUCE_API SerialPortOutputStream : public juce::OutputStream, private juce::Thread
{
public:
    // called on the writer thread. sent is true once all of the data has been handed to the operating system's
    // driver (it may still be in the driver's or the device's buffers), or false if the port went away, or the
    // stream was deleted, before then
    using WriteCompleteCallback = std::function<void (bool sent)>;

    explicit SerialPortOutputStream (SerialPort* port);
    ~SerialPortOutputStream () override;

    // queues the data to be written by the writer thread, so it never waits for the device. returns false, without
    // queueing anything, if the port has gone away
    bool write (const void* dataToWrite, size_t howManyBytes) override;
    // as above, and the callback is called when the data has been sent. if this returns false, the callback is never called
    bool write (const void* dataToWrite, size_t howManyBytes, WriteCompleteCallback onComplete);
    // blocks until everything queued has been sent, the port has gone away, or the timeout expires (-1 waits forever).
    // returns true if everything was sent
    bool waitUntilSent (int timeoutMs);
    // blocks until everything queued has been sent, or the port has gone away
    void flush () override { waitUntilSent (-1); }
    bool setPosition (juce::int64 /*newPosition*/) override { return false; }
    juce::int64 getPosition () override { return -1; }

    // pre-allocates the output queue, so write () does not allocate unless more than this is pending
    void setQueueSize (size_t numBytes);
    // the number of bytes queued, which have not been sent yet
    size_t getNumBytesPending () const;
    // the total number of bytes handed to the driver
    juce::uint64 getNumBytesSent () const noexcept { return numBytesSent.load (); }

    virtual void cancel ();
    SerialPort* getPort () { return port; }
    void setWriterPriority (juce::Thread::Priority newPriority) { setPriority (newPriority); }

private:
    struct PendingCompletion
    {
        juce::uint64 endPosition; // the value of numBytesSent when the write has been sent
        WriteCompleteCallback callback;
    };

    SerialPort* const port;
    SerialPortByteQueue buffer;
    std::deque<PendingCompletion> pendingCompletions;
    juce::uint64 numBytesQueued { 0 };
    std::atomic<juce::uint64> numBytesSent { 0 };
    // false once the writer thread has stopped, after which nothing more is queued
    bool acceptingWrites { true };
    juce::CriticalSection bufferCriticalSection;
    juce::WaitableEvent triggerWrite;
    juce::WaitableEvent sentEvent;

    bool queueWrite (const void* dataToWrite, size_t howManyBytes, WriteCompleteCallback onComplete);
    void run () override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SerialPortOutputStream)
};
