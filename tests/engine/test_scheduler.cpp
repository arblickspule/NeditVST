// VoiceScheduler: Slice Length + Clock scheduling behaviour (timing tests
// with deterministic seeds), plus the Tempo.h helpers they rely on.
//
// Timing constants used throughout: host 44.1 kHz @ 120 bpm ->
// 1 beat = 22050 samples, 8n = 11025, a 1-bar reset window = 88200.
//
// Exact ppq boundaries carry a +-1-sample double-rounding jitter (the
// engine computes each sample's beat position as blockPpq + i*ppqPerSample,
// exactly like the original), so event assertions use the playQuiet /
// advanceToPicks pair: a proven-quiet stretch up to just before the
// boundary, then a small budget in which the event MUST fire.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <engine/Scheduler.h>
#include <engine/Tempo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace nedit::engine;
using namespace nedit::state;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kRate = 44100.0;

// Source of numBars bars (2 s per bar at 44.1 kHz), trimmed to its full
// length with loopLengthBars matching -- repitchRatio and playbackRate
// come out exactly 1.0 at any host tempo.
struct Fixture
{
    PluginState state;
    std::vector<Slice> slices;
    std::vector<float> source;
    BlockContext ctx;
    VoiceScheduler scheduler;

    explicit Fixture (std::vector<Slice> sliceList, int numBars = 1)
        : slices (std::move (sliceList)), source (static_cast<std::size_t> (88200 * numBars))
    {
        for (std::size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<float> (i) / static_cast<float> (source.size());

        state.sample.samplePath = "test.wav";
        state.sample.sampleSampleRate = kRate;
        state.sample.sampleLengthFrames = static_cast<std::int64_t> (source.size());
        state.sample.trimStartFrame = 0;
        state.sample.trimEndFrame = static_cast<std::int64_t> (source.size());
        state.sample.loopLengthBars = numBars;

        state.generate.sliceWeights.assign (slices.size(), 1.0f);
        // Default styleWeights draw Forward only; default reset = 4 bars.

        ctx.hostSampleRate = kRate;
        ctx.sourceSampleRate = kRate;

        scheduler.prepare (kRate);
        scheduler.setSeed (7u);
    }

    void run (const TransportFrame& transport, int numSamples)
    {
        const float* channels[] = { source.data() };
        ctx.source = channels;
        ctx.sourceChannels = 1;
        ctx.sourceFrames = static_cast<std::int64_t> (source.size());

        std::vector<float> sink (static_cast<std::size_t> (numSamples), 0.0f);
        float* outs[] = { sink.data() };
        scheduler.process (state, slices, ctx, transport, outs, 1, numSamples);
    }

    // Play while the transport runs, advancing the ppq cursor exactly
    // like a host would across blocks (the scheduler derives each
    // sample's beat position from the block's START ppq).
    void play (int numSamples)
    {
        run ({ true, 120.0, ppqCursor }, numSamples);
        ppqCursor += static_cast<double> (numSamples) * ((120.0 / 60.0) / kRate);
        totalProcessed += numSamples;
    }

    // Play while rendering into an explicit output buffer, so tests can
    // inspect the actual waveform (envelope tails, fades) rather than just
    // the pick counter.
    void playInto (const TransportFrame& transport, int numSamples, std::vector<float>& sink)
    {
        sink.assign (static_cast<std::size_t> (numSamples), 0.0f);
        const float* channels[] = { source.data() };
        ctx.source = channels;
        ctx.sourceChannels = 1;
        ctx.sourceFrames = static_cast<std::int64_t> (source.size());

        float* outs[] = { sink.data() };
        scheduler.process (state, slices, ctx, transport, outs, 1, numSamples);
        ppqCursor += static_cast<double> (numSamples) * ((120.0 / 60.0) / kRate);
        totalProcessed += numSamples;
    }

    [[nodiscard]] std::uint64_t picks() const { return scheduler.picksStarted(); }

    // Play `numSamples` during which NO new pick may start.
    void playQuiet (int numSamples)
    {
        const auto before = picks();
        play (numSamples);
        INFO ("expected quiet stretch of " << numSamples << " samples");
        REQUIRE (picks() == before);
    }

    // Play up to `budget` samples until the pick counter reaches
    // `target`. Absorbs the +-1-sample floating-point jitter of exact
    // ppq boundaries without loosening what the tests pin down.
    void advanceToPicks (std::uint64_t target, int budget)
    {
        const int startTotal = totalProcessed;

        while (picks() < target)
        {
            INFO ("waiting for pick #" << target << " (budget " << budget << ")");
            REQUIRE (totalProcessed - startTotal <= budget);
            play (1);
        }
    }

    double ppqCursor = 0.0;
    int totalProcessed = 0;

    // --- sequencer helpers ---------------------------------------------------
    // Size the working grid like the UI mutators do: rows = slice count,
    // columns = one bar of 16n steps (16 at any tempo).
    void initSequencerGrid()
    {
        auto& seq = state.sequencer;
        seq.stepResolutionIndex = kNoteValue16n;
        seq.patternLengthBarsIndex = kDefaultPatternLengthBarsIndex;
        seq.rows = static_cast<int> (slices.size());
        seq.columns = 16;
        seq.grid.assign (static_cast<std::size_t> (seq.rows) * static_cast<std::size_t> (seq.columns),
                         static_cast<std::int8_t> (-1));
    }

    void fillCell (int row, int column, std::int8_t style)
    {
        auto& seq = state.sequencer;
        seq.grid[static_cast<std::size_t> (row) * static_cast<std::size_t> (seq.columns)
                 + static_cast<std::size_t> (column)] = style;
    }

    void setCellOverride (int row, int column, StyleParamId id, float value)
    {
        const auto cell = static_cast<std::uint32_t> (row) * static_cast<std::uint32_t> (state.sequencer.columns)
                        + static_cast<std::uint32_t> (column);
        state.sequencer.overrides[cell][id] = value;
    }

    [[nodiscard]] SequencerPattern makeBankPattern (
        int slot, std::vector<std::pair<int, int>> cells) const
    {
        SequencerPattern pattern;
        pattern.populated = true;
        pattern.stepResolutionIndex = kNoteValue16n;
        pattern.patternLengthBarsIndex = kDefaultPatternLengthBarsIndex;
        pattern.rows = static_cast<int> (slices.size());
        pattern.columns = 16;
        pattern.grid.assign (static_cast<std::size_t> (pattern.rows) * static_cast<std::size_t> (pattern.columns),
                             static_cast<std::int8_t> (-1));

        for (const auto& [row, column] : cells)
            pattern.grid[static_cast<std::size_t> (row) * static_cast<std::size_t> (pattern.columns)
                         + static_cast<std::size_t> (column)] = 0;   // Forward

        (void) slot;
        return pattern;
    }
};

} // namespace

