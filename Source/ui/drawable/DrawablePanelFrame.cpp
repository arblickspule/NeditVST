#include "DrawablePanelFrame.h"
#include "svg_assets.h"

namespace ui
{
namespace drawable
{

DrawablePanelFrame::DrawablePanelFrame()
{
    for (int i = 0; i < 9; ++i)
        tiles[i] = drawableFromSVG (juce::String (art::panelSlices[i]));
}

void DrawablePanelFrame::paint (juce::Graphics& g)
{
    const float w = (float) getWidth();
    const float h = (float) getHeight();
    if (w < 2.0f * tileSize || h < 2.0f * tileSize)
        return;

    const float t = (float) tileSize;

    // Corners first (native size), then edges stretched along one axis,
    // then the centre stretched both ways. Tiles share edges exactly, so the
    // fill is continuous and the border lines meet without gaps.
    drawSlice (g, tiles[0].get(), { 0.0f, 0.0f, t, t });
    drawSlice (g, tiles[1].get(), { w - t, 0.0f, t, t });
    drawSlice (g, tiles[2].get(), { w - t, h - t, t, t });
    drawSlice (g, tiles[3].get(), { 0.0f, h - t, t, t });
    drawSlice (g, tiles[4].get(), { t, 0.0f, w - 2.0f * t, t });
    drawSlice (g, tiles[5].get(), { t, h - t, w - 2.0f * t, t });
    drawSlice (g, tiles[6].get(), { 0.0f, t, t, h - 2.0f * t });
    drawSlice (g, tiles[7].get(), { w - t, t, t, h - 2.0f * t });
    drawSlice (g, tiles[8].get(), { t, t, w - 2.0f * t, h - 2.0f * t });
}

juce::Rectangle<int> DrawablePanelFrame::getContentArea() const
{
    return getLocalBounds().reduced (tileSize);
}

void DrawablePanelFrame::drawSlice (juce::Graphics& g, juce::Drawable* slice, const juce::Rectangle<float>& rect)
{
    if (slice != nullptr)
        slice->drawWithin (g, rect, juce::RectanglePlacement::stretchToFit, 1.0f);
}

} // namespace drawable
} // namespace ui
