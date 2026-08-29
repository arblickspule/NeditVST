// Nedit -- Plugin layer. WaveformView implementation.

#include "WaveformView.h"

#include "NeditProcessor.h"
#include "engine/Slice.h"
#include "state/SampleState.h"

#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/cgraphicspath.h"
#include "vstgui/lib/events.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nedit::plugin {

using namespace VSTGUI;

// ── Design tokens (Graphite & Salmon) ──────────────────────────────────
namespace {

const CColor kWaveBg        {  20,  22,  26, 255 }; // graphite-900
const CColor kWaveform      { 100, 108, 120, 204 }; // solid peak lines
const CColor kTrimLine      { 250, 128, 114, 230 }; // salmon-400
const CColor kTrimDim       {   0,   0,   0, 153 }; // excluded regions
const CColor kSliceAuto     { 232, 234, 237, 128 }; // auto boundary
const CColor kSliceManual   { 232, 234, 237, 255 }; // manual boundary
const CColor kProbFill      { 250, 128, 114, 100 }; // salmon-400 @ 39%
const CColor kProbBorder    { 250, 128, 114, 230 }; // salmon-400
const CColor kPlayhead      { 232, 234, 237, 200 }; // primary, translucent
const CColor kSliceAuditionHl { 250, 128, 114, 60 }; // salmon wash

} // anonymous namespace

//------------------------------------------------------------------------
WaveformView::WaveformView (NeditProcessor& processor,
                            IControlListener* listener,
                            int height)
    : CView (CRect (0, 0, 0, height)), processor_ (processor), listener_ (listener)
{
}

// ── Coordinate mapping ─────────────────────────────────────────────────

int64_t WaveformView::xToSample (CCoord x) const
{
    const int64_t range = visibleEnd_ - visibleStart_;
    const CCoord w = std::max (1.0, getWidth());
    const double frac = std::clamp (x / w, 0.0, 1.0);
    return visibleStart_ + static_cast<int64_t> (frac * static_cast<double> (range));
}

CCoord WaveformView::sampleToX (int64_t sample) const
{
    const int64_t range = std::max (static_cast<int64_t> (1), visibleEnd_ - visibleStart_);
    const CCoord w = std::max (1.0, getWidth());
    return (static_cast<double> (sample - visibleStart_) / static_cast<double> (range)) * w;
}

CPoint WaveformView::toLocal (const CPoint& framePoint) const
{
    CPoint local = framePoint;
    local.offset (-getViewSize().left, -getViewSize().top);
    return local;
}

// ── Peak computation ───────────────────────────────────────────────────

void WaveformView::rebuildPeaks()
{
    const auto loaded = processor_.acquireLoadedSample();
    if (loaded == nullptr || loaded->audio == nullptr || loaded->audio->frames == 0)
    {
        peaks_.clear();
        return;
    }

    const auto& audio = *loaded->audio;
    const int w = static_cast<int> (getWidth());
    if (w <= 0)
    {
        peaks_.clear();
        return;
    }

    const int64_t range = std::max (static_cast<int64_t> (1), visibleEnd_ - visibleStart_);
    const int64_t totalFrames = audio.frames;
    const int chans = audio.channelCount();

    peaks_.resize (static_cast<std::size_t> (w));

    for (int x = 0; x < w; ++x)
    {
        const int64_t s0 = visibleStart_ + (static_cast<int64_t> (x) * range) / w;
        const int64_t s1 = visibleStart_ + (static_cast<int64_t> (x + 1) * range) / w;
        const int64_t clamped0 = std::max (static_cast<int64_t> (0), std::min (s0, totalFrames));
        const int64_t clamped1 = std::max (static_cast<int64_t> (0), std::min (s1, totalFrames));

        float lo = 0.f, hi = 0.f;

        if (clamped0 < clamped1 && chans > 0)
        {
            lo = 1.f;
            hi = -1.f;

            for (int c = 0; c < chans; ++c)
            {
                const float* data = audio.channels[static_cast<std::size_t> (c)].data();
                for (int64_t f = clamped0; f < clamped1; ++f)
                {
                    const float v = data[static_cast<std::size_t> (f)];
                    lo = std::min (lo, v);
                    hi = std::max (hi, v);
                }
            }
        }

        peaks_[static_cast<std::size_t> (x)] = { lo, hi };
    }
}

