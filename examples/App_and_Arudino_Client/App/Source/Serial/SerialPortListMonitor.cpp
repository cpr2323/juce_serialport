#include "SerialPortListMonitor.h"

SerialPortListMonitor::SerialPortListMonitor(void)
    : Thread("Serial Port Monitor")
{
    startThread();
}

SerialPortListMonitor::~SerialPortListMonitor(void)
{
    stopThread(5000);
}

juce::StringPairArray SerialPortListMonitor::getSerialPortList(void)
{
    juce::ScopedLock sl (dataLock);
    listChanged = false;
    return serialPortNames;
}

void SerialPortListMonitor::setSleepTime (int newSleepTime)
{
    sleepTime = newSleepTime;
}

bool SerialPortListMonitor::hasListChanged (void)
{
    return listChanged;
}

// thread for keeping an up to date list of serial port names
void SerialPortListMonitor::run()
{
    while (!threadShouldExit())
    {
        // wake up every mSleepTime to check the serial port list
        sleep (sleepTime);

        const auto serialPortList { SerialPort::getSerialPortPaths () };
        juce::ScopedLock sl (dataLock);
        if ((serialPortList.size() != serialPortNames.size()) || serialPortList != serialPortNames)
        {
            juce::Logger::outputDebugString ("Serial Port List");
            for (auto serialPortNameIndex { 0 }; serialPortNameIndex < serialPortList.size (); ++serialPortNameIndex)
                juce::Logger::outputDebugString (serialPortList.getAllValues () [serialPortNameIndex]);
            serialPortNames = serialPortList;
            listChanged = true;
        }
    }
}
