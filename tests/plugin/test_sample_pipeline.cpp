// Nedit -- Plugin-layer tests: WAV decode + SampleManager + the first
// truly AUDIBLE end-to-end pass through the VST3 shell (decode ->
// analyze -> slices -> scheduler -> rendered output energy).

#include <plugin/NeditProcessor.h>
#include <plugin/SampleManager.h>
#include <plugin/WavDecoder.h>
#include <plugin/WaveformView.h>

#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void pushU16 (Bytes& b, std::uint16_t v)
{
    b.push_back (static_cast<std::uint8_t> (v & 0xFF));
    b.push_back (static_cast<std::uint8_t> ((v >> 8) & 0xFF));
}

void pushU32 (Bytes& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back (static_cast<std::uint8_t> ((v >> (8 * i)) & 0xFF));
}

void pushTag (Bytes& b, const char (&tag)[5])
{
    for (int i = 0; i < 4; ++i)
        b.push_back (static_cast<std::uint8_t> (tag[i]));
}

// Builds a little-endian WAV from planar float channels. `format`:
// 1 = PCM int, 3 = IEEE float. For PCM, values are quantized to `bits`.
Bytes makeWav (const std::vector<const float*>& channelData, int numChannels,
               std::int64_t frames, std::uint32_t sampleRate, std::uint16_t bits,
               std::uint16_t format = 1, bool extensible = false)
{
    const auto bytesPerSample = static_cast<std::uint32_t> (bits / 8);
    const auto blockAlign = static_cast<std::uint32_t> (numChannels) * bytesPerSample;
    const auto dataSize = static_cast<std::uint32_t> (frames) * blockAlign;

    Bytes body;
    // fmt chunk
    pushTag (body, "fmt ");
    const auto fmtSize = extensible ? 40u : 16u;
    pushU32 (body, fmtSize);
    pushU16 (body, extensible ? static_cast<std::uint16_t> (0xFFFE) : format);
    pushU16 (body, static_cast<std::uint16_t> (numChannels));
    pushU32 (body, sampleRate);
    pushU32 (body, sampleRate * blockAlign);
    pushU16 (body, static_cast<std::uint16_t> (blockAlign));
    pushU16 (body, bits);
    if (extensible)
    {
        pushU16 (body, 22);      // cbSize
        pushU16 (body, bits);    // wValidBitsPerSample
        pushU32 (body, 0);       // dwChannelMask
        pushU16 (body, format);  // SubFormat GUID starts with the format tag
        for (int i = 0; i < 14; ++i)
            body.push_back (0);
    }
    // data chunk
    pushTag (body, "data");
    pushU32 (body, dataSize);
    for (std::int64_t f = 0; f < frames; ++f)
        for (int c = 0; c < numChannels; ++c)
        {
            const float v = channelData[static_cast<std::size_t> (c)][static_cast<std::size_t> (f)];
            if (format == 3 && bits == 32)
            {
                std::uint32_t raw = 0;
                std::memcpy (&raw, &v, 4);
                pushU32 (body, raw);
            }
            else
            {
                // Scale by 2^(bits-1) so decode (divide by the same)
                // reconstructs the source within half an LSB.
                const auto fullScale = std::ldexp (1.0, bits - 1);
                const auto qMax = static_cast<std::int32_t> (fullScale) - 1;
                const auto q = std::clamp (static_cast<std::int32_t> (std::lround (v * fullScale)),
                                           -qMax - 1, qMax);
                for (unsigned i = 0; i < bytesPerSample; ++i)
                    body.push_back (static_cast<std::uint8_t> (
                        (static_cast<std::uint32_t> (q) >> (8 * i)) & 0xFF));
            }
        }

    Bytes out;
    pushTag (out, "RIFF");
    pushU32 (out, static_cast<std::uint32_t> (body.size()));
    pushTag (out, "WAVE");
    out.insert (out.end(), body.begin(), body.end());
    return out;
}

[[nodiscard]] std::vector<float> ramp (std::size_t n)
{
    std::vector<float> v (n);
    std::iota (v.begin(), v.end(), 0.0f);
    for (auto& x : v)
        x /= static_cast<float> (n);
    return v;
}

