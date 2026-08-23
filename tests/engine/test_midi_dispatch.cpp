// routeMidiNote(): mode-dependent MIDI routing into the scheduler's
// per-mode entry points, plus the velocity byte conversion. Runs on the
// audio thread inside process(), so the tests drive it through real
// process() blocks.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/MidiDispatch.h>

#include <cstdint>
#include <vector>

using namespace nedit::engine;
using namespace nedit::state;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kRate = 44100.0;

struct DispatchFixture
{
    PluginState state;
    std::vector<Slice> slices { Slice { 0, 44100 }, Slice { 44100, 88200 } };
    std::vector<float> source;
    BlockContext ctx;
    VoiceScheduler scheduler;

    DispatchFixture()
        : source (static_cast<std::size_t> (88200))
    {
        state.sample.samplePath = "test.wav";
        state.sample.sampleSampleRate = kRate;
        state.sample.sampleLengthFrames = static_cast<std::int64_t> (source.size());
        state.sample.trimStartFrame = 0;
        state.sample.trimEndFrame = static_cast<std::int64_t> (source.size());
        state.generate.sliceWeights.assign (slices.size(), 1.0f);

        ctx.hostSampleRate = kRate;
        ctx.sourceSampleRate = kRate;
        scheduler.prepare (kRate);
        scheduler.setSeed (7u);
    }

    void run (bool playing, int numSamples)
    {
        const float* channels[] = { source.data() };
        ctx.source = channels;
        ctx.sourceChannels = 1;
        ctx.sourceFrames = static_cast<std::int64_t> (source.size());

        std::vector<float> sink (static_cast<std::size_t> (numSamples), 0.0f);
        float* outs[] = { sink.data() };
        scheduler.process (state, slices, ctx, { playing, 120.0, 0.0 },
                           outs, 1, numSamples);
    }

    void noteOn (int note)
    {
        routeMidiNote (scheduler, state, note, velocityFromMidiByte (100),
                       true, false, static_cast<int> (slices.size()));
    }

    void noteOff (int note)
    {
        routeMidiNote (scheduler, state, note, 0.0f,
                       false, false, static_cast<int> (slices.size()));
    }

    [[nodiscard]] std::uint64_t picks() const { return scheduler.picksStarted(); }
};

} // namespace

TEST_CASE ("midi dispatch: velocity byte conversion", "[midi]")
{
    CHECK_THAT (velocityFromMidiByte (127), WithinAbs (1.0, 1e-9));
    CHECK_THAT (velocityFromMidiByte (100), WithinAbs (100.0f / 127.0f, 1e-6));
    CHECK (velocityFromMidiByte (0) == 0.0f);
}

TEST_CASE ("midi dispatch: performance note-on recalls a snapshot while stopped",
           "[midi]")
{
    DispatchFixture fx;
    fx.state.triggerMode = TriggerMode::performance;

    PerformanceSnapshot snapshot;
    snapshot.populated = true;
    snapshot.trimStartFrame = 11025;
    snapshot.trimEndFrame = 55125;
    fx.state.performance.bank[60] = snapshot;

    fx.run (false, 1);   // silent before any note
    CHECK (fx.picks() == 0);

    fx.noteOn (60);
    fx.run (false, 1);   // stopped transport: recall falls straight through
    REQUIRE (fx.picks() == 1);
    CHECK (fx.scheduler.renderer().currentPick().sliceStartFrame == 11025);

    fx.noteOff (60);     // performance ignores note-offs entirely
    fx.run (false, 1);
    CHECK (fx.picks() == 1);
}

TEST_CASE ("midi dispatch: control notes trigger, keyswitches select",
           "[midi]")
{
    DispatchFixture fx;
    fx.state.triggerMode = TriggerMode::control;

    fx.noteOn (35);      // base-1-0: selects style 0 silently
    CHECK (fx.scheduler.controlActiveStyleOrdinal() == 0);
    fx.run (false, 1);
    CHECK (fx.picks() == 0);

    fx.noteOn (37);      // slice 1 with the selected style
    fx.run (false, 1);
    REQUIRE (fx.picks() == 1);

    const auto& pick = fx.scheduler.renderer().currentPick();
    CHECK (pick.sliceStartFrame == 44100);
    CHECK_THAT (pick.velocityGain, WithinAbs (100.0 / 127.0, 1e-6));
}

TEST_CASE ("midi dispatch: control gate release via note-off route",
           "[midi]")
{
    DispatchFixture fx;
    fx.state.triggerMode = TriggerMode::control;
    fx.state.control.gateMode = true;
    fx.state.render.fadeOutMs = 5;   // ~220 samples of ramp

    fx.noteOn (36);
    fx.run (false, 10);
    REQUIRE (fx.picks() == 1);

    fx.noteOff (36);                 // gate mode: begin the release fade
    fx.run (false, 100);
    CHECK (fx.scheduler.renderer().hasPick());   // still inside the ramp

    fx.run (false, 400);             // ramp long since complete
    CHECK_FALSE (fx.scheduler.renderer().hasPick());
}

TEST_CASE ("midi dispatch: sequenced notes arm pattern recalls only on note-on",
           "[midi]")
{
    DispatchFixture fx;
    fx.state.triggerMode = TriggerMode::sequenced;

    fx.noteOff (3);
    CHECK_FALSE (fx.scheduler.patternSwitchPending());   // offs never arm

    fx.noteOn (3);
    CHECK (fx.scheduler.patternSwitchPending());
}

TEST_CASE ("midi dispatch: slice length and clock ignore MIDI entirely",
           "[midi]")
{
    for (const auto mode : { TriggerMode::sliceLength, TriggerMode::clock })
    {
        DispatchFixture fx;
        fx.state.triggerMode = mode;
        fx.state.sequencer.patternBank[3].populated = false;

        // Stopped transport: in these modes ONLY the host beat position
        // can start picks, so any activity here would mean the routed
        // note had done something.
        fx.noteOn (36);
        fx.run (false, 32);
        fx.noteOff (36);
        fx.run (false, 32);

        CHECK (fx.picks() == 0);
        CHECK_FALSE (fx.scheduler.patternSwitchPending());
        CHECK_FALSE (fx.scheduler.performanceRecallPending());
    }
}
