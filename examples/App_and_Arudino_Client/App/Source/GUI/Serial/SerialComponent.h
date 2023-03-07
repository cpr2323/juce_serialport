#pragma once

#include <JuceHeader.h>

class SerialComponent : public juce::Component
{
public:
    SerialComponent ();
    void init ();

private:
    juce::ToggleButton checkBox;

    void paint (juce::Graphics& g) override;
    void resized () override;
};