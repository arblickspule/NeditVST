// Nedit -- Plugin layer. See NeditEditor.h.

#include "NeditEditor.h"

#include "NeditProcessor.h"
#include "ParameterSurface.h"

#include <ui/WaveformGeometry.h>

#include "vstgui/lib/ccolor.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cfileselector.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/controls/cbuttons.h"
#include "vstgui/lib/controls/coptionmenu.h"
#include "vstgui/lib/controls/ctextlabel.h"
#include "vstgui/lib/cvstguitimer.h"
#include "vstgui/lib/events.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nedit::plugin {

using namespace VSTGUI;

namespace {

constexpr int kEditorWidth = 760;
constexpr int kEditorHeight = 440;
constexpr int kTopBarHeight = 40;

const CColor kColorBackground { 24, 24, 28, 255 };
const CColor kColorPanel { 34, 34, 40, 255 };
const CColor kColorWave { 120, 190, 255, 255 };
const CColor kColorMarker { 255, 150, 60, 200 };
const CColor kColorText { 220, 220, 224, 255 };

Steinberg::ViewRect kEditorRect (0, 0, kEditorWidth, kEditorHeight);

const char* const kTriggerModeNames[5] = { "Slice Length", "Clock", "Sequenced",
                                           "Performance", "Control" };

} // namespace

//------------------------------------------------------------------------
// WaveformView: stateless renderer over the processor's sample slot and
// UiState; wheel = anchored zoom, drag = pan (both persisted in UiState).
//------------------------------------------------------------------------
class WaveformView : public CView
{
public:
    WaveformView (const CRect& size, NeditProcessor* owner)
        : CView (size), owner_ (owner)
    {
    }

    void draw (CDrawContext* dc) override
    {
        const CRect r = getViewSize();

        dc->setFillColor (kColorPanel);
        dc->drawRect (r, kDrawFilled);

        const auto loaded = owner_->acquireLoadedSample();
        const auto& st = owner_->uiStateView();

        if (loaded == nullptr || loaded->audio.frames <= 0)
        {
            dc->setFontColor (kColorText);
            dc->drawString ("Load a sample to begin", CPoint (r.left + 16.0, r.top + 28.0));
            setDirty (false);
            return;
        }

        std::vector<const float*> channels;
        channels.reserve (loaded->audio.channels.size());
        for (const auto& c : loaded->audio.channels)
            channels.push_back (c.data());

        const auto width = std::max (1, static_cast<int> (r.getWidth()));
        const auto peaks = ui::computeWaveformPeaks (
            channels.data(), static_cast<int> (channels.size()), loaded->audio.frames,
            st.sample.trimStartFrame, st.sample.trimEndFrame, st.ui, width);

        const double midY = r.top + r.getHeight() / 2.0;
        const double halfH = r.getHeight() / 2.0 - 3.0;

        dc->setFrameColor (kColorWave);
        dc->setLineWidth (1.0);

        for (std::size_t c = 0; c < peaks.size(); ++c)
        {
            const double x = r.left + static_cast<double> (c);
            const double yTop = midY - static_cast<double> (peaks[c].max) * halfH;
            const double yBottom = midY - static_cast<double> (peaks[c].min) * halfH;
            dc->drawLine (CPoint (x, yTop), CPoint (x, std::max (yBottom, yTop + 1.0)));
        }

        const auto markers = ui::computeSliceMarkerX (
            loaded->slices, st.sample.trimStartFrame, st.sample.trimEndFrame, st.ui,
            r.getWidth());

        dc->setFrameColor (kColorMarker);
        for (const double mx : markers)
        {
            const double x = r.left + mx;
            dc->drawLine (CPoint (x, r.top), CPoint (x, r.bottom));
        }

        setDirty (false);
    }

    void onMouseWheelEvent (MouseWheelEvent& event) override
    {
        const CRect r = getViewSize();
        if (r.getWidth() <= 0.0)
            return;

        const auto& st = owner_->uiStateView();
        const double span = st.ui.visibleEndNorm - st.ui.visibleStartNorm;
        const double anchorNorm = st.ui.visibleStartNorm
                                + ((event.mousePosition.x - r.left) / r.getWidth()) * span;

        const double factor = event.deltaY > 0.0 ? 1.25 : 0.8;
        const auto w = ui::zoomedWindow (st.ui, anchorNorm, factor);
        owner_->setVisibleWindow (w.start, w.end);

        invalid();
        event.consumed = true;
    }

    void onMouseDownEvent (MouseDownEvent& event) override
    {
        if (event.buttonState.isLeft())
        {
            dragging_ = true;
            lastDragX_ = event.mousePosition.x;
            event.consumed = true;
        }
    }

