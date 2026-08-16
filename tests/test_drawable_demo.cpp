// Drawable-demo smoke tests (drawable-demo branch): the three SVG-<->-Drawable
// techniques must parse their embedded SVG artwork headlessly, paint into a
// real Graphics context without crashing, and lay out as Flex pages. If the
// SVG strings drift out of the parser's supported subset, the drawables come
// back nullptr and the painted panel comes back transparent -- so the pixel
// checks below are the guard against artwork regressions.

#include "ui/pages/DrawableDemoPage.h"
#include "ui/drawable/DrawableKnob.h"
#include "ui/drawable/DrawableSlider.h"
#include "ui/drawable/DrawablePanelFrame.h"
#include "ui/drawable/svg_assets.h"

#include <doctest/doctest.h>

namespace
{

juce::Image paintIntoImage (juce::Component& component, int width, int height)
{
    component.setSize (width, height);
    juce::Image image (juce::Image::ARGB, width, height, true);
    juce::Graphics g (image);
    component.paint (g);
    return image;
}

} // namespace

TEST_CASE ("Drawable demo: every embedded SVG parses into a Drawable")
{
    const juce::String knob = ui::drawable::art::knob;
    const juce::String track = ui::drawable::art::sliderTrack;
    const juce::String thumb = ui::drawable::art::sliderThumb;

    CHECK (ui::drawable::drawableFromSVG (knob) != nullptr);
    CHECK (ui::drawable::drawableFromSVG (track) != nullptr);
    CHECK (ui::drawable::drawableFromSVG (thumb) != nullptr);

    for (int i = 0; i < 9; ++i)
        CHECK (ui::drawable::drawableFromSVG (juce::String (ui::drawable::art::panelSlices[i])) != nullptr);
}

TEST_CASE ("Drawable demo: knob rotates over the value range and paints")
{
    ui::drawable::DrawableKnob knob;
    CHECK (knob.getSliderStyle() == juce::Slider::RotaryVerticalDrag);
    CHECK (knob.getWidth() == ui::drawable::DrawableKnob::size);

    // The pointer sweep must move across the value range (rotary params span
    // 300 degrees around 12 o'clock).
    const auto params = knob.getRotaryParameters();
    CHECK (params.endAngleRadians > params.startAngleRadians);

    const auto image = paintIntoImage (knob, knob.getWidth(), knob.getHeight());
    CHECK (image.isValid());
}

TEST_CASE ("Drawable demo: slider thumb moves from min to max and paints")
{
    ui::drawable::DrawableSlider slider;

    slider.setValue (0.0, juce::dontSendNotification);
    const float minPos = slider.getPositionOfValue (slider.getValue());

    slider.setValue (100.0, juce::dontSendNotification);
    const float maxPos = slider.getPositionOfValue (slider.getValue());

    CHECK (maxPos > minPos);

    const auto image = paintIntoImage (slider, slider.getWidth(), slider.getHeight());
    CHECK (image.isValid());
}

TEST_CASE ("Drawable demo: 9-slice panel fills its frame at small and large sizes")
{
    ui::drawable::DrawablePanelFrame frame;
    frame.setSize (200, 140);

    const auto area = frame.getContentArea();
    CHECK (area.getX() == ui::drawable::DrawablePanelFrame::tileSize);
    CHECK (area.getY() == ui::drawable::DrawablePanelFrame::tileSize);
    CHECK (area.getWidth() == 200 - 2 * ui::drawable::DrawablePanelFrame::tileSize);
    CHECK (area.getHeight() == 140 - 2 * ui::drawable::DrawablePanelFrame::tileSize);

    // The centre of a painted frame must be the opaque tile fill (#4A4A4A);
    // a transparent pixel means the slices failed to parse or paint.
    const auto small = paintIntoImage (frame, 200, 140);
    CHECK (small.getPixelAt (100, 70).getAlpha() > 0);

    const auto big = paintIntoImage (frame, 640, 480);
    CHECK (big.getPixelAt (320, 240).getAlpha() > 0); // centre tile
    CHECK (big.getPixelAt (5, 25).getAlpha() > 0);    // inside the rounded corner tile
    CHECK (big.getPixelAt (320, 5).getAlpha() > 0);   // stretched top edge tile
}

TEST_CASE ("Drawable demo: demo page constructs, sizes and paints as a Flex page")
{
    ui::DrawableDemoPage page;
    CHECK (page.getPreferredMainSize() > 0.0f);
    CHECK (dynamic_cast<const ui::Flex*> (&page) != nullptr);
    CHECK (page.getNumChildComponents() == 2); // header + demo row

    const auto image = paintIntoImage (page, 900, 420);
    CHECK (image.isValid());
    CHECK (image.getPixelAt (450, 200).getAlpha() > 0); // the page-wide frame tile
}

TEST_CASE ("DrawablePanelFrame: 9-slice geometry at two sizes")
{
    const juce::Colour salmon { 0xFFFF7E79 };
    const juce::Colour tile   { 0xFF4A4A4A };
    const juce::Colour bg     { 0xFF383838 };

    ui::drawable::DrawablePanelFrame frame;

    // Small frame (200x140).
    juce::Image smallImg (juce::Image::ARGB, 200, 140, true);
    {
        juce::Graphics g (smallImg);
        g.fillAll (bg);
        frame.setBounds (0, 0, 200, 140);
        frame.paint (g);
    }

    // 2px salmon border hugs the outer edge, tile fill inside it, and the
    // rounded notch at the very corner stays the background colour (corners
    // are NOT stretched).
    CHECK (smallImg.getPixelAt (100, 1).getARGB() == salmon.getARGB());     // top border
    CHECK (smallImg.getPixelAt (1, 70).getARGB() == salmon.getARGB());      // left border
    CHECK (smallImg.getPixelAt (100, 139).getARGB() == salmon.getARGB());   // bottom border
    CHECK (smallImg.getPixelAt (100, 70).getARGB() == tile.getARGB());      // interior
    CHECK (smallImg.getPixelAt (5, 25).getARGB() == tile.getARGB());        // inside rounded corner
    CHECK (smallImg.getPixelAt (1, 1).getARGB() == bg.getARGB());           // rounded notch

    // Big frame (640x480): the same geometry must hold when the tiles are
    // stretched along one axis, and the border lines must stay on the edge.
    juce::Image bigImg (juce::Image::ARGB, 640, 480, true);
    {
        juce::Graphics g (bigImg);
        g.fillAll (bg);
        frame.setBounds (0, 0, 640, 480);
        frame.paint (g);
    }
    CHECK (bigImg.getPixelAt (320, 1).getARGB() == salmon.getARGB());       // top border
    CHECK (bigImg.getPixelAt (1, 240).getARGB() == salmon.getARGB());       // left border
    CHECK (bigImg.getPixelAt (320, 479).getARGB() == salmon.getARGB());     // bottom border
    CHECK (bigImg.getPixelAt (320, 240).getARGB() == tile.getARGB());       // interior
    CHECK (bigImg.getPixelAt (1, 1).getARGB() == bg.getARGB());             // rounded notch
}
