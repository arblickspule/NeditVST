#include "SlicerModel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace
{
    // Snapshot-based undo action (Step 12): every slice-editing operation
    // (add/move/remove/exclude/reset) is captured as "manual+excluded
    // point state before" vs "...after", and undo/redo just re-applies
    // whichever snapshot is needed. One class covers all of them rather
    // than a bespoke action type per operation -- coarse-grained on
    // purpose, keeps the system simple and uniform.
    class SliceEditUndoableAction : public juce::UndoableAction
    {
    public:
        SliceEditUndoableAction (SlicerModel& modelToUse,
                                  std::vector<SlicerModel::ManualPointInfo> beforeManual,
                                  std::vector<SlicerModel::ManualPointInfo> beforeExcluded,
                                  std::vector<SlicerModel::ManualPointInfo> afterManual,
                                  std::vector<SlicerModel::ManualPointInfo> afterExcluded)
            : model (modelToUse),
              before { std::move (beforeManual), std::move (beforeExcluded) },
              after { std::move (afterManual), std::move (afterExcluded) }
        {
        }

        bool perform() override
        {
            model.applyManualState (after.first, after.second);
            return true;
        }

        bool undo() override
        {
            model.applyManualState (before.first, before.second);
            return true;
        }

    private:
        SlicerModel& model;
        std::pair<std::vector<SlicerModel::ManualPointInfo>,
                  std::vector<SlicerModel::ManualPointInfo>> before, after;
    };

    // Note-value palette shared by the clock-reference menu and the
    // subdivision probability table (Step 14/15). Matches the standard
    // Max/M4L tempo-relative rate set, sorted shortest to longest, capped
    // at 1n (1nd -- 1.5 bars -- deliberately excluded per the "no longer
    // than 1 bar" decision). Beats are quarter-note units.
    struct NoteValueOption { const char* name; double beats; };

    const std::array<NoteValueOption, SlicerModel::numNoteValueOptions> noteValueOptions { {
        { "128n", 1.0 / 32.0 },
        { "64n",  1.0 / 16.0 },
        { "32nt", 1.0 / 12.0 },
        { "64nd", 3.0 / 32.0 },
        { "32n",  1.0 / 8.0 },
        { "16nt", 1.0 / 6.0 },
        { "32nd", 3.0 / 16.0 },
        { "16n",  1.0 / 4.0 },
        { "8nt",  1.0 / 3.0 },
        { "16nd", 3.0 / 8.0 },
        { "8n",   1.0 / 2.0 },
        { "4nt",  2.0 / 3.0 },
        { "8nd",  3.0 / 4.0 },
        { "4n",   1.0 },
        { "2nt",  4.0 / 3.0 },
        { "4nd",  3.0 / 2.0 },
        { "2n",   2.0 },
        { "1nt",  8.0 / 3.0 },
        { "2nd",  3.0 },
        { "1n",   4.0 }
    } };

    // Playback style names (Step 19/21/22/29/30), indexed the same way the
    // weighted table stores them.
    const std::array<const char*, SlicerModel::numPlaybackStyleOptions> playbackStyleNames { {
        "Forward", "Ping-Pong", "Tape Stop", "Stretch", "Filter Down", "Filter Up", "Bitcrush", "Scratch", "Flanger"
    } };

    // Sweep mode names (Step 49), shared by every swept parameter's own
    // Mode index -- Bitcrush's Sample Rate Reduction Mode/Bit Depth Mode,
    // and Flanger's Delay Time Mode/Mix Mode -- see SlicerModel::
    // isSequencerCellParameterSwept().
    const std::array<const char*, 3> sweepModeNames { {
        "Static", "Sweep In", "Sweep Out"
    } };

    // Volume ramp mode names -- Volume's own Mode option list, parallel to
    // sweepModeNames above but with directional language ("Ramp Up"/"Ramp
    // Down") rather than "Sweep In"/"Sweep Out", since volume has an
    // intuitive up/down sense the other swept parameters' effects don't.
    // Both ramp toward/away from silence (0.0), a fixed extreme like every
    // other swept parameter's Sweep In/Out -- see processBlock()'s
    // sweptVolumeValue.
    const std::array<const char*, 3> volumeRampModeNames { {
        "Static", "Ramp Up", "Ramp Down"
    } };

    // Tape Stop scope names (Step 21).
    const std::array<const char*, SlicerModel::numTapeStopScopeOptions> tapeStopScopeNames { {
        "Whole window", "Per tick"
    } };

    // Filter Sweep scope names (Step 30).
    const std::array<const char*, SlicerModel::numFilterSweepScopeOptions> filterSweepScopeNames { {
        "Whole window", "Per tick"
    } };

    // Filter Sweep filter type names (Step 46).
    const std::array<const char*, SlicerModel::numFilterSweepFilterTypeOptions> filterSweepFilterTypeNames { {
        "Low-pass", "High-pass", "Band-pass"
    } };

    // Curve shape names (Step 46) -- shared by Tape Stop decel and
    // Ping-Pong turnaround fade.
    const std::array<const char*, SlicerModel::numCurveShapeOptions> curveShapeNames { {
        "Linear", "Exponential"
    } };

    // Sequencer step parameter names (Step 45/46/47), indexed the same
    // way the per-cell override map's lookups (and the right-click menu)
    // use. Subdivide (index 5, Step 47) is general -- see
    // SlicerModel::getApplicableSequencerCellParameters(), which
    // appends it unconditionally rather than listing it per-style here.
    const std::array<const char*, SlicerModel::numSequencerCellParameters> sequencerCellParameterNames { {
        "Resonance", "Filter Type", "Curve Shape", "Grain Size", "Grain Speed", "Subdivide",
        "Sample Rate Reduction", "Sample Rate Reduction Mode", "Bit Depth", "Bit Depth Mode", "Rate",
        "Forward Curve", "Backward Curve", "Delay Time", "Delay Time Mode", "Mix", "Mix Mode",
        "Feedback", "Feedback Mode", "Volume", "Volume Mode"
    } };

    // Slice Length periodic reset (Step 34) -- names and their underlying
    // bar counts, held as a parallel pair rather than a NoteValueOption-
    // style struct since bar counts (not beats) are the natural unit
    // here, and every other place in this codebase already converts bars
    // to beats via "* 4" (4/4) rather than storing beats directly.
    const std::array<const char*, SlicerModel::numResetBarsOptions> resetBarsNames { {
        "1 bar", "2 bars", "4 bars", "8 bars"
    } };
    const std::array<int, SlicerModel::numResetBarsOptions> resetBarsValues { { 1, 2, 4, 8 } };

    // Sequenced mode's Pattern length (Step 38) -- same parallel-array
    // pattern as the reset-bars pair above, deliberately capped at 4 bars
    // (not 8) per spec.
    const std::array<const char*, SlicerModel::numPatternLengthBarsOptions> patternLengthBarsNames { {
        "1 bar", "2 bars", "4 bars"
    } };
    const std::array<int, SlicerModel::numPatternLengthBarsOptions> patternLengthBarsValues { { 1, 2, 4 } };
}

//==============================================================================
SlicerModel::SlicerModel()
{
    formatManager.registerBasicFormats();
    subdivisionProbabilities.assign (numNoteValueOptions, 1.0f);

    // Forward-only by default (NOT even odds like the other tables) --
    // guarantees byte-identical default playback, since none of Ping-Pong/
    // Tape Stop/Stretch/Filter Down/Filter Up/Bitcrush/Scratch is ever
    // drawn unless the user explicitly turns its weight up. (Step 22's
    // spec described this as "all other styles at weight 1, Stretch at
    // weight 0," which would actually break that guarantee for existing
    // users -- kept Forward-only here instead, since "must sound identical
    // to current behavior" is the longstanding hard requirement across
    // every style added so far.)
    playbackStyleProbabilities = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    // Performance mode's working state (Pass 1) -- seed its parameter
    // values from the same global defaults Slice Length/Clock start with,
    // rather than zeros (a broken default for parameters like Bit Depth).
    // From here on it's independent storage -- editing it never touches,
    // and is never touched by, the global values this seeded from.
    for (int i = 0; i < numSequencerCellParameters; ++i)
        performanceWorkingState.parameterValues[(size_t) i] = getSequencerCellParameterGlobalValue (i);
}

//==============================================================================
// Static schema (names/values/ranges) -- moved from SlicerAudioProcessor.

juce::String SlicerModel::getPlaybackStyleName (int index)
{
    if (index < 0 || index >= numPlaybackStyleOptions)
        return {};

    return playbackStyleNames[(size_t) index];
}

juce::String SlicerModel::getTapeStopScopeName (int index)
{
    if (index < 0 || index >= numTapeStopScopeOptions)
        return {};

    return tapeStopScopeNames[(size_t) index];
}

juce::String SlicerModel::getFilterSweepScopeName (int index)
{
    if (index < 0 || index >= numFilterSweepScopeOptions)
        return {};

    return filterSweepScopeNames[(size_t) index];
}

juce::String SlicerModel::getFilterSweepFilterTypeName (int index)
{
    if (index < 0 || index >= numFilterSweepFilterTypeOptions)
        return {};

    return filterSweepFilterTypeNames[(size_t) index];
}

juce::String SlicerModel::getCurveShapeName (int index)
{
    if (index < 0 || index >= numCurveShapeOptions)
        return {};

    return curveShapeNames[(size_t) index];
}

juce::String SlicerModel::getResetBarsName (int index)
{
    if (index < 0 || index >= numResetBarsOptions)
        return {};

    return resetBarsNames[(size_t) index];
}

int SlicerModel::getResetBarsValue (int index)
{
    if (index < 0 || index >= numResetBarsOptions)
        return 4; // matches the default index (2 -> 4 bars)

    return resetBarsValues[(size_t) index];
}

juce::String SlicerModel::getPatternLengthBarsName (int index)
{
    if (index < 0 || index >= numPatternLengthBarsOptions)
        return {};

    return patternLengthBarsNames[(size_t) index];
}

