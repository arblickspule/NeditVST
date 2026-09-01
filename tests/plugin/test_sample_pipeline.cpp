// Nedit -- Plugin-layer tests: WAV decode + SampleManager + the first
// truly AUDIBLE end-to-end pass through the VST3 shell (decode ->
// analyze -> slices -> scheduler -> rendered output energy).

#include <plugin/NeditProcessor.h>
#include <plugin/NeditEditor.h>
#include <plugin/SampleManager.h>
#include <plugin/WavDecoder.h>
#include <plugin/WaveformView.h>

#include <engine/Slice.h>

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

TEST_CASE ("wav: rejects an internally inconsistent blockAlign (lying fmt)")
{
    // Regression: blockAlign comes straight from the file and used to be
    // trusted (only checked != 0). A fmt chunk claiming a frame narrower
    // than numChannels x bytesPerSample inflates `frames` and sends the
    // tail-frame reads past the end of the data chunk -- heap OOB read.
    const auto left = ramp (64);
    std::vector<float> right (64, 0.25f);
    Bytes wav = makeWav ({ left.data(), right.data() }, 2, 64, 44100, 16);

    // Fmt payload starts at byte 20 (RIFF header 12 + chunk id/size 8);
    // blockAlign is payload offset 12 -> file offset 32 (little-endian).
    constexpr std::size_t kBlockAlignOffset = 12 + 8 + 12;
    constexpr std::size_t kBitsOffset = 12 + 8 + 14;
    REQUIRE (wav[kBlockAlignOffset] == 4);   // sanity: stereo pcm16 = 4 bytes

    // Control: the untouched file decodes.
    REQUIRE (nedit::plugin::decodeWav (wav.data(), wav.size()).has_value());

    // Any claimed frame width below channels x bytesPerSample (4 here) must
    // be rejected -- blockAlign = 1 would read 3 bytes past the chunk on
    // the last frame, and blockAlign < numChannels additionally collapses
    // the per-channel stride to zero.
    for (const std::uint8_t bogus : { std::uint8_t { 1 }, std::uint8_t { 2 },
                                      std::uint8_t { 3 } })
    {
        wav[kBlockAlignOffset] = bogus;
        CHECK_FALSE (nedit::plugin::decodeWav (wav.data(), wav.size()).has_value());
    }

    // Wider-than-needed frames (padded containers) stay legal: frames
    // re-derive from the stride and every read stays inside its frame.
    wav[kBlockAlignOffset] = 8;
    const auto padded = nedit::plugin::decodeWav (wav.data(), wav.size());
    REQUIRE (padded.has_value());
    CHECK (padded->frames == 32);   // 64 true frames re-grouped as 8-byte frames

    // The other lying shape: sample width widened without the frame
    // following (mono pcm16 file claiming 32-bit samples => 4-byte reads
    // on 2-byte frames).
    Bytes narrow = makeWav ({ left.data() }, 1, 64, 44100, 16);
    REQUIRE (narrow[kBitsOffset] == 16);
    narrow[kBitsOffset] = 32;
    CHECK_FALSE (nedit::plugin::decodeWav (narrow.data(), narrow.size()).has_value());
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

TEST_CASE ("waveform redraw: replacing the sample via a second load is detected")
{
    // Issue #10: the waveform would not redraw after LOADING A SECOND SAMPLE
    // over the first. The editor's idle timer keyed the redraw on sample
    // PRESENCE (bool false->true), so a replacement (true->true) looked
    // unchanged. It now keys on the published LoadedSample IDENTITY, which
    // every load replaces -- even when the path is the same.

    auto wavForLength = [] (std::int64_t frames) {
        // A shorter/longer click track must give a different sample length,
        // so the replacement is unambiguous even at the state level.
        std::vector<float> click (static_cast<std::size_t> (frames), 0.0f);
        for (std::int64_t i = 0; i + 400 < frames; i += 12000)
            for (int n = 0; n < 400; ++n)
                click[static_cast<std::size_t> (i + n)]
                    = 0.9f * std::exp (-static_cast<float> (n) / 60.0f)
                    * std::sin (static_cast<float> (n) * 0.5f);
        return makeWav ({ click.data() }, 1, frames,
                        static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    };

    const TempFile first ("nedit_test_first.wav", wavForLength (ClickTrack::kFrames));
    const TempFile second ("nedit_test_second.wav", wavForLength (96000));

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (first.path));

    nedit::plugin::NeditEditor editor (&processor);

    // First idle tick: sample went from absent to present -> change detected.
    editor.notify (nullptr, VSTGUI::CVSTGUITimer::kMsgTimer);
    CHECK (editor.notifySampleChanged());

    // Steady state: a tick with no new load sees the same identity -> no change.
    editor.notify (nullptr, VSTGUI::CVSTGUITimer::kMsgTimer);
    CHECK_FALSE (editor.notifySampleChanged());

    // THE REGRESSION: a SECOND load replaces the sample. presence stays
    // true->true, but the LoadedSample object is a fresh identity, so the
    // editor must report a change (and would refresh the waveform + grid).
    REQUIRE (processor.requestSampleLoad (second.path));
    editor.notify (nullptr, VSTGUI::CVSTGUITimer::kMsgTimer);
    CHECK (editor.notifySampleChanged());

    // And the same-second-load again is quiet (dedup still works).
    editor.notify (nullptr, VSTGUI::CVSTGUITimer::kMsgTimer);
    CHECK_FALSE (editor.notifySampleChanged());
}

TEST_CASE ("shell: loading a sample establishes the sequencer grid dimensions")
{
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    const auto& seq = processor.debugUiState().sequencer;
    const int sliceCount = processor.debugSliceCount();
    // rows = min(sliceCount, kMaxSequencerRows); default 16n + 1 bar => 16 cols.
    CHECK (seq.rows == std::min (sliceCount, nedit::state::kMaxSequencerRows));
    CHECK (seq.columns == 16);
    CHECK (seq.grid.size()
           == static_cast<std::size_t> (seq.rows) * static_cast<std::size_t> (seq.columns));
}

TEST_CASE ("shell: sequencer cell writes enforce monophony and clear cleanly")
{
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    const auto cell = [&] (int row, int col) {
        const auto& s = processor.debugUiState().sequencer;
        return static_cast<int> (s.grid[static_cast<std::size_t> (row)
                                         * static_cast<std::size_t> (s.columns)
                                         + static_cast<std::size_t> (col)]);
    };

    REQUIRE (processor.debugUiState().sequencer.rows >= 2);

    // Write style 3 at (row 0, col 0).
    CHECK (processor.setSequencerCell (0, 0, 3));
    CHECK (cell (0, 0) == 3);

    // Writing a DIFFERENT row in the SAME column clears the first (monophony).
    CHECK (processor.setSequencerCell (1, 0, 5));
    CHECK (cell (1, 0) == 5);
    CHECK (cell (0, 0) == -1);

    // Re-writing the same style over itself is a no-op (returns false).
    CHECK_FALSE (processor.setSequencerCell (1, 0, 5));

    // Clearing drops the cell.
    CHECK (processor.setSequencerCell (1, 0, -1));
    CHECK (cell (1, 0) == -1);

    // Out-of-range / bad style rejected.
    CHECK_FALSE (processor.setSequencerCell (999, 0, 1));
    CHECK_FALSE (processor.setSequencerCell (0, 0, 99));
}

TEST_CASE ("shell: sequencer cell extension grows, clamps and needs a filled cell")
{
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    const auto extAt = [&] (int row, int col) -> int {
        const auto& s = processor.debugUiState().sequencer;
        const auto flat = static_cast<std::uint32_t> (row)
                        * static_cast<std::uint32_t> (s.columns)
                        + static_cast<std::uint32_t> (col);
        const auto it = s.extensions.find (flat);
        return it != s.extensions.end() ? static_cast<int> (it->second) : 0;
    };

    // Extension on an EMPTY cell is rejected.
    CHECK_FALSE (processor.setSequencerCellExtension (0, 0, 3));

    // Fill then extend.
    CHECK (processor.setSequencerCell (0, 0, 0));
    CHECK (processor.setSequencerCellExtension (0, 0, 3));
    CHECK (extAt (0, 0) == 3);

    // Negative delta clamps at 0 and erases the entry.
    CHECK (processor.setSequencerCellExtension (0, 0, -10));
    CHECK (extAt (0, 0) == 0);
}

TEST_CASE ("shell: per-cell parameter override setters clamp and key by cell")
{
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    // Fill a cell (style 4 = filterDown) at (row 0, col 0).
    REQUIRE (processor.setSequencerCell (0, 0, 4));

    const auto& seq = processor.debugUiState().sequencer;
    const auto ovAt = [&] (int row, int col, nedit::state::StyleParamId id) -> bool {
        const auto flat = static_cast<std::uint32_t> (row)
                        * static_cast<std::uint32_t> (seq.columns)
                        + static_cast<std::uint32_t> (col);
        const auto cit = seq.overrides.find (flat);
        if (cit == seq.overrides.end())
            return false;
        return cit->second.count (id) > 0;
    };

    using nedit::state::StyleParamId;

    // Continuous param clamps into range and is keyed by StyleParamId.
    CHECK (processor.setSequencerCellOverride (0, 0, StyleParamId::filterResonance, 99.0f));
    REQUIRE (ovAt (0, 0, StyleParamId::filterResonance));
    CHECK (seq.overrides.at (0).at (StyleParamId::filterResonance) == Catch::Approx (10.0f));

    // Discrete param rounds + clamps to a valid option index.
    CHECK (processor.setSequencerCellOverride (0, 0, StyleParamId::filterType, 42.0f));
    REQUIRE (ovAt (0, 0, StyleParamId::filterType));
    CHECK (seq.overrides.at (0).at (StyleParamId::filterType) == Catch::Approx (2.0f));

    // Idempotent re-write returns false.
    CHECK_FALSE (processor.setSequencerCellOverride (0, 0, StyleParamId::filterResonance, 10.0f));

    // Empty cell / invalid param id rejected.
    CHECK_FALSE (processor.setSequencerCellOverride (1, 0, StyleParamId::filterResonance, 1.0f));
    CHECK_FALSE (processor.setSequencerCellOverride (
        0, 0, static_cast<StyleParamId> (200), 0.5f));

    // Clearing one override; clearing the last prunes the cell entry.
    CHECK (processor.clearSequencerCellOverride (0, 0, StyleParamId::filterType));
    CHECK_FALSE (ovAt (0, 0, StyleParamId::filterType));
    CHECK (processor.clearSequencerCellOverride (0, 0, StyleParamId::filterResonance));
    CHECK (seq.overrides.count (0) == 0);
}

TEST_CASE ("shell: selected drawing style persists and range-checks")
{
    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);

    processor.setSelectedDrawingStyle (7);
    CHECK (processor.debugUiState().sequencer.selectedDrawingStyle == 7);

    processor.setSelectedDrawingStyle (99);   // rejected, unchanged
    CHECK (processor.debugUiState().sequencer.selectedDrawingStyle == 7);

    processor.setSelectedDrawingStyle (-1);    // rejected, unchanged
    CHECK (processor.debugUiState().sequencer.selectedDrawingStyle == 7);
}

