#include "SerialComponent.h"

SerialComponent::SerialComponent ()
{
    setOpaque (true);

    addAndMakeVisible (checkBox);
}

void SerialComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::darkgrey);
}

void SerialComponent::resized ()
{
    checkBox.setBounds (15, 100, 50, 50);
}

void SerialComponent::init ()
{
}
