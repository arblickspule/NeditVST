#pragma once

#include <JuceHeader.h>

namespace ui
{
namespace drawable
{

//==============================================================================
/** A horizontal slider that paints its groove and thumb from two SVG
    Drawables (art::sliderTrack / art::sliderThumb). The groove keeps its
    native rounded-end geometry; the thumb is drawn centred on the stock
    Slider's getPositionOfValue() so drag/scroll/value mapping stay built-in. */
class DrawableSlider : public juce::Slider
{
public:
    DrawableSlider();

    void paint (juce::Graphics&) override;

    static constexpr int trackWidth = 240;
    static constexpr int height = 30;

private:
    std::unique_ptr<juce::Drawable> track;
    std::unique_ptr<juce::Drawable> thumb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrawableSlider)
};

} // namespace drawable
} // namespace ui