TEST_CASE ("tempo: nearest note value", "[scheduler][tempo]")
{
    CHECK (tempo::nearestNoteValueIndex (1.0) == kNoteValue4n);
    CHECK (tempo::nearestNoteValueIndex (0.249) == 7);   // just under 16n
    CHECK (tempo::nearestNoteValueIndex (2.6) == 17);    // 1nt = 8/3 beats

    // Exact tie -> earliest (shortest) entry: 0.4375 sits halfway between
    // 16nd (0.375) and 8n (0.5).
    CHECK (tempo::nearestNoteValueIndex (0.4375) == 9);
}

TEST_CASE ("tempo: beat quantize target", "[scheduler][tempo]")
{
    CHECK_FALSE (tempo::computeBeatQuantizeTarget (0, false, kRate, 120.0, 120.0).quantized);
    CHECK_FALSE (tempo::computeBeatQuantizeTarget (44100, false, kRate, 0.0, 120.0).quantized);

    // 3 s slice @ 120 original bpm = 6 beats: decomposes to 1 bar + 2n.
    const auto multiBar = tempo::computeBeatQuantizeTarget (3 * 44100, false, kRate, 120.0, 120.0);
    REQUIRE (multiBar.quantized);
    CHECK_THAT (multiBar.targetHostSeconds, WithinAbs (3.0, 1e-12));
    CHECK_THAT (multiBar.stretchRatio, WithinAbs (1.0, 1e-12));

    // Ping-Pong quantizes the FULL ROUND TRIP: a 1 s slice spans 2 s.
    const auto roundTrip = tempo::computeBeatQuantizeTarget (44100, true, kRate, 120.0, 120.0);
    REQUIRE (roundTrip.quantized);
    CHECK_THAT (roundTrip.targetHostSeconds, WithinAbs (2.0, 1e-12));

    // Host tempo mismatch produces a compensating ratio: 2 beats of audio
    // against a 60 bpm host lands on 2 x 1 s.
    const auto halfTime = tempo::computeBeatQuantizeTarget (44100, false, kRate, 120.0, 60.0);
    REQUIRE (halfTime.quantized);
    CHECK_THAT (halfTime.targetHostSeconds, WithinAbs (2.0, 1e-12));
    CHECK_THAT (halfTime.stretchRatio, WithinAbs (0.5, 1e-12));
}

TEST_CASE ("tempo: scratch cycle length", "[scheduler][tempo]")
{
    // Rate 4n @ 120 bpm wants a 22050-sample cycle; the leg clamps to the
    // 1000-frame slice, so both legs pin to the content length.
    const double clamped =
        tempo::scratchCycleLengthHostSamples (kNoteValue4n, 1000, 120.0, kRate, 1.0);
    CHECK_THAT (clamped, WithinAbs (2000.0, 1e-9));

    // Roomy slice: the full desired cycle survives (1 beat round trip).
    const double roomy =
        tempo::scratchCycleLengthHostSamples (kNoteValue4n, 44100, 120.0, kRate, 1.0);
    CHECK_THAT (roomy, WithinAbs (22050.0, 1e-9));

    CHECK (tempo::scratchCycleLengthHostSamples (kNoteValue4n, 1000, 120.0, kRate, 0.0) == 0.0);
}

TEST_CASE ("slice length: immediate pick on transport start", "[scheduler][sl]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.play (10);

    CHECK (fx.picks() == 1);
    CHECK (fx.scheduler.renderer().hasPick());
}

TEST_CASE ("slice length: chains picks back to back", "[scheduler][sl]")
{
    // One natural pass = 44100 samples at rate 1; the chain fires on the
    // sample where position reaches end-1, so consecutive picks are
    // spaced len-1 apart (the original's own completion convention).
    Fixture fx ({ Slice { 0, 44100 } });

    fx.play (1);
    CHECK (fx.picks() == 1);

    fx.playQuiet (44096);          // through sample 44097
    fx.advanceToPicks (2, 5);      // chain near sample 44099

    fx.playQuiet (44095);          // nothing before ~88198
    fx.advanceToPicks (3, 6);
}

TEST_CASE ("slice length: reset boundary cuts with anticipated fade", "[scheduler][sl]")
{
    // Reset every 1 bar (88200 samples); a 3 s slice outlives the window,
    // so its pick is capped up front and cut exactly on the boundary.
    Fixture fx ({ Slice { 0, 132300 } }, /*numBars*/ 2);
    fx.state.generate.resetBarsIndex = 0;

    fx.play (1);
    REQUIRE (fx.picks() == 1);
    CHECK_THAT (fx.scheduler.renderer().currentPick().pickLengthHostSamples,
                WithinAbs (88200.0, 1.0));

    fx.playQuiet (88196);          // quiet through ~88198
    fx.advanceToPicks (2, 6);      // boundary cut + rechain by 88201
}

