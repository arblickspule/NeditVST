// StyleParameters: defaults, generic access, clamping, info table,
// applicable-parameter lists.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <state/StyleParameters.h>

#include <string>
#include <vector>

using namespace nedit::state;

namespace {

std::vector<StyleParamId> applicable (PlaybackStyle style)
{
    const auto result = applicableStyleParams (style);
    return { result.ids.begin(), result.ids.begin() + result.count };
}

} // namespace

TEST_CASE ("defaults match the original global values", "[styleparams]")
{
    const StyleParameters params;

    CHECK (params.filterResonance == 2.0f);
    CHECK (params.filterType == FilterType::lowPass);
    CHECK (params.curveShape == CurveShape::linear);
    CHECK (params.grainSizeMs == 10.0f);
    CHECK (params.grainSpeed == 4.0f);
    CHECK (params.subdivide == 0);  // Off
    CHECK (params.srReduction == 12.0f);
    CHECK (params.srReductionMode == SweepMode::fixed);
    CHECK (params.bitDepth == 5.0f);
    CHECK (params.bitDepthMode == SweepMode::fixed);
    CHECK (params.scratchRate == kNoteValue16n);
    CHECK (params.scratchForwardCurve == EasingCurve::linear);
    CHECK (params.scratchBackwardCurve == EasingCurve::linear);
    CHECK (params.flangerDelayMs == 2.0f);
    CHECK (params.flangerMix == 0.5f);
    CHECK (params.flangerFeedback == 0.3f);
    CHECK (params.volume == 1.0f);
    CHECK (params.volumeMode == VolumeRampMode::fixed);
}

TEST_CASE ("info-table defaults agree with struct defaults", "[styleparams]")
{
    const StyleParameters params;

    for (int i = 0; i < kNumStyleParams; ++i)
    {
        const auto id = static_cast<StyleParamId> (i);
        INFO ("param " << styleParamInfo (id).name);
        CHECK (params.get (id) == styleParamInfo (id).defaultValue);
    }
}

TEST_CASE ("generic set updates typed fields", "[styleparams]")
{
    StyleParameters params;

    params.set (StyleParamId::filterResonance, 7.5f);
    CHECK (params.filterResonance == 7.5f);

    params.set (StyleParamId::filterType, 2.0f);
    CHECK (params.filterType == FilterType::bandPass);

    params.set (StyleParamId::subdivide, 8.0f);
    CHECK (params.subdivide == 8);

    params.set (StyleParamId::scratchForwardCurve, 3.0f);
    CHECK (params.scratchForwardCurve == EasingCurve::easeInEaseOut);

    params.set (StyleParamId::volumeMode, 1.0f);
    CHECK (params.volumeMode == VolumeRampMode::rampUp);
}

TEST_CASE ("set clamps to documented ranges", "[styleparams]")
{
    StyleParameters params;

    params.set (StyleParamId::filterResonance, 100.0f);
    CHECK (params.filterResonance == kMaxFilterResonance);

    params.set (StyleParamId::filterResonance, -1.0f);
    CHECK (params.filterResonance == kMinFilterResonance);

    params.set (StyleParamId::grainSizeMs, 0.0f);
    CHECK (params.grainSizeMs == kMinGrainSizeMs);

    params.set (StyleParamId::srReduction, 1000.0f);
    CHECK (params.srReduction == kMaxSrReduction);

    params.set (StyleParamId::flangerFeedback, 1.0f);
    CHECK (params.flangerFeedback == kMaxFlangerFeedback);

    params.set (StyleParamId::filterType, 99.0f);
    CHECK (params.filterType == FilterType::bandPass);

    params.set (StyleParamId::subdivide, -5.0f);
    CHECK (params.subdivide == 0);

    params.set (StyleParamId::subdivide, 99.0f);
    CHECK (params.subdivide == kNumNoteValues);  // last option = longest note value

    params.set (StyleParamId::scratchRate, 99.0f);
    CHECK (params.scratchRate == kNumNoteValues - 1);
}

TEST_CASE ("get/set round-trips every parameter", "[styleparams]")
{
    StyleParameters params;

    for (int i = 0; i < kNumStyleParams; ++i)
    {
        const auto id = static_cast<StyleParamId> (i);
        const auto& info = styleParamInfo (id);

        params.set (id, info.maxValue);
        CHECK (params.get (id) == info.maxValue);

        params.set (id, info.minValue);
        CHECK (params.get (id) == info.minValue);
    }
}

