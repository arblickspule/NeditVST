// The whole UI, behind the ui::contract seam (docs/ui-layout-decision.md).
// This file is the "App.vue": it builds the page tree and knows nothing
// about the plugin shell beyond the AudioProcessor ref it must hand to the
// JUCE editor base class. Swapping the GUI means replacing this file's
// makeEditor() implementation -- nothing else changes.
//
// SHELL (the "lead dev's design", per the first UI block): the old
// PluginEditor and the first Generate-page attempt were archived to
// docs/archive/ (see docs/ui-layout-decision.md). The shell is now:
//
//   [ header                         ]  pinned
//   [ Beats | Textures               ]  top-level tab row (always visible)
//   [ Wave-basics block              ]  PERSISTENT under both tabs: a compact
//   [   Load Audition BPM Fade ...   ]  strip of the six wave-basics inputs
//   [   Wave view (resizable)        ]  above a wave-view region. The drag
//   [--------------------------------]  strip below resizes this block's
//   [ Generate | Sequence | Control | ] height (the wave view is the big
//   [  Perform ]                      ] part, so it gets the real estate).
//   [ content viewport               ]  sub-mode content (Beats only;
//   [                                ]  Textures swaps this for a placeholder)
//
// The top-level tab changes the block BELOW the wave-basics section; that
// block is itself tabbed Generate/Sequence/Control/Perform (the old editor's
// sub-mode tabs). Wave-basics stays put for both top-level tabs.
//
// BLANK PASS: everything below the tab rows is currently a PlaceholderPage /
// blank titled panel -- the areas and panels are in place so the structure
// can be judged before the real controls (the archived GeneratePage +
// BeatsPage content) get redistributed into them.
//
// TOP-LEVEL WINDOW POLICY (this file): the plugin window must never be
// larger than the display it's shown on. The host wraps this editor in its
// own window and sizes it to our requested size -- if that size exceeds the
// available screen area the window can't be dragged to reach the overflow
// (the "layout extends below my visible/mouse-accessible bounds" regression
// from the old editor). So:
//   - setResizeLimits() tells the host the window is user-resizable within
//     [minimumSize, currentDisplay.userBounds] (logical pixels; JUCE applies
//     the display's DPI scale automatically, so Retina 4K portrait and a
//     low-res laptop both work in the same logical space).
//   - the initial size is the default clamped to the work area.
//   - the header, tab rows and wave-basics block are pinned; the active page
//     lives inside a juce::Viewport, and a page taller than the window
//     scrolls internally instead of forcing the window off the screen.

#include "contract.h"
#include "pages/PlaceholderPage.h"
#include "components/WaveBasicsPanel.h"
#include "../SegmentedButtonRow.h"
#include "../NeditPalette.h"

#include <array>

namespace ui
{

namespace
{

//==============================================================================
/** The thin (6px) drag strip between the wave-basics block and the sub-mode
    row: dragging it resizes the persistent block, which is how the wave view
    gets its "resizable" behaviour (the strip is what the host editor clamps
    against min/max before applying). */
class WaveResizeStrip : public juce::Component
{
public:
    std::function<void (int deltaY)> onDragDelta;

    WaveResizeStrip()
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        const int cy = getLocalBounds().getCentreY();
        g.setColour (NeditPalette::tungstenTint (1.6f));
        g.drawHorizontalLine (cy - 1, 0.0f, (float) getWidth());
        g.setColour (NeditPalette::tungstenTint (2.2f));
        g.drawHorizontalLine (cy, 0.0f, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent&) override { isDragging = true; }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        // Total distance from drag start, so the callback can just add it.
        if (isDragging && onDragDelta != nullptr)
            onDragDelta (event.getDistanceFromDragStartY());
    }