struct TempFile
{
    std::string path;
    explicit TempFile (const char* name, const Bytes& bytes)
    {
        // OS temp dir (NOT a hard-coded local scratch path -- CI runners
        // don't have /tmp/opencode). The directory exists by definition,
        // but create_directories costs nothing if it does.
        std::filesystem::path dir = std::filesystem::temp_directory_path();
        std::filesystem::create_directories (dir);
        path = (dir / name).string();
        std::ofstream f (path, std::ios::binary);
        f.write (reinterpret_cast<const char*> (bytes.data()),
                 static_cast<std::streamsize> (bytes.size()));
    }
};

// A click track: sharp bursts every `spacing` frames, enough for the
// detector's holdoff but far above its sensitivity threshold.
struct ClickTrack
{
    static constexpr std::int64_t kFrames = 48000 * 2;
    static constexpr double kSampleRate = 48000.0;
    static constexpr std::int64_t kSpacing = 12000;   // 4 per second

    [[nodiscard]] static std::vector<float> render()
    {
        std::vector<float> v (static_cast<std::size_t> (kFrames), 0.0f);
        for (std::int64_t base = 0; base + 400 < kFrames; base += kSpacing)
            for (int n = 0; n < 400; ++n)
                v[static_cast<std::size_t> (base + n)]
                    = 0.9f * std::exp (-static_cast<float> (n) / 60.0f)
                    * std::sin (static_cast<float> (n) * 0.5f);
        return v;
    }
};

} // namespace

TEST_CASE ("wav: decodes pcm16 mono")
{
    const auto src = ramp (100);
    const Bytes wav = makeWav ({ src.data() }, 1, 100, 44100, 16);

    const auto decoded = nedit::plugin::decodeWav (wav.data(), wav.size());
    REQUIRE (decoded.has_value());
    CHECK (decoded->frames == 100);
    CHECK (decoded->channelCount() == 1);
    CHECK (decoded->sampleRate == Catch::Approx (44100.0));

    for (std::size_t i = 0; i < 100; ++i)
        CHECK (decoded->channels[0][i]
               == Catch::Approx (src[i]).margin (1.0 / 32768.0));
}

TEST_CASE ("wav: stereo deinterleave, float32, pcm24, extensible wrapper")
{
    const auto left = ramp (64);
    std::vector<float> right (64, -0.5f);

    SECTION ("stereo pcm16 order")
    {
        const Bytes wav = makeWav ({ left.data(), right.data() }, 2, 64, 22050, 16);
        const auto d = nedit::plugin::decodeWav (wav.data(), wav.size());
        REQUIRE (d.has_value());
        CHECK (d->channelCount() == 2);
        CHECK (d->channels[1][10] == Catch::Approx (-0.5f).margin (1.0 / 32768.0));
    }

    SECTION ("float32 passthrough")
    {
        const Bytes wav = makeWav ({ left.data() }, 1, 64, 48000, 32, 3);
        const auto d = nedit::plugin::decodeWav (wav.data(), wav.size());
        REQUIRE (d.has_value());
        for (std::size_t i = 0; i < 64; ++i)
            CHECK (d->channels[0][i] == left[i]);
    }

    SECTION ("pcm24 quantization")
    {
        const Bytes wav = makeWav ({ left.data() }, 1, 64, 48000, 24);
        const auto d = nedit::plugin::decodeWav (wav.data(), wav.size());
        REQUIRE (d.has_value());
        CHECK (d->channels[0][32] == Catch::Approx (left[32]).margin (1.0 / 8388608.0));
    }

    SECTION ("extensible float")
    {
        const Bytes wav = makeWav ({ left.data() }, 1, 64, 48000, 32, 3, true);
        const auto d = nedit::plugin::decodeWav (wav.data(), wav.size());
        REQUIRE (d.has_value());
        CHECK (d->channels[0][63] == left[63]);
    }
}

TEST_CASE ("wav: rejects garbage, survives truncation at every byte")
{
    // Deterministic garbage.
    Bytes garbage (512);
    std::uint32_t seed = 0x12345678;
    for (auto& b : garbage)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        b = static_cast<std::uint8_t> (seed & 0xFF);
    }
    // May or may not decode; must never crash. (A 1-in-2^32 accident of a
    // valid header would still be handled correctly.)
    (void) nedit::plugin::decodeWav (garbage.data(), garbage.size());

    const auto src = ramp (50);
    const Bytes wav = makeWav ({ src.data() }, 1, 50, 44100, 16);

    std::size_t decodedCount = 0;
    for (std::size_t cut = 0; cut <= wav.size(); ++cut)
    {
        const auto d = nedit::plugin::decodeWav (wav.data(), cut);
        if (d.has_value())
        {
            ++decodedCount;
            CHECK (d->frames >= 0);
            CHECK (d->frames <= 50);
            CHECK (! d->channels.empty());
        }
    }
    CHECK (decodedCount > 0);   // full-length prefix must decode
    CHECK (nedit::plugin::decodeWav (wav.data(), wav.size()).has_value());

    CHECK_FALSE (nedit::plugin::decodeWav (nullptr, 100).has_value());
}