TEST_CASE ("shell: sequencer transport setters persist, clamp and resize")
{
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    const auto colsOf = [&] { return processor.debugUiState().sequencer.columns; };
    const auto rowsOf = [&] { return processor.debugUiState().sequencer.rows; };
    const int defaultCols = colsOf();
    const int defaultRows = rowsOf();

    using nedit::state::PatternSwitchTiming;

    // Pattern length: sets the index + resizes the grid (dimension change;
    // more bars => more columns; rows depend only on slice count).
    CHECK (processor.debugUiState().sequencer.patternLengthBarsIndex == 0);
    CHECK (colsOf() == defaultCols);
    REQUIRE (processor.setSequencerPatternLength (2));     // 4 bars
    CHECK (processor.debugUiState().sequencer.patternLengthBarsIndex == 2);
    CHECK (colsOf() > defaultCols);
    CHECK (rowsOf() == defaultRows);
    CHECK (processor.setSequencerPatternLength (0));       // back to 1 bar
    CHECK (colsOf() == defaultCols);
    CHECK_FALSE (processor.setSequencerPatternLength (99)); // out of range
    CHECK_FALSE (processor.setSequencerPatternLength (-1));

    // Grid interval: sets the note-value index + resizes the grid.
    const int pre = colsOf();
    REQUIRE (processor.setSequencerStepResolution (9));
    CHECK (processor.debugUiState().sequencer.stepResolutionIndex == 9);
    CHECK (colsOf() != pre);
    CHECK_FALSE (processor.setSequencerStepResolution (200));
    CHECK_FALSE (processor.setSequencerStepResolution (-1));

    // Switch timing: ordinal into PatternSwitchTiming (0..2).
    CHECK (processor.debugUiState().sequencer.patternSwitchTiming
           == PatternSwitchTiming::immediate);   // default
    REQUIRE (processor.setSequencerSwitchTiming (1));      // set interval
    CHECK (processor.debugUiState().sequencer.patternSwitchTiming
           == PatternSwitchTiming::setInterval);
    CHECK (processor.setSequencerSwitchTiming (2));        // end of pattern
    CHECK (processor.debugUiState().sequencer.patternSwitchTiming
           == PatternSwitchTiming::endOfPattern);
    CHECK (processor.setSequencerSwitchTiming (0));        // back to immediate
    CHECK (processor.debugUiState().sequencer.patternSwitchTiming
           == PatternSwitchTiming::immediate);
    CHECK_FALSE (processor.setSequencerSwitchTiming (7));  // rejected
    CHECK_FALSE (processor.setSequencerSwitchTiming (-1));

    // Switch interval: note-value index.
    REQUIRE (processor.setSequencerSwitchInterval (5));
    CHECK (processor.debugUiState().sequencer.patternSwitchIntervalIndex == 5);
    CHECK_FALSE (processor.setSequencerSwitchInterval (5));   // idempotent
    CHECK_FALSE (processor.setSequencerSwitchInterval (200));
}

