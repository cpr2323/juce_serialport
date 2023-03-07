#pragma once

#include <JuceHeader.h>

class ToolWindow : public juce::Component
{
public:
    ToolWindow ();
    void init ();

private:

    void paint (juce::Graphics& g) override;
    void resized () override;
};
