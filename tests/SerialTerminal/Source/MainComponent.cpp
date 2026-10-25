#include "MainComponent.h"

namespace
{
    constexpr auto kMaxReceivedTextLength { 64 * 1024 };
    constexpr auto kEchoTestSize { 16 * 1024 };
    constexpr auto kFloodChunkSize { 1024 };
    const juce::StringArray kBaudRates { "9600", "19200", "28800", "38400", "57600", "115200", "230400", "460800", "921600", "1000000", "2000000" };
}

//==============================================================================
MainComponent::PortScanner::PortScanner (std::function<void (juce::StringPairArray)> newOnPortsChanged)
    : Thread ("PortScanner"), onPortsChanged { std::move (newOnPortsChanged) }
{
    startThread ();
}

MainComponent::PortScanner::~PortScanner ()
{
    stopThread (2000);
}

void MainComponent::PortScanner::run ()
{
    while (! threadShouldExit ())
    {
        const auto newPorts { SerialPort::getSerialPortPaths () };
        if (newPorts != lastPorts || newPorts.size () != lastPorts.size ())
        {
            lastPorts = newPorts;
            onPortsChanged (newPorts);
        }
        wait (1000);
    }
}

//==============================================================================
MainComponent::MainComponent ()
{
    addAndMakeVisible (portComboBox);
    portComboBox.setTextWhenNothingSelected ("no serial ports");

    addAndMakeVisible (baudComboBox);
    baudComboBox.setEditableText (true);
    baudComboBox.addItemList (kBaudRates, 1);
    baudComboBox.setText ("115200", juce::dontSendNotification);

    addAndMakeVisible (openButton);
    openButton.onClick = [this] { serialPort != nullptr ? closePort () : openPort (); };

    addAndMakeVisible (statusLabel);
    statusLabel.setText ("closed", juce::dontSendNotification);
    addAndMakeVisible (statsLabel);
    statsLabel.setFont (juce::FontOptions { juce::Font::getDefaultMonospacedFontName (), 12.0f, juce::Font::plain });

    addAndMakeVisible (receivedText);
    receivedText.setMultiLine (true);
    receivedText.setReadOnly (true);
    receivedText.setScrollbarsShown (true);
    receivedText.setFont (juce::FontOptions { juce::Font::getDefaultMonospacedFontName (), 13.0f, juce::Font::plain });

    addAndMakeVisible (hexToggle);
    addAndMakeVisible (clearButton);
    clearButton.onClick = [this] { receivedText.clear (); };

    addAndMakeVisible (sendText);
    sendText.setTextToShowWhenEmpty ("text to send", juce::Colours::grey);
    sendText.onReturnKey = [this] { send (); };
    addAndMakeVisible (lineEndingComboBox);
    lineEndingComboBox.addItemList ({ "no line ending", "\\n", "\\r\\n" }, 1);
    lineEndingComboBox.setSelectedId (2, juce::dontSendNotification);
    addAndMakeVisible (sendButton);
    sendButton.onClick = [this] { send (); };

    addAndMakeVisible (echoTestButton);
    echoTestButton.setTooltip ("Sends random data, and checks it comes back. Needs a device running tests/arduino_echo");
    echoTestButton.onClick = [this] { startEchoTest (); };
    addAndMakeVisible (floodToggle);
    floodToggle.setTooltip ("Sends data continuously, to measure throughput");

    portScanner = std::make_unique<PortScanner> ([safeThis = juce::WeakReference<MainComponent> { this }] (juce::StringPairArray newPorts)
    {
        juce::MessageManager::callAsync ([safeThis, newPorts]
        {
            if (safeThis != nullptr)
                safeThis->updatePortList (newPorts);
        });
    });

    setSize (720, 560);
    startTimerHz (30);
}