TEST_CASE ("sample manager: load file -> analysis -> state metadata")
{
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::SampleManager manager;
    const auto result = manager.loadFile (file.path, {});
    REQUIRE (result.has_value());

    CHECK (result->updated.samplePath == file.path);
    CHECK (result->updated.sampleLengthFrames == ClickTrack::kFrames);
    CHECK (result->updated.sampleSampleRate == Catch::Approx (ClickTrack::kSampleRate));
    CHECK (result->updated.trimStartFrame == 0);
    CHECK (result->updated.trimEndFrame == ClickTrack::kFrames);
    CHECK (result->updated.hasSample());

    // Clean clicks at default sensitivity: several slices, boundaries
    // near the click positions.
    REQUIRE (! result->sample->slices.empty());
    CHECK (result->sample->slices.size() >= 5);

    // The slot is published immediately for the audio thread.
    CHECK (manager.acquire() != nullptr);
    CHECK (manager.acquire()->slices.size() == result->sample->slices.size());

    // Garbage file rejected, slot untouched.
    const TempFile junk ("nedit_test_junk.wav", Bytes (256, 0xAB));
    const auto failed = manager.loadFile (junk.path, {});
    CHECK_FALSE (failed.has_value());
    CHECK (manager.acquire()->slices.size() == result->sample->slices.size());
}

TEST_CASE ("shell: loaded click track renders audible picks through process()")
{
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.setActive (1) == Steinberg::kResultOk);
    REQUIRE (processor.setProcessing (1) == Steinberg::kResultOk);

    REQUIRE (processor.requestSampleLoad (file.path));
    CHECK (processor.debugUiState().sample.sampleLengthFrames == ClickTrack::kFrames);

    // Slice Length mode is the default trigger mode; drive the transport
    // across ~30 blocks like a playing host.
    std::vector<float> out (512, 0.0f);
    float* channels[1] { out.data() };

    Steinberg::Vst::AudioBusBuffers bus {};
    bus.numChannels = 1;
    bus.channelBuffers32 = channels;

    Steinberg::Vst::ProcessData data {};
    data.numSamples = 512;
    data.numOutputs = 1;
    data.outputs = &bus;

    const double bpm = 120.0;
    double ppq = 0.0;

    int picks = 0;
    float peak = 0.0f;

    for (int block = 0; block < 30; ++block)
    {
        Steinberg::Vst::ProcessContext ctx {};
        ctx.state = Steinberg::Vst::ProcessContext::kPlaying
                  | Steinberg::Vst::ProcessContext::kTempoValid
                  | Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
        ctx.tempo = bpm;
        ctx.projectTimeMusic = ppq;
        data.processContext = &ctx;

        std::fill (out.begin(), out.end(), 0.0f);
        REQUIRE (processor.process (data) == Steinberg::kResultOk);

        for (const float s : out)
            peak = std::max (peak, std::abs (s));

        ppq += static_cast<double> (512) * (bpm / 60.0) / 44100.0;
    }

    picks = static_cast<int> (processor.debugScheduler().picksStarted());
    CHECK (picks > 0);
    CHECK (peak > 0.01f);
}

// --- host automation plumbing ---------------------------------------------
// Mirrors the TestEventList pattern: minimal FUnknown doubles so we can
// drive process() exactly like a host does when a user twiddles knobs.

