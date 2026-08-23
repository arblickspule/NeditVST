// Nedit -- Plugin-layer tests: WAV decode + SampleManager + the first
// truly AUDIBLE end-to-end pass through the VST3 shell (decode ->
// analyze -> slices -> scheduler -> rendered output energy).

#include <plugin/NeditProcessor.h>
#include <plugin/SampleManager.h>
#include <plugin/WavDecoder.h>

#include <pluginterfaces/vst/ivstprocesscontext.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
        : path { std::string ("/tmp/opencode/") + name }
    {
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
