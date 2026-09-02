#include "JsonIO.h"

#include <nlohmann/json.hpp>

namespace nedit::state {

namespace {

    using nlohmann::json;

    // ---------------------------------------------------------------------
    // Writers
    // ---------------------------------------------------------------------

    json styleParametersToJson (const StyleParameters& params)
    {
        json j = json::object();

        for (int i = 0; i < kNumStyleParams; ++i)
        {
            const auto id = static_cast<StyleParamId> (i);
            j[styleParamInfo (id).name] = params.get (id);
        }

        // Per-style volume is not part of the id-indexed vocabulary; emit it
        // as a named per-style array (in PlaybackStyle ordinal order) so the
        // JSON stays self-describing.
        json volumes = json::array();
        for (int i = 0; i < kNumPlaybackStyles; ++i)
            volumes.push_back (params.getStyleVolume (static_cast<PlaybackStyle> (i)));
        j["Per-Style Volume"] = volumes;

        return j;
    }

    json samplePointsToJson (const std::vector<SamplePoint>& points)
    {
        json j = json::array();

        for (const auto& point : points)
            j.push_back ({ { "id", point.id }, { "position", point.position } });

        return j;
    }

    json overridesToJson (const std::map<std::uint32_t, std::map<StyleParamId, float>>& overrides)
    {
        json j = json::object();

        for (const auto& [cell, params] : overrides)
        {
            json cellJson = json::object();

            for (const auto& [id, value] : params)
                cellJson[styleParamInfo (id).name] = value;

            j[std::to_string (cell)] = std::move (cellJson);
        }

        return j;
    }

    json extensionsToJson (const std::map<std::uint32_t, std::uint16_t>& extensions)
    {
        json j = json::object();

        for (const auto& [cell, steps] : extensions)
            j[std::to_string (cell)] = steps;

        return j;
    }

    json gridToJson (const std::vector<std::int8_t>& grid)
    {
        json j = json::array();

        for (const auto cell : grid)
            j.push_back (static_cast<int> (cell));

        return j;
    }

    json patternToJson (const SequencerPattern& pattern)
    {
        return {
            { "rows", pattern.rows },
            { "columns", pattern.columns },
            { "stepResolutionIndex", pattern.stepResolutionIndex },
            { "patternLengthBarsIndex", pattern.patternLengthBarsIndex },
            { "grid", gridToJson (pattern.grid) },
            { "overrides", overridesToJson (pattern.overrides) },
            { "extensions", extensionsToJson (pattern.extensions) }
        };
    }

    json snapshotToJson (const PerformanceSnapshot& snapshot)
    {
        return {
            { "trimStartFrame", snapshot.trimStartFrame },
            { "trimEndFrame", snapshot.trimEndFrame },
            { "style", snapshot.style },
            { "params", styleParametersToJson (snapshot.params) },
            { "loop", snapshot.loop },
            { "sync", snapshot.sync }
        };
    }

    // ---------------------------------------------------------------------
    // Readers (tolerant: missing/mistyped fields keep defaults)
    // ---------------------------------------------------------------------

    template <typename T>
    void readField (const json& j, const char* key, T& target)
    {
        if (const auto it = j.find (key); it != j.end())
        {
            try
            {
                target = it->get<T>();
            }
            catch (const json::exception&)
            {
                // keep default
            }
        }
    }

    template <typename Enum>
    void readEnumField (const json& j, const char* key, Enum& target)
    {
        int raw = static_cast<int> (target);
        readField (j, key, raw);
        target = static_cast<Enum> (raw);
    }

    void styleParametersFromJson (const json& j, StyleParameters& params)
    {
        if (! j.is_object())
            return;

        for (int i = 0; i < kNumStyleParams; ++i)
        {
            const auto id = static_cast<StyleParamId> (i);
            float value = params.get (id);
            readField (j, styleParamInfo (id).name, value);
            params.set (id, value);
        }

        if (const auto it = j.find ("Per-Style Volume"); it != j.end() && it->is_array())
        {
            const std::size_t n = std::min (it->size(),
                                            static_cast<std::size_t> (kNumPlaybackStyles));
            for (std::size_t i = 0; i < n; ++i)
            {
                if ((*it)[i].is_number())
                    params.setStyleVolume (static_cast<PlaybackStyle> (i),
                                           (*it)[i].get<float>());
            }
        }
    }