class TestParamValueQueue : public Steinberg::Vst::IParamValueQueue
{
public:
    explicit TestParamValueQueue (Steinberg::Vst::ParamID id) : id_ { id }
    {
        FUNKNOWN_CTOR
    }
    ~TestParamValueQueue() { FUNKNOWN_DTOR }

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    Steinberg::int32 PLUGIN_API getPointCount() override
    {
        return static_cast<Steinberg::int32> (points.size());
    }
    Steinberg::tresult PLUGIN_API getPoint (Steinberg::int32 index,
                                            Steinberg::int32& offsetSamples,
                                            Steinberg::Vst::ParamValue& value) override
    {
        if (index < 0 || index >= static_cast<Steinberg::int32> (points.size()))
            return Steinberg::kResultFalse;
        offsetSamples = points[static_cast<std::size_t> (index)].first;
        value = points[static_cast<std::size_t> (index)].second;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addPoint (Steinberg::int32 offsetSamples,
                                            Steinberg::Vst::ParamValue value,
                                            Steinberg::int32& index) override
    {
        points.emplace_back (offsetSamples, value);
        index = static_cast<Steinberg::int32> (points.size()) - 1;
        return Steinberg::kResultOk;
    }

    DECLARE_FUNKNOWN_METHODS

private:
    Steinberg::Vst::ParamID id_;
    std::vector<std::pair<Steinberg::int32, Steinberg::Vst::ParamValue>> points;
};

class TestParamChanges : public Steinberg::Vst::IParameterChanges
{
public:
    TestParamChanges() { FUNKNOWN_CTOR }
    ~TestParamChanges() { FUNKNOWN_DTOR }

