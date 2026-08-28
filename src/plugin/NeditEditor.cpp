// Nedit -- Plugin layer. See NeditEditor.h.

#include "NeditEditor.h"

#include "NeditProcessor.h"
#include "ParameterSurface.h"
#include "WaveformView.h"

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

// Column param row: one parameter shown in a style column's space under
// the % readout and right of the vertical weight slider, following the
// Flanger pilot recipe. Continuous params get a horizontal mini-slider
// under their caption; discrete params are mini dropdowns whose caption
// IS the current option. Volume rides last in every column.
struct StyleParamRow
{
    state::StyleParamId id;
    bool continuous;
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
        rows.push_back ({ id, ! info.discrete });
        if (info.swept)
        {
            const auto mode = static_cast<state::StyleParamId> (
                static_cast<int> (id) + 1);
            if (mode != state::StyleParamId::volumeMode)
                rows.push_back ({ mode, false });
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
          rows_ (columnParamsFor (static_cast<state::PlaybackStyle> (styleIndex)))
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
    // block above the list is included, plus 1px leading.
    [[nodiscard]] double captionRowLocalY (std::size_t i) const noexcept
    {
        double y = kProbChipH + kProbChipReadoutGap + kProbReadoutH
                 + kProbReadoutSliderGap + 1.0;
        for (std::size_t j = 0; j < i; ++j)
            y += paramEntrySpan (rows_[j]);
        return y;
    }

    [[nodiscard]] double sliderRowLocalY (std::size_t i) const noexcept
    {
        return captionRowLocalY (i) + kProbRowH + kProbCaptionSliderGap;
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

        // ── Vertical slider, aligned left, spanning the remaining height.
        // Track = surface, fill = the style colour, tick at the value.
        const double top = sliderTop (r);
        const double bottom = r.bottom - kProbBottomPad;
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

        // ── Param controls: every column hosts its style's params in the ──
        // space under the % and right of the slider. Subdivide is dropped
        // (a SEQUENCER retrigger function, not a style effect). Continuous
        // params get a 12px caption row here + a ParamMiniSlider (drawn on
        // top) in the 10px row beneath; discrete params' rows are owned
        // entirely by their ParamMiniMenu on top (caption + current value).
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
                dc->drawString (state::styleParamInfo (rows_[i].id).name,
                                CRect (listX, labelY, listX + listW, labelY + kProbRowH),
                                kLeftText);
            }
        }

