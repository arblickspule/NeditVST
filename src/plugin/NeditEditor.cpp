// Nedit -- Plugin layer. See NeditEditor.h.

#include "NeditEditor.h"

#include "NeditProcessor.h"
#include "ParameterSurface.h"
#include "ProbBandGeometry.h"
#include "WaveformView.h"

#include "ui/SequencerGridGeometry.h"

#include "state/StyleParameters.h"

#include "engine/Tempo.h"

#include "vstgui/lib/ccolor.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cfileselector.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/cgradient.h"
#include "vstgui/lib/controls/cbuttons.h"
#include "vstgui/lib/controls/coptionmenu.h"
#include "vstgui/lib/controls/ctextlabel.h"
#include "vstgui/lib/cvstguitimer.h"
#include "vstgui/lib/events.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#if __linux__
#include <unistd.h>
#endif

#if SMTG_OS_LINUX
#include "public.sdk/source/vst/vstgui_linux_runloop_support.h"
#include "vstgui/lib/platform/linux/linuxfactory.h"
#include "vstgui/lib/platform/platform_x11.h"
#include "vstgui/lib/platform/platformfactory.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace nedit::plugin {

using namespace VSTGUI;

namespace {

// ── Layout constants (from mockup shell-layout.svg) ────────────────────
constexpr int kEditorWidth  = 960;
constexpr int kEditorHeight = 800;
constexpr int kAppBarHeight = 48;
constexpr int kToolBarHeight = 48;   // second strip: bar length / BPM / etc.

// Toolbar control geometry (local to each control's 48px strip): caption
// zone on top, editable box below.
constexpr double kBoxTop = 14.0;     // caption baseline sits above this
constexpr double kBoxH = 24.0;
constexpr double kStepperArmW = 22.0;

// Loop-length bar range (mirror of ParameterSurface's kMaxBars) and the
// manual-BPM surface range. 1..16 bars, 30..300 BPM.
constexpr int kBarsMax = 16;
constexpr double kBpmMin = 30.0;
constexpr double kBpmMax = 300.0;
// Scrub feel: one whole field-width of horizontal drag = 80 BPM. Wide
// enough to sweep the range in a few swipes, fine enough that a 112px
// field gives ~0.7 BPM/px (Shift scales to 0.1x for 0.07 BPM/px).
constexpr double kScrubBpmPerFieldWidth = 80.0;

// ── Colour tokens (design-language.md "Graphite & Salmon") ─────────────
const CColor kWindowBase    {  20,  22,  26, 255 }; // graphite-900
const CColor kSurface1      {  27,  30,  35, 255 }; // graphite-800  app bar, cards
const CColor kSurface2      {  34,  38,  44, 255 }; // graphite-700  chips, inputs
const CColor kSurface3      {  42,  47,  54, 255 }; // graphite-600  hover / active-fill
const CColor kOutline       {  51,  56,  63, 255 }; // hairlines, strokes
const CColor kTextPrimary   { 232, 234, 237, 255 }; // primary copy
const CColor kTextSecondary { 154, 160, 166, 255 }; // labels, inactive
const CColor kTextDisabled  {  95,  99, 104, 255 }; // disabled
const CColor kAccent        { 250, 128, 114, 255 }; // salmon-400
const CColor kAccentBright  { 255, 154, 140, 255 }; // salmon-300  playhead
const CColor kAccentMuted   { 168, 108, 102, 255 }; // muted salmon   active outlines
const CColor kAccentMutedHi { 198, 124, 117, 255 }; // muted salmon+  active value labels
const CColor kAccentOn      {  32,  16,  13, 255 }; // on-salmon   text on accent fill
const CColor kAccentHover   { 224, 104,  90, 255 }; // salmon-500  hover
const CColor kAccentPressed { 194,  85,  72, 255 }; // salmon-600  pressed

// Per-playback-style accents (same mapping as the original's
// PlaybackStylePalette::getStyleColour, ordered by PlaybackStyle ordinal).
const CColor kStyleColours[static_cast<std::size_t> (state::PlaybackStyle::flanger) + 1] {
    { 255, 165,   0, 255 }, // forward     orange
    { 128,   0, 130, 255 }, // ping-pong   purple
    {  30, 144, 255, 255 }, // tape-stop   dodgerblue
    {   0, 128, 128, 255 }, // stretch     teal
    { 255,  64,  64, 255 }, // filter-down red
    { 255, 200,  40, 255 }, // filter-up   gold
    {  70, 200,  80, 255 }, // bitcrush    limegreen
    { 255, 105, 180, 255 }, // scratch     hotpink
    {  60, 210, 220, 255 }  // flanger     cyan
};

// Linear blend of two colours, `t` in [0,1] (0 = a, 1 = b).
constexpr CColor mixColor (const CColor& a, const CColor& b, double t) noexcept
{
    const auto m = [&] (uint8_t ka, uint8_t kb) {
        return static_cast<std::uint8_t> (
            static_cast<double> (ka) + (static_cast<double> (kb) - static_cast<double> (ka)) * t);
    };
    return CColor (m (a.red, b.red), m (a.green, b.green), m (a.blue, b.blue), 255);
}

// Panel layout: card padding and the height of the style-probability band
// shown on the Generate and Sequence tabs (spec: 208px, 9 columns).
constexpr double kCardPad = 12.0;
constexpr double kStyleBandH = 208.0;

// Sequencer transport bar (bottom of the Sequence tab): pattern length
// spinner + grid interval / switch timing / switch interval dropdowns.
constexpr double kSeqTransportH = 48.0;
constexpr double kSeqTransportCaptionH = 12.0;

// Generate-page Timing section geometry (below the style-probability band).
constexpr double kTimingModeBtnH = 36.0;        // SL | Clock switch
constexpr double kTimingGap      = 16.0;        // band -> switch / menus -> band
constexpr double kTimingOptionsGap = 10.0;      // switch -> option menus
constexpr double kTimingMenuH    = 36.0;        // caption (12) + option row (24)
constexpr double kTimingMenuGap  = 16.0;        // between the option menus
constexpr double kSubdivLabelH   = 14.0;        // interval-band per-bar labels
constexpr double kIntervalCaptionH = 14.0;     // band caption row (N=0 chips share it)
constexpr double kZeroChipW  = 46.0;            // subdivision quick-clear chip width
constexpr double kZeroChipGap = 6.0;            // ... gap between the three chips

// ── Style-probability column geometry ──────────────────────────────────
// The header block above the vertical weight slider -- chip + % readout +
// gaps -- and the slider track offsets feed BOTH the column's own
// drawing/hit-testing and the Flanger mini-slider row placement, so they
// live here (not private to StyleProbSlider).
constexpr double kProbChipH = 18.0;            // style label chip
constexpr double kProbReadoutH = 12.0;         // % readout row
constexpr double kProbChipReadoutGap = 2.0;    // chip -> readout
constexpr double kProbReadoutSliderGap = 4.0;  // readout -> slider top
constexpr double kProbTrackW = 8.0;            // vertical slider thickness
constexpr double kProbTrackX = 2.0;            // vertical slider left offset
constexpr double kProbBottomPad = 4.0;
constexpr double kProbRowH = 12.0;             // one caption / mini-slider row
constexpr double kProbSliderRowH = 10.0;       // mini-slider row (thinner: no thumb)
constexpr double kProbCaptionSliderGap = 1.0;  // caption -> its slider
constexpr double kProbRowGap = 5.0;            // inter-entry breathing room

// Editor-local selectors laid into the param columns (tags >= 1000: NOT
// host params -- these are GenerateState.tapeStopScope / filterSweepScope,
// scoped to the Tape Stop and Filter styles, published via publish-only
// setters instead of the automatable surface).
constexpr auto kTagTapeStopScope    = static_cast<VSTGUI_INT32> (1020);
constexpr auto kTagFilterSweepScope = static_cast<VSTGUI_INT32> (1021);
// kTagGenerateModeSL/Clock are declared in NeditEditor.h (1022/1023);
// the plugin tests drive them through the same handler.
constexpr auto kTagResetBars        = static_cast<VSTGUI_INT32> (1024);
constexpr auto kTagClockReference   = static_cast<VSTGUI_INT32> (1025);
constexpr auto kTagIntervalProbBand = static_cast<VSTGUI_INT32> (1026);

// Column param row: one parameter shown in a style column's space under
// the % readout and right of the vertical weight slider, following the
// Flanger pilot recipe. Continuous params get a horizontal mini-slider
// under their caption; discrete params are mini dropdowns whose caption
// IS the current option. Volume rides last in every column. `tag` < 1000
// is a StyleParamId (host-automatable, driven through the host-edit
// protocol); tag >= 1000 is an editor-local scope selector (see the two
// kTag*Scope constants) driven by a publish-only setter.
struct StyleParamRow
{
    VSTGUI_INT32 tag;
    bool continuous;

    [[nodiscard]] state::StyleParamId paramId() const noexcept
    {
        return static_cast<state::StyleParamId> (tag);
    }
};

// Derived from applicableStyleParams: Subdivide is dropped everywhere (it
// is a sequencer retrigger, not a style effect) and Volume Mode is kept
// out (Volume is a bare slider in every column, per the Flanger pilot);
// each swept base param gets its paired *Mode sibling (info.swept => next
// id) inserted right beneath it so e.g. Delay Time / Delay Time Mode ride
// together. Forward's column comes out as a lone Volume slider.
[[nodiscard]] std::vector<StyleParamRow> columnParamsFor (state::PlaybackStyle style)
{
    const auto applicable = state::applicableStyleParams (style);
    std::vector<StyleParamRow> rows;
    rows.reserve (static_cast<std::size_t> (applicable.count) + 4);
    for (int i = 0; i < applicable.count; ++i)
    {
        const auto id = applicable.ids[static_cast<std::size_t> (i)];
        if (id == state::StyleParamId::subdivide
            || id == state::StyleParamId::volumeMode)
            continue;
        const auto& info = state::styleParamInfo (id);
        rows.push_back ({ static_cast<VSTGUI_INT32> (id), ! info.discrete });
        if (info.swept)
        {
            const auto mode = static_cast<state::StyleParamId> (
                static_cast<int> (id) + 1);
            if (mode != state::StyleParamId::volumeMode)
                rows.push_back ({ static_cast<VSTGUI_INT32> (mode), false });
        }
    }
    return rows;
}

// Height one entry consumes in the column list. Continuous entries take a
// caption row + its slider + both gaps; discrete entries a caption row +
// the trailing gap (the list pads out to fill the 208px band).
[[nodiscard]] constexpr double paramEntrySpan (const StyleParamRow& row) noexcept
{
    return row.continuous ? kProbRowH + kProbCaptionSliderGap + kProbSliderRowH
                                + kProbRowGap
                          : kProbRowH + kProbRowGap;
}

// Column-local list origin: the header block (chip + % readout + gaps)
// plus 1px leading that every column's param list starts below.
[[nodiscard]] constexpr double paramListTop() noexcept
{
    return kProbChipH + kProbChipReadoutGap + kProbReadoutH
         + kProbReadoutSliderGap + 1.0;
}

// Local Y of entry i's caption row / of the row directly under a
// continuous caption, for a TOP-anchored list (no alignment offset yet).
[[nodiscard]] constexpr double paramEntryCaptionY (const std::vector<StyleParamRow>& rows,
                                                   std::size_t i) noexcept
{
    double y = paramListTop();
    for (std::size_t j = 0; j < i; ++j)
        y += paramEntrySpan (rows[j]);
    return y;
}

[[nodiscard]] constexpr double paramEntrySliderY (const std::vector<StyleParamRow>& rows,
                                                  std::size_t i) noexcept
{
    return paramEntryCaptionY (rows, i) + kProbRowH + kProbCaptionSliderGap;
}

// The Flanger column's list is the volume-alignment reference: it is the
// longest list (7 entries), so its Volume slider row is where every
// column's own Volume parks. Built once (constexpr-size derived at load).
[[nodiscard]] const std::vector<StyleParamRow>& flangerParamRows()
{
    static const std::vector<StyleParamRow> rows =
        columnParamsFor (state::PlaybackStyle::flanger);
    return rows;
}

// Readout format for a continuous mini-slider's raw value (ms for temps
// that read best in milliseconds, x for a multiplier, plain otherwise).
[[nodiscard]] const char* paramReadoutFormat (state::StyleParamId id) noexcept
{
    switch (id)
    {
        case state::StyleParamId::flangerDelayMs: return "%.1f ms";
        case state::StyleParamId::grainSizeMs:    return "%.0f ms";
        case state::StyleParamId::grainSpeed:     return "%.1f x";
        default:                                  return "%.2f";
    }
}

// ── Control tags ───────────────────────────────────────────────────────
constexpr auto kTagAudition   = static_cast<VSTGUI_INT32> (1001);
constexpr auto kTagSensitivity = static_cast<VSTGUI_INT32> (1002);
constexpr auto kTagQuantize   = static_cast<VSTGUI_INT32> (1003);
constexpr auto kTagQuantizeGrid = static_cast<VSTGUI_INT32> (1004);
constexpr auto kTagFadeIn   = static_cast<VSTGUI_INT32> (1005);
constexpr auto kTagFadeOut  = static_cast<VSTGUI_INT32> (1006);
constexpr auto kTagPitchMode = static_cast<VSTGUI_INT32> (1007);
constexpr auto kTagGrainSize = static_cast<VSTGUI_INT32> (1008);
constexpr auto kTagGrainSpeed = static_cast<VSTGUI_INT32> (1009);
constexpr auto kTagTabBar = static_cast<VSTGUI_INT32> (1010);
constexpr auto kTagStyleProbBase = static_cast<VSTGUI_INT32> (1011); // + style index
// Sequencer transport bar (bottom of the Sequence tab).
constexpr auto kTagSeqPatternLength = static_cast<VSTGUI_INT32> (1031);
constexpr auto kTagSeqGridInterval  = static_cast<VSTGUI_INT32> (1032);
constexpr auto kTagSeqSwitchTiming  = static_cast<VSTGUI_INT32> (1033);
constexpr auto kTagSeqSwitchInterval = static_cast<VSTGUI_INT32> (1034);
// kTagSeqClear (1035) / kTagSeqRandomize (1036) come from NeditEditor.h.

Steinberg::ViewRect kEditorRect (0, 0, kEditorWidth, kEditorHeight);

// ── AppBar: graphite-800 background + salmon logo dot + wordmark ───────
class AppBarView : public CView
{
public:
    using CView::CView;

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();

        // Full-fill graphite-800.
        dc->setFillColor (kSurface1);
        dc->drawRect (r, kDrawFilled);

        // Salmon logo dot — drawn via CGraphicsPath so Cairo's
        // antialiasing applies. VSTGUI defaults to kAliasing (NONE);
        // must explicitly enable AA before drawing.
        if (auto* path = dc->createGraphicsPath())
        {
            path->addEllipse (CRect (r.left + 19, r.top + 21,
                                     r.left + 29, r.top + 31));
            dc->setDrawMode (kAntiAliasing);
            dc->setFillColor (kAccent);
            dc->drawGraphicsPath (path, CDrawContext::kPathFilled);
            dc->setDrawMode (kAliasing);
            path->forget ();
        }

        // "NEDIT" wordmark — baseline y so cap-height centres on the dot.
        dc->setFont (kNormalFontBig, 17);
        dc->setFontColor (kTextPrimary);
        dc->drawString ("NEDIT", CPoint (r.left + 38, r.top + 32));

        // Hairline divider at bottom edge.
        dc->setFrameColor (kOutline);
        dc->setLineWidth (1);
        dc->drawLine (CPoint (r.left, r.bottom - 1),
                      CPoint (r.right, r.bottom - 1));

        setDirty (false);
    }
};

// ── ToolBar: second 48px strip between the app bar and the waveform. ───
// Holds the document-level controls (bar length, BPM override, ...); the
// background band matches the app bar so the two rows read as one header
// unit over the graphite-900 work area.
class ToolBarView : public CView
{
public:
    using CView::CView;

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();

        // Full-fill graphite-800, hairline divider at the bottom edge.
        dc->setFillColor (kSurface1);
        dc->drawRect (r, kDrawFilled);
        dc->setFrameColor (kOutline);
        dc->setLineWidth (1);
        dc->drawLine (CPoint (r.left, r.bottom - 1),
                      CPoint (r.right, r.bottom - 1));

        setDirty (false);
    }
};

// VSTGUI's CDrawContext has no drawRoundRect; route rounded corners through
// a CGraphicsPath (needs kAntiAliasing on the backend, like the logo dot).
void drawRoundedRect (CDrawContext* dc, const CRect& r, CCoord radius,
                      const CColor& fill, const CColor& frame)
{
    auto* path = dc->createGraphicsPath();
    if (path == nullptr)
        return;
    path->addRoundRect (r, radius);
    dc->setDrawMode (kAntiAliasing);
    if (fill.alpha > 0)
    {
        dc->setFillColor (fill);
        dc->drawGraphicsPath (path, CDrawContext::kPathFilled);
    }
    if (frame.alpha > 0)
    {
        dc->setFrameColor (frame);
        dc->setLineWidth (1);
        dc->drawGraphicsPath (path, CDrawContext::kPathStroked);
    }
    path->forget();
    dc->setDrawMode (kAliasing);
}

} // namespace

// ── Toolbar controls --------------------------------------------------------
namespace ui {

// ── Bars stepper: [−] N [+] over the loopLengthBars param (tag 103). ──
// A single CControl drawing its own caption + shell + arms; click on the
// left/right arm steps the bar count and publishes through valueChanged.
class BarsStepper : public CControl
{
public:
    BarsStepper (const CRect& size, IControlListener* l, int32_t t)
        : CControl (size, l, t)
    {
    }

    CBaseObject* newCopy () const override
    {
        return new BarsStepper (getViewSize(), getListener(), getTag());
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        const CRect box (r.left, r.top + kBoxTop, r.right, r.top + kBoxTop + kBoxH);
        const CRect minusBox (box.left, box.top, box.left + kStepperArmW, box.bottom);
        const CRect plusBox (box.right - kStepperArmW, box.top, box.right, box.bottom);

        dc->setDrawMode (kAliasing);
        dc->setFont (kNormalFontSmall);
        dc->setFontColor (kTextSecondary);
        dc->drawString ("BARS", CRect (r.left, r.top + 2, r.right, r.top + 12), kLeftText);

        dc->setFillColor (kSurface2);
        dc->drawRect (box, kDrawFilled);

        dc->setFrameColor (kOutline);
        dc->setLineWidth (1);
        dc->drawLine (CPoint (minusBox.right, minusBox.top),
                      CPoint (minusBox.right, minusBox.bottom));
        dc->drawLine (CPoint (plusBox.left, plusBox.top),
                      CPoint (plusBox.left, plusBox.bottom));

        if (hoverMinus_)
        {
            dc->setFillColor (kSurface3);
            dc->drawRect (minusBox, kDrawFilled);
        }
        if (hoverPlus_)
        {
            dc->setFillColor (kSurface3);
            dc->drawRect (plusBox, kDrawFilled);
        }

        dc->setFont (kNormalFontBig);
        dc->setFontColor (kTextSecondary);
        dc->drawString ("-", minusBox, kCenterText);
        dc->drawString ("+", plusBox, kCenterText);

        dc->setFontColor (kTextPrimary);
        const int bars = currentBars();
        char buf[16];
        std::snprintf (buf, sizeof (buf), "%d", bars);
        dc->drawString (buf, CRect (minusBox.right, box.top, plusBox.left, box.bottom),
                        kCenterText);

        // Shell stroke on top of everything.
        dc->setFrameColor (kOutline);
        dc->setLineWidth (1);
        dc->drawRect (box, kDrawStroked);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! buttons.isLeftButton())
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        const CRect box (0, kBoxTop, getViewSize().getWidth(), kBoxTop + kBoxH);
        if (! box.pointInside (local))
            return kMouseEventNotHandled;

        const int bars = currentBars();
        int next = bars;
        if (local.x <= kStepperArmW)
            next = std::max (1, bars - 1);
        else if (local.x >= getViewSize().getWidth() - kStepperArmW)
            next = std::min (kBarsMax, bars + 1);
        else
            return kMouseEventNotHandled;

        setValueNormalized (static_cast<float> (next - 1)
                            / static_cast<float> (kBarsMax - 1));
        hoverFollow (where);
        invalid();
        valueChanged();
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        hoverFollow (where);
        return kMouseEventNotHandled;
    }

private:
    int currentBars() const noexcept
    {
        const float normalized = getValueNormalized();
        const int bars = 1 + static_cast<int> (
            std::lround (normalized * static_cast<float> (kBarsMax - 1)));
        return bars < 1 ? 1 : (bars > kBarsMax ? kBarsMax : bars);
    }

    void hoverFollow (const CPoint& where) noexcept
    {
        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        const bool inBox = local.y >= kBoxTop && local.y < kBoxTop + kBoxH;
        const bool hMinus = inBox && local.x <= kStepperArmW;
        const bool hPlus = inBox && local.x >= getViewSize().getWidth() - kStepperArmW;
        if (hMinus != hoverMinus_ || hPlus != hoverPlus_)
        {
            hoverMinus_ = hMinus;
            hoverPlus_ = hPlus;
            invalid();
        }
    }

    bool hoverMinus_ = false;
    bool hoverPlus_ = false;
};

// ── Pattern length spinner (Sequencer transport bar) ───────────────────
// A BarsStepper-style +/- stepper but over the discrete pattern-length
// table kPatternLengthBarsValues = {1, 2, 4} (state's
// patternLengthBarsIndex 0..2). The box shows the bar count; a step wraps
// at the ends of the table. Publishes through valueChanged.
class PatternLengthStepper : public CControl
{
public:
    PatternLengthStepper (const CRect& size, IControlListener* l, int32_t t)
        : CControl (size, l, t)
    {
    }

    CBaseObject* newCopy () const override
    {
        return new PatternLengthStepper (getViewSize(), getListener(), getTag());
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        const CRect box (r.left, r.top + kBoxTop, r.right, r.top + kBoxTop + kBoxH);
        const CRect minusBox (box.left, box.top, box.left + kStepperArmW, box.bottom);
        const CRect plusBox (box.right - kStepperArmW, box.top, box.right, box.bottom);

        dc->setDrawMode (kAliasing);
        dc->setFont (kNormalFontSmall);
        dc->setFontColor (kTextSecondary);

        dc->setFillColor (kSurface2);
        dc->drawRect (box, kDrawFilled);

        dc->setFrameColor (kOutline);
        dc->setLineWidth (1);
        dc->drawLine (CPoint (minusBox.right, minusBox.top),
                      CPoint (minusBox.right, minusBox.bottom));
        dc->drawLine (CPoint (plusBox.left, plusBox.top),
                      CPoint (plusBox.left, plusBox.bottom));

        if (hoverMinus_)
        {
            dc->setFillColor (kSurface3);
            dc->drawRect (minusBox, kDrawFilled);
        }
        if (hoverPlus_)
        {
            dc->setFillColor (kSurface3);
            dc->drawRect (plusBox, kDrawFilled);
        }

        dc->setFont (kNormalFontBig);
        dc->setFontColor (kTextSecondary);
        dc->drawString ("-", minusBox, kCenterText);
        dc->drawString ("+", plusBox, kCenterText);

        dc->setFontColor (kTextPrimary);
        char buf[16];
        std::snprintf (buf, sizeof (buf), "%d", currentBars());
        dc->drawString (buf, CRect (minusBox.right, box.top, plusBox.left, box.bottom),
                        kCenterText);

        dc->setFrameColor (kOutline);
        dc->setLineWidth (1);
        dc->drawRect (box, kDrawStroked);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! buttons.isLeftButton())
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        const CRect box (0, kBoxTop, getViewSize().getWidth(), kBoxTop + kBoxH);
        if (! box.pointInside (local))
            return kMouseEventNotHandled;

