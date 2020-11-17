//mac_SerialPort.cpp
//Serial Port classes in a Juce stylee, by graffiti
//see SerialPort.h for details
//
// Updated for current Juce API 8/1/13 Marc Lindahl
//

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
