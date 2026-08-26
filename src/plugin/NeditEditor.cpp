// Nedit -- Plugin layer. See NeditEditor.h.

#include "NeditEditor.h"

#include "NeditProcessor.h"
#include "ParameterSurface.h"

#include "vstgui/lib/ccolor.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/cgradient.h"
#include "vstgui/lib/controls/cbuttons.h"
#include "vstgui/lib/controls/ctextlabel.h"
#include "vstgui/lib/cvstguitimer.h"
#include "vstgui/lib/events.h"

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

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
constexpr auto kTagAudition = static_cast<VSTGUI_INT32> (1001);

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

} // namespace

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

    setIdleRate (60);
    return true;
}

//------------------------------------------------------------------------
void PLUGIN_API NeditEditor::close()
{
    sampleNameLabel_ = nullptr;
    auditionBtn_ = nullptr;
    loadBtn_ = nullptr;
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
        if (control->getValue() > 0.5f)
            runFileSelector();
        return;
    }

    if (control->getTag() == kTagAudition)
    {
        if (control->getValue() > 0.5f && owner_->hasSample())
        {
            const bool wasOn = owner_->uiStateView().ui.auditionEnabled;
            owner_->setAuditionEnabled (! wasOn);
            styleAuditionButton();
        }
        return;
    }

    applyParamFromControl (*control);
}

//------------------------------------------------------------------------
void NeditEditor::applyParamFromControl (CControl& control)
{
    const auto tag = control.getTag();

    if (tag < 0 || ! isValidParamId (static_cast<std::uint32_t> (tag)))
        return;

    const auto id = static_cast<Steinberg::Vst::ParamID> (tag);

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

    auto* ec = getController();
    ec->beginEdit (id);
    ec->setParamNormalized (id, norm);
    ec->performEdit (id, norm);
    ec->endEdit (id);
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

} // namespace

//------------------------------------------------------------------------
void NeditEditor::runFileSelector()
{
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
    }
    return VSTGUIEditor::notify (sender, message);
}

} // namespace nedit::plugin
