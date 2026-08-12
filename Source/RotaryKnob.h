#pragma once

#include <JuceHeader.h>

//==============================================================================
/** A custom-painted rotary drag control -- replaces juce::Slider for Loop
    Length and Transient Sensitivity on the Generate page. Drawn as a vector
    arc (juce::Path::addArc), flat fills only, same no-gradient/no-image
    technique as the rest of this codebase's hand-painted components.

    Interaction mirrors juce::Slider closely enough that an existing
    slider-driven call site barely changes: vertical drag (up = increase,
    JUCE's own rotary/linear-vertical convention), onValueChange fires on
    every value update during a drag (mirroring juce::Slider::onValueChange
    firing continuously while dragging) and onDragEnd fires once on mouse
    release (mirroring juce::Slider::onDragEnd) regardless of whether the
    value net-changed. isDragging() lets a caller replicate the
    isMouseButtonDown()-gated live-preview-vs-commit pattern the old
    sensitivity slider used (preview while dragging, commit on release). */
class RotaryKnob : public juce::Component
{
public:
    RotaryKnob() = default;

    void setRange (double newMin, double newMax, double newStep);
    void setValue (double newValue, juce::NotificationType notification = juce::sendNotification);
    double getValue() const noexcept { return value; }

    void setDoubleClickReturnValue (bool shouldBeEnabled, double valueOnDoubleClick);

    // 0 for an integer display (e.g. Loop Length's bar count), 2 for
    // Sensitivity's 0.00-1.00 fractional display.
    void setNumDecimalPlacesToDisplay (int places) { decimalPlaces = places; repaint(); }

    // Short unit/suffix drawn beneath the numeric value, e.g. "bars".
    void setLabel (juce::String newUnitLabel) { unitLabel = std::move (newUnitLabel); repaint(); }

    std::function<void()> onValueChange;
    std::function<void()> onDragEnd;

    bool isDragging() const noexcept { return dragging; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    static constexpr int preferredSize = 64; // square

private:
    void setValueInternal (double newValue, bool notify);

    double minValue = 0.0, maxValue = 1.0, stepSize = 0.0;
    double value = 0.0;
    double dragStartValue = 0.0;
    int dragStartY = 0;
    bool dragging = false;

    bool doubleClickReturnEnabled = false;
    double doubleClickReturnValue = 0.0;

    int decimalPlaces = 0;
    juce::String unitLabel;

    // Vertical pixels needed to traverse the full min->max range -- tuning
    // constant, implementer's discretion (matches WaveformDisplay's own
    // "implementer's discretion, per spec" comment convention for values
    // like this with no single objectively-correct number).
    static constexpr double pixelsPerFullSweep = 200.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RotaryKnob)
};
