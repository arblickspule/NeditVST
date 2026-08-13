#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SlicerModel.h"

SlicerAudioProcessor::SlicerAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // Ensure no stale MIDI-learn arm survives construction. Everything else
    // the constructor used to do (filter setup, onPickStateInvalidated,
    // debug watchdog) now lives in SlicerEngine (Phase 2).
    model.cancelMidiLearn();
}

SlicerAudioProcessor::~SlicerAudioProcessor()
{
}

void SlicerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
}

void SlicerAudioProcessor::releaseResources() {}

bool SlicerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SlicerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // Playhead/transport state is sampled HERE, ahead of the lock (host
    // reads only -- behaviorally identical to the old pre-lock read), then
    // handed to the engine, whose processBlock() takes model.sampleLock
    // itself exactly as the old body did.
    auto* playHead = getPlayHead();
    const auto position = playHead != nullptr ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo>{};
    const bool hostTransportPlaying = position.hasValue() && position->getIsPlaying();

    engine.processBlock (buffer, midiMessages, position, playHead != nullptr, hostTransportPlaying, getSampleRate());
}

juce::AudioProcessorEditor* SlicerAudioProcessor::createEditor()
{
    return new SlicerAudioProcessorEditor (*this);
}

void SlicerAudioProcessor::getStateInformation (juce::MemoryBlock& /*destData*/)
{
    // TODO once there are parameters worth persisting (loop length, slice
    // probabilities) — not wired up yet in this step.
}

void SlicerAudioProcessor::setStateInformation (const void* /*data*/, int /*sizeInBytes*/)
{
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SlicerAudioProcessor();
}