        const int n = static_cast<int> (state::kPatternLengthBarsValues.size());
        int idx = currentIndex();
        int next = idx;
        if (local.x <= kStepperArmW)
            next = (idx - 1 + n) % n;
        else if (local.x >= getViewSize().getWidth() - kStepperArmW)
            next = (idx + 1) % n;
        else
            return kMouseEventNotHandled;

        if (next != idx)
        {
            setValueNormalized (static_cast<float> (next) / static_cast<float> (n - 1));
            hoverFollow (where);
            invalid();
            valueChanged();
        }
        else
        {
            hoverFollow (where);
        }
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        hoverFollow (where);
        return kMouseEventNotHandled;
    }

private:
    [[nodiscard]] int currentIndex() const noexcept
    {
        const int n = static_cast<int> (state::kPatternLengthBarsValues.size());
        const int idx = static_cast<int> (
            std::lround (getValueNormalized() * static_cast<float> (n - 1)));
        return idx < 0 ? 0 : (idx >= n ? n - 1 : idx);
    }

    [[nodiscard]] int currentBars() const noexcept
    {
        return state::kPatternLengthBarsValues[static_cast<std::size_t> (currentIndex())];
    }

    void hoverFollow (const CPoint& where) noexcept
    {
        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        const bool inBox = local.y >= kBoxTop && local.y < kBoxTop + kBoxH;
        const bool hMinus = inBox && local.x <= kStepperArmW;
        const bool hPlus = inBox && local.x >= getViewSize().getWidth() - kStepperArmW;
        if (hMinus != hoverMinus_ || hPlus != hoverPlus_)
        {
            hoverMinus_ = hMinus;
            hoverPlus_ = hPlus;
            invalid();
        }
    }

    bool hoverMinus_ = false;
    bool hoverPlus_ = false;
};

// ── Sequencer transport bar scrim ──────────────────────────────────────
// Draws the divider line + the caption row above the four transport
// controls (pattern length spinner + the three dropdowns). The controls
// themselves are sibling boxes overlaid on this view; putting the captions
// here keeps them aligned with the COptionMenus, which can't draw their own
// caption row.
class SequencerTransportBar : public CView
{
public:
    struct Caption
    {
        const char* label;
        double left;
        double right;
    };

    // Owns a COPY of the captions -- the caller's array is usually a
    // stack-local in open(), so holding the pointer would dangle the moment
    // open() returns (crash on the first Sequence-tab draw).
    SequencerTransportBar (const CRect& size, const Caption* captions, std::size_t count)
        : CView (size), captions_ (captions, captions + count)
    {
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();

        // Divider line above the section.
        dc->setDrawMode (kAliasing);
        dc->setFrameColor (kOutline);
        dc->setLineWidth (1);
        dc->drawLine (CPoint (r.left, r.top), CPoint (r.right, r.top));

        dc->setFont (kNormalFontSmall);
        dc->setFontColor (kTextSecondary);
        for (const auto& c : captions_)
            dc->drawString (c.label,
                            CRect (c.left, r.top + 2, c.right, r.top + kSeqTransportCaptionH),
                            kLeftText);

        setDirty (false);
    }

    CBaseObject* newCopy () const override
    {
        return new SequencerTransportBar (getViewSize(), captions_.data(), captions_.size());
    }

private:
    std::vector<Caption> captions_;
};

// ── BPM field: drag-to-scrub readout over the manualTempoBpm param ────
// (tag 102). With the override off it is a read-only greyed label showing
// the tempo DERIVED from loopLengthBars + the source span (calculatedBpm);
// when override is on it becomes active and drag across the field scrubs
// the value (Shift = 0.1x resolution), wheel nudges by 1 BPM.
class BpmScrubField : public CControl
{
public:
    BpmScrubField (const CRect& size, IControlListener* l, int32_t t)
        : CControl (size, l, t)
    {
    }

    CBaseObject* newCopy () const override
    {
        return new BpmScrubField (getViewSize(), getListener(), getTag());
    }

    void setMode (bool active, double calculatedBpm)
    {
        active_ = active;
        calcBpm_ = calculatedBpm;
        setMouseEnabled (active);
        if (! active)
            dragging_ = false;
        invalid();
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        const CRect box (r.left, r.top + kBoxTop, r.right, r.top + kBoxTop + kBoxH);

        dc->setDrawMode (kAliasing);
        dc->setFont (kNormalFontSmall);
        dc->setFontColor (kTextSecondary);
        dc->drawString ("BPM", CRect (r.left, r.top + 2, r.right, r.top + 12), kLeftText);

        dc->setFillColor (kSurface2);
        dc->drawRect (box, kDrawFilled);

        const double displayed = active_ ? bpmFromValue (getValueNormalized()) : calcBpm_;
        char buf[32];
        if (displayed <= 0.0)
            std::snprintf (buf, sizeof (buf), "--");
        else
            std::snprintf (buf, sizeof (buf), "%.1f", displayed);

        dc->setFont (kNormalFontBig);
        dc->setFontColor (active_ ? kTextPrimary : kTextDisabled);
        dc->drawString (buf, box, kCenterText);

        dc->setFrameColor (active_ ? kAccentMuted : kOutline);
        dc->setLineWidth (1);
        dc->drawRect (box, kDrawStroked);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! active_ || ! buttons.isLeftButton())
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        const CRect box (0, kBoxTop, getViewSize().getWidth(), kBoxTop + kBoxH);
        if (! box.pointInside (local))
            return kMouseEventNotHandled;

        dragging_ = true;
        startX_ = local.x;
        startBpm_ = bpmFromValue (getValueNormalized());
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState& buttons) override
    {
        if (! dragging_)
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        const double width = getViewSize().getWidth();
        double delta = (local.x - startX_) / width * kScrubBpmPerFieldWidth;
        if (buttons.isShiftSet())
            delta *= 0.1;

        double bpm = startBpm_ + delta;
        if (bpm < kBpmMin)
            bpm = kBpmMin;
        if (bpm > kBpmMax)
            bpm = kBpmMax;
        applyBpm (std::round (bpm * 10.0) / 10.0);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint& where, const CButtonState& buttons) override
    {
        onMouseMoved (where, buttons);
        dragging_ = false;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        dragging_ = false;
        return kMouseEventHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        if (! active_ || event.deltaY == 0.0)
            return;

        const double step = event.modifiers.has (ModifierKey::Shift) ? 0.1 : 1.0;
        double bpm = bpmFromValue (getValueNormalized()) + event.deltaY * step;
        if (bpm < kBpmMin)
            bpm = kBpmMin;
        if (bpm > kBpmMax)
            bpm = kBpmMax;
        applyBpm (std::round (bpm * 10.0) / 10.0);
        event.consumed = true;
    }

private:
    double bpmFromValue (float normalized) const noexcept
    {
        return kBpmMin + static_cast<double> (normalized) * (kBpmMax - kBpmMin);
    }

    void applyBpm (double bpm)
    {
        const float normalized = static_cast<float> ((bpm - kBpmMin) / (kBpmMax - kBpmMin));
        setValueNormalized (normalized);
        invalid();
        valueChanged();
    }

    bool active_ = false;
    bool dragging_ = false;
    double calcBpm_ = 0.0;
    double startBpm_ = 0.0;
    VSTGUI::CCoord startX_ = 0.0;
};

// ── Sensitivity slider: transient-detection threshold, 0..1. ─────────────
// (editor-local tag 1002, NOT a host param -- dragging re-runs detection at
// the new threshold and rebuilds the slice list, so it is a structural edit
// like a manual marker, not automatable automation). Dragging anywhere on
// the box sets the value; wheel nudges by 0.05. Only active with a sample.
class SensitivitySlider : public CControl
{
public:
    SensitivitySlider (const CRect& size, IControlListener* l, int32_t t)
        : CControl (size, l, t)
    {
    }

    CBaseObject* newCopy () const override
    {
        return new SensitivitySlider (getViewSize(), getListener(), getTag());
    }

    void setActive (bool active)
    {
        active_ = active;
        setMouseEnabled (active);
        if (! active)
            dragging_ = false;
        invalid();
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        const CRect box (r.left, r.top + kBoxTop, r.right, r.top + kBoxTop + kBoxH);

        dc->setDrawMode (kAliasing);
        dc->setFont (kNormalFontSmall);
        dc->setFontColor (kTextSecondary);
        dc->drawString ("SENS", CRect (r.left, r.top + 2, r.right, r.top + 12), kLeftText);

        const float v = getValueNormalized();

        char buf[24];
std::snprintf (buf, sizeof (buf), "%d%%",
                   static_cast<int> (std::lround (v * 100.0f)));
        dc->setFontColor (active_ ? kAccentMutedHi : kTextDisabled);
        dc->drawString (buf, CRect (r.right - 52, r.top + 2, r.right, r.top + 12), kRightText);

        dc->setFillColor (kSurface2);
        dc->drawRect (box, kDrawFilled);

        if (v > 0.0f)
        {
            const double inset = 2.0;
            const double fillW = (box.right - box.left - 2.0 * inset) * v;
            const CRect fill (box.left + inset, box.top + inset,
                              box.left + inset + fillW, box.bottom - inset);
            dc->setFillColor (kAccent);
            dc->drawRect (fill, kDrawFilled);
        }

        dc->setFrameColor (active_ ? kAccentMuted : kOutline);
        dc->setLineWidth (1);
        dc->drawRect (box, kDrawStroked);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! active_ || ! buttons.isLeftButton())
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        const CRect box (0, kBoxTop, getViewSize().getWidth(), kBoxTop + kBoxH);
        if (! box.pointInside (local))
            return kMouseEventNotHandled;

        dragging_ = true;
        applyFromX (local.x);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        if (! dragging_)
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        applyFromX (local.x);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint& where, const CButtonState& buttons) override
    {
        onMouseMoved (where, buttons);
        dragging_ = false;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        dragging_ = false;
        return kMouseEventHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        if (! active_ || event.deltaY == 0.0)
            return;

        applyValue (getValueNormalized()
                    + static_cast<float> (event.deltaY * 0.05));
        event.consumed = true;
    }

private:
    void applyFromX (double localX)
    {
        const double width = getViewSize().getWidth();
        applyValue (static_cast<float> (localX / width));
    }

    void applyValue (float v)
    {
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1.0f)
            v = 1.0f;
        setValueNormalized (v);
        invalid();
        valueChanged();
    }

    bool active_ = false;
    bool dragging_ = false;
};

// ── Fade slider: per-pick declick fade in ms, 0..10. ──────────────────────
// (editor-local tags 1005/1006, NOT host params -- attack = render.fadeInMs,
// release = render.fadeOutMs, a global play-feel setting). Compact stacked
// form: the whole rect is the box -- label left + ms readout right on the
// top line, thin accent track along the bottom. Always active (valid
// without a sample). Wheel nudges by 1 ms.
class FadeSlider : public CControl
{
public:
    FadeSlider (const CRect& size, IControlListener* l, int32_t t, const char* label)
        : CControl (size, l, t), label_ (label != nullptr ? label : "")
    {
    }

    CBaseObject* newCopy () const override
    {
        return new FadeSlider (getViewSize(), getListener(), getTag(), label_.c_str());
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        dc->setDrawMode (kAliasing);
        const float v = getValueNormalized();

        dc->setFillColor (kSurface2);
        dc->drawRect (r, kDrawFilled);

        // Thin proportional track along the bottom of the box.
        constexpr double kTrackH = 6.0;
        const CRect track (r.left + 2, r.bottom - 2 - kTrackH, r.right - 2, r.bottom - 2);
        dc->setFillColor (kSurface3);
        dc->drawRect (track, kDrawFilled);
        if (v > 0.0f)
        {
            const double fillW = (track.right - track.left) * v;
            const CRect fill (track.left, track.top, track.left + fillW, track.bottom);
            dc->setFillColor (kAccent);
            dc->drawRect (fill, kDrawFilled);
        }

        dc->setFont (kNormalFontSmall);
        dc->setFontColor (kTextSecondary);
        dc->drawString (label_.c_str(),
                        CRect (r.left + 4, r.top + 1, r.right - 4, r.top + 13), kLeftText);

        char buf[24];
std::snprintf (buf, sizeof (buf), "%dms",
                   static_cast<int> (std::lround (msFromValue (v))));
        dc->setFontColor (kAccentMutedHi);
        dc->drawString (buf, CRect (r.left + 4, r.top + 1, r.right - 4, r.top + 13), kRightText);

        dc->setFrameColor (kAccentMuted);
        dc->setLineWidth (1);
        dc->drawRect (r, kDrawStroked);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! buttons.isLeftButton())
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        if (local.x < 0 || local.x > getViewSize().getWidth())
            return kMouseEventNotHandled;

        dragging_ = true;
        applyFromX (local.x);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        if (! dragging_)
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        applyFromX (local.x);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint& where, const CButtonState& buttons) override
    {
        onMouseMoved (where, buttons);
        dragging_ = false;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        dragging_ = false;
        return kMouseEventHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        if (event.deltaY == 0.0)
            return;

        applyValue (msFromValue (getValueNormalized()) + event.deltaY * 1.0);
        event.consumed = true;
    }

private:
    static constexpr double kMinMs = 0.0;
    static constexpr double kMaxMs = 10.0;

    double msFromValue (float normalized) const noexcept
    {
        return kMinMs + static_cast<double> (normalized) * (kMaxMs - kMinMs);
    }

    void applyFromX (double localX)
    {
        const double width = getViewSize().getWidth();
        applyValue (msFromValue (static_cast<float> (localX / width)));
    }

    void applyValue (double ms)
    {
        if (ms < kMinMs)
            ms = kMinMs;
        if (ms > kMaxMs)
            ms = kMaxMs;
        setValueNormalized (static_cast<float> ((ms - kMinMs) / (kMaxMs - kMinMs)));
        invalid();
        valueChanged();
    }

    std::string label_;
    bool dragging_ = false;
};

// Time-Stretch granular-character slider (grain size in ms / grain speed in
// x). Same compact stacked layout as FadeSlider but parameterized over a
// [min,max] range and only ENABLED while the render pitch mode is
// time-stretch; disabled sliders are dimmed and reject mouse input.
class GrainSlider : public CControl
{
public:
    GrainSlider (const CRect& size, IControlListener* l, int32_t t, const char* label,
                 double min, double max, const char* fmt, double wheelStep)
        : CControl (size, l, t),
          label_ (label != nullptr ? label : ""),
          min_ (min), max_ (max),
          fmt_ (fmt != nullptr ? fmt : "%.1f"),
          wheelStep_ (wheelStep)
    {
    }

    CBaseObject* newCopy () const override
    {
        return new GrainSlider (getViewSize(), getListener(), getTag(), label_.c_str(),
                                min_, max_, fmt_.c_str(), wheelStep_);
    }

    void setActive (bool active)
    {
        if (active == active_)
            return;
        active_ = active;
        setMouseEnabled (active);
        invalid();
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        dc->setDrawMode (kAliasing);
        const float v = getValueNormalized();

        dc->setFillColor (active_ ? kSurface2 : kSurface1);
        dc->drawRect (r, kDrawFilled);

        // Thin proportional track along the bottom of the box.
        constexpr double kTrackH = 6.0;
        const CRect track (r.left + 2, r.bottom - 2 - kTrackH, r.right - 2, r.bottom - 2);
        dc->setFillColor (active_ ? kSurface3 : kOutline);
        dc->drawRect (track, kDrawFilled);
        if (active_ && v > 0.0f)
        {
            const double fillW = (track.right - track.left) * v;
            const CRect fill (track.left, track.top, track.left + fillW, track.bottom);
            dc->setFillColor (kAccent);
            dc->drawRect (fill, kDrawFilled);
        }

        dc->setFont (kNormalFontSmall);
        dc->setFontColor (active_ ? kTextSecondary : kTextDisabled);
        dc->drawString (label_.c_str(),
                        CRect (r.left + 4, r.top + 1, r.right - 4, r.top + 13), kLeftText);

        char buf[24];
        std::snprintf (buf, sizeof (buf), fmt_.c_str(), valueFromNorm (v));
        dc->setFontColor (active_ ? kAccentMutedHi : kTextDisabled);
        dc->drawString (buf, CRect (r.left + 4, r.top + 1, r.right - 4, r.top + 13), kRightText);

        dc->setFrameColor (active_ ? kAccentMuted : kOutline);
        dc->setLineWidth (1);
        dc->drawRect (r, kDrawStroked);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! active_ || ! buttons.isLeftButton())
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        if (local.x < 0 || local.x > getViewSize().getWidth())
            return kMouseEventNotHandled;

        dragging_ = true;
        applyFromX (local.x);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        if (! dragging_)
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        applyFromX (local.x);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint& where, const CButtonState& buttons) override
    {
        onMouseMoved (where, buttons);
        dragging_ = false;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        dragging_ = false;
        return kMouseEventHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        if (! active_ || event.deltaY == 0.0)
            return;

        applyValue (valueFromNorm (getValueNormalized()) + event.deltaY * wheelStep_);
        event.consumed = true;
    }

private:
    double valueFromNorm (float normalized) const noexcept
    {
        return min_ + static_cast<double> (normalized) * (max_ - min_);
    }

    void applyFromX (double localX)
    {
        const double width = getViewSize().getWidth();
        applyValue (valueFromNorm (static_cast<float> (localX / width)));
    }

    void applyValue (double newValue)
    {
        if (newValue < min_)
            newValue = min_;
        if (newValue > max_)
            newValue = max_;
        setValueNormalized (static_cast<float> ((newValue - min_) / (max_ - min_)));
        invalid();
        valueChanged();
    }

    std::string label_;
    double min_ = 0.0;
    double max_ = 1.0;
    std::string fmt_;
    double wheelStep_ = 1.0;
    bool active_ = true;
    bool dragging_ = false;
};

// ── Style-probability slider: one per playback style, the vertical	  ──
// probability bar aligned to the LEFT edge of its column inside the
// Generate/Sequence panel band. Each control is one column: a label chip
// at the top (centred, background tinted from the style's palette colour)
// plus the slider spanning the remaining height, filled upward from the
// bottom. Value = the style's draw weight in [0,1]; tags are
// kTagStyleProbBase + style ordinal. Only the slider track itself is the
// hit target for drags; once grabbed, the pointer can leave the track.
class StyleProbSlider : public CControl
{
public:
    StyleProbSlider (const CRect& size, IControlListener* l, int32_t t, int styleIndex)
        : CControl (size, l, t), styleIndex_ (styleIndex),
          rows_ (columnParamsFor (static_cast<state::PlaybackStyle> (styleIndex))),
          owner_ (static_cast<NeditEditor*> (l))
    {
    }

    CBaseObject* newCopy () const override
    {
        return new StyleProbSlider (getViewSize(), getListener(), getTag(), styleIndex_);
    }

    // The style's param layout rows (see columnParamsFor); the editor lays
    // the mini-slider/menu controls on top of this column from here.
    [[nodiscard]] const std::vector<StyleParamRow>& paramRows() const noexcept
    {
        return rows_;
    }

// Local (column-relative) Y of entry i's caption row / of the
// mini-slider row directly under a continuous caption. The header
// block above the list is included, plus 1px leading. Every row except
// the final one (Volume, which ends every column) stays top-anchored in
// the column's own list; the final row parks on the Flanger column's
// Volume row so all the volumes line up across the band.
[[nodiscard]] double captionRowLocalY (std::size_t i) const noexcept
{
    return (i + 1 == rows_.size()) ? alignedVolumeCaptionY()
                                   : paramEntryCaptionY (rows_, i);
}

[[nodiscard]] double sliderRowLocalY (std::size_t i) const noexcept
{
    return (i + 1 == rows_.size()) ? alignedVolumeSliderY()
                                   : paramEntrySliderY (rows_, i);
}

// The aligned Volume row: the Flanger column's list is the reference
// (longest, so its Volume row is the line every other column's Volume
// parks on).
[[nodiscard]] double alignedVolumeSliderY() const noexcept
{
    const auto& ref = flangerParamRows();
    return paramEntrySliderY (ref, ref.size() - 1);
}

[[nodiscard]] double alignedVolumeCaptionY() const noexcept
{
    return alignedVolumeSliderY() - kProbRowH - kProbCaptionSliderGap;
}

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        dc->setDrawMode (kAliasing);

        const double v = getValueNormalized();
        const CColor& accent = kStyleColours[static_cast<std::size_t> (styleIndex_)];

        // ── Label chip: top-centred, background = the style colour dimmed
        // toward the card surface; text = brightened style colour so the
        // graphite theme holds while each style stays identifiable.
        const CRect chip (r.left + 2, r.top, r.right - 2, r.top + kProbChipH);
        dc->setFillColor (mixColor (accent, kSurface1, 0.78));
        dc->drawRect (chip, kDrawFilled);
        dc->setFont (kNormalFontSmall);
        dc->setFontColor (mixColor (accent, kTextPrimary, 0.35));
        dc->drawString (state::playbackStyleName (
                            static_cast<state::PlaybackStyle> (styleIndex_)),
                        chip, kCenterText);

        // ── Percent readout, top row under the chip, right-aligned so it
        // clears the left-aligned slider below. ──
        char buf[16];
        std::snprintf (buf, sizeof (buf), "%d%%",
                       static_cast<int> (std::lround (v * 100.0)));
        dc->setFont (kNormalFontSmaller);
        dc->setFontColor (kTextSecondary);
        dc->drawString (buf,
                        CRect (r.left + 2, r.top + kProbChipH + kProbChipReadoutGap,
                               r.right - 2,
                               r.top + kProbChipH + kProbChipReadoutGap + kProbReadoutH),
                        kRightText);

