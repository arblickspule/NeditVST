# Next Steps

Findings from a full-codebase review (three parallel sweeps of every file in
`Source/`, `tests/`, and the docs) against the target architecture
`Engine <- Model <-> UI` described in AGENTS.md.

Legend: [LAYERING] [DEAD CODE] [STALE COMMENT] [OTHER]

---

## 1. Architecture / layering

The three-layer split is real (model owns shared state, engine owns DSP,
processor is a 66-line shell), but the boundary is blurred in places.

### 1.1 Engine real-time state parked in the model [LAYERING]

`SlicerModel.h:1028-1032` — `auditionPosition` is the engine's read cursor,
mutated every sample in `SlicerEngine::renderAudition()` (SlicerEngine.cpp:2660).
Per the architecture this is per-pick/per-window scheduling state and belongs
on the engine. It lives in the model because `setAuditionActive()` touches it.

`SlicerModel.h:1065, 1069, 1078` — `activePatternBankSlot`, `pendingPatternSwitchNote`,
`pendingPerformanceRecallNote` are armed/consumed by the engine's per-sample
boundary checks (`SlicerEngine::handlePatternSwitchNoteOn`,
`handlePerformanceStateNoteOn`, `processBlock()`). Deferred-recall scheduling
state that sits in the model only so the model's setters can clear it.

`SlicerModel.h:1112-1119` — the three `debugFocusChange*` atomics exist solely to
feed `SlicerEngine::FreezeWatchdog`. Debug/diagnostic state for an engine
member is model-owned.

**Proposal:** migrate these five state groups to `SlicerEngine`, expose the
model setters' side-effects (clearing pending switches) through an engine API
that the model's setters call. Medium-to-high regression risk on the RT path —
wants the full test suite plus a manual performance-mode soak after.

### 1.2 `processBlock()` god-function [LAYERING]

SlicerEngine.cpp:251-2619 — ~2,370 lines. The class split happened but a
god-*function* survives, with three near-identical ~100-line pick-start blocks
(Clock / Sequenced / Slice-Length) capturing the same `currentPick*` set per
style. Factoring the three pick-start paths into one parameterised routine is
the single biggest readability win available, and the safest one in this
section (pure RT code, no state moves).

### 1.3 Model → engine callback [LAYERING]

`SlicerModel.h:1121-1122` — `onPickStateInvalidated` is the one point the model
"knows about" engine lifecycle. Documented as the deliberate exception, but the
class doc describing it is stale (see 3.1).

### 1.4 State serialization is an empty stub [LAYERING]

`PluginProcessor.cpp:52-60` — `getStateInformation`/`setStateInformation` are
empty while the model owns ~30 stored parameters (probability tables, pattern
bank, performance bank, globals) and the full schema tables
(`SlicerModel.cpp:183-460`). The persistence layer simply was never built.
The TODO's premise ("once there are parameters worth persisting") is stale —
see 3.14.

**DONE** — decision doc `docs/state-serialization-decision.md` (XML for the
dev stage; scope = all settings + banks, no audio); `SlicerModel::saveState`/
`restoreState` implemented; processor delegates to the model. Follow-up (NOT
in scope of the batch): the editor is push-based with no ChangeListener, so
a host-initiated `setStateInformation` (preset change while the editor is
open) won't refresh any controls — needs a "refresh all controls" hook.

### 1.5 Processor no-op [DEAD CODE]

`PluginProcessor.cpp:9-13` — `model.cancelMidiLearn()` in the constructor is a
guaranteed no-op (`midiLearnArmed` defaults false and nothing can arm it
before construction). Delete the call.

---

## 2. Dead code

- `SlicerEngine.h:189-195` — `resetSequencerGrid()` private forwarder, never
  called (the engine calls `model.resetSequencerGrid()` directly).
- `SlicerEngine.h:294` — `computeSourceSpanSeconds()` private forwarder, never
  called.
- `SlicerEngine.h:197-200` — `SequencerPatternSnapshot` alias, never used.
- `SlicerEngine.h:66-71` — empty `private:` section; the comment beneath it
  describes the `public:` pure-helper block below.
- `SectionPanel.h:28` / `SectionPanel.cpp:10-12` — `setTitle()`, declared and
  defined, never called.
