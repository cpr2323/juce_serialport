// implementation for platforms without serial port support (iOS, etc), included by juce_serialport.cpp
//
// everything compiles, so apps can share code with the supported platforms, but no ports are found, and opening fails

struct SerialPort::Native
{
};

juce::StringPairArray SerialPort::getSerialPortPaths () { return {}; }

bool SerialPort::isOpen () const { return false; }

void SerialPort::close () {}

bool SerialPort::open (const juce::String& newPortPath)
{
    portPath = newPortPath;
    DebugLog ("SerialPort::open", "serial ports are not supported on this platform");
    return false;
}

void SerialPort::cancel () {}

bool SerialPort::setConfig (const SerialPortConfig&) { return false; }

bool SerialPort::getConfig (SerialPortConfig&) { return false; }

int SerialPort::readBytes (void*, int, int) { return -1; }

int SerialPort::writeBytes (const void*, int, int) { return -1; }
