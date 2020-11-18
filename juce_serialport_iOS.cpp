#include "../JuceLibraryCode/JuceHeader.h"

#if JUCE_IOS

using namespace juce;

#include "juce_serialport.h"

StringPairArray SerialPort::getSerialPortPaths () { return StringPairArray(); }

bool SerialPort::exists () { return false; }

bool SerialPort::open (const String & portPath) { return false; }

void SerialPort::close () {}

void SerialPort::cancel () {}

bool SerialPort::setConfig(const SerialPortConfig &) { return false; }

bool SerialPort::getConfig(SerialPortConfig &) { return false; }

//========== SerialPortInputStream ==========
void SerialPortInputStream::cancel () {}

void SerialPortInputStream::run() {}

int SerialPortInputStream::read(void*, int) { return -1; }

//========== SerialPortOutputStream ==========
void SerialPortOutputStream::cancel () {}

void SerialPortOutputStream::run() {}

bool SerialPortOutputStream::write(const void*, size_t) { return false; }

#endif // JUCE_IOS
