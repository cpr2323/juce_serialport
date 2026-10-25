/***********************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 juce_serialport
  vendor:             om ohm productions
  version:            3.0.0
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
        SerialPortInputStream and SerialPortOutputStream each own a thread which does the actual I/O, so reading from
        and writing to the streams never blocks on the device. write () and read () may be called from any thread.
        The port must outlive the streams attached to it, and must not be opened, closed or reconfigured while
        streams are attached to it. If the device goes away, the stream threads stop and exists () returns false;
        the owner should then delete the streams and close the port.

    A typical serial port scenario may be:

    // get a list of the serial ports installed on the system, as a StringPairArray of friendly names and port paths
    const auto portList { SerialPort::getSerialPortPaths () };
    if (portList.size () > 0)
    {
        // open the first port on the system
        SerialPort serialPort { portList.getAllValues ()[0],
                                SerialPortConfig (9600, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1, SerialPortConfig::FLOWCONTROL_NONE),
                                nullptr };
        if (serialPort.exists ())
        {
            // create streams for reading/writing
            SerialPortOutputStream outputStream { &serialPort };
            SerialPortInputStream inputStream { &serialPort };

            outputStream.write ("hello world via serial", 22); // write some bytes

            // read chars one at a time:
            char c;
            while (! inputStream.isExhausted ())
                inputStream.read (&c, 1);

            // or, read line by line:
            juce::String s;
            while (inputStream.canReadLine ())
                s = inputStream.readNextLine ();

            // or ask to be notified when a new line is available:
            inputStream.addChangeListener (this); // we must be a ChangeListener to receive notifications
            inputStream.setNotify (SerialPortInputStream::NOTIFY_ON_CHAR, '\n');

            // or ask to be notified whenever any data is received
            // NOTE - use with care at high baud rates!!!!
            inputStream.setNotify (SerialPortInputStream::NOTIFY_ALWAYS);

            // please see class definitions for other features/functions etc
        }
    }
*/

#pragma once
#define JUCE_SERIALPORT_H_INCLUDED

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
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
    explicit SerialPortInputStream (SerialPort* port);
    ~SerialPortInputStream () override;

    enum notifyflag { NOTIFY_OFF = 0, NOTIFY_ON_CHAR, NOTIFY_ALWAYS };
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

    virtual void cancel ();
    SerialPort* getPort () { return port; }
    void setReaderPriority (juce::Thread::Priority newPriority) { setPriority (newPriority); }

private:
    SerialPort* const port;
    juce::MemoryBlock buffer;
    juce::CriticalSection bufferCriticalSection;
    std::atomic<notifyflag> notify { NOTIFY_OFF };
    std::atomic<char> notifyChar { 0 };

    void run () override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SerialPortInputStream)
};

//////////////////////////////////////////////////////////////////
class JUCE_API SerialPortOutputStream : public juce::OutputStream, private juce::Thread
{
public:
    explicit SerialPortOutputStream (SerialPort* port);
    ~SerialPortOutputStream () override;

    // queues the data to be written by the writer thread, so it does not block on the device
    bool write (const void* dataToWrite, size_t howManyBytes) override;
    void flush () override {}
    bool setPosition (juce::int64 /*newPosition*/) override { return false; }
    juce::int64 getPosition () override { return -1; }

    virtual void cancel ();
    SerialPort* getPort () { return port; }
    void setWriterPriority (juce::Thread::Priority newPriority) { setPriority (newPriority); }

private:
    SerialPort* const port;
    juce::MemoryBlock buffer;
    juce::CriticalSection bufferCriticalSection;
    juce::WaitableEvent triggerWrite;

    void run () override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SerialPortOutputStream)
};
