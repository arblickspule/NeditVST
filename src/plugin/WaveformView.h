// Nedit -- Plugin layer.
//
// The waveform display: solid peak lines, trim handles, slice markers,
// per-slice probability sliders, and zoom/pan.
//
// Rendering and interaction follow the original NeditVST WaveformDisplay
// faithfully, adapted to VSTGUI's drawing and mouse APIs.  Zoom/pan
// state persists through UiState so the view always initialises FROM
// the model (pitfall #6).

#pragma once

#include "vstgui/lib/cview.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace nedit::plugin {

class NeditProcessor;

class WaveformView : public VSTGUI::CView
{
public:
    explicit WaveformView (NeditProcessor& processor,
                           VSTGUI::IControlListener* listener,
                           int height = 96);

    //--- CView -----------------------------------------------------------
    void draw (VSTGUI::CDrawContext* dc) override;
    VSTGUI::CMouseEventResult onMouseDown (VSTGUI::CPoint& where,
                                           const VSTGUI::CButtonState& buttons) override;
    VSTGUI::CMouseEventResult onMouseUp (VSTGUI::CPoint& where,
                                         const VSTGUI::CButtonState& buttons) override;
    VSTGUI::CMouseEventResult onMouseMoved (VSTGUI::CPoint& where,
                                            const VSTGUI::CButtonState& buttons) override;
    VSTGUI::CMouseEventResult onMouseCancel () override;
    void onMouseWheelEvent (VSTGUI::MouseWheelEvent& event) override;

    //--- Public API ------------------------------------------------------
    void refresh();                 // re-sync peaks + repaint
    void zoomToTrims();             // view jumps to [trimStart-margin, trimEnd+margin]
    void resetZoom();               // view resets to [0, totalSamples)

    // Hit testing (public for tests): nearest VISIBLE marker/boundary to an
    // on-screen x within the padded hit radius. findManualPointNear returns
    // a manual point's id; findAutoPointNear returns an auto boundary's
    // frame (skipping manual-coincident boundaries -- manual wins priority).
    std::int32_t findManualPointNear (VSTGUI::CCoord x) const;
    std::int64_t findAutoPointNear (VSTGUI::CCoord x) const;

private:
    //--- Coordinate mapping -----------------------------------------------
    int64_t xToSample (VSTGUI::CCoord x) const;
    VSTGUI::CCoord sampleToX (int64_t sample) const;

    // Mouse coordinates arrive in the PARENT (frame) coordinate system, not
    // this view's local space (CViewContainer offsets by its own origin, and
    // the frame's origin is 0,0). Convert to local (x in 0..width, y in
    // 0..height) so hit-testing and the probability Y-axis are correct.
    VSTGUI::CPoint toLocal (const VSTGUI::CPoint& framePoint) const;

    //--- Peak computation -------------------------------------------------
    struct Peak { float min; float max; };
    void rebuildPeaks();

    //--- Hit testing ------------------------------------------------------
    enum class TrimHandle { none, start, end };
    TrimHandle findTrimHandleNear (const VSTGUI::CPoint& p) const;
    int getSliceIndexAtX (VSTGUI::CCoord x) const;

    // Snap a frame position to the nearest slice boundary within a small
    // on-screen pixel radius (zoom-independent feel). Returns the input
    // unchanged when nothing is close enough.
    int64_t snapToSliceMarker (int64_t frame) const;

    //--- Helpers ----------------------------------------------------------
    void clampVisibleRange();
    int64_t minVisibleRangeFrames() const;

    NeditProcessor& processor_;
    VSTGUI::IControlListener* listener_ = nullptr;

    // Zoom/pan window in source-sample frames.
    int64_t visibleStart_ = 0;
    int64_t visibleEnd_ = 0;
    int64_t lastKnownTotalFrames_ = 0;

    // Pre-computed peaks: one {min,max} per pixel column.
    std::vector<Peak> peaks_;

    // Drag state.
    TrimHandle draggingTrim_ = TrimHandle::none;
    int64_t dragStartValue_ = 0;           // snapshot for undo / delta
    bool draggingPan_ = false;
    VSTGUI::CCoord dragLastX_ = 0;

    // RMB slice audition.
    int auditionSliceIndex_ = -1;

    // Zoom constants.
    static constexpr double kZoomFactorPerNotch = 1.5;
    static constexpr double kPanFractionPerNotch = 0.15;
    static constexpr int64_t kMinVisibleRangeMs = 20;  // floor: 20ms

    // Trim handle hit radius (px).
    static constexpr VSTGUI::CCoord kTrimHitRadius = 8.0;
    // Slice marker hit radius (px). Deliberately generous -- the boundary
    // lines are 1-2px thin, so the click target is padded for fiddliness.
    static constexpr VSTGUI::CCoord kMarkerHitRadius = 10.0;
    // Trim-to-slice-marker snap radius (px). Hold Shift while dragging to
    // disable snapping for fine positioning (faithful to the original).
    static constexpr VSTGUI::CCoord kSnapPixels = 8.0;

    enum class DragMode { none, trimStart, trimEnd, pan, probability, manualPoint };
    DragMode dragMode_ = DragMode::none;

    // Manual-marker drag: the grabbed point's id, and whether the mouse
    // actually moved (a plain click that never moved must NOT re-snap).
    std::int32_t draggingManualId_ = -1;
    bool manualDragMoved_ = false;

    // Paint suppression: a double-click fires a single-click first, which
    // would paint the slice's probability underneath the incoming add. We
    // capture the slice's pre-paint weight so the add can roll it back.
    // lastDownPainted_ marks whether the IMMEDIATELY preceding mouse-down
    // painted (so we only roll back a paint that really belongs to this
    // double-click, never stale state).
    bool lastDownPainted_ = false;
    int prePaintSliceIdx_ = -1;
    float prePaintProb_ = 1.0f;
};

} // namespace nedit::plugin