TEST_CASE ("slice length: fade-out engages at slice end even with a decaying source", "[scheduler][sl][fade]")
{
    // A single-slice forward pick through the REAL trigger path, with a
    // decaying source (the "natural envelope" that can mask a fade-out).
    // Source ramps 1.0 -> 0.4 across the slice, so the slice's own end
    // amplitude is audibly nonzero (0.4). A long fade-out must nevertheless
    // pull the tail toward silence.
    constexpr std::int64_t kLen = 44100;
    constexpr std::int64_t kFadeWin = 4410;   // 100 ms @ 44.1 kHz

    std::vector<float> sink;

    // ----- control: no fade-out. The tail stays at the source's natural
    // value (~0.4) until the pick ends. --------------------------------------
    {
        Fixture fx ({ Slice { 0, kLen } });
        for (std::int64_t i = 0; i < kLen; ++i)
            fx.source[static_cast<std::size_t> (i)] =
                static_cast<float> (1.0 - 0.6 * (static_cast<double> (i) / static_cast<double> (kLen)));
        fx.state.render.fadeInMs = 0.0f;
        fx.state.render.fadeOutMs = 0.0f;

        // The last sample that still belongs to pick 1 (the pick ends at
        // position kLen-1, so its final rendered sample is at index kLen-2;
        // the very next sample already chains pick 2 from the slice top).
        const auto lastPick1 = static_cast<std::size_t> (kLen) - 2;

        fx.playInto ({ true, 120.0, 0.0 }, static_cast<int> (kLen), sink);
        const float tailValue = std::fabs (sink[lastPick1]);
        CHECK_THAT (static_cast<double> (tailValue), WithinAbs (0.4, 0.02));

        // ----- with a 100 ms fade-out: the tail ramps to ~0. ---------------
        Fixture fx2 ({ Slice { 0, kLen } });
        for (std::int64_t i = 0; i < kLen; ++i)
            fx2.source[static_cast<std::size_t> (i)] =
                static_cast<float> (1.0 - 0.6 * (static_cast<double> (i) / static_cast<double> (kLen)));
        fx2.state.render.fadeInMs = 0.0f;
        fx2.state.render.fadeOutMs = 100.0f;

        fx2.playInto ({ true, 120.0, 0.0 }, static_cast<int> (kLen), sink);

        const float fadedTail = std::fabs (sink[lastPick1]);

        // The fade-out clearly engages: the tail is pulled far below the
        // source's natural end amplitude (0.4) and below the no-fade control.
        CHECK (fadedTail < 0.03f);
        CHECK (fadedTail < tailValue * 0.1f);

        // And the ramp is progressive across the fade window: the midpoint of
        // the fade sits between the natural level and silence.
        const auto halfWay = static_cast<std::size_t> (kLen) - static_cast<std::size_t> (kFadeWin / 2);
        CHECK_THAT (static_cast<double> (std::fabs (sink[halfWay])), WithinAbs (0.2, 0.03));
    }
}

TEST_CASE ("slice length: reset bars change takes effect at next boundary", "[scheduler][sl]")
{
    Fixture fx ({ Slice { 0, 132300 } }, /*numBars*/ 2);
    fx.state.generate.resetBarsIndex = 0;   // 1-bar windows

    fx.play (1000);
    REQUIRE (fx.picks() == 1);

    // Widen to 2 bars mid-stream. The ALREADY-ARMED boundary near 88200
    // still cuts (live-read affects the NEXT advance), but the following
    // window now spans 8 beats instead of two further 1-bar cuts.
    fx.state.generate.resetBarsIndex = 1;

    fx.playQuiet (87196);          // quiet through ~88197
    fx.advanceToPicks (2, 6);      // armed boundary fires ~88200

    fx.playQuiet (88190);          // no second 1-bar cut anywhere in between
    fx.advanceToPicks (3, 15);     // widened boundary ~176400
}

TEST_CASE ("slice length: beat quantize substitutes palette duration", "[scheduler][sl]")
{
    // 57330 frames = 1.3 s = 2.6 beats @ 120 bpm original -> quantizes to
    // 1nt (8/3 beats) -> 4/3 s = 58800 samples, ratio 0.975.
    Fixture fx ({ Slice { 0, 57330 } });
    fx.state.render.beatQuantizeRepitch = true;

    fx.play (10);
    REQUIRE (fx.picks() == 1);

    const auto& pick = fx.scheduler.renderer().currentPick();
    CHECK (pick.beatQuantized);
    CHECK_THAT (pick.pickLengthHostSamples, WithinAbs (58800.0, 0.5));
    CHECK_THAT (pick.quantizedStretchRatio, WithinAbs (0.975, 1e-9));
}

TEST_CASE ("slice length: all-zero weights still plays via uniform fallback", "[scheduler][sl]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    std::fill (fx.state.generate.sliceWeights.begin(),
               fx.state.generate.sliceWeights.end(), 0.0f);

    fx.play (10);
    CHECK (fx.picks() == 1);
}

TEST_CASE ("slice length: transport stop silences and rearms", "[scheduler][sl]")
{
    Fixture fx ({ Slice { 0, 44100 } });

    fx.play (100);
    CHECK (fx.scheduler.renderer().hasPick());

    fx.run ({ false, 120.0, fx.ppqCursor }, 50);   // transport stops: ppq freezes
    CHECK_FALSE (fx.scheduler.renderer().hasPick());

    const auto before = fx.picks();
    fx.play (10);   // fresh snap + immediate pick on restart
    CHECK (fx.picks() == before + 1);
}

TEST_CASE ("slice length: unimplemented modes stay silent", "[scheduler]")
{
    Fixture fx ({ Slice { 0, 44100 } });

    for (const auto mode : { TriggerMode::sequenced, TriggerMode::performance,
                             TriggerMode::control })
    {
        fx.state.triggerMode = mode;
        fx.play (64);
        CHECK_FALSE (fx.scheduler.renderer().hasPick());
        CHECK (fx.picks() == 0);
    }
}