        // ── Vertical slider ──. Normally a narrow left-aligned track with
        // an accent fill + tick; while the paint overlay is active every
        // column draws a FULL-WIDTH track instead (the whole style section
        // reads as a painted bar) and the column under the pointer gets a
        // bright outline.
        const double top = sliderTop (r);
        const double bottom = r.bottom - kProbBottomPad;
        if (owner_->stylePaintActive())
        {
            const CRect wide (r.left + 2, top, r.right - 2, bottom);
            dc->setFillColor (kSurface2);
            dc->drawRect (wide, kDrawFilled);
            if (v > 0.0)
            {
                const double fillH = (bottom - top) * v;
                dc->setFillColor (accent);
                dc->drawRect (CRect (wide.left, bottom - fillH, wide.right, bottom),
                              kDrawFilled);
            }
            // The parametres are not painted over -- every column is full
            // width live, so the gesture reads as one continuous paint.
            if (owner_->stylePaintColumn() == styleIndex_)
            {
                dc->setFrameColor (kAccentBright);
                dc->drawRect (wide, kDrawStroked);
            }
        }
        else
        {
            const CRect track (r.left + kProbTrackX, top,
                               r.left + kProbTrackX + kProbTrackW, bottom);
            dc->setFillColor (kSurface2);
            dc->drawRect (track, kDrawFilled);
            if (v > 0.0)
            {
                const double fillH = (bottom - top) * v;
                dc->setFillColor (accent);
                dc->drawRect (CRect (track.left, bottom - fillH, track.right, bottom),
                              kDrawFilled);
            }
            const double tickY = bottom - (bottom - top) * v;
            dc->setFillColor (kAccentBright);
            dc->drawRect (CRect (track.left - 1, tickY, track.right + 1, tickY + 2.0),
                          kDrawFilled);

            // ── Param controls: every column hosts its style's params in
            // the space under the % and right of the slider. Subdivide is
            // dropped (a SEQUENCER retrigger function, not a style effect).
            // Continuous params get a 12px caption row here + a
            // ParamMiniSlider (drawn on top) in the 10px row beneath;
            // discrete params' rows are owned entirely by their
            // ParamMiniMenu on top (caption + current value). Skipped in
            // paint mode so the wide bars stay uncluttered.
            if (! rows_.empty())
            {
                dc->setFont (kNormalFontSmaller);
                dc->setFontColor (mixColor (accent, kTextPrimary, 0.45));
                const double listX = track.right + 4.0;
                const double listW = r.right - 2.0 - listX;
                for (std::size_t i = 0; i < rows_.size(); ++i)
                {
                    if (! rows_[i].continuous)
                        continue;   // owned by the mini dropdown overlay
                    const double labelY = r.top + captionRowLocalY (i);
                    dc->drawString (state::styleParamInfo (rows_[i].paramId()).name,
                                    CRect (listX, labelY, listX + listW,
                                           labelY + kProbRowH),
                                    kLeftText);
                }
            }
        }

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! buttons.isLeftButton())
            return kMouseEventNotHandled;
        // Hit-test only the actual slider track (a narrow vertical bar at
        // the column's left). A press ON the track is a precise single-
        // column adjust; a press anywhere else in the column enters the
        // paint overlay -- this view keeps every subsequent move, so it can
        // paint whichever column the pointer crosses.
        const double x = where.x - getViewSize().left;
        if (x < kProbTrackX || x > kProbTrackX + kProbTrackW)
        {
            paintOwner_ = true;
            owner_->setStylePaintActive (true);
            paintAt (where);
            return kMouseEventHandled;
        }
        dragging_ = true;
        applyFromY (where.y - getViewSize().top);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        if (paintOwner_)
        {
            paintAt (where);
            return kMouseEventHandled;
        }
        if (! dragging_)
            return kMouseEventNotHandled;
        applyFromY (where.y - getViewSize().top);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint& where, const CButtonState& buttons) override
    {
        if (paintOwner_)
        {
            paintAt (where);
            paintOwner_ = false;
            owner_->setStylePaintActive (false);
            return kMouseEventHandled;
        }
        onMouseMoved (where, buttons);
        dragging_ = false;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        if (paintOwner_)
        {
            paintOwner_ = false;
            owner_->setStylePaintActive (false);
            return kMouseEventHandled;
        }
        dragging_ = false;
        return kMouseEventHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        if (event.deltaY == 0.0)
            return;
        applyValue (getValueNormalized() + static_cast<double> (event.deltaY) * 0.05);
        event.consumed = true;
    }

private:
    // Absolute (frame-space) top of the slider band, for drawing.
    [[nodiscard]] double sliderTop (const CRect& r) const noexcept
    {
        return r.top + sliderBandTopLocal();
    }

    // Local-header height above the slider (same value feeds drawing rects
    // and the hit->value mapping in applyFromY). kProb* geometry lives at
    // file scope so the Flanger mini-slider rows line up with it.
    [[nodiscard]] static constexpr double sliderBandTopLocal() noexcept
    {
        return kProbChipH + kProbChipReadoutGap + kProbReadoutH + kProbReadoutSliderGap;
    }

    // Paint the column currently under the pointer at the pointer's height.
    // `where` is frame-space; the columns are contiguous, each colW wide,
    // so the whole band is recoverable from this column's own rect (its
    // left = bandLeft + styleIndex_ * colW). Cross out of the band and the
    // geometry clamps to the edge columns.
    void paintAt (const CPoint& where)
    {
        const double colW = getViewSize().getWidth();
        const double bandLeft = getViewSize().left
                              - static_cast<double> (styleIndex_) * colW;
        const int col = ui::probColumnFromX (where.x, bandLeft, colW);
        // All columns share the band's top, so any column's local offset
        // works; the paint value maps from the pointer's height in the
        // same slider band the vertical bars use.
        const double top = sliderBandTopLocal();
        const double bottom = getViewSize().getHeight() - kProbBottomPad;
        owner_->stylePaintTo (col, probValueFromY (where.y - getViewSize().top, top, bottom));
    }

    void applyFromY (double localY)
    {
        // localY arrived in the column's OWN coordinate space (already
        // offset by getViewSize().top in onMouseDown/Moved), so the slider
        // band must be described in the same local space -- NOT the
        // absolute-ized sliderTop() used for drawing.
        const double top = sliderBandTopLocal();
        const double bottom = getViewSize().getHeight() - kProbBottomPad;
        setValueNormalized (probValueFromY (localY, top, bottom));
        invalid();
        valueChanged();
    }

    void applyValue (double v)
    {
        if (v < 0.0)
            v = 0.0;
        if (v > 1.0)
            v = 1.0;
        setValueNormalized (static_cast<float> (v));
        invalid();
        valueChanged();
    }

    int styleIndex_ = 0;
    bool dragging_ = false;
    bool paintOwner_ = false;   // this column captured the paint gesture
    std::vector<StyleParamRow> rows_;
    NeditEditor* owner_ = nullptr;
};

// ── Horizontal mini-slider for ONE continuous style param, sitting in ──
// the 12px row directly under its caption in the Flanger column. The tag
// IS the StyleParamId (== a ParameterSurface id below 1000), so user edits
// flow through the default host-edit protocol (applyParamFromControl ->
// setParam) instead of a publisher-only setter. Value = normalized param;
// the right-hand readout shows the raw value via styleParamInfo.
class ParamMiniSlider : public CControl
{
public:
    ParamMiniSlider (const CRect& size, IControlListener* l, int32_t t,
                     int styleIndex, const char* fmt)
        : CControl (size, l, t), styleIndex_ (styleIndex),
          fmt_ (fmt != nullptr ? fmt : "%.2f")
    {
    }

    CBaseObject* newCopy () const override
    {
        return new ParamMiniSlider (getViewSize(), getListener(), getTag(),
                                    styleIndex_, fmt_.c_str());
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        dc->setDrawMode (kAliasing);
        const float v = getValueNormalized();
        const CColor& accent = kStyleColours[static_cast<std::size_t> (styleIndex_)];

        constexpr double kTrackH = 4.0;
        const double trackTop = r.top + (r.getHeight() - kTrackH) / 2.0;
        const CRect track (r.left + 2.0, trackTop, r.right - kValueW - 2.0,
                           trackTop + kTrackH);
        dc->setFillColor (kSurface2);
        dc->drawRect (track, kDrawFilled);
        if (v > 0.0f)
        {
            const double fillW = (track.right - track.left) * v;
            dc->setFillColor (accent);
            dc->drawRect (CRect (track.left, track.top, track.left + fillW, track.bottom),
                          kDrawFilled);
        }

        char buf[24];
        std::snprintf (buf, sizeof (buf), fmt_.c_str(), valueFromNorm (v));
        dc->setFont (kNormalFontSmaller);
        dc->setFontColor (kAccentMutedHi);
        dc->drawString (buf,
                        CRect (r.right - kValueW, r.top, r.right - 2.0, r.bottom),
                        kRightText);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! buttons.isLeftButton())
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        if (local.x < 0 || local.x > getViewSize().getWidth())
            return kMouseEventNotHandled;

        dragging_ = true;
        applyFromX (local.x);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        if (! dragging_)
            return kMouseEventNotHandled;

        const CPoint local (where.x - getViewSize().left, where.y - getViewSize().top);
        applyFromX (local.x);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint& where, const CButtonState& buttons) override
    {
        onMouseMoved (where, buttons);
        dragging_ = false;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        dragging_ = false;
        return kMouseEventHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        if (event.deltaY == 0.0)
            return;
        applyValue (getValueNormalized() + static_cast<double> (event.deltaY) * 0.05);
        event.consumed = true;
    }

private:
    // Raw display value (for the readout): normalized -> styleParamInfo
    // range keyed by the control's tag (the StyleParamId).
    double valueFromNorm (float normalized) const noexcept
    {
        const auto& info = state::styleParamInfo (
            static_cast<state::StyleParamId> (getTag()));
        return info.minValue + static_cast<double> (normalized)
                                   * (info.maxValue - info.minValue);
    }

    void applyFromX (double localX)
    {
        // Map against the TRACK width only -- the 32px value readout on the
        // right is not part of the fill, so clamps keep 100% at the track's
        // right edge (the readout slot would otherwise offset the drag).
        const double trackLeft = 2.0;
        const double trackRight = getViewSize().getWidth() - kValueW - 2.0;
        const double span = trackRight - trackLeft;
        double v = span > 0.0 ? (localX - trackLeft) / span : 0.0;
        if (v < 0.0)
            v = 0.0;
        if (v > 1.0)
            v = 1.0;
        applyValue (v);
    }

    void applyValue (double v)
    {
        if (v < 0.0)
            v = 0.0;
        if (v > 1.0)
            v = 1.0;
        setValueNormalized (static_cast<float> (v));
        invalid();
        valueChanged();
    }

    int styleIndex_ = 0;
    bool dragging_ = false;
    static constexpr double kValueW = 32.0;   // right slot for the raw readout
    std::string fmt_;
};

// ── Option list helpers for a discrete param row ─────────────────────
// Scope selectors (editor-local tags) are the fixed two-item
// "Whole window" / "Per tick" list (WindowScope, same wording as the
// original's selectors); anything else is a StyleParamId.

[[nodiscard]] int paramRowOptionCount (VSTGUI_INT32 tag) noexcept
{
    if (tag == kTagTapeStopScope || tag == kTagFilterSweepScope)
        return 2;
    // Only StyleParamIds (tags < kNumStyleParams) may index the option
    // table -- editor-local tags (timing menus etc.) supply their own
    // entries after construction.
    if (tag < 0 || tag >= static_cast<VSTGUI_INT32> (state::kNumStyleParams))
        return 0;
    return state::styleParamInfo (static_cast<state::StyleParamId> (tag)).numOptions;
}

[[nodiscard]] const char* paramRowOptionName (VSTGUI_INT32 tag, int index) noexcept
{
    if (tag == kTagTapeStopScope || tag == kTagFilterSweepScope)
        return index <= 0 ? "Whole window" : "Per tick";
    if (tag < 0 || tag >= static_cast<VSTGUI_INT32> (state::kNumStyleParams))
        return nullptr;
    return state::styleParamOptionName (static_cast<state::StyleParamId> (tag), index);
}

// Normalized value of a scope selector read from state. Entry index 0 =
// wholeWindow, 1 = perTick -- matches WindowScope's numeric values, so
// the menu index IS the enum and no mapping is needed.
[[nodiscard]] float scopeSelectorNorm (const state::PluginState& s, VSTGUI_INT32 tag) noexcept
{
    const auto scope = tag == kTagTapeStopScope ? s.generate.tapeStopScope
                                                : s.generate.filterSweepScope;
    return scope == state::WindowScope::perTick ? 1.0f : 0.0f;
}

// ── Mini dropdown for ONE discrete param row ───────────────────────
// For the band columns it is the 12px caption row: the current option IS
// the row's label, a quiet accent-muted text + caret + hairline. The same
// control doubles as the Generate-page timing options with an optional
// caption line drawn above (option list supplied by the caller) and a
// greyed/disabled state for the inactive mode's column. Subclassed from
// COptionMenu so the click publishes its own popup and the value stays an
// entry index. Tag < 1000 = a StyleParamId (entries built from its option
// table via paramRowOptionCount/Name); >= 1000 = editor-local (entries
// must be supplied after construction, or built from the scope tags).
class ParamMiniMenu : public COptionMenu
{
public:
    ParamMiniMenu (const CRect& size, IControlListener* l, int32_t t, int styleIndex,
                   const char* caption = nullptr)
        : COptionMenu (size, l, t), styleIndex_ (styleIndex), caption_ (caption)
    {
        for (int i = 0; i < paramRowOptionCount (t); ++i)
            addEntry (paramRowOptionName (t, i));
        // COptionMenu starts with currentIndex == -1, so getCurrent() returns
        // nullptr and the row draws BLANK until something calls setValue.
        // syncStyleProbs only pushes when the state value differs from the
        // menu's (dedup gate skips state defaults of index 0), so a default-0
        // param never populated its row. Pre-select the first entry so the
        // row always shows the current option; sync then corrects non-zero
        // state defaults (setValue does not echo a valueChanged).
        setValue (0.f);
    }

    CBaseObject* newCopy () const override
    {
        auto* copy = new ParamMiniMenu (getViewSize(), getListener(), getTag(),
                                        styleIndex_, caption_);
        for (int32_t i = 0; i < getNbEntries(); ++i)
            if (auto* e = getEntry (i))
                copy->addEntry (e->getTitle().data());
        copy->setValue (getValue());
        copy->greyed_ = greyed_;
        return copy;
    }

    // Greyed rows are read-only: input disabled and the whole row drawn in
    // the disabled palette (the inactive mode's column).
    void setGreyed (bool greyed)
    {
        greyed_ = greyed;
        setMouseEnabled (! greyed);
        invalid();
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        dc->setDrawMode (kAliasing);

        const double optTop = (caption_ != nullptr) ? r.top + kMenuCaptionH : r.top;
        const double optTextBottom = (caption_ != nullptr) ? r.bottom - 2.0 : r.bottom;

        if (caption_ != nullptr)
        {
            dc->setFont (kNormalFontSmaller);
            dc->setFontColor (greyed_ ? kTextDisabled : kTextSecondary);
            dc->drawString (caption_, CRect (r.left + 2, r.top, r.right - 2, optTop),
                            kLeftText);
        }

        // The current option's title is the row's own text, tinted like a
        // value readout (interactive, unlike the muted caption rows).
        CMenuItem* item = getCurrent();
        dc->setFont (kNormalFontSmaller);
        dc->setFontColor (greyed_ ? kTextDisabled : kAccentMutedHi);
        dc->drawString (item != nullptr ? item->getTitle().data() : "",
                        CRect (r.left + 2, optTop, r.right - 12, optTextBottom), kLeftText);

        // Caret triangle, drawn (the default font's glyph set is not
        // guaranteed to include ▾), antialiased for the small size.
        const double cx = r.right - 7.0;
        const double cy = (optTop + optTextBottom) / 2.0;
        dc->setDrawMode (kAntiAliasing);
        dc->setFillColor (greyed_ ? kTextDisabled : kTextSecondary);
        const std::vector<CPoint> tri {
            { cx - 3.0, cy - 1.5 }, { cx + 3.0, cy - 1.5 }, { cx, cy + 2.0 }
        };
        dc->drawPolygon (tri, kDrawFilled);
        dc->setDrawMode (kAliasing);

        // Hairline underline = the input affordance (matches the quiet
        // read of the mini-slider rows above).
        dc->setFrameColor (kOutline);
        dc->setLineWidth (1.0);
        dc->drawLine (CPoint (r.left + 2, r.bottom - 1), CPoint (r.right - 2, r.bottom - 1));

        setDirty (false);
    }

private:
    static constexpr double kMenuCaptionH = 12.0;
    int styleIndex_ = 0;
    const char* caption_ = nullptr;
    bool greyed_ = false;
};

// ── Clock subdivision weights: 20 thin probability bars, one per note  ──
// value (kNoteValues palette order, 128n..1n), spanning the Generate
// panel's remaining width below the mode switch + option menus. Value =
// raw GenerateState.subdivisionWeights[i] in [0,1], the weighted draw for
// the retrigger interval inside each clock window. The whole band is the
// hit target: press/drag paints the bar under the pointer (same gesture
// language as the style columns' paint overlay), wheel nudges the column
// under the cursor. Only live in Clock mode -- in Slice Length the band
// greys and ignores input (the weights are Clock-only state).
class IntervalProbBand : public CControl
{
public:
    IntervalProbBand (const CRect& size, IControlListener* l, int32_t t,
                      NeditProcessor* owner)
        : CControl (size, l, t), owner_ (owner)
    {
    }

    CBaseObject* newCopy () const override
    {
        return new IntervalProbBand (getViewSize(), getListener(), getTag(), owner_);
    }

    // Column (note-value slot) of the current/ last drag, for the editor's
    // valueChanged dispatch.
    [[nodiscard]] int activeColumnIndex () const noexcept { return activeColumnIndex_; }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        const bool enabled = clockEnabled();
        const auto& weights = owner_->uiStateView().generate.subdivisionWeights;

        dc->setDrawMode (kAliasing);
        dc->setFont (kNormalFontSmaller);
        dc->setFontColor (enabled ? kTextSecondary : kTextDisabled);
        dc->drawString (enabled ? "INTERVAL PROBABILITY"
                                : "INTERVAL PROBABILITY  (CLOCK MODE ONLY)",
                        CRect (r.left, r.top, r.right, r.top + kCaptionH), kLeftText);

        const double barTop = r.top + kCaptionH;
        const double barBottom = r.bottom - kSubdivLabelH;
        const double colW = r.getWidth() / static_cast<double> (state::kNumNoteValues);
        constexpr double barW = 8.0;

        for (int i = 0; i < state::kNumNoteValues; ++i)
        {
            const double cx = r.left + static_cast<double> (i) * colW;

            dc->setFont (kNormalFontSmaller);
            dc->setFontColor (enabled ? kTextSecondary : kTextDisabled);
            dc->drawString (
                state::kNoteValues[static_cast<std::size_t> (i)].name,
                CRect (cx, barBottom, cx + colW, r.bottom), kCenterText);

            const CRect track (cx + 2.0, barTop, cx + 2.0 + barW, barBottom);
            dc->setFillColor (kSurface2);
            dc->drawRect (track, kDrawFilled);

            const double v = std::clamp (
                static_cast<double> (weights[static_cast<std::size_t> (i)]), 0.0, 1.0);
            if (v > 0.0)
            {
                const double fillH = (track.bottom - track.top) * v;
                dc->setFillColor (enabled ? kAccent : kSurface3);
                dc->drawRect (
                    CRect (track.left, track.bottom - fillH, track.right, track.bottom),
                    kDrawFilled);
            }

            // The grabbed column gets a bright outline while dragging, like
            // the style-band paint overlay's live column.
            if (dragging_ && activeColumnIndex_ == i)
            {
                dc->setFrameColor (kAccentBright);
                dc->setLineWidth (1.0);
                dc->drawRect (CRect (cx, barTop, cx + colW, barBottom), kDrawStroked);
            }
        }

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! clockEnabled() || ! buttons.isLeftButton())
            return kMouseEventNotHandled;
        dragging_ = true;
        applyAt (where);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        if (! dragging_)
            return kMouseEventNotHandled;
        applyAt (where);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint& where, const CButtonState& buttons) override
    {
        onMouseMoved (where, buttons);
        dragging_ = false;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        dragging_ = false;
        return kMouseEventHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        if (! clockEnabled() || event.deltaY == 0.0)
            return;
        const double colW = getViewSize().getWidth()
                            / static_cast<double> (state::kNumNoteValues);
        const int col = std::clamp (
            static_cast<int> ((event.mousePosition.x - getViewSize().left) / colW),
            0, state::kNumNoteValues - 1);
        activeColumnIndex_ = col;
        setValueNormalized (std::clamp (getValueNormalized()
                                            + static_cast<float> (event.deltaY) * 0.05f,
                                        0.0f, 1.0f));
        invalid();
        valueChanged();
        event.consumed = true;
    }

private:
    [[nodiscard]] bool clockEnabled () const noexcept
    {
        return owner_->uiStateView().generate.generateMode == state::TriggerMode::clock;
    }

    // Paint gesture: map a frame-space point to a column (by x) + value
    // (top of the bar area = 1.0, bottom = 0.0), publish via valueChanged
    // so the editor's single dispatch path owns the setter call.
    void applyAt (const CPoint& where)
    {
        const CRect r = getViewSize();
        const double colW = r.getWidth() / static_cast<double> (state::kNumNoteValues);
        const int col = std::clamp (
            static_cast<int> ((where.x - r.left) / colW), 0, state::kNumNoteValues - 1);
        const double barTop = r.top + kCaptionH;
        const double barBottom = r.bottom - kSubdivLabelH;
        double v = (barBottom > barTop)
                       ? 1.0 - (where.y - barTop) / (barBottom - barTop)
                       : 0.0;
        v = std::clamp (v, 0.0, 1.0);

        activeColumnIndex_ = col;
        setValueNormalized (static_cast<float> (v));
        invalid();
        valueChanged();
    }

    // Shared with the editor so the N=0 chips sit exactly on the caption row.
    static constexpr double kCaptionH = kIntervalCaptionH;
    NeditProcessor* owner_ = nullptr;
    bool dragging_ = false;
    int activeColumnIndex_ = 0;
};

// The framework-free step-grid geometry lives in nedit::ui (nedit_ui);
// bring the helpers into this nedit::plugin::ui scope so the control reads
// them unqualified.
using nedit::ui::clampSequencerScroll;
using nedit::ui::columnFromX;
using nedit::ui::computeSequencerLayout;
using nedit::ui::naturalStepsForSlice;
using nedit::ui::noteValueBeats;
using nedit::ui::rowFromBottomY;
using nedit::ui::SequencerGridLayout;

// ── Sequencer step grid (Sequence tab). ─────────────────────────────────
// A piano-roll step grid (slice 0 at the BOTTOM): procedural sizing
// (everything derives from the viewport + grid dims via the pure
// SequencerGridGeometry helpers), vertical scrolling when the slice count
// exceeds the viewport, beat-bar shading, active cells rendered as bars
// spanning their declared length (mirroring the engine's Sequenced pick,
// including the anticipatory clamp), per-cell extension + override
// markers, and a live playhead column. Interactions: left-drag paints the
// selected drawing style (painting over the same style erases), right-drag
// erases, Shift+drag extends the grabbed cell's declared length, wheel
// scrolls.
class SequencerGridView : public CControl
{
public:
    SequencerGridView (const CRect& size, IControlListener* l, int32_t t,
                       NeditEditor* editor)
        : CControl (size, l, t), editor_ (editor)
    {
    }

