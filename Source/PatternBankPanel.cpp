#include "PatternBankPanel.h"

PatternBankPanel::PatternBankPanel (BankSource& sourceToUse)
    : source (sourceToUse)
{
    addAndMakeVisible (saveButton);
    saveButton.onClick = [this] { saveButtonClicked(); };

    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));
    addAndMakeVisible (statusLabel);

    populatedSlots = source.getPopulatedSlots();
    activeSlot = source.getActiveSlot();
    pendingSlot = source.getPendingSlot();
    learnArmed = source.isLearnArmed();
    updateSaveButtonText();
    updateStatusLabelForHoveredNote (-1);

    setSize (preferredWidth, getPreferredHeight());
    startTimerHz (15); // slot state can change on its own (a note-on arriving), nothing else to trigger a repaint
}

PatternBankPanel::~PatternBankPanel()
{
    stopTimer();
}

int PatternBankPanel::getPreferredHeight()
{
    return gridTop + rows * cellSize + (rows - 1) * cellGap + statusLabelHeight;
}

void PatternBankPanel::resized()
{
    saveButton.setBounds (0, 0, getWidth(), 22);
    statusLabel.setBounds (0, getHeight() - statusLabelHeight, getWidth(), statusLabelHeight);
}

juce::Rectangle<int> PatternBankPanel::getCellBounds (int noteNumber) const
{
    const int row = noteNumber / columns;
    const int col = noteNumber % columns;
    const int x = col * (cellSize + cellGap);
    const int y = gridTop + row * (cellSize + cellGap);
    return { x, y, cellSize, cellSize };
}

int PatternBankPanel::getNoteAtPosition (juce::Point<int> position) const
{
    if (position.y < gridTop)
        return -1;

    const int col = position.x / (cellSize + cellGap);
    const int row = (position.y - gridTop) / (cellSize + cellGap);

    if (col < 0 || col >= columns || row < 0 || row >= rows)
        return -1;

    const int note = row * columns + col;

    if (note >= 128 || ! getCellBounds (note).contains (position))
        return -1;

    return note;
}

void PatternBankPanel::updateSaveButtonText()
{
    saveButton.setButtonText (learnArmed ? "Play a note..." : "Save to...");
}

void PatternBankPanel::updateStatusLabelForHoveredNote (int noteNumber)
{
    if (noteNumber >= 0)
    {
        const auto name = juce::MidiMessage::getMidiNoteName (noteNumber, true, true, 3);
        const bool populated = populatedSlots[(size_t) noteNumber];
        statusLabel.setText (name + (populated ? " (saved)" : " (empty)"), juce::dontSendNotification);
        return;
    }

    if (learnArmed)
    {
        statusLabel.setText ("Play a note to save here", juce::dontSendNotification);
        return;
    }

    // Active and pending are independent facts (a switch can be pending
    // while a DIFFERENT pattern is still the one actually sounding) --
    // shown together, never conflated into one word.
    juce::String text;

    if (activeSlot >= 0)
        text << "Active: " << juce::MidiMessage::getMidiNoteName (activeSlot, true, true, 3);

    if (pendingSlot >= 0)
    {
        if (text.isNotEmpty())
            text << "  ";

        text << "Pending: " << juce::MidiMessage::getMidiNoteName (pendingSlot, true, true, 3);
    }

    statusLabel.setText (text, juce::dontSendNotification);
}

void PatternBankPanel::mouseMove (const juce::MouseEvent& event)
{
    updateStatusLabelForHoveredNote (getNoteAtPosition (event.getPosition()));
}

void PatternBankPanel::mouseExit (const juce::MouseEvent&)
{
    updateStatusLabelForHoveredNote (-1);
}

void PatternBankPanel::saveButtonClicked()
{
    if (learnArmed)
        source.cancelLearn();
    else
        source.armSave();

    learnArmed = source.isLearnArmed();
    updateSaveButtonText();
    updateStatusLabelForHoveredNote (-1);
    repaint();
}

void PatternBankPanel::timerCallback()
{
    const auto newPopulated = source.getPopulatedSlots();
    const int newActiveSlot = source.getActiveSlot();
    const int newPendingSlot = source.getPendingSlot();
    const bool newLearnArmed = source.isLearnArmed();

    const bool changed = (newPopulated != populatedSlots) || (newActiveSlot != activeSlot)
                          || (newPendingSlot != pendingSlot) || (newLearnArmed != learnArmed);

    if (! changed)
        return;

    populatedSlots = newPopulated;
    activeSlot = newActiveSlot;
    pendingSlot = newPendingSlot;
    learnArmed = newLearnArmed;
    updateSaveButtonText();
    updateStatusLabelForHoveredNote (getNoteAtPosition (getMouseXYRelative()));
    repaint();
}

void PatternBankPanel::paint (juce::Graphics& g)
{
    if (learnArmed)
    {
        const juce::Rectangle<int> gridBounds (0, gridTop, columns * cellSize + (columns - 1) * cellGap,
                                                 rows * cellSize + (rows - 1) * cellGap);
        g.setColour (juce::Colours::orange);
        g.drawRect (gridBounds.expanded (2), 2);
    }

    for (int note = 0; note < 128; ++note)
    {
        const auto bounds = getCellBounds (note);
        const bool populated = populatedSlots[(size_t) note];
        const bool active = (note == activeSlot);
        const bool pending = (note == pendingSlot);

        g.setColour (populated ? juce::Colours::mediumseagreen : juce::Colours::black.withAlpha (0.25f));
        g.fillRect (bounds);

        g.setColour (active ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
        g.drawRect (bounds, active ? 2 : 1);

        // Pending switch (Pattern Switch Timing: Set Interval/End of
        // Pattern) -- a dashed amber ring, deliberately a different colour
        // AND stroke style from the active cell's solid white border, so a
        // pattern that's merely headed-toward is never mistaken for one
        // that's already sounding. Drawn outside the cell's own border
        // (expanded) so it never visually merges with it, including when a
        // slot is simultaneously pending AND the current active slot.
        if (pending)
        {
            const auto pendingBounds = bounds.expanded (2).toFloat();
            juce::Path dashedRing;
            dashedRing.addRectangle (pendingBounds);

            float dashLengths[] = { 3.0f, 2.0f };
            juce::Path dashedPath;
            juce::PathStrokeType (1.5f).createDashedStroke (dashedPath, dashedRing, dashLengths, 2);

            g.setColour (juce::Colours::orange);
            g.fillPath (dashedPath);
        }
    }
}
