#include "Serialization.h"
#include "StreamIO.h"

namespace nedit::state {

namespace {

    // ---------------------------------------------------------------------
    // Section tags
    // ---------------------------------------------------------------------

    constexpr std::uint32_t fourcc (char a, char b, char c, char d) noexcept
    {
        return static_cast<std::uint32_t> (static_cast<std::uint8_t> (a))
             | (static_cast<std::uint32_t> (static_cast<std::uint8_t> (b)) << 8)
             | (static_cast<std::uint32_t> (static_cast<std::uint8_t> (c)) << 16)
             | (static_cast<std::uint32_t> (static_cast<std::uint8_t> (d)) << 24);
    }

    constexpr std::uint32_t kMagic       = fourcc ('N', 'E', 'D', 'T');
    constexpr std::uint32_t kTagGlobal   = fourcc ('G', 'L', 'O', 'B');
    constexpr std::uint32_t kTagSample   = fourcc ('S', 'M', 'P', 'L');
    constexpr std::uint32_t kTagRender   = fourcc ('R', 'N', 'D', 'R');
    constexpr std::uint32_t kTagGenerate = fourcc ('G', 'N', 'R', 'T');
    constexpr std::uint32_t kTagSequencer = fourcc ('S', 'E', 'Q', 'R');
    constexpr std::uint32_t kTagPerformance = fourcc ('P', 'E', 'R', 'F');
    constexpr std::uint32_t kTagControl = fourcc ('C', 'T', 'R', 'L');
    constexpr std::uint32_t kTagUi      = fourcc ('U', 'I', 'S', 'T');

    // ---------------------------------------------------------------------
    // Section framing
    // ---------------------------------------------------------------------

    class SectionWriter
    {
    public:
        SectionWriter (StreamWriter& writer, std::uint32_t tag)
            : out (writer)
        {
            out.writeU32 (tag);
            sizeOffset = out.size();
            out.writeU32 (0);  // patched in the destructor
            payloadStart = out.size();
        }

        ~SectionWriter()
        {
            out.patchU32 (sizeOffset, static_cast<std::uint32_t> (out.size() - payloadStart));
        }

        SectionWriter (const SectionWriter&) = delete;
        SectionWriter& operator= (const SectionWriter&) = delete;

    private:
        StreamWriter& out;
        std::size_t sizeOffset = 0;
        std::size_t payloadStart = 0;
    };

    // ---------------------------------------------------------------------
    // Common pieces
    // ---------------------------------------------------------------------

    void writeStyleParameters (StreamWriter& out, const StyleParameters& params)
    {
        out.writeU32 (static_cast<std::uint32_t> (kNumStyleParams));

        for (int i = 0; i < kNumStyleParams; ++i)
            out.writeF32 (params.get (static_cast<StyleParamId> (i)));

        // Per-style volume array (v3+), appended after the generic params.
        for (const float v : params.styleVolume)
            out.writeF32 (v);
    }

    [[nodiscard]] bool readStyleParameters (StreamReader& in, StyleParameters& params,
                                             std::uint32_t version)
    {
        const auto count = in.readU32();

        if (! in.ok() || count > 1024)
            return false;

        // Read what was written; ignore ids beyond what this build knows
        // (newer writer), keep defaults for ids it doesn't provide (older
        // writer). v2 streams carried a single scalar Volume (id 19) and its
        // mode (id 20) here; both are now per-style/dropped, so they are
        // skipped and the volumes keep their 1.0 defaults.
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const float value = in.readF32();

            if (! in.ok())
                return false;

            if (i < static_cast<std::uint32_t> (kNumStyleParams))
                params.set (static_cast<StyleParamId> (i), value);
        }

        // v3+: the per-style volume array follows.
        if (version >= 3)
        {
            for (auto& v : params.styleVolume)
            {
                v = in.readF32();
                if (! in.ok())
                    return false;
            }
        }

        return true;
    }

    void writeSamplePoints (StreamWriter& out, const std::vector<SamplePoint>& points)
    {
        out.writeU32 (static_cast<std::uint32_t> (points.size()));

        for (const auto& point : points)
        {
            out.writeI32 (point.id);
            out.writeI64 (point.position);
        }
    }