    CBaseObject* newCopy () const override
    {
        return new SequencerGridView (getViewSize(), getListener(), getTag(), editor_);
    }

    // Height of the style-palette bar reserved at the top of the grid view
    // (a thin strip of 9 colour lines aligned to the probability band above,
    // with a salmon indicator under the selected drawing style).
    static constexpr double kPaletteH = 18.0;

    [[nodiscard]] int scroll() const noexcept { return scrollRows_; }

    void setScroll (int rows) noexcept
    {
        const int clamped = std::max (0, rows);
        if (clamped != scrollRows_)
        {
            scrollRows_ = clamped;
            invalid();
        }
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        dc->setDrawMode (kAliasing);

        const NeditProcessor* owner = editor_->owner();
        const auto& seq = owner->uiStateView().sequencer;
        auto loaded = owner->acquireLoadedSample();
        const int totalRows = loaded != nullptr ? seq.rows : 0;

        rebuildLayout (totalRows);

        // Inset panel: slightly brighter than the card + hairline.
        dc->setFillColor (kSurface2);
        dc->drawRect (r, kDrawFilled);
        dc->setFrameColor (kOutline);
        dc->setLineWidth (1);
        dc->drawRect (r, kDrawStroked);

        // Style-palette strip (aligned to the probability band's columns):
        // a coloured line per style, salmon indicator under the selected one.
        const int drawingStyle = seq.selectedDrawingStyle;
        drawPalette (dc, r, drawingStyle);

        if (layout_.visibleRows <= 0 || layout_.totalCols <= 0)
        {
            drawEmptyHint (dc, gridRect (r));
            setDirty (false);
            return;
        }

        // ── Beat-bar column shading: every bar boundary (a whole bar's
        // worth of steps) gets a subtly brighter band + a divider line so
        // the grid reads in bars regardless of step resolution. ──
        const double stepsPerBarD = noteValueBeats (seq.stepResolutionIndex) > 0.0
                                        ? 4.0 / noteValueBeats (seq.stepResolutionIndex)
                                        : 4.0;
        const int stepsPerBar = std::max (1, static_cast<int> (std::lround (stepsPerBarD)));

        const CRect g = gridRect (r);

        for (int col = 0; col < layout_.totalCols; ++col)
        {
            const double x = r.left + static_cast<double> (col) * layout_.colWidth;
            if (col % stepsPerBar == 0)
            {
                if (col > 0)
                {
                    dc->setFrameColor (kOutline);
                    dc->setLineWidth (1);
                    dc->drawLine (CPoint (x, g.top), CPoint (x, g.bottom));
                }
            }
            else if ((col / std::max (1, stepsPerBar / 4)) % 2 == 0)
            {
                // Faint per-beat shading inside the bar.
                dc->setFillColor (mixColor (kSurface2, kSurface1, 0.4));
                dc->drawRect (CRect (x, g.top + 1, x + layout_.colWidth, g.bottom - 1),
                              kDrawFilled);
            }
        }

        if (loaded != nullptr)
            drawBars (dc, g, *loaded, seq);

        // ── Playhead column (bright vertical line on the current step) ──
        const int playingStep = owner->debugScheduler().playingStepIndex();
        if (playingStep >= 0 && playingStep < layout_.totalCols)
        {
            const double x = r.left + static_cast<double> (playingStep) * layout_.colWidth;
            dc->setFrameColor (kAccentBright);
            dc->setLineWidth (1);
            dc->drawLine (CPoint (x, g.top), CPoint (x, g.bottom));
        }

        // ── Scroll hint when rows are clipped ──
        if (layout_.scrolls)
        {
            const double trackH = g.getHeight();
            const double knobH = trackH * static_cast<double> (layout_.visibleRows)
                                 / static_cast<double> (layout_.totalRows);
            const double knobTop = g.top
                                   + (trackH - knobH)
                                         * static_cast<double> (scrollRows_)
                                         / static_cast<double> (layout_.maxScroll > 0
                                                                    ? layout_.maxScroll
                                                                    : 1);
            dc->setFillColor (kTextDisabled);
            dc->drawRect (CRect (g.right - 4, knobTop, g.right - 2, knobTop + std::max (8.0, knobH)),
                          kDrawFilled);
        }

        // ── In-place override slider overlay ──
        if (edit_.active)
            drawEditSlider (dc, r);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        const NeditProcessor* owner = editor_->owner();
        const auto& seq = owner->uiStateView().sequencer;
        auto loaded = owner->acquireLoadedSample();
        rebuildLayout (loaded != nullptr ? seq.rows : 0);

        // Click in the style-palette strip selects the drawing style (and
        // dismisses an open override slider without further edits).
        const CRect r = getViewSize();
        if (where.y >= r.top && where.y < r.top + kPaletteH)
        {
            finishEditSlider();
            const double colW = r.getWidth()
                                / static_cast<double> (state::kNumPlaybackStyles);
            const int style = static_cast<int> ((where.x - r.left) / colW);
            if (style >= 0 && style < static_cast<int> (state::kNumPlaybackStyles)
                && style != seq.selectedDrawingStyle)
            {
                editor_->owner()->setSelectedDrawingStyle (style);
                invalid();
            }
            return kMouseEventHandled;
        }

        if (layout_.visibleRows <= 0)
            return kMouseEventNotHandled;

        // An in-place override slider is live: a press inside the grid band
        // starts the drag (pointer-capture style) so the user can scrub a
        // value before releasing; the edit commits on release.
        if (edit_.active)
        {
            editDragging_ = true;
            editSliderFromX (where.x);
            return kMouseEventHandled;
        }

        const int col = columnFromX (layout_, where.x, r.left);
        const int row = rowFromBottomY (layout_, where.y, r.top + kPaletteH,
                                        r.bottom, scrollRows_);
        if (row < 0 || col < 0)
            return kMouseEventNotHandled;

        if (buttons.isShiftSet())
        {
            // Extension gesture: capture the grabbed cell + its base length.
            if (gridAt (row, col) >= 0)
            {
                grabRow_ = row;
                grabCol_ = col;
                lastDelta_ = 0;
                extending_ = true;
                return kMouseEventHandled;
            }
            return kMouseEventNotHandled;
        }

        // Paint or erase gesture. Toggle decided once at the press so the
        // drag stays consistent: painting the same style on the first cell
        // means we're erasing.
        const int first = gridAt (row, col);
        if (buttons.isRightButton())
        {
            // Right-click on an OCCUPIED cell opens the per-cell parameter
            // override menu (the original's popup gesture). An already-set
            // cell re-pops the menu; left-drag erase is the dedicated way to
            // clear a note.
            if (first >= 0)
            {
                openCellMenu (row, col, where);
                return kMouseEventHandled;
            }
            erase_ = true;
        }
        else if (buttons.isLeftButton())
        {
            erase_ = false;
            const int drawingStyle = owner->uiStateView().sequencer.selectedDrawingStyle;
            erase_ = (first == drawingStyle && first >= 0);
            paintStyle_ = drawingStyle;
        }
        else
        {
            return kMouseEventNotHandled;
        }

        dragging_ = true;
        applyPaintAt (row, col);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        const NeditProcessor* owner = editor_->owner();
        const auto& seq = owner->uiStateView().sequencer;
        auto loaded = owner->acquireLoadedSample();
        rebuildLayout (loaded != nullptr ? seq.rows : 0);

        const CRect r = getViewSize();

        if (edit_.active)
        {
            if (editDragging_)
                editSliderFromX (where.x);
            return kMouseEventHandled;
        }

        if (extending_)
        {
            const int col = columnFromX (layout_, where.x, r.left);
            if (col < 0)
                return kMouseEventHandled;
            const int delta = col - grabCol_;
            if (delta != lastDelta_)
            {
                if (editor_->owner()->setSequencerCellExtension (
                        grabRow_, grabCol_, delta - lastDelta_))
                    lastDelta_ = delta;
                invalid();
            }
            return kMouseEventHandled;
        }

        if (! dragging_)
            return kMouseEventNotHandled;

        const int col = columnFromX (layout_, where.x, r.left);
        const int row = rowFromBottomY (layout_, where.y, r.top + kPaletteH,
                                        r.bottom, scrollRows_);
        if (row < 0 || col < 0)
            return kMouseEventHandled;
        applyPaintAt (row, col);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseUp (CPoint& where, const CButtonState& buttons) override
    {
        onMouseMoved (where, buttons);
        if (edit_.active)
            finishEditSlider();
        editDragging_ = false;
        dragging_ = false;
        extending_ = false;
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseCancel () override
    {
        dragging_ = false;
        extending_ = false;
        editDragging_ = false;
        finishEditSlider();
        return kMouseEventHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        if (event.deltaY == 0.0)
            return;
        const auto& seq = editor_->owner()->uiStateView().sequencer;
        auto loaded = editor_->owner()->acquireLoadedSample();
        rebuildLayout (loaded != nullptr ? seq.rows : 0);
        if (! layout_.scrolls)
            return;

        const int steps = event.deltaY < 0.0 ? -1 : 1;
        setScroll (scrollRows_ + steps);
        event.consumed = true;
    }

private:
    [[nodiscard]] int gridAt (int row, int column) const noexcept
    {
        const auto& seq = editor_->owner()->uiStateView().sequencer;
        if (row < 0 || column < 0 || row >= seq.rows || column >= seq.columns)
            return -1;
        return seq.grid[static_cast<std::size_t> (row) * static_cast<std::size_t> (seq.columns)
                        + static_cast<std::size_t> (column)];
    }

    void rebuildLayout (int totalRows) noexcept
    {
        const CRect r = getViewSize();
        layout_ = computeSequencerLayout (r.getWidth(),
                                          r.getHeight() - kPaletteH,
                                          totalRows, rowsEffectiveColumns());
        scrollRows_ = clampSequencerScroll (layout_, scrollRows_);
    }

    // The grid's sub-rect: the view minus the palette strip at its top. All
    // row/bar/playhead geometry operates in this area.
    [[nodiscard]] CRect gridRect (const CRect& r) const noexcept
    {
        return CRect (r.left, r.top + kPaletteH, r.right, r.bottom);
    }

    void drawPalette (CDrawContext* dc, const CRect& r, int selected)
    {
        const int n = static_cast<int> (state::kNumPlaybackStyles);
        const double colW = r.getWidth() / static_cast<double> (n);

        for (int i = 0; i < n; ++i)
        {
            const double x0 = r.left + static_cast<double> (i) * colW;
            const double cx = x0 + colW * 0.5;

            // The "label": a short horizontal line in the style's colour.
            const CColor& base = kStyleColours[static_cast<std::size_t> (i)];
            dc->setFillColor (base);
            dc->drawRect (CRect (cx - colW * 0.15, r.top + 3.0,
                                 cx + colW * 0.15, r.top + 6.0),
                          kDrawFilled);

            // Salmon selection indicator under the active style.
            if (i == selected)
            {
                dc->setFillColor (kAccentMutedHi);
                dc->drawRect (CRect (x0 + 2.0, r.top + kPaletteH - 3.0,
                                     x0 + colW - 2.0, r.top + kPaletteH - 1.0),
                              kDrawFilled);
            }
        }
    }

    [[nodiscard]] int rowsEffectiveColumns() const noexcept
    {
        return editor_->owner()->uiStateView().sequencer.columns;
    }

    void applyPaintAt (int row, int column)
    {
        // The gesture's role (paint vs erase) is decided ONCE at the press
        // (see onMouseDown): erase_ means "clear these cells", otherwise we
        // write the selected style. Do NOT toggle per cell: the old per-cell
        // toggle, combined with onMouseUp re-applying at the release cell,
        // turned a single note back off on mouse-up. The processor's setter
        // already treats writing the same style over itself as a no-op, so
        // re-applying at the release cell is harmless here.
        NeditProcessor* owner = editor_->owner();
        if (owner->setSequencerCell (row, column, erase_ ? -1 : paintStyle_))
            invalid();
    }

    void drawEmptyHint (CDrawContext* dc, const CRect& r)
    {
        // Draw a placeholder lattice (a default bar of 16 steps x 4 rows) so
        // the Sequence tab clearly reads as a step grid even before a sample
        // exists, then a centred call-to-action over it.
        constexpr int kEmptyCols = 16;
        constexpr int kEmptyRows = 4;
        const double colW = r.getWidth() / static_cast<double> (kEmptyCols);
        const double rowH = r.getHeight() / static_cast<double> (kEmptyRows);

        dc->setLineWidth (1);
        for (int c = 1; c < kEmptyCols; ++c)
        {
            const double x = r.left + static_cast<double> (c) * colW;
            dc->setFrameColor (c % 4 == 0 ? kOutline : mixColor (kOutline, kSurface2, 0.5));
            dc->drawLine (CPoint (x, r.top), CPoint (x, r.bottom));
        }
        dc->setFrameColor (mixColor (kOutline, kSurface2, 0.5));
        for (int rr = 1; rr < kEmptyRows; ++rr)
        {
            const double y = r.top + static_cast<double> (rr) * rowH;
            dc->drawLine (CPoint (r.left, y), CPoint (r.right, y));
        }

        dc->setFont (kNormalFontSmall);
        dc->setFontColor (kTextSecondary);
        dc->drawString ("SEQUENCER — load a sample to populate the step grid",
                        CRect (r.left, r.top + r.getHeight() * 0.5 - 8,
                               r.right, r.top + r.getHeight() * 0.5 + 8),
                        kCenterText);
    }

    void drawBars (CDrawContext* dc, const CRect& r, const LoadedSample& loaded,
                   const state::SequencerState& seq)
    {
        const double originalBpm = engine::tempo::calculatedOriginalBpm (
            editor_->owner()->uiStateView().sample);

        // Visible row bands, bottom-up; band b shows slice scrollRows_ + b.
        for (int b = 0; b < layout_.visibleRows; ++b)
        {
            const int row = scrollRows_ + b;
            if (row >= layout_.totalRows || row >= static_cast<int> (loaded.slices.size()))
                break;

            const double rowBottom = r.bottom - static_cast<double> (b) * layout_.rowHeight;
            const double rowTop = rowBottom - layout_.rowHeight;

            // Row separator.
            dc->setFrameColor (kOutline);
            dc->setLineWidth (1);
            dc->drawLine (CPoint (r.left, rowBottom), CPoint (r.right, rowBottom));

            // Scan this row's cells directly (monophonic rows, one style).
            // Use the SampleState's rate + tempo (exactly what the engine's
            // Sequenced scheduler reads) so the bars match what plays.
            const std::int64_t sliceFrames = loaded.slices[static_cast<std::size_t> (row)].lengthFrames();
            const double sampleRate = editor_->owner()->uiStateView().sample.sampleSampleRate;

            for (int col = 0; col < layout_.totalCols; ++col)
            {
                const int style = gridAt (row, col);
                if (style < 0)
                    continue;

                const auto cell = static_cast<std::uint32_t> (row)
                                * static_cast<std::uint32_t> (layout_.totalCols)
                                + static_cast<std::uint32_t> (col);

                const double stepBeats = noteValueBeats (seq.stepResolutionIndex);
                int declared = naturalStepsForSlice (sliceFrames, sampleRate, originalBpm,
                                                     stepBeats);
                if (const auto it = seq.extensions.find (cell); it != seq.extensions.end())
                    declared = std::max (declared, static_cast<int> (it->second));

                int end = std::min (layout_.totalCols, col + declared);
                for (int c = col + 1; c < end; ++c)
                {
                    if (anyColumnActive (c))
                    {
                        end = c;
                        break;
                    }
                }

                drawBar (dc, r, rowTop, rowBottom, col, end, style);
                drawMarkers (dc, r, rowTop, col, end, style, seq, cell);
            }
        }
    }

    [[nodiscard]] bool anyColumnActive (int column) const noexcept
    {
        if (column >= layout_.totalCols)
            return false;
        for (int rr = 0; rr < layout_.totalRows; ++rr)
        {
            if (gridAt (rr, column) >= 0)
                return true;
        }
        return false;
    }

    void drawBar (CDrawContext* dc, const CRect& r, double rowTop, double rowBottom,
                  int col, int end, int style)
    {
        const double left = r.left + static_cast<double> (col) * layout_.colWidth;
        const double width = static_cast<double> (end - col) * layout_.colWidth - 1.0;
        const double inset = 1.0;
        const CColor& base = kStyleColours[static_cast<std::size_t> (style)];
        const CColor fill = mixColor (base, kSurface2, 0.12);   // tone it toward the panel

        dc->setFillColor (fill);
        dc->drawRect (CRect (left, rowTop + inset, left + std::max (0.0, width),
                             rowBottom - inset),
                      kDrawFilled);
        dc->setFrameColor (base);
        dc->setLineWidth (1);
        dc->drawRect (CRect (left, rowTop + inset, left + std::max (0.0, width),
                             rowBottom - inset),
                      kDrawStroked);
    }

    void drawMarkers (CDrawContext* dc, const CRect& r, double rowTop,
                      int col, int end, int style, const state::SequencerState& seq,
                      std::uint32_t cell)
    {
        const double left = r.left + static_cast<double> (col) * layout_.colWidth;
        const double top = rowTop + 2.0;
        constexpr double size = 4.0;

        // Extended tail (declared > natural) renders brighter than the head.
        const auto& base = kStyleColours[static_cast<std::size_t> (style)];

        // Per-cell override marker: a small accent triangle in the top-right.
        if (const auto it = seq.overrides.find (cell); it != seq.overrides.end())
        {
            (void) it;
            const double cx = left + static_cast<double> (end - col) * layout_.colWidth - 8.0;
            dc->setFillColor (kAccentMutedHi);
            drawTriangle (dc, CPoint (cx, top), CPoint (cx + size - 1, top),
                          CPoint (cx, top + size - 1));
        }

        // Extension marker: brighten the bar start if this cell is extended.
        if (const auto it = seq.extensions.find (cell); it != seq.extensions.end())
        {
            (void) it;
            dc->setFillColor (base);
            dc->drawRect (CRect (left + 1.0, top, left + 4.0, top + 3.0), kDrawFilled);
        }
    }

    void drawTriangle (CDrawContext* dc, const CPoint& a, const CPoint& b, const CPoint& c)
    {
        auto* path = dc->createGraphicsPath();
        if (path == nullptr)
            return;
        path->beginSubpath (a);
        path->addLine (b);
        path->addLine (c);
        path->closeSubpath();
        dc->setDrawMode (kAntiAliasing);
        dc->drawGraphicsPath (path, CDrawContext::kPathFilled);
        path->forget();
        dc->setDrawMode (kAliasing);
    }

    // ── Per-cell parameter override: context menu + in-place value slider ──
    // Width of the in-place drag slider (centred on the step's column).
    static constexpr double kEditSliderW = 120.0;

    // The live state of the in-place slider while a value is being scrubbed.
    struct OverrideEdit
    {
        bool active = false;
        int row = 0;
        int col = 0;
        state::StyleParamId id {};
        float value = 0.0f;      // current stored value / option index
        double minValue = 0.0;   // slider mapping space
        double maxValue = 1.0;
        double left = 0.0;       // slider x-extent (view-space)
        double right = 0.0;
    };

    // The current override value for a cell param, falling back to the
    // parameter's global default when the cell has no override.
    [[nodiscard]] float currentCellValue (int row, int col, state::StyleParamId id) const noexcept
    {
        const auto& seq = editor_->owner()->uiStateView().sequencer;
        const auto flat = static_cast<std::uint32_t> (row)
                        * static_cast<std::uint32_t> (seq.columns)
                        + static_cast<std::uint32_t> (col);
        if (const auto cit = seq.overrides.find (flat); cit != seq.overrides.end())
            if (const auto it = cit->second.find (id); it != cit->second.end())
                return it->second;
        return state::styleParamInfo (id).defaultValue;
    }

    // Centred on the step's column, clamped inside the view.
    void editSliderBounds (int col, const CRect& r, double& left, double& right) const noexcept
    {
        const double center = r.left + (static_cast<double> (col) + 0.5) * layout_.colWidth;
        left  = std::max (r.left + 2.0, center - kEditSliderW * 0.5);
        right = std::min (r.right - 2.0, center + kEditSliderW * 0.5);
    }

    void beginEditSlider (int row, int col, state::StyleParamId id)
    {
        const auto& info = state::styleParamInfo (id);
        if (! info.discrete && info.minValue == info.maxValue)
            return;
        edit_.active = true;
        edit_.row = row;
        edit_.col = col;
        edit_.id = id;
        edit_.value = currentCellValue (row, col, id);
        edit_.minValue = info.discrete ? 0.0 : static_cast<double> (info.minValue);
        edit_.maxValue = info.discrete ? static_cast<double> (info.numOptions - 1)
                                       : static_cast<double> (info.maxValue);
        const CRect r = getViewSize();
        editSliderBounds (col, r, edit_.left, edit_.right);
        invalid();
    }

    void editSliderFromX (double x) noexcept
    {
        if (! edit_.active)
            return;
        const double span = std::max (1.0, edit_.right - edit_.left);
        const double t = state::clampValue ((x - edit_.left) / span, 0.0, 1.0);
        const double v = edit_.minValue + t * (edit_.maxValue - edit_.minValue);
        edit_.value = static_cast<float> (v);
        (void) editor_->owner()->setSequencerCellOverride (
            edit_.row, edit_.col, edit_.id, edit_.value);
        invalid();
    }

    void finishEditSlider() noexcept
    {
        if (edit_.active)
        {
            edit_.active = false;
            invalid();
        }
    }

    // `name: value` label for the in-place slider (option name for discrete
    // params, a formatted raw value for continuous ones).
    [[nodiscard]] std::string editSliderLabel() const
    {
        const auto& info = state::styleParamInfo (edit_.id);
        if (info.discrete)
        {
            const int idx = static_cast<int> (std::lround (edit_.value));
            const char* name = state::styleParamOptionName (edit_.id, idx);
            return std::string (info.name) + ": "
                 + (name != nullptr ? name : "-");
        }
        char buf[64];
        std::snprintf (buf, sizeof (buf), paramReadoutFormat (edit_.id), edit_.value);
        return std::string (info.name) + ": " + buf;
    }

    void drawEditSlider (CDrawContext* dc, const CRect& r)
    {
        if (edit_.minValue == edit_.maxValue)
            return;
        const double rowIndex = static_cast<double> (edit_.row - scrollRows_);
        const double rowBottom = r.bottom - rowIndex * layout_.rowHeight;
        const double rowTop = rowBottom - layout_.rowHeight;

        // A compact box centred on the step's column at that row.
        const double boxW = edit_.right - edit_.left;
        const double boxX = edit_.left;
        const double boxTop = rowTop + 1.0;
        const double boxBottom = rowBottom - 1.0;

        // Tint the box toward the panel so it reads as an overlay, not a cell.
        dc->setFillColor (mixColor (kAccent, kSurface2, 0.35));
        dc->drawRect (CRect (boxX, boxTop, boxX + boxW, boxBottom), kDrawFilled);
        dc->setFrameColor (kAccentBright);
        dc->setLineWidth (1);
        dc->drawRect (CRect (boxX, boxTop, boxX + boxW, boxBottom), kDrawStroked);

        // Fill fraction = t of the current value.
        const double t = (edit_.value - edit_.minValue)
                         / std::max (1e-9, edit_.maxValue - edit_.minValue);
        const double fillX = boxX + static_cast<double> (t) * boxW;
        dc->setFillColor (kAccentBright);
        dc->drawRect (CRect (boxX, boxTop, std::max (boxX, fillX), boxBottom), kDrawFilled);

        // `name: value` label, clipped inside the box.
        const std::string label = editSliderLabel();
        dc->setFont (kNormalFontSmall);
        dc->setFontColor (kTextPrimary);
        dc->drawString (label.c_str(),
                        CRect (boxX + 4.0, boxTop, boxX + boxW - 4.0, boxBottom),
                        kLeftText, true);
    }

    // The right-click context menu for an occupied cell: one entry per
    // applicable style parameter (Subdivide + Volume included always).
    // Continuous / stepped entries open the in-place slider; discrete
    // params present a submenu of option names that writes the override
    // directly; swept params present a submenu of their *Mode choices first
    // and then open the value slider.
    void openCellMenu (int row, int col, const CPoint& where)
    {
        const int style = gridAt (row, col);
        if (style < 0)
            return;
        const auto styleEnum = static_cast<state::PlaybackStyle> (style);
        const auto entries = nedit::ui::cellOverrideMenuEntries (styleEnum);

        // All menus (top + submenus) live in menuMenus_ so the CMenuItems'
        // raw submenu pointers stay valid until the popup finishes; the
        // callback releases them. CMenuItem::setSubmenu does NOT retain its
        // submenu, so a plain new/forget would dangle.
        auto* top = new COptionMenu (CRect (0, 0, 0, 0), nullptr, -1);
        menuMenus_.clear();
        menuMenus_.emplace_back (top);

        for (const auto& e : entries)
        {
            const auto& info = state::styleParamInfo (e.id);
            const UTF8String title = static_cast<UTF8String> (info.name);

            if (e.kind == nedit::ui::CellOverrideMenuKind::slider)
            {
                auto* item = new CCommandMenuItem (CCommandMenuItem::Desc (title, -1));
                item->setActions (
                    [this, row, col, id = e.id] (CCommandMenuItem*) {
                        beginEditSlider (row, col, id);
                    });
                top->addEntry (item);
                continue;
            }

            // submenu or modeSubmenu: a submenu of option names.
            const auto pid = e.kind == nedit::ui::CellOverrideMenuKind::modeSubmenu ? e.modeId : e.id;
            const int optCount = std::max (1, state::styleParamInfo (pid).numOptions);
            auto* sub = new COptionMenu (CRect (0, 0, 0, 0), nullptr, -1);
            menuMenus_.emplace_back (sub);
            for (int o = 0; o < optCount; ++o)
            {
                const char* oname = state::styleParamOptionName (pid, o);
                auto* leaf = new CCommandMenuItem (
                    CCommandMenuItem::Desc (UTF8String (oname != nullptr ? oname : ""), -1));
                leaf->setActions (
                    [this, row, col, e, o] (CCommandMenuItem*) {
                        if (e.kind == nedit::ui::CellOverrideMenuKind::modeSubmenu)
                        {
                            // Write the mode override, then ask for the value.
                            (void) editor_->owner()->setSequencerCellOverride (
                                row, col, e.modeId, static_cast<float> (o));
                            beginEditSlider (row, col, e.id);
                        }
                        else
                        {
                            (void) editor_->owner()->setSequencerCellOverride (
                                row, col, e.id, static_cast<float> (o));
                        }
                    });
                sub->addEntry (leaf);
            }
            top->addEntry (sub, title);
        }

        top->setStyle (COptionMenu::kPopupStyle);
        top->popup (getFrame(), where, [this] (COptionMenu*) {
            menuMenus_.clear ();
        });
    }

    NeditEditor* editor_ = nullptr;
    SequencerGridLayout layout_;   // cached for draw/hit-testing
    int scrollRows_ = 0;               // scroll offset (rows)
    bool dragging_ = false;
    bool erase_ = false;
    int paintStyle_ = 0;
    bool extending_ = false;
    int grabRow_ = 0;
    int grabCol_ = 0;
    int lastDelta_ = 0;
    OverrideEdit edit_;                         // in-place override slider
    bool editDragging_ = false;                 // slider drag in progress
    // Owns the live right-click menu tree (top + submenus) until its popup
    // callback fires. CMenuItems hold raw submenu pointers that must stay
    // valid through display.
    std::vector<VSTGUI::SharedPointer<COptionMenu>> menuMenus_;
};

// ── Performance-page tab strip (GENERATE / SEQUENCER / CONTROL / ───────
// PERFORMANCE). 48px tall per spec deviation in AGENTS. Container-less
// underline tabs (design-language.md §6): active = salmon label + 2px
// salmon indicator, inactive = text-secondary, hover = subtle surface-
// 2 pill. The value is the selected tab ordinal normalized over the range
// [0, 3]; tag kTagTabBar routes clicks through valueChanged.
class TabBar : public CControl
{
public:
    static constexpr int kTabCount = 4;

    TabBar (const CRect& size, IControlListener* l, int32_t t)
        : CControl (size, l, t)
    {
        setValueNormalized (0.0f);
    }

    CBaseObject* newCopy () const override
    {
        return new TabBar (getViewSize(), getListener(), getTag());
    }

    [[nodiscard]] static const char* labelForOrdinal (int ordinal)
    {
        static const char* const kLabels[kTabCount] = {
            "GENERATE", "SEQUENCER", "CONTROL", "PERFORMANCE"
        };
        return kLabels[ordinal < 0 ? 0 : (ordinal > kTabCount - 1 ? kTabCount - 1 : ordinal)];
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        dc->setDrawMode (kAliasing);

        const int selected = ordinalFromValue (getValueNormalized());

        for (int i = 0; i < kTabCount; ++i)
        {
            const CRect seg = segmentForTab (r, i);

            // Hover surface (subtle lighter pill on the base).
            if (i == hoveredTab_)
            {
                const double pad = 8.0;
                drawRoundedRect (dc, CRect (seg.left + 4, seg.top + pad,
                                            seg.right - 4, seg.bottom - pad),
                                 CCoord (4), kSurface2, CColor (0, 0, 0, 0));
            }

            dc->setFont (kNormalFontSmall);
            const bool active = (i == selected);
            dc->setFontColor (active ? kAccent : kTextSecondary);
            dc->drawString (labelForOrdinal (i), seg, kCenterText);
        }

        // 2px salmon underline indicator on the selected tab.
        const CRect sel = segmentForTab (r, selected);
        const CCoord indH = 2.0;
        dc->setFillColor (kAccent);
        dc->drawRect (CRect (sel.left, sel.bottom - indH, sel.right, sel.bottom), kDrawFilled);

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! buttons.isLeftButton())
            return kMouseEventNotHandled;
        selectTabFromPoint (where);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        const int hovered = tabFromPoint (where);
        if (hovered != hoveredTab_)
        {
            hoveredTab_ = hovered;
            invalid();
        }
        return kMouseEventNotHandled;
    }

    CMouseEventResult onMouseEntered (CPoint& where, const CButtonState&) override
    {
        hoveredTab_ = tabFromPoint (where);
        invalid();
        return kMouseEventNotHandled;
    }

    CMouseEventResult onMouseExited (CPoint&, const CButtonState&) override
    {
        hoveredTab_ = -1;
        invalid();
        return kMouseEventNotHandled;
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        event.consumed = true;
    }

private:
    [[nodiscard]] static CRect segmentForTab (const CRect& bar, int index)
    {
        // Equal-width segments spanning the full strip — the click target
        // and the underline indicator fill the entire bar.
        const double segW = bar.getWidth() / static_cast<double> (kTabCount);
        const double left = bar.left + static_cast<double> (index) * segW;
        return CRect (left, bar.top, left + segW, bar.bottom);
    }

    [[nodiscard]] static int ordinalFromValue (float normalized)
    {
        const int ord = static_cast<int> (std::lround (normalized * static_cast<float> (kTabCount - 1)));
        return ord < 0 ? 0 : (ord > kTabCount - 1 ? kTabCount - 1 : ord);
    }

    [[nodiscard]] int tabFromPoint (const CPoint& where) const
    {
        const CRect r = getViewSize();
        for (int i = 0; i < kTabCount; ++i)
            if (segmentForTab (r, i).pointInside (where))
                return i;
        return -1;
    }

    void selectTabFromPoint (const CPoint& where)
    {
        const int tab = tabFromPoint (where);
        if (tab < 0 || tab == ordinalFromValue (getValueNormalized()))
            return;
        setValueNormalized (static_cast<float> (tab) / static_cast<float> (kTabCount - 1));
        invalid();
        valueChanged();
    }

    int hoveredTab_ = -1;
};

// ── Panel area below the tab bar. One card-set per performance page      ──
// (§6: tabs swap only the panel area; cross-tab persistence comes from
// UiState.activeTab). Stateless renderer over the processor's live state;
// each page currently shows a placeholder skeleton that the per-mode
// panel builds (style params, sequencer grid, ...) will replace.
class PanelView : public CView
{
public:
    explicit PanelView (NeditProcessor* owner) : CView (CRect (0, 0, 0, 0)), owner_ (owner) {}

