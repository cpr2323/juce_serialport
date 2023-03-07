#pragma once

#include <JuceHeader.h>

const auto kSerialPortListMonitorSleepTime { 1000 };

class SerialPortListMonitor : public juce::Thread
{
public:
    SerialPortListMonitor (void);
    ~SerialPortListMonitor (void);
    bool hasListChanged (void);
    juce::StringPairArray getSerialPortList (void);
    void setSleepTime (int sleepTime);
    void run () override;

private:
    juce::CriticalSection dataLock;
    juce::StringPairArray serialPortNames;
    int sleepTime { kSerialPortListMonitorSleepTime };
    bool listChanged { false };
};
