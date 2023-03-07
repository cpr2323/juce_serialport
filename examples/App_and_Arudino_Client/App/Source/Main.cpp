#include <JuceHeader.h>
#include "GUI/MainComponent.h"
#include "Serial/SerialDevice.h"

// NOTE: this is hard coded for test purposes, and should be replaced with a way to set it in the UI
const juce::String kSerialPortName { "\\\\.\\COM3" };

class JuceSerialApplication : public juce::JUCEApplication
{
public:
    JuceSerialApplication () {}

    const juce::String getApplicationName () override { return ProjectInfo::projectName; }
    const juce::String getApplicationVersion () override { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed () override { return true; }

    void initialise ([[maybe_unused]] const juce::String& commandLine) override
    {
        initSerial ();
        initUi ();
    }

    void shutdown () override
    {
        mainWindow = nullptr; // (deletes our window)
    }

    void anotherInstanceStarted ([[maybe_unused]] const juce::String& commandLine) override
    {
        // When another instance of the app is launched while this one is running,
        // this method is invoked, and the commandLine parameter tells you what
        // the other instance's command-line arguments were.
    }

    void suspended ()
    {
    }

    void resumed ()
    {
    }

    void systemRequestedQuit ()
    {
    }

    void initSerial ()
    {
        // NOTE: for example purposes, the SerialDevice class tries to open the serial port once a name has been assigned
        serialDevice.init (kSerialPortName);
    }

    void initUi ()
    {
        mainWindow.reset (new MainWindow (getApplicationName ()));
    }

    class MainWindow    : public juce::DocumentWindow
    {
    public:
        MainWindow (juce::String name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance ().getDefaultLookAndFeel ()
                                                          .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (), true);

           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen (true);
           #else
            setResizable (true, true);
            centreWithSize (getWidth (), getHeight ());
           #endif

            setVisible (true);
        }

        void closeButtonPressed () override
        {
            JUCEApplication::getInstance ()->systemRequestedQuit ();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;

    SerialDevice serialDevice;
};

START_JUCE_APPLICATION (JuceSerialApplication)