TEST_CASE ("clock: immediate pick then per-tick retriggers", "[scheduler][clock]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::clock;

    // Reference 4n (default): 1-beat windows = 22050 samples. Force an
    // 8n subdivision (11025 samples): events land every half-window --
    // ticks inside, and the window boundary itself redraws.
    std::fill (fx.state.generate.subdivisionWeights.begin(),
               fx.state.generate.subdivisionWeights.end(), 0.0f);
    fx.state.generate.subdivisionWeights[10] = 1.0f;

    fx.play (1);
    CHECK (fx.picks() == 1);   // forced first tick at once

    // Every tick retriggers from the slice start, capped to one tick.
    CHECK_THAT (fx.scheduler.renderer().currentPick().pickLengthHostSamples,
                WithinAbs (11025.0, 1.0));

    fx.playQuiet (11022);
    fx.advanceToPicks (2, 5);      // tick ~11025

    fx.playQuiet (11021);
    fx.advanceToPicks (3, 5);      // boundary + tick together ~22050

    fx.playQuiet (11021);
    fx.advanceToPicks (4, 5);      // tick ~33075

    fx.advanceToPicks (5, 11032);  // next window boundary ~44100
}

TEST_CASE ("clock: whole-window tape stop renders once per window", "[scheduler][clock]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::clock;

    std::fill (fx.state.generate.styleWeights.begin(),
               fx.state.generate.styleWeights.end(), 0.0f);
    fx.state.generate.styleWeights[2] = 1.0f;       // Tape Stop only
    std::fill (fx.state.generate.subdivisionWeights.begin(),
               fx.state.generate.subdivisionWeights.end(), 0.0f);
    fx.state.generate.subdivisionWeights[10] = 1.0f;  // 8n ticks (ignored)

    fx.play (1);
    CHECK (fx.picks() == 1);
    CHECK_THAT (fx.scheduler.renderer().currentPick().tapeStopDurationHostSamples,
                WithinAbs (22050.0, 1.0));          // the whole 1-beat window

    fx.playQuiet (22046);
    fx.advanceToPicks (2, 6);      // only the window boundary itself

    fx.playQuiet (22045);
    fx.advanceToPicks (3, 7);
}

TEST_CASE ("clock: per-tick tape stop retriggers every tick", "[scheduler][clock]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::clock;
    fx.state.generate.tapeStopScope = WindowScope::perTick;

    std::fill (fx.state.generate.styleWeights.begin(),
               fx.state.generate.styleWeights.end(), 0.0f);
    fx.state.generate.styleWeights[2] = 1.0f;       // Tape Stop only
    std::fill (fx.state.generate.subdivisionWeights.begin(),
               fx.state.generate.subdivisionWeights.end(), 0.0f);
    fx.state.generate.subdivisionWeights[10] = 1.0f;  // 8n

    fx.play (1);
    CHECK (fx.picks() == 1);
    CHECK_THAT (fx.scheduler.renderer().currentPick().tapeStopDurationHostSamples,
                WithinAbs (11025.0, 1.0));          // one TICK, not the window

    fx.playQuiet (11022);
    fx.advanceToPicks (2, 5);
}

TEST_CASE ("clock: stretch spans the window ignoring subdivisions", "[scheduler][clock]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::clock;

    std::fill (fx.state.generate.styleWeights.begin(),
               fx.state.generate.styleWeights.end(), 0.0f);
    fx.state.generate.styleWeights[3] = 1.0f;       // Stretch only
    std::fill (fx.state.generate.subdivisionWeights.begin(),
               fx.state.generate.subdivisionWeights.end(), 0.0f);
    fx.state.generate.subdivisionWeights[10] = 1.0f;  // 8n ticks (ignored)

    fx.play (1);
    REQUIRE (fx.picks() == 1);

    // Duration = min(Grain Speed x natural, window) = min(4 x 44100, 22050).
    CHECK_THAT (fx.scheduler.renderer().currentPick().pickLengthHostSamples,
                WithinAbs (22050.0, 1.0));

    fx.playQuiet (22046);
    fx.advanceToPicks (2, 6);      // one continuous render per window
}

TEST_CASE ("clock: ticks clamp to the window end", "[scheduler][clock]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::clock;
    fx.state.generate.clockReferenceIndex = kNoteValue1n;   // 4-beat windows

    std::fill (fx.state.generate.subdivisionWeights.begin(),
               fx.state.generate.subdivisionWeights.end(), 0.0f);
    fx.state.generate.subdivisionWeights[kNoteValue4n] = 1.0f;  // 1-beat ticks

    fx.play (1);
    CHECK (fx.picks() == 1);

    fx.playQuiet (22047);          // tick ~22050
    fx.advanceToPicks (2, 5);

    fx.playQuiet (22047);          // tick ~44100
    fx.advanceToPicks (3, 5);

    fx.playQuiet (22047);          // tick ~66150
    fx.advanceToPicks (4, 5);

    fx.advanceToPicks (5, 22062);  // then ONLY the window boundary ~88200:
                                   // the clamped nextTick IS that boundary
}

// ===========================================================================
// Sequenced mode. Timing constants @ 120 bpm / 44.1 kHz with a 16n step
// grid: one step = 5512.5 samples, one bar pattern = 16 steps = 88200.
// ===========================================================================

TEST_CASE ("sequenced: filled column triggers immediately and loops", "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, 0);   // Forward on the very first column

    // Entering the mode re-syncs to whichever step is current: step 0 is
    // filled, so the note fires at once.
    fx.play (1);
    CHECK (fx.picks() == 1);
    CHECK (fx.scheduler.playingStepIndex() == 0);
    CHECK_THAT (fx.scheduler.renderer().currentPick().pickLengthHostSamples,
                WithinAbs (44100.0, 2.0));   // natural slice length in steps

    // Empty columns never retrigger; the next event is the PATTERN wrap
    // back onto column 0 after a full bar (~88200).
    fx.playQuiet (88194);
    fx.advanceToPicks (2, 10);
}