int SlicerModel::getPatternLengthBarsValue (int index)
{
    if (index < 0 || index >= numPatternLengthBarsOptions)
        return 1; // matches the default index (0 -> 1 bar)

    return patternLengthBarsValues[(size_t) index];
}

juce::String SlicerModel::getNoteValueName (int index)
{
    if (index < 0 || index >= numNoteValueOptions)
        return {};

    return noteValueOptions[(size_t) index].name;
}

double SlicerModel::getNoteValueBeats (int index)
{
    if (index < 0 || index >= numNoteValueOptions)
        return 1.0;

    return noteValueOptions[(size_t) index].beats;
}

juce::String SlicerModel::getSequencerCellParameterName (int index)
{
    if (index < 0 || index >= numSequencerCellParameters)
        return {};

    return sequencerCellParameterNames[(size_t) index];
}

bool SlicerModel::isSequencerCellParameterDiscrete (int index)
{
    return index == 1 || index == 2 || index == 5 || index == 7 || index == 9 || index == 10 || index == 11 || index == 12 || index == 14 || index == 16 || index == 18 || index == 20; // Filter Type, Curve Shape (Step 46); Subdivide (Step 47); Sample Rate Reduction Mode, Bit Depth Mode (Step 49); Rate, Forward Curve, Backward Curve (Scratch v1/v2); Delay Time Mode, Mix Mode, Feedback Mode (Flanger); Volume Mode
}

bool SlicerModel::isSequencerCellParameterSteppedSlider (int index)
{
    return index == 5; // Subdivide (Step 47) -- see its declaration in SlicerModel.h for why
}

bool SlicerModel::isSequencerCellParameterSwept (int index)
{
    return index == 6 || index == 8 || index == 13 || index == 15 || index == 17 || index == 19; // Sample Rate Reduction, Bit Depth (Step 49); Delay Time, Mix, Feedback (Flanger); Volume -- see declaration in SlicerModel.h
}

int SlicerModel::getSequencerCellParameterNumOptions (int index)
{
    if (index == 1) return numFilterSweepFilterTypeOptions;
    if (index == 2) return numCurveShapeOptions;
    if (index == 5) return numNoteValueOptions + 1; // Subdivide (Step 47): "Off" (option 0) + the shared note-value palette
    if (index == 7 || index == 9 || index == 14 || index == 16 || index == 18) return (int) sweepModeNames.size(); // Sample Rate Reduction Mode, Bit Depth Mode (Step 49); Delay Time Mode, Mix Mode, Feedback Mode (Flanger)
    if (index == 10) return numNoteValueOptions; // Rate (Scratch v1) -- the shared note-value palette, no "Off" (Scratch always has a rate)
    if (index == 11 || index == 12) return numEasingCurveOptions; // Forward Curve, Backward Curve (Scratch v2)
    if (index == 20) return (int) volumeRampModeNames.size(); // Volume Mode

    return 0;
}

juce::String SlicerModel::getSequencerCellParameterOptionName (int index, int optionIndex)
{
    if (index == 1) return getFilterSweepFilterTypeName (optionIndex);
    if (index == 2) return getCurveShapeName (optionIndex);
    if (index == 5) return optionIndex <= 0 ? "Off" : getNoteValueName (optionIndex - 1); // Subdivide (Step 47)
    if (index == 10) return getNoteValueName (optionIndex); // Rate (Scratch v1)
    if (index == 11 || index == 12) return getEasingCurveName (easingCurveFromIndex (optionIndex)); // Forward Curve, Backward Curve (Scratch v2)

    if (index == 7 || index == 9 || index == 14 || index == 16 || index == 18) // Sample Rate Reduction Mode, Bit Depth Mode (Step 49); Delay Time Mode, Mix Mode, Feedback Mode (Flanger)
    {
        if (optionIndex < 0 || optionIndex >= (int) sweepModeNames.size())
            return {};

        return sweepModeNames[(size_t) optionIndex];
    }

    if (index == 20) // Volume Mode
    {
        if (optionIndex < 0 || optionIndex >= (int) volumeRampModeNames.size())
            return {};

        return volumeRampModeNames[(size_t) optionIndex];
    }

    return {};
}

float SlicerModel::getSequencerCellParameterMin (int index)
{
    if (index == 0) return minFilterSweepResonance;
    if (index == 3) return minStretchGrainSizeMs;
    if (index == 4) return minStretchSpeedMultiplier;
    if (index == 6) return 1.0f;  // Sample Rate Reduction (Step 49) -- 1 sample hold = no reduction at all
    if (index == 8) return 1.0f;  // Bit Depth (Step 49) -- 1 bit is the quantizer's own hard floor
    if (index == 13) return flangerDelayTimeMinMs; // Delay Time (Flanger) -- least pronounced comb character
    if (index == 15) return 0.0f; // Mix (Flanger) -- fully dry
    if (index == 17) return 0.0f; // Feedback (Flanger) -- no feedback at all
    if (index == 19) return 0.0f; // Volume -- silence

    return 0.0f;
}

float SlicerModel::getSequencerCellParameterMax (int index)
{
    if (index == 0) return maxFilterSweepResonance;
    if (index == 3) return maxStretchGrainSizeMs;
    if (index == 4) return maxStretchSpeedMultiplier;
    if (index == 5) return (float) (numNoteValueOptions + 1 - 1); // Subdivide (Step 47) -- unused by the stepped slider itself (see isSequencerCellParameterSteppedSlider), kept for API symmetry
    if (index == 6) return bitcrushRateReductionExtreme; // Sample Rate Reduction (Step 49) -- slider tops out exactly at the Sweep In/Out target, so "Static, maxed out" and "fully swept" read as the same crush amount
    if (index == 8) return 16.0f; // Bit Depth (Step 49) -- a generous ceiling; the sweep's own extreme (1 bit) sits at the MIN end instead, see getSequencerCellParameterMin()
    if (index == 13) return flangerDelayTimeExtremeMs; // Delay Time (Flanger) -- slider tops out exactly at the Sweep In/Out target, same "Static maxed out == fully swept" convention as Sample Rate Reduction
    if (index == 15) return flangerMixExtreme; // Mix (Flanger) -- fully wet, same convention
    if (index == 17) return flangerFeedbackExtreme; // Feedback (Flanger) -- 88%, short of self-oscillation, same convention
    if (index == 19) return 1.0f; // Volume -- full volume/unity gain

    return 1.0f;
}

std::vector<int> SlicerModel::getApplicableSequencerCellParameters (int style)
{
    // Parameter indices below match getSequencerCellParameterName()'s own
    // ordering: 0 Resonance, 1 Filter Type, 2 Curve Shape, 3 Grain Size,
    // 4 Grain Speed (Step 45/46), 6 Sample Rate Reduction, 8 Bit Depth
    // (Step 49), 13 Delay Time, 15 Mix, 17 Feedback (Flanger -- their
    // paired Mode indices 7/9/14/16/18 are deliberately never listed
    // here, see isSequencerCellParameterSwept()). Table form rather
    // than a single flat list filtered by style -- keeps "which style
    // offers what" readable at a glance and trivially extensible (a
    // future parameter just adds itself to whichever case(s) it applies
    // to). Style indices match indexToPlaybackStyle()'s ordinals, the
    // same ones sequencerGrid itself stores.
    std::vector<int> params;

    switch (style)
    {
        case 0: break;             // Forward -- no style-specific params, but Subdivide (below) still applies
        case 1: params = { 2 }; break;    // Ping-Pong -- Curve Shape (turnaround fade)
        case 2: params = { 2 }; break;    // Tape Stop -- Curve Shape (decel)
        case 3: params = { 3, 4 }; break; // Stretch -- Grain Size, Grain Speed
        case 4:                           // Filter Down
        case 5: params = { 0, 1 }; break; // Filter Up -- Resonance, Filter Type
        case 6: params = { 6, 8 }; break;      // Bitcrush (Step 49) -- Sample Rate Reduction, Bit Depth
        case 7: params = { 10, 11, 12 }; break; // Scratch (v1/v2) -- Rate, Forward Curve, Backward Curve
        case 8: params = { 13, 15, 17 }; break; // Flanger -- Delay Time, Mix, Feedback
        default: return {};        // empty/invalid cell (-1) -- no menu at all
    }

    // Subdivide (Step 47) and Volume (index 19): both general, not
    // style-specific -- appended for every valid style above (including
    // Forward), unlike everything else in this function. Volume is a
    // pure gain stage layered after whatever the style's own DSP already
    // produces, so it applies identically regardless of which style (if
    // any -- Forward included) is active.
    params.push_back (5);
    params.push_back (19);
    return params;
}

float SlicerModel::getSequencerCellParameterGlobalValue (int index) const
{
    switch (index)
    {
        case 0: return getFilterSweepResonance();
        case 1: return (float) getFilterSweepFilterType();
        case 2: return (float) getCurveShape();
        case 3: return getStretchGrainSizeMs();
        case 4: return getStretchSpeedMultiplier();
        case 5: return 0.0f; // Subdivide (Step 47) -- no global dial; Off is always the fallback/default
        case 6: return getBitcrushRateReductionGlobal(); // Sample Rate Reduction
        case 7: return (float) getBitcrushRateReductionModeGlobal();
        case 8: return getBitcrushBitDepthGlobal(); // Bit Depth
        case 9: return (float) getBitcrushBitDepthModeGlobal();
        case 10: return (float) getScratchRateGlobal(); // Rate (Scratch v1)
        case 11: return (float) getScratchForwardCurveGlobal(); // Forward Curve (Scratch v2)
        case 12: return (float) getScratchBackwardCurveGlobal(); // Backward Curve (Scratch v2)
        case 13: return getFlangerDelayTimeGlobal(); // Delay Time (Flanger)
        case 14: return (float) getFlangerDelayTimeModeGlobal();
        case 15: return getFlangerMixGlobal(); // Mix (Flanger)
        case 16: return (float) getFlangerMixModeGlobal();
        case 17: return getFlangerFeedbackGlobal(); // Feedback (Flanger)
        case 18: return (float) getFlangerFeedbackModeGlobal();
        case 19: return 1.0f; // Volume -- no global dial; full volume (no change) is always the fallback/default
        case 20: return 0.0f; // Volume Mode -- Static is always the fallback/default
        default: return 0.0f;
    }
}