    // Whether the currently selected page uses the style-probability band.
    [[nodiscard]] bool showsStyleProbs () const noexcept
    {
        return showsStyleProbsFor (owner_->uiStateView().ui.activeTab);
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();

        // Card: graphite-800 fill, hairline outline, radius-s (4) per the
        // panel containers in design-language.md.
        drawRoundedRect (dc, r, CCoord (4), kSurface1, kOutline);

        const auto tab = owner_->uiStateView().ui.activeTab;

        if (showsStyleProbsFor (tab))
        {
            // The Generate/Sequence band is drawn by the 9 sibling
            // StyleProbSlider columns layered over this card; here we only
            // mark the band's bottom edge and hint at what sits below
            // (the Generate timing section has no hint -- it fills the
            // space with the IntervalProbBand wrapper just above this text
            // ... but that band lives in the SAME card, so the Generate
            // page's hint must be empty; the Sequencer hint stays).
            const double bandBottom = r.top + kCardPad + kStyleBandH;
            dc->setDrawMode (kAliasing);
            dc->setFrameColor (kOutline);
            dc->setLineWidth (1);
            dc->drawLine (CPoint (r.left + kCardPad, bandBottom),
                          CPoint (r.right - kCardPad, bandBottom));

            const char* hint = hintBelow (tab);
            if (hint[0] != '\0')
            {
                dc->setFont (kNormalFontSmaller);
                dc->setFontColor (kTextDisabled);
                dc->drawString (hint,
                                CRect (r.left + kCardPad, r.bottom - 18,
                                       r.right - kCardPad, r.bottom - 8),
                                kLeftText);
            }
        }
        else
        {
            // Skeleton placeholder for the remaining pages.
            const char* name = TabBar::labelForOrdinal (static_cast<int> (tab));
            dc->setFont (kNormalFontSmall);
            dc->setFontColor (kTextSecondary);
            dc->drawString (name, CRect (r.left + 12, r.top + 10, r.right - 12, r.top + 22),
                            kLeftText);
            dc->setFont (kNormalFontSmaller);
            dc->setFontColor (kTextDisabled);
            dc->drawString (hintForTab (tab),
                            CRect (r.left + 12, r.top + 26, r.right - 12, r.top + 38),
                            kLeftText);
        }

        setDirty (false);
    }

private:
    [[nodiscard]] static bool showsStyleProbsFor (state::UiTab tab) noexcept
    {
        return tab == state::UiTab::generate || tab == state::UiTab::sequence;
    }

    [[nodiscard]] static const char* hintBelow (state::UiTab tab)
    {
        return tab == state::UiTab::generate
                   ? ""   // the timing section fills the space below the band
                   : "";  // the step grid fills the space below the band
    }

    [[nodiscard]] static const char* hintForTab (state::UiTab tab)
    {
        switch (tab)
        {
            case state::UiTab::generate: return "style weights · style parameters · probabilities";
            case state::UiTab::sequence: return "step grid · pattern controls";
            case state::UiTab::control:  return "keyswitch mapping · gate · velocity";
            case state::UiTab::perform:  return "slot bank · snapshots · quantized recall";
        }
        return "";
    }

    NeditProcessor* owner_ = nullptr;
};

}; // namespace ui

//------------------------------------------------------------------------
NeditEditor::NeditEditor (NeditProcessor* owner, Steinberg::ViewRect* size)
    : VSTGUIEditor (owner, size != nullptr ? size : &kEditorRect), owner_ (owner)
{
    // Force the first interval-band repaint on the next sync even if the
    // restored state equals the zero-filled default (weights are valid
    // 0..1 values, so a sentinel that no real value can collide with is
    // needed to make "never synced" distinguishable from "already synced
    // to 0").
    lastSubdivWeightsSync_.fill (-1.0f);
}

//------------------------------------------------------------------------
// Apply the current audition state + sample presence to the button's
// visual appearance. Called after every toggle and on idle-timer sync.
void NeditEditor::styleAuditionButton()
{
    if (! auditionBtn_)
        return;

    const bool hasSample = owner_->hasSample();
    const bool auditionOn = owner_->uiStateView().ui.auditionEnabled;

    // Button is clickable only when a sample is loaded.
    auditionBtn_->setMouseEnabled (hasSample);

    if (auditionOn)
    {
        // Salmon filled — audition is ON.
        GradientColorStopMap stops;
        stops.emplace (0., kAccent);
        stops.emplace (1., kAccent);
        auditionBtn_->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kAccentPressed);
        hlStops.emplace (1., kAccentPressed);
        auditionBtn_->setGradientHighlighted (CGradient::create (hlStops));
        auditionBtn_->setTextColor (kAccentOn);
        auditionBtn_->setTextColorHighlighted (kAccentOn);
        auditionBtn_->setFrameColor (kAccent);
        auditionBtn_->setFrameColorHighlighted (kAccentPressed);
        auditionBtn_->setRoundRadius (4);
        auditionBtn_->setTitle ("SILENCE");
    }
    else
    {
        // Graphite outlined — audition is OFF.
        GradientColorStopMap stops;
        stops.emplace (0., kSurface2);
        stops.emplace (1., kSurface2);
        auditionBtn_->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kSurface3);
        hlStops.emplace (1., kSurface3);
        auditionBtn_->setGradientHighlighted (CGradient::create (hlStops));
        auditionBtn_->setTextColor (hasSample ? kTextSecondary : kTextDisabled);
        auditionBtn_->setTextColorHighlighted (kTextPrimary);
        auditionBtn_->setFrameColor (kOutline);
        auditionBtn_->setFrameColorHighlighted (hasSample ? kOutline : kTextDisabled);
        auditionBtn_->setRoundRadius (2);
        auditionBtn_->setTitle ("AUDITION");
    }
}

//------------------------------------------------------------------------
// Apply the manual-BPM override state to the toolbar toggle's visual
// appearance (salmon filled when ON, graphite outlined when OFF).
void NeditEditor::styleOverrideButton()
{
    if (! overrideBtn_)
        return;

    const bool on = owner_->uiStateView().sample.manualBpmOverrideEnabled;

    if (on)
    {
        GradientColorStopMap stops;
        stops.emplace (0., kAccent);
        stops.emplace (1., kAccent);
        overrideBtn_->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kAccentPressed);
        hlStops.emplace (1., kAccentPressed);
        overrideBtn_->setGradientHighlighted (CGradient::create (hlStops));
        overrideBtn_->setTextColor (kAccentOn);
        overrideBtn_->setTextColorHighlighted (kAccentOn);
        overrideBtn_->setFrameColor (kAccent);
        overrideBtn_->setFrameColorHighlighted (kAccentPressed);
    }
    else
    {
        GradientColorStopMap stops;
        stops.emplace (0., kSurface2);
        stops.emplace (1., kSurface2);
        overrideBtn_->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kSurface3);
        hlStops.emplace (1., kSurface3);
        overrideBtn_->setGradientHighlighted (CGradient::create (hlStops));
        overrideBtn_->setTextColor (kTextSecondary);
        overrideBtn_->setTextColorHighlighted (kTextPrimary);
        overrideBtn_->setFrameColor (kOutline);
        overrideBtn_->setFrameColorHighlighted (kOutline);
    }
    overrideBtn_->setRoundRadius (2);
}

//------------------------------------------------------------------------
// Apply the auto-transient grid-quantize state to the toolbar toggle's
// visual appearance (salmon filled when ON, graphite outlined when OFF,
// disabled without a sample).
void NeditEditor::styleQuantizeButton()
{
    if (! quantizeBtn_)
        return;

    const bool on = owner_->uiStateView().sample.quantizeTransients;
    const bool hasSample = owner_->hasSample();
    quantizeBtn_->setMouseEnabled (hasSample);

    if (on)
    {
        GradientColorStopMap stops;
        stops.emplace (0., kAccent);
        stops.emplace (1., kAccent);
        quantizeBtn_->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kAccentPressed);
        hlStops.emplace (1., kAccentPressed);
        quantizeBtn_->setGradientHighlighted (CGradient::create (hlStops));
        quantizeBtn_->setTextColor (kAccentOn);
        quantizeBtn_->setTextColorHighlighted (kAccentOn);
        quantizeBtn_->setFrameColor (kAccent);
        quantizeBtn_->setFrameColorHighlighted (kAccentPressed);
    }
    else
    {
        GradientColorStopMap stops;
        stops.emplace (0., kSurface2);
        stops.emplace (1., kSurface2);
        quantizeBtn_->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kSurface3);
        hlStops.emplace (1., kSurface3);
        quantizeBtn_->setGradientHighlighted (CGradient::create (hlStops));
        quantizeBtn_->setTextColor (hasSample ? kTextSecondary : kTextDisabled);
        quantizeBtn_->setTextColorHighlighted (kTextPrimary);
        quantizeBtn_->setFrameColor (kOutline);
        quantizeBtn_->setFrameColorHighlighted (kOutline);
    }
    quantizeBtn_->setRoundRadius (2);
}

//------------------------------------------------------------------------
// Apply the render pitch-mode state to the toolbar toggle's appearance:
// salmon filled when time-stretching (granular, pitch preserved), graphite
// outlined when repitching.
void NeditEditor::stylePitchButton()
{
    if (! pitchBtn_)
        return;

    const bool stretch = owner_->uiStateView().render.pitchMode
                         == state::PitchMode::timeStretch;

    if (stretch)
    {
        GradientColorStopMap stops;
        stops.emplace (0., kAccent);
        stops.emplace (1., kAccent);
        pitchBtn_->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kAccentPressed);
        hlStops.emplace (1., kAccentPressed);
        pitchBtn_->setGradientHighlighted (CGradient::create (hlStops));
        pitchBtn_->setTextColor (kAccentOn);
        pitchBtn_->setTextColorHighlighted (kAccentOn);
        pitchBtn_->setFrameColor (kAccent);
        pitchBtn_->setFrameColorHighlighted (kAccentPressed);
    }
    else
    {
        GradientColorStopMap stops;
        stops.emplace (0., kSurface2);
        stops.emplace (1., kSurface2);
        pitchBtn_->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kSurface3);
        hlStops.emplace (1., kSurface3);
        pitchBtn_->setGradientHighlighted (CGradient::create (hlStops));
        pitchBtn_->setTextColor (kTextSecondary);
        pitchBtn_->setTextColorHighlighted (kTextPrimary);
        pitchBtn_->setFrameColor (kOutline);
        pitchBtn_->setFrameColorHighlighted (kOutline);
    }
    pitchBtn_->setRoundRadius (2);
    // The caption always shows the mode that will apply AFTER the next
    // press -- the model state is authoritative, never the button value.
    pitchBtn_->setTitle (stretch ? "TIMESTRETCH" : "REPITCH");
}

//------------------------------------------------------------------------
void NeditEditor::styleModeSegment (CTextButton* button, bool active)
{
    if (button == nullptr)
        return;

    if (active)
    {
        GradientColorStopMap stops;
        stops.emplace (0., kAccent);
        stops.emplace (1., kAccent);
        button->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kAccentPressed);
        hlStops.emplace (1., kAccentPressed);
        button->setGradientHighlighted (CGradient::create (hlStops));
        button->setTextColor (kAccentOn);
        button->setTextColorHighlighted (kAccentOn);
        button->setFrameColor (kAccent);
        button->setFrameColorHighlighted (kAccentPressed);
    }
    else
    {
        GradientColorStopMap stops;
        stops.emplace (0., kSurface2);
        stops.emplace (1., kSurface2);
        button->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kSurface3);
        hlStops.emplace (1., kSurface3);
        button->setGradientHighlighted (CGradient::create (hlStops));
        button->setTextColor (kTextSecondary);
        button->setTextColorHighlighted (kTextPrimary);
        button->setFrameColor (kOutline);
        button->setFrameColorHighlighted (kOutline);
    }
    button->setRoundRadius (2);
}

//------------------------------------------------------------------------
// The Slice-Length/Clock mode switch: the segment matching
// GenerateState.generateMode is accent-filled, the other quiet. The title
// on each is fixed (the model state decides what's active, never the
// pressed button value).
void NeditEditor::styleModeButtons()
{
    const auto mode = owner_->uiStateView().generate.generateMode;
    styleModeSegment (modeSlBtn_, mode == state::TriggerMode::sliceLength);
    styleModeSegment (modeClockBtn_, mode == state::TriggerMode::clock);
}

//------------------------------------------------------------------------
// The subdivision quick-clear chips ("n=0" / "nd=0" / "nt=0") ride the
// interval band's caption row. They act on CLOCK-mode-only weights, so
// they are active exactly when Clock is: under Slice Length they dim to
// disabled text and reject input (same contract as the band + menus).
void NeditEditor::styleZeroChips()
{
    const bool enabled = owner_->uiStateView().generate.generateMode
                         == state::TriggerMode::clock;
    for (auto* chip : { zeroPlainBtn_, zeroDottedBtn_, zeroTripletBtn_ })
    {
        if (chip == nullptr)
            continue;
        chip->setMouseEnabled (enabled);
        GradientColorStopMap stops;
        stops.emplace (0., enabled ? kSurface2 : kSurface1);
        stops.emplace (1., enabled ? kSurface2 : kSurface1);
        chip->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., enabled ? kSurface3 : kSurface1);
        hlStops.emplace (1., enabled ? kSurface3 : kSurface1);
        chip->setGradientHighlighted (CGradient::create (hlStops));
        chip->setTextColor (enabled ? kAccentMutedHi : kTextDisabled);
        chip->setTextColorHighlighted (enabled ? kAccentBright : kTextDisabled);
        chip->setFrameColor (enabled ? kOutline : kSurface1);
        chip->setFrameColorHighlighted (enabled ? kAccent : kSurface1);
        chip->setRoundRadius (2);
    }
}

//------------------------------------------------------------------------
// Grain size/speed sliders are only meaningful while rendering with the
// granular time-stretch engine; otherwise they are dimmed and inert.
void NeditEditor::setGrainEnabled (bool enabled)
{
    if (grainSizeSlider_ != nullptr)
        grainSizeSlider_->setActive (enabled);
    if (grainSpeedSlider_ != nullptr)
        grainSpeedSlider_->setActive (enabled);
}