MainComponent::~MainComponent ()
{
    portScanner = nullptr;
    closePort ();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel ().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized ()
{
    auto bounds { getLocalBounds ().reduced (8) };
    constexpr auto kRowHeight { 28 };
    constexpr auto kGap { 6 };

    auto row { bounds.removeFromTop (kRowHeight) };
    openButton.setBounds (row.removeFromRight (80));
    row.removeFromRight (kGap);
    baudComboBox.setBounds (row.removeFromRight (110));
    row.removeFromRight (kGap);
    portComboBox.setBounds (row);
    bounds.removeFromTop (kGap);

    statusLabel.setBounds (bounds.removeFromTop (22));
    statsLabel.setBounds (bounds.removeFromTop (40));
    bounds.removeFromTop (kGap);

    row = bounds.removeFromBottom (kRowHeight);
    echoTestButton.setBounds (row.removeFromLeft (100));
    row.removeFromLeft (kGap);
    floodToggle.setBounds (row.removeFromLeft (80));
    clearButton.setBounds (row.removeFromRight (70));
    row.removeFromRight (kGap);
    hexToggle.setBounds (row.removeFromRight (60));
    bounds.removeFromBottom (kGap);

    row = bounds.removeFromBottom (kRowHeight);
    sendButton.setBounds (row.removeFromRight (70));
    row.removeFromRight (kGap);
    lineEndingComboBox.setBounds (row.removeFromRight (130));
    row.removeFromRight (kGap);
    sendText.setBounds (row);
    bounds.removeFromBottom (kGap);

    receivedText.setBounds (bounds);
}

void MainComponent::updatePortList (juce::StringPairArray newPorts)
{
    const auto selectedPath { ports.getValue (portComboBox.getText (), {}) };
    ports = newPorts;

    portComboBox.clear (juce::dontSendNotification);
    for (auto index { 0 }; index < ports.size (); ++index)
        portComboBox.addItem (ports.getAllKeys ()[index], index + 1);

    // keep the selection if the port is still there
    const auto selectedIndex { ports.getAllValues ().indexOf (selectedPath) };
    if (selectedIndex >= 0)
        portComboBox.setSelectedItemIndex (selectedIndex, juce::dontSendNotification);
    else if (ports.size () > 0)
        portComboBox.setSelectedItemIndex (0, juce::dontSendNotification);
}

void MainComponent::openPort ()
{
    const auto path { ports.getValue (portComboBox.getText (), {}) };
    if (path.isEmpty ())
        return;

    auto newPort { std::make_unique<SerialPort> ([] (juce::String prefix, juce::String text) { DBG (prefix + " : " + text); }) };
    if (! newPort->open (path))
    {
        statusLabel.setText ("could not open " + path, juce::dontSendNotification);
        return;
    }

    SerialPortConfig config;
    config.bps = static_cast<uint32_t> (baudComboBox.getText ().getIntValue ());
    if (! newPort->setConfig (config))
        statusLabel.setText ("could not set " + juce::String (config.bps) + " bps", juce::dontSendNotification);

    // called on a stream thread, which must not delete the streams, so close the port on the message thread
    newPort->setConnectionLostCallback ([safeThis = juce::WeakReference<MainComponent> { this }]
    {
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr)
                safeThis->handleConnectionLost ();
        });
    });

    serialPort = std::move (newPort);
    // the callback is given to the constructor, so it sees all of the data
    serialPortInput = std::make_unique<SerialPortInputStream> (serialPort.get (), [this] (const void* data, int numBytes)
    {
        const juce::ScopedLock sl { receivedLock };
        receivedPending.append (data, static_cast<size_t> (numBytes));
    });
    serialPortOutput = std::make_unique<SerialPortOutputStream> (serialPort.get ());
    serialPortOutput->setQueueSize (64 * 1024);

    lastBytesReceived = 0;
    lastBytesSent = 0;
    lastStatsTime = juce::Time::getMillisecondCounterHiRes ();
    numWritesCompleted = 0;
    numWritesFailed = 0;
    openButton.setButtonText ("Close");
    statusLabel.setText ("open: " + path + " at " + juce::String (config.bps) + " bps", juce::dontSendNotification);
}

void MainComponent::closePort ()
{
    // the streams must be deleted before the port is closed
    serialPortOutput = nullptr;
    serialPortInput = nullptr;
    if (serialPort != nullptr)
        serialPort->close ();
    serialPort = nullptr;

    floodToggle.setToggleState (false, juce::dontSendNotification);
    echoTestRunning = false;
    openButton.setButtonText ("Open");
}

void MainComponent::handleConnectionLost ()
{
    if (serialPort == nullptr)
        return;

    const auto path { serialPort->getPortPath () };
    closePort ();
    statusLabel.setText ("connection lost: " + path, juce::dontSendNotification);
}

void MainComponent::writeWithStats (const void* data, size_t numBytes)
{
    if (serialPortOutput == nullptr)
        return;

    // the completion is called on the writer thread, once the data has been handed to the driver
    const auto startTicks { juce::Time::getHighResolutionTicks () };
    serialPortOutput->write (data, numBytes, [this, startTicks] (bool sent)
    {
        if (sent)
        {
            ++numWritesCompleted;
            lastWriteLatencyMs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks () - startTicks) * 1000.0;
        }
        else
        {
            ++numWritesFailed;
        }
    });
}

void MainComponent::send ()
{
    auto text { sendText.getText () };
    if (lineEndingComboBox.getSelectedId () == 2)
        text << "\n";
    else if (lineEndingComboBox.getSelectedId () == 3)
        text << "\r\n";
    writeWithStats (text.toRawUTF8 (), text.getNumBytesAsUTF8 ());
}

