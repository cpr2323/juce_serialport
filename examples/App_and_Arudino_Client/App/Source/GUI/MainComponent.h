#pragma once

#include <JuceHeader.h>
#include "ToolWindow.h"
#include "Serial/SerialComponent.h"

class MainComponent  : public juce::Component
{
public:
    MainComponent ();
    ~MainComponent () override;

private:
    SerialComponent serialComponent;

    void resized () override;
    void paint (juce::Graphics& g) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
