# Status and implementation history

Archived out of AGENTS.md to keep it lean; optional reading. Covers the live
project status and implementation history.

---

# Current status (updated 2026-09-03)

**Merge note (2026-09-03): fork `main` reconciled into `feature/custom-ui-components`
so PR #29 could merge.** The fork's `main` had diverged: it carried the copilot CI
polish (editor-smoke fixes, macOS ccache, least-privilege workflow) merged via PRs
#26/#27, and an independent (rebased) copy of the editor-smoke / macOS-crash work,
while `feature` carried the newer multi-instance/reopen smoke + our later bug fixes.
Resolution: feature's version won for the duplicated work (`editor_smoke.*`,
`NeditEditor` font-guard block); main's unique additions were kept — the
`-fobjc-arc` / `/Zc:char8_t-` smoke compile options (`tools/CMakeLists.txt`), the
Windows smoke exe path + ccache/least-privilege CI bits (`build.yml`), the
`NSRunLoop` pump fix (`editor_smoke_mac.mm` — ours had the buggy `[app runMode:]`),
and both documentation sections in AGENTS.md. Verified: 290/290 tests green,
smoke opens editor on Linux. NOTE: feature/fork-main now share a common ancestor
(`c532421`) again; the un-merged orphan is only against `nedrush/NeditVST`'s own
`main` (the JUCE prototype).

**Phase 1 (State) — implemented, 51 tests green.**

- `src/state/` — pure C++20, zero framework dependencies:
  - `Types.h` — note-value palette (20 entries, verified against original),
    playback styles (9), trigger modes (5), all small enums, bar tables.
  - `StyleParameters.h/.cpp` — the 21-parameter style vocabulary, typed
    fields + generic id-indexed access, info table (name/range/default/
    discrete/swept), applicable-params-per-style table.
  - `SampleState.h` — sample path (full path — original stored name only),
    global trim, detection inputs, manual/excluded points, tempo.
  - `RenderState.h`, `GenerateState.h`, `SequencerState.h`,
    `PerformanceState.h`, `ControlState.h`, `UiState.h`, `PluginState.h`.
  - `Serialization.h/.cpp` + `StreamIO.h` — versioned, section-tagged binary
    chunk (`'NEDT'` magic, v1). Unknown sections skipped (forward compat),
    missing sections keep defaults (backward compat), everything
    range-clamped on load. Fuzz-tested: truncation at every byte, random
    garbage, 500 bit-flip corruptions — no crash, no out-of-range state.
  - `JsonIO.h/.cpp` (separate target `nedit_state_json`, nlohmann) — human
    readable export/import for debugging/diffing/golden files.
- `src/debug/DebugTools.h` — `NEDIT_SCOPED_TIMER`, `NEDIT_LOG`, lock-free
  `Mailbox<T>`; all compile to nothing unless `NEDIT_DEBUG_TOOLS` is on.
- `tests/state/` — Catch2 v3 (FetchContent). `TestStateBuilder.h` builds a
  fully-mutated state used by both round-trip suites.

### Original-codebase pitfalls FIXED in the state design

1. **Style probabilities were triple-shared** (Slice Length + Clock +
   Sequencer Randomize) → `GenerateState.styleWeights` and
   `SequencerState.randomizeStyleWeights` are independent.
2. **Performance mode repointed the global trim atomics** (needed a
   duplicate `tempoTrim*` pair as a workaround) → `PerformanceSnapshot`
   (including the working state) owns its trim; `SampleState`'s trim is the
   only global trim and Performance never touches it.
3. **Global style params silently shared** by Generate, Control, and
   sequencer fallback → three independent `StyleParameters` copies
   (`generate.styleParams`, `control.styleParams`,
   `sequencer.fallbackParams`); Performance snapshots each own one (as
   before).
4. **Per-step overrides keyed by name strings** → keyed by `StyleParamId`.
5. **Nothing was ever persisted** (`get/setStateInformation` were stubs) →
   full state serialization including both 128-slot banks.
6. **Editor-owned zoom/pan reset on reopen caused a SIGSEGV** → zoom/pan
   and active tab live in `UiState` in the model; views must always
   initialize FROM state.
7. `loopLengthBars` edits must NOT reset the sequencer grid (stale coupling
   in the original) — documented in `SequencerState.h`, to be enforced in
   the Engine.

### Known deliberate simplifications (revisit later)

- Slice identity: `generate.sliceWeights` is parallel to the derived slice
  list and resets when slices rebuild (same as original). Stable slice IDs
  are a possible post-Phase-2 enhancement.
- `SequencerState` monophony (one style per column) is enforced by
  mutators/engine, not by the data structure.

### Known deferred issues (2026-08-29 review — real, unfixed, tracked)

Findings from the crash-review sweep that were deliberately NOT fixed
(design work, not spot fixes). None are crash-class; the crash-class and
UB findings from the same sweep were fixed (see the CRASH FIXES and
HARDENING entries below).

- **Audio-thread allocation: the automation fold.** `NeditProcessor::
  process()` does `automationScratch_ = *snapshot` whenever the block
  carries parameter changes. Only `manualPoints`/`excludedPoints` capacity
  is pre-reserved, so the "steady-state alloc-free" claim holds only for a
  near-default state: copying `sequencer.grid`, populated
  `patternBank` grids/override `std::map`s and `performance.bank` allocates
  (maps are node-based — they ALWAYS allocate per copy). Glitch/priority-
  inversion risk under automation + heavy state. Fix sketch: cache the
  last-seen snapshot pointer and re-copy scratch only when a new publish
  landed; automation points are absolute last-point-wins values, so they
  can be re-applied onto the existing scratch without a fresh copy.
- **Audio-thread allocation: pattern recall.** `applyPatternRecallFromState`
  deep-copies a `SequencerPattern` (grid vector + two maps) into
  `recalledPattern_` inside runSequenced's per-sample loop when a recall
  boundary fires. Same class of violation; needs a preallocated
  double-buffer or a UI-thread-prepared copy handed over via pointer swap.
- **`retrigger()` can resurrect a dead pick.** The Sequenced subdivide path
  consults `renderer_.currentPick()` and calls `retrigger()` (which sets
  `active = true` unconditionally) without checking `renderer_.hasPick()` —
  a pick cleared earlier in the step revives with stale PickParams. All
  source reads are clamped, so wrong audio only, never a crash.
- **Stale frozen performance trim after a sample swap.** Bank snapshots and
  `performanceFrozenSnapshot_` sanitized against the OLD sample keep
  out-of-range trims when a shorter file is loaded mid-session; renderer
  clamping makes this safe (silence/edge audio). Correct fix: re-clamp
  snapshot trims on sample load.
- **`data.symbolicSampleSize` never checked.** A host insisting on 64-bit
  float processing would make `channelBuffers32` a reinterpreted double
  buffer — garbage audio, not a crash (SingleComponentEffect's default
  `canProcessSampleSize` only accepts kSample32, so a conforming host
  won't). One-line bail if it ever shows up in the wild.
- **Interpolation `frac` unclamped at the edges.** When a read position
  clamps at index 0, `frac` can go negative (bounded extrapolation between
  two real samples; at the top edge idx1 == idx0 neutralizes it). Audible
  pop at worst.
- **Sequenced Subdivide floors its tick at 1 sample.** Degenerate bpm/step
  combinations turn a subdivided step into per-sample retriggers —
  `picksStarted_` churn / CPU burn, audio stays defined.