    void mouseUp (const juce::MouseEvent&) override { isDragging = false; }

private:
    bool isDragging = false;
};

//==============================================================================
class NeditVstEditor : public juce::AudioProcessorEditor
{
public:
    NeditVstEditor (juce::AudioProcessor& host, SlicerModel&, SlicerEngine&, const UiCallbacks&)
        : AudioProcessorEditor (host)
    {
        setLookAndFeel (&lookAndFeel);
        setColour (juce::ResizableWindow::backgroundColourId, NeditPalette::tungsten);

        // Window policy: resizable by the host, bounded by [min, display work
        // area]. At construction the editor has no peer yet, so this clamps
        // the (empty) bounds up to the minimum -- setSize() below then picks
        // the real initial size.
        updateSizeLimitsForDisplay();

        // Top-level tab row -- always visible; the tab changes the sub-mode
        // block below the wave-basics section.
        topLevelTabs.setOptions ({ { "Beats", std::nullopt }, { "Textures", std::nullopt } });
        topLevelTabs.setSelectedIndex (0, juce::dontSendNotification);
        topLevelTabs.onSelectionChanged = [this] (int index) { showTopLevelTab (index); };
        addAndMakeVisible (topLevelTabs);

        // Persistent wave-basics block + the strip that resizes it.
        addAndMakeVisible (waveBasics);
        waveResizeStrip.onDragDelta = [this] (int deltaY)
        {
            persistentHeight = juce::jlimit (minPersistentHeight, maxPersistentHeight(), persistentHeight + deltaY);
            resized();
        };
        addAndMakeVisible (waveResizeStrip);

        // Sub-mode tab row -- Beats only (the old editor's semantics); the
        // top-level tab switch hides/shows it alongside the content swap.
        subModeTabs.setOptions ({ { "Generate", std::nullopt }, { "Sequence", std::nullopt },
                                  { "Control", std::nullopt }, { "Perform", std::nullopt } });
        subModeTabs.setSelectedIndex (0, juce::dontSendNotification);
        subModeTabs.onSelectionChanged = [this] (int index) { showSubModeTab (index); };
        addAndMakeVisible (subModeTabs);

        // Content pages -- blank for now (see the BLANK PASS comment at the
        // top of this file). Each placeholder says what will land there.
        subPages[0] = std::make_unique<PlaceholderPage> ("Generate", "Playback style and timing controls land here.");
        subPages[1] = std::make_unique<PlaceholderPage> ("Sequence", "Sequencer grid and pattern bank land here.");
        subPages[2] = std::make_unique<PlaceholderPage> ("Control", "Control/automation lands here.");
        subPages[3] = std::make_unique<PlaceholderPage> ("Perform", "Performance keyboard and style parameters land here.");
        texturesPage = std::make_unique<PlaceholderPage> ("Textures", "Texture engine and UI not realised yet.");

        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        const auto initial = initialSize();
        persistentHeight = initial.getHeight() / 3; // the lead dev's ~1/3-of-window persistent block
        setSize (initial.getWidth(), initial.getHeight());

        showTopLevelTab (0); // Beats first, land on Generate
    }

