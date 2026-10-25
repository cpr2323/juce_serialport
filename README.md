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
`SerialPortInputStream` and `SerialPortOutputStream` each run a thread that does the actual I/O, so `read ()` and
`write ()` never block on the device, and can be called from any thread. The rules are:
- The `SerialPort` must outlive the streams attached to it.
- Don't `open ()`, `close ()` or `setConfig ()` a port while streams are attached to it. Delete the streams first.
- If the device goes away, the stream threads stop and `exists ()` returns false. Delete the streams, then close the port.
- `exists ()` and `getSerialPortPaths ()` can be called from any thread.

## Changes in version 3
- Restructured as a standard JUCE module: one `juce_serialport.cpp`, with the platform code in `native/`. The old
  `#include "../JuceLibraryCode/JuceHeader.h"` is gone, so the module builds under CMake, and with the Projucer.
- The `USING_JUCE_PRIOR_TO_7_0_5` macro has been removed, `setReaderPriority ()` and `setWriterPriority ()` take a
  `juce::Thread::Priority`.
- `run ()` is no longer public on the streams.
- Windows: hardware flow control is now RTS/CTS, as it is on the other platforms. It used to also enable DTR/DSR handshaking.
- The stream threads no longer close the port when the device goes away, the owner does.
- Added Linux support.