void SlicerModel::setSequencerCellParameterGlobalValue (int index, float value)
{
    switch (index)
    {
        case 0: setFilterSweepResonance (value); break;
        case 1: setFilterSweepFilterType (juce::roundToInt (value)); break;
        case 2: setCurveShape (juce::roundToInt (value)); break;
        case 3: setStretchGrainSizeMs (value); break;
        case 4: setStretchSpeedMultiplier (value); break;
        case 6: setBitcrushRateReductionGlobal (value); break;
        case 7: setBitcrushRateReductionModeGlobal (juce::roundToInt (value)); break;
        case 8: setBitcrushBitDepthGlobal (value); break;
        case 9: setBitcrushBitDepthModeGlobal (juce::roundToInt (value)); break;
        case 10: setScratchRateGlobal (juce::roundToInt (value)); break;
        case 11: setScratchForwardCurveGlobal (juce::roundToInt (value)); break;
        case 12: setScratchBackwardCurveGlobal (juce::roundToInt (value)); break;
        case 13: setFlangerDelayTimeGlobal (value); break;
        case 14: setFlangerDelayTimeModeGlobal (juce::roundToInt (value)); break;
        case 15: setFlangerMixGlobal (value); break;
        case 16: setFlangerMixModeGlobal (juce::roundToInt (value)); break;
        case 17: setFlangerFeedbackGlobal (value); break;
        case 18: setFlangerFeedbackModeGlobal (juce::roundToInt (value)); break;
        default: break; // indices 5 (Subdivide) and 19/20 (Volume/Volume Mode) have no global dial; out-of-range is a no-op
    }
}

//==============================================================================
// State persistence (nextsteps 1.4) -- XML-encoded for the dev stage (see
// docs/state-serialization-decision.md). The two methods are the only place
// the encoding leaks: the processor's getStateInformation/setStateInformation
// just forward the MemoryBlock. Additive + tolerant by design -- restoreState
// ignores unknown tags, leaves absent sections unchanged, clamps every
// restored index/value to its valid range, and size-guards the slice-indexed
// tables (grid/overrides/slice probabilities) against a different sample.

// Comma-joined list helpers shared by both methods below (anonymous-namespace
// free functions, so they stay out of the class interface).
namespace
{
    juce::String joinFloatList (const std::vector<float>& values)
    {
        juce::StringArray tokens;
        for (float v : values)
            tokens.add (juce::String (v, 6));
        return tokens.joinIntoString (",");
    }

    juce::String joinIntList (const std::vector<int>& values)
    {
        juce::StringArray tokens;
        for (int v : values)
            tokens.add (juce::String (v));
        return tokens.joinIntoString (",");
    }

    void parseFloatList (const juce::XmlElement* element, const char* attribute, std::vector<float>& out)
    {
        if (element == nullptr || ! element->hasAttribute (attribute))
            return;

        juce::StringArray tokens;
        tokens.addTokens (element->getStringAttribute (attribute), ",", "");

        out.clear();
        out.reserve (tokens.size());
        for (const auto& token : tokens)
            out.push_back (token.getFloatValue());
    }

    void parseIntList (const juce::XmlElement* element, const char* attribute, std::vector<int>& out)
    {
        if (element == nullptr || ! element->hasAttribute (attribute))
            return;

        juce::StringArray tokens;
        tokens.addTokens (element->getStringAttribute (attribute), ",", "");

        out.clear();
        out.reserve (tokens.size());
        for (const auto& token : tokens)
            out.push_back (token.getIntValue());
    }
}

void SlicerModel::saveState (juce::MemoryBlock& destData)
{
    const juce::ScopedLock sl (sampleLock);

    juce::XmlElement root ("neditvst");
    root.setAttribute ("version", 1);

    juce::XmlElement* globals = root.createNewChildElement ("globals");
    globals->setAttribute ("loopLengthBars", loopLengthBars.load());
    globals->setAttribute ("trimStartSample", trimStartSample.load());
    globals->setAttribute ("trimEndSample", trimEndSample.load());
    globals->setAttribute ("tempoTrimStartSample", tempoTrimStartSample.load());
    globals->setAttribute ("tempoTrimEndSample", tempoTrimEndSample.load());
    globals->setAttribute ("manualBpmOverrideEnabled", manualBpmOverrideEnabled.load());
    globals->setAttribute ("manualBpmOverrideValue", (double) manualBpmOverrideValue.load());
    globals->setAttribute ("sensitivity", currentSensitivity.load());
    globals->setAttribute ("fadeInMs", fadeInMs.load());
    globals->setAttribute ("fadeOutMs", fadeOutMs.load());
    globals->setAttribute ("quantizeTransientsEnabled", quantizeTransientsEnabled.load());
    globals->setAttribute ("quantizeGridIndex", quantizeGridIndex.load());
    globals->setAttribute ("performanceTrimSnapMode", (int) performanceTrimSnapMode.load());
    globals->setAttribute ("performanceTrimGridIndex", performanceTrimGridIndex.load());
    globals->setAttribute ("triggerMode", (int) triggerMode.load());
    globals->setAttribute ("clockReferenceIndex", clockReferenceIndex.load());
    globals->setAttribute ("tapeStopScope", (int) tapeStopScope.load());
    globals->setAttribute ("filterSweepScope", (int) filterSweepScope.load());
    globals->setAttribute ("resetBarsIndex", resetBarsIndex.load());
    globals->setAttribute ("pitchMode", (int) pitchMode.load());
    globals->setAttribute ("grainSizeMs", grainSizeMs.load());
    globals->setAttribute ("grainWindowShape", (int) grainWindowShape.load());
    globals->setAttribute ("pitchShiftSemitones", pitchShiftSemitones.load());
    globals->setAttribute ("beatQuantizeSliceLengthEnabled", beatQuantizeSliceLengthEnabled.load());
    globals->setAttribute ("beatQuantizeSliceLengthEnabledRepitch", beatQuantizeSliceLengthEnabledRepitch.load());

    // The style globals map 1:1 onto the cell-parameter indices 0..18 (the
    // shared schema getSequencerCellParameter*() exposes), so both save and
    // restore round-trip them through that same index space.
    juce::XmlElement* styleGlobals = root.createNewChildElement ("styleGlobals");
    styleGlobals->setAttribute ("stretchGrainSizeMs", stretchGrainSizeMsValue.load());
    styleGlobals->setAttribute ("stretchSpeedMultiplier", stretchSpeedMultiplierValue.load());
    styleGlobals->setAttribute ("filterSweepResonance", filterSweepResonanceValue.load());
    styleGlobals->setAttribute ("filterSweepFilterType", filterSweepFilterTypeValue.load());
    styleGlobals->setAttribute ("curveShape", curveShapeValue.load());
    styleGlobals->setAttribute ("bitcrushRateReduction", bitcrushRateReductionGlobalValue.load());
    styleGlobals->setAttribute ("bitcrushRateReductionMode", bitcrushRateReductionModeGlobalValue.load());
    styleGlobals->setAttribute ("bitcrushBitDepth", bitcrushBitDepthGlobalValue.load());
    styleGlobals->setAttribute ("bitcrushBitDepthMode", bitcrushBitDepthModeGlobalValue.load());
    styleGlobals->setAttribute ("scratchRate", scratchRateGlobalValue.load());
    styleGlobals->setAttribute ("scratchForwardCurve", scratchForwardCurveGlobalValue.load());
    styleGlobals->setAttribute ("scratchBackwardCurve", scratchBackwardCurveGlobalValue.load());
    styleGlobals->setAttribute ("flangerDelayTime", flangerDelayTimeGlobalValue.load());
    styleGlobals->setAttribute ("flangerDelayTimeMode", flangerDelayTimeModeGlobalValue.load());
    styleGlobals->setAttribute ("flangerMix", flangerMixGlobalValue.load());
    styleGlobals->setAttribute ("flangerMixMode", flangerMixModeGlobalValue.load());
    styleGlobals->setAttribute ("flangerFeedback", flangerFeedbackGlobalValue.load());
    styleGlobals->setAttribute ("flangerFeedbackMode", flangerFeedbackModeGlobalValue.load());

    juce::XmlElement* probabilities = root.createNewChildElement ("probabilities");
    probabilities->setAttribute ("slice", joinFloatList (sliceProbabilities));
    probabilities->setAttribute ("subdivision", joinFloatList (subdivisionProbabilities));
    probabilities->setAttribute ("playbackStyle", joinFloatList (playbackStyleProbabilities));

    juce::XmlElement* sequencer = root.createNewChildElement ("sequencer");
    sequencer->setAttribute ("stepResolutionIndex", stepResolutionIndex.load());
    sequencer->setAttribute ("patternLengthBarsIndex", patternLengthBarsIndex.load());
    sequencer->setAttribute ("selectedDrawingStyle", selectedDrawingStyle.load());
    sequencer->setAttribute ("grid", joinIntList (sequencerGrid));

    for (const auto& cell : sequencerCellParameterOverrides)
    {
        juce::XmlElement* cellElement = sequencer->createNewChildElement ("override");
        cellElement->setAttribute ("index", cell.first);

        for (const auto& param : cell.second)
        {
            juce::XmlElement* paramElement = cellElement->createNewChildElement ("param");
            paramElement->setAttribute ("name", param.first);
            paramElement->setAttribute ("value", param.second);
        }
    }

    for (const auto& cell : sequencerCellExtendedLengthSteps)
    {
        juce::XmlElement* cellElement = sequencer->createNewChildElement ("extendedLength");
        cellElement->setAttribute ("index", cell.first);
        cellElement->setAttribute ("steps", cell.second);
    }

    juce::XmlElement* recall = root.createNewChildElement ("recall");
    recall->setAttribute ("patternSwitchTiming", (int) patternSwitchTiming.load());
    recall->setAttribute ("patternSwitchIntervalIndex", patternSwitchIntervalIndex.load());
    recall->setAttribute ("performanceQuantizeRecallEnabled", performanceQuantizeRecallEnabled.load());
    recall->setAttribute ("performanceQuantizeRecallIntervalIndex", performanceQuantizeRecallIntervalIndex.load());

    juce::XmlElement* patternBankElement = root.createNewChildElement ("patternBank");
    for (int slot = 0; slot < 128; ++slot)
    {
        const auto& pattern = patternBank[(size_t) slot];
        if (! pattern.populated)
            continue;

        juce::XmlElement* patternElement = patternBankElement->createNewChildElement ("pattern");
        patternElement->setAttribute ("slot", slot);
        patternElement->setAttribute ("populated", true);
        patternElement->setAttribute ("rows", pattern.rows);
        patternElement->setAttribute ("columns", pattern.columns);
        patternElement->setAttribute ("stepResolutionIndex", pattern.stepResolutionIndex);
        patternElement->setAttribute ("patternLengthBarsIndex", pattern.patternLengthBarsIndex);
        patternElement->setAttribute ("grid", joinIntList (pattern.grid));

        for (const auto& cell : pattern.parameterOverrides)
        {
            juce::XmlElement* cellElement = patternElement->createNewChildElement ("override");
            cellElement->setAttribute ("index", cell.first);

            for (const auto& param : cell.second)
            {
                juce::XmlElement* paramElement = cellElement->createNewChildElement ("param");
                paramElement->setAttribute ("name", param.first);
                paramElement->setAttribute ("value", param.second);
            }
        }

        for (const auto& cell : pattern.extendedLengthSteps)
        {
            juce::XmlElement* cellElement = patternElement->createNewChildElement ("extendedLength");
            cellElement->setAttribute ("index", cell.first);
            cellElement->setAttribute ("steps", cell.second);
        }
    }

    juce::XmlElement* performanceBankElement = root.createNewChildElement ("performanceBank");
    for (int slot = 0; slot < 128; ++slot)
    {
        const auto& state = performanceStateBank[(size_t) slot];
        if (! state.populated)
            continue;

        juce::XmlElement* stateElement = performanceBankElement->createNewChildElement ("state");
        stateElement->setAttribute ("slot", slot);
        stateElement->setAttribute ("populated", true);
        stateElement->setAttribute ("trimStartSample", state.trimStartSample);
        stateElement->setAttribute ("trimEndSample", state.trimEndSample);
        stateElement->setAttribute ("style", state.style);
        stateElement->setAttribute ("loop", state.loop);
        stateElement->setAttribute ("sync", state.sync);
        stateElement->setAttribute ("parameters", joinFloatList (std::vector<float> (state.parameterValues.begin(),
                                                                                     state.parameterValues.end())));
    }

    juce::XmlElement* workingElement = root.createNewChildElement ("performanceWorkingState");
    workingElement->setAttribute ("style", performanceWorkingState.style);
    workingElement->setAttribute ("loop", performanceWorkingState.loop);
    workingElement->setAttribute ("sync", performanceWorkingState.sync);
    workingElement->setAttribute ("parameters", joinFloatList (std::vector<float> (performanceWorkingState.parameterValues.begin(),
                                                                                   performanceWorkingState.parameterValues.end())));
    workingElement->setAttribute ("focusedPerformanceStateSlot", focusedPerformanceStateSlot.load());

    juce::MemoryOutputStream out (destData, false);
    root.writeTo (out);
}

