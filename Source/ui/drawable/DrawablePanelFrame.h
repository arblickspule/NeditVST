#pragma once

#include <JuceHeader.h>

namespace ui
{
namespace drawable
{

//==============================================================================
/** A component that paints a nine-slice SVG panel frame at whatever size it
    is given. Nine Drawables (art::panelSlices, tileSize x tileSize) are tiled
    into the bounds: corners at native size (crisp at every resolution), the
    four edges stretched along one axis, the centre stretched both ways. The
    artwork's fill and outline are part of the tiles themselves, so the result
    is a solid rounded panel with a salmon border and no seam work from the
    caller.

    Children are not positioned by this class -- callers size them from
    getContentArea() (the region inside the tiles). */
class DrawablePanelFrame : public juce::Component
{
public:
    DrawablePanelFrame();

    void paint (juce::Graphics&) override;

    // The interior region inside the 9-slice border (frame tile frame only;
    // add a few pixels more if you want breathing room).
    juce::Rectangle<int> getContentArea() const;

    static constexpr int tileSize = 30;

private:
    void drawSlice (juce::Graphics&, juce::Drawable*, const juce::Rectangle<float>&);

    std::array<std::unique_ptr<juce::Drawable>, 9> tiles;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrawablePanelFrame)
};

} // namespace drawable
} // namespace ui