    Steinberg::int32 PLUGIN_API getParameterCount() override
    {
        return static_cast<Steinberg::int32> (queues.size());
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData (
        Steinberg::int32 index) override
    {
        if (index < 0 || index >= static_cast<Steinberg::int32> (queues.size()))
            return nullptr;
        return queues[static_cast<std::size_t> (index)].get();
    }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData (
        const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override
    {
        queues.push_back (Steinberg::owned (new TestParamValueQueue (id)));
        index = static_cast<Steinberg::int32> (queues.size()) - 1;
        return queues.back().get();
    }

    DECLARE_FUNKNOWN_METHODS

private:
    std::vector<Steinberg::IPtr<TestParamValueQueue>> queues;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
IMPLEMENT_FUNKNOWN_METHODS (TestParamValueQueue, Steinberg::Vst::IParamValueQueue,
                            Steinberg::Vst::IParamValueQueue::iid)
IMPLEMENT_FUNKNOWN_METHODS (TestParamChanges, Steinberg::Vst::IParameterChanges,
                            Steinberg::Vst::IParameterChanges::iid)
#pragma GCC diagnostic pop

TEST_CASE ("shell: host automation reaches the renderer")
{
    // NOTE on scope (faithful to the original, see its PluginProcessor.cpp
    // volume-gain comment): most style params only affect their own styles,
    // and static Volume is a SEQUENCED-mode-only ramp -- Slice Length/Clock
    // have no global volume dial. With default weights (Forward x1.0) the
    // only immediately audible automatable surface is the manual-tempo
    // override, which feeds Tempo::repitchRatio. So prove the automation
    // path with THAT: enabling it at 240bpm while the host plays 120 must
    // double playback rate => double the zero-crossing rate.
    constexpr Steinberg::Vst::ParamID kManualTempoEnabledId = 101;
    constexpr Steinberg::Vst::ParamID kManualTempoBpmId = 102;

    auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click_auto.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.setActive (1) == Steinberg::kResultOk);
    REQUIRE (processor.setProcessing (1) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    std::vector<float> outL (512, 0.0f);
    float* channels[1] { outL.data() };
    Steinberg::Vst::AudioBusBuffers bus {};
    bus.numChannels = 1;
    bus.channelBuffers32 = channels;
    Steinberg::Vst::ProcessData data {};
    data.numSamples = 512;
    data.numOutputs = 1;
    data.outputs = &bus;

    const double bpm = 120.0;
    double ppq = 0.0;

    auto runBlocks = [&] (int blocks, Steinberg::Vst::IParameterChanges* changes,
                          double& zcrOut) {
        std::int64_t crossings = 0;
        float prev = 0.0f;
        float peak = 0.0f;
        std::int64_t count = 0;
        for (int b = 0; b < blocks; ++b)
        {
            Steinberg::Vst::ProcessContext ctx {};
            ctx.state = Steinberg::Vst::ProcessContext::kPlaying
                      | Steinberg::Vst::ProcessContext::kTempoValid
                      | Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
            ctx.tempo = bpm;
            ctx.projectTimeMusic = ppq;
            data.processContext = &ctx;
            data.inputParameterChanges = changes;
            std::fill (outL.begin(), outL.end(), 0.0f);
            REQUIRE (processor.process (data) == Steinberg::kResultOk);
            for (const float s : outL)
            {
                peak = std::max (peak, std::abs (s));
                if (s > 0.001f || s < -0.001f)
                {
                    if ((prev < 0.0f && s > 0.0f) || (prev > 0.0f && s < 0.0f))
                        ++crossings;
                    prev = s;
                    ++count;
                }
            }
            ppq += static_cast<double> (512) * (bpm / 60.0) / 44100.0;
            if (std::getenv ("NEDIT_DBG2"))
                std::printf ("[blk %02d] pk=%.4f\n", b, *std::max_element (outL.begin(), outL.end(), [](float a, float b2){ return std::abs(a)<std::abs(b2); }));
        }
        data.inputParameterChanges = nullptr;
        zcrOut = count > 0 ? static_cast<double> (crossings) / static_cast<double> (count)
                           : 0.0;
        if (std::getenv ("NEDIT_DBG"))
            std::printf ("[dbg] blocks=%d peak=%.4f activeCount=%lld crossings=%lld zcr=%.4f\n",
                         blocks, peak, (long long) count, (long long) crossings, zcrOut);
        return peak;
    };

    double baseZcr = 0.0;
    double scratchZcr = 0.0;
    const float baselinePeak = runBlocks (30, nullptr, baseZcr);
    CHECK (baselinePeak > 0.005f);

    // Enable the manual-tempo override at 240 BPM through a REAL host queue.
    auto bpmNormFor = [&] (double bpmValue) {
        // Manual tempo range is 30..300 BPM, linear (see ParameterSurface).
        return (bpmValue - 30.0) / (300.0 - 30.0);
    };

    TestParamChanges changes;
    Steinberg::int32 queueIndex = -1;
    auto* enabledQueue = changes.addParameterData (kManualTempoEnabledId, queueIndex);
    REQUIRE (enabledQueue != nullptr);
    Steinberg::int32 pointIndex = -1;
    REQUIRE (enabledQueue->addPoint (0, 1.0, pointIndex) == Steinberg::kResultOk);

    auto* bpmQueue = changes.addParameterData (kManualTempoBpmId, queueIndex);
    REQUIRE (bpmQueue != nullptr);
    REQUIRE (bpmQueue->addPoint (0, bpmNormFor (240.0), pointIndex)
             == Steinberg::kResultOk);

    // NOTE: a longer manual-tempo value means the trim is INTERPRETED as a
    // longer loop => playback runs SLOWER (repitchRatio shrinks). Measured
    // as sign-crossings per active sample inside the rendered blips.
    runBlocks (15, &changes, scratchZcr);   // settle: rotate picks

    double fastZcr = 0.0;
    const float fastPeak = runBlocks (20, &changes, fastZcr);
    CHECK (fastPeak > 0.005f);                       // still audible
    CHECK (fastZcr > baseZcr * 0.35);                // ~half playback rate
    CHECK (fastZcr < baseZcr * 0.65);

    // Turning the override back OFF returns to native rate.
    REQUIRE (enabledQueue->addPoint (0, 0.0, pointIndex) == Steinberg::kResultOk);
    runBlocks (15, &changes, scratchZcr);
    double backZcr = 0.0;
    runBlocks (20, &changes, backZcr);
    CHECK (backZcr > fastZcr * 1.5);
    CHECK (backZcr < baseZcr * 1.25);
}

// ── Manual markers ─────────────────────────────────────────────────────

namespace {

// Load the standard click track into a fresh processor. Returns the temp
// file (keep it alive for the processor's lifetime) via out-param.
[[nodiscard]] Bytes clickWav()
{
    const auto click = ClickTrack::render();
    return makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                    static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
}

// The detector's derivative peaks a couple frames INTO each burst, so the
// derived onsets sit ~2 frames past the 12000-multiple burst starts (e.g.
// 24002, not 24000). Locate a boundary the robust way: the slice whose
// start is closest to `about`, within half the spacing.
const nedit::engine::Slice* boundaryNear (const std::vector<nedit::engine::Slice>& slices,
                                          std::int64_t about)
{
    const nedit::engine::Slice* best = nullptr;
    std::int64_t bestDist = 6000;
    for (const auto& s : slices)
    {
        if (s.startFrame == 0) continue;   // never the trim-start boundary
        const std::int64_t d = std::llabs (s.startFrame - about);
        if (d < bestDist) { bestDist = d; best = &s; }
    }
    return best;
}

} // namespace

TEST_CASE ("manual marker: add (no snap) inserts a boundary, remove restores it")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    const int before = processor.debugSliceCount();
    REQUIRE (before >= 5);

    // Drop a marker in the middle of an interior slice with snapping OFF, so
    // it lands exactly where we ask and creates a fresh boundary.
    auto loaded = processor.acquireLoadedSample();
    REQUIRE (loaded != nullptr);
    const auto midSlice = loaded->slices[2];
    const std::int64_t mid = (midSlice.startFrame + midSlice.endFrame) / 2;

    const std::int32_t id = processor.addManualPoint (mid, /*snap*/ false);
    CHECK (id >= 0);
    CHECK (processor.debugSliceCount() == before + 1);

    const auto& mps = processor.debugUiState().sample.manualPoints;
    REQUIRE (mps.size() == 1);
    CHECK (mps[0].position == mid);

    bool boundaryAtMid = false;
    for (const auto& s : processor.acquireLoadedSample()->slices)
        if (s.startFrame == mid) { boundaryAtMid = true; break; }
    CHECK (boundaryAtMid);

    // Removing it restores the original slice list.
    CHECK (processor.removeManualPoint (id));
    CHECK (processor.debugSliceCount() == before);
    CHECK (processor.debugUiState().sample.manualPoints.empty());

    // Removing a non-existent id is a no-op.
    CHECK_FALSE (processor.removeManualPoint (9999));
}

TEST_CASE ("manual marker: snap pulls the point to the nearest transient")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    // A click sits at 24000; aim 1000 frames past it (well within the
    // 50ms / 2400-frame snap radius at 48 kHz).
    const std::int64_t clickFrame = 24000;
    const std::int64_t target = clickFrame + 1000;

    auto positionOf = [&] (std::int32_t id) -> std::int64_t {
        for (const auto& mp : processor.debugUiState().sample.manualPoints)
            if (mp.id == id) return mp.position;
        return -1;
    };

    // No-snap: the point stays exactly at the target.
    const std::int32_t idNoSnap = processor.addManualPoint (target, /*snap*/ false);
    REQUIRE (idNoSnap >= 0);
    CHECK (positionOf (idNoSnap) == target);

    // Snap: the point is pulled toward the click's onset, and lands closer
    // to the transient than the raw target was.
    const std::int32_t idSnap = processor.addManualPoint (target, /*snap*/ true);
    REQUIRE (idSnap >= 0);
    const std::int64_t posSnap = positionOf (idSnap);
    CHECK (posSnap != target);
    CHECK (std::llabs (posSnap - clickFrame) < std::llabs (target - clickFrame));
    CHECK (std::llabs (posSnap - clickFrame) < 600);
}

TEST_CASE ("manual marker: painted probabilities survive a split (child inherits parent)")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    auto loaded = processor.acquireLoadedSample();
    REQUIRE (loaded != nullptr);
    REQUIRE (loaded->slices.size() >= 4);