        setDirty (false);
    }

    CMouseEventResult onMouseDown (CPoint& where, const CButtonState& buttons) override
    {
        if (! buttons.isLeftButton())
            return kMouseEventNotHandled;
        // Hit-test only the actual slider track (a narrow vertical bar at
        // the column's left); clicks on the chip / % / parameter list fall
        // through to the views behind. Once grabbed, the drag adjusts on
        // any x so the pointer can leave the few-pixel track.
        const double x = where.x - getViewSize().left;
        if (x < kProbTrackX || x > kProbTrackX + kProbTrackW)
            return kMouseEventNotHandled;
        dragging_ = true;
        applyFromY (where.y - getViewSize().top);
        return kMouseEventHandled;
    }

    CMouseEventResult onMouseMoved (CPoint& where, const CButtonState&) override
    {
        if (! dragging_)
            return kMouseEventNotHandled;
        applyFromY (where.y - getViewSize().top);
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

    void applyFromY (double localY)
    {
        // localY arrived in the column's OWN coordinate space (already
        // offset by getViewSize().top in onMouseDown/Moved), so the slider
        // band must be described in the same local space -- NOT the
        // absolute-ized sliderTop() used for drawing.
        const double top = sliderBandTopLocal();
        const double bottom = getViewSize().getHeight() - kProbBottomPad;
        const double span = bottom - top;
        double v = span > 0.0 ? 1.0 - (localY - top) / span : 0.0;
        if (v < 0.0)
            v = 0.0;
        if (v > 1.0)
            v = 1.0;
        setValueNormalized (static_cast<float> (v));
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
    std::vector<StyleParamRow> rows_;
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

// ── Mini dropdown for ONE discrete style param, occupying the 12px ──
// caption row of the Flanger column (the current option IS the row's
// label). Subclassed from COptionMenu so the click publishes its own
// popup (platform list, anchored at this row) and the value stays an
// entry index routed straight through the host-edit protocol like the
// mini-sliders; draw() keeps the row as slim and quiet as the captions
// (no 19px menu chrome). Tag = the StyleParamId (< 1000).
class ParamMiniMenu : public COptionMenu
{
public:
    ParamMiniMenu (const CRect& size, IControlListener* l, int32_t t, int styleIndex)
        : COptionMenu (size, l, t), styleIndex_ (styleIndex)
    {
        const auto param = static_cast<state::StyleParamId> (t);
        const auto& info = state::styleParamInfo (param);
        for (int i = 0; i < info.numOptions; ++i)
            addEntry (state::styleParamOptionName (param, i));
    }

    CBaseObject* newCopy () const override
    {
        auto* copy = new ParamMiniMenu (getViewSize(), getListener(), getTag(), styleIndex_);
        copy->setValue (getValue());
        return copy;
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();
        dc->setDrawMode (kAliasing);

        const auto param = static_cast<state::StyleParamId> (getTag());
        const auto idx = static_cast<int> (getValue());

        // The current option is the row's only text, tinted like a value
        // readout (interactive, unlike the muted caption rows).
        dc->setFont (kNormalFontSmaller);
        dc->setFontColor (kAccentMutedHi);
        dc->drawString (state::styleParamOptionName (param, idx),
                        CRect (r.left + 2, r.top, r.right - 12, r.bottom), kLeftText);

        // Caret triangle, drawn (the default font's glyph set is not
        // guaranteed to include ▾), antialiased for the small size.
        const double cx = r.right - 7.0;
        const double cy = r.top + r.getHeight() / 2.0;
        dc->setDrawMode (kAntiAliasing);
        dc->setFillColor (kTextSecondary);
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
    int styleIndex_ = 0;
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
            // mark the band's bottom edge and hint at what sits below.
            const double bandBottom = r.top + kCardPad + kStyleBandH;
            dc->setDrawMode (kAliasing);
            dc->setFrameColor (kOutline);
            dc->setLineWidth (1);
            dc->drawLine (CPoint (r.left + kCardPad, bandBottom),
                          CPoint (r.right - kCardPad, bandBottom));

            dc->setFont (kNormalFontSmaller);
            dc->setFontColor (kTextDisabled);
            dc->drawString (hintBelow (tab),
                            CRect (r.left + kCardPad, r.bottom - 18,
                                   r.right - kCardPad, r.bottom - 8),
                            kLeftText);
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
                   ? "style parameters (pending)"
                   : "step grid · pattern controls (pending)";
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
NeditEditor::NeditEditor (NeditProcessor* owner)
    : VSTGUIEditor (owner, &kEditorRect), owner_ (owner)
{
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
    // COptionMenu::setValue so the valueChanged echo stays silent.
    for (auto& column : paramMiniMenus_)
        for (auto* menu : column)
        {
            if (menu == nullptr)
                continue;
            const float norm = toNormalized (owner_->uiStateView(),
                                             static_cast<std::uint32_t> (menu->getTag()));
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

    // Every column hosts its style's params: continuous params get a 12px
    // horizontal mini-slider row directly under their caption, discrete
    // params a mini dropdown whose caption row IS the control (rows from
    // columnParamsFor -- Subdivide/Volume-Mode skipped, swept params pair
    // with their *Mode, Volume last). Tags = the StyleParamId (=
    // ParameterSurface id < 1000), so edits route through the host edit
    // protocol. Added after the prob sliders, so they sit on top for
    // hit-testing; the columns' caption rows line up via the shared kProb*
    // geometry and the same captionRowLocalY the columns draw with.
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
                    this, static_cast<VSTGUI_INT32> (static_cast<std::uint32_t> (row.id)),
                    col);
                frame->addView (menu);
                paramMiniMenus_[static_cast<std::size_t> (col)]
                               [i] = menu;
                continue;
            }

            const double localY = prob->sliderRowLocalY (i);
            auto* mini = new ui::ParamMiniSlider (
                CRect (listX, bandTop + localY, listX + listW,
                       bandTop + localY + kProbSliderRowH),
                this, static_cast<VSTGUI_INT32> (static_cast<std::uint32_t> (row.id)),
                col, paramReadoutFormat (row.id));
            frame->addView (mini);
            paramMiniSliders_[static_cast<std::size_t> (col)]
                             [i] = mini;
        }
    }

    syncTabBar();   // seed the strip + panel from the restored activeTab

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
        owner_->setActiveTab (static_cast<state::UiTab> (ord));
        if (panelView_)
            panelView_->invalid();   // the panel area re-renders per page
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
    }
    return VSTGUIEditor::notify (sender, message);
}

} // namespace nedit::plugin