- TEMPORARY DEBUG leftovers:
  - `PerformanceKeyboardPanel.cpp:3-5, 30-40` — `#include <iostream>` +
    `std::cerr` prints in `mouseDownOnKey`.
  - `SlicerModel.cpp:7, 972-987` — `#include <iostream>` + `std::cerr` prints in
    `setFocusedPerformanceStateSlot`, plus the `debugFocusChange*` atomics
    (SlicerModel.h:1112-1119).
  - `SequencerGrid.cpp:110-118` — the 30fps UI timer drains
    `engine.drainDebugTapeStopEvents()`/`drainDebugStretchEvents()` under
    `#if JUCE_DEBUG` "TEMPORARY DEBUG -- remove once step-extension Tape Stop
    testing is done". The engine-side mailbox infrastructure
    (SlicerEngine.h:606-716, SlicerEngine.cpp:2809-2900) is the tail of the
    same leftover. Remove together.

---

## 3. Stale comments

~30 of the 40 findings are Phase-3 leftovers where "the processor /
PluginProcessor.h" should read `SlicerModel` / `SlicerEngine`. Mechanical
identifier sweep clears most.

### 3.1 Class docs still narrate the Phase-1 god-class world

- `SlicerModel.h:22-25` — "The processor retains its full public API as
  one-line forwarders... the audio-thread engine still lives in the
  processor." Forwarders were deleted in Phase 3; the engine is `SlicerEngine`.
- `SlicerEngine.h:15-17` — "SlicerAudioProcessor ... forwards the public API
  here." Phase 3 deleted the forwarders; the UI binds model/engine directly.
- `SlicerModel.h:36-37` — "the processor binds it in its constructor" — now
  `SlicerEngine::SlicerEngine` (SlicerEngine.cpp:142).
- `SlicerModel.h:181-182` — "See the processor's processBlock()" — engine's.
- `SlicerModel.h:205-206` — "Written every block by the processor's
  renderAudition()" — engine's.
