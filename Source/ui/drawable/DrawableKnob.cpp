#include "DrawableKnob.h"
#include "svg_assets.h"

namespace ui
{
namespace drawable
{

DrawableKnob::DrawableKnob()
    : body (drawableFromSVG (juce::String (art::knob)))
{
    setSliderStyle (juce::Slider::RotaryVerticalDrag);

    // Pointer sweeps -150..+150 degrees around 12 o'clock, matching the
    // artwork (a pointer pointing straight up at rest).
    constexpr float pi = juce::MathConstants<float>::pi;
    setRotaryParameters (-5.0f * pi / 6.0f, 5.0f * pi / 6.0f, 1.0f);

    setRange (0.0, 100.0);
    setValue (0.0, juce::dontSendNotification);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setSize (size, size);
}

void DrawableKnob::paint (juce::Graphics& g)
{
    if (body == nullptr)
        return;

    const auto bounds = body->getDrawableBounds();
    const auto params = getRotaryParameters();
    const float angle = params.startAngleRadians
                      + (float) valueToProportionOfLength (getValue())
                          * (params.endAngleRadians - params.startAngleRadians);

    // Rotate about the artwork's own centre, then translate the artwork's
    // centre onto the component's centre. The knob SVG is a square, so the
    // rotated bounds never leave the component.
    auto transform = juce::AffineTransform::rotation (angle, bounds.getCentreX(), bounds.getCentreY());
    transform = transform.followedBy (juce::AffineTransform::translation (
        getLocalBounds().getCentreX() - bounds.getCentreX(),
        getLocalBounds().getCentreY() - bounds.getCentreY()));

    body->draw (g, 1.0f, transform);
}

} // namespace drawable
} // namespace ui