    // Paint one interior slice and its neighbour to distinct probabilities.
    const int kIdx = 2;
    const auto parent = loaded->slices[static_cast<std::size_t> (kIdx)];
    const std::int64_t neighbourStart =
        loaded->slices[static_cast<std::size_t> (kIdx + 1)].startFrame;

    processor.setSliceProbability (kIdx, 0.3f);
    processor.setSliceProbability (kIdx + 1, 0.8f);

    // Split the painted slice in half (no snap => exact boundary at mid).
    const std::int64_t mid = (parent.startFrame + parent.endFrame) / 2;
    REQUIRE (processor.addManualPoint (mid, false) >= 0);

    auto after = processor.acquireLoadedSample();
    REQUIRE (after != nullptr);

    auto probAtStart = [&] (std::int64_t start) -> float {
        for (std::size_t i = 0; i < after->slices.size(); ++i)
            if (after->slices[i].startFrame == start)
                return processor.getSliceProbability (static_cast<int> (i));
        return -1.0f;
    };

    CHECK (probAtStart (parent.startFrame) == Catch::Approx (0.3f)); // left half
    CHECK (probAtStart (mid)               == Catch::Approx (0.3f)); // right half inherits
    CHECK (probAtStart (neighbourStart)    == Catch::Approx (0.8f)); // neighbour intact
}

TEST_CASE ("manual marker: drag moves the boundary, snaps on release, preserves weights")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    auto loaded = processor.acquireLoadedSample();
    REQUIRE (loaded != nullptr);
    REQUIRE (loaded->slices.size() >= 4);
    const auto host = loaded->slices[2];
    const std::int64_t startPos = (host.startFrame + host.endFrame) / 2;

