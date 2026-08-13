#pragma once

#include <JuceHeader.h>
#include "SlicerModel.h"
#include "SlicerEngine.h"

//==============================================================================
// SlicerAudioProcessor -- the thin JUCE plugin shell (Phase 3).
//
// Everything that used to live here moved out across the refactor:
//   - Phase 1: shared audio state, the sample, slices, undo, serialization
//     tables and every parameter -> SlicerModel (the UI now talks to the
//     model directly, not through this class).
//   - Phase 2: the real-time audio core (processBlock() DSP, MIDI dispatch,
//     audition, per-pick state, FreezeWatchdog) -> SlicerEngine.
//   - Phase 3: the ~220 public forwarding accessors the UI previously
//     called on this class were deleted once the UI retargeted onto
//     SlicerModel/SlicerEngine (target architecture: Engine <- Model <-> UI).
//
// What remains is genuinely plugin plumbing: the JUCE lifecycle overrides,
// the state-serialization stubs, and the two objects the other two layers
// are built around (public, since the editor binds its model/engine
// references from them).
//==============================================================================

class SlicerAudioProcessor : public juce::AudioProcessor
{
public:
    SlicerAudioProcessor();
    ~SlicerAudioProcessor() override;

    // Shared audio state + model.sampleLock (Phase 1). The UI talks to this
    // directly; see SlicerModel.h.
    SlicerModel model;

    // Real-time audio core (Phase 2) -- takes model.sampleLock itself in
    // processBlock(); see SlicerEngine.h.
    SlicerEngine engine { model };

    //=== Standard AudioProcessor overrides ===
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; } // Sequenced mode's pattern-bank recall (see SlicerEngine.h) needs note-on input
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessor)
};
