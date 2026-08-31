// ParameterSurface: the pure parameter-mapping vocabulary (no SDK).
// IDs, step counts, titles, defaults and the normalized<->state round
// trips are the automation contract -- tested thoroughly here.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "plugin/ParameterSurface.h"

#include <cmath>

using namespace nedit;
using namespace nedit::plugin;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr float kEps = 1e-6f;
}

TEST_CASE ("surface: every id in range is valid, outside is not")
{
    for (std::uint32_t id = 0; id <= kLastStyleParamId; ++id)
        CHECK (isValidParamId (id));

    CHECK (isValidParamId (kParamTriggerMode));
    CHECK (isValidParamId (kParamQuantizeRecallInterval));

    CHECK_FALSE (isValidParamId (21));      // gap after style params
    CHECK_FALSE (isValidParamId (99));
    CHECK_FALSE (isValidParamId (kParamQuantizeRecallInterval + 1));
}

TEST_CASE ("surface: every valid id has a title and sane step count")
{
    for (std::uint32_t id = 0; id <= kLastStyleParamId; ++id)
    {
        INFO ("style param id " << id);
        const auto& info = state::styleParamInfo (static_cast<state::StyleParamId> (id));
        CHECK (titleFor (id) == info.name);
        CHECK (stepCountFor (id) == (info.discrete ? info.numOptions - 1 : 0));
    }

    CHECK (std::string_view (titleFor (kParamTriggerMode)) == "Trigger Mode");
    CHECK (stepCountFor (kParamTriggerMode) == 4);          // 5 modes
    CHECK (stepCountFor (kParamManualTempoEnabled) == 1);
    CHECK (stepCountFor (kParamManualTempoBpm) == 0);       // continuous
    CHECK (stepCountFor (kParamLoopLengthBars) == 0);
    CHECK (stepCountFor (kParamControlBaseNote) == 0);
    CHECK (stepCountFor (kParamControlGateMode) == 1);
    CHECK (stepCountFor (kParamQuantizeRecallEnabled) == 1);
    CHECK (stepCountFor (kParamQuantizeRecallInterval) == state::kNumNoteValues - 1);
}

TEST_CASE ("surface: style param round trip preserves values at endpoints")
{
    for (std::uint32_t id = 0; id <= kLastStyleParamId; ++id)
    {
        CAPTURE (id);

        // 0.0 -> min (or option 0), back out.
        state::PluginState s;
        applyNormalized (s, id, 0.0f);
        CHECK_THAT (toNormalized (s, id), WithinAbs (0.0f, kEps));

        // 1.0 -> max (or last option), back out.
        applyNormalized (s, id, 1.0f);
        CHECK_THAT (toNormalized (s, id), WithinAbs (1.0f, kEps));
    }
}

TEST_CASE ("surface: continuous style params map linearly")
{
    state::PluginState s;

    // Grain Speed 1..8x: norm 0.5 lands halfway (4.5 -> rounds? no:
    // continuous! value stays 4.5).
    applyNormalized (s, static_cast<std::uint32_t> (state::StyleParamId::grainSpeed), 0.5f);
    CHECK_THAT (s.generate.styleParams.grainSpeed, WithinAbs (4.5f, kEps));

    // Volume 0..1 identity.
    applyNormalized (s, static_cast<std::uint32_t> (state::StyleParamId::volume), 0.25f);
    CHECK_THAT (s.generate.styleParams.volume, WithinAbs (0.25f, kEps));
}

TEST_CASE ("surface: discrete style params land on exact options")
{
    state::PluginState s;

    // Filter Type: 3 options; norm 0.5 -> option 1 (high-pass? order per
    // table -- just assert it's an integer option and round-trips).
    const auto filterTypeId = static_cast<std::uint32_t> (state::StyleParamId::filterType);
    applyNormalized (s, filterTypeId, 0.5f);
    const auto option = static_cast<int> (s.generate.styleParams.filterType);
    CHECK (option >= 0);
    CHECK (option <= 2);
    CHECK_THAT (toNormalized (s, filterTypeId),
                WithinAbs (static_cast<float> (option) / 2.0f, kEps));

    // Subdivide: 21 options (Off + 20 note values); extremes.
    const auto subdivideId = static_cast<std::uint32_t> (state::StyleParamId::subdivide);
    applyNormalized (s, subdivideId, 1.0f);
    CHECK (s.generate.styleParams.subdivide == 20);
    applyNormalized (s, subdivideId, 0.0f);
    CHECK (s.generate.styleParams.subdivide == 0);
}

