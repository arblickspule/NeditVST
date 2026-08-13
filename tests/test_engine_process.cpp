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

juce::AudioPlayHead::PositionInfo playingAtBpm (double bpm)
{
    juce::AudioPlayHead::PositionInfo info;
    info.setBpm (bpm);
    info.setPpqPosition (0.0);
    info.setIsPlaying (true);
    return info;
}

juce::AudioPlayHead::PositionInfo playingAt120Bpm() { return playingAtBpm (120.0); }

// Overwrites the loaded source with src[i] = i (a linear ramp), so a
// rendered output sample reads back as the exact source position it was
// taken from -- makes read-position math (folds, repitch stride) directly
// observable in the output.
void fillRamp (SlicerModel& model)
{
    auto* src = model.sampleBuffer.getWritePointer (0);
    for (int i = 0; i < model.sampleBuffer.getNumSamples(); ++i)
        src[i] = (float) i;
}

// Overwrites the loaded source with a constant, so output variation comes
// purely from the engine's own gain/rate envelope, not from source content.
void fillConstant (SlicerModel& model, float value)
{
    auto* src = model.sampleBuffer.getWritePointer (0);
    for (int i = 0; i < model.sampleBuffer.getNumSamples(); ++i)
        src[i] = value;
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

TEST_CASE ("SlicerEngine: fade-in ramps gain from zero across the requested fade length")
{
    SlicerModel model;
    readyTheModel (model);
    model.setFadeInMs (1.0f); // 1ms @ 44.1k = 44.1 host samples
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, true, hostRate);

    const auto* src = model.sampleBuffer.getReadPointer (0);
    const double fadeInSamples = 1.0 / 1000.0 * hostRate; // 44.1

    // First sample sits at gain 0.
    CHECK (buffer.getSample (0, 0) == doctest::Approx (0.0));

    // Mid-fade: output is the source scaled by samplesSincePickStart / fadeInSamples.
    CHECK (buffer.getSample (0, 20) == doctest::Approx (src[20] * (20.0 / fadeInSamples)));
    CHECK (std::fabs (buffer.getSample (0, 20)) < std::fabs (src[20])); // genuinely attenuated

    // Past the fade length: full gain, output == source.
    CHECK (buffer.getSample (0, 50) == doctest::Approx (src[50]));
}

TEST_CASE ("SlicerEngine: repitch plays the source faster when host tempo exceeds the loop tempo")
{
    SlicerModel model;
    readyTheModel (model);
    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;

    // Source spans 2s (1 bar => ~120bpm); host at 240bpm wants that bar in 1s,
    // so playbackRate doubles and output[k] == source[2k].
    const auto pos = playingAtBpm (240.0);
    engine.processBlock (buffer, midi, pos, true, true, hostRate);

    const auto* src = model.sampleBuffer.getReadPointer (0);
    bool differsFromUnityRate = false;
    for (int k = 0; k < blockSize / 2; ++k)
    {
        CHECK (buffer.getSample (0, k) == doctest::Approx (src[2 * k]));
        if (std::fabs (src[2 * k] - src[k]) > 1.0e-4)
            differsFromUnityRate = true;
    }
    CHECK (differsFromUnityRate); // proves the rate actually changed, not a 1:1 read
}

TEST_CASE ("SlicerEngine: Ping-Pong folds the read position back at the slice midpoint")
{
    SlicerModel model;
    readyTheModel (model);
    fillRamp (model); // output value == read position
    model.slices = { { 0, 40 } };
    model.sliceProbabilities = { 1.0f };
    model.playbackStyleProbabilities.assign (SlicerModel::numPlaybackStyleOptions, 0.0f);
    model.playbackStyleProbabilities[1] = 1.0f; // Ping-Pong only

    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, true, hostRate);

    // Forward leg counts up to the slice length; backward leg mirrors back
    // down (80 - k for a 40-sample slice, period 80).
    CHECK (buffer.getSample (0, 10) == doctest::Approx (10.0));
    CHECK (buffer.getSample (0, 39) == doctest::Approx (39.0));
    CHECK (buffer.getSample (0, 40) == doctest::Approx (40.0));
    CHECK (buffer.getSample (0, 50) == doctest::Approx (30.0));
    CHECK (buffer.getSample (0, 63) == doctest::Approx (17.0));

    // Ascends before the turnaround, descends after -- not a forward sawtooth.
    CHECK (buffer.getSample (0, 30) > buffer.getSample (0, 10));
    CHECK (buffer.getSample (0, 50) < buffer.getSample (0, 40));
}

TEST_CASE ("SlicerEngine: Tape Stop decays gain to near silence across the pick")
{
    SlicerModel model;
    readyTheModel (model);
    fillConstant (model, 0.5f); // isolate the gain/rate envelope from source content
    model.slices = { { 0, 128 } };
    model.sliceProbabilities = { 1.0f };
    model.playbackStyleProbabilities.assign (SlicerModel::numPlaybackStyleOptions, 0.0f);
    model.playbackStyleProbabilities[2] = 1.0f; // Tape Stop only

    SlicerEngine engine { model };
    engine.prepare (hostRate, 128);

    juce::AudioBuffer<float> buffer (2, 128);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, true, hostRate);

    // Linear decel over the 128-sample natural length: gain == 1 - k/128,
    // applied to a constant 0.5 source.
    CHECK (buffer.getSample (0, 0) == doctest::Approx (0.5));
    CHECK (buffer.getSample (0, 64) == doctest::Approx (0.5 * (1.0 - 64.0 / 128.0)));
    CHECK (buffer.getSample (0, 127) == doctest::Approx (0.5 * (1.0 - 127.0 / 128.0)));

    // Strictly monotonic decay throughout.
    for (int k = 1; k < 128; ++k)
        CHECK (buffer.getSample (0, k) < buffer.getSample (0, k - 1));
}

TEST_CASE ("SlicerEngine: Bitcrush holds and quantizes in sample-and-hold blocks")
{
    SlicerModel model;
    readyTheModel (model);

    auto* ramp = model.sampleBuffer.getWritePointer (0);
    for (int i = 0; i < 64; ++i)
        ramp[i] = 0.2f + 0.01f * (float) i; // rising, so consecutive hold blocks grab distinct values

    model.playbackStyleProbabilities.assign (SlicerModel::numPlaybackStyleOptions, 0.0f);
    model.playbackStyleProbabilities[6] = 1.0f; // Bitcrush only

    SlicerEngine engine { model };
    engine.prepare (hostRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto pos = playingAt120Bpm();

    engine.processBlock (buffer, midi, pos, true, true, hostRate);

    // Defaults: 12-sample hold, 5-bit depth (quantStep = 2 / 2^5 = 0.0625).
    // Grab at sample 0 (source 0.2 -> 0.1875) holds through sample 11; grab at
    // sample 12 (source 0.32 -> 0.3125) holds from there.
    CHECK (buffer.getSample (0, 0) == doctest::Approx (0.1875));
    CHECK (buffer.getSample (0, 5) == buffer.getSample (0, 0));   // held
    CHECK (buffer.getSample (0, 11) == buffer.getSample (0, 0));  // still held
    CHECK (buffer.getSample (0, 12) == doctest::Approx (0.3125)); // fresh grab
    CHECK (buffer.getSample (0, 20) == buffer.getSample (0, 12)); // held again
}

TEST_CASE ("SlicerEngine: sequenced mode triggers a placed step")
{
    SlicerModel model;
    readyTheModel (model);
    model.triggerMode.store (SlicerModel::TriggerMode::sequenced);
    model.setSequencerCell (0, 0, 0); // row 0, step 0, Forward

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
