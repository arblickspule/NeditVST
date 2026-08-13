#include "PlaybackStyleParameterPanel.h"

PlaybackStyleParameterPanel::PlaybackStyleParameterPanel (SlicerAudioProcessor& processorToUse,
                                                            GetParameterValue getValueIn,
                                                            SetParameterValue setValueIn,
                                                            int initialStyleId,
                                                            std::function<void (int)> onStyleChangedIn)
    : processor (processorToUse),
      getValue (getValueIn ? std::move (getValueIn)
                            : GetParameterValue ([&processorToUse] (int paramIndex)
                                                  { return processorToUse.getSequencerCellParameterGlobalValue (paramIndex); })),
      setValue (setValueIn ? std::move (setValueIn)
                            : SetParameterValue ([&processorToUse] (int paramIndex, float value)
                                                  { processorToUse.setSequencerCellParameterGlobalValue (paramIndex, value); })),
      onStyleChanged (std::move (onStyleChangedIn))
{
    addAndMakeVisible (styleSelector);

    for (int i = 0; i < SlicerAudioProcessor::numPlaybackStyleOptions; ++i)
        styleSelector.addItem (SlicerAudioProcessor::getPlaybackStyleName (i), i + 1); // JUCE item IDs are 1-based

    styleSelector.setSelectedId (initialStyleId, juce::dontSendNotification);
    styleSelector.onChange = [this]
    {
        repaint();

        if (onStyleChanged)
            onStyleChanged (styleSelector.getSelectedId() - 1);
    };
}

std::vector<PlaybackStyleParameterPanel::PanelRow> PlaybackStyleParameterPanel::buildRowsForStyle (int style)
{
    std::vector<PanelRow> rows;
    const auto applicable = SlicerAudioProcessor::getApplicableSequencerCellParameters (style);

    for (int paramIndex : applicable)
    {
        if (paramIndex == 5 || paramIndex == 19) // Subdivide, Volume -- excluded, see class doc comment
            continue;

        if (SlicerAudioProcessor::isSequencerCellParameterSwept (paramIndex))
        {
            // Mode first, then Value -- same order the right-click menu's
            // own submenu-then-slider flow presents them in.
            rows.push_back ({ paramIndex + 1, true });
            rows.push_back ({ paramIndex, false });
        }
        else if (SlicerAudioProcessor::isSequencerCellParameterDiscrete (paramIndex)
            && ! SlicerAudioProcessor::isSequencerCellParameterSteppedSlider (paramIndex))
        {
            rows.push_back ({ paramIndex, true });
        }
        else
        {
            rows.push_back ({ paramIndex, false });
        }
    }

    return rows;
}

void PlaybackStyleParameterPanel::setSelectedStyle (int styleIndex)
{
    const int newId = styleIndex + 1;

    if (styleSelector.getSelectedId() == newId)
        return;

    styleSelector.setSelectedId (newId, juce::dontSendNotification);
    repaint();
}

int PlaybackStyleParameterPanel::getPreferredHeight()
{
    int maxRows = 0;

    for (int style = 0; style < SlicerAudioProcessor::numPlaybackStyleOptions; ++style)
        maxRows = juce::jmax (maxRows, (int) buildRowsForStyle (style).size());

    return styleSelectorHeight + rowGap + maxRows * (rowHeight + rowGap);
}

int PlaybackStyleParameterPanel::getPreferredHeightForStyle (int styleIndex)
{
    const int rowCount = (int) buildRowsForStyle (styleIndex).size();
    return styleSelectorHeight + rowGap + rowCount * (rowHeight + rowGap);
}

juce::Rectangle<int> PlaybackStyleParameterPanel::getRowBounds (int rowIndex) const
{
    const int y = styleSelectorHeight + rowGap + rowIndex * (rowHeight + rowGap);
    return { 0, y, getWidth(), rowHeight };
}

void PlaybackStyleParameterPanel::resized()
{
    styleSelector.setBounds (getLocalBounds().removeFromTop (styleSelectorHeight).removeFromLeft (200));
}

