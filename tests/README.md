# juce_serialport test applications

| App | What it is | Platforms |
|-----|------------|-----------|
| `SerialPortTests` | console app, automated functional tests | Windows, macOS, Linux |
| `SerialTerminal` | GUI serial terminal, with an echo test and a throughput (flood) mode | Windows, macOS, Linux, iOS (CMake), Android (Projucer) |
| `arduino_echo` | Arduino sketch which echoes everything back, for hardware tests | any Arduino |
| `daisy_echo` | Daisy Seed firmware which echoes everything back, and reports the line settings and control lines it receives | Daisy Seed |

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
SerialPortTests --daisy PORT            a Daisy Seed running daisy_echo, as --echo plus the line settings tests
SerialPortTests --write-only PORT       a port with nothing connected, only sending is tested
  --baud N                              default 115200, must match arduino_echo's rate in --echo mode
  --unplug                              with --pair or --echo, finish by asking you to unplug the device
  --list                                list the ports
```

`--daisy` runs everything `--echo` does.

| Test | pty | pair | echo | write only |
|------|-----|------|------|------------|
| configuration | yes (no parity, ptys ignore it) | yes | yes | yes |
| the rate, data bits, parity, stop bits, DTR and RTS the device receives, flow control, DTR dropping on close | - | - | `--daisy` only | - |
| write completion callbacks, order, thread | yes | yes | yes | yes |
| flush () / waitUntilSent () wait for the completion callbacks, and work from inside one | yes | yes | yes | yes |
| deleting a stream with writes pending | yes | yes | yes | yes |
| write () never blocks, 1 kHz "sequencer" thread | yes | yes | yes | yes |
| receive callback, latency, throughput | yes | yes | round trip | - |
| polling reads, lines | yes | yes | yes | - |
| polling reads of bulk data, in odd sized reads | yes | yes | yes | - |
| canReadString () | yes | yes | yes | - |
| a callback which also buffers (alsoBufferData) | yes | yes | yes | - |
| change notifications on the message thread (NOTIFY_OFF, NOTIFY_ON_CHAR, NOTIFY_ALWAYS) | yes | yes | yes | - |
| switching between callback only, polling, and both | yes | yes | yes | - |
| both directions at once | yes | yes | - | - |
| replacing the receive callback while data arrives | yes | yes | - | - |
| connection lost | yes (hangs up the pty) | with --unplug | with --unplug | - |

### Daisy Seed
A real UART never tells you what it was configured with, so the other modes can only check that the driver accepted
the settings. `daisy_echo` is a USB CDC device, and the host's driver sends it the line coding (rate, data bits,
parity, stop bits) and the DTR and RTS lines as USB requests, which it records and reports when asked. The test sends
the request in band (`F5 5A A5 0F 'S'`), so it is echoed like any other data, followed by the reply.

Build it with the [Daisy toolchain](https://github.com/electro-smith/DaisyWiki/wiki/1.-Setting-Up-Your-Development-Environment)
and a built [libDaisy](https://github.com/electro-smith/libDaisy) (`make` in libDaisy, after `git submodule update --init`):
```
cd daisy_echo
make LIBDAISY_DIR=/path/to/libDaisy APP_TYPE=BOOT_SRAM
make LIBDAISY_DIR=/path/to/libDaisy APP_TYPE=BOOT_SRAM program-dfu
```
`APP_TYPE=BOOT_SRAM` is for a Seed with the Daisy bootloader (`dfu-util -l` shows `@Flash /0x90000000...`). For the
STM32 ROM bootloader (hold BOOT, press RESET, `dfu-util -l` shows `@Internal Flash`), leave `APP_TYPE` out. Then run
`SerialPortTests --daisy COMx` (`/dev/ttyACM0` on Linux, `/dev/cu.usbmodem...` on macOS). The Seed's LED is on while
the port is open (DTR), and flickers with data. Add `--unplug` to finish by unplugging it.

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
3. Attach it to WSL (no administrator needed after bind). While attached, Windows can't use it. WSL must be running,
   so keep a WSL terminal open, otherwise attach fails with "There is no WSL 2 distribution running":
   ```
   usbipd attach --wsl --busid 1-4
   ```
4. In WSL it appears as `/dev/ttyACM0` (Arduinos with native USB, Uno, etc) or `/dev/ttyUSB0` (FTDI, CH340, CP210x
   adapters). Give yourself access, then restart WSL with `wsl --shutdown`:
   ```
   sudo usermod -aG dialout $USER
   ```
5. `usbipd detach --busid 1-4` gives it back to Windows. Detaching while `SerialPortTests --echo /dev/ttyACM0 --unplug`
   is waiting tests unplugging, without touching the cable. The device must be attached again after unplugging, or
   after WSL restarts.

A motherboard COM port can't be passed to WSL 2.