// ── Drawing ────────────────────────────────────────────────────────────

void WaveformView::draw (CDrawContext* dc)
{
    const CRect r = getViewSize();
    const CCoord w = r.getWidth();
    const CCoord h = r.getHeight();

    // Background.
    dc->setFillColor (kWaveBg);
    dc->drawRect (r, kDrawFilled);

    const auto loaded = processor_.acquireLoadedSample();
    const auto& st = processor_.uiStateView();
    const auto& sample = st.sample;
    // NB: a `cond ? lvalue : prvalue{}` ternary here would materialize a
    // COPY of the whole slice vector every draw (the conditional's common
    // type is a prvalue); bind the shared static instead.
    static const std::vector<engine::Slice> kNoSlices;
    const auto& slices = (loaded != nullptr) ? loaded->slices : kNoSlices;

    const int64_t totalFrames = sample.sampleLengthFrames;
    if (totalFrames <= 0 || w <= 0 || h <= 0)
    {
        // Placeholder.
        dc->setFont (kNormalFontSmall);
        dc->setFontColor (CColor (154, 160, 166, 255));
        dc->drawString ("Drag and drop a sample here...",
                        CRect (r.left, r.top, r.right, r.bottom),
                        kCenterText);
        setDirty (false);
        return;
    }

    // ── Dimmed excluded regions outside trim ────────────────────────────
    const int64_t trimStart = sample.trimStartFrame;
    const int64_t trimEnd = sample.trimEndFrame;
    const CCoord trimStartX = sampleToX (trimStart);
    const CCoord trimEndX = sampleToX (trimEnd);

    dc->setFillColor (kTrimDim);
    if (trimStartX > r.left)
        dc->drawRect (CRect (r.left, r.top, trimStartX, r.bottom), kDrawFilled);
    if (trimEndX < r.right)
        dc->drawRect (CRect (trimEndX, r.top, r.right, r.bottom), kDrawFilled);

    // ── Waveform peaks ─────────────────────────────────────────────────
    if (! peaks_.empty())
    {
        const CCoord midY = r.top + h * 0.5;
        const CCoord halfH = h * 0.42;

        dc->setFrameColor (kWaveform);
        dc->setLineWidth (1);

        for (CCoord x = 0; x < w; ++x)
        {
            const auto idx = static_cast<std::size_t> (x);
            if (idx >= peaks_.size())
                break;
            const auto& pk = peaks_[idx];
            const CCoord yTop = midY - pk.max * halfH;
            const CCoord yBot = midY - pk.min * halfH;
            if (yTop != yBot)
                dc->drawLine (CPoint (x, yTop), CPoint (x, yBot));
        }
    }

    // ── Slice audition highlight ────────────────────────────────────────
    if (auditionSliceIndex_ >= 0
        && auditionSliceIndex_ < static_cast<int> (slices.size()))
    {
        const auto& sl = slices[static_cast<std::size_t> (auditionSliceIndex_)];
        const CCoord sx0 = sampleToX (sl.startFrame);
        const CCoord sx1 = sampleToX (sl.endFrame);
        dc->setFillColor (kSliceAuditionHl);
        dc->drawRect (CRect (sx0, r.top, sx1, r.bottom), kDrawFilled);
    }

    // ── Slice boundaries + probability sliders ─────────────────────────
    // The trim is a live selection over the FIXED slice list: we deliberately
    // do NOT rebuild slices during a trim drag (that recompute-mid-interaction
    // pattern is exactly what caused the original editor's SIGSEGV). Instead,
    // slices outside the trim are simply hidden; widening the trim reveals
    // them again with their weights intact (soft, reversible).
    if (! slices.empty())
    {
        for (std::size_t i = 0; i < slices.size(); ++i)
        {
            const auto& sl = slices[i];

            // Relevance: the slice must overlap the current trim region.
            if (sl.endFrame <= trimStart || sl.startFrame >= trimEnd)
                continue;

            // Clip the drawn span to the trim edges so a slice straddling a
            // handle doesn't bleed a bar into the dimmed region.
            const int64_t drawStart = std::max (sl.startFrame, trimStart);
            const int64_t drawEnd   = std::min (sl.endFrame, trimEnd);
            const CCoord sx0 = sampleToX (drawStart);
            const CCoord sx1 = sampleToX (drawEnd);

            // Skip entirely off-screen slices (zoom window).
            if (sx1 < r.left || sx0 > r.right)
                continue;

            // Probability bar (fills from bottom, full slice height).
            const float prob = processor_.getSliceProbability (static_cast<int> (i));
            const CCoord probH = h * static_cast<CCoord> (prob);
            const CColor probFillC { kProbFill.red, kProbFill.green, kProbFill.blue,
                                     static_cast<std::uint8_t> (prob * 100) };
            dc->setFillColor (probFillC);
            dc->drawRect (CRect (sx0, r.bottom - probH, sx1, r.bottom), kDrawFilled);

            // Slice boundary line -- only for boundaries strictly INSIDE the
            // trim (the trim edges themselves are drawn as trim handles).
            if (sl.startFrame > trimStart && sl.startFrame < trimEnd)
            {
                // A boundary is "manual" when it coincides with a user-placed
                // point (manual points are merged un-quantized, so the frame
                // matches exactly). Manual markers draw brighter and thicker.
                bool isManual = false;
                for (const auto& mp : sample.manualPoints)
                    if (mp.position == sl.startFrame) { isManual = true; break; }

                const CCoord lineX = sampleToX (sl.startFrame);
                dc->setFrameColor (isManual ? kSliceManual : kSliceAuto);
                dc->setLineWidth (isManual ? 2 : 1);
                dc->drawLine (CPoint (lineX, r.top), CPoint (lineX, r.bottom));
            }
        }
    }

    // ── Trim handles ───────────────────────────────────────────────────
    // Yellow flagged lines with triangle flags.
    dc->setFrameColor (kTrimLine);
    dc->setFillColor (kTrimLine);
    dc->setLineWidth (2);

    // Start handle.
    {
        const CCoord x = trimStartX;
        dc->drawLine (CPoint (x, r.top + 10), CPoint (x, r.bottom));
        // Flag: right-pointing triangle.
        if (auto* path = dc->createGraphicsPath())
        {
            path->beginSubpath (x, r.top);
            path->addLine (x + 9, r.top + 5);
            path->addLine (x, r.top + 10);
            path->closeSubpath ();
            dc->setDrawMode (kAntiAliasing);
            dc->drawGraphicsPath (path, CDrawContext::kPathFilled);
            dc->setDrawMode (kAliasing);
            path->forget ();
        }
    }

    // End handle.
    {
        const CCoord x = trimEndX;
        dc->drawLine (CPoint (x, r.top + 10), CPoint (x, r.bottom));
        if (auto* path = dc->createGraphicsPath())
        {
            path->beginSubpath (x, r.top);
            path->addLine (x - 9, r.top + 5);
            path->addLine (x, r.top + 10);
            path->closeSubpath ();
            dc->setDrawMode (kAntiAliasing);
            dc->drawGraphicsPath (path, CDrawContext::kPathFilled);
            dc->setDrawMode (kAliasing);
            path->forget ();
        }
    }

    // ── Generative playhead ─────────────────────────────────────────────
    if (st.ui.auditionEnabled)
    {
        // Audition is on — draw the audition read position.
        // (The processor doesn't yet expose auditionPosition, so skip.)
    }

    // ── Hairline top/bottom edges ───────────────────────────────────────
    dc->setFrameColor (CColor (51, 56, 63, 255)); // outline
    dc->setLineWidth (1);
    dc->drawLine (CPoint (r.left, r.top), CPoint (r.right, r.top));
    dc->drawLine (CPoint (r.left, r.bottom - 1), CPoint (r.right, r.bottom - 1));

    setDirty (false);
}

