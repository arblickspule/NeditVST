#include "DrawableDemoPage.h"
#include "../drawable/svg_assets.h"
#include "../drawable/DrawableKnob.h"
#include "../drawable/DrawableSlider.h"
#include "../NeditPalette.h"

namespace ui
{

DrawableDemoPage::DrawableDemoPage()
    : Flex (FlexConfig{ .direction = Direction::column, .gap = 14.0f,
                        .padding = juce::BorderSize<int> (18, 18, 18, 18) })
{
    for (int i = 0; i < 9; ++i)
        tiles[i] = drawable::drawableFromSVG (juce::String (drawable::art::panelSlices[i]));

    // Header.
    auto header = std::make_unique<juce::Label>();
    header->setText ("Drawable demo -- SVG knob / SVG slider / 9-slice SVG panel frame",
                     juce::dontSendNotification);
    header->setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    header->setColour (juce::Label::textColourId, NeditPalette::textOnTungsten);
    header->setSize (500, 24);

    // Demo row: three groups (knob / slider / small 9-slice panel).
    auto knob = std::make_unique<drawable::DrawableKnob>();
    drawable::DrawableKnob* knobPtr = knob.get();
    auto knobValue = makeValueLabel();
    knobValueLabel = knobValue.get();
    knob->onValueChange = [this, knobPtr]
    {
        knobValueLabel->setText (juce::String (juce::roundToInt (knobPtr->getValue())),
                                 juce::dontSendNotification);
    };

    auto slider = std::make_unique<drawable::DrawableSlider>();
    drawable::DrawableSlider* sliderPtr = slider.get();
    auto sliderValue = makeValueLabel();
    sliderValueLabel = sliderValue.get();
    slider->onValueChange = [this, sliderPtr]
    {
        sliderValueLabel->setText (juce::String (juce::roundToInt (sliderPtr->getValue())),
                                   juce::dontSendNotification);
    };

    auto smallFrameBox = std::make_unique<drawable::DrawablePanelFrame>();
    smallFrameBox->setSize (200, 140);
    smallFrame = smallFrameBox.get();

    auto inner = std::make_unique<juce::Label>();
    inner->setText ("content inside\n9-slice frame", juce::dontSendNotification);
    inner->setColour (juce::Label::textColourId, NeditPalette::textOnTungsten);
    inner->setJustificationType (juce::Justification::centred);
    innerLabel = inner.get();
    smallFrameBox->addAndMakeVisible (inner.release()); // frame takes ownership

    std::vector<Cell> cells;
    cells.push_back (cell (std::move (header)));
    cells.push_back (row (
        column (cell (std::move (knob)),
                cell (std::move (knobValue)),
                cell (makeCaption ("SVG knob -- drag vertically or scroll"))),
        column (cell (std::move (slider)),
                cell (std::move (sliderValue)),
                cell (makeCaption ("SVG slider -- drag the thumb"))),
        column (cell (std::move (smallFrameBox)),
                cell (makeCaption ("9-slice panel -- corners stay crisp")))
    ));
    setCells (std::move (cells));
}

void DrawableDemoPage::paint (juce::Graphics& g)
{
    // The whole page is a 9-slice panel: the same tiles as DrawablePanelFrame,
    // drawn across the page's full size so the corner scaling is obvious.
    drawTiles (g, getLocalBounds().toFloat());
}

void DrawableDemoPage::resized()
{
    Flex::resized();

    if (smallFrame != nullptr && innerLabel != nullptr)
        innerLabel->setBounds (smallFrame->getContentArea().reduced (6));
}

std::unique_ptr<juce::Label> DrawableDemoPage::makeCaption (const juce::String& text)
{
    auto label = std::make_unique<juce::Label>();
    label->setText (text, juce::dontSendNotification);
    label->setFont (juce::Font (juce::FontOptions (12.0f)));
    label->setColour (juce::Label::textColourId, NeditPalette::textOnTungsten.withAlpha (0.7f));
    label->setJustificationType (juce::Justification::centred);
    label->setSize (260, 16);
    return label;
}

std::unique_ptr<juce::Label> DrawableDemoPage::makeValueLabel()
{
    auto label = std::make_unique<juce::Label>();
    label->setText ("0", juce::dontSendNotification);
    label->setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
    label->setColour (juce::Label::textColourId, NeditPalette::salmon);
    label->setJustificationType (juce::Justification::centred);
    label->setSize (80, 22);
    return label;
}

void DrawableDemoPage::drawTiles (juce::Graphics& g, const juce::Rectangle<float>& area)
{
    const float w = area.getWidth();
    const float h = area.getHeight();
    constexpr float t = (float) drawable::DrawablePanelFrame::tileSize;
    if (w < 2.0f * t || h < 2.0f * t)
        return;

    drawSlice (g, tiles[0].get(), { area.getX(), area.getY(), t, t });
    drawSlice (g, tiles[1].get(), { area.getX() + w - t, area.getY(), t, t });
    drawSlice (g, tiles[2].get(), { area.getX() + w - t, area.getY() + h - t, t, t });
    drawSlice (g, tiles[3].get(), { area.getX(), area.getY() + h - t, t, t });
    drawSlice (g, tiles[4].get(), { area.getX() + t, area.getY(), w - 2.0f * t, t });
    drawSlice (g, tiles[5].get(), { area.getX() + t, area.getY() + h - t, w - 2.0f * t, t });
    drawSlice (g, tiles[6].get(), { area.getX(), area.getY() + t, t, h - 2.0f * t });
    drawSlice (g, tiles[7].get(), { area.getX() + w - t, area.getY() + t, t, h - 2.0f * t });
    drawSlice (g, tiles[8].get(), { area.getX() + t, area.getY() + t, w - 2.0f * t, h - 2.0f * t });
}

void DrawableDemoPage::drawSlice (juce::Graphics& g, juce::Drawable* slice, const juce::Rectangle<float>& rect)
{
    if (slice != nullptr)
        slice->drawWithin (g, rect, juce::RectanglePlacement::stretchToFit, 1.0f);
}

} // namespace ui
