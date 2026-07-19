#pragma once

#include <JuceHeader.h>

//==============================================================================
// STEP 1 of the build: plugin shell + sample loading + whole-sample playback.
//
// No slicing yet — every MIDI note just retriggers the whole loaded sample
// from the start, at unity pitch. This gets the plumbing (file loading,
// MIDI-triggered playback, polyphony) proven and testable in a DAW before
// we port the transient detector and cut the sample into slices.
//
// A `triggerProbability` hook is already wired in (see noteOn handling in
// the .cpp) so that when we get to the generative layer, we're extending
// something real rather than retrofitting it.
//==============================================================================

class SlicerAudioProcessor;

//==============================================================================
/** A "sound" that matches any MIDI note/channel — standard JUCE Synthesiser
    pattern. Later, when we have real slices, we'll likely map specific
    note ranges to specific slices; for now, one sound covers everything. */
class SliceSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote (int /*midiNoteNumber*/) override { return true; }
    bool appliesToChannel (int /*midiChannel*/) override { return true; }
};

//==============================================================================
/** A single playback voice. Reads directly from the processor's shared
    sample buffer. Multiple voices = polyphony (e.g. overlapping slice hits). */
class SliceVoice : public juce::SynthesiserVoice
{
public:
    explicit SliceVoice (SlicerAudioProcessor& ownerProcessor);

    bool canPlaySound (juce::SynthesiserSound* sound) override;
    void startNote (int midiNoteNumber, float velocity,
                     juce::SynthesiserSound* sound, int currentPitchWheelPosition) override;
    void stopNote (float velocity, bool allowTailOff) override;
    void pitchWheelMoved (int newPitchWheelValue) override;
    void controllerMoved (int controllerNumber, int newControllerValue) override;
    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer,
                           int startSample, int numSamples) override;

private:
    SlicerAudioProcessor& processor;
    double samplePosition = 0.0;
    double playbackRatio = 1.0; // sampleRate conversion ratio (source -> host)
    bool isActive = false;
    float currentVelocity = 1.0f;
};

//==============================================================================
class SlicerAudioProcessor : public juce::AudioProcessor
{
public:
    SlicerAudioProcessor();
    ~SlicerAudioProcessor() override;

    //=== Standard AudioProcessor overrides ===
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
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

    //=== Sample loading (called from the editor) ===
    void loadSample (const juce::File& file);
    bool hasSample() const { return sampleLoaded; }
    juce::String getLoadedFileName() const { return loadedFileName; }

    //=== Shared read access for voices ===
    const juce::AudioBuffer<float>& getSampleBuffer() const { return sampleBuffer; }
    double getSampleSampleRate() const { return sampleSampleRate; }
    juce::CriticalSection& getSampleLock() { return sampleLock; }

    //=== Generative hook (placeholder for the probability layer) ===
    // 0.0 = never trigger, 1.0 = always trigger. Used later once we have
    // multiple slices to choose between; for now it's unused by voices.
    std::atomic<float> triggerProbability { 1.0f };

private:
    juce::AudioFormatManager formatManager;
    juce::Synthesiser synth;

    juce::AudioBuffer<float> sampleBuffer;
    double sampleSampleRate = 44100.0;
    bool sampleLoaded = false;
    juce::String loadedFileName;
    juce::CriticalSection sampleLock; // guards sampleBuffer during loadSample()

    static constexpr int numVoices = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlicerAudioProcessor)
};