    [[nodiscard]] bool readSamplePoints (StreamReader& in, std::vector<SamplePoint>& points)
    {
        const auto count = in.readU32();

        // 12 bytes per point -- reject counts the payload cannot contain.
        if (! in.ok() || static_cast<std::size_t> (count) > in.remaining() / 12)
            return false;

        points.clear();
        points.reserve (count);

        for (std::uint32_t i = 0; i < count; ++i)
        {
            SamplePoint point;
            point.id = in.readI32();
            point.position = in.readI64();
            points.push_back (point);
        }

        return in.ok();
    }

    void writeGridData (StreamWriter& out,
                        const std::vector<std::int8_t>& grid,
                        const std::map<std::uint32_t, std::map<StyleParamId, float>>& overrides,
                        const std::map<std::uint32_t, std::uint16_t>& extensions)
    {
        out.writeU32 (static_cast<std::uint32_t> (grid.size()));
        for (const auto cell : grid)
            out.writeI8 (cell);

        out.writeU32 (static_cast<std::uint32_t> (overrides.size()));
        for (const auto& [cell, params] : overrides)
        {
            out.writeU32 (cell);
            out.writeU32 (static_cast<std::uint32_t> (params.size()));

            for (const auto& [id, value] : params)
            {
                out.writeU8 (static_cast<std::uint8_t> (id));
                out.writeF32 (value);
            }
        }

        out.writeU32 (static_cast<std::uint32_t> (extensions.size()));
        for (const auto& [cell, steps] : extensions)
        {
            out.writeU32 (cell);
            out.writeU16 (steps);
        }
    }

    [[nodiscard]] bool readGridData (StreamReader& in,
                                     std::vector<std::int8_t>& grid,
                                     std::map<std::uint32_t, std::map<StyleParamId, float>>& overrides,
                                     std::map<std::uint32_t, std::uint16_t>& extensions)
    {
        const auto gridSize = in.readU32();

        if (! in.ok() || static_cast<std::size_t> (gridSize) > in.remaining())
            return false;

        grid.clear();
        grid.reserve (gridSize);
        for (std::uint32_t i = 0; i < gridSize; ++i)
            grid.push_back (in.readI8());

        const auto overrideCount = in.readU32();

        if (! in.ok() || static_cast<std::size_t> (overrideCount) > in.remaining() / 8)
            return false;

        overrides.clear();
        for (std::uint32_t i = 0; i < overrideCount; ++i)
        {
            const auto cell = in.readU32();
            const auto paramCount = in.readU32();

            if (! in.ok() || static_cast<std::size_t> (paramCount) > in.remaining() / 5)
                return false;

            auto& params = overrides[cell];
            for (std::uint32_t p = 0; p < paramCount; ++p)
            {
                const auto rawId = in.readU8();
                const float value = in.readF32();

                if (isValidSequencerOverrideId (rawId))
                    params[static_cast<StyleParamId> (rawId)] = value;
            }

            if (params.empty())
                overrides.erase (cell);
        }

        const auto extensionCount = in.readU32();

        if (! in.ok() || static_cast<std::size_t> (extensionCount) > in.remaining() / 6)
            return false;

        extensions.clear();
        for (std::uint32_t i = 0; i < extensionCount; ++i)
        {
            const auto cell = in.readU32();
            const auto steps = in.readU16();
            extensions[cell] = steps;
        }

        return in.ok();
    }

    // ---------------------------------------------------------------------
    // Sections
    // ---------------------------------------------------------------------

    void writeGlobal (StreamWriter& out, const PluginState& state)
    {
        SectionWriter section (out, kTagGlobal);
        out.writeU8 (static_cast<std::uint8_t> (state.triggerMode));
    }

    [[nodiscard]] bool readGlobal (StreamReader& in, PluginState& state)
    {
        state.triggerMode = static_cast<TriggerMode> (in.readU8());
        return in.ok();
    }