TEST_CASE ("sequenced: empty columns leave silence after the note fades", "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 3, 0);   // only column 3 filled

    // Entering inside step 0: nothing triggers (column 0 is empty), and
    // no boundary ever produces a pick until the wrap onto column 3.
    fx.play (5512);
    CHECK (fx.picks() == 0);

    // Crossing into column 3 starts the note...
    fx.advanceToPicks (1, 11040);
    CHECK (fx.scheduler.playingStepIndex() == 3);

    // ...and after its natural length the voice runs out silently; no
    // further retriggers for the rest of the pattern.
    fx.playQuiet (44100 + 30000);
    CHECK (fx.scheduler.renderer().finished (fx.ctx));
}

TEST_CASE ("sequenced: next active column caps the note", "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, 0);   // Forward
    fx.fillCell (0, 4, 1);   // Ping-Pong four steps later

    // The first note anticipates the cut at column 4: four 16n steps =
    // 1 beat = 22050 samples, well short of its natural 44100.
    fx.play (1);
    REQUIRE (fx.picks() == 1);
    CHECK_THAT (fx.scheduler.renderer().currentPick().pickLengthHostSamples,
                WithinAbs (22050.0, 2.0));

    fx.playQuiet (22046);
    fx.advanceToPicks (2, 8);

    // The second note carries the cell's own style.
    CHECK (fx.scheduler.renderer().currentPick().style == PlaybackStyle::pingPong);
    CHECK (fx.scheduler.renderer().currentPick().halfSliceFold);   // sequenced half-window
}

TEST_CASE ("sequenced: scratch plays one Rate cycle, not a grid step", "[scheduler][seq][scratch]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, static_cast<std::int8_t> (PlaybackStyle::scratch));

    fx.play (1);
    REQUIRE (fx.picks() == 1);
    const auto& pick = fx.scheduler.renderer().currentPick();

    // Scratch must declare ONE full Rate cycle (default Rate = 16n), NOT the
    // natural slice/step length (44100) the generic path used to hand it --
    // that was the sequencer-only bug: the whip never completed within its
    // window. With only column 0 filled the next active column is the
    // pattern wrap (16 steps, 88200), so the cycle is not capped here.
    const double expectedCycle = tempo::scratchCycleLengthHostSamples (
        kNoteValue16n, 44100, 120.0, kRate, 1.0);
    CHECK_THAT (pick.pickLengthHostSamples, WithinAbs (expectedCycle, 2.0));
    CHECK (pick.useDurationGate);   // fold style ends by declared window
    CHECK_FALSE (pick.pickLengthHostSamples > 44000.0);   // was the natural length
}

TEST_CASE ("sequenced: cell overrides beat the generated slider params", "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, 3);   // Stretch
    fx.setCellOverride (0, 0, StyleParamId::grainSizeMs, 25.0f);

    fx.play (1);
    REQUIRE (fx.picks() == 1);

    const auto& params = fx.scheduler.renderer().currentPick().params;
    CHECK_THAT (params.grainSizeMs, WithinAbs (25.0f, 1e-6f));   // override wins
    CHECK_THAT (params.grainSpeed,
                WithinAbs (fx.state.generate.styleParams.grainSpeed, 1e-6f));
}

TEST_CASE ("sequenced: notes without overrides use the generate (slider) params",
           "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, static_cast<std::int8_t> (PlaybackStyle::flanger));

    // A slider value on the Generate/Sequence bands flows into the cell:
    // the pick's params come from generate.styleParams (the surface the UI
    // edits), NOT the sequencer's own fallback copy.
    fx.state.generate.styleParams.set (StyleParamId::flangerDelayMs, 8.5f);
    fx.state.sequencer.fallbackParams.set (StyleParamId::flangerDelayMs, 1.0f);

    fx.play (1);
    REQUIRE (fx.picks() == 1);

    const auto& params = fx.scheduler.renderer().currentPick().params;
    CHECK_THAT (params.flangerDelayMs, WithinAbs (8.5f, 1e-6f));
}

TEST_CASE ("sequenced: Subdivide retriggers within a step, sweeps stay whole-step",
           "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, 0);   // Forward
    fx.setCellOverride (0, 0, StyleParamId::subdivide,
                        static_cast<float> (kNoteValue4n) + 1.0f);   // 1-beat ticks

    fx.play (1);
    REQUIRE (fx.picks() == 1);

    // Declared length = 8 natural steps = 44100; the window spans THAT,
    // while each subdivision slot (this trigger included) lasts one tick.
    const auto& pick = fx.scheduler.renderer().currentPick();
    CHECK_THAT (pick.pickLengthHostSamples, WithinAbs (22050.0, 2.0));

    // Whole-step progress for every swept parameter while Subdivide is on.
    CHECK (pick.volumeRampActive);
    CHECK (pick.volumeWholeWindow);
    CHECK (pick.flangerWholeWindow);
    CHECK (pick.filterWholeWindow);

    // One retrigger at the tick boundary ~22050, then quiet until the
    // pattern wraps back onto column 0 at ~88200.
    fx.playQuiet (22045);
    fx.advanceToPicks (2, 8);
    CHECK_THAT (fx.scheduler.renderer().currentPick().pickLengthHostSamples,
                WithinAbs (22050.0, 2.0));   // min(tick, remaining window)

    fx.playQuiet (66140);
    fx.advanceToPicks (3, 15);
}

TEST_CASE ("sequenced: immediate pattern switch applies on the next check",
           "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, 0);
    fx.state.sequencer.patternBank[60] = fx.makeBankPattern (60, { { 0, 7 } });

    fx.play (1);
    REQUIRE (fx.picks() == 1);

    fx.scheduler.requestPatternSwitch (60);
    CHECK (fx.scheduler.patternSwitchPending());

    // Immediate timing never defers: the recall lands on the very next
    // per-sample check. The step tracker re-syncs against the NEW grid
    // (whose column 0 is empty), so the playhead moves but nothing fires.
    fx.play (2);
    CHECK_FALSE (fx.scheduler.patternSwitchPending());
    CHECK (fx.scheduler.playingStepIndex() == 0);

    // The audible proof of the swap: the recalled pattern's column 7
    // triggers at absolute step 7 (~7 x 5512.5 = 38587.5).
    fx.playQuiet (38580);
    fx.advanceToPicks (2, 10);
}

