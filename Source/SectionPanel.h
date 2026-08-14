#pragma once

#include <JuceHeader.h>

//==============================================================================
/** A titled, visually-bounded group backdrop for the Generate page's
    sections (Sample / Trim & Tempo / Detection / Fade In/Out / Pitch Mode /
    Playback Style in controlsContent, plus Timing inside the Generate
    sub-mode tab) -- and, in later passes, Sequence/Control/Perform's own
    grouped sections.

    Deliberately does NOT own or reparent its child controls -- the real
    controls (buttons, knobs, segmented rows, labels) stay direct children of
    whatever component already owns them (controlsContent), positioned by the
    caller's own layout code to sit inside getContentArea(). This keeps this
    pass's reparenting risk at zero (no focus-traversal/z-order surprises
    from moving addAndMakeVisible calls) and keeps existing bounds-based
    tricks -- e.g. PluginEditor's getLocalArea()-based Loop-Length staleness
    highlight -- working unmodified regardless of how deeply a control ends
    up visually nested inside a panel.

    Paint style matches the rest of this codebase's hand-painted components:
    flat Tungsten fill, a slightly tinted title-bar strip with the section
    name, thin border -- no gradients, no images. */
class SectionPanel : public juce::Component
{
public:
    explicit SectionPanel (juce::String titleText);

    // Local-space area below the title bar -- callers lay real controls out
    // into this rect (offset by this panel's own position within its
    // parent) rather than SectionPanel positioning anything itself.
    juce::Rectangle<int> getContentArea() const;

    void paint (juce::Graphics&) override;

    static constexpr int titleBarHeight = 26;

private:
    juce::String title;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionPanel)
};