    void samplePointsFromJson (const json& j, std::vector<SamplePoint>& points)
    {
        if (! j.is_array())
            return;

        points.clear();

        for (const auto& entry : j)
        {
            if (! entry.is_object())
                continue;

            SamplePoint point;
            readField (entry, "id", point.id);
            readField (entry, "position", point.position);
            points.push_back (point);
        }
    }

    void overridesFromJson (const json& j,
                            std::map<std::uint32_t, std::map<StyleParamId, float>>& overrides)
    {
        if (! j.is_object())
            return;

        overrides.clear();

        for (const auto& [cellKey, cellJson] : j.items())
        {
            if (! cellJson.is_object())
                continue;

            std::uint32_t cell = 0;

            try
            {
                cell = static_cast<std::uint32_t> (std::stoul (cellKey));
            }
            catch (...)
            {
                continue;
            }

            std::map<StyleParamId, float> params;

            // kNumStyleParamIds includes the reserved per-cell Volume key.
            for (int i = 0; i < kNumStyleParamIds; ++i)
            {
                const auto id = static_cast<StyleParamId> (i);
                const auto it = cellJson.find (styleParamInfo (id).name);

                if (it != cellJson.end() && it->is_number())
                    params[id] = it->get<float>();
            }

            if (! params.empty())
                overrides[cell] = std::move (params);
        }
    }

    void extensionsFromJson (const json& j, std::map<std::uint32_t, std::uint16_t>& extensions)
    {
        if (! j.is_object())
            return;

        extensions.clear();

        for (const auto& [cellKey, value] : j.items())
        {
            if (! value.is_number())
                continue;

            try
            {
                extensions[static_cast<std::uint32_t> (std::stoul (cellKey))] =
                    value.get<std::uint16_t>();
            }
            catch (...)
            {
            }
        }
    }

    void gridFromJson (const json& j, std::vector<std::int8_t>& grid)
    {
        if (! j.is_array())
            return;

        grid.clear();
        grid.reserve (j.size());

        for (const auto& cell : j)
            grid.push_back (cell.is_number() ? static_cast<std::int8_t> (cell.get<int>())
                                             : static_cast<std::int8_t> (-1));
    }

    void patternFromJson (const json& j, SequencerPattern& pattern)
    {
        if (! j.is_object())
            return;

        pattern.populated = true;
        readField (j, "rows", pattern.rows);
        readField (j, "columns", pattern.columns);
        readField (j, "stepResolutionIndex", pattern.stepResolutionIndex);
        readField (j, "patternLengthBarsIndex", pattern.patternLengthBarsIndex);

        if (const auto it = j.find ("grid"); it != j.end())
            gridFromJson (*it, pattern.grid);

        if (const auto it = j.find ("overrides"); it != j.end())
            overridesFromJson (*it, pattern.overrides);

        if (const auto it = j.find ("extensions"); it != j.end())
            extensionsFromJson (*it, pattern.extensions);
    }

