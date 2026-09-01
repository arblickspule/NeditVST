// Nedit -- UI layer.
//
// Pure geometry behind the editor's host-size fitting: given the frame
// size the host granted us and the fixed 960x800 design size, decide how
// to place+scale the design inside the window.
//
// The editor's layout is (deliberately) hardcoded against a 960x800
// design rect. Hosts do not always honor that exactly: a DAW may open the
// plugin window too small (the macOS "everything below the waveform is
// missing" report -- onSize arriving with a shorter viewport clips every
// view below the cut) or too large (Retina hosts in point space vs pixel
// space). Instead of reflowing ~50 controls, the editor installs a
// VSTGUI CGraphicsTransform on the CFrame:
//   * design smaller than the window  -> scale 1.0, recentre (margins
//     show the frame background, kWindowBase)
//   * window smaller than the design  -> scale down to fit (everything
//     stays visible; "refuse nothing")
//   * empty/degenerate inputs         -> identity (nothing changes)
// This helper computes {scale, offsetX, offsetY}; the editor applies it
// once at open() and again from onSize(). Pure C++, framework-free,
// unit-tested in tests/ui/test_fit_geometry.cpp.

#pragma once

#include <algorithm>
#include <cmath>

namespace nedit::ui {

// The placement of the 960x800 design rect inside an arbitrary viewport.
struct EditorFit
{
    double scale = 1.0;    // 1.0 when the window is >= the design
    double offsetX = 0.0;  // left edge of the design in window coords
    double offsetY = 0.0;  // top edge of the design in window coords
};

// Fit the design into (frameW x frameH) without ever upscaling it.
//   * scale = min(frameW/designW, frameH/designH), clamped to [minScale,
//     1.0] -- a window smaller than the design scales the whole UI down so
//     nothing is clipped; a larger window keeps the design at native size.
//   * offsets centre the (possibly scaled) design in the window.
// Degenerate inputs (non-positive frame or design dimensions) or a clamp
// that would overflow the window return the identity fit.
[[nodiscard]] inline EditorFit computeEditorFit (double frameW, double frameH,
                                                 double designW, double designH,
                                                 double minScale = 0.2) noexcept
{
    EditorFit fit;
    if (frameW <= 0.0 || frameH <= 0.0 || designW <= 0.0 || designH <= 0.0)
        return fit;

    const double scale = std::min (frameW / designW, frameH / designH);
    if (scale >= 1.0)
        fit.scale = 1.0;                       // big window: native size, centred
    else
        fit.scale = std::max (scale, minScale);

    fit.offsetX = (frameW - designW * fit.scale) / 2.0;
    fit.offsetY = (frameH - designH * fit.scale) / 2.0;

    // Only an extreme min-scale clamp (a window a few pixels tall) leaves
    // the scaled design larger than one axis; pin that offset to the
    // window edge rather than overflowing it. Content is unusably tiny at
    // that point anyway -- this is just best-effort containment.
    if (fit.offsetX < 0.0)
        fit.offsetX = 0.0;
    if (fit.offsetY < 0.0)
        fit.offsetY = 0.0;

    return fit;
}

} // namespace nedit::ui