void SlicerModel::restoreState (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    const juce::String xml ((const char*) data, (size_t) sizeInBytes);
    auto root = juce::XmlDocument::parse (xml);

    if (root == nullptr || ! root->hasTagName ("neditvst"))
        return;

    const juce::ScopedLock sl (sampleLock);

    // The version attribute is deliberately ignored -- the reader below is
    // additive (unknown tags skipped) and tolerant (absent sections leave
    // current state untouched), so older saves degrade gracefully and
    // newer saves lose only the sections this build doesn't know.
    const int version = root->getIntAttribute ("version", 1); (void) version;

    // Clamps a restored cell-parameter value (continuous values to the
    // parameter's own min/max, discrete values to a rounded, in-range
    // option index) -- shared by the style globals, the performance bank
    // slots, and the working state, all of which store the same 0..18
    // index space. Indices 5 (Subdivide) and 19/20 (Volume/Volume Mode)
    // have no global dial but still valid ranges in the banks.
    auto clampCellParameter = [this] (int index, float value) -> float
    {
        if (index < 0 || index >= numSequencerCellParameters)
            return value;

        if (isSequencerCellParameterDiscrete (index))
        {
            const int numOptions = getSequencerCellParameterNumOptions (index);
            return (float) juce::jlimit (0, juce::jmax (0, numOptions - 1), juce::roundToInt (value));
        }

        return juce::jlimit (getSequencerCellParameterMin (index), getSequencerCellParameterMax (index), value);
    };

    if (auto* globals = root->getChildByName ("globals"))
    {
        if (globals->hasAttribute ("loopLengthBars"))
            loopLengthBars.store (globals->getIntAttribute ("loopLengthBars", loopLengthBars.load()));
        if (globals->hasAttribute ("trimStartSample"))
            trimStartSample.store (globals->getIntAttribute ("trimStartSample", trimStartSample.load()));
        if (globals->hasAttribute ("trimEndSample"))
            trimEndSample.store (globals->getIntAttribute ("trimEndSample", trimEndSample.load()));
        if (globals->hasAttribute ("tempoTrimStartSample"))
            tempoTrimStartSample.store (globals->getIntAttribute ("tempoTrimStartSample", tempoTrimStartSample.load()));
        if (globals->hasAttribute ("tempoTrimEndSample"))
            tempoTrimEndSample.store (globals->getIntAttribute ("tempoTrimEndSample", tempoTrimEndSample.load()));
        if (globals->hasAttribute ("manualBpmOverrideEnabled"))
            manualBpmOverrideEnabled.store (globals->getBoolAttribute ("manualBpmOverrideEnabled", manualBpmOverrideEnabled.load()));
        if (globals->hasAttribute ("manualBpmOverrideValue"))
            manualBpmOverrideValue.store (globals->getDoubleAttribute ("manualBpmOverrideValue", manualBpmOverrideValue.load()));
        if (globals->hasAttribute ("sensitivity"))
            currentSensitivity.store (globals->getDoubleAttribute ("sensitivity", currentSensitivity.load()));
        if (globals->hasAttribute ("fadeInMs"))
            fadeInMs.store (globals->getDoubleAttribute ("fadeInMs", fadeInMs.load()));
        if (globals->hasAttribute ("fadeOutMs"))
            fadeOutMs.store (globals->getDoubleAttribute ("fadeOutMs", fadeOutMs.load()));
        if (globals->hasAttribute ("quantizeTransientsEnabled"))
            quantizeTransientsEnabled.store (globals->getBoolAttribute ("quantizeTransientsEnabled", quantizeTransientsEnabled.load()));
        if (globals->hasAttribute ("quantizeGridIndex"))
            quantizeGridIndex.store (juce::jlimit (0, numNoteValueOptions - 1, globals->getIntAttribute ("quantizeGridIndex", quantizeGridIndex.load())));
        if (globals->hasAttribute ("performanceTrimSnapMode"))
            performanceTrimSnapMode.store ((TrimSnapMode) juce::jlimit ((int) TrimSnapMode::transients, (int) TrimSnapMode::grid,
                                                                         globals->getIntAttribute ("performanceTrimSnapMode", (int) performanceTrimSnapMode.load())));
        if (globals->hasAttribute ("performanceTrimGridIndex"))
            performanceTrimGridIndex.store (juce::jlimit (0, numNoteValueOptions - 1, globals->getIntAttribute ("performanceTrimGridIndex", performanceTrimGridIndex.load())));
        if (globals->hasAttribute ("triggerMode"))
            triggerMode.store ((TriggerMode) juce::jlimit ((int) TriggerMode::sliceLength, (int) TriggerMode::performance,
                                                            globals->getIntAttribute ("triggerMode", (int) triggerMode.load())));
        if (globals->hasAttribute ("clockReferenceIndex"))
            clockReferenceIndex.store (juce::jlimit (0, numNoteValueOptions - 1, globals->getIntAttribute ("clockReferenceIndex", clockReferenceIndex.load())));
        if (globals->hasAttribute ("tapeStopScope"))
            tapeStopScope.store ((TapeStopScope) juce::jlimit (0, numTapeStopScopeOptions - 1,
                                                                globals->getIntAttribute ("tapeStopScope", (int) tapeStopScope.load())));
        if (globals->hasAttribute ("filterSweepScope"))
            filterSweepScope.store ((FilterSweepScope) juce::jlimit (0, numFilterSweepScopeOptions - 1,
                                                                      globals->getIntAttribute ("filterSweepScope", (int) filterSweepScope.load())));
        if (globals->hasAttribute ("resetBarsIndex"))
            resetBarsIndex.store (juce::jlimit (0, numResetBarsOptions - 1, globals->getIntAttribute ("resetBarsIndex", resetBarsIndex.load())));
        if (globals->hasAttribute ("pitchMode"))
            pitchMode.store ((PitchMode) juce::jlimit ((int) PitchMode::repitch, (int) PitchMode::timeStretch,
                                                        globals->getIntAttribute ("pitchMode", (int) pitchMode.load())));
        if (globals->hasAttribute ("grainSizeMs"))
            grainSizeMs.store (globals->getDoubleAttribute ("grainSizeMs", grainSizeMs.load()));
        if (globals->hasAttribute ("grainWindowShape"))
            grainWindowShape.store ((GrainWindowShape) juce::jlimit ((int) GrainWindowShape::hann, (int) GrainWindowShape::triangular,
                                                                      globals->getIntAttribute ("grainWindowShape", (int) grainWindowShape.load())));
        if (globals->hasAttribute ("pitchShiftSemitones"))
            pitchShiftSemitones.store (globals->getDoubleAttribute ("pitchShiftSemitones", pitchShiftSemitones.load()));
        if (globals->hasAttribute ("beatQuantizeSliceLengthEnabled"))
            beatQuantizeSliceLengthEnabled.store (globals->getBoolAttribute ("beatQuantizeSliceLengthEnabled", beatQuantizeSliceLengthEnabled.load()));
        if (globals->hasAttribute ("beatQuantizeSliceLengthEnabledRepitch"))
            beatQuantizeSliceLengthEnabledRepitch.store (globals->getBoolAttribute ("beatQuantizeSliceLengthEnabledRepitch", beatQuantizeSliceLengthEnabledRepitch.load()));
    }

    if (auto* styleGlobals = root->getChildByName ("styleGlobals"))
    {
        struct StyleGlobalField { const char* name; int paramIndex; };
        const StyleGlobalField fields[] = {
            { "filterSweepResonance", 0 }, { "filterSweepFilterType", 1 }, { "curveShape", 2 },
            { "stretchGrainSizeMs", 3 }, { "stretchSpeedMultiplier", 4 },
            { "bitcrushRateReduction", 6 }, { "bitcrushRateReductionMode", 7 },
            { "bitcrushBitDepth", 8 }, { "bitcrushBitDepthMode", 9 },
            { "scratchRate", 10 }, { "scratchForwardCurve", 11 }, { "scratchBackwardCurve", 12 },
            { "flangerDelayTime", 13 }, { "flangerDelayTimeMode", 14 },
            { "flangerMix", 15 }, { "flangerMixMode", 16 },
            { "flangerFeedback", 17 }, { "flangerFeedbackMode", 18 }
        };

        for (const auto& field : fields)
        {
            if (! styleGlobals->hasAttribute (field.name))
                continue;

            const float value = clampCellParameter (field.paramIndex, styleGlobals->getDoubleAttribute (field.name, 0.0));
            switch (field.paramIndex)
            {
                case 0: filterSweepResonanceValue.store (value); break;
                case 1: filterSweepFilterTypeValue.store ((int) value); break;
                case 2: curveShapeValue.store ((int) value); break;
                case 3: stretchGrainSizeMsValue.store (value); break;
                case 4: stretchSpeedMultiplierValue.store (value); break;
                case 6: bitcrushRateReductionGlobalValue.store (value); break;
                case 7: bitcrushRateReductionModeGlobalValue.store ((int) value); break;
                case 8: bitcrushBitDepthGlobalValue.store (value); break;
                case 9: bitcrushBitDepthModeGlobalValue.store ((int) value); break;
                case 10: scratchRateGlobalValue.store ((int) value); break;
                case 11: scratchForwardCurveGlobalValue.store ((int) value); break;
                case 12: scratchBackwardCurveGlobalValue.store ((int) value); break;
                case 13: flangerDelayTimeGlobalValue.store (value); break;
                case 14: flangerDelayTimeModeGlobalValue.store ((int) value); break;
                case 15: flangerMixGlobalValue.store (value); break;
                case 16: flangerMixModeGlobalValue.store ((int) value); break;
                case 17: flangerFeedbackGlobalValue.store (value); break;
                case 18: flangerFeedbackModeGlobalValue.store ((int) value); break;
                default: break;
            }
        }
    }

    if (auto* probabilities = root->getChildByName ("probabilities"))
    {
        std::vector<float> restored;

        // Slice probabilities are slice-indexed -- only meaningful when the
        // stored length matches the current slice count, otherwise the table
        // would index a different sample's slices.
        parseFloatList (probabilities, "slice", restored);
        if (! restored.empty() && (int) restored.size() == (int) slices.size())
            sliceProbabilities = restored;

        parseFloatList (probabilities, "subdivision", restored);
        if ((int) restored.size() == numNoteValueOptions)
            subdivisionProbabilities = restored;

        parseFloatList (probabilities, "playbackStyle", restored);
        if ((int) restored.size() == numPlaybackStyleOptions)
            playbackStyleProbabilities = restored;
    }

    // Sequencer: indices first (they define the grid's dimensions -- but
    // stored directly, NOT via the clamps-reset-grid setters, so the grid
    // being restored below isn't wiped), then the grid/overrides, each
    // size-guarded against the resulting dimensions.
    if (auto* sequencer = root->getChildByName ("sequencer"))
    {
        if (sequencer->hasAttribute ("stepResolutionIndex"))
            stepResolutionIndex.store (juce::jlimit (0, numNoteValueOptions - 1, sequencer->getIntAttribute ("stepResolutionIndex", stepResolutionIndex.load())));
        if (sequencer->hasAttribute ("patternLengthBarsIndex"))
            patternLengthBarsIndex.store (juce::jlimit (0, numPatternLengthBarsOptions - 1, sequencer->getIntAttribute ("patternLengthBarsIndex", patternLengthBarsIndex.load())));
        if (sequencer->hasAttribute ("selectedDrawingStyle"))
            selectedDrawingStyle.store (juce::jlimit (0, numPlaybackStyleOptions - 1, sequencer->getIntAttribute ("selectedDrawingStyle", selectedDrawingStyle.load())));

        const size_t expectedGridSize = (size_t) juce::jmax (0, getSequencerNumRows() * getSequencerNumSteps());

        std::vector<int> restoredGrid;
        parseIntList (sequencer, "grid", restoredGrid);
        const bool gridMatches = (restoredGrid.size() == expectedGridSize);
        if (gridMatches)
            sequencerGrid = restoredGrid;

        // The overrides/extended lengths index the STORED grid; when that
        // grid was rejected (size mismatch against the current slices) they
        // would describe a different layout, so they are skipped as a set.
        sequencerCellParameterOverrides.clear();
        sequencerCellExtendedLengthSteps.clear();

        if (gridMatches)
        {
            for (auto* cellElement : sequencer->getChildWithTagNameIterator ("override"))
            {
                const int flatIndex = cellElement->getIntAttribute ("index", -1);
                if (flatIndex < 0 || (size_t) flatIndex >= restoredGrid.size())
                    continue;

                for (auto* paramElement : cellElement->getChildWithTagNameIterator ("param"))
                {
                    const juce::String name = paramElement->getStringAttribute ("name");
                    const float value = paramElement->getDoubleAttribute ("value", 0.0);
                    if (name.isNotEmpty())
                        sequencerCellParameterOverrides[flatIndex][name] = value;
                }
            }

            for (auto* cellElement : sequencer->getChildWithTagNameIterator ("extendedLength"))
            {
                const int flatIndex = cellElement->getIntAttribute ("index", -1);
                if (flatIndex < 0 || (size_t) flatIndex >= restoredGrid.size())
                    continue;

                sequencerCellExtendedLengthSteps[flatIndex] = cellElement->getIntAttribute ("steps", 0);
            }
        }
    }

    if (auto* recall = root->getChildByName ("recall"))
    {
        if (recall->hasAttribute ("patternSwitchTiming"))
            patternSwitchTiming.store ((PatternSwitchTiming) juce::jlimit ((int) PatternSwitchTiming::immediate, (int) PatternSwitchTiming::endOfPattern,
                                                                            recall->getIntAttribute ("patternSwitchTiming", (int) patternSwitchTiming.load())));
        if (recall->hasAttribute ("patternSwitchIntervalIndex"))
            patternSwitchIntervalIndex.store (juce::jlimit (0, numNoteValueOptions - 1, recall->getIntAttribute ("patternSwitchIntervalIndex", patternSwitchIntervalIndex.load())));
        if (recall->hasAttribute ("performanceQuantizeRecallEnabled"))
            performanceQuantizeRecallEnabled.store (recall->getBoolAttribute ("performanceQuantizeRecallEnabled", performanceQuantizeRecallEnabled.load()));
        if (recall->hasAttribute ("performanceQuantizeRecallIntervalIndex"))
            performanceQuantizeRecallIntervalIndex.store (juce::jlimit (0, numNoteValueOptions - 1, recall->getIntAttribute ("performanceQuantizeRecallIntervalIndex", performanceQuantizeRecallIntervalIndex.load())));
    }

    if (auto* patternBankElement = root->getChildByName ("patternBank"))
    {
        for (auto* patternElement : patternBankElement->getChildWithTagNameIterator ("pattern"))
        {
            const int slot = patternElement->getIntAttribute ("slot", -1);
            if (slot < 0 || slot >= 128)
                continue;

            const int rows = juce::jlimit (0, numSequencerRows, patternElement->getIntAttribute ("rows", 0));
            const int columns = juce::jlimit (0, maxSequencerColumns, patternElement->getIntAttribute ("columns", 0));

            std::vector<int> grid;
            parseIntList (patternElement, "grid", grid);

            // Self-consistency guard: the engine indexes a recalled pattern
            // by its own rows*columns; a mismatched grid would go out of
            // bounds, so a corrupt/mismatched slot is skipped entirely.
            if ((int) grid.size() != rows * columns)
                continue;

            SequencerPatternSnapshot& restored = patternBank[(size_t) slot];
            restored.populated = true;
            restored.rows = rows;
            restored.columns = columns;
            restored.stepResolutionIndex = juce::jlimit (0, numNoteValueOptions - 1, patternElement->getIntAttribute ("stepResolutionIndex", 0));
            restored.patternLengthBarsIndex = juce::jlimit (0, numPatternLengthBarsOptions - 1, patternElement->getIntAttribute ("patternLengthBarsIndex", 0));
            restored.grid = grid;
            restored.parameterOverrides.clear();
            restored.extendedLengthSteps.clear();

            for (auto* cellElement : patternElement->getChildWithTagNameIterator ("override"))
            {
                const int flatIndex = cellElement->getIntAttribute ("index", -1);
                if (flatIndex < 0 || (size_t) flatIndex >= restored.grid.size())
                    continue;

                for (auto* paramElement : cellElement->getChildWithTagNameIterator ("param"))
                {
                    const juce::String name = paramElement->getStringAttribute ("name");
                    const float value = paramElement->getDoubleAttribute ("value", 0.0);
                    if (name.isNotEmpty())
                        restored.parameterOverrides[flatIndex][name] = value;
                }
            }

            for (auto* cellElement : patternElement->getChildWithTagNameIterator ("extendedLength"))
            {
                const int flatIndex = cellElement->getIntAttribute ("index", -1);
                if (flatIndex < 0 || (size_t) flatIndex >= restored.grid.size())
                    continue;

                restored.extendedLengthSteps[flatIndex] = cellElement->getIntAttribute ("steps", 0);
            }
        }
    }

    if (auto* performanceBankElement = root->getChildByName ("performanceBank"))
    {
        for (auto* stateElement : performanceBankElement->getChildWithTagNameIterator ("state"))
        {
            const int slot = stateElement->getIntAttribute ("slot", -1);
            if (slot < 0 || slot >= 128)
                continue;

            PerformanceStateSnapshot& restored = performanceStateBank[(size_t) slot];
            restored.populated = true;
            restored.trimStartSample = stateElement->getIntAttribute ("trimStartSample", 0);
            restored.trimEndSample = stateElement->getIntAttribute ("trimEndSample", 0);
            restored.style = juce::jlimit (0, numPlaybackStyleOptions - 1, stateElement->getIntAttribute ("style", 0));
            restored.loop = stateElement->getBoolAttribute ("loop", false);
            restored.sync = stateElement->getBoolAttribute ("sync", true);

            std::vector<float> restoredParams;
            parseFloatList (stateElement, "parameters", restoredParams);
            for (int i = 0; i < numSequencerCellParameters; ++i)
                restored.parameterValues[(size_t) i] = (i < (int) restoredParams.size())
                    ? clampCellParameter (i, restoredParams[(size_t) i])
                    : getSequencerCellParameterGlobalValue (i);
        }
    }

    if (auto* workingElement = root->getChildByName ("performanceWorkingState"))
    {
        performanceWorkingState.style = juce::jlimit (0, numPlaybackStyleOptions - 1, workingElement->getIntAttribute ("style", performanceWorkingState.style));
        performanceWorkingState.loop = workingElement->getBoolAttribute ("loop", performanceWorkingState.loop);
        performanceWorkingState.sync = workingElement->getBoolAttribute ("sync", performanceWorkingState.sync);

        std::vector<float> restoredParams;
        parseFloatList (workingElement, "parameters", restoredParams);
        for (int i = 0; i < numSequencerCellParameters; ++i)
            performanceWorkingState.parameterValues[(size_t) i] = (i < (int) restoredParams.size())
                ? clampCellParameter (i, restoredParams[(size_t) i])
                : getSequencerCellParameterGlobalValue (i);

        if (workingElement->hasAttribute ("focusedPerformanceStateSlot"))
            focusedPerformanceStateSlot.store (juce::jlimit (-1, 127, workingElement->getIntAttribute ("focusedPerformanceStateSlot", -1)));
    }

    onPickStateInvalidated(); // restored structure/parameters -- the engine must force a fresh pick
}