//------------------------------------------------------------------------
// Push the live model state into the toolbar controls. Runs on the idle
// timer and after local edits so host automation / state loads / box-tool
// changes all surface. Only pushes on change; the controls repaint lazily.
void NeditEditor::syncToolBarControls()
{
    if (! barsStepper_ || ! bpmField_ || ! quantizeMenu_ || ! fadeInSlider_
        || ! fadeOutSlider_ || ! pitchBtn_ || ! grainSizeSlider_ || ! grainSpeedSlider_)
        return;

    const auto& state = owner_->uiStateView();
    const auto& sample = state.sample;

    const int bars = sample.loopLengthBars < 1 ? 1 : sample.loopLengthBars;
    if (bars != lastBarsSync_)
    {
        lastBarsSync_ = bars;
        barsStepper_->setValueNormalized (static_cast<float> (bars - 1)
                                          / static_cast<float> (kBarsMax - 1));
        barsStepper_->invalid();
    }

    const float bpmNorm = static_cast<float> (
        (sample.manualBpmOverrideValue - kBpmMin) / (kBpmMax - kBpmMin));
    if (bpmNorm != lastBpmNormSync_)
    {
        lastBpmNormSync_ = bpmNorm;
        bpmField_->setValueNormalized (bpmNorm);
        bpmField_->invalid();
    }

    const bool overrideOn = sample.manualBpmOverrideEnabled;
    const double calcBpm = engine::tempo::calculatedOriginalBpm (sample);
    if (overrideOn != lastOverrideSync_ || calcBpm != lastCalcBpmSync_)
    {
        lastOverrideSync_ = overrideOn;
        lastCalcBpmSync_ = calcBpm;
        bpmField_->setMode (overrideOn, calcBpm);
    }
    if (overrideOn != lastOverrideStyling_)
    {
        lastOverrideStyling_ = overrideOn;
        styleOverrideButton();
    }

    const bool hasSample = owner_->hasSample();

    const float sens = sample.sensitivity;
    if (sens != lastSensitivitySync_ || hasSample != lastSensActive_)
    {
        lastSensitivitySync_ = sens;
        lastSensActive_ = hasSample;
        sensSlider_->setValueNormalized (sens);
        sensSlider_->setActive (hasSample);
        sensSlider_->invalid();
    }

    const bool quantOn = sample.quantizeTransients;
    if (quantOn != lastQuantizeSync_ || hasSample != lastQuantizeActive_)
    {
        lastQuantizeSync_ = quantOn;
        lastQuantizeActive_ = hasSample;
        styleQuantizeButton();
    }

    const int grid = sample.quantizeGridIndex;
    if (grid != lastGridSync_ || hasSample != lastGridActive_)
    {
        lastGridSync_ = grid;
        lastGridActive_ = hasSample;
        quantizeMenu_->setValue (static_cast<float> (grid));
        quantizeMenu_->setMouseEnabled (hasSample);
        quantizeMenu_->invalid();
    }

    const float fadeInMs = state.render.fadeInMs;
    if (fadeInMs != lastFadeInSync_)
    {
        lastFadeInSync_ = fadeInMs;
        fadeInSlider_->setValueNormalized (fadeInMs / 10.0f);
        fadeInSlider_->invalid();
    }

    const float fadeOutMs = state.render.fadeOutMs;
    if (fadeOutMs != lastFadeOutSync_)
    {
        lastFadeOutSync_ = fadeOutMs;
        fadeOutSlider_->setValueNormalized (fadeOutMs / 10.0f);
        fadeOutSlider_->invalid();
    }

    const float grainNorm = (state.render.grainSizeMs
                             - state::RenderState::kMinGrainSizeMs)
                          / (state::RenderState::kMaxGrainSizeMs
                             - state::RenderState::kMinGrainSizeMs);
    if (grainNorm != lastGrainSizeSync_)
    {
        lastGrainSizeSync_ = grainNorm;
        grainSizeSlider_->setValueNormalized (grainNorm);
        grainSizeSlider_->invalid();
    }

    const float grainSpeedNorm = (state.render.grainSpeed
                                  - state::RenderState::kMinGrainSpeed)
                               / (state::RenderState::kMaxGrainSpeed
                                  - state::RenderState::kMinGrainSpeed);
    if (grainSpeedNorm != lastGrainSpeedSync_)
    {
        lastGrainSpeedSync_ = grainSpeedNorm;
        grainSpeedSlider_->setValueNormalized (grainSpeedNorm);
        grainSpeedSlider_->invalid();
    }

    const bool stretching = state.render.pitchMode
                            == state::PitchMode::timeStretch;
    if (stretching != lastPitchSync_)
    {
        lastPitchSync_ = stretching;
        stylePitchButton();
        setGrainEnabled (stretching);
    }
}

//------------------------------------------------------------------------
// Theme a transport-bar dropdown: flat surface-2 button matching the
// toolbar's boxed controls (surface-2 fill + outline frame, left-aligned
// small text). The caller adds entries and the view to the frame.
VSTGUI::COptionMenu* NeditEditor::makeTransportMenu (const CRect& size, int32_t tag)
{
    auto* menu = new VSTGUI::COptionMenu (size, this, static_cast<VSTGUI_INT32> (tag));
    menu->setBackColor (kSurface2);
    menu->setFrameColor (kOutline);
    menu->setFont (kNormalFontSmall);
    menu->setFontColor (kTextPrimary);
    menu->setHoriAlign (kLeftText);
    return menu;
}

//------------------------------------------------------------------------
// Push the Sequence transport bar from state -> controls: pattern length
// (springer index), grid interval (note-value index), switch timing
// (PatternSwitchTiming ordinal) and switch interval (note-value index).
// The switch-interval dropdown is ENABLED only when switch timing ==
// Set Interval (it's meaningless otherwise); when disabled it draws grey
// and rejects input.
void NeditEditor::syncSequencerTransport()
{
    if (! seqPatternLength_ || ! seqGridInterval_ || ! seqSwitchTiming_
        || ! seqSwitchInterval_)
        return;

    const auto& seq = owner_->uiStateView().sequencer;
    const int nPlen = static_cast<int> (state::kPatternLengthBarsValues.size());

    const int plen = seq.patternLengthBarsIndex;
    if (plen != lastSeqPlenSync_)
    {
        lastSeqPlenSync_ = plen;
        seqPatternLength_->setValueNormalized (
            static_cast<float> (plen) / static_cast<float> (std::max (nPlen - 1, 1)));
        seqPatternLength_->invalid();
    }

    const int grid = seq.stepResolutionIndex;
    if (grid != lastSeqGridSync_)
    {
        lastSeqGridSync_ = grid;
        seqGridInterval_->setValue (static_cast<float> (grid));
        seqGridInterval_->invalid();
    }

    const int timing = static_cast<int> (seq.patternSwitchTiming);
    if (timing != lastSeqSwitchTimingSync_)
    {
        lastSeqSwitchTimingSync_ = timing;
        seqSwitchTiming_->setValue (static_cast<float> (timing));
        seqSwitchTiming_->invalid();
    }

    const int swi = seq.patternSwitchIntervalIndex;
    if (swi != lastSeqSwitchIntervalSync_)
    {
        lastSeqSwitchIntervalSync_ = swi;
        seqSwitchInterval_->setValue (static_cast<float> (swi));
        seqSwitchInterval_->invalid();
    }

    const bool swiEnabled = seq.patternSwitchTiming
                            == state::PatternSwitchTiming::setInterval;
    if (swiEnabled != lastSeqSwiEnabled_)
    {
        lastSeqSwiEnabled_ = swiEnabled;
        seqSwitchInterval_->setMouseEnabled (swiEnabled);
        seqSwitchInterval_->setFontColor (swiEnabled ? kTextPrimary : kTextSecondary);
        seqSwitchInterval_->invalid();
    }

    // Randomize needs a loaded sample (it redraws from the slice list);
    // Clear is always available.
    if (seqRandomizeBtn_ != nullptr)
    {
        const bool enabled = owner_->hasSample();
        if (enabled != lastSeqRandomizeEnabled_)
        {
            lastSeqRandomizeEnabled_ = enabled;
            seqRandomizeBtn_->setMouseEnabled (enabled);
            seqRandomizeBtn_->setTextColor (enabled ? kTextSecondary : kTextDisabled);
            seqRandomizeBtn_->invalid();
        }
    }
}

//------------------------------------------------------------------------
// Push the active performance-page tab from state to the bar + panel.
void NeditEditor::syncTabBar()
{
    if (! tabBar_ || ! panelView_)
        return;

    const int tab = static_cast<int> (owner_->uiStateView().ui.activeTab);
    if (tab == lastTabSync_)
        return;
    lastTabSync_ = tab;
    tabBar_->setValueNormalized (static_cast<float> (tab)
                                 / static_cast<float> (ui::TabBar::kTabCount - 1));
    tabBar_->invalid();
    panelView_->invalid();

    // The style-probability band is shared by the Generate and Sequence
    // pages; show its sliders there and align them to the state.
    const bool showProbs = panelView_->showsStyleProbs ();
    for (auto* sl : styleProbSliders_)
    {
        if (sl != nullptr)
            sl->setVisible (showProbs);
    }
    // Param mini-sliders AND mini dropdowns ride with the band.
    for (auto& column : paramMiniSliders_)
        for (auto* sl : column)
        {
            if (sl != nullptr)
                sl->setVisible (showProbs);
        }
    for (auto& column : paramMiniMenus_)
        for (auto* menu : column)
        {
            if (menu != nullptr)
                menu->setVisible (showProbs);
        }
    if (showProbs)
        syncStyleProbs();

    // The Generate-page timing section is Generate-only.
    const bool showTiming = (tab == static_cast<int> (state::UiTab::generate));
    if (modeSlBtn_ != nullptr)
        modeSlBtn_->setVisible (showTiming);
    if (modeClockBtn_ != nullptr)
        modeClockBtn_->setVisible (showTiming);
    if (resetBarsMenu_ != nullptr)
        resetBarsMenu_->setVisible (showTiming);
    if (clockRefMenu_ != nullptr)
        clockRefMenu_->setVisible (showTiming);
    if (tapeScopeMenu_ != nullptr)
        tapeScopeMenu_->setVisible (showTiming);
    if (filterScopeMenu_ != nullptr)
        filterScopeMenu_->setVisible (showTiming);
    if (intervalBand_ != nullptr)
        intervalBand_->setVisible (showTiming);
    if (zeroPlainBtn_ != nullptr)
        zeroPlainBtn_->setVisible (showTiming);
    if (zeroDottedBtn_ != nullptr)
        zeroDottedBtn_->setVisible (showTiming);
    if (zeroTripletBtn_ != nullptr)
        zeroTripletBtn_->setVisible (showTiming);
    if (showTiming)
        syncGenerateControls();

    // The step grid is Sequence-only.
    const bool showGrid = (tab == static_cast<int> (state::UiTab::sequence));
    if (sequencerGrid_ != nullptr)
    {
        sequencerGrid_->setVisible (showGrid);
        if (showGrid)
            sequencerGrid_->invalid();
    }
    // The transport bar rides with the step grid (both Sequence-only).
    if (seqTransportScrim_ != nullptr)
        seqTransportScrim_->setVisible (showGrid);
    if (seqPatternLength_ != nullptr)
        seqPatternLength_->setVisible (showGrid);
    if (seqGridInterval_ != nullptr)
        seqGridInterval_->setVisible (showGrid);
    if (seqSwitchTiming_ != nullptr)
        seqSwitchTiming_->setVisible (showGrid);
    if (seqSwitchInterval_ != nullptr)
        seqSwitchInterval_->setVisible (showGrid);
    if (seqClearBtn_ != nullptr)
        seqClearBtn_->setVisible (showGrid);
    if (seqRandomizeBtn_ != nullptr)
        seqRandomizeBtn_->setVisible (showGrid);
    if (showGrid)
        syncSequencerTransport();
}

//------------------------------------------------------------------------
// Push the per-style draw weights into the probability sliders and the
// Flanger param mini-sliders (deduped -- only push on real change so a
// dragged slider isn't fought; the last values ARE the control values).
void NeditEditor::syncStyleProbs()
{
    const auto& weights = owner_->uiStateView().generate.styleWeights;
    for (std::size_t i = 0; i < weights.size(); ++i)
    {
        auto* sl = styleProbSliders_[i];
        if (sl == nullptr)
            continue;
        const float w = weights[i];
        if (std::abs (w - lastStyleWeightsSync_[i]) > 1e-3f)
        {
            lastStyleWeightsSync_[i] = w;
            sl->setValueNormalized (w);
            sl->invalid();
        }
    }

    // Mini-sliders bind to Generate's style params (the automatable
    // surface 0..20); normalize via ParameterSurface so host automation
    // and UI edits converge on the same mapping.
    for (auto& column : paramMiniSliders_)
        for (auto* sl : column)
        {
            if (sl == nullptr)
                continue;
            const float norm = toNormalized (owner_->uiStateView(),
                                             static_cast<std::uint32_t> (sl->getTag()));
            if (std::abs (norm - sl->getValueNormalized()) > 1e-3f)
            {
                sl->setValueNormalized (norm);
                sl->invalid();
            }
        }

    // Mini dropdowns: same normalized mapping (a discrete value sits
    // exactly on index/(numOptions-1)), pushed as the raw entry index via
    // COptionMenu::setValue so the valueChanged echo stays silent. (The
    // sweep-scope selectors now live in the Generate timing ribbon instead
    // of the columns, so every menu here is a StyleParamId.)
    for (auto& column : paramMiniMenus_)
        for (auto* menu : column)
        {
            if (menu == nullptr)
                continue;
            const auto tag = menu->getTag();
            const float norm = toNormalized (owner_->uiStateView(),
                                             static_cast<std::uint32_t> (tag));
            if (std::abs (norm - menu->getValueNormalized()) > 1e-3f)
            {
                const int index = static_cast<int> (
                    std::lround (norm * static_cast<float> (menu->getNbEntries() - 1)));
                menu->setValue (static_cast<float> (index));
                menu->invalid();
            }
        }
}

//------------------------------------------------------------------------
//------------------------------------------------------------------------
// Pure per-mode grey contract for the Generate timing option menus, shared
// by syncGenerateControls and unit tests.
TimingGreyState timingGreyState (state::TriggerMode mode) noexcept
{
    const bool sl = (mode == state::TriggerMode::sliceLength);
    TimingGreyState g;
    g.resetBarsGreyed = ! sl;
    g.clockRefGreyed = sl;
    g.tapeScopeGreyed = sl;
    g.filterScopeGreyed = sl;
    return g;
}

//------------------------------------------------------------------------
// Generate-page timing: full-width SL|Clock mode switch, the four option
// menus (reset bars / clock reference / recalled Tape Stop + Filter sweep
// scopes) and the Clock subdivision band, pushed state->controls on
// change. The inactive mode's option column is greyed (disabled by mode
// selection); the interval band only paints in Clock mode.
void NeditEditor::syncGenerateControls()
{
    if (modeSlBtn_ == nullptr || modeClockBtn_ == nullptr
        || resetBarsMenu_ == nullptr || clockRefMenu_ == nullptr
        || tapeScopeMenu_ == nullptr || filterScopeMenu_ == nullptr
        || intervalBand_ == nullptr)
        return;

    const auto& gen = owner_->uiStateView().generate;
    const auto mode = gen.generateMode;

    if (mode != lastGenerateModeSync_)
    {
        lastGenerateModeSync_ = mode;
        styleModeButtons();
        intervalBand_->invalid();
    }

    if (gen.resetBarsIndex != lastResetBarsSync_)
    {
        lastResetBarsSync_ = gen.resetBarsIndex;
        resetBarsMenu_->setValue (static_cast<float> (gen.resetBarsIndex));
        resetBarsMenu_->invalid();
    }

    if (gen.clockReferenceIndex != lastClockRefSync_)
    {
        lastClockRefSync_ = gen.clockReferenceIndex;
        clockRefMenu_->setValue (static_cast<float> (gen.clockReferenceIndex));
        clockRefMenu_->invalid();
    }

    // Recalled sweep-scope selectors (GenerateState.tapeStopScope /
    // filterSweepScope), moved off the style columns into the Clock
    // options here; their menu index IS the WindowScope enum value.
    const float tapeNorm = ui::scopeSelectorNorm (owner_->uiStateView(),
                                                  kTagTapeStopScope);
    if (tapeNorm != lastTapeScopeSync_)
    {
        lastTapeScopeSync_ = tapeNorm;
        tapeScopeMenu_->setValue (tapeNorm);
        tapeScopeMenu_->invalid();
    }
    const float filterNorm = ui::scopeSelectorNorm (owner_->uiStateView(),
                                                    kTagFilterSweepScope);
    if (filterNorm != lastFilterScopeSync_)
    {
        lastFilterScopeSync_ = filterNorm;
        filterScopeMenu_->setValue (filterNorm);
        filterScopeMenu_->invalid();
    }

    // Mode-dependent availability: reset bars rides with Slice Length; the
    // Clock options (reference, Tape Stop scope, Filter sweep scope) ride
    // with Clock. Each control is enabled EXACTLY when its mode is selected
    // (the counterpart menu greys under the other mode).
    const auto grey = timingGreyState (mode);
    if (lastResetBarsGreyed_ != grey.resetBarsGreyed)
    {
        lastResetBarsGreyed_ = grey.resetBarsGreyed;
        resetBarsMenu_->setGreyed (grey.resetBarsGreyed);
    }
    if (lastClockRefGreyed_ != grey.clockRefGreyed)
    {
        lastClockRefGreyed_ = grey.clockRefGreyed;
        clockRefMenu_->setGreyed (grey.clockRefGreyed);
    }
    if (lastTapeScopeGreyed_ != grey.tapeScopeGreyed)
    {
        lastTapeScopeGreyed_ = grey.tapeScopeGreyed;
        tapeScopeMenu_->setGreyed (grey.tapeScopeGreyed);
    }
    if (lastFilterScopeGreyed_ != grey.filterScopeGreyed)
    {
        lastFilterScopeGreyed_ = grey.filterScopeGreyed;
        filterScopeMenu_->setGreyed (grey.filterScopeGreyed);
    }

    // The quick-clear chips act on CLOCK-mode-only weights, so they are
    // enabled exactly when Clock is (like the band they ride).
    const bool zeroEnabled = (mode == state::TriggerMode::clock);
    if (lastZeroChipsEnabled_ != zeroEnabled)
    {
        lastZeroChipsEnabled_ = zeroEnabled;
        styleZeroChips();
    }

    if (! std::equal (gen.subdivisionWeights.begin(), gen.subdivisionWeights.end(),
                      lastSubdivWeightsSync_.begin()))
    {
        lastSubdivWeightsSync_ = gen.subdivisionWeights;
        intervalBand_->invalid();
    }
}

//------------------------------------------------------------------------
void NeditEditor::setStylePaintActive (bool on)
{
    stylePaintActive_ = on;

    // While painting, the full-width bars replace the param mini-controls:
    // step them aside and restore their band-visibility on exit.
    const bool showParams = on ? false
                               : (panelView_ != nullptr && panelView_->showsStyleProbs());
    for (auto& column : paramMiniSliders_)
        for (auto* sl : column)
        {
            if (sl != nullptr)
                sl->setVisible (showParams);
        }
    for (auto& column : paramMiniMenus_)
        for (auto* menu : column)
        {
            if (menu != nullptr)
                menu->setVisible (showParams);
        }
    for (auto* sl : styleProbSliders_)
        if (sl != nullptr)
            sl->invalid();
}

//------------------------------------------------------------------------
void NeditEditor::stylePaintTo (int column, float value)
{
    if (column < 0 || column >= state::kNumPlaybackStyles)
        return;
    stylePaintColumn_ = column;
    auto* sl = styleProbSliders_[static_cast<std::size_t> (column)];
    if (sl != nullptr)
    {
        sl->setValueNormalized (value);
        sl->invalid();
        sl->valueChanged();   // -> setStyleWeight -> state publish
    }
    // The under-pointer highlight moved; neighbours repaint.
    for (auto* s : styleProbSliders_)
        if (s != nullptr && s != sl)
            s->invalid();
}

