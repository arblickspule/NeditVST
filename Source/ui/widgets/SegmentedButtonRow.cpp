#include "SegmentedButtonRow.h"
#include "../NeditPalette.h"

void SegmentedButtonRow::setOptions (std::vector<Option> newOptions)
{
    options = std::move (newOptions);
    selectedIndex = juce::jlimit (0, juce::jmax (0, (int) options.size() - 1), selectedIndex);
    resized();
    repaint();
}

void SegmentedButtonRow::setSelectedIndex (int index, juce::NotificationType notification)
{
    if (options.empty())
        return;

    const int clamped = juce::jlimit (0, (int) options.size() - 1, index);

    if (clamped == selectedIndex)
        return;

    selectedIndex = clamped;
    repaint();

    if (notification == juce::sendNotification && onSelectionChanged)
        onSelectionChanged (selectedIndex);
}

void SegmentedButtonRow::resized()
{
    segmentBounds.clear();

    if (options.empty())
        return;

    const auto bounds = getLocalBounds();
    const int n = (int) options.size();
    const int totalGap = segmentGap * (n - 1);
    const int segmentWidth = juce::jmax (1, (bounds.getWidth() - totalGap) / n);

    int x = bounds.getX();

    for (int i = 0; i < n; ++i)
    {
        // Last segment absorbs any rounding remainder so the row's right
        // edge always lines up exactly with getLocalBounds() rather than
        // leaving a sliver gap from integer division.
        const int width = (i == n - 1) ? (bounds.getRight() - x) : segmentWidth;
        segmentBounds.push_back ({ x, bounds.getY(), width, bounds.getHeight() });
        x += width + segmentGap;
    }
}

int SegmentedButtonRow::getSegmentIndexAt (juce::Point<int> position) const
{
    for (int i = 0; i < (int) segmentBounds.size(); ++i)
        if (segmentBounds[(size_t) i].contains (position))
            return i;

    return -1;
}

void SegmentedButtonRow::mouseDown (const juce::MouseEvent& event)
{
    const int index = getSegmentIndexAt (event.getPosition());

    if (index >= 0)
        setSelectedIndex (index, juce::sendNotification);
}

void SegmentedButtonRow::paint (juce::Graphics& g)
{
    g.setFont (13.0f);

    for (int i = 0; i < (int) options.size(); ++i)
    {
        const auto& option = options[(size_t) i];
        const auto bounds = segmentBounds[(size_t) i];
        const bool selected = (i == selectedIndex);

        juce::Colour fill;
        juce::Colour textColour;

        if (option.colour.has_value())
        {
            fill = selected ? *option.colour : option.colour->withAlpha (0.45f);
            textColour = selected ? NeditPalette::textOnSalmon : NeditPalette::textOnTungsten;
        }
        else
        {
            fill = selected ? NeditPalette::salmon : NeditPalette::tungsten;
            textColour = selected ? NeditPalette::textOnSalmon : NeditPalette::textOnTungsten;
        }

        g.setColour (fill);
        g.fillRect (bounds);

        g.setColour (selected ? juce::Colours::white.withAlpha (0.8f) : juce::Colours::black.withAlpha (0.3f));
        g.drawRect (bounds, selected ? 2 : 1);

        g.setColour (textColour);
        g.drawFittedText (option.label, bounds.reduced (4, 0), juce::Justification::centred, 1);
    }
}