    void writeSample (StreamWriter& out, const SampleState& sample)
    {
        SectionWriter section (out, kTagSample);

        out.writeString (sample.samplePath);
        out.writeU64 (sample.sampleContentHash);
        out.writeI64 (sample.sampleLengthFrames);
        out.writeF64 (sample.sampleSampleRate);
        out.writeI64 (sample.trimStartFrame);
        out.writeI64 (sample.trimEndFrame);
        out.writeF32 (sample.sensitivity);
        out.writeBool (sample.quantizeTransients);
        out.writeI32 (sample.quantizeGridIndex);
        writeSamplePoints (out, sample.manualPoints);
        writeSamplePoints (out, sample.excludedPoints);
        out.writeI32 (sample.nextManualPointId);
        out.writeI32 (sample.nextExcludedPointId);
        out.writeI32 (sample.loopLengthBars);
        out.writeBool (sample.manualBpmOverrideEnabled);
        out.writeF64 (sample.manualBpmOverrideValue);
    }

    [[nodiscard]] bool readSample (StreamReader& in, SampleState& sample)
    {
        sample.samplePath = in.readString();
        sample.sampleContentHash = in.readU64();
        sample.sampleLengthFrames = in.readI64();
        sample.sampleSampleRate = in.readF64();
        sample.trimStartFrame = in.readI64();
        sample.trimEndFrame = in.readI64();
        sample.sensitivity = in.readF32();
        sample.quantizeTransients = in.readBool();
        sample.quantizeGridIndex = in.readI32();

        if (! readSamplePoints (in, sample.manualPoints))
            return false;

        if (! readSamplePoints (in, sample.excludedPoints))
            return false;

        sample.nextManualPointId = in.readI32();
        sample.nextExcludedPointId = in.readI32();
        sample.loopLengthBars = in.readI32();
        sample.manualBpmOverrideEnabled = in.readBool();
        sample.manualBpmOverrideValue = in.readF64();
        return in.ok();
    }

    void writeRender (StreamWriter& out, const RenderState& render)
    {
        SectionWriter section (out, kTagRender);

        out.writeF32 (render.fadeInMs);
        out.writeF32 (render.fadeOutMs);
        out.writeU8 (static_cast<std::uint8_t> (render.pitchMode));
        out.writeF32 (render.grainSizeMs);
        out.writeF32 (render.grainSpeed);
        out.writeU8 (static_cast<std::uint8_t> (render.grainWindowShape));
        out.writeF32 (render.pitchShiftSemitones);
        out.writeBool (render.beatQuantizeTimeStretch);
        out.writeBool (render.beatQuantizeRepitch);
    }

    [[nodiscard]] bool readRender (StreamReader& in, RenderState& render, std::uint32_t version)
    {
        render.fadeInMs = in.readF32();
        render.fadeOutMs = in.readF32();
        render.pitchMode = static_cast<PitchMode> (in.readU8() != 0 ? 1 : 0);
        render.grainSizeMs = in.readF32();
        if (version >= 2)
            render.grainSpeed = in.readF32();
        render.grainWindowShape = static_cast<GrainWindowShape> (in.readU8() != 0 ? 1 : 0);
        render.pitchShiftSemitones = in.readF32();
        render.beatQuantizeTimeStretch = in.readBool();
        render.beatQuantizeRepitch = in.readBool();
        return in.ok();
    }

    void writeGenerate (StreamWriter& out, const GenerateState& generate)
    {
        SectionWriter section (out, kTagGenerate);

        out.writeU8 (static_cast<std::uint8_t> (generate.generateMode));

        out.writeU32 (static_cast<std::uint32_t> (generate.sliceWeights.size()));
        for (const float w : generate.sliceWeights)
            out.writeF32 (w);

        for (const float w : generate.styleWeights)
            out.writeF32 (w);

        writeStyleParameters (out, generate.styleParams);

        out.writeI32 (generate.resetBarsIndex);
        out.writeI32 (generate.clockReferenceIndex);

        for (const float w : generate.subdivisionWeights)
            out.writeF32 (w);

        out.writeU8 (static_cast<std::uint8_t> (generate.tapeStopScope));
        out.writeU8 (static_cast<std::uint8_t> (generate.filterSweepScope));
    }

