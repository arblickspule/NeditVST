#include <doctest/doctest.h>
#include "SlicerModel.h"
#include "SlicerEngine.h"

#include <cmath>

namespace
{
constexpr int sourceLength = 88200;
constexpr double hostRate = 44100.0;
constexpr int blockSize = 64;

void fillSource (SlicerModel& model)
{
    model.sampleBuffer.setSize (1, sourceLength);
    auto* src = model.sampleBuffer.getWritePointer (0);
    for (int i = 0; i < sourceLength; ++i)
        src[i] = 0.25f * std::sin (2.0 * double (juce::MathConstants<double>::pi) * double (i) / 97.0);
}

void readyTheModel (SlicerModel& model)
{
    fillSource (model);
    model.sampleLoaded = true;
    model.sampleSampleRate = 44100.0;
    model.trimStartSample.store (0);
    model.trimEndSample.store (sourceLength);
    model.tempoTrimStartSample.store (0);
    model.tempoTrimEndSample.store (sourceLength);
    model.slices = { { 0, 44100 } };
    model.sliceProbabilities = { 1.0f };
    model.playbackStyleProbabilities.assign (SlicerModel::numPlaybackStyleOptions, 0.0f);
    model.playbackStyleProbabilities[0] = 1.0f;
    model.setFadeInMs (0.0f);
    model.setFadeOutMs (0.0f);
    model.onPickStateInvalidated = [] {};
}

juce::AudioPlayHead::PositionInfo playingAt120Bpm()
{
    juce::AudioPlayHead::PositionInfo info;
    info.setBpm (120.0);
    info.setPpqPosition (0.0);
    info.setIsPlaying (true);
    return info;
}

bool allSamplesZero (const juce::AudioBuffer<float>& buffer)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (buffer.getSample (ch, i) != 0.0f)
                return false;
    return true;
}
}

TEST_CASE ("SlicerEngine: no sample loaded leaves the buffer untouched")
{
    SlicerModel model;
    model.onPickStateInvalidated = [] {};
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, true, hostRate);
    CHECK (allSamplesZero (buffer));
}

TEST_CASE ("SlicerEngine: empty slice list leaves the buffer untouched")
{
    SlicerModel model;
    readyTheModel (model);
    model.slices.clear();
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, true, hostRate);
    CHECK (allSamplesZero (buffer));
}

TEST_CASE ("SlicerEngine: stopped transport silences slice length mode")
{
    SlicerModel model;
    readyTheModel (model);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, false, hostRate);
    CHECK (allSamplesZero (buffer));
}

TEST_CASE ("SlicerEngine: audition reproduces the source exactly at 1:1 rate")
{
    SlicerModel model;
    readyTheModel (model);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);
    model.setAuditionActive (true);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;

    engine.processBlock (buffer, midi, {}, true, false, hostRate);

    const auto* src = model.sampleBuffer.getReadPointer (0);
    for (int k = 0; k < blockSize; ++k)
    {
        CHECK (buffer.getSample (0, k) == doctest::Approx (src[k]));
        CHECK (buffer.getSample (1, k) == doctest::Approx (src[k]));
    }
}

TEST_CASE ("SlicerEngine: audition scales by sample-rate ratio")
{
    SlicerModel model;
    readyTheModel (model);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);
    model.setAuditionActive (true);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;

    engine.processBlock (buffer, midi, {}, true, false, hostRate / 2.0);

    const auto* src = model.sampleBuffer.getReadPointer (0);
    for (int k = 0; k < blockSize; ++k)
    {
        CHECK (buffer.getSample (0, k) == doctest::Approx (src[2 * k]));
        CHECK (buffer.getSample (1, k) == doctest::Approx (src[2 * k]));
    }
}

