# Next Steps

Findings from a full-codebase review (three parallel sweeps of every file in
`Source/`, `tests/`, and the docs) against the target architecture
`Engine <- Model <-> UI` described in AGENTS.md.

Status: the layering batch is largely landed (1.1, 1.2's pick-start factor,
1.4, 1.5, plus the whole dead-code and almost all of the comment sweeps).
This file is now the *current* backlog: only what is still hanging, plus
findings from the serialization batch.

Legend: [LAYERING] [DEAD CODE] [STALE COMMENT] [OTHER] [NEW]

---

## 1. Architecture / layering

### 1.1 Engine real-time state parked in the model [LAYERING] — DONE

`auditionPosition`, `activePatternBankSlot`, `pendingPatternSwitchNote`,
`pendingPerformanceRecallNote` and the `debugFocusChange*` atomics moved to
the engine (`a277de1`). The model keeps only the UI-facing
`auditionPlaybackPositionForUI` (SlicerModel.h:1043).

### 1.2 `processBlock()` god-function [LAYERING] — PARTIAL

SlicerEngine.cpp:398-2351 — still ~1,950 lines. The three near-identical
pick-start blocks (Clock / Sequenced / Slice-Length) were factored into
`capturePickStyleValues`/`applyPickState` (SlicerEngine.h:353,370;
SlicerEngine.cpp:258,316; call sites 692-693, 932-933, 1430-1431) in `b97dc4b`.
The god-*function* survives, just shorter; the next readable slice is
elsewhere in the block (candidate: the per-style render branches).

### 1.3 Model → engine callback [LAYERING] — DONE

`onPickStateInvalidated` (SlicerModel.h:1121) remains the one deliberate
exception, and the class doc describing it (SlicerModel.h:31-37) is now
accurate; the engine binds it at SlicerEngine.cpp:141.

### 1.4 State serialization [LAYERING] — DONE (with follow-ups)

Implemented and wired (`3bb6dda`): decision doc
`docs/state-serialization-decision.md`, `SlicerModel::saveState`/`restoreState`
(SlicerModel.cpp:519-1012), processor delegates at PluginProcessor.cpp:48-62.
Follow-ups still hanging — see Section 5 (NEW) for the trim-clamp and
version-field gaps, and the editor-refresh item below:

- The editor is push-based with no ChangeListener: a host-initiated
  `setStateInformation` (preset change while the editor is open) won't
  refresh any controls. The 10fps `timerCallback` polls undo/redo/audition/
  grid selectors (PluginEditor.cpp:1396-1422), covering part of it, but the
  panels themselves need a "refresh all controls" hook.

### 1.5 Processor no-op [DEAD CODE] — DONE

Constructor is empty (PluginProcessor.cpp:5-9); the `model.cancelMidiLearn()`
no-op is gone (`cancelMidiLearn` survives as a real model method called by
the editor at PluginEditor.h:597).

---

## 2. Dead code — CLEARED

The `SlicerEngine.h` forwarders (`resetSequencerGrid`, `computeSourceSpanSeconds`,
`SequencerPatternSnapshot` alias, empty `private:` section) and all TEMPORARY
DEBUG leftovers (iostream/cerr, `debugFocusChange*`, 30fps debug drains,
engine mailboxes, `FreezeWatchdog`) are gone (`dab5199`, `a277de1`).
`SectionPanel::setTitle()` no longer exists.

---

## 3. Stale comments — only the survivors

All of 3.2, 3.4-3.10 and all but one bullet of 3.1/3.3 landed
(`91463e7` + the 1.x commits). Remaining:

- `SlicerEngine.h:16-17` — "SlicerAudioProcessor keeps the plugin plumbing
  (buses, editor, state serialization) and forwards the public API here."
  Phase 3 deleted the ~220 forwarders; the processor forwards only the two
  real-time entry points (prepareToPlay/processBlock).
- `PluginEditor.cpp:1416,1420` — local variable *names*
  `processorStepResolutionId`/`processorPatternLengthId` still use the
  "processor" prefix for data that now lives on the model (cosmetic; the
  surrounding comment text is correct).
- `README.md` (3.8) — entirely stale pre-refactor roadmap; see Section 6.

---