//==============================================================================
// Audio-thread data path (engine support).

void SlicerModel::loadSample (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
        return;

    juce::AudioBuffer<float> newBuffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&newBuffer, 0, (int) reader->lengthInSamples, 0, true, true);

    {
        const juce::ScopedLock sl (sampleLock);
        sampleBuffer = std::move (newBuffer);
        sampleSampleRate = reader->sampleRate;
        sampleLoaded = true;
        loadedFileName = file.getFileName();

        transientDetector.analyze (sampleBuffer, sampleSampleRate);

        // Trim markers (Step 23): default to the full sample length, so
        // behaviour is unchanged until the user actually drags a handle.
        trimStartSample.store (0);
        trimEndSample.store (sampleBuffer.getNumSamples());
        tempoTrimStartSample.store (0);
        tempoTrimEndSample.store (sampleBuffer.getNumSamples());

        manualPoints.clear(); // positions from the old sample don't mean anything here
        excludedPoints.clear();
        onPickStateInvalidated(); // engine per-pick state must not read past the new buffer's end
        auditionActive.store (false); // Step 25 -- a stale audition loop from the old buffer/trim makes no sense against the new one
        auditionPlaybackPositionForUI.store (-1); // Step 28 -- and neither does its stale playhead indicator
    }

    undoManager.clearUndoHistory(); // old undo steps reference positions in a different file now

    redetectSlices (defaultSensitivity, computeMinimumHoldoffMs());
}

