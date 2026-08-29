// Nedit -- Plugin layer.
//
// The VSTGUI editor shell: a programmatic CFrame (no uidescription XML)
// whose views are stateless renderers over NeditProcessor's state.
// All geometry derives from Theme.h tokens and getViewSize(); nothing
// hardcodes absolute pixel positions.

#pragma once

#include "public.sdk/source/vst/vstguieditor.h"

#include "vstgui/lib/controls/icontrollistener.h"

#include "state/Types.h"

#include <array>

namespace VSTGUI { class CTextLabel; class CTextButton; class COptionMenu; }

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace nedit::plugin {

class NeditProcessor;
class WaveformView;

namespace ui {

class BarsStepper;
class BpmScrubField;
class SensitivitySlider;
class FadeSlider;
class GrainSlider;
class TabBar;
class PanelView;
class StyleProbSlider;
class ParamMiniSlider;
class ParamMiniMenu;
class IntervalProbBand;

} // namespace ui

// Per-mode enabled/grey mapping for the Generate timing option menus. Each
// menu is enabled EXACTLY when its mode is selected: RESET EVERY rides Slice
// Length; CLOCK REFERENCE + the two sweep scopes ride Clock. Pure + free so
// the mode->grey contract is unit-testable without a widget tree.
struct TimingGreyState
{
    bool resetBarsGreyed = false;
    bool clockRefGreyed = false;
    bool tapeScopeGreyed = false;
    bool filterScopeGreyed = false;
};
[[nodiscard]] TimingGreyState timingGreyState (state::TriggerMode mode) noexcept;

class NeditEditor : public Steinberg::Vst::VSTGUIEditor,
                    public VSTGUI::IControlListener
{
public:
    explicit NeditEditor (NeditProcessor* owner, Steinberg::ViewRect* size = nullptr);

    // Control tags. Values < 1000 are ParamIDs routed to the host edit
    // protocol; the rest are editor-local actions.
    static constexpr std::int32_t kTagLoadSample = 1000;
    static constexpr std::int32_t kTagGenerateModeSL = 1022;
    static constexpr std::int32_t kTagGenerateModeClock = 1023;

    //--- VSTGUIEditor --------------------------------------------------------
    bool PLUGIN_API open (void* parent, const VSTGUI::PlatformType& platformType) override;
    void PLUGIN_API close() override;

    // Only forward real parameter IDs to the host's edit protocol; editor-
    // local control tags (e.g. the Load button) must never reach it, or the
    // host logs "beginEdit() with an invalid parameter ID".
    void beginEdit (VSTGUI_INT32 index) override;
    void endEdit (VSTGUI_INT32 index) override;

    //--- IControlListener ----------------------------------------------------
    void valueChanged (VSTGUI::CControl* control) override;

    //--- CBaseObject (idle timer) --------------------------------------------
    VSTGUI::CMessageResult notify (VSTGUI::CBaseObject* sender,
                                   VSTGUI::IdStringPtr message) override;

    //--- Style-probability paint overlay -------------------------------------
    // Pressing off the thin vertical track in a probability column enters a
    // full-width paint overlay: every column draws a column-width slider and
    // dragging across columns paints each one the pointer crosses at the
    // pointer's height; mouse-up hides the overlay. These are driven by the
    // StyleProbSlider that captured the gesture (it keeps all moves).
    void setStylePaintActive (bool on);
    [[nodiscard]] bool stylePaintActive() const noexcept { return stylePaintActive_; }
    [[nodiscard]] int stylePaintColumn() const noexcept { return stylePaintColumn_; }
    void stylePaintTo (int column, float value);

private:
void runFileSelector();
    void applyParamFromControl (VSTGUI::CControl& control);
    void styleAuditionButton();
    void styleOverrideButton();
    void styleQuantizeButton();
    void stylePitchButton();
    void setGrainEnabled (bool enabled);
    void syncTabBar();
    void syncStyleProbs();
    void syncGenerateControls();
    void syncToolBarControls();
    // Slice-Length/Clock mode-switch segment styling (active = accent fill,
    // like the pitch-mode toggle). styleModeSegment is the per-button rule.
    void styleModeButtons();
    void styleModeSegment (VSTGUI::CTextButton* button, bool active);
    // Returns true only on the RISING EDGE of a CTextButton press. The
    // default kKickStyle fires valueChanged once with the flipped value
    // (max) and once after resetting to min — the rising edge is the one
    // actionable press; this dedupes to exactly one action per click
    // regardless of button value semantics.
    bool pressedEdge (VSTGUI::CControl& control, bool& lastPressed);
    // Host edit protocol for an editor-originated param change:
    // beginEdit / setParamNormalized / performEdit / endEdit.
    void setParam (std::uint32_t id, float normalized);

    NeditProcessor* owner_ = nullptr;
    WaveformView* waveformView_ = nullptr;
    VSTGUI::CTextLabel* sampleNameLabel_ = nullptr; // app-bar sample pill
    VSTGUI::CTextButton* auditionBtn_ = nullptr;    // app-bar audition toggle
    VSTGUI::CTextButton* loadBtn_ = nullptr;         // app-bar load button
    VSTGUI::CTextButton* overrideBtn_ = nullptr;    // toolbar manual-BPM toggle
    ui::BarsStepper* barsStepper_ = nullptr;        // toolbar loop-length stepper
    ui::BpmScrubField* bpmField_ = nullptr;         // toolbar BPM scrub
    ui::SensitivitySlider* sensSlider_ = nullptr;   // toolbar detection slider
    VSTGUI::CTextButton* quantizeBtn_ = nullptr;    // toolbar auto-quantize toggle
    VSTGUI::COptionMenu* quantizeMenu_ = nullptr;   // toolbar quantize-grid dropdown
    ui::FadeSlider* fadeInSlider_ = nullptr;    // toolbar fade-in (attack)
    ui::FadeSlider* fadeOutSlider_ = nullptr;   // toolbar fade-out (release)
    VSTGUI::CTextButton* pitchBtn_ = nullptr;   // toolbar repitch/timestretch toggle
    ui::GrainSlider* grainSizeSlider_ = nullptr;  // toolbar grain size (stretch only)
    ui::GrainSlider* grainSpeedSlider_ = nullptr; // toolbar grain speed (stretch only)
    ui::TabBar* tabBar_ = nullptr;         // performance-page tab strip
    ui::PanelView* panelView_ = nullptr;   // panel area below the tab bar
    VSTGUI::CTextButton* modeSlBtn_ = nullptr;      // Generate timing mode switch
    VSTGUI::CTextButton* modeClockBtn_ = nullptr;
    ui::ParamMiniMenu* resetBarsMenu_ = nullptr;    // Slice Length -> reset bars
    ui::ParamMiniMenu* clockRefMenu_ = nullptr;     // Clock -> reference note value
    ui::ParamMiniMenu* tapeScopeMenu_ = nullptr;    // Clock -> Tape Stop scope
    ui::ParamMiniMenu* filterScopeMenu_ = nullptr;  // Clock -> Filter sweep scope
    ui::IntervalProbBand* intervalBand_ = nullptr;  // Clock -> subdivision weights
    std::array<ui::StyleProbSlider*, state::kNumPlaybackStyles> styleProbSliders_ {};

    // Per-style-column param mini-sliders and mini dropdowns, indexed
    // [style][row], parallel to the rows StyleProbSlider builds from
    // columnParamsFor. Each column uses at most kParamMiniRowCount rows
    // (the Flanger max, 7); a row holds either the continuous mini-slider
    // or the discrete mini dropdown, never both.
    static constexpr std::size_t kParamMiniRowCount = 7;
    std::array<std::array<ui::ParamMiniSlider*, kParamMiniRowCount>,
               state::kNumPlaybackStyles> paramMiniSliders_ {};
    std::array<std::array<ui::ParamMiniMenu*, kParamMiniRowCount>,
               state::kNumPlaybackStyles> paramMiniMenus_ {};

    // Paint-overlay gesture state (see the accessors above).
    bool stylePaintActive_ = false;
    int stylePaintColumn_ = 0;

    // Idle-timer dedup for the toolbar control-value sync (host automation
    // can change the state any time; only push to the controls on change).
    int lastBarsSync_ = -1;
    float lastBpmNormSync_ = -1.0f;
    bool lastOverrideSync_ = false;
    bool lastOverrideStyling_ = false;
    double lastCalcBpmSync_ = -1.0;
    float lastSensitivitySync_ = -1.0f;
    bool lastSensActive_ = false;
    bool lastQuantizeSync_ = false;
    bool lastQuantizeActive_ = false;
    int lastGridSync_ = -1;     // quantize-grid palette index (toolbar dropdown)
    bool lastGridActive_ = false;
    float lastFadeInSync_ = -1.0f;   // fade-in ms (toolbar attack slider)
    float lastFadeOutSync_ = -1.0f;  // fade-out ms (toolbar release slider)
    float lastGrainSizeSync_ = -1.0f;   // grain size (toolbar, stretch only)
    float lastGrainSpeedSync_ = -1.0f;  // grain speed (toolbar, stretch only)
    bool lastPitchSync_ = false;     // pitch-mode toggle (timestretch on)
    bool lastPitchPressed_ = false;  // CTextButton press-edge latch
    int lastTabSync_ = -1;           // performance-page tab (state -> bar)
    std::array<float, state::kNumPlaybackStyles> lastStyleWeightsSync_ {};

    // Generate-page timing sync dedup (state -> controls) + press edges.
    state::TriggerMode lastGenerateModeSync_ = state::TriggerMode::sliceLength;
    int lastResetBarsSync_ = -1;
    int lastClockRefSync_ = -1;
    bool lastResetBarsGreyed_ = false;
    bool lastClockRefGreyed_ = false;
    bool lastTapeScopeGreyed_ = false;
    bool lastFilterScopeGreyed_ = false;
    float lastTapeScopeSync_ = -1.0f;     // recalled scope menu values
    float lastFilterScopeSync_ = -1.0f;
    bool lastModeSlPressed_ = false;      // mode-switch press-edge latches
    bool lastModeClockPressed_ = false;
    std::array<float, state::kNumNoteValues> lastSubdivWeightsSync_ {};

    // Async native file dialog. Runs on a detached background thread so the
    // UI/run-loop thread keeps servicing X events (a blocking dialog on the
    // embedded X11 window thread deadlocks the X server -> desktop freeze).
    // Shared state outlives the editor via shared_ptr, so a still-open
    // dialog can't write into a destroyed editor.
    struct FileDialogState
    {
        std::mutex mutex;
        std::string path;
        std::atomic<bool> ready { false };
        std::atomic<bool> running { false };
    };
    std::shared_ptr<FileDialogState> fileDialog_;
    bool testAutoloadFired_ = false;   // NEDIT_TEST_AUTOLOAD one-shot
    bool lastAuditionSync_ = false;    // idle-timer dedup for audition btn
    bool lastSampleSync_  = false;     // idle-timer dedup for sample presence
    std::string lastSamplePath_;        // idle-timer dedup for sample name
    bool lastLoadPressed_ = false;      // CTextButton press-edge latches
    bool lastAuditionPressed_ = false;
    bool lastOverridePressed_ = false;
    bool lastQuantizePressed_ = false;
};

} // namespace nedit::plugin