TEST_CASE ("parameter names match the original table", "[styleparams]")
{
    CHECK (std::string (styleParamInfo (StyleParamId::filterResonance).name) == "Resonance");
    CHECK (std::string (styleParamInfo (StyleParamId::subdivide).name) == "Subdivide");
    CHECK (std::string (styleParamInfo (StyleParamId::srReduction).name) == "Sample Rate Reduction");
    CHECK (std::string (styleParamInfo (StyleParamId::scratchRate).name) == "Rate");
    CHECK (std::string (styleParamInfo (StyleParamId::volumeMode).name) == "Volume Mode");
}

TEST_CASE ("discrete option names", "[styleparams]")
{
    CHECK (std::string (styleParamOptionName (StyleParamId::subdivide, 0)) == "Off");
    CHECK (std::string (styleParamOptionName (StyleParamId::subdivide, 8)) == "16n");
    CHECK (std::string (styleParamOptionName (StyleParamId::scratchRate, 7)) == "16n");
    CHECK (std::string (styleParamOptionName (StyleParamId::filterType, 1)) == "High-pass");
    CHECK (std::string (styleParamOptionName (StyleParamId::volumeMode, 2)) == "Ramp Down");
    CHECK (std::string (styleParamOptionName (StyleParamId::srReductionMode, 1)) == "Sweep In");
    CHECK (std::string (styleParamOptionName (StyleParamId::scratchForwardCurve, 3)) == "Ease In-Out");

    // Out of range / not discrete -> nullptr.
    CHECK (styleParamOptionName (StyleParamId::subdivide, 21) == nullptr);
    CHECK (styleParamOptionName (StyleParamId::filterResonance, 0) == nullptr);
    CHECK (styleParamOptionName (StyleParamId::filterType, -1) == nullptr);
}

TEST_CASE ("applicable parameters per style match the original table", "[styleparams]")
{
    using Id = StyleParamId;

    // Every style ends with the two general parameters.
    for (int i = 0; i < kNumPlaybackStyles; ++i)
    {
        const auto ids = applicable (static_cast<PlaybackStyle> (i));
        REQUIRE (ids.size() >= 2);
        CHECK (ids[ids.size() - 2] == Id::subdivide);
        CHECK (ids[ids.size() - 1] == Id::volume);
    }

    CHECK (applicable (PlaybackStyle::forward)
           == std::vector<Id> { Id::subdivide, Id::volume });
    CHECK (applicable (PlaybackStyle::pingPong)
           == std::vector<Id> { Id::curveShape, Id::subdivide, Id::volume });
    CHECK (applicable (PlaybackStyle::tapeStop)
           == std::vector<Id> { Id::curveShape, Id::subdivide, Id::volume });
    CHECK (applicable (PlaybackStyle::stretch)
           == std::vector<Id> { Id::grainSizeMs, Id::grainSpeed, Id::subdivide, Id::volume });
    CHECK (applicable (PlaybackStyle::filterDown)
           == std::vector<Id> { Id::filterResonance, Id::filterType, Id::subdivide, Id::volume });
    CHECK (applicable (PlaybackStyle::filterUp)
           == std::vector<Id> { Id::filterResonance, Id::filterType, Id::subdivide, Id::volume });
    CHECK (applicable (PlaybackStyle::bitcrush)
           == std::vector<Id> { Id::srReduction, Id::bitDepth, Id::subdivide, Id::volume });
    CHECK (applicable (PlaybackStyle::scratch)
           == std::vector<Id> { Id::scratchRate, Id::scratchForwardCurve,
                                Id::scratchBackwardCurve, Id::subdivide, Id::volume });
    CHECK (applicable (PlaybackStyle::flanger)
           == std::vector<Id> { Id::flangerDelayMs, Id::flangerMix, Id::flangerFeedback,
                                Id::subdivide, Id::volume });
}

TEST_CASE ("swept parameters are exactly the original six", "[styleparams]")
{
    for (int i = 0; i < kNumStyleParams; ++i)
    {
        const auto id = static_cast<StyleParamId> (i);
        const bool expected = id == StyleParamId::srReduction
                           || id == StyleParamId::bitDepth
                           || id == StyleParamId::flangerDelayMs
                           || id == StyleParamId::flangerMix
                           || id == StyleParamId::flangerFeedback
                           || id == StyleParamId::volume;

        INFO ("param " << styleParamInfo (id).name);
        CHECK (styleParamInfo (id).swept == expected);
    }
}