// ── Mouse events ───────────────────────────────────────────────────────

CMouseEventResult WaveformView::onMouseDown (CPoint& where, const CButtonState& buttons)
{
    const CRect r = getViewSize();
    const CPoint p = toLocal (where);

    // Did the IMMEDIATELY preceding mouse-down paint a probability? Snapshot
    // it, then clear -- only the paint branch below re-arms it. A double-
    // click's second down reads this to undo the stray paint from its first.
    const bool prevDownPainted = lastDownPainted_;
    lastDownPainted_ = false;

    // ── RMB: slice audition ────────────────────────────────────────────
    if (buttons.isRightButton())
    {
        const int idx = getSliceIndexAtX (p.x);
        if (idx >= 0)
        {
            const auto loaded = processor_.acquireLoadedSample();
            if (loaded != nullptr && idx < static_cast<int> (loaded->slices.size()))
            {
                const auto& sl = loaded->slices[static_cast<std::size_t> (idx)];
                processor_.startSliceAudition (sl.startFrame, sl.endFrame);
                auditionSliceIndex_ = idx;
                invalid();
            }
        }
        return kMouseEventHandled;
    }

    // ── MMB: pan ───────────────────────────────────────────────────────
    if (buttons.isMiddleButton())
    {
        dragMode_ = DragMode::pan;
        dragLastX_ = p.x;
        return kMouseEventHandled;
    }

    // ── LMB ────────────────────────────────────────────────────────────
    if (buttons.isLeftButton())
    {
        // Trim handle hit? (takes priority even on a double-click)
        const auto trimHit = findTrimHandleNear (p);
        if (trimHit != TrimHandle::none)
        {
            dragMode_ = DragMode::trimStart;
            dragStartValue_ = (trimHit == TrimHandle::start)
                                  ? processor_.uiStateView().sample.trimStartFrame
                                  : processor_.uiStateView().sample.trimEndFrame;
            if (trimHit == TrimHandle::end)
                dragMode_ = DragMode::trimEnd;
            return kMouseEventHandled;
        }

        // Double-click gestures, in original priority order: remove a manual
        // marker, exclude an auto onset, or add a manual marker. Undo the
        // stray probability paint the first click of the double-click left
        // on the slice underneath (snap to the nearest transient unless
        // Shift is held -- faithful to the original's snap = !shift).
        if (buttons.isDoubleClick())
        {
            // Roll the first click's paint back so whichever gesture lands
            // (remove/exclude/add) inherits the ORIGINAL probability.
            if (prevDownPainted && prePaintSliceIdx_ >= 0)
                processor_.setSliceProbability (prePaintSliceIdx_, prePaintProb_);

            const std::int32_t nearId = findManualPointNear (p.x);
            if (nearId >= 0)
            {
                processor_.removeManualPoint (nearId);
            }
            else if (const int64_t autoBoundary = findAutoPointNear (p.x); autoBoundary > 0)
            {
                // Exclude the raw onset the user double-clicked -- target the
                // VISIBLE (possibly grid-quantized) boundary frame, exactly
                // like the original's findAutoPointNear -> excludeNearestAutoPoint.
                processor_.excludeNearestAutoPoint (autoBoundary);
            }
            else
            {
                processor_.addManualPoint (xToSample (p.x), ! buttons.isShiftSet());
            }
            invalid();
            return kMouseEventHandled;
        }

        // Single click on an existing manual marker → grab it to drag. The
        // marker tracks the mouse freely; it snaps to the nearest transient
        // on release (unless Shift). A click that never moves is a no-op.
        const std::int32_t manualId = findManualPointNear (p.x);
        if (manualId >= 0)
        {
            dragMode_ = DragMode::manualPoint;
            draggingManualId_ = manualId;
            manualDragMoved_ = false;
            return kMouseEventHandled;
        }

        // Single click on a slice → set probability from Y (top = 1.0).
        const int sliceIdx = getSliceIndexAtX (p.x);
        if (sliceIdx >= 0)
        {
            // Remember the pre-paint value + arm the flag so a double-click
            // (which fires this single click first) can roll the paint back.
            prePaintSliceIdx_ = sliceIdx;
            prePaintProb_ = processor_.getSliceProbability (sliceIdx);
            lastDownPainted_ = true;

            const float prob = 1.0f - static_cast<float> (std::clamp (
                p.y / std::max (1.0, r.getHeight()), 0.0, 1.0));
            processor_.setSliceProbability (sliceIdx, prob);
            dragMode_ = DragMode::probability;
            invalid();
            return kMouseEventHandled;
        }
    }

    return kMouseEventNotHandled;
}

