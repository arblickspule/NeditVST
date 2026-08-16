#pragma once

#include <JuceHeader.h>

//==============================================================================
/** Shared colour palette for the redesigned UI (Pass 1: Generate page) --
    Tungsten (panel/section backgrounds, inactive states) and Salmon
    (selected/active states). Header-only so every custom-painted component
    (SegmentedButtonRow, SectionPanel) and the LookAndFeel below can use
    these without duplicating hex values, and so later passes (Sequence/
    Control/Perform) can reuse the exact same palette. */
namespace NeditPalette
{
    inline const juce::Colour tungsten { 0xFF383838 };
    inline const juce::Colour salmon   { 0xFFFF7E79 };

    // Contrast text colours: Tungsten is dark, needs a light/cream text
    // colour; Salmon is a light warm coral -- pure white reads washed out
    // against it, a warm near-black reads far more legibly.
    inline const juce::Colour textOnTungsten { 0xFFF5EDE6 };
    inline const juce::Colour textOnSalmon   { 0xFF2A2020 };

    // Lightens/darkens Tungsten for title-bar strips, borders, etc. without
    // introducing a second named colour.
    inline juce::Colour tungstenTint (float brightnessFactor)
    {
        return tungsten.withMultipliedBrightness (brightnessFactor);
    }

    //==============================================================================
    /** Minimal LookAndFeel_V4 subclass -- setColour() calls only, no drawing
        overrides -- that recolours every ComboBox/TextButton/ToggleButton/
        Label toward this palette without rebuilding those widget types.
        Scoped to controlsContent only (see PluginEditor), so it never
        affects native dialog chrome (e.g. the file chooser). */
    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
        {
            setColour (juce::ComboBox::backgroundColourId, tungsten);
            setColour (juce::ComboBox::outlineColourId, tungstenTint (1.6f));
            setColour (juce::ComboBox::textColourId, textOnTungsten);
            setColour (juce::ComboBox::arrowColourId, salmon);
            setColour (juce::ComboBox::buttonColourId, tungstenTint (1.3f));

            setColour (juce::PopupMenu::backgroundColourId, tungsten);
            setColour (juce::PopupMenu::textColourId, textOnTungsten);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, salmon);
            setColour (juce::PopupMenu::highlightedTextColourId, textOnSalmon);

            setColour (juce::TextButton::buttonColourId, tungstenTint (1.3f));
            setColour (juce::TextButton::buttonOnColourId, salmon);
            setColour (juce::TextButton::textColourOffId, textOnTungsten);
            setColour (juce::TextButton::textColourOnId, textOnSalmon);

            setColour (juce::ToggleButton::textColourId, textOnTungsten);
            setColour (juce::ToggleButton::tickColourId, salmon);
            setColour (juce::ToggleButton::tickDisabledColourId, tungstenTint (1.6f));

            setColour (juce::Label::textColourId, textOnTungsten);

            setColour (juce::Slider::backgroundColourId, tungsten);
            setColour (juce::Slider::trackColourId, salmon);
            setColour (juce::Slider::thumbColourId, salmon);
            setColour (juce::Slider::textBoxTextColourId, textOnTungsten);
            setColour (juce::Slider::textBoxBackgroundColourId, tungstenTint (1.2f));
            setColour (juce::Slider::textBoxOutlineColourId, tungstenTint (1.6f));

            setColour (juce::TextEditor::backgroundColourId, tungstenTint (1.2f));
            setColour (juce::TextEditor::textColourId, textOnTungsten);
            setColour (juce::TextEditor::outlineColourId, tungstenTint (1.6f));
            setColour (juce::TextEditor::focusedOutlineColourId, salmon);
        }
    };
}