    const std::int32_t id = processor.addManualPoint (startPos, /*snap*/ false);
    REQUIRE (id >= 0);

    auto posOf = [&] () -> std::int64_t {
        for (const auto& mp : processor.debugUiState().sample.manualPoints)
            if (mp.id == id) return mp.position;
        return -1;
    };
    auto idxOf = [&] (std::int64_t start) -> int {
        auto s = processor.acquireLoadedSample();
        for (std::size_t i = 0; i < s->slices.size(); ++i)
            if (s->slices[i].startFrame == start) return static_cast<int> (i);
        return -1;
    };
    auto probAtStart = [&] (std::int64_t start) -> float {
        const int idx = idxOf (start);
        return idx >= 0 ? processor.getSliceProbability (idx) : -1.0f;
    };

    CHECK (posOf() == startPos);

    // Paint the two halves the marker created (left starts at host.start,
    // right starts at the marker).
    processor.setSliceProbability (idxOf (host.startFrame), 0.25f);
    processor.setSliceProbability (idxOf (startPos), 0.75f);

    // Free move (no snap) lands exactly, and the boundary + weights follow.
    const std::int64_t movedPos = startPos + 800;
    processor.moveManualPoint (id, movedPos, /*snap*/ false);
    CHECK (posOf() == movedPos);

    bool boundaryAtMoved = false;
    for (const auto& s : processor.acquireLoadedSample()->slices)
        if (s.startFrame == movedPos) { boundaryAtMoved = true; break; }
    CHECK (boundaryAtMoved);

    CHECK (probAtStart (host.startFrame) == Catch::Approx (0.25f));
    CHECK (probAtStart (movedPos)        == Catch::Approx (0.75f));

    // Release-snap near the click at 24000 pulls the marker onto the onset.
    processor.moveManualPoint (id, 24000 + 900, /*snap*/ true);
    CHECK (std::llabs (posOf() - 24000) < 600);

    // Moving a non-existent id is a harmless no-op.
    const std::int64_t keep = posOf();
    processor.moveManualPoint (9999, 100, /*snap*/ false);
    CHECK (posOf() == keep);
}

// ── Auto-onset exclusion ───────────────────────────────────────────────

TEST_CASE ("auto onset: exclusion removes the boundary, targets the raw onset, keeps the left slice's weight")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    auto loaded = processor.acquireLoadedSample();
    REQUIRE (loaded != nullptr);
    REQUIRE (loaded->slices.size() >= 8);

    // The auto boundary near 24000 (sits ~2 frames past the burst start).
    const auto* target = boundaryNear (loaded->slices, 24000);
    REQUIRE (target != nullptr);
    const std::int64_t targetStart = target->startFrame;
    const std::size_t beforeCount = loaded->slices.size();

    // The slice immediately LEFT of the boundary keeps its weight across the
    // exclusion (its end == the boundary that gets removed).
    const nedit::engine::Slice* leftSlice = nullptr;
    for (const auto& s : loaded->slices)
        if (s.endFrame == targetStart) { leftSlice = &s; break; }
    REQUIRE (leftSlice != nullptr);
    const std::int64_t leftStart = leftSlice->startFrame;
    const int leftIdx = static_cast<int> (leftSlice - loaded->slices.data());
    processor.setSliceProbability (leftIdx, 0.35f);

    // Double-click gesture: exclude the nearest raw onset to the boundary.
    const bool excluded = processor.excludeNearestAutoPoint (targetStart + 800);
    REQUIRE (excluded);

    const auto& eps = processor.debugUiState().sample.excludedPoints;
    REQUIRE (eps.size() == 1);
    CHECK (std::llabs (eps[0].position - 24000) < 600);

    auto after = processor.acquireLoadedSample();
    REQUIRE (after != nullptr);
    CHECK (after->slices.size() + 1 == beforeCount);

    // The boundary is gone; the merged slice starts where the left slice did
    // and inherits its painted weight; the trim-start stays put.
    bool boundaryGone = true;
    bool trimStartPresent = false;
    for (const auto& s : after->slices)
    {
        if (s.startFrame == targetStart) boundaryGone = false;
        if (s.startFrame == 0)          trimStartPresent = true;
    }
    CHECK (boundaryGone);
    CHECK (trimStartPresent);

    for (std::size_t i = 0; i < after->slices.size(); ++i)
        if (after->slices[i].startFrame == leftStart)
            CHECK (processor.getSliceProbability (static_cast<int> (i)) == Catch::Approx (0.35f));
}

