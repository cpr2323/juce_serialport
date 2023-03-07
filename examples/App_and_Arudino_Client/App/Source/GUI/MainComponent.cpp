#include "MainComponent.h"

const auto toolWindowHeight { 30 };

MainComponent::MainComponent ()
{

    addAndMakeVisible (serialComponent);
    setSize (800, 600);
}

MainComponent::~MainComponent ()
{
}

void MainComponent::paint ([[maybe_unused]] juce::Graphics& g)
{
}

void MainComponent::resized ()
{
    auto localBounds { getLocalBounds () };
    serialComponent.setBounds (localBounds);
}