    [[nodiscard]] bool readGenerate (StreamReader& in, GenerateState& generate,
                                     std::uint32_t version)
    {
        generate.generateMode = static_cast<TriggerMode> (in.readU8());

        const auto sliceWeightCount = in.readU32();

        if (! in.ok() || static_cast<std::size_t> (sliceWeightCount) > in.remaining() / 4)
            return false;

        generate.sliceWeights.clear();
        generate.sliceWeights.reserve (sliceWeightCount);
        for (std::uint32_t i = 0; i < sliceWeightCount; ++i)
            generate.sliceWeights.push_back (in.readF32());

        for (auto& w : generate.styleWeights)
            w = in.readF32();

        if (! readStyleParameters (in, generate.styleParams, version))
            return false;

        generate.resetBarsIndex = in.readI32();
        generate.clockReferenceIndex = in.readI32();

        for (auto& w : generate.subdivisionWeights)
            w = in.readF32();

        generate.tapeStopScope = static_cast<WindowScope> (in.readU8() != 0 ? 1 : 0);
        generate.filterSweepScope = static_cast<WindowScope> (in.readU8() != 0 ? 1 : 0);
        return in.ok();
    }

    void writeSequencerPatternBody (StreamWriter& out, const SequencerPattern& pattern)
    {
        out.writeI32 (pattern.rows);
        out.writeI32 (pattern.columns);
        out.writeI32 (pattern.stepResolutionIndex);
        out.writeI32 (pattern.patternLengthBarsIndex);
        writeGridData (out, pattern.grid, pattern.overrides, pattern.extensions);
    }

    [[nodiscard]] bool readSequencerPatternBody (StreamReader& in, SequencerPattern& pattern)
    {
        pattern.populated = true;
        pattern.rows = in.readI32();
        pattern.columns = in.readI32();
        pattern.stepResolutionIndex = in.readI32();
        pattern.patternLengthBarsIndex = in.readI32();
        return readGridData (in, pattern.grid, pattern.overrides, pattern.extensions);
    }

    void writeSequencer (StreamWriter& out, const SequencerState& sequencer)
    {
        SectionWriter section (out, kTagSequencer);

        out.writeI32 (sequencer.stepResolutionIndex);
        out.writeI32 (sequencer.patternLengthBarsIndex);
        out.writeI32 (sequencer.rows);
        out.writeI32 (sequencer.columns);
        writeGridData (out, sequencer.grid, sequencer.overrides, sequencer.extensions);
        writeStyleParameters (out, sequencer.fallbackParams);

        for (const float w : sequencer.randomizeStyleWeights)
            out.writeF32 (w);

        for (const bool flag : sequencer.randomizeParametersForStyle)
            out.writeBool (flag);

        out.writeI32 (sequencer.selectedDrawingStyle);

        // Pattern bank: only populated slots.
        std::uint32_t populatedCount = 0;
        for (const auto& pattern : sequencer.patternBank)
            if (pattern.populated)
                ++populatedCount;

        out.writeU32 (populatedCount);

        for (int slot = 0; slot < kNumMidiNotes; ++slot)
        {
            const auto& pattern = sequencer.patternBank[static_cast<std::size_t> (slot)];

            if (! pattern.populated)
                continue;

            out.writeU8 (static_cast<std::uint8_t> (slot));
            writeSequencerPatternBody (out, pattern);
        }

        out.writeU8 (static_cast<std::uint8_t> (sequencer.patternSwitchTiming));
        out.writeI32 (sequencer.patternSwitchIntervalIndex);

        // v4+: the grid viewport (issue #2). Appended at the section's end
        // so pre-v4 readers skip it harmlessly (trailing bytes are ignored).
        out.writeF64 (sequencer.viewport.zoomX);
        out.writeF64 (sequencer.viewport.zoomY);
        out.writeF64 (sequencer.viewport.originX);
        out.writeF64 (sequencer.viewport.originY);
    }