TEST_CASE ("surface: non-style params write through and round trip")
{
    state::PluginState s;

    applyNormalized (s, kParamTriggerMode, 1.0f);           // last mode = control
    CHECK (s.triggerMode == state::TriggerMode::control);
    // Mode drives tab: automating the trigger mode moves the visible tab so
    // the editor's syncTabBar follows a host automation lane.
    CHECK (s.ui.activeTab == state::UiTab::control);
    CHECK_THAT (toNormalized (s, kParamTriggerMode), WithinAbs (1.0f, kEps));

    applyNormalized (s, kParamTriggerMode, 0.0f);
    CHECK (s.triggerMode == state::TriggerMode::sliceLength);
    CHECK (s.ui.activeTab == state::UiTab::generate);

    applyNormalized (s, kParamTriggerMode, 0.5f);           // sequenced
    CHECK (s.triggerMode == state::TriggerMode::sequenced);
    CHECK (s.ui.activeTab == state::UiTab::sequence);

    // The top-level Generate modes double as generate.generateMode (the
    // ribbon aliases the same two entries), so a top-bar selection must
    // keep the ribbon's sub-mode in step -- mirror of setGenerateMode.
    applyNormalized (s, kParamTriggerMode, 1.0f);       // ... off generate
    s.generate.generateMode = state::TriggerMode::sliceLength;
    applyNormalized (s, kParamTriggerMode, 0.25f);      // clock
    CHECK (s.triggerMode == state::TriggerMode::clock);
    CHECK (s.generate.generateMode == state::TriggerMode::clock);
    applyNormalized (s, kParamTriggerMode, 0.0f);       // sliceLength
    CHECK (s.generate.generateMode == state::TriggerMode::sliceLength);

    applyNormalized (s, kParamManualTempoEnabled, 1.0f);
    CHECK (s.sample.manualBpmOverrideEnabled);

    applyNormalized (s, kParamManualTempoBpm, 0.5f);
    CHECK_THAT (s.sample.manualBpmOverrideValue, WithinAbs (165.0, 1e-3));

    applyNormalized (s, kParamLoopLengthBars, 1.0f / 15.0f * 3.0f);   // 4 bars
    CHECK (s.sample.loopLengthBars == 4);

    applyNormalized (s, kParamControlBaseNote, 36.0f / 127.0f);
    CHECK (s.control.baseNote == 36);

    applyNormalized (s, kParamControlGateMode, 0.9f);
    CHECK (s.control.gateMode);

    applyNormalized (s, kParamQuantizeRecallEnabled, 0.2f);   // below 0.5 = off
    CHECK_FALSE (s.performance.quantizeRecallEnabled);

    applyNormalized (s, kParamQuantizeRecallInterval, 13.0f / 19.0f);   // 4n palette slot
    CHECK (s.performance.quantizeRecallIntervalIndex == 13);
}

TEST_CASE ("surface: out-of-range normals clamp instead of corrupting")
{
    state::PluginState s;

    applyNormalized (s, kParamManualTempoBpm, -5.0f);
    CHECK_THAT (s.sample.manualBpmOverrideValue, WithinAbs (30.0, 1e-3));

    applyNormalized (s, kParamManualTempoBpm, 42.0f);
    CHECK_THAT (s.sample.manualBpmOverrideValue, WithinAbs (300.0, 1e-3));

    applyNormalized (s, kParamLoopLengthBars, 7.0f);
    CHECK (s.sample.loopLengthBars == 16);
}

TEST_CASE ("surface: defaults match a default-constructed state")
{
    for (std::uint32_t id = 0; id <= kLastStyleParamId; ++id)
        CHECK_THAT (defaultNormalized (id), WithinAbs (toNormalized (state::PluginState {}, id), kEps));

    CHECK_THAT (defaultNormalized (kParamTriggerMode), WithinAbs (0.0f, kEps));
    CHECK_FALSE (state::PluginState {}.sample.manualBpmOverrideEnabled);
}
