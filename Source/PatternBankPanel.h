#pragma once

#include <JuceHeader.h>
#include <array>

//==============================================================================
/** A 128-note-indexed MIDI bank UI -- shows, at a glance, which slots are
    populated and which one is currently active, and provides the "Save
    to..." control that arms MIDI Learn for assigning whatever the caller's
    context currently holds to whichever note is played next.

    Slots are laid out piano-style: 12 columns (pitch class, C at the left)
    by 11 rows (octave, low notes at the top), matching MIDI note number
    directly (row = note / 12, column = note % 12) -- no separate remapping
    UI needed. An empty cell is a plain outline; a populated cell is filled;
    the active cell also gets a bright border. A slot with a switch pending
    against it gets its own amber dashed ring instead -- deliberately a
    different colour/style from the active cell's solid white border, so
    "headed there" is never mistaken for "already there." Hovering a cell
    updates the status label below the grid with that note's name, so
    identifying a slot doesn't require memorizing raw MIDI note numbers.

    Deliberately generic over WHAT gets saved/recalled -- everything specific
    to a slot's own contents (Sequencer patterns, Performance states, or any
    future context) lives entirely on the BankSource side; this class only
    ever asks BankSource for slot occupancy/active/pending state and to
    arm/cancel Learn. Two contexts share this today: the Sequencer pattern
    bank (whose BankSource wraps SlicerModel's own pattern-bank
    methods, including its real Pattern Switch Timing pending slot) and
    Performance mode's state bank (Pass 1 -- Immediate switching only, so its
    BankSource::getPendingSlot() always returns -1).

    Polls the source on a timer (same reasoning as SequencerGrid's own 30fps
    poll -- the audio thread can populate/activate slots on its own, via an
    incoming note-on, with nothing else to push a repaint). */
class PatternBankPanel : public juce::Component,
                          private juce::Timer
{
public:
    // Everything this panel needs from whatever context it's showing a bank
    // for -- see class doc comment above. getPendingSlot() returns -1 for a
    // context with no quantized-switch concept (Performance mode, Pass 1).
    struct BankSource
    {
        virtual ~BankSource() = default;
        virtual void armSave() = 0;
        virtual void cancelLearn() = 0;
        virtual bool isLearnArmed() const = 0;
        virtual std::array<bool, 128> getPopulatedSlots() const = 0;
        virtual int getActiveSlot() const = 0;
        virtual int getPendingSlot() const = 0;
    };

    explicit PatternBankPanel (BankSource& sourceToUse);
    ~PatternBankPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;

    static constexpr int preferredWidth = 190;
    static int getPreferredHeight();

private:
    void timerCallback() override;
    void saveButtonClicked();

    // -1 if the given point isn't over any cell.
    int getNoteAtPosition (juce::Point<int> position) const;
    juce::Rectangle<int> getCellBounds (int noteNumber) const;

    void updateStatusLabelForHoveredNote (int noteNumber);
    void updateSaveButtonText();

    BankSource& source;

    juce::TextButton saveButton { "Save to..." };
    juce::Label statusLabel;

    std::array<bool, 128> populatedSlots {};
    int activeSlot = -1;
    int pendingSlot = -1;
    bool learnArmed = false;

    static constexpr int columns = 12;
    static constexpr int rows = 11; // ceil(128 / 12)
    static constexpr int cellSize = 14;
    static constexpr int cellGap = 2;
    static constexpr int gridTop = 26;      // below the Save button
    static constexpr int statusLabelHeight = 18;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternBankPanel)
};