void SlicerModel::redetectSlices (float sensitivity, float holdoffMs)
{
    rebuildSlicesFromDetectionAndManualPoints (sensitivity, holdoffMs);
}

double SlicerModel::computeSourceSpanSeconds() const
{
    if (manualBpmOverrideEnabled.load())
    {
        const double bpm = manualBpmOverrideValue.load();

        if (bpm <= 0.0)
            return 0.0;

        const double beats = (double) loopLengthBars.load() * 4.0; // assumes 4/4, same as elsewhere
        return (beats * 60.0) / bpm;
    }

    const int trimStart = tempoTrimStartSample.load();
    const int trimEnd = tempoTrimEndSample.load();
    const int spanSamples = juce::jmax (0, trimEnd - trimStart);
    return (double) spanSamples / sampleSampleRate;
}

void SlicerModel::rebuildSlicesFromDetectionAndManualPoints (float sensitivity, float holdoffMs)
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    auto autoSlices = transientDetector.detectSlices (sensitivity, holdoffMs, trimStart, trimEnd);

    const juce::ScopedLock sl (sampleLock);

    slices = mergeOnsetsIntoSlices (autoSlices, trimStart, trimEnd);
    sliceProbabilities.assign (slices.size(), 1.0f); // default: even odds across all slices
    resetSequencerGrid(); // Step 37 -- row count just changed
    onPickStateInvalidated(); // boundaries changed -- the engine must force a fresh pick
}

std::vector<Slice> SlicerModel::previewSlicesAtSensitivity (float sensitivity) const
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    auto autoSlices = transientDetector.detectSlices (sensitivity, computeMinimumHoldoffMs(), trimStart, trimEnd);

    const juce::ScopedLock sl (sampleLock);
    return mergeOnsetsIntoSlices (autoSlices, trimStart, trimEnd);
}

std::vector<Slice> SlicerModel::mergeOnsetsIntoSlices (const std::vector<Slice>& autoSlices, int trimStart, int trimEnd) const
{
    const int matchToleranceSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);

    std::vector<int> onsets;
    onsets.reserve (autoSlices.size() + manualPoints.size());

    for (const auto& s : autoSlices)
    {
        if (s.startSample == trimStart)
        {
            onsets.push_back (trimStart); // the trim start is never excludable
            continue;
        }

        bool excluded = false;

        for (const auto& ep : excludedPoints)
        {
            if (std::abs (s.startSample - ep.samplePosition) <= matchToleranceSamples)
            {
                excluded = true;
                break;
            }
        }

        if (! excluded)
        {
            // Quantize detected transients to grid (Step 35) -- applied
            // AFTER exclusion matching, against the ORIGINAL (unquantized)
            // position: an exclusion click targets the raw detected peak
            // the user actually saw on the waveform, and matching against
            // that first is what keeps an exclusion correct regardless of
            // whether quantization is on. Manual points below are never
            // touched by this -- they're pushed as-is, further down.
            const int onsetSample = quantizeTransientsEnabled.load()
                ? quantizeOnsetToGrid (s.startSample, trimStart, trimEnd)
                : s.startSample;
            onsets.push_back (onsetSample);
        }
    }

    // Manual points outside the current trim range are filtered out here
    // (not deleted from manualPoints itself) -- the same "soft exclude"
    // widening the trim back out later can undo, matching excludedPoints'
    // own semantics just above.
    for (const auto& mp : manualPoints)
        if (mp.samplePosition > trimStart && mp.samplePosition < trimEnd)
            onsets.push_back (mp.samplePosition);

    if (onsets.empty() || onsets.front() != trimStart)
        onsets.insert (onsets.begin(), trimStart);

    std::sort (onsets.begin(), onsets.end());
    onsets.erase (std::unique (onsets.begin(), onsets.end()), onsets.end());

    std::vector<Slice> result;

    for (size_t i = 0; i < onsets.size(); ++i)
    {
        Slice slice;
        slice.startSample = onsets[i];
        slice.endSample = (i + 1 < onsets.size()) ? onsets[i + 1] : trimEnd;

        if (slice.lengthInSamples() > 0)
            result.push_back (slice);
    }

    return result;
}

int SlicerModel::quantizeOnsetToGrid (int onsetSample, int trimStart, int trimEnd) const
{
    const double gridBeats = getNoteValueBeats (quantizeGridIndex.load());

    if (gridBeats <= 0.0)
        return onsetSample;

    // Same source-tempo derivation used everywhere else in this class
    // (computeBeatQuantizeTarget's naturalBeats calculation for the
    // analogous per-pick feature works the same way): originalBpm/60
    // converts a duration in seconds directly to beats, so there's no
    // need to separately compute a "seconds per beat" intermediate.
    const double originalBpm = getCalculatedOriginalBpm();

    if (originalBpm <= 0.0 || sampleSampleRate <= 0.0)
        return onsetSample;

    const double onsetSeconds = (double) (onsetSample - trimStart) / sampleSampleRate;
    const double onsetBeats = onsetSeconds * (originalBpm / 60.0);
    const double nearestGridStep = std::round (onsetBeats / gridBeats);
    const double quantizedBeats = nearestGridStep * gridBeats;
    const double quantizedSeconds = quantizedBeats * (60.0 / originalBpm);
    const double quantizedSampleDouble = (double) trimStart + quantizedSeconds * sampleSampleRate;

    // Clamped so an onset near either edge of the trim can never quantize
    // to a position outside it -- trimEnd - 1 mirrors the same upper bound
    // addManualSlicePoint()/setTrimStartSample() etc. already use for
    // exactly this reason.
    return juce::jlimit (trimStart, juce::jmax (trimStart, trimEnd - 1), (int) std::llround (quantizedSampleDouble));
}

