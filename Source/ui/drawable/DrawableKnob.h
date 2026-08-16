#pragma once

#include <JuceHeader.h>

namespace ui
{
namespace drawable
{

//==============================================================================
/** A rotary knob that paints itself from an SVG Drawable (art::knob): the
    pointer sweep is a rotation of the whole SVG about its own centre, so the
    artwork is authored once and re-used across the value range. Interaction
    is the stock JUCE rotary slider (drag vertically, or scroll-wheel), value
    mapping and accessibility come from the Slider base for free. */
class DrawableKnob : public juce::Slider
{
public:
    DrawableKnob();

    void paint (juce::Graphics&) override;

    static constexpr int size = 72;

private:
    std::unique_ptr<juce::Drawable> body;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrawableKnob)
};

} // namespace drawable
} // namespace ui
