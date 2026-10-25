#include "MainComponent.h"

class SerialTerminalApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName () override { return "SerialTerminal"; }
    const juce::String getApplicationVersion () override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed () override { return true; }

    void initialise (const juce::String& /*commandLine*/) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName ());
    }

    void shutdown () override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit () override
    {
        quit ();
    }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Desktop::getInstance ().getDefaultLookAndFeel ().findColour (juce::ResizableWindow::backgroundColourId), allButtons)
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

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (SerialTerminalApplication)