    void onMouseMoveEvent (MouseMoveEvent& event) override
    {
        if (! dragging_)
            return;

        const CRect r = getViewSize();
        if (r.getWidth() <= 0.0)
            return;

        const auto& st = owner_->uiStateView();
        const double span = st.ui.visibleEndNorm - st.ui.visibleStartNorm;
        const double deltaNorm =
            -(event.mousePosition.x - lastDragX_) / r.getWidth() * span;
        lastDragX_ = event.mousePosition.x;

        const auto w = ui::pannedWindow (st.ui, deltaNorm);
        owner_->setVisibleWindow (w.start, w.end);

        invalid();
        event.consumed = true;
    }

    void onMouseUpEvent (MouseUpEvent& event) override
    {
        if (dragging_)
        {
            dragging_ = false;
            event.consumed = true;
        }
    }

private:
    NeditProcessor* owner_ = nullptr;
    bool dragging_ = false;
    double lastDragX_ = 0.0;
};

//------------------------------------------------------------------------
NeditEditor::NeditEditor (NeditProcessor* owner)
    : VSTGUIEditor (owner, &kEditorRect), owner_ (owner)
{
}

//------------------------------------------------------------------------
bool PLUGIN_API NeditEditor::open (void* parent, const PlatformType& platformType)
{
    CRect frameSize (0, 0, kEditorWidth, kEditorHeight);

    frame = new CFrame (frameSize, this);
    frame->setBackgroundColor (kColorBackground);

    if (! frame->open (parent, platformType))
    {
        frame->forget();
        frame = nullptr;
        return false;
    }

    // --- top bar -------------------------------------------------------------
    auto* title = new CTextLabel (CRect (12, 8, 130, kTopBarHeight - 8), "NEDIT");
    title->setBackColor (kTransparentCColor);
    title->setFrameColor (kTransparentCColor);
    title->setFontColor (kColorText);
    title->setHoriAlign (kLeftText);
    frame->addView (title);

    auto* modeMenu = new COptionMenu (CRect (140, 8, 320, kTopBarHeight - 8), this,
                                      static_cast<std::int32_t> (kParamTriggerMode));
    for (const char* name : kTriggerModeNames)
        modeMenu->addEntry (name);
    const auto modeNorm = getController()->getParamNormalized (kParamTriggerMode);
    modeMenu->setCurrent (static_cast<std::int32_t> (
        std::lround (modeNorm * 4.0)));
    frame->addView (modeMenu);

    auto* loadButton = new CTextButton (
        CRect (kEditorWidth - 170, 8, kEditorWidth - 12, kTopBarHeight - 8), this,
        kTagLoadSample, "Load Sample...");
    frame->addView (loadButton);

    // --- waveform ------------------------------------------------------------
    waveform_ = new WaveformView (
        CRect (12, kTopBarHeight + 8, kEditorWidth - 12, kEditorHeight - 12), owner_);
    frame->addView (waveform_);

    lastSampleIdentity_ = owner_->acquireLoadedSample().get();

    setIdleRate (60);   // ms; drives notify() below
    return true;
}

//------------------------------------------------------------------------
void PLUGIN_API NeditEditor::close()
{
    if (frame != nullptr)
    {
        waveform_ = nullptr;   // owned (and destroyed) by the frame
        frame->close();        // also forgets the frame
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
        // Fire on release only (buttons emit 1 then 0).
        if (control->getValue() > 0.5f)
            runFileSelector();
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
void NeditEditor::runFileSelector()
{
    auto* selector = CNewFileSelector::create (frame, CNewFileSelector::kSelectFile);
    if (selector == nullptr)
        return;

    selector->setTitle ("Load Sample");
    selector->addFileExtension (CFileExtension ("WAV audio", "wav"));

    selector->run ([this] (CNewFileSelector* s) {
        if (s->getNumSelectedFiles() > 0)
        {
            owner_->requestSampleLoad (s->getSelectedFile (0));
            if (waveform_ != nullptr)
                waveform_->invalid();
        }
    });
    selector->forget();
}

//------------------------------------------------------------------------
CMessageResult NeditEditor::notify (CBaseObject* sender, IdStringPtr message)
{
    if (message == CVSTGUITimer::kMsgTimer)
    {
        // Redraw the waveform when the sample slot changes underneath us
        // (loads can come from state restore, not just our own button).
        const void* identity = owner_->acquireLoadedSample().get();
        if (identity != lastSampleIdentity_)
        {
            lastSampleIdentity_ = identity;
            if (waveform_ != nullptr)
                waveform_->invalid();
        }
    }
    return VSTGUIEditor::notify (sender, message);
}

} // namespace nedit::plugin