## 4. Naming / logic inconsistencies [OTHER]

- `SlicerEngine.cpp:153-158` — `prepare()` resets clock/reset flags but not
  `sequencedModeInitialized`/`performanceModeInitialized`, while
  `setTriggerMode()` (SlicerEngine.cpp:182-186) resets all five together.
  Probably fine in practice, but inconsistent with the "always start aligned"
  convention in SlicerEngine.h:443-451.
- `SlicerEngine.h:380` — self-referential comment "see
  getRandomizeParametersForStyle()'s own doc comment" — the accessors carry
  no doc comment (same pattern at SlicerEngine.cpp:2652-2653).
- `PlaybackStyleGrid` / `SubdivisionProbabilityGrid` — near-duplicate classes
  (identical paint loop, differ only in rowHeight 18 vs 16, labelWidth 70 vs
  44, count/getters). Duplication documented at PlaybackStyleGrid.h:9-14; one
  parameterised multislider would replace both.
- Hard-coded colours bypass `NeditPalette`: PlaybackStyleGrid.cpp:26,33,39,42;
  SubdivisionProbabilityGrid.cpp:26,33,39,42; PlaybackStylePalette.cpp:18-27;
  PluginEditor.cpp:1407.
- `PluginEditor.cpp:123,167,221,259,274,335,350` — identical per-slider comment
  "a holdover from when this editor scrolled internally" repeated 7 times while
  PluginEditor.h:95 documents it once for the whole editor.

---

## 5. Spotted during the serialization batch (1.4) [NEW]

1. **`restoreState` does not clamp the trim markers against the current
   buffer.** SlicerModel.cpp:724-731 stores `trimStartSample`/`trimEndSample`/
   `tempoTrimStartSample`/`tempoTrimEndSample` raw (the performance-bank slot
   trims at :979-980 are equally unclamped). The interactive setters clamp
   (setTrimStartSample to [0, trimEnd-64], setTrimEndSample to the buffer
   length) and `loadSample` resets trims to the full buffer — so the hazard
   is specifically a host-initiated preset restore while a different/shorter
   sample is loaded. Today it is memory-safe (renderAudition clamps reads,
   SlicerEngine.cpp:2380-2381) but degenerate: `setAuditionActive` re-arms the
   cursor at the out-of-range trimStart (SlicerEngine.cpp:225) and playback
   reads the last sample; the WaveformDisplay draws the handles off-screen
   and unreachable. Also, the restoreState doc at SlicerModel.h:940-943
   overclaims — "clamps every index" — the trims are the exception. Fix:
   jlimit the four trims to the loaded buffer (or skip them when
   `! sampleLoaded`) inside restoreState.

2. **The `version` attribute is write-only metadata.** saveState writes
   `version=1` (SlicerModel.cpp:524); restoreState reads and deliberately
   ignores it (SlicerModel.cpp:694-698). Fine for the dev-stage XML choice,
   but any format evolution must branch on it — record that the reader's
   "additive/tolerant" contract is what makes old saves load into new builds,
   not the version number.

3. **Slice probabilities auto-size gap** (minor, informational): the
   slice-indexed `<probabilities slice>` list is skipped when its length
   doesn't match the current slice count, and nothing re-initialises the
   table — so after a preset restore against a different sample the table
   keeps whatever `rebuildSlicesFromDetectionAndManualPoints` last wrote.
   That is the intended "defaults until the matching sample is loaded"
   behaviour; just noting it is a deliberate consequence of the size-guard,
   not a leak.

---

## 6. Suggested batching

1. **Comments (trivial)** — SlicerEngine.h:16-17, the two processor-named
   locals (PluginEditor.cpp:1416,1420), and the restoreState doc overclaim
   (SlicerModel.h:940-943).
2. **Structural (small, testable)** — clamp the trims in restoreState (add a
   serialization test for the mismatched-sample case); make `prepare()`
   reset-set match `setTriggerMode()`.
3. **Docs** — rewrite or delete `README.md` (3.8).
4. **Editor follow-up** — the host-preset-change control refresh hook from
   1.4.
5. **Nice-to-haves (leave for a polish pass)** — multislider dedupe,
   NeditPalette colours, the 7x holdover comment, the version-migration hook.