    [[nodiscard]] bool readSequencer (StreamReader& in, SequencerState& sequencer,
                                      std::uint32_t version)
    {
        sequencer.stepResolutionIndex = in.readI32();
        sequencer.patternLengthBarsIndex = in.readI32();
        sequencer.rows = in.readI32();
        sequencer.columns = in.readI32();

        if (! readGridData (in, sequencer.grid, sequencer.overrides, sequencer.extensions))
            return false;

        if (! readStyleParameters (in, sequencer.fallbackParams, version))
            return false;

        for (auto& w : sequencer.randomizeStyleWeights)
            w = in.readF32();

        for (auto& flag : sequencer.randomizeParametersForStyle)
            flag = in.readBool();

        sequencer.selectedDrawingStyle = in.readI32();

        const auto populatedCount = in.readU32();

        if (! in.ok() || populatedCount > static_cast<std::uint32_t> (kNumMidiNotes))
            return false;

        sequencer.patternBank.fill (SequencerPattern {});

        for (std::uint32_t i = 0; i < populatedCount; ++i)
        {
            const auto slot = in.readU8();

            if (! in.ok() || slot >= static_cast<std::uint8_t> (kNumMidiNotes))
                return false;

            SequencerPattern pattern;

            if (! readSequencerPatternBody (in, pattern))
                return false;

            sequencer.patternBank[static_cast<std::size_t> (slot)] = std::move (pattern);
        }

        sequencer.patternSwitchTiming = static_cast<PatternSwitchTiming> (in.readU8());

        if (static_cast<int> (sequencer.patternSwitchTiming) > 2)
            sequencer.patternSwitchTiming = PatternSwitchTiming::immediate;

        sequencer.patternSwitchIntervalIndex = in.readI32();

        // v4+ carries the grid viewport; older streams keep its defaults.
        if (version >= 4)
        {
            sequencer.viewport.zoomX = in.readF64();
            sequencer.viewport.zoomY = in.readF64();
            sequencer.viewport.originX = in.readF64();
            sequencer.viewport.originY = in.readF64();
        }

        return in.ok();
    }

    void writePerformanceSnapshotBody (StreamWriter& out, const PerformanceSnapshot& snapshot)
    {
        out.writeI64 (snapshot.trimStartFrame);
        out.writeI64 (snapshot.trimEndFrame);
        out.writeI32 (snapshot.style);
        writeStyleParameters (out, snapshot.params);
        out.writeBool (snapshot.loop);
        out.writeBool (snapshot.sync);
    }

    [[nodiscard]] bool readPerformanceSnapshotBody (StreamReader& in, PerformanceSnapshot& snapshot,
                                                     std::uint32_t version)
    {
        snapshot.populated = true;
        snapshot.trimStartFrame = in.readI64();
        snapshot.trimEndFrame = in.readI64();
        snapshot.style = in.readI32();

        if (! readStyleParameters (in, snapshot.params, version))
            return false;

        snapshot.loop = in.readBool();
        snapshot.sync = in.readBool();
        return in.ok();
    }

    void writePerformance (StreamWriter& out, const PerformanceState& performance)
    {
        SectionWriter section (out, kTagPerformance);

        std::uint32_t populatedCount = 0;
        for (const auto& snapshot : performance.bank)
            if (snapshot.populated)
                ++populatedCount;

        out.writeU32 (populatedCount);

        for (int slot = 0; slot < kNumMidiNotes; ++slot)
        {
            const auto& snapshot = performance.bank[static_cast<std::size_t> (slot)];

            if (! snapshot.populated)
                continue;

            out.writeU8 (static_cast<std::uint8_t> (slot));
            writePerformanceSnapshotBody (out, snapshot);
        }

        writePerformanceSnapshotBody (out, performance.workingState);
        out.writeBool (performance.workingState.populated);

        out.writeI32 (performance.focusedSlot);
        out.writeU8 (static_cast<std::uint8_t> (performance.trimSnapMode));
        out.writeI32 (performance.trimGridIndex);
        out.writeBool (performance.quantizeRecallEnabled);
        out.writeI32 (performance.quantizeRecallIntervalIndex);
    }