void MainComponent::startEchoTest ()
{
    if (serialPortOutput == nullptr)
        return;

    juce::MemoryBlock data { kEchoTestSize };
    auto& random { juce::Random::getSystemRandom () };
    for (size_t index { 0 }; index < data.getSize (); ++index)
        data[index] = static_cast<char> (random.nextInt (256));

    {
        const juce::ScopedLock sl { echoLock };
        echoExpected = data;
        echoReceived.reset ();
        echoStartTicks = juce::Time::getHighResolutionTicks ();
        echoTestRunning = true;
    }
    floodToggle.setToggleState (false, juce::dontSendNotification);
    statusLabel.setText ("echo test: sending " + juce::String (kEchoTestSize / 1024) + " KB", juce::dontSendNotification);
    writeWithStats (data.getData (), data.getSize ());
}

void MainComponent::checkEchoTest ()
{
    if (! echoTestRunning)
        return;

    const auto elapsed { juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks () - echoStartTicks) };
    if (echoReceived.getSize () >= echoExpected.getSize ())
    {
        echoTestRunning = false;
        const auto matches { std::memcmp (echoReceived.getData (), echoExpected.getData (), echoExpected.getSize ()) == 0 };
        statusLabel.setText (juce::String ("echo test: ") + (matches ? "PASSED" : "FAILED, data differs") + " in "
                             + juce::String (elapsed * 1000.0, 0) + " ms", juce::dontSendNotification);
    }
    else if (elapsed > 10.0)
    {
        echoTestRunning = false;
        statusLabel.setText ("echo test: FAILED, only " + juce::String (echoReceived.getSize ()) + " of " + juce::String (echoExpected.getSize ())
                             + " bytes came back", juce::dontSendNotification);
    }
}

void MainComponent::appendReceived (const juce::MemoryBlock& data)
{
    if (echoTestRunning)
        echoReceived.append (data.getData (), data.getSize ());

    if (floodToggle.getToggleState () || echoTestRunning)
        return; // too much to display

    juce::String text;
    if (hexToggle.getToggleState ())
        text = juce::String::toHexString (data.getData (), static_cast<int> (data.getSize ())) + " ";
    else
        text = juce::String::fromUTF8 (static_cast<const char*> (data.getData ()), static_cast<int> (data.getSize ()));

    receivedText.moveCaretToEnd ();
    receivedText.insertTextAtCaret (text);
    if (receivedText.getTotalNumChars () > kMaxReceivedTextLength)
    {
        receivedText.setHighlightedRegion ({ 0, receivedText.getTotalNumChars () - kMaxReceivedTextLength / 2 });
        receivedText.insertTextAtCaret ({});
        receivedText.moveCaretToEnd ();
    }
}

void MainComponent::updateStats ()
{
    if (serialPortInput == nullptr || serialPortOutput == nullptr)
    {
        statsLabel.setText ({}, juce::dontSendNotification);
        return;
    }

    const auto now { juce::Time::getMillisecondCounterHiRes () };
    const auto received { serialPortInput->getNumBytesReceived () };
    const auto sent { serialPortOutput->getNumBytesSent () };
    if (now - lastStatsTime >= 500.0)
    {
        const auto seconds { (now - lastStatsTime) / 1000.0 };
        receiveRate = static_cast<double> (received - lastBytesReceived) / seconds;
        sendRate = static_cast<double> (sent - lastBytesSent) / seconds;
        lastBytesReceived = received;
        lastBytesSent = sent;
        lastStatsTime = now;
    }

    statsLabel.setText ("rx " + juce::String (received) + " bytes  " + juce::String (receiveRate / 1024.0, 1) + " KB/s"
                        + "    tx " + juce::String (sent) + " bytes  " + juce::String (sendRate / 1024.0, 1) + " KB/s  pending "
                        + juce::String (serialPortOutput->getNumBytesPending ()) + "\n"
                        + "writes completed " + juce::String (numWritesCompleted.load ()) + "  failed " + juce::String (numWritesFailed.load ())
                        + "  last write latency " + juce::String (lastWriteLatencyMs.load (), 2) + " ms",
                        juce::dontSendNotification);
}

void MainComponent::timerCallback ()
{
    juce::MemoryBlock data;
    {
        const juce::ScopedLock sl { receivedLock };
        data.swapWith (receivedPending);
    }
    if (data.getSize () > 0)
        appendReceived (data);
    checkEchoTest ();

    // keep the queue topped up, without letting it grow
    if (floodToggle.getToggleState () && serialPortOutput != nullptr && serialPortOutput->getNumBytesPending () < kFloodChunkSize * 8)
    {
        juce::MemoryBlock flood { kFloodChunkSize * 8 };
        flood.fillWith ('U');
        writeWithStats (flood.getData (), flood.getSize ());
    }

    updateStats ();
}