CMouseEventResult WaveformView::onMouseUp (CPoint& where, const CButtonState& buttons)
{
    // ── RMB up: stop slice audition ────────────────────────────────────
    if (auditionSliceIndex_ >= 0)
    {
        processor_.stopSliceAudition();
        auditionSliceIndex_ = -1;
        invalid();
    }

    // ── Manual-marker drag release: snap to the nearest transient (unless
    //    Shift). A click that never moved leaves the marker exactly put. ──
    if (dragMode_ == DragMode::manualPoint && draggingManualId_ >= 0)
    {
        if (manualDragMoved_)
        {
            const int64_t frame = xToSample (toLocal (where).x);
            processor_.moveManualPoint (draggingManualId_, frame, ! buttons.isShiftSet());
            invalid();
        }
        draggingManualId_ = -1;
        manualDragMoved_ = false;
    }

    dragMode_ = DragMode::none;
    return kMouseEventHandled;
}

CMouseEventResult WaveformView::onMouseMoved (CPoint& where, const CButtonState& buttons)
{
    const CRect r = getViewSize();
    const CCoord h = r.getHeight();
    const CPoint p = toLocal (where);
    const bool snap = ! buttons.isShiftSet();  // Shift disables trim snapping

    switch (dragMode_)
    {
        case DragMode::pan:
        {
            const CCoord dx = p.x - dragLastX_;
            dragLastX_ = p.x;
            const int64_t range = visibleEnd_ - visibleStart_;
            const CCoord w = std::max (1.0, getWidth());
            const int64_t shift = static_cast<int64_t> (-(dx / w) * static_cast<CCoord> (range));
            visibleStart_ += shift;
            visibleEnd_ += shift;
            clampVisibleRange();
            rebuildPeaks();
            invalid();
            break;
        }

        case DragMode::trimStart:
        {
            int64_t sample = xToSample (p.x);
            if (snap)
                sample = snapToSliceMarker (sample);
            const auto& s = processor_.uiStateView().sample;
            processor_.setTrimFrames (
                std::clamp (sample, static_cast<int64_t> (0),
                            s.trimEndFrame - state::SampleState::kMinTrimGapFrames),
                s.trimEndFrame);
            rebuildPeaks();
            invalid();
            break;
        }

        case DragMode::trimEnd:
        {
            int64_t sample = xToSample (p.x);
            if (snap)
                sample = snapToSliceMarker (sample);
            const auto& s = processor_.uiStateView().sample;
            processor_.setTrimFrames (
                s.trimStartFrame,
                std::clamp (sample,
                            s.trimStartFrame + state::SampleState::kMinTrimGapFrames,
                            s.sampleLengthFrames));
            rebuildPeaks();
            invalid();
            break;
        }

        case DragMode::probability:
        {
            // Paint mode: recompute the slice under the CURRENT cursor X on
            // every move so wiping across the display sets each slice it
            // passes over (faithful to the original's setProbabilityFromMouse,
            // which re-derives the slice index every mouseDrag).
            const int sliceIdx = getSliceIndexAtX (p.x);
            if (sliceIdx >= 0)
            {
                const float prob = 1.0f - static_cast<float> (std::clamp (
                    p.y / std::max (1.0, h), 0.0, 1.0));
                processor_.setSliceProbability (sliceIdx, prob);
                invalid();
            }
            break;
        }

        case DragMode::manualPoint:
        {
            // Free drag: the marker tracks the mouse 1:1 (no snapping mid-
            // drag). The per-frame rebuild is cheap and weight-preserving, so
            // the boundary + bars follow live. Snap happens on release.
            if (draggingManualId_ >= 0)
            {
                manualDragMoved_ = true;
                processor_.moveManualPoint (draggingManualId_, xToSample (p.x), /*snap*/ false);
                invalid();
            }
            break;
        }

        default:
            break;
    }

    return kMouseEventHandled;
}