//------------------------------------------------------------------------
bool PLUGIN_API NeditEditor::open (void* parent, const PlatformType& platformType)
{
    CRect frameSize (0, 0, kEditorWidth, kEditorHeight);

    frame = new CFrame (frameSize, this);
    frame->setBackgroundColor (kWindowBase);

#if SMTG_OS_LINUX
    VSTGUI::X11::FrameConfig x11Config;
    if (plugFrame)
        Steinberg::Linux::setupVSTGUIRunloop (plugFrame);
    if (auto* linuxFactory = VSTGUI::getPlatformFactory().asLinuxFactory())
        x11Config.runLoop = linuxFactory->getRunLoop();

    if (! frame->open (parent, platformType, &x11Config))
#else
    if (! frame->open (parent, platformType))
#endif
    {
        frame->forget();
        frame = nullptr;
        return false;
    }

    // ── App bar (h48) ──────────────────────────────────────────────────
    auto* appBar = new AppBarView (CRect (0, 0, kEditorWidth, kAppBarHeight));
    frame->addView (appBar);

    // Sample name pill — right-aligned group.
    // Position: right edge of bar minus trailing controls.
    constexpr int kLoadBtnW   = 110;
    constexpr int kAuditionW  =  64;
    constexpr int kSampleW    = 170;
    constexpr int kBarRight   = kEditorWidth - 12;     // right margin

    const int loadX  = kBarRight - kLoadBtnW;
    const int audX   = loadX - 8 - kAuditionW;
    const int sampX  = audX - 8 - kSampleW;

    // Sample name chip (graphite-700 fill, outline stroke, secondary text).
    auto* sampleChip = new CTextLabel (
        CRect (sampX, 12, sampX + kSampleW, 12 + 24), "No sample");
    sampleChip->setBackColor (kSurface2);
    sampleChip->setFrameColor (kOutline);
    sampleChip->setFontColor (kTextSecondary);
    sampleChip->setHoriAlign (kLeftText);
    frame->addView (sampleChip);
    sampleNameLabel_ = sampleChip;

    // Audition button (toggle: AUDITION off / SILENCE on).
    auto* auditionBtn = new CTextButton (
        CRect (audX, 12, audX + kAuditionW, 12 + 24), this,
        kTagAudition, "AUDITION");
    frame->addView (auditionBtn);
    auditionBtn_ = auditionBtn;
    styleAuditionButton();

    // Load Sample button (salmon-400 fill, on-salmon text, rx=4).
    auto* loadBtn = new CTextButton (
        CRect (loadX, 12, loadX + kLoadBtnW, 12 + 24), this,
        kTagLoadSample, "LOAD SAMPLE");
    {
        GradientColorStopMap stops;
        stops.emplace (0., kAccent);
        stops.emplace (1., kAccent);
        loadBtn->setGradient (CGradient::create (stops));
        GradientColorStopMap hlStops;
        hlStops.emplace (0., kAccentPressed);
        hlStops.emplace (1., kAccentPressed);
        loadBtn->setGradientHighlighted (CGradient::create (hlStops));
        loadBtn->setTextColor (kAccentOn);
        loadBtn->setTextColorHighlighted (kAccentOn);
        loadBtn->setRoundRadius (4);
    }
    frame->addView (loadBtn);
    loadBtn_ = loadBtn;

    // ── Tool bar (second strip, h48): bar length / BPM override / other
    //    document-level controls land here. ──────────────────────────────
    auto* toolbar = new ToolBarView (
        CRect (0, kAppBarHeight, kEditorWidth, kAppBarHeight + kToolBarHeight));
    frame->addView (toolbar);

    // Document-level controls, left-aligned in the strip. Each custom
    // control draws its own caption + box within the full 48px band; all
    // route value edits through the param IDs (101/102/103) so host
    // automation and the state stay in lockstep.
    constexpr int kBarsW = 66;   // shrunk 0.75x to free room for the
    constexpr int kBpmW = 56;    // pitch-mode toggle; BPM field halved, quantize
    constexpr int kOverrideW = 88;   // grid dropdown halved, fades stacked.
    constexpr int kSensW = 110;
    constexpr int kQuantW = 88;
    constexpr int kGridW = 55;
    constexpr int kFadeW = 108;
    constexpr int kPitchW = 110;
    constexpr int kGrainW = 104;
    constexpr int kX0 = 16;
    constexpr int kGap = 18;

    const int barsX = kX0;
    const int bpmX = barsX + kBarsW + kGap;
    const int overX = bpmX + kBpmW + kGap;
    const int sensX = overX + kOverrideW + kGap;
    const int quantX = sensX + kSensW + kGap;
    const int gridX = quantX + kQuantW + kGap;
    const int fadeX  = gridX + kGridW + kGap;
    const int pitchX = fadeX + kFadeW + kGap;
    const int grainX = pitchX + kPitchW + kGap;
    const CRect strip (0, kAppBarHeight, 0, kAppBarHeight + kToolBarHeight);

    auto* bars = new ui::BarsStepper (
        CRect (barsX, strip.top, barsX + kBarsW, strip.bottom), this,
        static_cast<VSTGUI_INT32> (kParamLoopLengthBars));
    frame->addView (bars);
    barsStepper_ = bars;

    auto* bpm = new ui::BpmScrubField (
        CRect (bpmX, strip.top, bpmX + kBpmW, strip.bottom), this,
        static_cast<VSTGUI_INT32> (kParamManualTempoBpm));
    bpm->setMode (false, 0.0);
    frame->addView (bpm);
    bpmField_ = bpm;

    // Override toggle sits INSIDE the BPM field's box band (not the full
    // 48px strip) so its height matches its neighbours — 24px starting at
    // kBoxTop down into the strip.
    auto* overrideBtn = new CTextButton (
        CRect (overX, strip.top + static_cast<int> (std::lround (kBoxTop)),
               overX + kOverrideW,
               strip.top + static_cast<int> (std::lround (kBoxTop + kBoxH))),
        this, static_cast<VSTGUI_INT32> (kParamManualTempoEnabled), "OVERRIDE");
    frame->addView (overrideBtn);
    overrideBtn_ = overrideBtn;
    styleOverrideButton();

    // Detection sensitivity slider (editor-local; drag re-runs detection).
    auto* sens = new ui::SensitivitySlider (
        CRect (sensX, strip.top, sensX + kSensW, strip.bottom), this,
        kTagSensitivity);
    sens->setActive (false);
    frame->addView (sens);
    sensSlider_ = sens;

    // Auto-onsets grid-quantize toggle -- the ANALYSIS quantize (snaps
    // detected onsets to the note-value grid during slicing); NOT the
    // playback beat-quantize (RenderState.beatQuantize*, exposed with the
    // style-param sliders later). Sits INSIDE the box band like OVERRIDE.
    auto* quant = new CTextButton (
        CRect (quantX, strip.top + static_cast<int> (std::lround (kBoxTop)),
               quantX + kQuantW,
               strip.top + static_cast<int> (std::lround (kBoxTop + kBoxH))),
        this, kTagQuantize, "QUANTIZE");
    frame->addView (quant);
    quantizeBtn_ = quant;
    styleQuantizeButton();

    // Quantize-grid dropdown -- the note-value the quantize toggle snaps
    // detected auto onsets to (SampleState.quantizeGridIndex). Palette names
    // in kNoteValues order so ENTRY INDEX == palette index; getValue() is
    // the 0-based entry/index. Editor-local, not a host param. The popup
    // list itself is themed by the platform; the button is themed to match
    // the toolbar's flat boxed controls.
    auto* grid = new COptionMenu (
        CRect (gridX, strip.top + static_cast<int> (std::lround (kBoxTop)),
               gridX + kGridW,
               strip.top + static_cast<int> (std::lround (kBoxTop + kBoxH))),
        this, kTagQuantizeGrid);
    for (int i = 0; i < state::kNumNoteValues; ++i)
        grid->addEntry (state::kNoteValues[static_cast<std::size_t> (i)].name);
    grid->setBackColor (kSurface2);
    grid->setFrameColor (kOutline);
    grid->setFont (kNormalFontSmall);
    grid->setFontColor (kTextPrimary);
    grid->setHoriAlign (kLeftText);
    frame->addView (grid);
    quantizeMenu_ = grid;

    // Per-pick declick fades (global play feel, always active). Attack =
    // fade-in, release = fade-out; each clamped by the engine to half the
    // pick length during rendering. Stacked vertically to save row space --
    // each occupies a ~20px-high band within the 48px strip.
    const int fadeTop = static_cast<int> (std::lround (strip.top)) + 3;
    const int fadeMid = static_cast<int> (std::lround (strip.top)) + 25;
    auto* fadeIn = new ui::FadeSlider (
        CRect (fadeX, fadeTop, fadeX + kFadeW, fadeTop + 20), this,
        kTagFadeIn, "FADE IN");
    frame->addView (fadeIn);
    fadeInSlider_ = fadeIn;

    auto* fadeOut = new ui::FadeSlider (
        CRect (fadeX, fadeMid, fadeX + kFadeW, fadeMid + 20), this,
        kTagFadeOut, "FADE OUT");
    frame->addView (fadeOut);
    fadeOutSlider_ = fadeOut;

    // Repitch vs time-stretch render mode toggle. Lit = time-stretching
    // (granular, pitch preserved); unlit = repitch. Renders RenderState
    // only -- no slice rebuild. Sits INSIDE the box band like OVERRIDE.
    auto* pitch = new CTextButton (
        CRect (pitchX, strip.top + static_cast<int> (std::lround (kBoxTop)),
               pitchX + kPitchW,
               strip.top + static_cast<int> (std::lround (kBoxTop + kBoxH))),
        this, kTagPitchMode, "TIMESTRETCH");
    frame->addView (pitch);
    pitchBtn_ = pitch;
    stylePitchButton();

    // Time-Stretch granular character. Enabled only while pitchMode is
    // time-stretch (the granular engine); dimmed + inert when repitching,
    // where grain size/speed have no effect. Stacked like the fades:
    // SIZE (20..150 ms) above SPEED (1..8 x), each a ~20px band.
    auto* grainSize = new ui::GrainSlider (
        CRect (grainX, fadeTop, grainX + kGrainW, fadeTop + 20), this,
        kTagGrainSize, "SIZE", 20.0, 150.0, "%.0fms", 5.0);
    grainSize->setActive (false);
    frame->addView (grainSize);
    grainSizeSlider_ = grainSize;

    auto* grainSpeed = new ui::GrainSlider (
        CRect (grainX, fadeMid, grainX + kGrainW, fadeMid + 20), this,
        kTagGrainSpeed, "SPEED", 1.0, 8.0, "%.1fx", 0.25);
    grainSpeed->setActive (false);
    frame->addView (grainSpeed);
    grainSpeedSlider_ = grainSpeed;
    setGrainEnabled (false);

    syncToolBarControls();

    // ── Waveform display (below the tool bar) ──────────────────────────
    constexpr int kWaveformH = 96;
    constexpr int kWaveTop  = kAppBarHeight + kToolBarHeight;
    auto* wave = new WaveformView (*owner_, this, kWaveformH);
    wave->setViewSize (CRect (0, kWaveTop, kEditorWidth, kWaveTop + kWaveformH));
    frame->addView (wave);
    waveformView_ = wave;

    // ── Performance-page tab bar (48px total, per spec) ────────────────
    // Container-less underline tabs (design-language.md §6): the strip sits
    // on the window base below the sample-level areas and swaps only the
    // panel area beneath it.
    constexpr int kTabBarH = 48;
    constexpr int kTabTop  = kWaveTop + kWaveformH;
    auto* tabBar = new ui::TabBar (
        CRect (0, kTabTop, kEditorWidth, kTabTop + kTabBarH), this, kTagTabBar);
    frame->addView (tabBar);
    tabBar_ = tabBar;

    // ── Panel area below the tab bar (one card-set per page) ───────────
    constexpr int kPanelGutter = 8;     // gutter under the tab strip
    constexpr int kPanelMarginX = 24;   // window margins (design guide)
    constexpr int kPanelTop = kTabTop + kTabBarH + kPanelGutter;
    auto* panel = new ui::PanelView (owner_);
    panel->setViewSize (CRect (kPanelMarginX, kPanelTop,
                               kEditorWidth - kPanelMarginX,
                               kEditorHeight - kPanelGutter));
    frame->addView (panel);
    panelView_ = panel;

    // ── Style-probability band (Generate/Sequence tabs): a 208px band    ──
    // across the card's inner area, divided into 9 columns. Each column is
    // one StyleProbSlider (label chip + left-aligned vertical weight bar),
    // tags kTagStyleProbBase + style ordinal, positioned to overlay the
    // card area drawn by PanelView.
    const double bandLeft = kPanelMarginX + kCardPad;
    const double bandTop  = kPanelTop + kCardPad;
    const double bandW    = (kEditorWidth - 2.0 * kPanelMarginX) - 2.0 * kCardPad;
    const double colW     = bandW / static_cast<double> (state::kNumPlaybackStyles);
    for (int i = 0; i < state::kNumPlaybackStyles; ++i)
    {
        const double colX = bandLeft + static_cast<double> (i) * colW;
        auto* prob = new ui::StyleProbSlider (
            CRect (colX, bandTop, colX + colW, bandTop + kStyleBandH),
            this, static_cast<VSTGUI_INT32> (kTagStyleProbBase + i), i);
        frame->addView (prob);
        styleProbSliders_[static_cast<std::size_t> (i)] = prob;
    }

    // ── Sequencer step grid (Sequence tab only) ────────────────────────
    // Fills the card area below the shared style band, stopping short of the
    // bottom transport bar. Procedural sizing + vertical scrolling live in
    // the view; here we only hand it its rect.
    {
        const double transportBottom = kEditorHeight - kPanelGutter - kCardPad;
        const double transportTop = transportBottom - kSeqTransportH;
        const double gridTop = bandTop + kStyleBandH + 16.0;
        const double gridBottom = transportTop - 8.0;
        seqGridTop_ = gridTop;
        seqGridBottom_ = gridBottom;
        auto* seqGrid = new ui::SequencerGridView (
            CRect (bandLeft, gridTop, bandLeft + bandW, gridBottom),
            this, kTagSequencerGrid, this);
        frame->addView (seqGrid);
        sequencerGrid_ = seqGrid;
        seqGrid->setVisible (false);   // shown by syncTabBar on the Sequence tab
    }

    // ── Sequencer transport bar: pattern length spinner + grid interval / ──
    // switch timing / switch interval dropdowns. All editor-local (route
    // through publish-only processor setters); the switch-interval dropdown
    // is enabled only when switch timing == Set Interval. Hidden outside the
    // Sequence tab by syncTabBar.
    {
        const double transportBottom = kEditorHeight - kPanelGutter - kCardPad;
        const double transportTop = transportBottom - kSeqTransportH;
        const double tLeft = bandLeft;
        const double boxTop = transportTop + kBoxTop;
        const double boxBottom = boxTop + kBoxH;

        constexpr double kCtlGap = 16.0;
        constexpr double kPlenW = 150.0;   // pattern length spinner
        constexpr double kGridWt = 96.0;   // grid interval
        constexpr double kSwtW = 150.0;    // switch timing
        constexpr double kSwiW = 120.0;    // switch interval

        // Control boxes + the captions the scrim draws above them. The
        // scrim (divider + captions) is added BEFORE the interactive
        // controls so they stay on top for hit-testing; its caption band
        // sits above the control boxes, so nothing overlaps.
        ui::SequencerTransportBar::Caption captions[4];
        double cx = tLeft;

        captions[0] = { "PATTERN LENGTH", cx, cx + kPlenW };
        captions[1] = { "GRID INTERVAL", cx + kPlenW + kCtlGap,
                        cx + kPlenW + kCtlGap + kGridWt };
        captions[2] = { "SWITCH TIMING", cx + kPlenW + kCtlGap + kGridWt + kCtlGap,
                        cx + kPlenW + kCtlGap + kGridWt + kCtlGap + kSwtW };
        captions[3] = { "SWITCH INTERVAL",
                        cx + kPlenW + kCtlGap + kGridWt + kCtlGap + kSwtW + kCtlGap,
                        cx + kPlenW + kCtlGap + kGridWt + kCtlGap + kSwtW + kCtlGap + kSwiW };

        auto* scrim = new ui::SequencerTransportBar (
            CRect (tLeft, transportTop, tLeft + bandW, transportBottom),
            captions, 4);
        frame->addView (scrim);
        seqTransportScrim_ = scrim;

        cx = tLeft;
        auto* plen = new ui::PatternLengthStepper (
            CRect (cx, transportTop, cx + kPlenW, transportBottom), this,
            kTagSeqPatternLength);
        frame->addView (plen);
        seqPatternLength_ = plen;
        cx += kPlenW + kCtlGap;

        auto* gridInterval = makeTransportMenu (
            CRect (cx, boxTop, cx + kGridWt, boxBottom), kTagSeqGridInterval);
        frame->addView (gridInterval);
        seqGridInterval_ = gridInterval;
        cx += kGridWt + kCtlGap;

        auto* swt = makeTransportMenu (
            CRect (cx, boxTop, cx + kSwtW, boxBottom), kTagSeqSwitchTiming);
        frame->addView (swt);
        seqSwitchTiming_ = swt;
        cx += kSwtW + kCtlGap;

        auto* swi = makeTransportMenu (
            CRect (cx, boxTop, cx + kSwiW, boxBottom), kTagSeqSwitchInterval);
        frame->addView (swi);
        seqSwitchInterval_ = swi;
        cx += kSwiW;

        // Entries: note values for grid/switch interval, timing modes for
        // switch timing. Entry indices == state indexes.
        for (int i = 0; i < state::kNumNoteValues; ++i)
        {
            gridInterval->addEntry (state::kNoteValues[static_cast<std::size_t> (i)].name);
            swi->addEntry (state::kNoteValues[static_cast<std::size_t> (i)].name);
        }
        for (int i = 0; i < static_cast<int> (state::kPatternSwitchTimingNames.size()); ++i)
            swt->addEntry (state::kPatternSwitchTimingNames[static_cast<std::size_t> (i)]);

        gridInterval->setValue (static_cast<float> (
            owner_->uiStateView().sequencer.stepResolutionIndex));
        swt->setValue (static_cast<float> (
            owner_->uiStateView().sequencer.patternSwitchTiming));
        swi->setValue (static_cast<float> (
            owner_->uiStateView().sequencer.patternSwitchIntervalIndex));
        swi->setMouseEnabled (false);   // armed only under Set Interval

        // Clear / Randomize actions (right-aligned beyond the four controls).
        // Graphite-outlined like the toolbar buttons; Randomize needs a
        // sample (wired in syncSequencerTransport), Clear always available.
        constexpr double kActW = 112.0;
        constexpr double kActGap = 12.0;
        const double rightEdge = tLeft + bandW;
        const double randLeft = rightEdge - kActW;
        const double clearLeft = randLeft - kActGap - kActW;
        auto* clearBtn = new CTextButton (
            CRect (clearLeft, boxTop, clearLeft + kActW, boxBottom), this,
            kTagSeqClear, "CLEAR");
        auto* randBtn = new CTextButton (
            CRect (randLeft, boxTop, randLeft + kActW, boxBottom), this,
            kTagSeqRandomize, "RANDOMIZE");
        for (auto* btn : { clearBtn, randBtn })
        {
            GradientColorStopMap stops;
            stops.emplace (0., kSurface2);
            stops.emplace (1., kSurface2);
            btn->setGradient (CGradient::create (stops));
            GradientColorStopMap hlStops;
            hlStops.emplace (0., kSurface3);
            hlStops.emplace (1., kSurface3);
            btn->setGradientHighlighted (CGradient::create (hlStops));
            btn->setTextColor (kTextSecondary);
            btn->setTextColorHighlighted (kTextPrimary);
            btn->setFrameColor (kOutline);
            btn->setFrameColorHighlighted (kOutline);
            btn->setRoundRadius (4);
        }
        frame->addView (clearBtn);
        frame->addView (randBtn);
        seqClearBtn_ = clearBtn;
        seqRandomizeBtn_ = randBtn;
    }

    // Every column hosts its style's params: continuous params get a 12px
    // horizontal mini-slider row directly under their caption, discrete
    // params a mini dropdown whose caption row IS the control (rows from
    // columnParamsFor -- Subdivide/Volume-Mode skipped, swept params pair
    // with their *Mode, Volume last). Tags = the StyleParamId (< 1000 =
    // ParameterSurface id), so edits route through the host edit
    // protocol; the style-scoped scope selectors use editor-local tags
    // (>= 1000) with publish-only setters. Added after the prob sliders,
    // so they sit on top for hit-testing; the columns' caption rows line
    // up via the shared kProb* geometry and the same captionRowLocalY the
    // columns draw with.
    for (auto& column : paramMiniSliders_)
        column.fill (nullptr);
    for (auto& column : paramMiniMenus_)
        column.fill (nullptr);
    for (int col = 0; col < state::kNumPlaybackStyles; ++col)
    {
        auto* prob = styleProbSliders_[static_cast<std::size_t> (col)];
        const auto& rows = prob->paramRows();
        if (rows.empty())
            continue;   // no params (none today; keep the weight slider)
        const double colX = bandLeft + static_cast<double> (col) * colW;
        const double listX = colX + kProbTrackX + kProbTrackW + 4.0;
        const double listW = colX + colW - 2.0 - listX;
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            if (! row.continuous)
            {
                // Discrete param: its caption row IS the control -- a mini
                // dropdown showing the current option.
                auto* menu = new ui::ParamMiniMenu (
                    CRect (listX, bandTop + prob->captionRowLocalY (i),
                           listX + listW,
                           bandTop + prob->captionRowLocalY (i) + kProbRowH),
                    this, row.tag, col);
                frame->addView (menu);
                paramMiniMenus_[static_cast<std::size_t> (col)]
                               [i] = menu;
                continue;
            }

            const double localY = prob->sliderRowLocalY (i);
            auto* mini = new ui::ParamMiniSlider (
                CRect (listX, bandTop + localY, listX + listW,
                       bandTop + localY + kProbSliderRowH),
                this, row.tag,
                col, paramReadoutFormat (row.paramId()));
            frame->addView (mini);
            paramMiniSliders_[static_cast<std::size_t> (col)]
                             [i] = mini;
        }
    }

    // ── Generate-page Timing section: mode switch → per-mode options → ──
    // Clock subdivision weights, all spanning the full panel width. The
    // sweep scopes recalled from the style columns (Tape Stop + Filter)
    // sit with the Clock options. These controls only exist on the
    // Generate page; hidden elsewhere by syncTabBar.
    const double panelInnerBottom = kEditorHeight - kPanelGutter - kCardPad;

    // Mode switch: Slice Length vs Clock as a full-width stick of two
    // segments (1px seam). The active segment is accent-filled; the other
    // stays a quiet outlined button (styleModeSegment/Buttons).
    const double modeY = bandTop + kStyleBandH + kTimingGap;
    const double segW = (bandW - 1.0) / 2.0;
    auto* modeSl = new CTextButton (
        CRect (bandLeft, modeY, bandLeft + segW, modeY + kTimingModeBtnH),
        this, kTagGenerateModeSL, "SLICE LENGTH");
    auto* modeClock = new CTextButton (
        CRect (bandLeft + segW + 1.0, modeY, bandLeft + bandW,
               modeY + kTimingModeBtnH),
        this, kTagGenerateModeClock, "CLOCK");
    frame->addView (modeSl);
    frame->addView (modeClock);
    modeSlBtn_ = modeSl;
    modeClockBtn_ = modeClock;
    styleModeButtons();

    // Per-mode option row, disabled by the mode selection (greyed while
    // the OTHER mode is active): Slice Length owns "RESET EVERY"; Clock
    // owns the reference note value and the recalled Tape Stop / Filter
    // sweep scopes (the engine honors the scopes only in Clock). Entries
    // are built from the state palettes after construction (the generic
    // ParamMiniMenu option path); the scope menus auto-fill from
    // paramRowOptionName via their tags.
    const double optY = modeY + kTimingModeBtnH + kTimingOptionsGap;
    const double menuW = (bandW - 3.0 * kTimingMenuGap) / 4.0;
    const auto menuRect = [&] (int i) {
        const double x = bandLeft + static_cast<double> (i) * (menuW + kTimingMenuGap);
        return CRect (x, optY, x + menuW, optY + kTimingMenuH);
    };

    auto* resetBars = new ui::ParamMiniMenu (menuRect (0), this, kTagResetBars,
                                             0, "RESET EVERY");
    for (int b : state::kResetBarsValues)
    {
        char label[24];
        std::snprintf (label, sizeof (label), "%d %s", b, b == 1 ? "bar" : "bars");
        resetBars->addEntry (label);
    }
    auto* clockRef = new ui::ParamMiniMenu (menuRect (1), this, kTagClockReference,
                                            0, "CLOCK REFERENCE");
    for (const auto& v : state::kNoteValues)
        clockRef->addEntry (v.name);
    auto* tapeScope = new ui::ParamMiniMenu (menuRect (2), this, kTagTapeStopScope,
                                             0, "TAPE STOP SCOPE");
    auto* filterScope = new ui::ParamMiniMenu (menuRect (3), this, kTagFilterSweepScope,
                                               0, "FILTER SWEEP SCOPE");
    frame->addView (resetBars);
    frame->addView (clockRef);
    frame->addView (tapeScope);
    frame->addView (filterScope);
    resetBarsMenu_ = resetBars;
    clockRefMenu_ = clockRef;
    tapeScopeMenu_ = tapeScope;
    filterScopeMenu_ = filterScope;
    // (Seeding happens below, once intervalBand_ exists -- the sync guard
    // early-returns while ANY timing control is still null.)

    // Clock subdivision weights: one thin probability bar per note value,
    // filling the panel width below the options. Clock-mode only (the band
    // greys under Slice Length).
    const double bandTopT = optY + kTimingMenuH + kTimingGap;
    auto* intervalBand = new ui::IntervalProbBand (
        CRect (bandLeft, bandTopT, bandLeft + bandW, panelInnerBottom),
        this, kTagIntervalProbBand, owner_);
    frame->addView (intervalBand);
    intervalBand_ = intervalBand;

    // Momentary quick-clears for the subdivision band, sitting on its
    // caption row's right side: "n=0" / "nd=0" / "nt=0" zero that
    // variant group's weights in one press (see setSubdivisionGroupZero).
    // Same label scheme as the user's N/ND/NT shorthand; there is no
    // persisted toggle state -- the weights are zeroed for real.
    const double chipY = bandTopT;
    const double chipRight = bandLeft + bandW;
    const double chip0Left = chipRight - 3.0 * kZeroChipW - 2.0 * kZeroChipGap;
    const auto chipRect = [&] (int i) {
        const double x = chip0Left + static_cast<double> (i) * (kZeroChipW + kZeroChipGap);
        return CRect (x, chipY, x + kZeroChipW, chipY + kIntervalCaptionH);
    };
    auto* zeroPlain = new CTextButton (chipRect (0), this, kTagClearPlain, "n=0");
    auto* zeroDotted = new CTextButton (chipRect (1), this, kTagClearDotted, "nd=0");
    auto* zeroTriplet = new CTextButton (chipRect (2), this, kTagClearTriplet, "nt=0");
    frame->addView (zeroPlain);
    frame->addView (zeroDotted);
    frame->addView (zeroTriplet);
    zeroPlainBtn_ = zeroPlain;
    zeroDottedBtn_ = zeroDotted;
    zeroTripletBtn_ = zeroTriplet;
    styleZeroChips();   // seed the Clock-only grey (Slice Length default)
    syncGenerateControls();   // seed greys + option values before first paint
                              // (ALL timing controls exist from here on)

    syncTabBar();   // seed the strip + panel from the restored activeTab
    syncSequencerTransport();

    setIdleRate (60);
    return true;
}

