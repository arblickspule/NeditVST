#include "SectionPanel.h"
#include "NeditPalette.h"

SectionPanel::SectionPanel (juce::String titleText)
    : title (std::move (titleText))
{
    setInterceptsMouseClicks (false, true); // pure backdrop -- clicks pass through to the real controls drawn on top
}

void SectionPanel::setTitle (juce::String newTitle)
{
    title = std::move (newTitle);
    repaint();
}

juce::Rectangle<int> SectionPanel::getContentArea() const
{
    return getLocalBounds().withTrimmedTop (titleBarHeight).reduced (8, 6);
}

void SectionPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();

    g.setColour (NeditPalette::tungsten);
    g.fillRect (bounds);

    auto titleBar = bounds.removeFromTop (titleBarHeight);
    g.setColour (NeditPalette::tungstenTint (1.35f));
    g.fillRect (titleBar);

    g.setColour (NeditPalette::salmon);
    g.fillRect (titleBar.removeFromBottom (2));

    g.setColour (NeditPalette::textOnTungsten);
    g.setFont (juce::Font (14.0f, juce::Font::bold));
    g.drawFittedText (title, titleBar.reduced (8, 0), juce::Justification::centredLeft, 1);

    g.setColour (NeditPalette::tungstenTint (1.6f));
    g.drawRect (getLocalBounds(), 1);
}