TEST_CASE ("editor: clear and randomize buttons act on the sequencer grid")
{
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    // Seed a small deterministic pattern (two cells, different styles).
    REQUIRE (processor.setSequencerCell (0, 0, 1));
    REQUIRE (processor.setSequencerCell (1, 2, 5));

    nedit::plugin::NeditEditor editor (&processor);

    // Stand-in for the real CTextButtons: a real one needs the platform
    // (font/gradient), but valueChanged is the entry point the buttons'
    // kKickStyle click sequence drives.
    struct FakeControl : VSTGUI::CControl
    {
        FakeControl (const VSTGUI::CRect& r, VSTGUI::IControlListener* l, int32_t t)
            : CControl (r, l, t) {}
        void draw (VSTGUI::CDrawContext*) override {}
        VSTGUI::CBaseObject* newCopy () const override { return new FakeControl (*this); }
        VSTGUI::CMouseEventResult onMouseDown (VSTGUI::CPoint&, const VSTGUI::CButtonState&) override
        {
            return VSTGUI::kMouseEventHandled;
        }
    };

    FakeControl clearBtn (VSTGUI::CRect (0, 0, 4, 4), &editor,
                          nedit::plugin::NeditEditor::kTagSeqClear);
    FakeControl randBtn (VSTGUI::CRect (0, 0, 4, 4), &editor,
                         nedit::plugin::NeditEditor::kTagSeqRandomize);
    const auto doClick = [] (FakeControl& btn, nedit::plugin::NeditEditor& ed) {
        btn.setValue (1.0f);
        ed.valueChanged (&btn);
        btn.setValue (0.0f);
        ed.valueChanged (&btn);
    };

    const auto monophonic = [&] {
        const auto& seq = processor.debugUiState().sequencer;
        for (int c = 0; c < seq.columns; ++c)
        {
            int filled = 0;
            for (int r = 0; r < seq.rows; ++r)
                if (seq.grid.at (static_cast<std::size_t> (r) * static_cast<std::size_t> (seq.columns)
                                 + static_cast<std::size_t> (c)) != -1)
                    ++filled;
            if (filled > 1)
                return false;
        }
        return true;
    };

    // Clear empties what we painted (deterministic).
    doClick (clearBtn, editor);
    CHECK (processor.debugUiState().sequencer.grid
           == std::vector<std::int8_t> (
               static_cast<std::size_t> (processor.debugUiState().sequencer.rows)
                   * static_cast<std::size_t> (processor.debugUiState().sequencer.columns),
               -1));

    // Randomize runs the real handler: the grid stays within dims and
    // monophonic (placement is RNG-seeded per call, so contents aren't
    // asserted here -- the seeded engine tests own that). Then Clear
    // leaves it empty again.
    //
    // NeditVST#4: Randomize must reach for the styles the probability band
    // shows (the band is the ONLY style-probability UI). At press time the
    // handler snapshots generate.styleWeights into the sequencer's decoupled
    // randomizeStyleWeights, so non-forward picks are possible.
    // Seed the band: only Flanger weighted (Forward's default 1.0 lowered
    // to 0 so Randomize would otherwise only ever reach Forward). This also
    // proves setStyleWeight was the UI path -- the readonly debug view stays
    // const, so all state edits go through the public setters.
    for (int i = 0; i < nedit::state::kNumPlaybackStyles; ++i)
        processor.setStyleWeight (i, 0.0f);
    processor.setStyleWeight (static_cast<int> (nedit::state::PlaybackStyle::flanger), 1.0f);

    doClick (randBtn, editor);
    REQUIRE (processor.debugUiState().sequencer.grid.size()
             == static_cast<std::size_t> (processor.debugUiState().sequencer.rows)
                    * static_cast<std::size_t> (processor.debugUiState().sequencer.columns));
    CHECK (monophonic());
    // #4: at press time the band's weights were snapshotted into the
    // sequencer's decoupled table, so Randomize can reach Flanger.
    CHECK (processor.debugUiState().sequencer.randomizeStyleWeights
           == processor.debugUiState().generate.styleWeights);
    // Stronger: the GRID itself must obey the snapshot -- every filled cell
    // is Flanger (Forward's weight is 0 and the total weight is nonzero, so
    // there is no fallback), proving the randomized pattern is no longer
    // forward-only.
    {
        const auto& grid = processor.debugUiState().sequencer.grid;
        for (const auto cell : grid)
        {
            if (cell < 0)
                continue;
            INFO ("non-forward populated cell = " << static_cast<int> (cell));
            CHECK (cell == static_cast<std::int8_t> (nedit::state::PlaybackStyle::flanger));
        }
    }
    doClick (clearBtn, editor);
    CHECK (processor.debugUiState().sequencer.grid
           == std::vector<std::int8_t> (
               static_cast<std::size_t> (processor.debugUiState().sequencer.rows)
                   * static_cast<std::size_t> (processor.debugUiState().sequencer.columns),
               -1));
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

TEST_CASE ("shell: a null output channel bails before anything renders")
{
    // Regression: with a sample loaded and the transport playing, the
    // Slice Length scheduler starts picks immediately -- and the renderer
    // used to write through whatever channel pointers the host provided
    // (only the zeroing loop null-checked). A null second channel was a
    // guaranteed write-through-null once a pick sounded.
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.setActive (1) == Steinberg::kResultOk);
    REQUIRE (processor.setProcessing (1) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    std::vector<float> left (512, -1.0f);
    float* channels[2] { left.data(), nullptr };   // hostile host
    Steinberg::Vst::AudioBusBuffers bus {};
    bus.numChannels = 2;
    bus.channelBuffers32 = channels;
    Steinberg::Vst::ProcessData data {};
    data.numSamples = 512;
    data.numOutputs = 1;
    data.outputs = &bus;

    const double bpm = 120.0;
    double ppq = 0.0;
    for (int block = 0; block < 30; ++block)
    {
        Steinberg::Vst::ProcessContext ctx {};
        ctx.state = Steinberg::Vst::ProcessContext::kPlaying
                  | Steinberg::Vst::ProcessContext::kTempoValid
                  | Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
        ctx.tempo = bpm;
        ctx.projectTimeMusic = ppq;
        data.processContext = &ctx;
        REQUIRE (processor.process (data) == Steinberg::kResultOk);
        ppq += static_cast<double> (512) * (bpm / 60.0) / 44100.0;
    }

    // Bailed before MIDI/scheduling every block: no picks, and the valid
    // channel was still cleared (never rendered into).
    CHECK (processor.debugScheduler().picksStarted() == 0);
    for (const float s : left)
        CHECK (s == 0.0f);
}

TEST_CASE ("audition: transport start defers the state fold to the UI thread")
{
    // Regression: process() used to mutate uiState_ AND publish() (a full
    // PluginState deep copy -- vectors included) from the AUDIO thread when
    // the transport started while auditioning. That copy races UI-thread
    // vector edits (heap corruption) and allocates on the audio thread.
    // The contract now: the audio thread only raises an atomic flag and the
    // engine takes over the very same block; the state fold happens on the
    // UI thread via pollAuditionAutoStop().
    const auto click = ClickTrack::render();
    const Bytes wav = makeWav ({ click.data() }, 1, ClickTrack::kFrames,
                                static_cast<std::uint32_t> (ClickTrack::kSampleRate), 16);
    const TempFile file ("nedit_test_click.wav", wav);

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.setActive (1) == Steinberg::kResultOk);
    REQUIRE (processor.setProcessing (1) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    std::vector<float> out (512, 0.0f);
    float* channels[1] { out.data() };
    Steinberg::Vst::AudioBusBuffers bus {};
    bus.numChannels = 1;
    bus.channelBuffers32 = channels;
    Steinberg::Vst::ProcessData data {};
    data.numSamples = 512;
    data.numOutputs = 1;
    data.outputs = &bus;

    processor.setAuditionEnabled (true);
    REQUIRE (processor.debugUiState().ui.auditionEnabled);

    // Stopped transport: audition renders the raw trim loop, scheduler idle.
    data.processContext = nullptr;
    REQUIRE (processor.process (data) == Steinberg::kResultOk);
    float auditionPeak = 0.0f;
    for (const float s : out)
        auditionPeak = std::max (auditionPeak, std::abs (s));
    CHECK (auditionPeak > 0.01f);   // trim starts on a click
    CHECK (processor.debugScheduler().picksStarted() == 0);

    // Playing transport: the ENGINE renders immediately -- and the state
    // flag is NOT folded by the audio thread (no mutation, no publish).
    const double bpm = 120.0;
    double ppq = 0.0;
    for (int block = 0; block < 30; ++block)
    {
        Steinberg::Vst::ProcessContext ctx {};
        ctx.state = Steinberg::Vst::ProcessContext::kPlaying
                  | Steinberg::Vst::ProcessContext::kTempoValid
                  | Steinberg::Vst::ProcessContext::kProjectTimeMusicValid;
        ctx.tempo = bpm;
        ctx.projectTimeMusic = ppq;
        data.processContext = &ctx;
        REQUIRE (processor.process (data) == Steinberg::kResultOk);
        ppq += static_cast<double> (512) * (bpm / 60.0) / 44100.0;
    }
    CHECK (processor.debugScheduler().picksStarted() > 0);
    CHECK (processor.debugUiState().ui.auditionEnabled);   // audio thread left it alone

    // The UI thread drains the pending auto-stop.
    processor.pollAuditionAutoStop();
    CHECK_FALSE (processor.debugUiState().ui.auditionEnabled);

    // Once drained, polling again is a no-op (a fresh enable stays on).
    processor.setAuditionEnabled (true);
    processor.pollAuditionAutoStop();
    CHECK (processor.debugUiState().ui.auditionEnabled);
}

TEST_CASE ("slice audition: RMB loop renders raw and stops clean")
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

    const auto loaded = processor.acquireLoadedSample();
    REQUIRE (loaded != nullptr);
    REQUIRE (loaded->slices.size() >= 2);
    const auto& sl = loaded->slices[1];

    std::vector<float> out (512, 0.0f);
    float* channels[1] { out.data() };
    Steinberg::Vst::AudioBusBuffers bus {};
    bus.numChannels = 1;
    bus.channelBuffers32 = channels;
    Steinberg::Vst::ProcessData data {};
    data.numSamples = 512;
    data.numOutputs = 1;
    data.outputs = &bus;

    // Hold: the slice loops raw (cursor reseeds to the slice head on the
    // audio side, so the first block already carries the onset burst).
    processor.startSliceAudition (sl.startFrame, sl.endFrame);
    REQUIRE (processor.process (data) == Steinberg::kResultOk);
    float peak = 0.0f;
    for (const float s : out)
        peak = std::max (peak, std::abs (s));
    CHECK (peak > 0.01f);
    CHECK (processor.debugScheduler().picksStarted() == 0);

    // Release: silence (stopped transport, no MIDI, nothing scheduled).
    processor.stopSliceAudition();
    REQUIRE (processor.process (data) == Steinberg::kResultOk);
    for (const float s : out)
        CHECK (s == 0.0f);
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

TEST_CASE ("detector sensitivity: 0 => trim-only slice; weights remap")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    auto loaded = processor.acquireLoadedSample();
    REQUIRE (loaded != nullptr);
    const std::size_t baseCount = loaded->slices.size();
    REQUIRE (baseCount >= 8);
    processor.setSliceProbability (1, 0.25f);

    // Invariant after every re-run: new slice i inherits the weight of the
    // old slice that contained its start-frame (1.0 if none).
    const auto checkRemap =
        [&] (const std::shared_ptr<const nedit::plugin::LoadedSample>& after,
             const std::vector<nedit::engine::Slice>& beforeSlices) {
            for (std::size_t i = 0; i < after->slices.size(); ++i)
            {
                float expected = 1.0f;
                for (std::size_t j = 0; j < beforeSlices.size(); ++j)
                    if (after->slices[i].startFrame >= beforeSlices[j].startFrame &&
                        after->slices[i].startFrame <  beforeSlices[j].endFrame)
                        expected = processor.getSliceProbability (static_cast<int> (j));
                CHECK (processor.getSliceProbability (static_cast<int> (i))
                       == Catch::Approx (expected));
            }
        };

    // Contract: sensitivity 0 => zero auto onsets => a single trim-span
    // slice (the trim-start boundary is always the first boundary).
    processor.setSensitivity (0.0f);
    CHECK (processor.debugUiState().sample.sensitivity == 0.0f);
    REQUIRE (processor.debugSliceCount() == 1);
    CHECK (processor.acquireLoadedSample()->slices[0].startFrame == 0);
    checkRemap (processor.acquireLoadedSample(), loaded->slices);

    // Ramping back up restores the detected boundaries (higher sensitivity
    // can only reveal MORE onsets between holdoffs) and remaps weights.
    processor.setSensitivity (1.0f);
    CHECK (processor.debugUiState().sample.sensitivity == 1.0f);
    CHECK (processor.debugSliceCount() >= static_cast<int> (baseCount));
    const auto restored = processor.acquireLoadedSample();
    checkRemap (restored, loaded->slices);

    // Out-of-range values are clamped to [0,1].
    processor.setSensitivity (5.0f);
    CHECK (processor.debugUiState().sample.sensitivity == 1.0f);
    processor.setSensitivity (-1.0f);
    CHECK (processor.debugUiState().sample.sensitivity == 0.0f);
}