//------------------------------------------------------------------------
void PLUGIN_API NeditEditor::close()
{
    sampleNameLabel_ = nullptr;
    auditionBtn_ = nullptr;
    loadBtn_ = nullptr;
    overrideBtn_ = nullptr;
    barsStepper_ = nullptr;
    bpmField_ = nullptr;
    sensSlider_ = nullptr;
    quantizeBtn_ = nullptr;
    quantizeMenu_ = nullptr;
    pitchBtn_ = nullptr;
    fadeInSlider_ = nullptr;
    fadeOutSlider_ = nullptr;
    grainSizeSlider_ = nullptr;
    grainSpeedSlider_ = nullptr;
    tabBar_ = nullptr;
    panelView_ = nullptr;
    modeSlBtn_ = nullptr;
    modeClockBtn_ = nullptr;
    resetBarsMenu_ = nullptr;
    clockRefMenu_ = nullptr;
    tapeScopeMenu_ = nullptr;
    filterScopeMenu_ = nullptr;
    intervalBand_ = nullptr;
    zeroPlainBtn_ = nullptr;
    zeroDottedBtn_ = nullptr;
    zeroTripletBtn_ = nullptr;
    styleProbSliders_.fill (nullptr);
    for (auto& column : paramMiniSliders_)
        column.fill (nullptr);
    for (auto& column : paramMiniMenus_)
        column.fill (nullptr);
    waveformView_ = nullptr;
    if (frame != nullptr)
    {
        frame->close();
        frame = nullptr;
    }
}

//------------------------------------------------------------------------
void NeditEditor::valueChanged (CControl* control)
{
    if (control == nullptr)
        return;

    if (control->getTag() == kTagLoadSample)
    {
        if (pressedEdge (*control, lastLoadPressed_))
            runFileSelector();
        return;
    }

    if (control->getTag() == kTagAudition)
    {
        const bool press = pressedEdge (*control, lastAuditionPressed_);
        if (press && owner_->hasSample())
        {
            const bool wasOn = owner_->uiStateView().ui.auditionEnabled;
            owner_->setAuditionEnabled (! wasOn);
            styleAuditionButton();
        }
        return;
    }

    if (control->getTag() == static_cast<VSTGUI_INT32> (kParamManualTempoEnabled))
    {
        if (pressedEdge (*control, lastOverridePressed_))
        {
            const bool enabled = owner_->uiStateView().sample.manualBpmOverrideEnabled;
            if (! enabled)
            {
                // Fresh override has manualBpmOverrideValue == 0 (never
                // scrubbed), which the engine treats as "no tempo". Seed the
                // BPM value to what was being derived so toggling ON starts
                // from the current state instead of a broken 0.
                const auto& sample = owner_->uiStateView().sample;
                double seed = engine::tempo::calculatedOriginalBpm (sample);
                if (seed < kBpmMin || seed > kBpmMax || seed != seed)   // NaN guard
                    seed = 0.5 * (kBpmMin + kBpmMax);
                const float norm = static_cast<float> ((seed - kBpmMin) / (kBpmMax - kBpmMin));
                setParam (kParamManualTempoBpm, norm);
            }
            setParam (kParamManualTempoEnabled, enabled ? 0.0f : 1.0f);
            // Idle sync re-derives the style from state; toggle immediately
            // so the toolbar reads correct even before the next timer tick.
            syncToolBarControls();
        }
        return;
    }

    if (control->getTag() == kTagSensitivity)
    {
        // Structural edit (re-runs detection), not a host param.
        if (owner_->hasSample())
        {
            owner_->setSensitivity (control->getValueNormalized());
            if (waveformView_)
                waveformView_->invalid();   // slice markers changed
        }
        return;
    }

    if (control->getTag() == kTagQuantize)
    {
        if (pressedEdge (*control, lastQuantizePressed_))
        {
            const bool on = owner_->uiStateView().sample.quantizeTransients;
            owner_->setQuantizeTransients (! on);
            styleQuantizeButton();
            if (waveformView_)
                waveformView_->invalid();   // auto-boundary positions changed
        }
        return;
    }

    if (control->getTag() == kTagQuantizeGrid)
    {
        // Structural edit (re-runs slicing), not a host param. getValue()
        // on a COptionMenu is the 0-based entry index == palette index.
        if (owner_->hasSample())
        {
            const int idx = static_cast<int> (std::lround (control->getValue()));
            owner_->setQuantizeGrid (idx);
            if (waveformView_)
                waveformView_->invalid();   // auto-boundary positions changed
        }
        return;
    }

    // ── Sequencer transport bar (Sequence tab) ─────────────────────────
    // All editor-local, route through publish-only setters. Grid interval
    // and switch interval are note-value indexes; switch timing an ordinal;
    // the pattern length is the spinner's normalized index.
    if (control->getTag() == kTagSeqPatternLength)
    {
        const int n = static_cast<int> (state::kPatternLengthBarsValues.size());
        const int idx = static_cast<int> (std::lround (
            control->getValueNormalized() * static_cast<float> (n - 1)));
        (void) owner_->setSequencerPatternLength (idx);
        return;
    }
    if (control->getTag() == kTagSeqGridInterval)
    {
        (void) owner_->setSequencerStepResolution (
            static_cast<int> (std::lround (control->getValue())));
        return;
    }
    if (control->getTag() == kTagSeqSwitchTiming)
    {
        (void) owner_->setSequencerSwitchTiming (
            static_cast<int> (std::lround (control->getValue())));
        // Re-arm the switch-interval dropdown immediately (not on the next
        // idle tick), matching the mode-switch's explicit first-click rule.
        syncSequencerTransport();
        return;
    }
    if (control->getTag() == kTagSeqSwitchInterval)
    {
        (void) owner_->setSequencerSwitchInterval (
            static_cast<int> (std::lround (control->getValue())));
        return;
    }

    // Clear / Randomize are momentary actions (CTextButton double-fires --
    // rising edge only). Both mutate the working grid; repaint it now.
    if (control->getTag() == kTagSeqClear)
    {
        if (pressedEdge (*control, lastSeqClearPressed_))
        {
            owner_->clearSequence();
            if (sequencerGrid_ != nullptr)
                sequencerGrid_->invalid();
        }
        return;
    }
    if (control->getTag() == kTagSeqRandomize)
    {
        if (pressedEdge (*control, lastSeqRandomizePressed_))
        {
            (void) owner_->randomizeSequence();
            if (sequencerGrid_ != nullptr)
                sequencerGrid_->invalid();
        }
        return;
    }

    if (control->getTag() == kTagFadeIn)
    {
        owner_->setFadeInMs (10.0f * control->getValueNormalized());
        return;
    }

    if (control->getTag() == kTagFadeOut)
    {
        owner_->setFadeOutMs (10.0f * control->getValueNormalized());
        return;
    }

    if (control->getTag() == kTagPitchMode)
    {
        if (pressedEdge (*control, lastPitchPressed_))
        {
            const bool stretch = owner_->uiStateView().render.pitchMode
                                 == state::PitchMode::timeStretch;
            owner_->setPitchMode (stretch ? state::PitchMode::repitch
                                          : state::PitchMode::timeStretch);
            stylePitchButton();
        }
        return;
    }

    if (control->getTag() == kTagGrainSize)
    {
        owner_->setGrainSizeMs (
            20.0f + (state::RenderState::kMaxGrainSizeMs
                     - state::RenderState::kMinGrainSizeMs)
                * control->getValueNormalized());
        return;
    }

    if (control->getTag() == kTagGrainSpeed)
    {
        owner_->setGrainSpeed (
            state::RenderState::kMinGrainSpeed
            + (state::RenderState::kMaxGrainSpeed
               - state::RenderState::kMinGrainSpeed)
                * control->getValueNormalized());
        return;
    }

    if (control->getTag() == kTagTabBar)
    {
        const int ord = static_cast<int> (
            std::lround (control->getValueNormalized()
                         * static_cast<float> (ui::TabBar::kTabCount - 1)));
        if (ord < 0 || ord >= ui::TabBar::kTabCount)
            return;
        // Selecting a tab TRANSFERS AUDIO CONTROL to that tab's mode. We drive
        // it through the automatable trigger-mode param (not a publish-only
        // setActiveTab) so the host records/reflects the change and won't
        // later re-push a stale value that snaps the mode back. The fold
        // (applyNormalized) sets triggerMode + the Generate sub-mode mirror +
        // ui.activeTab together, so tab and mode stay in lockstep.
        const auto tab = static_cast<state::UiTab> (ord);
        const auto mode = state::triggerModeForTab (
            tab, owner_->uiStateView().generate.generateMode);
        setParam (kParamTriggerMode,
                  static_cast<float> (static_cast<int> (mode))
                      / static_cast<float> (state::kNumTriggerModes - 1));
        if (panelView_)
            panelView_->invalid();   // the panel area re-renders per page
        syncTabBar();   // toggle per-tab views NOW, not on the next idle tick
        return;
    }

    // Style-scoped sweep scopes (GenerateState, publish-only like the
    // toolbar structural edits -- NOT host params). COptionMenu value is
    // the entry index 0 = wholeWindow / 1 = perTick (WindowScope's
    // numeric values).
    if (control->getTag() == kTagTapeStopScope)
    {
        const int idx = static_cast<int> (std::lround (control->getValue()));
        owner_->setTapeStopScope (idx <= 0 ? state::WindowScope::wholeWindow
                                           : state::WindowScope::perTick);
        return;
    }

    if (control->getTag() == kTagFilterSweepScope)
    {
        const int idx = static_cast<int> (std::lround (control->getValue()));
        owner_->setFilterSweepScope (idx <= 0 ? state::WindowScope::wholeWindow
                                              : state::WindowScope::perTick);
        return;
    }

    // Generate-page timing. The mode switch is a two-segment toggle styled
    // like the pitch-mode button: pressedEdge + the live model state decide
    // the new mode (never the button value, which double-echoes as a
    // CTextButton release), and re-selecting the active mode is a no-op.
    if (control->getTag() == kTagGenerateModeSL
        || control->getTag() == kTagGenerateModeClock)
    {
        const auto mode = control->getTag() == kTagGenerateModeSL
                              ? state::TriggerMode::sliceLength
                              : state::TriggerMode::clock;
        // CRITICAL: update the press-edge latch on EVERY valueChanged echo,
        // BEFORE the mode guard. CTextButton kick-style release echoes
        // [valueChanged(max), valueChanged(min)]; the min echo is what resets
        // the latch. If it short-circuits on "mode already selected", the
        // latch stays true and the NEXT physical click's max echo is
        // swallowed as a false edge -- the first click of every later change
        // does nothing (alternating one-good/one-dead clicks).
        const bool edge = pressedEdge (*control, control->getTag() == kTagGenerateModeSL
                                                      ? lastModeSlPressed_
                                                      : lastModeClockPressed_);
        if (edge && mode != owner_->uiStateView().generate.generateMode)
        {
            owner_->setGenerateMode (mode);
            // Refresh segment styling, per-mode greys AND the interval band
            // in the same call stack (the mode-change branch in
            // syncGenerateControls fires because lastGenerateModeSync_
            // still holds the old mode), so the section reacts on the FIRST
            // click -- nothing waits for the idle tick.
            syncGenerateControls();
        }
        return;
    }

    if (control->getTag() == kTagResetBars)
    {
        owner_->setResetBars (static_cast<int> (std::lround (control->getValue())));
        return;
    }

    if (control->getTag() == kTagClockReference)
    {
        owner_->setClockReference (static_cast<int> (std::lround (control->getValue())));
        return;
    }

    if (control->getTag() == kTagIntervalProbBand)
    {
        auto* band = dynamic_cast<ui::IntervalProbBand*> (control);
        if (band != nullptr)
            owner_->setSubdivisionWeight (band->activeColumnIndex(),
                                          control->getValueNormalized());
        return;
    }

    // Subdivision quick-clears ("n=0" / "nd=0" / "nt=0"): zero a whole
    // variant group at once. Momentary CTextButton presses, so the edge
    // latch updates on EVERY echo (same rule as the mode switch -- the min
    // echo is what resets it, short-circuiting would leave one dead click
    // per pair).
    if (control->getTag() == kTagClearPlain
        || control->getTag() == kTagClearDotted
        || control->getTag() == kTagClearTriplet)
    {
        const bool edge = pressedEdge (
            *control, control->getTag() == kTagClearPlain
                          ? lastZeroPlainPressed_
                          : control->getTag() == kTagClearDotted
                                ? lastZeroDottedPressed_
                                : lastZeroTripletPressed_);
        if (edge)
        {
            const auto variant = control->getTag() == kTagClearPlain
                                     ? state::NoteValueVariant::plain
                                     : control->getTag() == kTagClearDotted
                                           ? state::NoteValueVariant::dotted
                                           : state::NoteValueVariant::triplet;
            owner_->setSubdivisionGroupZero (variant);
            if (intervalBand_ != nullptr)
                intervalBand_->invalid();   // repaint on the first click
        }
        return;
    }

    // Style-probability sliders: tag = kTagStyleProbBase + style ordinal,
    // value is the raw weight in [0,1].
    if (control->getTag() >= kTagStyleProbBase
        && control->getTag() < kTagStyleProbBase + state::kNumPlaybackStyles)
    {
        const int idx = static_cast<int> (control->getTag() - kTagStyleProbBase);
        owner_->setStyleWeight (idx, control->getValueNormalized());
        return;
    }

    applyParamFromControl (*control);
}

//------------------------------------------------------------------------
bool NeditEditor::pressedEdge (CControl& control, bool& lastPressed)
{
    const bool pressed = control.getValue() > 0.5f;
    const bool edge = pressed && ! lastPressed;
    lastPressed = pressed;
    return edge;
}

//------------------------------------------------------------------------
void NeditEditor::applyParamFromControl (CControl& control)
{
    const auto tag = control.getTag();

    if (tag < 0 || ! isValidParamId (static_cast<std::uint32_t> (tag)))
        return;

    double norm = 0.0;
    if (auto* menu = dynamic_cast<COptionMenu*> (&control))
    {
        const auto steps = std::max (1, menu->getNbEntries() - 1);
        norm = static_cast<double> (menu->getCurrentIndex())
             / static_cast<double> (steps);
    }
    else
    {
        norm = control.getValueNormalized();
    }

    setParam (static_cast<std::uint32_t> (tag), static_cast<float> (norm));
}

//------------------------------------------------------------------------
void NeditEditor::setParam (std::uint32_t id, float normalized)
{
    if (! isValidParamId (id))
        return;

    const auto paramId = static_cast<Steinberg::Vst::ParamID> (id);

    auto* ec = getController();
    ec->beginEdit (paramId);
    ec->setParamNormalized (paramId, static_cast<Steinberg::Vst::ParamValue> (normalized));
    ec->performEdit (paramId, static_cast<Steinberg::Vst::ParamValue> (normalized));
    ec->endEdit (paramId);
}

//------------------------------------------------------------------------
void NeditEditor::beginEdit (VSTGUI_INT32 index)
{
    if (index >= 0 && isValidParamId (static_cast<std::uint32_t> (index)))
        VSTGUIEditor::beginEdit (index);
}

//------------------------------------------------------------------------
void NeditEditor::endEdit (VSTGUI_INT32 index)
{
    if (index >= 0 && isValidParamId (static_cast<std::uint32_t> (index)))
        VSTGUIEditor::endEdit (index);
}

namespace {

#if __linux__
// Linux-only: spawn zenity/kdialog and read its stdout on a pipe. This is
// deliberately threaded -- VSTGUI's Linux fileselector blocks the UI
// run-loop thread on a pipe read while zenity runs, wedging the embedded
// X11 window. The Windows/macOS backends run their own message loops and
// don't have this pathology, so they use the selector directly.
[[nodiscard]] std::string runNativeFileDialog()
{
    if (const char* forced = std::getenv ("NEDIT_TEST_FILE"))
        return forced;

    const char* cmd = nullptr;
    if (::access ("/usr/bin/zenity", X_OK) == 0)
        cmd = "/usr/bin/zenity --file-selection --title='Load Sample' "
              "--file-filter='Audio | *.wav *.WAV *.aif *.aiff' "
              "--file-filter='All files | *' 2>/dev/null";
    else if (::access ("/usr/bin/kdialog", X_OK) == 0)
        cmd = "/usr/bin/kdialog --getopenfilename . "
              "'*.wav *.WAV *.aif *.aiff|Audio files' 2>/dev/null";
    else
        return {};

    std::FILE* pipe = ::popen (cmd, "r");
    if (pipe == nullptr)
        return {};

    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread (buf, 1, sizeof (buf), pipe)) > 0)
        out.append (buf, n);
    ::pclose (pipe);

    while (! out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}
#endif

} // namespace

//------------------------------------------------------------------------
void NeditEditor::runFileSelector()
{
#if __linux__
    if (! fileDialog_)
        fileDialog_ = std::make_shared<FileDialogState>();

    if (fileDialog_->running.exchange (true))
        return;

    fileDialog_->ready.store (false);

    auto state = fileDialog_;
    std::thread ([state]() {
        std::string path = runNativeFileDialog();
        {
            std::lock_guard<std::mutex> lock (state->mutex);
            state->path = std::move (path);
        }
        state->ready.store (true);
        state->running.store (false);
    }).detach();
#else
    // Windows/macOS: the native detector pumps its own event loop, so it
    // can run synchronously on the UI thread.
    auto* selector = CNewFileSelector::create (frame, CNewFileSelector::kSelectFile);
    if (selector == nullptr)
        return;

    selector->setTitle ("Load Sample");
    selector->addFileExtension (CFileExtension ("WAV audio", "wav"));

    selector->run ([this] (CNewFileSelector* s) {
        if (s->getNumSelectedFiles() > 0)
            owner_->requestSampleLoad (s->getSelectedFile (0));
    });
    selector->forget();
#endif
}

//------------------------------------------------------------------------
CMessageResult NeditEditor::notify (CBaseObject* sender, IdStringPtr message)
{
    if (message == CVSTGUITimer::kMsgTimer)
    {
        if (std::getenv ("NEDIT_TEST_AUTOLOAD") && ! testAutoloadFired_)
        {
            testAutoloadFired_ = true;
            runFileSelector();
        }

        if (fileDialog_ && fileDialog_->ready.exchange (false))
        {
            std::string path;
            {
                std::lock_guard<std::mutex> lock (fileDialog_->mutex);
                path = fileDialog_->path;
            }
            if (! path.empty())
                owner_->requestSampleLoad (path);
        }

        // Fold the audio thread's audition auto-stop (transport started
        // while auditioning) back into state on THIS thread -- the audio
        // thread only raises a flag, it never mutates/publishes uiState_.
        // Must run before the audition-button sync below so the button
        // reflects the fold in the same tick.
        owner_->pollAuditionAutoStop();

        // Sync buttons on sample presence or audition state changes.
        const bool samplePresent = owner_->hasSample();
        const bool auditionOn    = owner_->uiStateView().ui.auditionEnabled;
        const bool sampleChanged = samplePresent != lastSampleSync_;
        const bool auditionChanged = auditionOn != lastAuditionSync_;
        lastSampleSync_ = samplePresent;
        lastAuditionSync_ = auditionOn;

        if (auditionChanged || sampleChanged)
            styleAuditionButton();

        // Toolbar controls (bars / BPM / override) — cheap deduped push so
        // host automation and state restores surface without waiting for a
        // user edit.
        syncToolBarControls();
        syncTabBar();
        syncGenerateControls();   // timing modes/options/interval weights
        syncSequencerTransport(); // Sequence transport bar (deduped)

        if (sampleChanged && loadBtn_)
        {
            const CColor fill = samplePresent ? kAccentPressed : kAccent;
            GradientColorStopMap stops;
            stops.emplace (0., fill);
            stops.emplace (1., fill);
            loadBtn_->setGradient (CGradient::create (stops));
        }

        // Sync sample name chip.
        const auto& path = owner_->uiStateView().sample.samplePath;
        if (path != lastSamplePath_)
        {
            lastSamplePath_ = path;
            if (sampleNameLabel_)
            {
                if (path.empty())
                    sampleNameLabel_->setText ("No sample");
                else
                {
                    // Extract filename from path.
                    const auto pos = path.find_last_of ("/\\");
                    const auto name = (pos != std::string::npos) ? path.substr (pos + 1)
                                                                 : path;
                    sampleNameLabel_->setText (name.c_str());
                }
            }
        }

        // Refresh waveform view when sample changes.
        if (sampleChanged && waveformView_)
            waveformView_->refresh();

        // Live sequencer playhead: repaint the grid when the playing step
        // moves (or the sample changed). Paint/extension edits self-
        // invalidate, so no per-tick repaint is needed when nothing moves.
        if (sequencerGrid_ != nullptr && sequencerGrid_->isVisible())
        {
            const int step = owner_->debugScheduler().playingStepIndex();
            if (step != lastPlayingStepSync_ || sampleChanged)
            {
                lastPlayingStepSync_ = step;
                sequencerGrid_->invalid();
            }
        }
    }
    return VSTGUIEditor::notify (sender, message);
}

} // namespace nedit::plugin
