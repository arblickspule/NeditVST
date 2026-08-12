#include "RotaryKnob.h"
#include "NeditPalette.h"

namespace
{
    // 270-degree sweep, matching the conventional rotary-pot look: pointing
    // straight down at minimum (-135 deg from top) sweeping clockwise to
    // straight down at maximum (+135 deg from top).
    constexpr float startAngle = juce::MathConstants<float>::pi * 1.25f;
    constexpr float endAngle = juce::MathConstants<float>::pi * 2.75f;
}

void RotaryKnob::setRange (double newMin, double newMax, double newStep)
{
    minValue = newMin;
    maxValue = newMax;
    stepSize = newStep;
    setValueInternal (juce::jlimit (minValue, maxValue, value), false);
}

void RotaryKnob::setValueInternal (double newValue, bool notify)
{
    double clamped = juce::jlimit (minValue, maxValue, newValue);

    if (stepSize > 0.0)
        clamped = minValue + std::round ((clamped - minValue) / stepSize) * stepSize;

    if (clamped == value)
        return;

    value = clamped;
    repaint();

    if (notify && onValueChange)
        onValueChange();
}

void RotaryKnob::setValue (double newValue, juce::NotificationType notification)
{
    setValueInternal (newValue, notification == juce::sendNotification);
}

void RotaryKnob::setDoubleClickReturnValue (bool shouldBeEnabled, double valueOnDoubleClick)
{
    doubleClickReturnEnabled = shouldBeEnabled;
    doubleClickReturnValue = valueOnDoubleClick;
}

void RotaryKnob::mouseDown (const juce::MouseEvent& event)
{
    dragStartY = event.y;
    dragStartValue = value;
    dragging = true;
}

void RotaryKnob::mouseDrag (const juce::MouseEvent& event)
{
    const double range = maxValue - minValue;
    const double delta = ((double) (dragStartY - event.y) / pixelsPerFullSweep) * range;
    setValueInternal (dragStartValue + delta, true);
}

void RotaryKnob::mouseUp (const juce::MouseEvent&)
{
    dragging = false;

    if (onDragEnd)
        onDragEnd();
}

void RotaryKnob::mouseDoubleClick (const juce::MouseEvent&)
{
    if (! doubleClickReturnEnabled)
        return;

    setValueInternal (doubleClickReturnValue, true);

    if (onDragEnd)
        onDragEnd();
}

void RotaryKnob::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const float diameter = juce::jmin (bounds.getWidth(), bounds.getHeight() - 16.0f); // leave room for the value/unit text below
    const auto arcBounds = juce::Rectangle<float> (diameter, diameter).withCentre ({ bounds.getCentreX(), diameter * 0.5f + 2.0f });
    const float radius = diameter * 0.5f;
    const float lineThickness = juce::jmax (2.0f, diameter * 0.12f);

    const double range = maxValue - minValue;
    const float fraction = range > 0.0 ? (float) ((value - minValue) / range) : 0.0f;
    const float valueAngle = startAngle + fraction * (endAngle - startAngle);

    // Background track.
    juce::Path track;
    track.addCentredArc (arcBounds.getCentreX(), arcBounds.getCentreY(), radius - lineThickness * 0.5f, radius - lineThickness * 0.5f,
                          0.0f, startAngle, endAngle, true);
    g.setColour (NeditPalette::tungstenTint (1.6f));
    g.strokePath (track, juce::PathStrokeType (lineThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Filled progress arc.
    if (fraction > 0.0f)
    {
        juce::Path progress;
        progress.addCentredArc (arcBounds.getCentreX(), arcBounds.getCentreY(), radius - lineThickness * 0.5f, radius - lineThickness * 0.5f,
                                 0.0f, startAngle, valueAngle, true);
        g.setColour (NeditPalette::salmon);
        g.strokePath (progress, juce::PathStrokeType (lineThickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Centre disc + pointer line.
    g.setColour (NeditPalette::tungsten);
    g.fillEllipse (arcBounds.reduced (lineThickness * 1.4f));

    juce::Path pointer;
    pointer.startNewSubPath (arcBounds.getCentreX(), arcBounds.getCentreY());
    pointer.lineTo (arcBounds.getCentreX() + std::sin (valueAngle) * (radius - lineThickness * 1.6f),
                     arcBounds.getCentreY() - std::cos (valueAngle) * (radius - lineThickness * 1.6f));
    g.setColour (NeditPalette::textOnTungsten);
    g.strokePath (pointer, juce::PathStrokeType (2.0f));

    // Value text, centred inside the knob.
    g.setColour (NeditPalette::textOnTungsten);
    g.setFont (juce::jmax (11.0f, diameter * 0.22f));
    g.drawFittedText (juce::String (value, decimalPlaces), arcBounds.toNearestInt(), juce::Justification::centred, 1);

    // Unit label beneath the knob, if any.
    if (unitLabel.isNotEmpty())
    {
        g.setFont (10.0f);
        g.setColour (NeditPalette::textOnTungsten.withAlpha (0.7f));
        g.drawFittedText (unitLabel, getLocalBounds().removeFromBottom (14), juce::Justification::centred, 1);
    }
}