int SlicerModel::findNearestGridSample (int rawSample) const
{
    const double gridBeats = getNoteValueBeats (performanceTrimGridIndex.load());

    if (gridBeats <= 0.0)
        return rawSample;

    const double originalBpm = getCalculatedOriginalBpm();

    if (originalBpm <= 0.0 || sampleSampleRate <= 0.0)
        return rawSample;

    // Same beats<->samples round-trip quantizeOnsetToGrid() above uses,
    // anchored at tempoTrimStartSample instead of a passed-in trim start --
    // see this function's own comment for why.
    const int anchor = tempoTrimStartSample.load();
    const double offsetSeconds = (double) (rawSample - anchor) / sampleSampleRate;
    const double offsetBeats = offsetSeconds * (originalBpm / 60.0);
    const double nearestGridStep = std::round (offsetBeats / gridBeats);
    const double quantizedBeats = nearestGridStep * gridBeats;
    const double quantizedSeconds = quantizedBeats * (60.0 / originalBpm);
    const double quantizedSampleDouble = (double) anchor + quantizedSeconds * sampleSampleRate;

    return (int) std::llround (quantizedSampleDouble);
}

void SlicerModel::resetSequencerGrid()
{
    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();
    sequencerGrid.assign ((size_t) juce::jmax (0, rows * columns), -1);
    sequencerCellParameterOverrides.clear(); // Step 45 -- dimensions just changed, old flat indices are meaningless now
    sequencerCellExtendedLengthSteps.clear(); // Step-extension (Pass 1) -- same reasoning
}

//==============================================================================
// Sequencer grid editing.

int SlicerModel::getSequencerCellStyle (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return -1;

    const size_t idx = (size_t) (row * columns + column);
    return idx < sequencerGrid.size() ? sequencerGrid[idx] : -1;
}

void SlicerModel::setSequencerCell (int row, int column, int style)
{
    const juce::ScopedLock sl (sampleLock);

    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= rows || column < 0 || column >= columns)
        return;

    if ((int) sequencerGrid.size() != rows * columns)
        resetSequencerGrid(); // defensive -- dimensions drifted out from under us somehow

    if (style >= 0)
    {
        // Structural monophony (Step 37, v1): clear any other active cell
        // in this SAME COLUMN across every row first, so "only one voice"
        // is true at the INPUT level the instant a pattern is drawn, not
        // just something the playback engine happens to enforce
        // afterward -- and it's what avoids needing a tie-break rule
        // entirely at playback time. Their parameter overrides go with
        // them (Step 45) -- a cell that's no longer active has no
        // meaningful style-specific parameters left to override.
        for (int r = 0; r < rows; ++r)
        {
            sequencerGrid[(size_t) (r * columns + column)] = -1;
            sequencerCellParameterOverrides.erase (r * columns + column);
            sequencerCellExtendedLengthSteps.erase (r * columns + column); // Step-extension (Pass 1) -- a cleared cell's own extension goes with it too
        }
    }

    // This cell's own style is changing (or clearing) too (Step 45) --
    // always drop its overrides first, so re-painting the same physical
    // cell later never resurrects a stale override left over from a
    // completely different style.
    sequencerCellParameterOverrides.erase (row * columns + column);
    sequencerCellExtendedLengthSteps.erase (row * columns + column); // Step-extension (Pass 1) -- same reasoning
    sequencerGrid[(size_t) (row * columns + column)] = style;
}

int SlicerModel::getSequencerCellExtendedLengthSteps (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return 0;

    const auto it = sequencerCellExtendedLengthSteps.find (row * columns + column);
    return it != sequencerCellExtendedLengthSteps.end() ? it->second : 0;
}

void SlicerModel::setSequencerCellExtendedLengthSteps (int row, int column, int lengthSteps)
{
    const juce::ScopedLock sl (sampleLock);

    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= rows || column < 0 || column >= columns)
        return;

    if ((int) sequencerGrid.size() != rows * columns)
        resetSequencerGrid(); // defensive -- dimensions drifted out from under us somehow, mirrors setSequencerCell()

    const int idx = row * columns + column;

    if (sequencerGrid[(size_t) idx] < 0)
        return; // only an already-active cell can be extended

    const int clampedLength = juce::jlimit (1, columns - column, lengthSteps);

    // Growing into columns another row already occupies clears those
    // conflicting cells -- the exact same per-column monophony rule
    // setSequencerCell() enforces above for a plain single-cell draw, just
    // applied across the whole newly-claimed span instead of one column.
    // This row's OWN later cells (if any) are deliberately left alone --
    // only ANOTHER row's occupancy counts as a conflict here.
    for (int offset = 1; offset < clampedLength; ++offset)
    {
        const int col = column + offset;

        for (int r = 0; r < rows; ++r)
        {
            if (r == row)
                continue;

            const int otherIdx = r * columns + col;

            if (sequencerGrid[(size_t) otherIdx] >= 0)
            {
                sequencerGrid[(size_t) otherIdx] = -1;
                sequencerCellParameterOverrides.erase (otherIdx);
                sequencerCellExtendedLengthSteps.erase (otherIdx);
            }
        }
    }

    sequencerCellExtendedLengthSteps[idx] = clampedLength;
}

int SlicerModel::getSequencerNaturalLengthSteps (int row) const
{
    const juce::ScopedLock sl (sampleLock);

    if (row < 0 || row >= (int) slices.size())
        return 1;

    const auto& slice = slices[(size_t) row];
    const int sliceLength = slice.endSample - slice.startSample;
    const double originalBpm = getCalculatedOriginalBpm();

    int naturalSteps = 1;

    if (sliceLength > 0 && sampleSampleRate > 0.0 && originalBpm > 0.0)
    {
        const double sliceSeconds = (double) sliceLength / sampleSampleRate;
        const double naturalBeats = sliceSeconds * (originalBpm / 60.0);
        const double stepBeats = getNoteValueBeats (stepResolutionIndex.load());

        if (stepBeats > 0.0)
            naturalSteps = juce::jmax (1, juce::roundToInt (naturalBeats / stepBeats));
    }

    return naturalSteps;
}

int SlicerModel::getSequencerCellDeclaredLengthSteps (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return 1;

    const int naturalSteps = getSequencerNaturalLengthSteps (row);
    const int extendedOverride = getSequencerCellExtendedLengthSteps (row, column);

    return juce::jmax (naturalSteps, extendedOverride);
}

void SlicerModel::clearSequence()
{
    const juce::ScopedLock sl (sampleLock);

    const int rows = getSequencerNumRows();
    const int columns = getSequencerNumSteps();

    if ((int) sequencerGrid.size() != rows * columns)
        resetSequencerGrid();

    std::fill (sequencerGrid.begin(), sequencerGrid.end(), -1);
    sequencerCellParameterOverrides.clear(); // Step 45
    sequencerCellExtendedLengthSteps.clear(); // Step-extension (Pass 1)
}

bool SlicerModel::getSequencerCellHasParameterOverride (int row, int column, const juce::String& parameterName) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return false;

    const auto it = sequencerCellParameterOverrides.find (row * columns + column);
    return it != sequencerCellParameterOverrides.end() && it->second.count (parameterName) > 0;
}

float SlicerModel::getSequencerCellParameterOverride (int row, int column, const juce::String& parameterName, float fallbackValue) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return fallbackValue;

    const auto it = sequencerCellParameterOverrides.find (row * columns + column);

    if (it == sequencerCellParameterOverrides.end())
        return fallbackValue;

    const auto valueIt = it->second.find (parameterName);
    return valueIt != it->second.end() ? valueIt->second : fallbackValue;
}

void SlicerModel::setSequencerCellParameterOverride (int row, int column, const juce::String& parameterName, float value)
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return;

    sequencerCellParameterOverrides[row * columns + column][parameterName] = value;
}

bool SlicerModel::getSequencerCellHasAnyParameterOverride (int row, int column) const
{
    const juce::ScopedLock sl (sampleLock);

    const int columns = getSequencerNumSteps();

    if (row < 0 || row >= getSequencerNumRows() || column < 0 || column >= columns)
        return false;

    const auto it = sequencerCellParameterOverrides.find (row * columns + column);
    return it != sequencerCellParameterOverrides.end() && ! it->second.empty();
}

//==============================================================================
// Sequencer pattern bank (MIDI Learn save / capture).

SlicerModel::SequencerPatternSnapshot SlicerModel::captureCurrentSequencerPatternSnapshot() const
{
    SequencerPatternSnapshot snapshot;
    snapshot.populated = true;
    snapshot.rows = getSequencerNumRows();
    snapshot.columns = getSequencerNumSteps();
    snapshot.stepResolutionIndex = stepResolutionIndex.load();
    snapshot.patternLengthBarsIndex = patternLengthBarsIndex.load();
    snapshot.grid = sequencerGrid;
    snapshot.parameterOverrides = sequencerCellParameterOverrides;
    snapshot.extendedLengthSteps = sequencerCellExtendedLengthSteps;
    return snapshot;
}

void SlicerModel::completeMidiLearn (int noteNumber)
{
    patternBank[(size_t) noteNumber] = pendingSaveSnapshot;
    midiLearnArmed.store (false);
}