CMouseEventResult WaveformView::onMouseCancel()
{
    if (auditionSliceIndex_ >= 0)
    {
        processor_.stopSliceAudition();
        auditionSliceIndex_ = -1;
    }
    dragMode_ = DragMode::none;
    draggingManualId_ = -1;
    manualDragMoved_ = false;
    return kMouseEventHandled;
}

void WaveformView::onMouseWheelEvent (MouseWheelEvent& event)
{
    const auto delta = static_cast<double> (event.deltaY);

    if (event.modifiers.has (ModifierKey::Shift))
    {
        // Pan: shift both bounds.
        const CCoord range = static_cast<CCoord> (visibleEnd_ - visibleStart_);
        const auto shift = static_cast<int64_t> (delta * range * kPanFractionPerNotch);
        visibleStart_ += shift;
        visibleEnd_ += shift;
    }
    else
    {
        // Zoom: centered on the sample under cursor.
        const int64_t sampleUnder = xToSample (toLocal (event.mousePosition).x);
        const double zoomFactor = std::pow (kZoomFactorPerNotch, -delta);
        const CCoord range = static_cast<CCoord> (visibleEnd_ - visibleStart_);
        const auto newRange = static_cast<int64_t> (std::max (
            static_cast<CCoord> (minVisibleRangeFrames()), range * zoomFactor));
        const double frac = static_cast<double> (sampleUnder - visibleStart_)
                          / static_cast<double> (range);
        visibleStart_ = sampleUnder - static_cast<int64_t> (frac * static_cast<CCoord> (newRange));
        visibleEnd_ = visibleStart_ + newRange;
    }

    clampVisibleRange();
    rebuildPeaks();
    invalid();
    event.consumed = true;
}