TEST_CASE ("sequenced: set-interval switch defers to the next interval boundary",
           "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, 0);
    fx.state.sequencer.patternBank[61] = fx.makeBankPattern (61, { { 0, 5 } });
    fx.state.sequencer.patternSwitchTiming = PatternSwitchTiming::setInterval;
    fx.state.sequencer.patternSwitchIntervalIndex = kNoteValue4n;   // 1-beat intervals

    fx.play (1);      // column 0 fires
    fx.play (1000);   // mid-step request
    REQUIRE (fx.picks() == 1);
    fx.scheduler.requestPatternSwitch (61);

    // The recall boundary is the next whole beat (~22050 absolute), but
    // the RECALLED pattern only fills column 5 (~27562.5): nothing may
    // fire anywhere in between -- not at the boundary itself.
    fx.playQuiet (26548);
    fx.advanceToPicks (2, 16);
    CHECK (fx.scheduler.playingStepIndex() == 5);
}

TEST_CASE ("sequenced: end-of-pattern switch waits for the genuine wrap",
           "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 15, 0);   // last column filled
    fx.state.sequencer.patternBank[62] = fx.makeBankPattern (62, { { 0, 0 } });
    fx.state.sequencer.patternSwitchTiming = PatternSwitchTiming::endOfPattern;

    // Entering mid-pattern (step 0 empty here) stays silent until 15...
    fx.play (15 * 5512);
    CHECK (fx.picks() == 0);

    fx.advanceToPicks (1, 8);
    CHECK (fx.scheduler.playingStepIndex() == 15);

    // Request DURING the final step; the wrap (15 -> 0) is the earliest
    // legal boundary -- a full step later -- where the recalled pattern's
    // own column 0 fires.
    fx.scheduler.requestPatternSwitch (62);
    fx.advanceToPicks (2, 5525);
    CHECK (fx.scheduler.playingStepIndex() == 0);
}

TEST_CASE ("sequenced: switching to an empty bank slot is a no-op", "[scheduler][seq]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, 0);

    fx.play (1);
    REQUIRE (fx.picks() == 1);

    fx.scheduler.requestPatternSwitch (99);   // never populated
    fx.play (64);
    CHECK_FALSE (fx.scheduler.patternSwitchPending());   // dropped at the check
    CHECK (fx.picks() == 1);                             // nothing disturbed

    // And the working pattern keeps looping as if nothing happened.
    fx.playQuiet (88130);
    fx.advanceToPicks (2, 10);
}

TEST_CASE ("sequenced: working-pattern release is deferred to the next block",
           "[scheduler][seq]")
{
    // requestWorkingPatternRelease is callable from ANY thread; the drop
    // itself happens at the top of the next process() block (destroying
    // recalledPattern_ mid-block would free the containers SequencerView
    // holds raw pointers into -- the use-after-free this API prevents).
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::sequenced;
    fx.initSequencerGrid();
    fx.fillCell (0, 0, 0);
    fx.state.sequencer.patternBank[60] = fx.makeBankPattern (60, { { 0, 7 } });

    fx.play (1);
    REQUIRE (fx.picks() == 1);

    // Recall the bank pattern (immediate timing) and prove it is live:
    // ITS column 7 fires (~38587.5 absolute).
    fx.scheduler.requestPatternSwitch (60);
    fx.play (2);
    CHECK_FALSE (fx.scheduler.patternSwitchPending());
    fx.playQuiet (38580 - fx.totalProcessed);
    fx.advanceToPicks (2, 10);

    // Drop the override. Applied on the NEXT process() call; the working
    // grid (column 0 only) is live again, so its wrap pick fires at
    // ~88200 absolute and nothing in between.
    fx.scheduler.requestWorkingPatternRelease();
    fx.playQuiet (88190 - fx.totalProcessed);
    fx.advanceToPicks (3, 20);
    CHECK (fx.scheduler.playingStepIndex() == 0);

    // The recalled pattern's column 7 must NOT fire again (~126787 would
    // be its next slot if the override still shadowed the working grid).
    fx.playQuiet (127000 - fx.totalProcessed);
}

TEST_CASE ("scheduler: out-of-range state indices are guarded (hardening)",
           "[scheduler]")
{
    // The scheduler consumes state snapshots as given. sanitize() normally
    // guarantees these ranges, but a missed clamp upstream must not become
    // an OOB read of the 4-entry kResetBarsValues table or a
    // floor(x / 0.0) -> int64 cast (UB): both indices are now guarded at
    // the point of use like every other state-derived index in the file.

    SECTION ("slice length: resetBarsIndex clamps into the table")
    {
        Fixture fx ({ Slice { 0, 88200 } });
        fx.state.generate.resetBarsIndex = -5;
        fx.play (4096);
        CHECK (fx.picks() > 0);

        Fixture fx2 ({ Slice { 0, 88200 } });
        fx2.state.generate.resetBarsIndex = 9999;
        fx2.play (4096);
        CHECK (fx2.picks() > 0);
    }

    SECTION ("clock: invalid clockReferenceIndex falls back to the 4n reference")
    {
        Fixture fx ({ Slice { 0, 88200 } });
        fx.state.triggerMode = TriggerMode::clock;
        fx.state.generate.clockReferenceIndex = 999;
        fx.play (4096);
        CHECK (fx.picks() > 0);

        // The fallback window is a quarter note (22050 samples @ 120 bpm):
        // the window clock the renderer sees must be finite and positive.
        CHECK (std::isfinite (fx.scheduler.renderer().windowLengthSamples()));
        CHECK (fx.scheduler.renderer().windowLengthSamples() > 0.0);
    }
}

// ===========================================================================
// Performance mode: MIDI-recalled snapshots over their own segments.
// ===========================================================================

namespace
{

[[nodiscard]] PerformanceSnapshot makeSnapshot (std::int64_t trimStart,
                                                std::int64_t trimEnd)
{
    PerformanceSnapshot snapshot;
    snapshot.populated = true;
    snapshot.trimStartFrame = trimStart;
    snapshot.trimEndFrame = trimEnd;
    snapshot.style = 0;     // Forward
    snapshot.loop = false;
    snapshot.sync = true;
    return snapshot;
}

} // namespace