    ~NeditVstEditor() override
    {
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

        auto header = getLocalBounds().removeFromTop (headerTextHeight);

        g.setColour (NeditPalette::textOnTungsten.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        g.drawFittedText ("NeditVST", header, juce::Justification::centredLeft, 1);

        // Live window diagnostics (dev aid while verifying the orientation/
        // resolution policy): logical size + the display scale factor the
        // DPI conversion is happening under.
        g.setColour (NeditPalette::textOnTungsten.withAlpha (0.45f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawFittedText (sizeDiagnostic(), header, juce::Justification::centredRight, 1);
    }

    void resized() override
    {
        // Clamp the user-dragged persistent height into [min, ~60% of the
        // window] so the wave-basics block can never swallow the content.
        persistentHeight = juce::jlimit (minPersistentHeight, maxPersistentHeight(), persistentHeight);

        auto area = getLocalBounds();
        area.removeFromTop (headerTextHeight);
        topLevelTabs.setBounds (area.removeFromTop (SegmentedButtonRow::preferredHeight));
        waveBasics.setBounds (area.removeFromTop (persistentHeight));
        waveResizeStrip.setBounds (area.removeFromTop (resizeStripHeight));

        if (beatsActive)
            subModeTabs.setBounds (area.removeFromTop (SegmentedButtonRow::preferredHeight));

        viewport.setBounds (area);

        // Page width = viewport minus its vertical scrollbar; page height =
        // the content's natural height (at least the viewport, so a blank
        // page still reads as a reserved region). The width guard keeps the
        // pre-setSize construction pass (0x0) from handing a negative width
        // to the page.
        if (activeContent != nullptr && viewport.getWidth() > 0)
            activeContent->setSize (viewport.getWidth() - viewport.getScrollBarThickness(),
                                    juce::jmax (juce::roundToInt (activeContent->getPreferredMainSize()),
                                                viewport.getHeight()));
    }

    void moved() override
    {
        // The plugin window was dragged to another display (or a display
        // scale changed). Re-bounding the maximum to the new work area also
        // clamps the current size down if it no longer fits -- so a window
        // that was fine on a big monitor never overhangs a small one.
        updateSizeLimitsForDisplay();
    }

private:
    void showTopLevelTab (int index)
    {
        beatsActive = (index == 0);
        subModeTabs.setVisible (beatsActive);

        activeContent = beatsActive ? static_cast<Flex*> (subPages[subModeIndex].get())
                                    : static_cast<Flex*> (texturesPage.get());

        viewport.setViewedComponent (activeContent, false); // we own the pages; don't let the viewport delete them
        viewport.setViewPosition (0, 0);
        resized();
    }

    void showSubModeTab (int index)
    {
        subModeIndex = index;
        if (! beatsActive)
            return; // sub-mode tabs only exist under Beats; the content swap already happened

        activeContent = subPages[index].get();
        viewport.setViewedComponent (activeContent, false);
        viewport.setViewPosition (0, 0);
        resized();
    }

    int maxPersistentHeight() const
    {
        return juce::jmax (minPersistentHeight, juce::roundToInt (getHeight() * 0.6f));
    }

    // The display this editor is on (logical pixels), or the primary display
    // before the window has been placed anywhere.
    const juce::Displays::Display* displayForWindow() const
    {
        auto& displays = juce::Desktop::getInstance().getDisplays();

        const auto screenBounds = getScreenBounds();
        if (! screenBounds.isEmpty())
            if (const auto* display = displays.getDisplayForRect (screenBounds))
                return display;

        return displays.getPrimaryDisplay();
    }

    void updateSizeLimitsForDisplay()
    {
        auto& displays = juce::Desktop::getInstance().getDisplays();

        const auto* display = displayForWindow();
        const auto work = (display != nullptr ? display : displays.getPrimaryDisplay())->userBounds.toNearestInt();

        setResizeLimits (minWidth, minHeight,
                         juce::jmax (minWidth,  work.getWidth()  - screenFitMargin),
                         juce::jmax (minHeight, work.getHeight() - screenFitMargin));
    }

    juce::Rectangle<int> initialSize() const
    {
        int width  = defaultWidth;
        int height = defaultHeight;

        if (const auto* display = displayForWindow())
        {
            const auto work = display->userBounds.toNearestInt();
            width  = juce::jmin (width,  juce::jmax (minWidth,  work.getWidth()  - screenFitMargin));
            height = juce::jmin (height, juce::jmax (minHeight, work.getHeight() - screenFitMargin));
        }

        return { width, height };
    }

    juce::String sizeDiagnostic() const
    {
        double scale = 1.0;
        if (const auto* display = displayForWindow())
            scale = display->scale;

        return juce::String (getWidth()) + " x " + juce::String (getHeight())
             + "   @ " + juce::String (scale, 2) + "x scale";
    }

    NeditPalette::LookAndFeel lookAndFeel;
    SegmentedButtonRow topLevelTabs;
    WaveBasicsPanel waveBasics;
    WaveResizeStrip waveResizeStrip;
    SegmentedButtonRow subModeTabs;
    juce::Viewport viewport;
    std::array<std::unique_ptr<PlaceholderPage>, 4> subPages; // Generate / Sequence / Control / Perform
    std::unique_ptr<PlaceholderPage> texturesPage;
    Flex* activeContent = nullptr; // the page currently inside the viewport

    bool beatsActive = true;
    int subModeIndex = 0;
    int persistentHeight = 240; // drag-adjusted; init to ~1/3 of the window in the ctor

    // Window policy constants (logical pixels -- DPI-independent).
    static constexpr int minWidth  = 800;
    static constexpr int minHeight = 600;
    static constexpr int defaultWidth  = 1000;
    static constexpr int defaultHeight = 720;
    static constexpr int headerTextHeight = 30;
    static constexpr int resizeStripHeight = 6;
    static constexpr int minPersistentHeight = 80;
    static constexpr int screenFitMargin = 64; // keep the window inside the work area, not touching the edges

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeditVstEditor)
};

} // namespace

std::unique_ptr<juce::AudioProcessorEditor> makeEditor (juce::AudioProcessor& host,
                                                        SlicerModel& model,
                                                        SlicerEngine& engine,
                                                        const UiCallbacks& callbacks)
{
    return std::make_unique<NeditVstEditor> (host, model, engine, callbacks);
}

} // namespace ui
