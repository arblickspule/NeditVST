#pragma once

#include "../layout/Layout.h"

namespace ui
{

//==============================================================================
/** A titled placeholder page (BLANK PASS): one titled section with a short
    note saying what belongs there, used for every sub-mode page (Generate /
    Sequence / Control / Perform) and the Textures top-level tab until their
    real content lands. The host editor sizes it to at least fill the content
    viewport, so each reserved area reads as a visible region rather than a
    stub tucked into a corner. */
class PlaceholderPage : public Flex
{
public:
    PlaceholderPage (juce::String title, juce::String note)
        : Flex (FlexConfig{ .direction = Direction::column })
    {
        auto label = std::make_unique<juce::Label> ("", std::move (note));
        label->setSize (300, 24);
        label->setJustificationType (juce::Justification::centred);
        label->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));

        std::vector<Cell> cells;
        cells.push_back (section (std::move (title), cell (std::move (label))));
        setCells (std::move (cells));
    }
};

} // namespace ui