TEST_CASE ("quantize toggle: snaps auto onsets only; manual points never move")
{
    const TempFile file ("nedit_test_click.wav", clickWav());

    nedit::plugin::NeditProcessor processor;
    REQUIRE (processor.initialize (nullptr) == Steinberg::kResultOk);
    REQUIRE (processor.requestSampleLoad (file.path));

    // Manual point at an arbitrary non-grid frame (snap off = exact).
    const std::int64_t oddFrame = 12345;
    REQUIRE (processor.addManualPoint (oddFrame, /*snap*/ false) >= 0);
    auto raw = processor.acquireLoadedSample();
    REQUIRE (raw != nullptr);
    const std::size_t rawCount = raw->slices.size();

    processor.setQuantizeTransients (true);
    CHECK (processor.debugUiState().sample.quantizeTransients);

    auto q = processor.acquireLoadedSample();
    REQUIRE (q != nullptr);
    // Quantizing only MERGES auto onsets that land on the same grid line --
    // it never invents boundaries -- so the count never grows, and trim
    // start + manual points (never quantized) stay as a floor of 2.
    CHECK (q->slices.size() <= rawCount);
    CHECK (q->slices.size() >= 2);
    bool manualExact = false;
    for (const auto& s : q->slices)
        if (s.startFrame == oddFrame) manualExact = true;
    CHECK (manualExact);

    // Toggling back off restores the exact raw slice list.
    processor.setQuantizeTransients (false);
    CHECK_FALSE (processor.debugUiState().sample.quantizeTransients);
    auto rawAgain = processor.acquireLoadedSample();
    REQUIRE (rawAgain != nullptr);
    CHECK (rawAgain->slices.size() == rawCount);
}