// ── Hit testing ────────────────────────────────────────────────────────

WaveformView::TrimHandle WaveformView::findTrimHandleNear (const CPoint& p) const
{
    const auto& sample = processor_.uiStateView().sample;
    const CCoord startX = sampleToX (sample.trimStartFrame);
    const CCoord endX = sampleToX (sample.trimEndFrame);

    if (std::abs (p.x - startX) <= kTrimHitRadius)
        return TrimHandle::start;
    if (std::abs (p.x - endX) <= kTrimHitRadius)
        return TrimHandle::end;
    return TrimHandle::none;
}

int WaveformView::getSliceIndexAtX (CCoord x) const
{
    const auto loaded = processor_.acquireLoadedSample();
    if (loaded == nullptr)
        return -1;

    const auto& slices = loaded->slices;
    const auto& sample = processor_.uiStateView().sample;
    const int64_t frame = xToSample (x);

    for (std::size_t i = 0; i < slices.size(); ++i)
    {
        if (frame >= slices[i].startFrame && frame < slices[i].endFrame)
        {
            // Only report slices that are part of the current trim selection;
            // hidden (outside-trim) slices are non-interactive, matching the
            // fact that their marker/slider isn't drawn.
            if (slices[i].endFrame <= sample.trimStartFrame
                || slices[i].startFrame >= sample.trimEndFrame)
                return -1;
            return static_cast<int> (i);
        }
    }
    return -1;
}

std::int32_t WaveformView::findManualPointNear (CCoord x) const
{
    const auto& sample = processor_.uiStateView().sample;
    std::int32_t bestId = -1;
    CCoord bestDist = kMarkerHitRadius + 1.0;

    for (const auto& mp : sample.manualPoints)
    {
        // Only match markers that are actually visible (inside the trim);
        // outside-trim points are soft-excluded and not drawn.
        if (mp.position <= sample.trimStartFrame || mp.position >= sample.trimEndFrame)
            continue;

        const CCoord d = std::abs (x - sampleToX (mp.position));
        if (d < bestDist)
        {
            bestDist = d;
            bestId = mp.id;
        }
    }

    return (bestDist <= kMarkerHitRadius) ? bestId : -1;
}

int64_t WaveformView::findAutoPointNear (CCoord x) const
{
    const auto loaded = processor_.acquireLoadedSample();
    if (loaded == nullptr)
        return -1;

    const auto& sample = processor_.uiStateView().sample;
    int64_t best = -1;
    CCoord bestDist = kMarkerHitRadius + 1.0;

    for (const auto& sl : loaded->slices)
    {
        // Only boundaries that are actually drawn: inside the trim, and not
        // the trim-start edge. Manual markers win priority (checked first).
        if (sl.startFrame <= sample.trimStartFrame || sl.startFrame >= sample.trimEndFrame)
            continue;

        bool isManual = false;
        for (const auto& mp : sample.manualPoints)
            if (mp.position == sl.startFrame) { isManual = true; break; }
        if (isManual)
            continue;

        const CCoord d = std::abs (x - sampleToX (sl.startFrame));
        if (d < bestDist)
        {
            bestDist = d;
            best = sl.startFrame;
        }
    }

    return (bestDist <= kMarkerHitRadius) ? best : -1;
}

