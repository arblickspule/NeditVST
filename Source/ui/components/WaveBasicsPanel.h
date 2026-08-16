#pragma once

#include "../layout/Layout.h"
#include <vector>

namespace ui
{

//==============================================================================
/** The persistent wave-basics block ("lead dev's design", docs/ui-layout-
    decision.md migration step 2): a compact row of six labelled blank input
    areas (Load / Audition / BPM set+override / Fade in/out / Transient
    sensitivity / Quantize) pinned above a wave-view region that fills the
    rest of the block's height. Always visible under BOTH top-level tabs --
    the tab row below this block switches the sub-mode content; this block
    does not change.

    BLANK PASS: the six areas are title-only TitledPanels and the wave view
    is an empty titled panel -- the real controls (sample chooser, audition
    button, BPM override row, fades, sensitivity slider, quantize grid) and
    the WaveformDisplay get wired into these slots in the next pass. The
    block's height is owned by the host editor (draggable via the resize
    strip it puts below this panel); this panel just fills whatever height it
    is given, with the inputs kept as compact as the DSL allows.

    The six cells share the strip's width equally (flex-grow 1 each), so the
    strip reads as a toolbar of input groups rather than a tall list -- the
    wave view is what gets the vertical real estate. */
class WaveBasicsPanel : public juce::Component
{
public:
    WaveBasicsPanel()
    {
        controlsStrip = std::make_unique<Flex> (
            FlexConfig{ .direction = Direction::row, .gap = 8.0f,
                        .padding = juce::BorderSize<int> (6, 6, 6, 6) });

        std::vector<Cell> cells;
        cells.push_back (cell (makeTitlePanel ("Load")).withGrow (1.0f));
        cells.push_back (cell (makeTitlePanel ("Audition")).withGrow (1.0f));
        cells.push_back (cell (makeTitlePanel ("BPM set + override")).withGrow (1.0f));
        cells.push_back (cell (makeTitlePanel ("Fade in/out")).withGrow (1.0f));
        cells.push_back (cell (makeTitlePanel ("Transient sensitivity")).withGrow (1.0f));
        cells.push_back (cell (makeTitlePanel ("Quantize")).withGrow (1.0f));
        controlsStrip->setCells (std::move (cells));
        addAndMakeVisible (*controlsStrip);

        waveView = std::make_unique<TitledPanel> ("Wave view");
        addAndMakeVisible (*waveView);
    }

    void resized() override
    {
        controlsStrip->setBounds (getLocalBounds().removeFromTop (controlsStripHeight));
        waveView->setBounds (getLocalBounds().withTop (controlsStripHeight).withTrimmedBottom (contentMargin));
    }

    static constexpr int controlsStripHeight = 50; // the six input areas, kept compact
    static constexpr int contentMargin = 6;        // inset under the wave view

private:
    static std::unique_ptr<TitledPanel> makeTitlePanel (juce::String title)
    {
        auto panel = std::make_unique<TitledPanel> (std::move (title));
        panel->setSize (1, 40);
        return panel;
    }

    std::unique_ptr<Flex> controlsStrip;
    std::unique_ptr<TitledPanel> waveView;
};

} // namespace ui