void PlaybackStyleParameterPanel::paint (juce::Graphics& g)
{
    const int style = styleSelector.getSelectedId() - 1;
    const auto rows = buildRowsForStyle (style);

    if (rows.empty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.setFont (12.0f);
        g.drawFittedText ("No adjustable parameters for this style",
                           getRowBounds (0), juce::Justification::centredLeft, 1);
        return;
    }

    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const auto& row = rows[(size_t) i];
        const auto bounds = getRowBounds (i);
        const juce::String name = SlicerAudioProcessor::getSequencerCellParameterName (row.paramIndex);

        if (row.discrete)
        {
            const int currentOption = juce::roundToInt (getValue (row.paramIndex));
            const juce::String optionName = SlicerAudioProcessor::getSequencerCellParameterOptionName (row.paramIndex, currentOption);

            g.setColour (juce::Colours::black.withAlpha (0.7f));
            g.fillRect (bounds);
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.drawRect (bounds, 1);

            g.setColour (juce::Colours::white);
            g.setFont (11.0f);
            g.drawFittedText (name + ": " + optionName, bounds.reduced (6, 0), juce::Justification::centredLeft, 1);

            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.setFont (10.0f);
            g.drawFittedText ("change...", bounds.reduced (6, 0), juce::Justification::centredRight, 1);
        }
        else
        {
            const float currentValue = getValue (row.paramIndex);
            const float minValue = SlicerAudioProcessor::getSequencerCellParameterMin (row.paramIndex);
            const float maxValue = SlicerAudioProcessor::getSequencerCellParameterMax (row.paramIndex);
            const float range = maxValue - minValue;
            const float t = range > 0.0f ? juce::jlimit (0.0f, 1.0f, (currentValue - minValue) / range) : 0.0f;

            g.setColour (juce::Colours::black.withAlpha (0.9f));
            g.fillRect (bounds);

            const auto fillBounds = bounds.withWidth ((int) ((float) bounds.getWidth() * t)).reduced (2);
            g.setColour (juce::Colours::cyan);
            g.fillRect (fillBounds);

            g.setColour (juce::Colours::white);
            g.drawRect (bounds, 1);
            g.setFont (11.0f);
            g.drawFittedText (name + ": " + juce::String (currentValue, 2), bounds.reduced (2), juce::Justification::centred, 1);
        }
    }
}

void PlaybackStyleParameterPanel::showDiscreteOptionsMenu (const PanelRow& row, juce::Rectangle<int> rowBounds)
{
    const int numOptions = SlicerAudioProcessor::getSequencerCellParameterNumOptions (row.paramIndex);
    const int currentOption = juce::roundToInt (getValue (row.paramIndex));

    juce::PopupMenu menu;

    for (int option = 0; option < numOptions; ++option)
        menu.addItem (option + 1, SlicerAudioProcessor::getSequencerCellParameterOptionName (row.paramIndex, option),
                      true, option == currentOption);

    // Parented to the top-level component, not this (potentially narrow)
    // panel, for the same reason SequencerGrid::showParameterMenuForCell()
    // does -- Rate's 20-item list would otherwise get squashed into
    // multiple columns by a parent too short to fit it as one list.
    const auto menuOptions = juce::PopupMenu::Options()
        .withParentComponent (getTopLevelComponent())
        .withTargetScreenArea (localAreaToGlobal (rowBounds));

    const int paramIndex = row.paramIndex;

    menu.showMenuAsync (menuOptions, [this, paramIndex] (int result)
    {
        if (result <= 0)
            return; // dismissed without choosing anything

        setValue (paramIndex, (float) (result - 1));
        repaint();
    });
}

void PlaybackStyleParameterPanel::updateContinuousValueFromMouseX (int paramIndex, int mouseX, const juce::Rectangle<int>& rowBounds)
{
    const float t = rowBounds.getWidth() > 0
        ? juce::jlimit (0.0f, 1.0f, (float) (mouseX - rowBounds.getX()) / (float) rowBounds.getWidth())
        : 0.0f;

    const float minValue = SlicerAudioProcessor::getSequencerCellParameterMin (paramIndex);
    const float maxValue = SlicerAudioProcessor::getSequencerCellParameterMax (paramIndex);
    setValue (paramIndex, minValue + t * (maxValue - minValue));
}

void PlaybackStyleParameterPanel::mouseDown (const juce::MouseEvent& event)
{
    const int style = styleSelector.getSelectedId() - 1;
    const auto rows = buildRowsForStyle (style);

    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const auto bounds = getRowBounds (i);

        if (! bounds.contains (event.x, event.y))
            continue;

        const auto& row = rows[(size_t) i];

        if (row.discrete)
        {
            showDiscreteOptionsMenu (row, bounds);
        }
        else
        {
            draggingParamIndex = row.paramIndex;
            draggingRowBounds = bounds;
            updateContinuousValueFromMouseX (row.paramIndex, event.x, bounds);
            repaint();
        }

        return;
    }
}

void PlaybackStyleParameterPanel::mouseDrag (const juce::MouseEvent& event)
{
    if (draggingParamIndex < 0)
        return;

    updateContinuousValueFromMouseX (draggingParamIndex, event.x, draggingRowBounds);
    repaint();
}

void PlaybackStyleParameterPanel::mouseUp (const juce::MouseEvent&)
{
    draggingParamIndex = -1;
}
