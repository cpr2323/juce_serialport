#include "ToolWindow.h"

ToolWindow::ToolWindow ()
{
}

void ToolWindow::init ()
{
}

void ToolWindow::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::cadetblue);
}

void ToolWindow::resized ()
{
    auto localBounds { getLocalBounds () };

    localBounds.reduce (5, 3);
}
