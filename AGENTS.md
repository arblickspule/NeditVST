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

## Session handoff (2026-08-23, afternoon)

- Working tree: Phase 1 committed (`467349b`), engine chunk 1 committed
  (`f28be3b`); engine chunk 2 (stretcher/fold/easing) done but NOT yet
  committed. `ctest` 96/96 green (51 state + 45 engine), zero warnings.
- **Phase 2 progress** — `src/engine/` (pure C++, framework-free, links
  only `nedit_state`):
  - `Slice.h` — derived `[startFrame, endFrame)` slice type (int64).
  - `TransientDetector.h/.cpp` — faithful port: 1ms/50ms envelope
    follower → positive derivative → adaptive threshold
    (`globalMax − s·(globalMax − noiseFloor)`, noise floor = mean positive
    derivative, sensitivity 0 ⇒ zero onsets by contract) → rising-edge
    peak-pick with holdoff; two-stage analyze/detect split retained;
    `findNearestPeak` for manual-point/trim snapping. Raw channel-pointer
    API, no JUCE AudioBuffer.
  - `SliceBuilder.h/.cpp` — merge pipeline with the original's ordering
    guarantees: exclusion matching against RAW detected positions
    (±50 ms) BEFORE quantize; auto onsets optionally grid-quantized;
    manual points merged as-is and soft-excluded outside trim; trim start
    always the first boundary.
  - `Tempo.h` — header-only pure functions: `sourceSpanSeconds`,
    `calculatedOriginalBpm`, `minimumHoldoffMs` (1/32-note, 1 ms floor,
    30 ms no-tempo fallback), `repitchRatio`, `playbackRate`,
    `quantizeFrameToGrid` (shared by transient quantize + performance
    trim snap). Reads `SampleState.trim*` directly — no `tempoTrim*`
    duplicate needed since Performance no longer aliases the trim.
  - `tests/engine/` — synthetic click-track tests (`TestSignals.h`):
    known-position detection, sensitivity ordering, holdoff suppression,
    trim confinement, stereo mono-summing, peak snapping, full
    detector→merge pipeline, all tempo math.
  - `Easing.h` — `applyEasingCurve` (linear/easeIn/easeOut/smoothstep);
    enum stays in `state/Types.h` (it is serialized).
  - `Fold.h` — `foldPosition` as a free function (shared by both pitch
    modes' render paths): forward identity, pingPong triangle over
    2×length with optional per-leg easing (kept as a SEPARATE branch so
    linear callers stay bit-identical, same guarantee as the original),
    loop modulo (Stretch step-extension fill).
  - `GranularStretcher.h/.cpp` — faithful port, framework-free +
    allocation-free (audio-thread safe): 4-grain pool with
    furthest-into-life stealing, 50% overlap, hop scheduling separated
    from per-grain read rate (pitchRatio never touches hops), Hann/
    Triangular/hardEdge (10% ramp) windows, `windowGain` public static
    for testability. Raw channel-pointer source API.
  - Stretcher tests are rendered-output tests: Hann 50%-overlap
    complementarity → unity DC reconstruction, ramp passthrough at 1:1,
    2× stretch tracks half-speed trajectory within a grain, pitch ratio
    leaves the stretch trajectory unchanged, loop fold keeps spawning
    inside the slice, degenerate-input safety (null source, zero grain,
    pool exhaustion).
- Next engine work (in order): the 9 playback-style renderers (voice/
  pick render path: repitch + time-stretch, fades, per-style DSP —
  filter sweep, bitcrush, flanger, tape stop, scratch, volume ramps),
  then per-mode schedulers (per-sample ppq boundaries!), then the
  audio-thread snapshot/message-passing mechanism.

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
