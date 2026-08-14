# AGENTS.md

## Project overview

NeditVST is a JUCE VST3 generative slicer: load a sample, detect transients
into slices, and play them back with a weighted-random engine across four
trigger modes (Slice Length, Clock, Sequenced, Performance) and nine
playback styles (Forward, Ping-Pong, Tape Stop, Stretch, Filter Down/Up,
Bitcrush, Scratch, Flanger). MIDI note-ons drive pattern-bank recall
(Sequenced) and state recall (Performance).

- Language/toolchain: C++20, CMake, JUCE 8.0.14 (fetched via FetchContent).
- Two CMake targets: `NeditVST` (the VST3) and `NeditVST_tests` (a
  doctest console binary — no plugin client, no GUI).
- A legacy Projucer/Xcode project also lives under `NeditVST/`; the CMake
  build is the CI workflow and the one used for tests. JUCE source ends up
  in `build/_deps/juce-src/` — that's where the `juce_*` headers to consult
  for API details live.
- `docs/` holds incident post-mortems (e.g. the logging system, an editor
  SIGSEGV fix). Do not treat `README.md` as current — it predates the
  refactor.

## Build & test

Configure once (if `build/` isn't set up yet):

```sh
cmake -B build
```

Test suite (primary workflow — always run before finishing work):

```sh
cmake --build build --target NeditVST_tests -j
./build/NeditVST_tests_artefacts/Release/NeditVST_tests
ctest --test-dir build --output-on-failure
```

Plugin build (run after any `Source/` change that could affect it):

```sh
cmake --build build --target NeditVST -j
# VST3 lands in build/NeditVST_artefacts/Release/VST3/
```

Run a subset of tests with doctest's own filters, e.g.
`./build/NeditVST_tests_artefacts/Release/NeditVST_tests -tc="SlicerEngine:*"`.
The full suite must stay green (no skipped/failed) before a commit.

## Architecture (target: `Engine <- Model <-> UI`)

Three layers, refactored out of the former `SlicerAudioProcessor` god-class.
The UI talks to the model/engine directly; the processor is just a shell.

- **`SlicerModel`** (Phase 1) — owns ALL shared audio state: sample buffer,
  detected/manual slices, probability tables, sequencer grid + per-cell
  parameter overrides, pattern bank, performance state bank, trim markers,
  every stored parameter, the single `sampleLock` that guards it, the undo
  system, and the static serialization/schema tables. Calls
  `onPickStateInvalidated` under the lock whenever structure changes.
- **`SlicerEngine`** (Phase 2) — the real-time audio core: `processBlock()`
  DSP, MIDI dispatch, audition, all per-pick/per-window scheduling state,
  effect buffers (filter/bitcrush/flanger), the Random sources, and the
  `#if JUCE_DEBUG` debug mailboxes/watchdog. `processBlock()` takes
  `model.sampleLock` itself. Holds `SlicerModel& model` (public reference,
  fixed at construction, never outlives the model). Its pure helper statics
  (e.g. `pickWeightedIndex`, `computeBeatQuantizeTarget`) are public and
  unit-tested directly.
- **`SlicerAudioProcessor`** (Phase 3) — thin JUCE plugin shell: lifecycle
  overrides, buses/editor wiring, state-serialization stubs. Public
  `model` and `engine` members the editor binds from.
- Helpers owned by the model/engine: `TransientDetector` (one-off
  `analyze()` + cheap `detectSlices()` re-runs), `GranularStretcher`
  (time-stretch grains + `foldPosition`), `EasingCurve` (linear/ease-in/
  ease-out/ease-in-ease-out). UI code lives in `Source/*Panel.*`,
  `SequencerGrid.*`, `WaveformDisplay.*`, etc.

## Test suite conventions

doctest harness, one file per area, all wired into the
`target_sources(NeditVST_tests ...)` block in `CMakeLists.txt`:

- `test_engine_statics.cpp` — pure engine helpers (weighted picks, style/
  note-value/index mappings, quantize/scratch math).
- `test_engine_process.cpp` — `processBlock()`/audition integration:
  render gates, deterministic forward picks, fades, repitch stride,
  ping-pong folds, tape-stop decel, bitcrush sample-and-hold, clock and
  performance recall.
- `test_model_tempo.cpp`, `test_model_slices.cpp` — model tempo/trim math
  and slice rebuild/undo behavior.
- `test_transient_detector.cpp`, `test_granular_stretcher.cpp` — detector
  thresholds/holdoff/mono-sum, stretch fold/position math.

When adding a test:

1. Create `tests/test_<area>.cpp`; only the harness file
   (`test_main.cpp`, `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`) has a `main`.
2. Add the file to `target_sources(NeditVST_tests ...)` in `CMakeLists.txt`.
3. Rebuild the test target and run the suite.

Known quirks to respect when writing tests:

- `SlicerModel` is non-copyable (`JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR`).
  Build shared setup as a helper that mutates a model in place, or prepare
  the model in the same scope as the engine.
- Any test that triggers a slice-rebuild path must set
  `model.onPickStateInvalidated = [] {};` first or the engine's unbound
  callback throws `std::bad_function_call`.
- doctest rejects complex expressions in `CHECK` ("Expression Too Complex
  Please Rewrite As Binary Comparison!"): hoist braced-init-list arguments
  and `||`/`&&` subexpressions into local variables first.
- Engine tests that construct a `juce::AudioPlayHead::PositionInfo` use the
  setters (`setBpm`, `setPpqPosition`, `setIsPlaying`) — they take JUCE
  `Optional`/plain-bool arguments, not the `*pos.bpm` read API.
- Model/engine members are deliberately public (Phase 1/2 "move, don't
  redesign"); tests populate them directly rather than through accessors
  where that's the point of the test. Don't tighten access without checking
  which tests/UI code read those members.

## Workflow conventions

- Commit in focused batches with a one-line message matching the repo's
  existing style (see `git log --oneline`); only commit/push when asked.
- `main` is the only branch. The user's "Phase N" refactors intentionally
  kept behavior byte-identical — treat unrelated behavior changes with
  suspicion unless explicitly requested.
