# Nedit — native VST3 rewrite of NeditVST

## Mission

Rewrite <https://github.com/nedrush/NeditVST> (a 15k-line JUCE monolith) as a
Steinberg-native VST3 plugin. Clear separation of concerns in the file
structure. UI decoupled from Engine; both operate on State. Tests for all
critical aspects. Profiling/debugging utilities that are trivially
included/excluded.

## Current status (updated 2026-08-23)

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

## Session handoff (2026-08-23)

- Working tree: Phase 1 complete, builds clean, `ctest` 51/51 green.
- Git: repo initialized, **nothing committed yet** — first commit of the
  Phase 1 baseline is a sensible next action (build/ is gitignored).
- Toolchain decisions locked in: VST3 SDK via CMake FetchContent, VSTGUI
  for UI, Catch2 for tests, binary chunk + JSON debug mirror for state.
- Next work item: **Phase 2 (Engine)** — see roadmap below. Suggested
  starting order: transient detector + slice builder (pure functions,
  easiest to test offline), then tempo/repitch math, then schedulers.

## Rules of engagement

- **State**: pure, serializable, no SDK/framework includes, every struct has
  `sanitize()` + `operator==`. New fields ⇒ new section or appended field +
  version bump + round-trip test. Derived state (slice boundaries, peaks)
  and runtime state (schedulers, current pick, DSP scratch) are NEVER
  serialized and do not live in `src/state/`.
- **Threading** (for Engine phase): no monolithic lock (the original held
  one `CriticalSection` for the whole `processBlock` and froze DAWs). Audio
  thread gets an immutable state snapshot per block; UI edits flow through
  a queue/atomics. No allocation, locks, logging, or string work on the
  audio thread — audio-thread diagnostics go through `nedit::debug::Mailbox`.
- **Timing** (for Engine phase): all ppq boundary checks are per-sample,
  not per-block (the original's "Step 6 bug").
- **Tests**: every state change lands with tests. Robustness tests
  (truncation/garbage/bit-flip) must stay green — they already caught one
  real out-of-bounds write (bank slot validation).
- **Warnings**: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion`, keep the build at zero warnings.
- Build: `cmake -B build && cmake --build build && ctest --test-dir build`.

## Reference

- Original source: clone of <https://github.com/nedrush/NeditVST> at
  `/tmp/opencode/NeditVST` (re-clone if missing). Behavioural spec:
  `NeditVST-User-Guide.md` in that repo.
- Verified original tables live in `src/state/Types.h` /
  `StyleParameters.cpp` (note values, style names, param ranges/defaults).

## Roadmap

- **Phase 1: State** — DONE (above).
- **Phase 2: Engine** (next)
  - `src/engine/`: transient detection (envelope follower → derivative →
    adaptive threshold → tempo-relative holdoff), slice building
    (onsets + manual/excluded points + trim + grid quantize → derived
    slice list), tempo/repitch math, granular stretcher, the 9 playback
    styles, per-mode schedulers (Slice Length, Clock, Sequenced,
    Performance, Control), MIDI dispatch.
  - All pure C++ operating on `PluginState` + audio buffers; unit-testable
    offline (rendered-output tests, scheduler timing tests).
  - Define the audio-thread state-snapshot/message-passing mechanism.
- **Phase 3: VST3 shell** — `src/plugin/`: `IComponent`/`IEditController`
  glue, `getState`/`setState` → `nedit::state::serialize/deserialize`,
  parameter surface decisions, bus/MIDI setup. VST3 SDK via FetchContent
  (`NEDIT_BUILD_PLUGIN=ON`).
- **Phase 4: UI** — VSTGUI; views are stateless renderers of
  `PluginState` + engine mailboxes (playhead, playing step). No polling
  timers where a notification will do; view state lives in `UiState`.
- **Phase 5: Polish** — presets, profiling passes, DAW matrix testing.

*This is an evolving document. Update with current status, rules of
engagement, and future plans on every significant change.*