TEST_CASE ("auto onset: re-exclusion is a no-op; the trim start can't be excluded")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    auto loaded = processor.acquireLoadedSample();
    REQUIRE (loaded != nullptr);
    REQUIRE (loaded->slices.size() >= 8);

    const auto* target = boundaryNear (loaded->slices, 24000);
    REQUIRE (target != nullptr);

    REQUIRE (processor.excludeNearestAutoPoint (target->startFrame));
    const int afterFirst = processor.debugSliceCount();

    // The same onset again: defensive dedup, no rebuild, no duplicate entry.
    CHECK_FALSE (processor.excludeNearestAutoPoint (target->startFrame));
    CHECK (processor.debugSliceCount() == afterFirst);
    CHECK (processor.debugUiState().sample.excludedPoints.size() == 1);

    // Excluding near the very start skips the trim-start boundary (never
    // excludable) and lands on the FIRST real onset (~2, the first burst's
    // derivative peak) -- never on 0.
    REQUIRE (processor.excludeNearestAutoPoint (0));
    const auto& eps = processor.debugUiState().sample.excludedPoints;
    REQUIRE (eps.size() == 2);
    CHECK (eps[1].position != 0);
    CHECK (eps[1].position > 0);

    bool trimStartPresent = false;
    for (const auto& s : processor.acquireLoadedSample()->slices)
        if (s.startFrame == 0) trimStartPresent = true;
    CHECK (trimStartPresent);
}

TEST_CASE ("waveform view: marker hit-testing pads the thin boundary lines")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    nedit::plugin::WaveformView view (processor, nullptr);
    view.setViewSize (VSTGUI::CRect (0, 0, 400, 144));
    view.refresh();

    // Full-zoom mapping: boundary B sits at x = B/96000*400 px (the visible
    // range is [0, total) on first load).
    const double kTotal = static_cast<double> (ClickTrack::kFrames);
    const auto toX = [&] (std::int64_t frame) -> VSTGUI::CCoord {
        return static_cast<double> (frame) / kTotal * 400.0;
    };

    const auto* autoBoundary = boundaryNear (processor.acquireLoadedSample()->slices, 24000);
    REQUIRE (autoBoundary != nullptr);
    const std::int64_t autoBoundaryFrame = autoBoundary->startFrame;
    const VSTGUI::CCoord autoX = toX (autoBoundaryFrame);

    // The padded radius (10px) makes the 1px line far easier to hit.
    CHECK (view.findAutoPointNear (autoX) == autoBoundaryFrame);
    CHECK (view.findAutoPointNear (autoX - 9.0) == autoBoundaryFrame);
    CHECK (view.findAutoPointNear (autoX + 10.0) == autoBoundaryFrame);
    CHECK (view.findAutoPointNear (autoX + 11.0) == -1);   // just past the pad

    // A manual marker placed EXACTLY on an existing auto boundary; same
    // padding applies, and the manual wins the coincident boundary (the auto
    // one stops being a hit target). Snap=false is deliberate here: snapped
    // adds land on the raw derivative peak (48003) while detection picks the
    // onset 1 frame earlier (48002), so only an unsnapped add can coincide.
    const auto* manualTarget = boundaryNear (processor.acquireLoadedSample()->slices, 48000);
    REQUIRE (manualTarget != nullptr);
    const std::int64_t manualFrame = manualTarget->startFrame;
    const std::int32_t midId = processor.addManualPoint (manualFrame, /*snap*/ false);
    REQUIRE (midId >= 0);
    const VSTGUI::CCoord midX = toX (manualFrame);

    CHECK (view.findManualPointNear (midX) == midId);
    CHECK (view.findManualPointNear (midX - 10.0) == midId);
    CHECK (view.findManualPointNear (midX + 11.0) == -1);
    CHECK (view.findAutoPointNear (midX) == -1);   // auto defers to manual
}
