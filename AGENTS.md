# Nedit — native VST3 rewrite of NeditVST

## Mission

Rewrite <https://github.com/nedrush/NeditVST> (a 15k-line JUCE monolith) as a
Steinberg-native VST3 plugin. Clear separation of concerns in the file
structure. UI decoupled from Engine; both operate on State. Tests for all
critical aspects. Profiling/debugging utilities that are trivially
included/excluded.

## Live status (quick read)

The long-form project status and session-handoff history live in the docs
folder (see below); this section is the short "where we are now" snapshot.

- Native VST3 rewrite complete end-to-end: State → Engine (slicing, schedulers,
  the 9 playback styles, DSP) → VST3 shell → VSTGUI editor (waveform, style
  band + params, timing ribbon, sequencer step grid + per-cell overrides).
- Sequencer grid now has issue #2's scroll/pan: normalized persistent 2D
  viewport in `SequencerState` (v4 serialization) + overlay scrollbars + h&v
  wheel-zoom (axis-locked over a bar) + middle-mouse drag pan, all driven by
  framework-free zoom/pan/scrollbar math in `src/ui/SequencerGridGeometry.h`.
- Plugin display name: **NeditRemix**.
- Branch model: the ORIGINAL `nedrush/NeditVST@main` is the JUCE PROTOTYPE
  (exploration: reverb/delay porting). The native rewrite lives on the
  rewrite branch (`rewrite` on the original; `feature/custom-ui-components` /
  `main` on the fork). The two share NO common ancestor — switch with
  `git checkout`, never merge/rebase between them.
- Cross-platform UI-test harness landed (`tools/nedit_ui_harness.cpp`): hosts
  the editor IN-PROCESS on a native parent window (reusing editor_smoke's
  platform glue) and drives synthetic mouse/wheel events straight into the
  live `CFrame` via `CFrame::dispatchEvent` (direct-first driver), asserting
  the persisted sequencer viewport reacts (defaults, canvas zoom, zoom clamps,
  scrollbar axis-lock, MMB pan, knob drag + track paging). Uses Debug-only
  test hooks (`NeditProcessor::testHookEditor`, `NeditEditor::testHookFrame`/
  `testHookSequencerGrid`); gated behind `NEDIT_BUILD_UI_HARNESS`; registered
  as the `ui_harness` ctest (exit 77 = skip when headless); Linux CI job runs
  it under xvfb. macOS/Windows harness runs are a follow-up (the in-process
  `VSTGUI::init` path is so far only verified on Linux/xcb).
- Test totals last run: 303/303 green, zero warnings (full plugin build incl.
  the ui_harness ctest).

## Docs (optional reading — split out of AGENTS.md to keep this file lean)

- `docs/STATUS.md` — detailed project status + implementation history.
- `docs/SESSION_HANDOFF.md` — chronological session-handoff notes.

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

*This is an evolving document. Update the "Live status" snapshot and the docs
with current status, rules of engagement, and future plans on every significant
change.*