    void snapshotFromJson (const json& j, PerformanceSnapshot& snapshot)
    {
        if (! j.is_object())
            return;

        snapshot.populated = true;
        readField (j, "trimStartFrame", snapshot.trimStartFrame);
        readField (j, "trimEndFrame", snapshot.trimEndFrame);
        readField (j, "style", snapshot.style);

        if (const auto it = j.find ("params"); it != j.end())
            styleParametersFromJson (*it, snapshot.params);

        readField (j, "loop", snapshot.loop);
        readField (j, "sync", snapshot.sync);
    }

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string toJson (const PluginState& state)
{
    json j;

    j["formatVersion"] = 1;
    j["triggerMode"] = static_cast<int> (state.triggerMode);

    // --- sample ---
    {
        const auto& s = state.sample;
        j["sample"] = {
            { "samplePath", s.samplePath },
            { "sampleContentHash", s.sampleContentHash },
            { "sampleLengthFrames", s.sampleLengthFrames },
            { "sampleSampleRate", s.sampleSampleRate },
            { "trimStartFrame", s.trimStartFrame },
            { "trimEndFrame", s.trimEndFrame },
            { "sensitivity", s.sensitivity },
            { "quantizeTransients", s.quantizeTransients },
            { "quantizeGridIndex", s.quantizeGridIndex },
            { "manualPoints", samplePointsToJson (s.manualPoints) },
            { "excludedPoints", samplePointsToJson (s.excludedPoints) },
            { "nextManualPointId", s.nextManualPointId },
            { "nextExcludedPointId", s.nextExcludedPointId },
            { "loopLengthBars", s.loopLengthBars },
            { "manualBpmOverrideEnabled", s.manualBpmOverrideEnabled },
            { "manualBpmOverrideValue", s.manualBpmOverrideValue }
        };
    }

    // --- render ---
    {
        const auto& r = state.render;
        j["render"] = {
            { "fadeInMs", r.fadeInMs },
            { "fadeOutMs", r.fadeOutMs },
            { "pitchMode", static_cast<int> (r.pitchMode) },
            { "grainSizeMs", r.grainSizeMs },
            { "grainSpeed", r.grainSpeed },
            { "grainWindowShape", static_cast<int> (r.grainWindowShape) },
            { "pitchShiftSemitones", r.pitchShiftSemitones },
            { "beatQuantizeTimeStretch", r.beatQuantizeTimeStretch },
            { "beatQuantizeRepitch", r.beatQuantizeRepitch }
        };
    }

    // --- generate ---
    {
        const auto& g = state.generate;
        j["generate"] = {
            { "generateMode", static_cast<int> (g.generateMode) },
            { "sliceWeights", g.sliceWeights },
            { "styleWeights", g.styleWeights },
            { "styleParams", styleParametersToJson (g.styleParams) },
            { "resetBarsIndex", g.resetBarsIndex },
            { "clockReferenceIndex", g.clockReferenceIndex },
            { "subdivisionWeights", g.subdivisionWeights },
            { "tapeStopScope", static_cast<int> (g.tapeStopScope) },
            { "filterSweepScope", static_cast<int> (g.filterSweepScope) }
        };
    }

    // --- sequencer ---
    {
        const auto& s = state.sequencer;

        json bank = json::object();
        for (int slot = 0; slot < kNumMidiNotes; ++slot)
        {
            const auto& pattern = s.patternBank[static_cast<std::size_t> (slot)];
            if (pattern.populated)
                bank[std::to_string (slot)] = patternToJson (pattern);
        }

        j["sequencer"] = {
            { "stepResolutionIndex", s.stepResolutionIndex },
            { "patternLengthBarsIndex", s.patternLengthBarsIndex },
            { "rows", s.rows },
            { "columns", s.columns },
            { "grid", gridToJson (s.grid) },
            { "overrides", overridesToJson (s.overrides) },
            { "extensions", extensionsToJson (s.extensions) },
            { "fallbackParams", styleParametersToJson (s.fallbackParams) },
            { "randomizeStyleWeights", s.randomizeStyleWeights },
            { "randomizeParametersForStyle", s.randomizeParametersForStyle },
            { "selectedDrawingStyle", s.selectedDrawingStyle },
            { "patternBank", std::move (bank) },
            { "patternSwitchTiming", static_cast<int> (s.patternSwitchTiming) },
            { "patternSwitchIntervalIndex", s.patternSwitchIntervalIndex }
        };
    }

    // --- performance ---
    {
        const auto& p = state.performance;

        json bank = json::object();
        for (int slot = 0; slot < kNumMidiNotes; ++slot)
        {
            const auto& snapshot = p.bank[static_cast<std::size_t> (slot)];
            if (snapshot.populated)
                bank[std::to_string (slot)] = snapshotToJson (snapshot);
        }

        j["performance"] = {
            { "bank", std::move (bank) },
            { "workingState", snapshotToJson (p.workingState) },
            { "workingStatePopulated", p.workingState.populated },
            { "focusedSlot", p.focusedSlot },
            { "trimSnapMode", static_cast<int> (p.trimSnapMode) },
            { "trimGridIndex", p.trimGridIndex },
            { "quantizeRecallEnabled", p.quantizeRecallEnabled },
            { "quantizeRecallIntervalIndex", p.quantizeRecallIntervalIndex }
        };
    }

    // --- control ---
    {
        const auto& c = state.control;
        j["control"] = {
            { "baseNote", c.baseNote },
            { "gateMode", c.gateMode },
            { "activeStyle", c.activeStyle },
            { "styleParams", styleParametersToJson (c.styleParams) }
        };
    }

    // --- ui ---
    {
        const auto& u = state.ui;
        j["ui"] = {
            { "activeTab", static_cast<int> (u.activeTab) },
            { "visibleStartNorm", u.visibleStartNorm },
            { "visibleEndNorm", u.visibleEndNorm }
        };
    }

    return j.dump (2);
}

std::optional<PluginState> fromJson (const std::string& text)
{
    const json j = json::parse (text, nullptr, false);

    if (j.is_discarded() || ! j.is_object())
        return std::nullopt;

    PluginState state;

    readEnumField (j, "triggerMode", state.triggerMode);

    if (const auto it = j.find ("sample"); it != j.end() && it->is_object())
    {
        auto& s = state.sample;
        readField (*it, "samplePath", s.samplePath);
        readField (*it, "sampleContentHash", s.sampleContentHash);
        readField (*it, "sampleLengthFrames", s.sampleLengthFrames);
        readField (*it, "sampleSampleRate", s.sampleSampleRate);
        readField (*it, "trimStartFrame", s.trimStartFrame);
        readField (*it, "trimEndFrame", s.trimEndFrame);
        readField (*it, "sensitivity", s.sensitivity);
        readField (*it, "quantizeTransients", s.quantizeTransients);
        readField (*it, "quantizeGridIndex", s.quantizeGridIndex);

        if (const auto pts = it->find ("manualPoints"); pts != it->end())
            samplePointsFromJson (*pts, s.manualPoints);

        if (const auto pts = it->find ("excludedPoints"); pts != it->end())
            samplePointsFromJson (*pts, s.excludedPoints);

        readField (*it, "nextManualPointId", s.nextManualPointId);
        readField (*it, "nextExcludedPointId", s.nextExcludedPointId);
        readField (*it, "loopLengthBars", s.loopLengthBars);
        readField (*it, "manualBpmOverrideEnabled", s.manualBpmOverrideEnabled);
        readField (*it, "manualBpmOverrideValue", s.manualBpmOverrideValue);
    }

    if (const auto it = j.find ("render"); it != j.end() && it->is_object())
    {
        auto& r = state.render;
        readField (*it, "fadeInMs", r.fadeInMs);
        readField (*it, "fadeOutMs", r.fadeOutMs);
        readEnumField (*it, "pitchMode", r.pitchMode);
        readField (*it, "grainSizeMs", r.grainSizeMs);
        readField (*it, "grainSpeed", r.grainSpeed);
        readEnumField (*it, "grainWindowShape", r.grainWindowShape);
        readField (*it, "pitchShiftSemitones", r.pitchShiftSemitones);
        readField (*it, "beatQuantizeTimeStretch", r.beatQuantizeTimeStretch);
        readField (*it, "beatQuantizeRepitch", r.beatQuantizeRepitch);
    }

    if (const auto it = j.find ("generate"); it != j.end() && it->is_object())
    {
        auto& g = state.generate;
        readEnumField (*it, "generateMode", g.generateMode);
        readField (*it, "sliceWeights", g.sliceWeights);
        readField (*it, "styleWeights", g.styleWeights);

        if (const auto params = it->find ("styleParams"); params != it->end())
            styleParametersFromJson (*params, g.styleParams);

        readField (*it, "resetBarsIndex", g.resetBarsIndex);
        readField (*it, "clockReferenceIndex", g.clockReferenceIndex);
        readField (*it, "subdivisionWeights", g.subdivisionWeights);
        readEnumField (*it, "tapeStopScope", g.tapeStopScope);
        readEnumField (*it, "filterSweepScope", g.filterSweepScope);
    }

    if (const auto it = j.find ("sequencer"); it != j.end() && it->is_object())
    {
        auto& s = state.sequencer;
        readField (*it, "stepResolutionIndex", s.stepResolutionIndex);
        readField (*it, "patternLengthBarsIndex", s.patternLengthBarsIndex);
        readField (*it, "rows", s.rows);
        readField (*it, "columns", s.columns);

        if (const auto grid = it->find ("grid"); grid != it->end())
            gridFromJson (*grid, s.grid);

        if (const auto overrides = it->find ("overrides"); overrides != it->end())
            overridesFromJson (*overrides, s.overrides);

        if (const auto extensions = it->find ("extensions"); extensions != it->end())
            extensionsFromJson (*extensions, s.extensions);

        if (const auto params = it->find ("fallbackParams"); params != it->end())
            styleParametersFromJson (*params, s.fallbackParams);

        readField (*it, "randomizeStyleWeights", s.randomizeStyleWeights);
        readField (*it, "randomizeParametersForStyle", s.randomizeParametersForStyle);
        readField (*it, "selectedDrawingStyle", s.selectedDrawingStyle);

        if (const auto bank = it->find ("patternBank"); bank != it->end() && bank->is_object())
        {
            for (const auto& [slotKey, patternJson] : bank->items())
            {
                try
                {
                    const auto slot = std::stoi (slotKey);

                    if (slot >= 0 && slot < kNumMidiNotes)
                        patternFromJson (patternJson,
                                         s.patternBank[static_cast<std::size_t> (slot)]);
                }
                catch (...)
                {
                }
            }
        }

        readEnumField (*it, "patternSwitchTiming", s.patternSwitchTiming);
        readField (*it, "patternSwitchIntervalIndex", s.patternSwitchIntervalIndex);
    }

    if (const auto it = j.find ("performance"); it != j.end() && it->is_object())
    {
        auto& p = state.performance;

        if (const auto bank = it->find ("bank"); bank != it->end() && bank->is_object())
        {
            for (const auto& [slotKey, snapshotJson] : bank->items())
            {
                try
                {
                    const auto slot = std::stoi (slotKey);

                    if (slot >= 0 && slot < kNumMidiNotes)
                        snapshotFromJson (snapshotJson,
                                          p.bank[static_cast<std::size_t> (slot)]);
                }
                catch (...)
                {
                }
            }
        }

        if (const auto working = it->find ("workingState"); working != it->end())
            snapshotFromJson (*working, p.workingState);

        readField (*it, "workingStatePopulated", p.workingState.populated);
        readField (*it, "focusedSlot", p.focusedSlot);
        readEnumField (*it, "trimSnapMode", p.trimSnapMode);
        readField (*it, "trimGridIndex", p.trimGridIndex);
        readField (*it, "quantizeRecallEnabled", p.quantizeRecallEnabled);
        readField (*it, "quantizeRecallIntervalIndex", p.quantizeRecallIntervalIndex);
    }

    if (const auto it = j.find ("control"); it != j.end() && it->is_object())
    {
        auto& c = state.control;
        readField (*it, "baseNote", c.baseNote);
        readField (*it, "gateMode", c.gateMode);
        readField (*it, "activeStyle", c.activeStyle);

        if (const auto params = it->find ("styleParams"); params != it->end())
            styleParametersFromJson (*params, c.styleParams);
    }

    if (const auto it = j.find ("ui"); it != j.end() && it->is_object())
    {
        auto& u = state.ui;
        readEnumField (*it, "activeTab", u.activeTab);
        readField (*it, "visibleStartNorm", u.visibleStartNorm);
        readField (*it, "visibleEndNorm", u.visibleEndNorm);
    }

    state.sanitize();
    return state;
}

} // namespace nedit::state
