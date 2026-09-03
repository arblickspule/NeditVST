#include "StyleParameters.h"

#include <cmath>

namespace nedit::state {

namespace {

    constexpr int kNumSweepModes = 3;
    constexpr int kNumVolumeRampModes = 3;
    constexpr int kNumFilterTypes = 3;
    constexpr int kNumCurveShapes = 2;
    constexpr int kNumSubdivideOptions = kNumNoteValues + 1;  // "Off" + palette

    constexpr std::array<StyleParamInfo, kNumStyleParamIds> kInfos { {
        //  name                          min                   max                   default  discrete stepped swept  numOptions
        { "Resonance",                   kMinFilterResonance,  kMaxFilterResonance,  2.0f,    false,  false,  false, 0 },
        { "Filter Type",                 0.0f,                 2.0f,                 0.0f,    true,   false,  false, kNumFilterTypes },
        { "Curve Shape",                 0.0f,                 1.0f,                 0.0f,    true,   false,  false, kNumCurveShapes },
        { "Grain Size",                  kMinGrainSizeMs,      kMaxGrainSizeMs,      10.0f,   false,  false,  false, 0 },
        { "Grain Speed",                 kMinGrainSpeed,       kMaxGrainSpeed,       4.0f,    false,  false,  false, 0 },
        { "Subdivide",                   0.0f,                 20.0f,                0.0f,    true,   true,   false, kNumSubdivideOptions },
        { "Sample Rate Reduction",       kMinSrReduction,      kMaxSrReduction,      12.0f,   false,  false,  true,  0 },
        { "Sample Rate Reduction Mode",  0.0f,                 2.0f,                 0.0f,    true,   false,  false, kNumSweepModes },
        { "Bit Depth",                   kMinBitDepth,         kMaxBitDepth,         5.0f,    false,  false,  true,  0 },
        { "Bit Depth Mode",              0.0f,                 2.0f,                 0.0f,    true,   false,  false, kNumSweepModes },
        { "Rate",                        0.0f,                 19.0f,                7.0f,    true,   false,  false, kNumNoteValues },
        { "Forward Curve",               0.0f,                 3.0f,                 3.0f,    true,   false,  false, kNumEasingCurves },
        { "Backward Curve",              0.0f,                 3.0f,                 3.0f,    true,   false,  false, kNumEasingCurves },
        { "Delay Time",                  kMinFlangerDelayMs,   kMaxFlangerDelayMs,   2.0f,    false,  false,  true,  0 },
        { "Delay Time Mode",             0.0f,                 2.0f,                 0.0f,    true,   false,  false, kNumSweepModes },
        { "Mix",                         0.0f,                 kMaxFlangerMix,       0.5f,    false,  false,  true,  0 },
        { "Mix Mode",                    0.0f,                 2.0f,                 0.0f,    true,   false,  false, kNumSweepModes },
        { "Feedback",                    0.0f,                 kMaxFlangerFeedback,  0.3f,    false,  false,  true,  0 },
        { "Feedback Mode",               0.0f,                 2.0f,                 0.0f,    true,   false,  false, kNumSweepModes },
        { "Volume",                      0.0f,                 1.0f,                 1.0f,    false,  false,  true,  0 },
        { "Volume Mode",                 0.0f,                 2.0f,                 0.0f,    true,   false,  false, kNumVolumeRampModes }
    } };

