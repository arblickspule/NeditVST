// The whole UI, behind the ui::contract seam (docs/ui-layout-decision.md).
// This file is the "App.vue": it builds the page tree, owns the poll timer,
// and knows nothing about the plugin shell beyond the AudioProcessor ref it
// must hand to the JUCE editor base class. Swapping the GUI means replacing
// this file's makeEditor() implementation -- nothing else changes.

#include "contract.h"
#include "layout/Layout.h"
#include "../SlicerModel.h"
#include "../SlicerEngine.h"
#include "../NeditPalette.h"

namespace ui
{

namespace
{

//==============================================================================
// Placeholder editor -- proves the seam before the real Generate/Sequence/
// Perform pages land. A titled column of rows that reports live model state
// through the polled-sync pattern the real pages will use.
class NeditVstEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    NeditVstEditor (juce::AudioProcessor& host, SlicerModel& modelToUse, SlicerEngine&, const UiCallbacks&)
        : AudioProcessorEditor (host), model (modelToUse)
    {
        buildUi();
        setSize (480, 220);
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        root.component->setBounds (getLocalBounds());
    }

    void timerCallback() override
    {
        syncFromModel();
    }

private:
    static std::unique_ptr<juce::Label> makeLabel (const juce::String& text, float fontSize = 13.0f)
    {
        auto label = std::make_unique<juce::Label> ("", text);
        label->setSize (200, 24);
        label->setJustificationType (juce::Justification::centredLeft);
        label->setColour (juce::Label::textColourId, NeditPalette::textOnTungsten);
        label->setFont (juce::Font (juce::FontOptions (fontSize)));
        return label;
    }

    void buildUi()
    {
        setColour (juce::ResizableWindow::backgroundColourId, NeditPalette::tungsten);

        auto ownedSample = makeLabel ("sample: (none)");
        auto ownedTrigger = makeLabel ("trigger: n/a");
        auto ownedSlices = makeLabel ("slices: 0");
        sampleText = ownedSample.get();
        triggerText = ownedTrigger.get();
        slicesText = ownedSlices.get();

        root = ui::column (
            ui::cell (makeLabel ("NeditVST v2 UI -- seam scaffold", 18.0f)),
            ui::spacer (12),
            ui::row (ui::cell (makeLabel ("sample:")), ui::fill (std::move (ownedSample))),
            ui::row (ui::cell (makeLabel ("trigger:")), ui::fill (std::move (ownedTrigger))),
            ui::row (ui::cell (makeLabel ("slices:")),  ui::fill (std::move (ownedSlices))),
            ui::spacer (16),
            ui::cell (makeLabel ("Generate / Sequence / Perform pages land on this seam.")));

        addAndMakeVisible (*root.component); // the editor adds exactly one child
    }

    void syncFromModel()
    {
        sampleText->setText ("sample: " + (model.loadedFileName.isNotEmpty() ? model.loadedFileName : juce::String ("(none)")),
                             juce::dontSendNotification);

        juce::String modeText;
        switch (model.triggerMode.load())
        {
            case SlicerModel::TriggerMode::sliceLength: modeText = "Slice Length"; break;
            case SlicerModel::TriggerMode::clock:       modeText = "Clock";       break;
            case SlicerModel::TriggerMode::sequenced:   modeText = "Sequenced";   break;
            case SlicerModel::TriggerMode::performance: modeText = "Performance"; break;
        }
        triggerText->setText ("trigger: " + modeText, juce::dontSendNotification);

        slicesText->setText ("slices: " + juce::String ((int) model.slices.size()), juce::dontSendNotification);
    }

    SlicerModel& model;
    ui::Cell root; // owns the whole component tree
    juce::Label* sampleText = nullptr;
    juce::Label* triggerText = nullptr;
    juce::Label* slicesText = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeditVstEditor)
};

} // namespace

std::unique_ptr<juce::AudioProcessorEditor> makeEditor (juce::AudioProcessor& host,
                                                        SlicerModel& model,
                                                        SlicerEngine& engine,
                                                        const UiCallbacks& callbacks)
{
    return std::make_unique<NeditVstEditor> (host, model, engine, callbacks);
}

} // namespace ui
