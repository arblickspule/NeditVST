// Nedit -- Plugin layer. See NeditEditor.h.

#include "NeditEditor.h"

#include "NeditProcessor.h"
#include "ParameterSurface.h"
#include "WaveformView.h"

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
const CColor kAccentOn      {  32,  16,  13, 255 }; // on-salmon   text on accent fill
const CColor kAccentHover   { 224, 104,  90, 255 }; // salmon-500  hover
const CColor kAccentPressed { 194,  85,  72, 255 }; // salmon-600  pressed

// ── Control tags ───────────────────────────────────────────────────────
constexpr auto kTagAudition   = static_cast<VSTGUI_INT32> (1001);
constexpr auto kTagSensitivity = static_cast<VSTGUI_INT32> (1002);
constexpr auto kTagQuantize   = static_cast<VSTGUI_INT32> (1003);
constexpr auto kTagQuantizeGrid = static_cast<VSTGUI_INT32> (1004);
constexpr auto kTagFadeIn   = static_cast<VSTGUI_INT32> (1005);
constexpr auto kTagFadeOut  = static_cast<VSTGUI_INT32> (1006);

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

        dc->setFrameColor (active_ ? kAccent : kOutline);
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
        dc->setFontColor (active_ ? kAccentBright : kTextDisabled);
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

        dc->setFrameColor (active_ ? kAccent : kOutline);
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

// ── Fade slider: per-pick declick fade in ms, 0..100. ─────────────────────
// (editor-local tags 1005/1006, NOT host params -- attack = render.fadeInMs,
// release = render.fadeOutMs, a global play-feel setting like the original's
// continuous Fade In/Out sliders). The caption row doubles as the ms readout:
// label left, current value right. Track fill + frame mirror the SENS slider;
// always active (valid without a sample). Wheel nudges by 5 ms.
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
        const CRect box (r.left, r.top + kBoxTop, r.right, r.top + kBoxTop + kBoxH);

        dc->setDrawMode (kAliasing);
        dc->setFont (kNormalFontSmall);
        const float v = getValueNormalized();
        dc->setFontColor (kTextSecondary);
        dc->drawString (label_.c_str(),
                        CRect (r.left, r.top + 2, r.right - 52, r.top + 12), kLeftText);

        char buf[24];
        std::snprintf (buf, sizeof (buf), "%dms",
                       static_cast<int> (std::lround (msFromValue (v))));
        dc->setFontColor (kAccentBright);
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

        dc->setFrameColor (kAccent);
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

        applyValue (msFromValue (getValueNormalized()) + event.deltaY * 5.0);
        event.consumed = true;
    }

private:
    static constexpr double kMinMs = 0.0;
    static constexpr double kMaxMs = 100.0;

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
// Push the live model state into the toolbar controls. Runs on the idle
// timer and after local edits so host automation / state loads / box-tool
// changes all surface. Only pushes on change; the controls repaint lazily.
void NeditEditor::syncToolBarControls()
{
    if (! barsStepper_ || ! bpmField_ || ! quantizeMenu_ || ! fadeInSlider_
        || ! fadeOutSlider_)
        return;

    const auto& sample = owner_->uiStateView().sample;

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

    const float fadeInMs = owner_->uiStateView().render.fadeInMs;
    if (fadeInMs != lastFadeInSync_)
    {
        lastFadeInSync_ = fadeInMs;
        fadeInSlider_->setValueNormalized (fadeInMs / 100.0f);
        fadeInSlider_->invalid();
    }

    const float fadeOutMs = owner_->uiStateView().render.fadeOutMs;
    if (fadeOutMs != lastFadeOutSync_)
    {
        lastFadeOutSync_ = fadeOutMs;
        fadeOutSlider_->setValueNormalized (fadeOutMs / 100.0f);
        fadeOutSlider_->invalid();
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
    constexpr int kBarsW = 88;
    constexpr int kBpmW = 112;
    constexpr int kOverrideW = 88;
    constexpr int kSensW = 110;
    constexpr int kQuantW = 88;
    constexpr int kGridW = 110;
    constexpr int kFadeW = 110;
    constexpr int kX0 = 16;
    constexpr int kGap = 18;

    const int barsX = kX0;
    const int bpmX = barsX + kBarsW + kGap;
    const int overX = bpmX + kBpmW + kGap;
    const int sensX = overX + kOverrideW + kGap;
    const int quantX = sensX + kSensW + kGap;
    const int gridX = quantX + kQuantW + kGap;
    const int fadeInX  = gridX + kGridW + kGap;
    const int fadeOutX = fadeInX + kFadeW + kGap;
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
    // pick length during rendering.
    auto* fadeIn = new ui::FadeSlider (
        CRect (fadeInX, strip.top, fadeInX + kFadeW, strip.bottom), this,
        kTagFadeIn, "ATTACK");
    frame->addView (fadeIn);
    fadeInSlider_ = fadeIn;

    auto* fadeOut = new ui::FadeSlider (
        CRect (fadeOutX, strip.top, fadeOutX + kFadeW, strip.bottom), this,
        kTagFadeOut, "RELEASE");
    frame->addView (fadeOut);
    fadeOutSlider_ = fadeOut;

    syncToolBarControls();

    // ── Waveform display (below the tool bar) ──────────────────────────
    constexpr int kWaveformH = 144;
    constexpr int kWaveTop  = kAppBarHeight + kToolBarHeight;
    auto* wave = new WaveformView (*owner_, this, kWaveformH);
    wave->setViewSize (CRect (0, kWaveTop, kEditorWidth, kWaveTop + kWaveformH));
    frame->addView (wave);
    waveformView_ = wave;

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
    fadeInSlider_ = nullptr;
    fadeOutSlider_ = nullptr;
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
        owner_->setFadeInMs (100.0f * control->getValueNormalized());
        return;
    }

    if (control->getTag() == kTagFadeOut)
    {
        owner_->setFadeOutMs (100.0f * control->getValueNormalized());
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