    [[nodiscard]] int clampOption (float value, int numOptions) noexcept
    {
        const int rounded = static_cast<int> (std::lround (value));
        return clampValue (rounded, 0, numOptions - 1);
    }

} // namespace

const StyleParamInfo& styleParamInfo (StyleParamId id) noexcept
{
    static constexpr StyleParamInfo kUnknown { "Unknown", 0.0f, 1.0f, 0.0f,
                                               false, false, false, 0 };

    const auto idx = static_cast<int> (id);
    if (idx < 0 || idx >= kNumStyleParamIds)
        return kUnknown;

    return kInfos[static_cast<std::size_t> (idx)];
}

const char* styleParamOptionName (StyleParamId id, int optionIndex) noexcept
{
    const auto& info = styleParamInfo (id);

    if (! info.discrete || optionIndex < 0 || optionIndex >= info.numOptions)
        return nullptr;

    switch (id)
    {
        case StyleParamId::filterType:
            return kFilterTypeNames[static_cast<std::size_t> (optionIndex)];

        case StyleParamId::curveShape:
            return kCurveShapeNames[static_cast<std::size_t> (optionIndex)];

        case StyleParamId::subdivide:
            return optionIndex == 0 ? "Off"
                                    : kNoteValues[static_cast<std::size_t> (optionIndex - 1)].name;

        case StyleParamId::srReductionMode:
        case StyleParamId::bitDepthMode:
        case StyleParamId::flangerDelayMode:
        case StyleParamId::flangerMixMode:
        case StyleParamId::flangerFeedbackMode:
            return kSweepModeNames[static_cast<std::size_t> (optionIndex)];

        case StyleParamId::scratchRate:
            return kNoteValues[static_cast<std::size_t> (optionIndex)].name;

        case StyleParamId::scratchForwardCurve:
        case StyleParamId::scratchBackwardCurve:
            return kEasingCurveNames[static_cast<std::size_t> (optionIndex)];

        case StyleParamId::volumeMode:
            return kVolumeRampModeNames[static_cast<std::size_t> (optionIndex)];

        default:
            return nullptr;
    }
}

ApplicableParams applicableStyleParams (PlaybackStyle style) noexcept
{
    ApplicableParams result;

    auto add = [&result] (StyleParamId id) noexcept
    {
        result.ids[static_cast<std::size_t> (result.count)] = id;
        ++result.count;
    };

    switch (style)
    {
        case PlaybackStyle::forward:
            break;

        case PlaybackStyle::pingPong:
        case PlaybackStyle::tapeStop:
            add (StyleParamId::curveShape);
            break;

        case PlaybackStyle::stretch:
            add (StyleParamId::grainSizeMs);
            add (StyleParamId::grainSpeed);
            break;

        case PlaybackStyle::filterDown:
        case PlaybackStyle::filterUp:
            add (StyleParamId::filterResonance);
            add (StyleParamId::filterType);
            break;

        case PlaybackStyle::bitcrush:
            add (StyleParamId::srReduction);
            add (StyleParamId::bitDepth);
            break;

        case PlaybackStyle::scratch:
            add (StyleParamId::scratchRate);
            add (StyleParamId::scratchForwardCurve);
            add (StyleParamId::scratchBackwardCurve);
            break;

        case PlaybackStyle::flanger:
            add (StyleParamId::flangerDelayMs);
            add (StyleParamId::flangerMix);
            add (StyleParamId::flangerFeedback);
            break;
    }

    // General parameters, available on every style (Forward included).
    // Subdivide is a retrigger; Volume is the per-cell override key.
    add (StyleParamId::subdivide);
    add (StyleParamId::volume);

    return result;
}

float StyleParameters::get (StyleParamId id) const noexcept
{
    switch (id)
    {
        case StyleParamId::filterResonance:      return filterResonance;
        case StyleParamId::filterType:           return static_cast<float> (filterType);
        case StyleParamId::curveShape:           return static_cast<float> (curveShape);
        case StyleParamId::grainSizeMs:          return grainSizeMs;
        case StyleParamId::grainSpeed:           return grainSpeed;
        case StyleParamId::subdivide:            return static_cast<float> (subdivide);
        case StyleParamId::srReduction:          return srReduction;
        case StyleParamId::srReductionMode:      return static_cast<float> (srReductionMode);
        case StyleParamId::bitDepth:             return bitDepth;
        case StyleParamId::bitDepthMode:         return static_cast<float> (bitDepthMode);
        case StyleParamId::scratchRate:          return static_cast<float> (scratchRate);
        case StyleParamId::scratchForwardCurve:  return static_cast<float> (scratchForwardCurve);
        case StyleParamId::scratchBackwardCurve: return static_cast<float> (scratchBackwardCurve);
        case StyleParamId::flangerDelayMs:       return flangerDelayMs;
        case StyleParamId::flangerDelayMode:     return static_cast<float> (flangerDelayMode);
        case StyleParamId::flangerMix:           return flangerMix;
        case StyleParamId::flangerMixMode:       return static_cast<float> (flangerMixMode);
        case StyleParamId::flangerFeedback:      return flangerFeedback;
        case StyleParamId::flangerFeedbackMode:  return static_cast<float> (flangerFeedbackMode);
        case StyleParamId::volume:               return styleVolume[0];   // per-style; no scalar generic value
        case StyleParamId::volumeMode:           return 0.0f;              // per-cell ramp; no scalar generic value
    }

    return 0.0f;
}

void StyleParameters::set (StyleParamId id, float value) noexcept
{
    if (! isValidStyleParamId (static_cast<int> (id)))
        return;

    const auto& info = styleParamInfo (id);
    const float clamped = clampValue (value, info.minValue, info.maxValue);

    switch (id)
    {
        case StyleParamId::filterResonance:
            filterResonance = clamped;
            break;
        case StyleParamId::filterType:
            filterType = static_cast<FilterType> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::curveShape:
            curveShape = static_cast<CurveShape> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::grainSizeMs:
            grainSizeMs = clamped;
            break;
        case StyleParamId::grainSpeed:
            grainSpeed = clamped;
            break;
        case StyleParamId::subdivide:
            subdivide = clampOption (value, info.numOptions);
            break;
        case StyleParamId::srReduction:
            srReduction = clamped;
            break;
        case StyleParamId::srReductionMode:
            srReductionMode = static_cast<SweepMode> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::bitDepth:
            bitDepth = clamped;
            break;
        case StyleParamId::bitDepthMode:
            bitDepthMode = static_cast<SweepMode> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::scratchRate:
            scratchRate = clampOption (value, info.numOptions);
            break;
        case StyleParamId::scratchForwardCurve:
            scratchForwardCurve = static_cast<EasingCurve> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::scratchBackwardCurve:
            scratchBackwardCurve = static_cast<EasingCurve> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::flangerDelayMs:
            flangerDelayMs = clamped;
            break;
        case StyleParamId::flangerDelayMode:
            flangerDelayMode = static_cast<SweepMode> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::flangerMix:
            flangerMix = clamped;
            break;
        case StyleParamId::flangerMixMode:
            flangerMixMode = static_cast<SweepMode> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::flangerFeedback:
            flangerFeedback = clamped;
            break;
        case StyleParamId::flangerFeedbackMode:
            flangerFeedbackMode = static_cast<SweepMode> (clampOption (value, info.numOptions));
            break;
        case StyleParamId::volume:
            break;   // per-style (setStyleVolume) / per-cell override; unreachable via set
        case StyleParamId::volumeMode:
            break;   // per-cell ramp mode; unreachable via set (guard rejects volumeMode)
    }
}

float StyleParameters::getStyleVolume (PlaybackStyle style) const noexcept
{
    const auto idx = static_cast<int> (style);
    return (idx >= 0 && idx < kNumPlaybackStyles)
        ? styleVolume[static_cast<std::size_t> (idx)]
        : 1.0f;
}

void StyleParameters::setStyleVolume (PlaybackStyle style, float value) noexcept
{
    const auto idx = static_cast<int> (style);
    if (idx < 0 || idx >= kNumPlaybackStyles)
        return;
    styleVolume[static_cast<std::size_t> (idx)] = clampValue (value, 0.0f, 1.0f);
}

void StyleParameters::sanitize() noexcept
{
    for (int i = 0; i < kNumStyleParams; ++i)
    {
        const auto id = static_cast<StyleParamId> (i);
        set (id, get (id));
    }

    for (auto& v : styleVolume)
        v = clampValue (v, 0.0f, 1.0f);
}

} // namespace nedit::state