TEST_CASE ("performance: silent until a recall, one-shot by default", "[scheduler][perf]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::performance;
    fx.state.performance.bank[60] = makeSnapshot (0, 44100);

    fx.play (1000);
    CHECK (fx.picks() == 0);
    CHECK_FALSE (fx.scheduler.renderer().hasPick());

    fx.scheduler.requestPerformanceRecall (fx.state, 60, true);
    fx.play (1);
    REQUIRE (fx.picks() == 1);

    const auto& pick = fx.scheduler.renderer().currentPick();
    CHECK (pick.sliceStartFrame == 0);
    CHECK (pick.sliceLengthFrames == 44100);
    CHECK_FALSE (pick.nativeRate);

    // Loop off: when the segment runs its course the voice goes silent
    // and stays silent until the next note-on. (The pick only started
    // one block ago -- its age is 1 + this stretch, so play well past
    // its ~44099-sample exhaustion point.)
    fx.playQuiet (44200);
    CHECK_FALSE (fx.scheduler.renderer().hasPick());
    CHECK (fx.picks() == 1);

    // An empty unfocused slot is a no-op.
    fx.scheduler.requestPerformanceRecall (fx.state, 61, true);
    fx.play (64);
    CHECK_FALSE (fx.scheduler.renderer().hasPick());
    CHECK (fx.picks() == 1);
}

TEST_CASE ("performance: loop on rechains the same segment", "[scheduler][perf]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::performance;

    auto snap = makeSnapshot (22050, 66150);
    snap.loop = true;
    fx.state.performance.bank[42] = snap;

    fx.scheduler.requestPerformanceRecall (fx.state, 42, true);
    fx.play (1);
    REQUIRE (fx.picks() == 1);

    fx.playQuiet (44093);          // quiet until just before the natural end
    fx.advanceToPicks (2, 8);      // rechain near sample 44099

    const auto& pick = fx.scheduler.renderer().currentPick();
    CHECK (pick.sliceStartFrame == 22050);   // SAME segment again
    CHECK (pick.sliceLengthFrames == 44100);
}

TEST_CASE ("performance: focused slot plays the shared trim live", "[scheduler][perf]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::performance;
    fx.state.sample.trimStartFrame = 11025;
    fx.state.sample.trimEndFrame = 77175;

    auto& working = fx.state.performance.workingState;
    working.populated = true;
    working.style = 0;
    fx.state.performance.focusedSlot = 5;

    fx.scheduler.requestPerformanceRecall (fx.state, 5, true);
    fx.play (1);
    REQUIRE (fx.picks() == 1);

    // The focused segment IS the shared SampleState trim -- not the
    // working snapshot's own fields, and never the other way round.
    const auto& pick = fx.scheduler.renderer().currentPick();
    CHECK (pick.sliceStartFrame == 11025);
    CHECK (pick.sliceLengthFrames == 66150);
}

TEST_CASE ("performance: quantized recall waits for the interval grid", "[scheduler][perf]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::performance;
    fx.state.performance.bank[61] = makeSnapshot (44100, 88200);
    fx.state.performance.quantizeRecallEnabled = true;
    fx.state.performance.quantizeRecallIntervalIndex = kNoteValue4n;   // 1-beat grid

    // Armed while "playing": lands on the very next whole beat (~22050),
    // not before -- even though the transport only starts afterwards here.
    fx.scheduler.requestPerformanceRecall (fx.state, 61, true);
    CHECK (fx.scheduler.performanceRecallPending());

    fx.playQuiet (22046);
    fx.advanceToPicks (1, 8);

    CHECK (fx.scheduler.renderer().currentPick().sliceStartFrame == 44100);
}

TEST_CASE ("performance: quantized recall applies at once when stopped", "[scheduler][perf]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::performance;
    fx.state.performance.bank[62] = makeSnapshot (11025, 55125);
    fx.state.performance.quantizeRecallEnabled = true;

    // Transport not playing: no meaningful beat position exists, so the
    // recall falls straight through (auditionable without pressing play).
    fx.scheduler.requestPerformanceRecall (fx.state, 62, false);

    fx.run ({ false, 120.0, 0.0 }, 1);
    CHECK (fx.picks() == 1);
    CHECK (fx.scheduler.renderer().currentPick().sliceStartFrame == 11025);
}

TEST_CASE ("performance: sync off plays at native rate", "[scheduler][perf]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::performance;

    auto snap = makeSnapshot (0, 44100);
    snap.sync = false;
    fx.state.performance.bank[63] = snap;

    fx.scheduler.requestPerformanceRecall (fx.state, 63, true);
    fx.play (1);
    REQUIRE (fx.picks() == 1);
    CHECK (fx.scheduler.renderer().currentPick().nativeRate);
}

// ===========================================================================
// Control mode: chromatic slice triggering with keyswitch style selection
// (baseNote 36: keyswitches at notes 27..35, slices at 36 and up).
// ===========================================================================

TEST_CASE ("control: silent without notes, keyswitches select style silently",
           "[scheduler][ctl]")
{
    Fixture fx ({ Slice { 0, 44100 }, Slice { 44100, 88200 } });
    fx.state.triggerMode = TriggerMode::control;

    fx.play (64);
    CHECK (fx.picks() == 0);

    fx.scheduler.controlNoteOn (36 - 1 - 3, 1.0f, 36, 2);   // style ordinal 3
    CHECK (fx.scheduler.controlActiveStyleOrdinal() == 3);

    fx.play (64);
    CHECK (fx.picks() == 0);   // keyswitches never make sound
}

