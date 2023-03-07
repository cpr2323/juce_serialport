#pragma once

#include "SerialPortListMonitor.h"

// NOTE: This class could be named for the name of the thing you are connecting to: LEDContllerDevice, DrumMachineDevice, SolarMonitorDevice, etc
class SerialDevice : private juce::Thread
{
public:
    SerialDevice ();
    ~SerialDevice ();
    void open (void);
    void close (void);
    void init (juce::String newSerialPortName);

    // NOTE: examples of API's which take different parameters
    void setLightColor (uint16_t color);
    void setTempo (float tempo);
    void setChargingAlarmLevel (uint8_t alarmType, uint8_t chargeLevel);

private:
    enum class ThreadTask
    {
        idle,
        delayBeforeOpening,
        openSerialPort,
        closeSerialPort,
        processSerialPort,
    };

    juce::String serialPortName;
    std::unique_ptr<SerialPort> serialPort;
    std::unique_ptr<SerialPortInputStream> serialPortInput;
    std::unique_ptr<SerialPortOutputStream> serialPortOutput;
    ThreadTask threadTask { ThreadTask::idle };
    uint64_t delayStartTime { 0 };

    float tempo { 60.0f };
    uint16_t lightColor { 0 };
    uint8_t alarmLevels [2] { 0, 0 };

    // NOTE: included but not used in this example. shows how to monitor for serial port device list changes
    SerialPortListMonitor serialPortListMonitor;

    bool openSerialPort (void);
    void closeSerialPort (void);

    void handleTempoCommand (uint8_t* data, int dataSize);
    void handleLightColorCommand (uint8_t* data, int dataSize);
    void handleChargingAlarmLevelCommand (uint8_t* data, int dataSize);
    void handleCommand (uint8_t command, uint8_t* data, int dataSize);

    void run () override;
};