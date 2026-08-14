#include "PluginProcessor.h"
#include "SlicerModel.h"
#include "ui/contract.h"

SlicerAudioProcessor::SlicerAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
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
    // The whole UI lives behind the ui::contract seam (gui.cpp); swapping
    // the GUI means swapping that implementation, never this line (docs/
    // ui-layout-decision.md). The old PluginEditor stays in the build until
    // the new Generate/Sequence/Perform pages replace it.
    return ui::makeEditor (*this, model, engine, {}).release();
}

void SlicerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Full instrument state (globals, probabilities, sequencer grid +
    // overrides, pattern bank, performance bank + working state) -- XML-
    // encoded by the model behind its saveState API (nextsteps 1.4, see
    // docs/state-serialization-decision.md). The audio sample itself is
    // intentionally NOT persisted; the restored preset is meaningful once
    // the matching sample is re-loaded.
    model.saveState (destData);
}

void SlicerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    model.restoreState (data, sizeInBytes);
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SlicerAudioProcessor();
}