    [[nodiscard]] bool readPerformance (StreamReader& in, PerformanceState& performance,
                                        std::uint32_t version)
    {
        const auto populatedCount = in.readU32();

        if (! in.ok() || populatedCount > static_cast<std::uint32_t> (kNumMidiNotes))
            return false;

        performance.bank.fill (PerformanceSnapshot {});

        for (std::uint32_t i = 0; i < populatedCount; ++i)
        {
            const auto slot = in.readU8();

            if (! in.ok() || slot >= static_cast<std::uint8_t> (kNumMidiNotes))
                return false;

            PerformanceSnapshot snapshot;

            if (! readPerformanceSnapshotBody (in, snapshot, version))
                return false;

            performance.bank[static_cast<std::size_t> (slot)] = snapshot;
        }

        if (! readPerformanceSnapshotBody (in, performance.workingState, version))
            return false;

        performance.workingState.populated = in.readBool();

        performance.focusedSlot = in.readI32();
        performance.trimSnapMode = static_cast<TrimSnapMode> (in.readU8() != 0 ? 1 : 0);
        performance.trimGridIndex = in.readI32();
        performance.quantizeRecallEnabled = in.readBool();
        performance.quantizeRecallIntervalIndex = in.readI32();
        return in.ok();
    }

    void writeControl (StreamWriter& out, const ControlState& control)
    {
        SectionWriter section (out, kTagControl);

        out.writeI32 (control.baseNote);
        out.writeBool (control.gateMode);
        out.writeI32 (control.activeStyle);
        writeStyleParameters (out, control.styleParams);
    }

    [[nodiscard]] bool readControl (StreamReader& in, ControlState& control,
                                    std::uint32_t version)
    {
        control.baseNote = in.readI32();
        control.gateMode = in.readBool();
        control.activeStyle = in.readI32();
        return readStyleParameters (in, control.styleParams, version);
    }

    void writeUi (StreamWriter& out, const UiState& ui)
    {
        SectionWriter section (out, kTagUi);

        out.writeU8 (static_cast<std::uint8_t> (ui.activeTab));
        out.writeF64 (ui.visibleStartNorm);
        out.writeF64 (ui.visibleEndNorm);
        out.writeBool (ui.auditionEnabled);
    }

    [[nodiscard]] bool readUi (StreamReader& in, UiState& ui)
    {
        ui.activeTab = static_cast<UiTab> (in.readU8());
        ui.visibleStartNorm = in.readF64();
        ui.visibleEndNorm = in.readF64();
        if (in.ok())
            ui.auditionEnabled = in.readBool();
        return in.ok();
    }

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> serialize (const PluginState& state)
{
    StreamWriter out;

    out.writeU32 (kMagic);
    out.writeU32 (kStateFormatVersion);

    writeGlobal (out, state);
    writeSample (out, state.sample);
    writeRender (out, state.render);
    writeGenerate (out, state.generate);
    writeSequencer (out, state.sequencer);
    writePerformance (out, state.performance);
    writeControl (out, state.control);
    writeUi (out, state.ui);

    return out.take();
}

std::optional<PluginState> deserialize (const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr)
        return std::nullopt;

    StreamReader in (data, size);

    if (in.readU32() != kMagic)
        return std::nullopt;

    const auto version = in.readU32();

    if (! in.ok() || version == 0 || version > kStateFormatVersion)
        return std::nullopt;

    PluginState state;

    while (! in.atEnd())
    {
        const auto tag = in.readU32();
        const auto sectionSize = in.readU32();

        if (! in.ok())
            return std::nullopt;

        auto section = in.subReader (sectionSize);

        if (! section.ok() && sectionSize > 0)
            return std::nullopt;

        bool sectionOk = true;

        switch (tag)
        {
            case kTagGlobal:      sectionOk = readGlobal (section, state); break;
            case kTagSample:      sectionOk = readSample (section, state.sample); break;
            case kTagRender:      sectionOk = readRender (section, state.render, version); break;
            case kTagGenerate:    sectionOk = readGenerate (section, state.generate, version); break;
            case kTagSequencer:   sectionOk = readSequencer (section, state.sequencer, version); break;
            case kTagPerformance: sectionOk = readPerformance (section, state.performance, version); break;
            case kTagControl:     sectionOk = readControl (section, state.control, version); break;
            case kTagUi:          sectionOk = readUi (section, state.ui); break;
            default:              break;  // unknown section: skip (forward compat)
        }

        if (! sectionOk)
            return std::nullopt;
    }

    state.sanitize();
    return state;
}

} // namespace nedit::state