TEST_CASE ("control: slice notes trigger with velocity gain", "[scheduler][ctl]")
{
    Fixture fx ({ Slice { 0, 44100 }, Slice { 44100, 88200 } });
    fx.state.triggerMode = TriggerMode::control;
    fx.state.control.styleParams.grainSpeed = 2.0f;   // stretch window: 2 x natural

    fx.scheduler.controlNoteOn (36 - 1 - 3, 0.8f, 36, 2);   // select Stretch...
    REQUIRE (fx.scheduler.controlActiveStyleOrdinal() == 3);
    fx.scheduler.controlNoteOn (36 + 1, 0.25f, 36, 2);      // ...then play slice 1

    fx.play (1);
    REQUIRE (fx.picks() == 1);

    const auto& pick = fx.scheduler.renderer().currentPick();
    CHECK (pick.sliceStartFrame == 44100);
    CHECK (pick.sliceLengthFrames == 44100);
    CHECK (pick.style == PlaybackStyle::stretch);
    CHECK_THAT (pick.velocityGain, WithinAbs (0.25, 1e-9));

    // One-shot: the DECLARED window (natural x grain speed) runs out and
    // the voice stays silent until the next note-on -- even though the
    // folded position never exhausts the slice by itself.
    fx.playQuiet (44000);
    CHECK (fx.scheduler.renderer().hasPick());   // still inside the 2x window

    fx.playQuiet (45000);                        // now past 88200 total
    CHECK_FALSE (fx.scheduler.renderer().hasPick());
}

TEST_CASE ("control: notes outside both ranges are no-ops", "[scheduler][ctl]")
{
    Fixture fx ({ Slice { 0, 44100 }, Slice { 44100, 88200 } });
    fx.state.triggerMode = TriggerMode::control;

    fx.scheduler.controlNoteOn (20, 1.0f, 36, 2);       // below every range
    fx.scheduler.controlNoteOn (38, 1.0f, 36, 2);       // beyond the slice cap

    fx.play (64);
    CHECK (fx.picks() == 0);
    CHECK_FALSE (fx.scheduler.renderer().hasPick());
}

TEST_CASE ("control: monophonic retrigger replaces the sounding note", "[scheduler][ctl]")
{
    Fixture fx ({ Slice { 0, 44100 }, Slice { 44100, 88200 } });
    fx.state.triggerMode = TriggerMode::control;

    fx.scheduler.controlNoteOn (36, 1.0f, 36, 2);
    fx.play (10);
    REQUIRE (fx.picks() == 1);
    CHECK (fx.scheduler.controlSoundingNote() == 36);

    fx.scheduler.controlNoteOn (37, 1.0f, 36, 2);
    fx.play (1);
    REQUIRE (fx.picks() == 2);
    CHECK (fx.scheduler.controlSoundingNote() == 37);
    CHECK (fx.scheduler.renderer().currentPick().sliceStartFrame == 44100);
}

TEST_CASE ("control: gate release fades and force-stops the pick", "[scheduler][ctl]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::control;
    fx.state.control.gateMode = true;
    fx.state.render.fadeOutMs = 20;   // ~882 samples of release ramp

    fx.scheduler.controlNoteOn (36, 1.0f, 36, 1);
    fx.play (10);
    REQUIRE (fx.picks() == 1);

    fx.scheduler.controlNoteOff (36, true);
    fx.play (400);
    CHECK (fx.scheduler.renderer().hasPick());   // still inside the ramp

    fx.play (1200);                              // ramp long since complete
    CHECK_FALSE (fx.scheduler.renderer().hasPick());
}

TEST_CASE ("control: trigger mode ignores note-offs entirely", "[scheduler][ctl]")
{
    Fixture fx ({ Slice { 0, 44100 } });
    fx.state.triggerMode = TriggerMode::control;
    fx.state.control.gateMode = false;

    fx.scheduler.controlNoteOn (36, 1.0f, 36, 1);
    fx.play (10);
    REQUIRE (fx.picks() == 1);

    fx.scheduler.controlNoteOff (36, false);
    fx.play (2000);
    CHECK (fx.scheduler.renderer().hasPick());   // untouched by the note-off
}

TEST_CASE ("scheduler: sliceWeightsOverride replaces state weights in the picker")
{
    const std::vector<Slice> list {
        {      0, 22050 },
        {  22050, 44100 },
        {  44100, 88200 },
    };

    // Baseline: the default path (fixture fills state weights with 1.0)
    // starts picks across a bar.
    Fixture base (list, 1);
    base.play (88200);
    REQUIRE (base.picks() > 0);

    // Empty override = no pickable slices even though the STATE weights are
    // all 1.0 -> proves the override is really consulted. The shell uses
    // this to hand the scheduler a list clipped to the SOFT trim, so a
    // clip-of-everything goes silent instead of playing outside the trim.
    Fixture fx (list, 1);
    std::vector<float> empty;
    const float* channels[] = { fx.source.data() };
    fx.ctx.source = channels;
    fx.ctx.sourceChannels = 1;
    fx.ctx.sourceFrames = static_cast<std::int64_t> (fx.source.size());
    std::vector<float> sink (static_cast<std::size_t> (88200), 0.0f);
    float* outs[] = { sink.data() };
    fx.scheduler.process (fx.state, fx.slices, fx.ctx, { true, 120.0, 0.0 },
                          outs, 1, 88200, &empty);
    CHECK (fx.picks() == 0);

    // Override matching the state weights reproduces the default pick count
    // (same deterministic seed) -- the shell's clipped-slices + remapped-
    // weights pair routes exactly like the untouched list.
    Fixture match (list, 1);
    const std::vector<float> sameWeights (list.size(), 1.0f);
    const float* mChannels[] = { match.source.data() };
    match.ctx.source = mChannels;
    match.ctx.sourceChannels = 1;
    match.ctx.sourceFrames = static_cast<std::int64_t> (match.source.size());
    std::vector<float> mSink (static_cast<std::size_t> (88200), 0.0f);
    float* mOuts[] = { mSink.data() };
    match.scheduler.process (match.state, match.slices, match.ctx,
                             { true, 120.0, 0.0 }, mOuts, 1, 88200, &sameWeights);
    CHECK (match.picks() == base.picks());
}