TEST_CASE ("trim-aware playback: slices are clipped to the soft trim")
{
    // Synthetic slice list covering [0, 96000) with five boundaries.
    const std::vector<nedit::engine::Slice> in {
        {       0,   12000 },
        {   12000,   24000 },
        {   24000,   48000 },
        {   48000,   60000 },
        {   60000,   96000 },
    };
    const std::vector<float> weights { 1.0f, 0.5f, 0.25f, 0.75f, 1.0f };

    std::vector<nedit::engine::Slice> out;
    std::vector<float> outWeights;

    // Whole-sample trim is the identity (the scheduler's default path --
    // what the existing audible end-to-end test exercises).
    nedit::plugin::clipSlicesToTrim (in, 0, 96000, weights, out, outWeights);
    CHECK (out == in);
    CHECK (outWeights == weights);

    // Trim [20000, 60000): the slice straddling the START handle is cut at
    // the handle (weight kept), the middle slices keep full extent, the one
    // straddling the END handle is cut there, and everything outside is
    // dropped -- no pick can read audio outside the trim.
    nedit::plugin::clipSlicesToTrim (in, 20000, 60000, weights, out, outWeights);
    REQUIRE (out.size() == 3);
    REQUIRE (outWeights.size() == 3);
    CHECK (out[0] == nedit::engine::Slice { 20000, 24000 });  // clipped at handle
    CHECK (outWeights[0] == Catch::Approx (0.5f));            // weight preserved
    CHECK (out[1] == nedit::engine::Slice { 24000, 48000 });
    CHECK (outWeights[1] == Catch::Approx (0.25f));
    CHECK (out[2] == nedit::engine::Slice { 48000, 60000 });  // full-thickness end slice
    CHECK (outWeights[2] == Catch::Approx (0.75f));

    // A trim entirely inside one slice keeps that slice (trim handles cut
    // both edges) -- playback still works on partial content.
    nedit::plugin::clipSlicesToTrim (in, 1000, 11000, weights, out, outWeights);
    REQUIRE (out.size() == 1);
    CHECK (out[0] == nedit::engine::Slice { 1000, 11000 });
    CHECK (outWeights[0] == Catch::Approx (1.0f));

    // Wholly-outside slices vanish: a trim in the silent tail produces no
    // pickable slices (scheduler would go silent, never sound outside-trim
    // content).
    nedit::plugin::clipSlicesToTrim (in, 96000, 96000, weights, out, outWeights);
    CHECK (out.empty());
    CHECK (outWeights.empty());

    // Shorter weights than slices: missing ones default to full weight.
    const std::vector<float> shortW { 0.1f };
    nedit::plugin::clipSlicesToTrim (in, 0, 96000, shortW, out, outWeights);
    REQUIRE (out.size() == in.size());
    CHECK (outWeights[0] == Catch::Approx (0.1f));
    for (std::size_t i = 1; i < outWeights.size(); ++i)
        CHECK (outWeights[i] == Catch::Approx (1.0f));
}
