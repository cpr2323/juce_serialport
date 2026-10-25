# juce_serialport
Access hardware serial ports or USB virtual serial ports

I did not write these classes, but I have been trying to maintain them, since I continue to work on things that need access to serial ports. The only history I have for them is the following:

contributed by graffiti

Updated for current Juce API 8/1/12 Marc Lindahl

## Requirements
JUCE 7.0.5 or later (developed against JUCE 9), C++17.

## Platforms
| Platform | Implementation | Port enumeration |
|----------|----------------|------------------|
| Windows  | Win32 comm API, overlapped I/O | `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM` |
| macOS    | termios | IOKit |
| Linux    | termios (termios2 for non-standard baud rates) | `/sys/class/tty` |
| Android  | USB serial devices, via usb-serial-for-android | USB devices |
| iOS      | not supported: compiles, but no ports are found and `open ()` fails | - |

## Using the module
### CMake
```cmake
juce_add_module (path/to/juce_serialport)
target_link_libraries (MyTarget PRIVATE juce_serialport)
```

### Projucer
Add the module folder with the module manager.

### Android
The Android implementation needs two sets of Java sources in the app:
- `com/artiphon/juce_serial/UsbSerialHelper.java`, from this module
- the usb-serial-for-android library (`submodules/usb-serial-for-android`, run `git submodule update --init` in this repository)

With the Projucer, add this module's folder to the Android exporter's "Java Source code folders" and add the
usb-serial-for-android library as a Gradle dependency, or add its `usbSerialForAndroid/src/main/java` folder to the
Java source folders. Port paths are `<USB device name>:<port number>`. Paths from version 2 of the module
(`serialport <n>`) are still accepted, and refer to port n of the first device.

## Threading
`SerialPortInputStream` and `SerialPortOutputStream` each run a thread that does the actual I/O (overlapped I/O on
Windows, non-blocking descriptors on macOS and Linux), so nothing the application calls waits for the device. A UI,
or a timing critical thread such as a sequencer, can send and receive without blocking. The rules are:
- The `SerialPort` must outlive the streams attached to it.
- Don't `open ()`, `close ()` or `setConfig ()` a port while streams are attached to it. Delete the streams first.
- If the device goes away, the stream threads stop, `exists ()` returns false, and the connection lost callback is
  called. Delete the streams, then close the port.
- Callbacks run on the stream threads. Keep them short, and never delete a stream or the port from one (that
  deadlocks), post that to another thread instead.
- `exists ()` and `getSerialPortPaths ()` can be called from any thread.

## Receiving
```cpp
// by callback, on the reader thread, as soon as data arrives. passing it to the constructor means no data is missed
SerialPortInputStream input { &port, [] (const void* data, int numBytes) { /* ... */ } };

// by polling, from any thread
char buffer [256];
const auto numRead { input.read (buffer, sizeof (buffer)) };
while (input.canReadLine ())
    handleLine (input.readNextLine ());

// with a notification on the message thread (a ChangeBroadcaster)
input.addChangeListener (this);
input.setNotify (SerialPortInputStream::NOTIFY_ON_CHAR, '\n');
```
With a callback the data is not buffered, unless `alsoBufferData` is true. `setDataReceivedCallback ()` changes the
callback at any time, and once it returns the old callback is not running and won't be called again.

## Sending
```cpp
output.write (data, numBytes); // queues a copy, and returns. never waits for the device
output.write (data, numBytes, [] (bool sent)
{
    // on the writer thread. sent is true once all of it has been handed to the OS driver, false if the port went
    // away or the stream was deleted first. called exactly once for each write () that returned true
});
output.waitUntilSent (100); // blocks until everything queued has been sent, or the timeout
output.flush ();            // the same, without a timeout
output.setQueueSize (64 * 1024); // write () then only allocates if more than this is pending
```
Completion means the data has been handed to the driver: it may still be in the driver's or the device's buffers.
`getNumBytesPending ()` and `getNumBytesSent ()` show progress.

## Device removal
```cpp
port.setConnectionLostCallback ([this]
{
    // on a stream thread, so close the port somewhere else
    juce::MessageManager::callAsync ([this] { closePort (); });
});
```

## Tests
See [tests/README.md](tests/README.md): an automated test runner, and a GUI terminal app, for every platform.

## Changes in version 3.1
- Receive callback on the reader thread, write completion callbacks on the writer thread, and a connection lost callback.
- `waitUntilSent ()`, and `flush ()` now waits until everything has been sent.
- `setQueueSize ()`, `getNumBytesPending ()`, `getNumBytesSent ()`, `getNumBytesReceived ()`.
- The stream buffers are ring buffers, so reading and writing no longer moves the remaining data.
- Writes are sized to the baud rate, so a write is only cancelled when the device has stalled.
- macOS/Linux: closing a port clears exclusive mode, which could stop it being reopened.
- Windows: opening a port discards anything received before it was opened, as macOS and Linux do.
- Test applications in `tests/`.

## Changes in version 3
- Restructured as a standard JUCE module: one `juce_serialport.cpp`, with the platform code in `native/`. The old
  `#include "../JuceLibraryCode/JuceHeader.h"` is gone, so the module builds under CMake, and with the Projucer.
- The `USING_JUCE_PRIOR_TO_7_0_5` macro has been removed, `setReaderPriority ()` and `setWriterPriority ()` take a
  `juce::Thread::Priority`.
- `run ()` is no longer public on the streams.
- Windows: hardware flow control is now RTS/CTS, as it is on the other platforms. It used to also enable DTR/DSR handshaking.
- The stream threads no longer close the port when the device goes away, the owner does.
- Added Linux support.
