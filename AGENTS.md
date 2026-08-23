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

## Session handoff (2026-08-23, evening)

- Working tree: Phase 1 committed (`467349b`), engine chunks 1+2
  committed (`f28be3b`, `3e83512`); engine chunks 3 (style renderers +
  dsp primitives), 4a (VoiceScheduler: Slice Length + Clock) and 4b
  (Sequenced scheduler) done but NOT yet committed. `ctest` 149/149
  green (51 state + 98 engine), zero warnings.
- Architecture decision (2026-08-23): DSP primitives live in
  `src/engine/dsp/` one-file-per-primitive, used directly by
  `PickRenderer` — the fancier "DspConfig.h alias swap seam" idea was
  considered and REJECTED as complexity without payoff.
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
  - `dsp/SweepFilter.h` — TPT (Zavalishin) state-variable filter,
    equivalent to the original's juce::dsp::StateVariableTPTFilter;
    LP/HP/BP, per-sample-sweepable cutoff, Q resonance, 2-channel state.
  - `dsp/Bitcrusher.h` — sample-and-hold + bit quantize, classic
    stair-step structure (quantize once per hold period). tick() once
    per output sample keeps stereo in lockstep; hold length re-read
    every grab so swept rates need no separate path.
  - `dsp/Flanger.h` — per-channel feedback comb; write = dry + fb·delayed
    (resonance), output = dry/wet crossfade independent of feedback;
    tick()/advance() once per output sample.
  - `PickRenderer.h/.cpp` — the shared voice path, faithful port of the
    original's ~900-line render section: BlockContext (per-block
    constants) + PickParams (captured at pick start, incl. a full
    StyleParameters copy) + per-sample `renderSample`. Covers: fades
    clamped to half pick length; bounce-midpoint fades (curve-shaped);
    Tape Stop decel with gain riding the curve (replacing fade-out),
    position-exhaustion freeze loop (25 ms "stuck tape"); Stretch always
    granular (own grain size, hardEdge, Grain Speed as character
    constant, loop fold fills extensions); Filter Down/Up log-sweep
    9k↔250 Hz per-tick/whole-window; Bitcrush/Flanger/Volume sweeps
    (per-pick vs whole-window progress via the renderer-owned window
    clock); beat-quantize ratio substitution; Performance Sync-off
    native rate; Control velocity gain + gate-release force-stop.
    Scheduler decides WHEN picks start; renderer decides what they
    sound like. `finished()` mirrors the original's pickWithinSchedule.
    Renderer additions for Sequenced: `retrigger()` (restarts the SAME
    pick with new durations WITHOUT resetting per-pick DSP or the window
    clock -- subdivided steps keep one continuous sweep underneath) and
    public window-clock accessors (`windowElapsedSamples()/
    windowLengthSamples()`).
  - `Scheduler.h/.cpp` — VoiceScheduler: Slice Length (reset windows,
    beat-quantize, weighted draws + uniform fallback), Clock (per-window
    slice/subdivision/style draw, tick retriggers, tape-stop/stretch
    whole-window override), AND Sequenced (4b): per-sample step grid on
    ppq boundaries; a filled column triggers its cell's style directly;
    params = fallbackParams + cell overrides merged; declared length =
    natural-steps vs Shift+drag extension, capped by the next active
    column anywhere in the grid (anticipatory fade); Subdivide slices a
    step into retrigger slots (first trigger IS slot 1); pattern recall
    via `requestPatternSwitch(note)` with immediate/setInterval/
    endOfPattern timing — immediate applies on the next per-sample pass
    (≤1 sample after the original's synchronous apply, documented);
    deferred timings evaluated against the CURRENTLY playing pattern's
    own dimensions; empty bank slot = no-op; recalls are applied from the
    current block's bank into a scheduler-owned working-pattern override
    (`releaseWorkingPatternOverride()` lets the future shell drop it when
    fresh state snapshots arrive). `playingStepIndex()` is the UI
    playhead signal.
  - Chunk 4c — Performance + Control schedulers + MIDI dispatch:
    Performance (`requestPerformanceRecall(state, note, transportPlaying)`):
    focused slot = live `workingState` over the SHARED trim; any other
    populated slot freezes ITS snapshot+trim into a scheduler-owned copy at
    recall time (later bank edits can't disturb a sounding pick);
    Quantize Recall defers to the next interval grid point while playing
    (boundary armed per-sample from the block-start ppq; a newer note-on
    overwrites the pending note) and falls straight through when stopped;
    loop on rechains the SAME segment via the finished-check, loop off goes
    silent; Sync off → `nativeRate` flag + srConversion as effective rate;
    durations per style (pingPong 2×natural, stretch grainSpeed×natural,
    scratch cycle, else natural); NO beat-quantize/reset-cap/window clock in
    this mode. Control (`controlNoteOn(note, vel, baseNote, numSlices)` /
    `controlNoteOff(note, gateMode)`): keyswitches at base−1−style select
    silently (scheduler-owned `controlActiveStyleOrdinal_`, seeded from
    state on mode entry since keyswitches can't mutate immutable snapshots;
    `controlActiveStyleOrdinal()` lets the shell fold it back); slice notes
    at base+k trigger with clamped velocity gain and control.styleParams;
    monophonic retrigger; one-shot; gate release force-stops after the fade,
    trigger mode ignores note-offs. Both runners: bounce/fold styles
    (pingPong/stretch/scratch) get `useDurationGate` on their declared
    window (they'd never exhaust position-wise) — same effect as the
    original's finite schedule end. `process()` now lets performance/control
    run with the transport STOPPED (MIDI-driven modes; quantize-recall falls
    through when there is no beat grid).
- Test-fixture lessons baked into `tests/engine/test_scheduler.cpp`: the
  caller must advance `ppqStart` across blocks like a host does (the
  scheduler derives each sample's beat position from the BLOCK START ppq
  -- feeding constant 0 stalls every boundary forever); exact ppq
  boundaries carry ±1-sample double-rounding jitter, absorbed by the
  `playQuiet`/`advanceToPicks` assertion pair. Test-authoring gotchas hit
  in chunk 4c: pick AGE starts when the pick starts (pre-recall blocks
  don't count); default grainSpeed is 4× so a default Stretch window is
  4 × natural samples; palette ordinals: forward0/pingPong1/tapeStop2/
  stretch3/filterDown4/filterUp5/bitcrush6/scratch7/flanger8.
- Audio-thread state-snapshot / message-passing mechanism DONE
  (header-only, `src/engine/`): `SnapshotProvider.h` — immutable
  per-block state views via `std::atomic<std::shared_ptr<const
  PluginState>>`; UI publishes clones (allocation stays off the audio
  thread), audio acquires a shared_ptr per block so mid-block publishes
  can't mutate the rendering view; successive reads never regress
  (modification order, stress-tested). `CommandQueue.h` — lock-free SPSC
  ring of trivially-copyable `EngineCommand`s (power-of-two capacity,
  free-running cursors, drop-on-full by caller's choice; advisory pokes
  only — sampleSlotReplaced / invalidateAnalysis / quit). `MidiDispatch.h`
  — pure `routeMidiNote()` mapping host notes onto the scheduler's
  per-mode entry points (performance recalls / control trigger+gate /
  sequenced pattern recall; SL+Clock have no MIDI semantics) +
  `velocityFromMidiByte`.
- Phase 3 VST3 shell DONE (`src/plugin/`, `NEDIT_BUILD_PLUGIN=ON`):
  - `ParameterSurface.h` — PURE (state-only, unit-tested without SDK):
    the automation contract. IDs 0..20 = generate.styleParams via the
    generic get/set; specials 100..107 (trigger mode, manual tempo
    +enable, loop length bars, control base note/gate, quantize recall
    +interval). IDs are a persisted-session contract: never renumber.
    Deliberate v1 decision: only Generate's styleParams are automatable
    (automating four parallel copies of the vocabulary would invite the
    silent-sharing bug back); other scopes stay UI-edited state.
  - `NeditProcessor.{h,cpp}` — SingleComponentEffect glue. Stereo out +
    MIDI event in, no audio input. Transport needs flags:
    TransportState|Tempo|ProjectTimeMusic; ppqStart = projectTimeMusic,
    frozen at last position when stopped (fixture semantics).
    Automation folds into a reusable `automationScratch_` copy per block
    (capacity pre-reserved ⇒ steady-state alloc-free); last-point-wins
    per param queue. getState/setState → serialize/deserialize; setState
    rejects garbage (kResultFalse, state untouched) and RESYNCS parameter
    objects afterwards (hosts read display values via getParamNormalized).
  - `factory.cpp` — BEGIN_FACTORY_DEF/DEF_CLASS2 with pinned FUID;
    subcategory "Instrument|Sampler".
  - CMake: SDK fetched (pinned commit) but NOT configured via its own
    cmake — minimal static lib compiles exactly the needed sources.
    GOTCHAS baked in: fdebug.h requires RELEASE=1 defined; STR16() only
    pastes literals (runtime titles need Steinberg::UString);
    vstsinglecomponenteffect.cpp #includes vsteditcontroller.cpp so NEVER
    compile both; module target links nedit_plugin via --whole-archive
    (static-initializer factory must survive archive resolution);
    factory.cpp belongs to the MODULE only (GetPluginFactory dup else);
    AudioBusBuffers.channelBuffers32 is float** (array of channel
    pointers); CMAKE_POSITION_INDEPENDENT_CODE ON globally.
  - Bundle assembled to build/nedit.vst3/Contents/x86_64-linux/
    libnedit.vst.so via POST_BUILD copy.
- Known Phase-3 boundary: the shell renders SILENCE until Phase 4 wires
  sample decode + transient analysis (slices list is empty ⇒ scheduler
  disarms; all plumbing verified by tests). No file IO exists yet by
  design — UI owns loading next phase.
- Next work: Phase 4 UI (VSTGUI): views as stateless renderers of
  PluginState + engine mailboxes; sample load/decode → analysis → slices
  into the processor; view state lives in UiState.
- Test totals: default build 173/173 (51 state + 122 engine);
  plugin build adds 13 shell tests = 186/186, zero warnings.

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