void SlicerModel::armMidiLearnForPatternSave()
{
    const juce::ScopedLock sl (sampleLock);
    pendingSaveSnapshot = captureCurrentSequencerPatternSnapshot();
    midiLearnArmed.store (true);
}

void SlicerModel::cancelMidiLearn()
{
    midiLearnArmed.store (false);
}

std::array<bool, 128> SlicerModel::getPopulatedPatternBankSlots() const
{
    const juce::ScopedLock sl (sampleLock);
    std::array<bool, 128> populated {};

    for (size_t i = 0; i < patternBank.size(); ++i)
        populated[i] = patternBank[i].populated;

    return populated;
}

//==============================================================================
// Performance mode state bank (click-to-focus + auto-save).

void SlicerModel::setFocusedPerformanceStateSlot (int noteNumber)
{
    if (noteNumber < 0 || noteNumber >= 128)
        return;

    const juce::ScopedLock sl (sampleLock);

    const int previous = focusedPerformanceStateSlot.load();

    if (noteNumber == previous)
        return; // already focused -- nothing to save away from or load

    // Auto-save (replaces the old MIDI-Learn "Save to..." button entirely):
    // whatever was being edited in the previously-focused slot is captured
    // now, the instant focus moves away from it.
    if (previous >= 0)
    {
        PerformanceStateSnapshot saved = performanceWorkingState;
        saved.populated = true;
        saved.trimStartSample = trimStartSample.load();
        saved.trimEndSample = trimEndSample.load();
        performanceStateBank[(size_t) previous] = saved;
    }

    // Load the new slot: its existing saved state, or a fresh default to
    // start editing from if it has none yet (matching the Artillery 2
    // reference) -- the same seed values the constructor gives
    // performanceWorkingState initially. Deliberately does NOT write this
    // default into performanceStateBank[noteNumber] -- it only becomes a
    // real saved slot (and lights up the keyboard's populated highlight)
    // once focus actually moves away from it, same as every other slot.
    const auto& existing = performanceStateBank[(size_t) noteNumber];

    if (existing.populated)
    {
        performanceWorkingState = existing;
        trimStartSample.store (existing.trimStartSample);
        trimEndSample.store (existing.trimEndSample);
    }
    else
    {
        performanceWorkingState = PerformanceStateSnapshot {};
        performanceWorkingState.populated = true; // lets this slot's own key live-audition immediately, even before its first auto-save

        for (int i = 0; i < numSequencerCellParameters; ++i)
            performanceWorkingState.parameterValues[(size_t) i] = getSequencerCellParameterGlobalValue (i);

        trimStartSample.store (0);
        trimEndSample.store (sampleBuffer.getNumSamples());
    }

    focusedPerformanceStateSlot.store (noteNumber);
}

std::array<bool, 128> SlicerModel::getPopulatedPerformanceStateBankSlots() const
{
    const juce::ScopedLock sl (sampleLock);
    std::array<bool, 128> populated {};

    for (size_t i = 0; i < performanceStateBank.size(); ++i)
        populated[i] = performanceStateBank[i].populated;

    return populated;
}

int SlicerModel::getPerformanceWorkingStyle() const
{
    const juce::ScopedLock sl (sampleLock);
    return performanceWorkingState.style;
}

void SlicerModel::setPerformanceWorkingStyle (int style)
{
    const juce::ScopedLock sl (sampleLock);
    performanceWorkingState.style = style;
}

float SlicerModel::getPerformanceWorkingParameterValue (int index) const
{
    const juce::ScopedLock sl (sampleLock);

    if (index < 0 || index >= numSequencerCellParameters)
        return 0.0f;

    return performanceWorkingState.parameterValues[(size_t) index];
}

void SlicerModel::setPerformanceWorkingParameterValue (int index, float value)
{
    const juce::ScopedLock sl (sampleLock);

    if (index < 0 || index >= numSequencerCellParameters)
        return;

    performanceWorkingState.parameterValues[(size_t) index] = value;
}

bool SlicerModel::getPerformanceWorkingLoop() const
{
    const juce::ScopedLock sl (sampleLock);
    return performanceWorkingState.loop;
}

void SlicerModel::setPerformanceWorkingLoop (bool loop)
{
    const juce::ScopedLock sl (sampleLock);
    performanceWorkingState.loop = loop;
}

bool SlicerModel::getPerformanceWorkingSync() const
{
    const juce::ScopedLock sl (sampleLock);
    return performanceWorkingState.sync;
}

void SlicerModel::setPerformanceWorkingSync (bool sync)
{
    const juce::ScopedLock sl (sampleLock);
    performanceWorkingState.sync = sync;
}

//==============================================================================
// Manual slice points (Step 10/11/12).

int SlicerModel::addManualSlicePoint (int targetSample, bool snapToTransient)
{
    // JUCE 8's UndoManager groups every perform() without an intervening
    // beginNewTransaction() into a single undo step -- one transaction per
    // edit action is what the class docs promise, so each public edit method
    // opens its own transaction before performing.
    undoManager.beginNewTransaction();

    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    int snapped = juce::jlimit (trimStart, juce::jmax (trimStart, trimEnd - 1), targetSample);

    if (snapToTransient)
    {
        const int radiusSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);
        snapped = transientDetector.findNearestPeak (snapped, radiusSamples, trimStart, trimEnd);
    }

    const int id = nextManualPointId++;

    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    auto afterManual = beforeManual;
    afterManual.push_back ({ id, snapped });

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded,
                                                        afterManual, beforeExcluded));
    return id;
}

void SlicerModel::moveManualSlicePoint (int id, int targetSample, bool snapToTransient)
{
    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();
    int snapped = juce::jlimit (trimStart, juce::jmax (trimStart, trimEnd - 1), targetSample);

    if (snapToTransient)
    {
        const int radiusSamples = (int) (manualSnapRadiusMs / 1000.0f * (float) sampleSampleRate);
        snapped = transientDetector.findNearestPeak (snapped, radiusSamples, trimStart, trimEnd);
    }

    {
        const juce::ScopedLock sl (sampleLock);

        for (auto& mp : manualPoints)
        {
            if (mp.id == id)
            {
                mp.samplePosition = snapped;
                break;
            }
        }
    }

    rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), computeMinimumHoldoffMs());
}

void SlicerModel::commitManualPointMove (int id, int originalSamplePosition)
{
    // The live position is already applied (moveManualSlicePoint was
    // called throughout the drag) -- "after" is just the current state.
    // "before" is that same state with only this one point's position
    // put back to where the drag started.
    undoManager.beginNewTransaction(); // the whole drag is one undo step

    auto afterManual = getManualSlicePoints();
    auto beforeManual = afterManual;

    for (auto& mp : beforeManual)
        if (mp.id == id)
            mp.samplePosition = originalSamplePosition;

    auto excluded = getExcludedPoints(); // unaffected by a move

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, excluded, afterManual, excluded));
}

void SlicerModel::removeManualSlicePoint (int id)
{
    undoManager.beginNewTransaction(); // one undo step per removal (see addManualSlicePoint)

    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    auto afterManual = beforeManual;
    afterManual.erase (std::remove_if (afterManual.begin(), afterManual.end(),
                                        [id] (const ManualPointInfo& mp) { return mp.id == id; }),
                        afterManual.end());

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded,
                                                        afterManual, beforeExcluded));
}

int SlicerModel::excludeNearestAutoPoint (int targetSample)
{
    undoManager.beginNewTransaction(); // one undo step per exclusion (see addManualSlicePoint)

    const int trimStart = trimStartSample.load();
    const int trimEnd = trimEndSample.load();

    // Search the raw current auto-detection result (not the merged
    // `slices`) for the nearest boundary to targetSample -- the trim start
    // is never a candidate, it can't be excluded.
    auto autoSlices = transientDetector.detectSlices (currentSensitivity.load(), computeMinimumHoldoffMs(), trimStart, trimEnd);

    int nearest = -1;
    int bestDistance = std::numeric_limits<int>::max();

    for (const auto& s : autoSlices)
    {
        if (s.startSample == trimStart)
            continue;

        const int distance = std::abs (s.startSample - targetSample);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            nearest = s.startSample;
        }
    }

    if (nearest < 0)
        return -1;

    const int id = nextExcludedPointId++;

    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    auto afterExcluded = beforeExcluded;
    afterExcluded.push_back ({ id, nearest });

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded,
                                                        beforeManual, afterExcluded));
    return id;
}

void SlicerModel::restoreExcludedPoint (int id)
{
    undoManager.beginNewTransaction(); // one undo step per restore (see addManualSlicePoint)

    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    auto afterExcluded = beforeExcluded;
    afterExcluded.erase (std::remove_if (afterExcluded.begin(), afterExcluded.end(),
                                          [id] (const ManualPointInfo& ep) { return ep.id == id; }),
                          afterExcluded.end());

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded,
                                                        beforeManual, afterExcluded));
}

void SlicerModel::resetAllManualEdits()
{
    undoManager.beginNewTransaction(); // one undo step for the whole reset (see addManualSlicePoint)

    auto beforeManual = getManualSlicePoints();
    auto beforeExcluded = getExcludedPoints();

    if (beforeManual.empty() && beforeExcluded.empty())
        return; // nothing to reset -- don't pollute undo history with a no-op

    undoManager.perform (new SliceEditUndoableAction (*this, beforeManual, beforeExcluded, {}, {}));
}

void SlicerModel::applyManualState (const std::vector<ManualPointInfo>& manual,
                                    const std::vector<ManualPointInfo>& excluded)
{
    {
        const juce::ScopedLock sl (sampleLock);

        manualPoints.clear();
        for (const auto& m : manual)
            manualPoints.push_back ({ m.id, m.samplePosition });

        excludedPoints.clear();
        for (const auto& e : excluded)
            excludedPoints.push_back ({ e.id, e.samplePosition });
    }

    rebuildSlicesFromDetectionAndManualPoints (currentSensitivity.load(), computeMinimumHoldoffMs());
}
