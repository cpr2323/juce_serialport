# juce_serialport test applications

| App | What it is | Platforms |
|-----|------------|-----------|
| `SerialPortTests` | console app, automated functional tests | Windows, macOS, Linux |
| `SerialTerminal` | GUI serial terminal, with an echo test and a throughput (flood) mode | Windows, macOS, Linux, iOS (CMake), Android (Projucer) |
| `arduino_echo` | Arduino sketch which echoes everything back, for hardware tests | any Arduino |

## Building (Windows, macOS, Linux)
```
cmake -S tests -B tests/build
cmake --build tests/build --config Debug
```
JUCE is found at `../../JUCE` relative to this folder (the layout in a project which has JUCE and this module side by
side, eg. light_plug's `submodules/`), or set `-DJUCE_SOURCE_DIR=/path/to/JUCE`. Otherwise JUCE 9.0.2 is downloaded.

Linux needs the JUCE GUI dependencies for `SerialTerminal`; the console tests only need a compiler and CMake:
```
sudo apt install cmake g++ libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev \
    libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libxi-dev
```

### iOS
```
cmake -S tests -B tests/build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
```
then build `SerialTerminal` in Xcode with your signing team. iOS has no serial port support, so this only checks
that the module builds and the app runs with no ports.

### Android
Open `SerialTerminal/SerialTerminal.jucer` in the Projucer (or `Projucer --resave SerialTerminal.jucer`), and open
`SerialTerminal/Builds/Android` in Android Studio. Run `git submodule update --init` in the module's repository first,
for usb-serial-for-android. The project expects JUCE at `../../../JUCE` (light_plug's layout); change the module paths
in the Projucer otherwise. Connect a USB serial device to the phone with an OTG adapter; the app asks for permission
the first time the port is opened, then Open again once permission is granted.

## SerialPortTests modes
```
SerialPortTests                         macOS/Linux: everything, over a pseudo terminal pair. Windows: just lists the ports
SerialPortTests --pair PORT1 PORT2      two ports connected to each other
SerialPortTests --echo PORT             a device running arduino_echo
SerialPortTests --write-only PORT       a port with nothing connected, only sending is tested
  --baud N                              default 115200, must match arduino_echo's rate in --echo mode
  --unplug                              with --pair or --echo, finish by asking you to unplug the device
  --list                                list the ports
```

| Test | pty | pair | echo | write only |
|------|-----|------|------|------------|
| configuration | yes (no parity, ptys ignore it) | yes | yes | yes |
| write completion callbacks, order, thread | yes | yes | yes | yes |
| deleting a stream with writes pending | yes | yes | yes | yes |
| write () never blocks, 1 kHz "sequencer" thread | yes | yes | yes | yes |
| receive callback, latency, throughput | yes | yes | round trip | - |
| polling reads, lines | yes | yes | yes | - |
| both directions at once | yes | yes | - | - |
| replacing the receive callback while data arrives | yes | yes | - | - |
| connection lost | yes (hangs up the pty) | with --unplug | with --unplug | - |

### What to use on each platform
- **macOS, Linux**: run it with no arguments, no hardware needed. Then `--echo` with an Arduino, plus `--unplug`, to
  test a real USB device.
- **Windows** has no built in loopback. Any of:
  - [com0com](https://sourceforge.net/projects/com0com/), a virtual null modem, creates a connected pair of ports,
    eg. `--pair COM10 COM11`. Use the signed 2.2.2.0 release on 64 bit Windows.
  - an Arduino running `arduino_echo`: `--echo COM3 --unplug`
  - a USB serial adapter with TX and RX connected together also echoes: `--echo COMx`
  - a port with nothing attached: `--write-only COM1`

## Serial ports in WSL
WSL 2 does not pass Windows COM ports through, its `/dev/ttyS*` devices are not connected to anything. A USB serial
device (an Arduino, a USB serial adapter) can be passed through with [usbipd-win](https://github.com/dorssel/usbipd-win):

1. Install it on Windows: `winget install --exact dorssel.usbipd-win`
2. In an administrator PowerShell, find the device's bus id, and share it (once per device):
   ```
   usbipd list
   usbipd bind --busid 1-4
   ```
3. Attach it to WSL (no administrator needed after bind). While attached, Windows can't use it:
   ```
   usbipd attach --wsl --busid 1-4
   ```
4. In WSL it appears as `/dev/ttyACM0` (Arduinos with native USB, Uno, etc) or `/dev/ttyUSB0` (FTDI, CH340, CP210x
   adapters). Give yourself access, then restart WSL with `wsl --shutdown`:
   ```
   sudo usermod -aG dialout $USER
   ```
5. `usbipd detach --busid 1-4` gives it back to Windows. Detaching while `SerialPortTests --echo /dev/ttyACM0 --unplug`
   is waiting is a good way to test unplugging.

A motherboard COM port can't be passed to WSL 2.