int64_t WaveformView::snapToSliceMarker (int64_t frame) const
{
    const auto loaded = processor_.acquireLoadedSample();
    if (loaded == nullptr || loaded->slices.empty())
        return frame;

    // Convert the on-screen pixel radius to a frame radius at the current
    // zoom so the snap "feel" is constant regardless of how far in we are.
    const int64_t range = std::max (static_cast<int64_t> (1), visibleEnd_ - visibleStart_);
    const CCoord w = std::max (1.0, getWidth());
    const auto radiusFrames = static_cast<int64_t> (kSnapPixels / w * static_cast<CCoord> (range));

    int64_t best = frame;
    int64_t bestDist = radiusFrames + 1;

    const auto consider = [&] (int64_t markerFrame)
    {
        const int64_t d = std::llabs (markerFrame - frame);
        if (d < bestDist)
        {
            bestDist = d;
            best = markerFrame;
        }
    };

    for (const auto& sl : loaded->slices)
    {
        consider (sl.startFrame);
        consider (sl.endFrame);   // covers the final boundary too
    }

    return (bestDist <= radiusFrames) ? best : frame;
}

// ── Range helpers ──────────────────────────────────────────────────────

int64_t WaveformView::minVisibleRangeFrames() const
{
    const auto& sample = processor_.uiStateView().sample;
    if (sample.sampleSampleRate > 0.0)
        return static_cast<int64_t> (kMinVisibleRangeMs * sample.sampleSampleRate / 1000.0);
    return 32;
}

void WaveformView::clampVisibleRange()
{
    const int64_t total = processor_.uiStateView().sample.sampleLengthFrames;
    const int64_t minRange = minVisibleRangeFrames();

    if (total <= 0)
    {
        visibleStart_ = 0;
        visibleEnd_ = 0;
        return;
    }

    if (visibleEnd_ - visibleStart_ < minRange)
    {
        const int64_t mid = (visibleStart_ + visibleEnd_) / 2;
        visibleStart_ = mid - minRange / 2;
        visibleEnd_ = visibleStart_ + minRange;
    }

    if (visibleStart_ < 0)
    {
        visibleStart_ = 0;
        visibleEnd_ = std::max (minRange, visibleEnd_);
    }
    if (visibleEnd_ > total)
    {
        visibleEnd_ = total;
        visibleStart_ = std::min (visibleStart_, total - minRange);
    }
    if (visibleStart_ < 0)
        visibleStart_ = 0;
}

// ── Public API ─────────────────────────────────────────────────────────

void WaveformView::refresh()
{
    const int64_t total = processor_.uiStateView().sample.sampleLengthFrames;
    if (total != lastKnownTotalFrames_)
    {
        visibleStart_ = 0;
        visibleEnd_ = total;
        lastKnownTotalFrames_ = total;
    }
    else
    {
        clampVisibleRange();
    }
    rebuildPeaks();
    invalid();
}

void WaveformView::zoomToTrims()
{
    const auto& sample = processor_.uiStateView().sample;
    const int64_t trimStart = sample.trimStartFrame;
    const int64_t trimEnd = sample.trimEndFrame;
    const int64_t trimSpan = trimEnd - trimStart;
    const int64_t minRange = minVisibleRangeFrames();
    const int64_t margin = std::max (minRange / 4, trimSpan / 20);

    visibleStart_ = std::max (static_cast<int64_t> (0), trimStart - margin);
    visibleEnd_ = std::min (sample.sampleLengthFrames, trimEnd + margin);
    clampVisibleRange();
    rebuildPeaks();
    invalid();
}

void WaveformView::resetZoom()
{
    visibleStart_ = 0;
    visibleEnd_ = processor_.uiStateView().sample.sampleLengthFrames;
    clampVisibleRange();
    rebuildPeaks();
    invalid();
}

} // namespace nedit::plugin
