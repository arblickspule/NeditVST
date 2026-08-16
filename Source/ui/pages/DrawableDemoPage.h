#pragma once

#include "../layout/Layout.h"
#include "../drawable/DrawablePanelFrame.h"

namespace ui
{

//==============================================================================
/** Drawable-demo page (branch drawable-demo): a live showcase of the three
    SVG-<->-Drawable techniques, so they can be auditioned in the plugin
    before any of them get adopted by the real pages:
      - DrawableKnob      -- an SVG painted with a rotation transform
      - DrawableSlider    -- an SVG groove + thumb at the value position
      - DrawablePanelFrame -- a 9-slice SVG panel, demonstrated twice: as the
        page's own full-size background (this page paints its own tiles) and
        as a small framed box with a label inside.
    The page is a plain Flex page like the others (viewport-compatible,
    getPreferredMainSize floors its height); only its paint() differs. */
class DrawableDemoPage : public Flex
{
public:
    DrawableDemoPage();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void drawTiles (juce::Graphics&, const juce::Rectangle<float>&);
    void drawSlice (juce::Graphics&, juce::Drawable*, const juce::Rectangle<float>&);

    std::unique_ptr<juce::Label> makeCaption (const juce::String& text);
    std::unique_ptr<juce::Label> makeValueLabel();

    std::array<std::unique_ptr<juce::Drawable>, 9> tiles;
    juce::Label* knobValueLabel = nullptr;
    juce::Label* sliderValueLabel = nullptr;
    drawable::DrawablePanelFrame* smallFrame = nullptr;
    juce::Label* innerLabel = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrawableDemoPage)
};

} // namespace ui
