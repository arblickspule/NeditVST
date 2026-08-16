#include "DrawableSlider.h"
#include "svg_assets.h"

namespace ui
{
namespace drawable
{

DrawableSlider::DrawableSlider()
    : track (drawableFromSVG (juce::String (art::sliderTrack))),
      thumb (drawableFromSVG (juce::String (art::sliderThumb)))
{
    setSliderStyle (juce::Slider::LinearHorizontal);
    setRange (0.0, 100.0);
    setValue (0.0, juce::dontSendNotification);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setSize (trackWidth, height);
}

void DrawableSlider::paint (juce::Graphics& g)
{
    // Groove keeps its native rounded-end geometry, centred in the component.
    if (track != nullptr)
    {
        const auto bounds = track->getDrawableBounds();
        track->drawAt (g,
                       (getWidth() - bounds.getWidth()) / 2.0f,
                       getLocalBounds().getCentreY() - bounds.getHeight() / 2.0f,
                       1.0f);
    }

    // Thumb centred on the stock slider's thumb centre -- the value->pixel
    // mapping the drag code itself uses.
    if (thumb != nullptr)
    {
        const auto bounds = thumb->getDrawableBounds();
        thumb->drawAt (g,
                       getPositionOfValue (getValue()) - bounds.getWidth() / 2.0f,
                       getLocalBounds().getCentreY() - bounds.getHeight() / 2.0f,
                       1.0f);
    }
}

} // namespace drawable
} // namespace ui
