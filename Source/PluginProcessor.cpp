#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SlicerModel.h"

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
    return new SlicerAudioProcessorEditor (*this);
}

void SlicerAudioProcessor::getStateInformation (juce::MemoryBlock& /*destData*/)
{
    // Persistence was never implemented -- the model owns ~30 stored
    // parameters (probability tables, pattern bank, performance bank,
    // globals) plus the schema tables to serialize them, but nothing writes
    // them to a MemoryBlock yet.
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