- `SlicerModel.h:409-412` — "the processor's `SlicerAudioProcessor::
  setTriggerMode()`" — no such method; wrapper is `SlicerEngine::setTriggerMode`
  (SlicerEngine.cpp:190). Self-contradictory ("processor's" vs "engine side").
- `SlicerModel.h:631-633` — same pattern for `setPitchMode`
  (`SlicerEngine::setPitchMode`, SlicerEngine.cpp:221).
- `SlicerModel.h:938-939` — "The processor's engine ... reads these directly."
- `SlicerModel.h:1113-1115` — "for the processor's freezeWatchdog to report" —
  `FreezeWatchdog` is the engine's (SlicerEngine.h:732-739).

### 3.2 "doc comment in PluginProcessor.h" cross-refs that now live elsewhere

The processor header is 66 lines and contains none of these docs.

- `SlicerEngine.cpp:18` — `setCurveShape` doc is in SlicerModel.h:555.
- `SlicerEngine.cpp:117-118` — `computeScratchCycleLengthHostSamples` doc is in
  SlicerEngine.h:171-187.
- `SlicerEngine.cpp:1078-1080, 2081-2082, 2813-2814` — mailbox docs are in
  SlicerEngine.h:629-638.
- `SlicerEngine.cpp:1783-1784` — bitcrush member docs are in SlicerEngine.h:375-388.
- `SlicerEngine.cpp:1887-1888` — flanger member docs are in SlicerEngine.h:390-401.
- `SlicerEngine.cpp:1919-1920` — "no global dial" rationale is in SlicerEngine.h:403-413.
- `PluginEditor.h:460-462` — "PluginProcessor.h's doc comment" — the quantize
  rationale is in SlicerModel.cpp:579-585.

### 3.3 Wrong-class references

- `SequencerGrid.cpp:113-114` — points at `SlicerModel::drainDebugTapeStopEvents`;
  the drain methods are `SlicerEngine::drainDebugTapeStopEvents`/
  `drainDebugStretchEvents` (called on the next lines).
- `SequencerGrid.h:20-22, 63-64, 120-121, 248-249` — "the processor's
  setSequencerCell()", "polls the processor's", "PluginProcessor::processBlock()",
  "committed to the processor" — all live on `SlicerModel`/`SlicerEngine`.
- `SequencerGrid.cpp:149-152, 296, 512-513, 726-728` — same.
- `SubdivisionProbabilityGrid.h:14-15` — "reads straight from the processor" —
  constructor takes `SlicerModel&`.
- `GranularStretcher.h:20-22, 43, 50-53, 61-63, 83-84` and
  `GranularStretcher.cpp:35-36, 133-134` — "the processor's job",
  "PluginProcessor's Stretch render branch", "PluginProcessor's Scratch style",
  "PluginProcessor's currentPosition" — all engine now.
- `EasingCurve.h:7, 24` — "PluginProcessor.cpp's applyCurveShape()" — now
  SlicerEngine.cpp:19 (anonymous namespace).
- `PerformanceKeyboardPanel.h:26` — "sourced from the processor's actual bank
  contents" — model via the `Source` adapter.
- `CMakeLists.txt:43-44` — "PluginProcessor.h's 'MIDI input / Sequencer pattern
  bank' section" — that section is in SlicerModel.h:811.
- `PluginEditor.cpp:1411` — "the processor's stepResolutionIndex/patternLengthBarsIndex"
  — model members.
- `SlicerEngine.cpp:2813-2814` — "the mailbox members' own doc comment in
  PluginProcessor.h" — SlicerEngine.h.

### 3.4 Garbled find-replace corruption

`SlicerEngine.cpp:1172-1175` and `:1215-1217` — prose mangled by a rename tool:
"BEFORE Subdivide model.slices currentPickLengthInHostSamples into individual
ticks" / "BEFORE Subdivide model.slices it into individual retrigger ticks
below". Intended text was presumably "before Subdivide splits it into
individual ticks". Unparseable as-is.

### 3.5 Stale constant / component references

- `SlicerEngine.cpp:2009` — "Stretch's is stretchDurationMultiplier-x" — the
  constant is `stretchSpeedMultiplier` now (SlicerEngine.h:373).
- `PluginEditor.h:134-135` — "RotaryKnob itself is now unused and deleted" —
  describes a component that no longer exists at all.
- `PluginEditor.h:386-389` / `PluginEditor.cpp:1402-1403` — "the processor can
  also stop it on its own" — the engine's processBlock auto-stops audition.
- `PluginEditor.h:578` — "sets the processor's currently selected drawing
  style" — `SlicerModel::setSelectedDrawingStyle`.

### 3.6 Editor header doc contradictions (PluginEditor.h)

- `:19` — "Step-41 editor" vs `PluginEditor.cpp:743` drawing "step 46". Two step
  markers disagree.
- `:50-52` — "laid out within a fixed, non-scrolling window sized once at
  construction" — contradicted by `updateWindowSize()` (PluginEditor.h:161-174,
  PluginEditor.cpp:868-907) and the header's own Pass-4 paragraph.
- `:43-44, 76-77` — trigger-mode history lists "Slice Length vs Clock" and "a
  third value, Sequenced (Step 37)" but never mentions the fourth value,
  Performance mode.
- `PluginEditor.cpp:746` — "loopLengthLabel/Knob" — both are `juce::Slider`
  number boxes since Pass 4.
- `PluginEditor.cpp:1661-1663` — "the old old triggerModeSelector.onChange" —
  "old old" typo; selector removed in the tab refactor.

### 3.7 Wrong component counts / stale panel docs

- `SectionPanel.h:6-8` — "five sections (Sample / Trim & Tempo / Detection /
  Engine / Playback Style)" — the Engine section no longer exists; the panels
  are Sample, Tempo, Detection, Fade In/Out, Pitch Mode, Playback Style
  (PluginEditor.h:313-331) plus Timing in the Generate tab.
- `PlaybackStyleGrid.h:7, 16-17` — "Step-19: playback style probability
  (Forward / Ping-Pong)" — there are now 9 styles and 4 trigger modes.
- `PatternBankPanel.h:27-42` — documents Performance mode as a user of the
  panel ("Performance mode's state bank ... Pass 1 -- Immediate switching
  only, so its BankSource::getPendingSlot() always returns -1"). Performance
  mode no longer uses `PatternBankPanel` at all — it uses
  `PerformanceKeyboardPanel::Source` (PluginEditor.h:587-589). Only the
  Sequencer context remains.

### 3.8 README.md

Entirely stale pre-refactor roadmap (AGENTS.md already warns not to treat it
as current). Dead-end claims:

- `:4-10` — "plays the whole sample back on any MIDI note (8-voice polyphony,
  one-shot, no slicing yet)".
- `:21-22` — "replace them with the four files here".
- `:40-43` — "Port the transient detector ... into C++" — shipped as
  `TransientDetector`.
- `:45-46` — "map slices to MIDI notes ... à la Simpler" — diverged entirely;
  actual model is weighted-random picks + pattern-bank/snapshot recall.
- `:47-50` — "The `triggerProbability` member is already sitting in
  PluginProcessor.h" — no such member exists.

Either rewrite or delete.

### 3.9 NeditVST-User-Guide.md

- `:20-22` — "NeditVST won't make sound until the host transport is running"
  — contradicted by Performance mode (plays on MIDI note-on with transport
  stopped; covered by test "performance mode recalls a snapshot with transport
  stopped").
- `:169` — "click near the left edge of the step" — the parameter menu opens on
  right-click anywhere on the cell; edge hit-zones belong to the Shift+drag
  extension.
- Section 5 lists only Slice Length / Clock / Sequenced under Trigger Mode —
  omits the fourth mode (Performance).

### 3.10 Minor history-narration

`SlicerModel.h:56-59` — "these are the accessors the processor's old inline
forwarders provided" — accurate as history but narrates deleted code. `:14` —
`getStateInformation` TODO premise ("once there are parameters worth
persisting") is obsolete; persistence was simply never implemented.

---

## 4. Naming / logic inconsistencies [OTHER]

- `SlicerEngine.cpp:1903` — `SlicerModel::SlicerModel::flangerDelayTimeExtremeMs`
  double qualification (compiles via injected-class-name; refactor artifact).
- `SlicerEngine.cpp:2907-2921` — `randomizeSequence()` re-implements the exact
  body of `SlicerModel::getSequencerNaturalLengthSteps()` (SlicerModel.cpp:806-830)
  instead of calling it; the engine already calls the shared accessor at
  SlicerEngine.cpp:1066. Drift risk.
- `SlicerEngine.cpp:164-169` — `prepare()` resets clock/reset flags but not
  `sequencedModeInitialized`/`performanceModeInitialized`, while
  `setTriggerMode()` resets all four together. Probably fine in practice, but
  inconsistent with the "always start aligned" convention in SlicerEngine.h:443-451.
- `SlicerEngine.h:305` — self-referential comment "see
  getRandomizeParametersForStyle()'s own doc comment" — the accessors carry no
  doc comment.
- `PlaybackStyleGrid` / `SubdivisionProbabilityGrid` — near-duplicate classes
  (identical paint loop, differ only in rowHeight/labelWidth/count/getters).
  PlaybackStyleGrid.h:8-13 documents the duplication; one parameterised
  multislider component would replace both.
- Hard-coded colours bypass `NeditPalette`: PlaybackStyleGrid.cpp:26,33,39,42;
  SubdivisionProbabilityGrid.cpp:26,33,39,42; PlaybackStylePalette.cpp:18-27;
  PluginEditor.cpp:1407.
- `PluginEditor.cpp:123,167,221,259,274,335,350` — identical per-slider comment
  "a holdover from when this editor scrolled internally" repeated 7 times while
  PluginEditor.h:91-93 documents it once for the whole editor.

---

## Suggested batching

1. **Comments only** — sections 3.1-3.7, 3.9, 3.10 + the two garbled sed
   fragments. Mechanical, zero behavioral risk.
2. **Comments + dead code** — add section 2 (all items) + 1.5. Low risk; dead
   members are private and unreferenced.
3. **Structural** — 4.1 (double qualification), 4.2 (dedupe natural-length
   math), 3.6's contradictory window doc. Medium risk; run the full test suite
   after (the math change has direct test coverage in test_model_tempo.cpp).
4. **Layering** — section 1.1 state migrations (highest risk), 1.2 processBlock
   factor (biggest win), 1.4 serialization, 1.1 debug atomics removal once the
   TEMPORARY DEBUG pass is done.
5. **Docs** — README rewrite/delete (3.8), user-guide corrections (3.9).
