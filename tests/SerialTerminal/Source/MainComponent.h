#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_serialport/juce_serialport.h>

// an interactive serial terminal, which also shows how to use the library without blocking the message thread:
// - the port list is scanned on a background thread
// - received data arrives in a callback on the reader thread, and is handed to the UI by a timer
// - writes never block, and their completions are counted on the writer thread
// - losing the connection is reported on a stream thread, and the port is closed on the message thread
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent ();
    ~MainComponent () override;

    void paint (juce::Graphics& g) override;
    void resized () override;

private:
    // scans for ports without blocking the message thread
    class PortScanner : private juce::Thread
    {
    public:
        explicit PortScanner (std::function<void (juce::StringPairArray)> onPortsChanged);
        ~PortScanner () override;

    private:
        std::function<void (juce::StringPairArray)> onPortsChanged;
        juce::StringPairArray lastPorts;

        void run () override;
    };

    juce::ComboBox portComboBox;
    juce::ComboBox baudComboBox;
    juce::TextButton openButton { "Open" };
    juce::Label statusLabel;
    juce::Label statsLabel;
    juce::TextEditor receivedText;
    juce::ToggleButton hexToggle { "Hex" };
    juce::TextButton clearButton { "Clear" };
    juce::TextEditor sendText;
    juce::ComboBox lineEndingComboBox;
    juce::TextButton sendButton { "Send" };
    juce::TextButton echoTestButton { "Echo test" };
    juce::ToggleButton floodToggle { "Flood" };

    juce::StringPairArray ports;
    std::unique_ptr<PortScanner> portScanner;

    std::unique_ptr<SerialPort> serialPort;
    std::unique_ptr<SerialPortInputStream> serialPortInput;
    std::unique_ptr<SerialPortOutputStream> serialPortOutput;

    // filled by the reader thread, emptied by the timer
    juce::CriticalSection receivedLock;
    juce::MemoryBlock receivedPending;

    // counted on the stream threads, shown by the timer
    std::atomic<juce::uint64> numWritesCompleted { 0 };
    std::atomic<juce::uint64> numWritesFailed { 0 };
    std::atomic<double> lastWriteLatencyMs { 0.0 };
    juce::uint64 lastBytesReceived { 0 };
    juce::uint64 lastBytesSent { 0 };
    double lastStatsTime { 0.0 };
    double receiveRate { 0.0 };
    double sendRate { 0.0 };

    // the echo test compares what comes back with what was sent
    juce::CriticalSection echoLock;
    juce::MemoryBlock echoExpected;
    juce::MemoryBlock echoReceived;
    juce::int64 echoStartTicks { 0 };
    bool echoTestRunning { false };

    void updatePortList (juce::StringPairArray newPorts);
    void openPort ();
    void closePort ();
    void handleConnectionLost ();
    void send ();
    void writeWithStats (const void* data, size_t numBytes);
    void startEchoTest ();
    void checkEchoTest ();
    void appendReceived (const juce::MemoryBlock& data);
    void updateStats ();
    void timerCallback () override;

    JUCE_DECLARE_WEAK_REFERENCEABLE (MainComponent)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