TEST_CASE ("SlicerEngine: audition wraps at the trim end")
{
    SlicerModel model;
    readyTheModel (model);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);
    model.setAuditionActive (true);
    model.auditionPosition = (double) (model.trimEndSample.load() - 1);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;

    engine.processBlock (buffer, midi, {}, true, false, hostRate);

    const auto* src = model.sampleBuffer.getReadPointer (0);
    CHECK (buffer.getSample (0, 0) == doctest::Approx (src[0]));
    CHECK (buffer.getSample (1, 1) == doctest::Approx (src[1]));
}

TEST_CASE ("SlicerEngine: slice length mode renders a deterministic forward pick")
{
    SlicerModel model;
    readyTheModel (model);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, true, hostRate);

    const auto* src = model.sampleBuffer.getReadPointer (0);
    bool heardSomething = false;
    for (int k = 0; k < blockSize; ++k)
    {
        CHECK (buffer.getSample (0, k) == doctest::Approx (src[k]));
        CHECK (buffer.getSample (1, k) == doctest::Approx (src[k]));
        if (std::fabs (buffer.getSample (0, k)) > 1.0e-4)
            heardSomething = true;
    }
    CHECK (heardSomething);
}

TEST_CASE ("SlicerEngine: slice length mode keeps position across blocks")
{
    SlicerModel model;
    readyTheModel (model);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);
    const auto pos = playingAt120Bpm();
    juce::MidiBuffer midi;

    juce::AudioBuffer<float> first (2, blockSize);
    first.clear();
    engine.processBlock (first, midi, pos, true, true, hostRate);

    juce::AudioBuffer<float> second (2, blockSize);
    second.clear();
    engine.processBlock (second, midi, pos, true, true, hostRate);

    const auto* src = model.sampleBuffer.getReadPointer (0);
    CHECK (second.getSample (0, 0) == doctest::Approx (src[blockSize]));
    CHECK (second.getSample (0, blockSize - 1) == doctest::Approx (src[2 * blockSize - 1]));
}

TEST_CASE ("SlicerEngine: clock mode produces bounded finite audio on the first block")
{
    SlicerModel model;
    readyTheModel (model);
    model.triggerMode.store (SlicerModel::TriggerMode::clock);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, true, hostRate);

    bool heardSomething = false;
    for (int k = 0; k < blockSize; ++k)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const float sample = buffer.getSample (ch, k);
            CHECK (std::isfinite (sample));
            CHECK (std::fabs (sample) < 2.0);
            if (std::fabs (sample) > 1.0e-4)
                heardSomething = true;
        }
    }
    CHECK (heardSomething);
}

TEST_CASE ("SlicerEngine: performance mode recalls a snapshot with transport stopped")
{
    SlicerModel model;
    readyTheModel (model);
    model.triggerMode.store (SlicerModel::TriggerMode::performance);

    SlicerModel::PerformanceStateSnapshot snapshot;
    snapshot.populated = true;
    snapshot.trimStartSample = 0;
    snapshot.trimEndSample = sourceLength;
    snapshot.style = 0;
    snapshot.loop = false;
    model.performanceStateBank[60] = snapshot;

    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

    engine.processBlock (buffer, midi, {}, false, false, hostRate);

    const auto* src = model.sampleBuffer.getReadPointer (0);
    bool heardSomething = false;
    for (int k = 0; k < blockSize; ++k)
    {
        CHECK (buffer.getSample (0, k) == doctest::Approx (src[k]));
        CHECK (buffer.getSample (1, k) == doctest::Approx (src[k]));
        if (std::fabs (buffer.getSample (0, k)) > 1.0e-4)
            heardSomething = true;
    }
    CHECK (heardSomething);
}

TEST_CASE ("SlicerEngine: performance mode ignores an empty slot")
{
    SlicerModel model;
    readyTheModel (model);
    model.triggerMode.store (SlicerModel::TriggerMode::performance);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 40, (juce::uint8) 100), 0);

    engine.processBlock (buffer, midi, {}, false, false, hostRate);

    CHECK (allSamplesZero (buffer));
}
