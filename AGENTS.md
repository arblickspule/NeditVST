# Nedit — native VST3 rewrite of NeditVST

## Mission

Rewrite <https://github.com/nedrush/NeditVST> (a 15k-line JUCE monolith) as a
Steinberg-native VST3 plugin. Clear separation of concerns in the file
structure. UI decoupled from Engine; both operate on State. Tests for all
critical aspects. Profiling/debugging utilities that are trivially
included/excluded.

## Current status (updated 2026-08-29)

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
    (`requestWorkingPatternRelease()` lets the future shell drop it when
    fresh state snapshots arrive — thread-safe: an atomic request drained
    at the top of the NEXT process() block, because destroying the
    override mid-block would free containers SequencerView holds raw
    pointers into). `playingStepIndex()` is the UI playhead signal.
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
  per-block state views via `engine::AtomicSharedPtr<const PluginState>`
  (see `AtomicSharedPtr.h`); UI publishes clones (allocation stays off the
  audio thread), audio acquires a shared_ptr per block so mid-block publishes
  can't mutate the rendering view; successive reads never regress
  (modification order, stress-tested). PORTABILITY GOTCHA: `std::atomic<
  std::shared_ptr<T>>` is a libstdc++/MS-STL extension that libc++ rejects
  (shared_ptr not trivially copyable — the "atomic smart pointers" C++20
  proposal was removed from the standard). `AtomicSharedPtr` hides the
  standard `atomic_load_explicit`/`atomic_store_explicit` free-function
  fallback (with the C++20 deprecation notice silenced) under `_LIBCPP_VERSION`;
  macOS CI lane depends on it. `CommandQueue.h` — lock-free SPSC
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
- Cross-platform CI + CMake (Windows/macOS builds work):
  - Platform SDK TUs selected per-OS (systemclipboard/threadchecker linux/
    mac(.mm)/win32), module entry linuxmain/dllmain(AU)/macmain(CFBundle —
    links CoreFoundation), VSTGUI Linux runloop bridge compiled Linux-only
    (Cocoa/GDI backends hook the host loop themselves).
  - Whole-archive linkage per platform: GNU `-Wl,--whole-archive`,
    Apple `-Wl,-force_load,$<TARGET_FILE:...>`, MSVC
    `/WHOLEARCHIVE:...` + `/EXPORT:GetPluginFactory`.
  - Bundle assembly per platform: Linux x86_64-linux/nedit.so (+ moduleinfo
    via the Linux-only nedit_moduleinfotool + strip); Windows
    Contents/x86_64-win/nedit.vst3 (copy, no strip); macOS uses CMake's
    MODULE+BUNDLE wrapper (nedit.vst3/Contents/MacOS/nedit) then ad-hoc
    codesigns (`--force --deep --sign -` — unsigned MH_BUNDLE won't dlopen
    on Apple Silicon). moduleinfo.json is deliberately Linux-only (SMTG
    behaviour; Windows/macOS hosts don't read it).
  - MSVC gets `/w` where GCC/Clang get `-w` (NEDIT_SILENCE_WARNINGS);
    `enable_language(OBJCXX)` for the .mm SDK TUs.
  - Editor file dialog: Linux keeps the detached zenity/kdialog thread (a
    blocking CNewFileSelector on the embedded X11 thread froze the desktop);
    Windows/macOS use CNewFileSelector synchronously — those backends pump
    their own event loop. `unistd.h`/popen guarded `#if __linux__`.
  - nedit_moduleinfotool + the `deploy` target are Linux-gated.
  - CI build.yml matrix: ubuntu-latest + macos-latest (arm64) +
    windows-latest (MSVC), VSTGUI system deps installed only on Linux,
    `--config Release` + `ctest -C Release`, bundles uploaded as artifacts.
- Known Phase-3 boundary RESOLVED in Phase 4a: the shell now renders real
  audio end-to-end (decode → analysis → slices → scheduler → output).
- Phase 4a (sample pipeline) DONE:
  - `WavDecoder.{h,cpp}` — strict RIFF/WAVE parser: PCM16/24/32 + float32/64
    (incl. WAVE_FORMAT_EXTENSIBLE), mono..16ch, planar float out. Truncated
    data chunks decode present frames; everything else malformed ⇒ nullopt.
    GOTCHA: fmt payload offsets are format(0) channels(2) rate(4)
    byteRate(8) blockAlign(12) bits(14) — byteRate is at 8, easy to misread
    as blockAlign.
  - `SampleManager.h` — header-only. AtomicSharedPtr<const LoadedSample>
    slot ({DecodedAudio audio, vector<Slice> slices}); loadFile/
    loadFromMemory run decode + TransientDetector.analyze +
    buildSlices on the CALLER thread and publish immutably. Returns
    LoadResult{sample, updated SampleState} — caller folds metadata
    (path/rate/length/full-span trim) into its authoritative state.
  - `NeditProcessor::requestSampleLoad(path)` (UI thread): manager load +
    fold sample metadata + REALIGN generate.sliceWeights to slices.size()
    filled 1.0 (weights are parallel to the derived list; empty weights =
    zero picks, see pickWeightedSlice) + publish.
  - process(): acquires the slot per block; fixed-capacity channel-pointer
    table (kMaxSourceChannels=16) so no audio-thread allocation; slice
    count for MIDI dispatch = min(actualSlices,32).
  - Tests (`test_sample_pipeline.cpp`): decoder round-trips per format,
    garbage rejection + truncation-at-every-byte fuzz, manager metadata +
    slot publication, AND first audible end-to-end (click-track WAV →
    transport-driven process() → picksStarted>0 + output energy).
- Phase 4b UI — VSTGUI editor shell DONE (xcb dev packages installed;
  the earlier environment blocker is gone):
  - VSTGUI built via its own cmake as a subdirectory (core `vstgui`
    target only: STANDALONE/TOOLS/UISCRIPTING/XMLPARSER all OFF); SDK's
    `vstguieditor.cpp` + `vstgui_linux_runloop_support.cpp` compiled into
    nedit_plugin (they need VSTGUI headers, so NOT in nedit_vst3_sdk).
  - `NeditEditor.{h,cpp}` — programmatic CFrame (no uidescription XML):
    top bar (trigger-mode COptionMenu bound to param 100 via the
    beginEdit/setParamNormalized/performEdit/endEdit protocol, Load
    Sample button → CNewFileSelector (x11fileselector) →
    requestSampleLoad), WaveformView = stateless renderer over
    acquireLoadedSample() + uiStateView() using ui::WaveformGeometry
    (peaks, slice markers, wheel = anchored zoom, drag = pan, both
    persisted through setVisibleWindow → UiState). Idle timer detects
    sample-slot changes (loads from state restore too) and invalidates.
  - CRITICAL FIX found on the way: the module exported no ModuleEntry/
    ModuleExit (hosts would refuse to load it!) — `linuxmain.cpp` now
    compiled via `nedit_plugin_entry` OBJECT lib (factory.cpp +
    linuxmain.cpp) shared by the module AND the test binaries (VSTGUI's
    ModuleInitializer references GetPluginFactory, so tests need real
    module semantics). dlopen(RTLD_NOW) + ModuleEntry + factory smoke
    test passes.
  - src/plugin added BEFORE tests/ in the root CMakeLists (test targets
    reference the entry objects).
  - VSTGUI antialiasing is OFF by default on Linux: `CDrawMode` defaults
    to `kAliasing` (value 0), which maps to `CAIRO_ANTIALIAS_NONE` in
    `cairographicscontext.cpp:doInContext()`. Must call
    `dc->setDrawMode(kAntiAliasing)` before any `drawGraphicsPath` or
    `drawEllipse` call that needs AA (logo dots, arcs, custom shapes).
    The backend supports it — the flag is just not set. Restore
    `kAliasing` after to avoid AA on subsequent rect fills.
  - Active toolbar CHROME is deliberately muted: `kAccentMuted` (active
    outlines) and `kAccentMutedHi` (active value readouts) are desaturated
    salmons, softer than the full `kAccent`/`kAccentBright`. The full
    accent stays for FILLS (button fills, slider track fills) and the
    playhead; only 1px frames and small readout labels use the muted pair.
  - CTextButton has no `setBackColor` — background is controlled via
    `setGradient(CGradient*)`. A single-stop gradient (same color at 0
    and 1) acts as a solid fill. `GradientColorStopMap` is
    `std::multimap<double,CColor>` — use `emplace()`, not `operator[]`.
    Two visual states only: normal and highlighted (pressed); no separate
    hover colour.
  - Toolbar strip (second 48px band under the app bar): document-level
    controls left-aligned, each a self-contained CControl drawn by us
    (caption + shell inside its own 48px band; controls are direct frame
    children so mouse coords are frame-space — subtract getViewSize()).
    "BARS" stepper (±, 1..16, tag 103 = kParamLoopLengthBars), "BPM"
    drag-to-scrub field (tag 102 = kParamManualTempoBpm), "OVERRIDE"
    toggle (tag 101 = kParamManualTempoEnabled), "SENS" sensitivity
    slider + "QUANTIZE" auto-onsets toggle (editor-local tags 1002/1003 —
    NOT params: they are structural ANALYSIS edits that re-run detection/
    slicing, like trim/manual markers). IMPORTANT: don't confuse the two
    quantizes — `SampleState.quantizeTransients` (this toolbar toggle)
    snaps DETECTED AUTO onsets onto the note-value grid while slicing;
    `RenderState.beatQuantizeTimeStretch/Repitch` (Slice Length playback
    pick-length quantization) is a PLAYBACK thing exposed with the
    style-param sliders later. Sensitivity 0 → zero onsets by contract
    (single trim-span slice); quantize only ever MERGES auto onsets onto
    shared grid lines (never adds boundaries) and never moves manual
    points. Both route through `NeditProcessor::setSensitivity` /
    `setQuantizeTransients` → `rebuildSlicesPreservingWeights()`;
    both repaint the waveform live (draw() re-acquires slices, so
    `waveformView_->invalid()` suffices — no full refresh needed).
    Layout decision: STATIC —
    no widget swapping on toggle; when override is OFF the BPM field is a
    greyed read-only label showing the derived tempo
    (engine::tempo::calculatedOriginalBpm); when ON it becomes active and
    horizontal drag scrubs (one field-width = 80 BPM, Shift = 0.1x, wheel
    = 1/0.1 BPM, clamped 30..300). Enabling override SEEDS the BPM param
    to the currently-derived value first — a fresh never-scrubbed
    manualBpmOverrideValue of 0 means "no tempo" to the engine, so
    toggling ON must start from something real. All edits route through a
    new `setParam(id,norm)` (begin/setParamNormalized/performEdit/endEdit)
    extracted from applyParamFromControl; an idle-timer
    syncToolBarControls() pushes state → controls on change (host
    automation / chunk loads surface without user edits). CControl-derived
    custom controls MUST implement the pure `newCopy()`. CControl members
    `listener`/`tag` shadow under -Wshadow — rename ctor params.
  - Quantize-grid dropdown (editor-local tag 1004): a themed COptionMenu
    next to QUANTIZE listing the kNoteValues palette in order, so its
    0-based entry index == quantizeGridIndex; getValue() == selected
    index (COptionMenu raw value is the index, not 0..1; setValue pushes
    programmatically without a valueChanged echo). Maps to
    `NeditProcessor::setQuantizeGrid` → rebuildSlicesPreservingWeights().
    No separators (getValue again = platform result.index); theming via
    CParamDisplay setters (setBackColor/setFrameColor/setFont/
    setFontColor/setHoriAlign — drawBack does a flat fill + frame stroke
    for style 0; entry list is platform-themed).
  - Fade sliders (editor-local tags 1005/1006): two `ui::FadeSlider` controls
    right of the quantize dropdown — "ATTACK" = `render.fadeInMs`,
    "RELEASE" = `render.fadeOutMs`, both 0..100 ms (matches the original's
    continuous Fade In/Out sliders). Caption row doubles as the ms readout
    (label left, value right in `kAccentBright`); the box is a SENS-style
    fill-track slider (drag anywhere = set, wheel = 5 ms, always active —
    valid without a sample). Route through `NeditProcessor::setFadeInMs /
    setFadeOutMs` (clamped, publish only — no slice rebuild). The engine
    reads these into `Scheduler.cpp`'s BlockContext per pick
    (`state.render.fadeInMs/1000 * sampleRate`), so NEW picks get the new
    fades; the renderer clamps each to half the pick length (declick
    guarantee). Not automatable; if that's wanted later the state edits
    already exist and promotion is trivial.
  - Repitch/time-stretch toggle (editor-local tag 1007): CTextButton
    "TIMESTRETCH" right of the fades — accent-filled when
    `render.pitchMode == timeStretch`, graphite outlined when repitch; the
    caption ALWAYS shows the current mode (REPITCH vs TIMESTRETCH) via
    `CTextButton::setTitle` in `stylePitchButton()` (the title mirrors the
    model, never the button value).
    `pressedEdge` latch → `NeditProcessor::setPitchMode` (publish only).
    `Scheduler.cpp` folds pitchMode into `BlockContext::timeStretchMode`
    per pick, so NEW picks get the new mode; PickRenderer branches on it
    (granular vs repitch-rate). Layout note: BARS bar-stepper is 0.75x
    (66px), BPM field and quantize-grid dropdown are halved (56/55px), and
    the two FadeSliders stack vertically — each a ~20px band with the
    label + ms readout on the top line and a thin accent track along the
    bottom (whole-control drag sets the value; wheels ±1 ms).
  - Grain size/speed sliders (editor-local tags 1008/1009): two compact
    `ui::GrainSlider`s (FadeSlider layout, parameterized min/max/format/
    wheel-step + an `active_` gate) stacked to the RIGHT of the pitch
    toggle — SIZE (20..150 ms, wheel 5) above SPEED (1..8 x, wheel 0.25).
    ENABLED ONLY in time-stretch mode (`pitchMode == timeStretch`); in
    repitch they are dimmed and reject mouse input (`setActive(false)` →
    `setMouseEnabled(false)`, dashed-outline grey draw). Publish-only
    setters `NeditProcessor::setGrainSizeMs` / `setGrainSpeed` (no slice
    rebuild). Engine wiring: `RenderState.grainSpeed` (NEW state field,
    serialized v2 — the format version bumped 1→2; v1 chunks keep the
    default) flows to `BlockContext::grainSpeed`, and PickRenderer's
    timestretch branch divides the rate-matching source hop by it
    (`grainSourceHop = <rate-matching hop> / grainSpeed`). Default 1.0 is
    IDENTITY — bit-identical to the pre-grainSpeed render — higher speeds
    re-granulate the same source region more times (choppier character).
    Tape-stop keeps its own decel and the Stretch style overwrites with
    its own grain speed; neither is affected.
    CTextButton defaults to kKickStyle and its `onMouseUp` fires
    `valueChanged` TWICE per click: once with the flipped value (max —
    the actionable press), then it resets to min and echoes a second
    `valueChanged` at 0. Gating on `getValue() > 0.5f` dedupes to one
    action per press, but a handler that toggles on EVERY valueChanged
    double-toggles (stuck off), and a handler that always writes
    `setParam(…, 1.0)` regardless of model state double-applies ON (stuck
    on). Robust rule: use `pressedEdge(control, latch)` (rising-edge
    detection on the button value) and derive the new state from the live
    model, never from the button value.
`NeditProcessor::setGrainSizeMs` / `setGrainSpeed` (publish-only — no
    slice rebuild, but changing them mid-play needs a fresh pick).
- Tab bar (performance pages): 48px-tall strip (spec deviation from the
  design-language default of 32) below the waveform (y 192..240), four
  container-less underline tabs GENERATE/SEQUENCER/CONTROL/PERFORMANCE in
  `UiTab` ordinal order (generate=0..perform=3, matches user order so
  value = ordinal/(kTabCount-1)); active = kAccent label + 2px kAccent
  indicator, inactive kTextSecondary, hover kSurface2 pill, click selects
  via tag 1010 → `NeditProcessor::setActiveTab` (publish-only, clamps >3);
  `PanelView` card below (x 24..936, y 248..792, surface-1 rounded rect
  via a `drawRoundedRect` CGraphicsPath helper — CDrawContext has NO
  drawRoundRect, and rounded shadow needs kAntiAliasing like the logo
  dot) re-renders per page from `uiStateView().ui.activeTab`; currently a
  placeholder skeleton, per-mode panels (style params / sequencer grid /
  keyswitch map / slot bank) to follow. VSTGUI facts: onMouseEntered/
  Exited return CMouseEventResult (kMouseEventNotHandled); CView has no
  default ctor — pass a zero CRect. Host state restores + user clicks both
  converge: syncTabBar() pushes state→bar on change (dedup lastTabSync_).
- Style-probability band (Generate + Sequence tabs, shared): a 208px band
  across the panel card's inner area divided into 9 columns
  (`kStyleBandH=208`, column width = bandW/9). Each column is a
  `ui::StyleProbSlider` (tags kTagStyleProbBase 1011 + PlaybackStyle
  ordinal) drawn as a top-centred label chip (background = the style's
  palette colour dimmed toward the card surface, text brightened) + a
  vertical weight slider ALIGNED LEFT spanning the remaining height.
  Value = raw `GenerateState.styleWeights[i]` weight in [0,1], fill
  upward, `kAccentBright` tick at the value, % readout right of the bar.
  Only the 8px slider track is a hit target for drags (chip/% /param-list
  clicks fall through; a grabbed drag keeps adjusting off-track), wheel
  ±0.05; routes through
  `NeditProcessor::setStyleWeight` (publish-only, clamps). Sliders are
  frame siblings layered over PanelView (which draws the card + divider
  + a "pending" hint below the band) and setVisible(false) on the other
  two tabs; syncStyleProbs() dedup-pushes state→controls.
  Per-style param layout (generalized from the Flanger probe, 2026-08-28):
  EVERY column hosts its style's applicable params right of the vertical
  weight slider, under the % readout. Rows come from `columnParamsFor`
  (derived at runtime from `applicableStyleParams`): **Subdivide dropped
  everywhere** (a sequencer retrigger, not a style effect) and **Volume
  Mode kept out** (Volume is a bare slider in every column); each `swept`
  base param gets its paired `*Mode` sibling (info.swept => next id)
  inserted DIRECTLY UNDER it (Base slider, then Base Mode menu). Derived
  lists: forward 1 (Volume), pingPong/tapeStop 2 (Curve Shape menu +
  Volume), stretch 3, filterDown/up 3, bitcrush 5 (SR Res + mode, Bit
  Depth + mode, Volume), scratch 4 (3 easing/note-value menus + Volume),
  flanger 7 (max — Delay/Mix/Feedback + modes + Volume). Continuous params
  get a 12px caption row drawn by the column + a horizontal
  `ui::ParamMiniSlider` under it; discrete params are `ui::ParamMiniMenu`
  dropdowns owning their caption row entirely (column draw SKIPS them).
  Layout geometry: shared file-scope `kProb*` constants + per-style
  `StyleParamRow` entries; `StyleProbSlider::captionRowLocalY(i)/
  sliderRowLocalY(i)` step the rows (caption 12 + 1px gap + slider 10 +
  5px inter-entry padding) and BOTH the column's caption draw and the
  editor's control placement call them — they can NEVER drift. The editor
  keeps 2D arrays `paramMiniSliders_/paramMiniMenus_[style][row]`
  (`kParamMiniRowCount = 7` = the flanger max), tags = StyleParamIds
  (< 1000, ParameterSurface ids), so user edits flow through the host edit
  protocol (applyParamFromControl -> setParam) and host automation
  converges via `toNormalized` in syncStyleProbs (dedup against control
  value). VOLUME ALIGNMENT: only each column's FINAL row — Volume, which
  ends every column — parks on the SAME row as the Flanger column's
  Volume slider (the Flanger list is the longest, so its Volume row is
  the reference); all OTHER per-style rows stay top-anchored in their
  own list exactly where they were. `captionRowLocalY/sliderRowLocalY`
  branch on the last row (`i + 1 == rows_.size()`) to the aligned slot
  (`alignedVolumeSliderY/CaptionY` via the cached `flangerParamRows()`
  reference); captions and controls derive their Y from the same helpers,
  so volumes line up across the bottom while each style's params keep
  their original positions above. SCOPE SELECTORS — RECALLED 2026-08-29
  into the Generate timing ribbon's Clock options (see the timing section
  below; the Tape Stop/Filter columns were slimmed one row and
  `StyleParamRow::isScope()` removed). They are editor-local discrete rows
  (tags 1020/1021, `StyleParamRow` generalised to `tag` where < 1000 =
  StyleParamId, >= 1000 = editor-local selector, driven by publish-only
  `NeditProcessor::setTapeStopScope/setFilterSweepScope` on
  `GenerateState.tapeStopScope/filterSweepScope`; `paramRowOptionCount/
  Name` and `scopeSelectorNorm` bridge the menus; NOT automatable — the
  original gated these on clock mode and our Clock scheduler honours
  them, the other trigger modes pass fixed per-window values). Inserted
  before Volume, so the volume-last invariant holds. Mini-slider readouts: raw value via styleParamInfo
  (Delay `%.1f ms`, Grain Size `%.0f ms`, Grain Speed `%.1f x`, else
  `%.2f`); drag maps to the TRACK width only (the 32px value readout is
  not part of the fill, so 100% stays at the track's right edge), wheel
  ±0.05. No thumb — the accent track fill IS the indicator, so slider rows
  are thinner (10px) than the 12px captions. `ParamMiniMenu` = a 12px
  COptionMenu subclass: option text as the row's only label in
  kAccentMutedHi + drawn caret triangle + hairline underline, no 19px menu
  chrome; popup publishes entry index through the same host-edit protocol.
  Both control types are frame siblings layered over the prob column
  (added later => on top for hit-testing; the column's x-gated track
  hit-test keeps them disjoint). Note for lead-dev: Volume currently does
  nothing on this tab (SL/Clock modes — static volume is Sequenced-only),
  so it may get dropped from the layout; decided at the user's word.
  PAINT OVERLAY (2026-08-28): a left-drag in a probability column ANYWHERE
  off the thin vertical track starts a paint gesture — every column draws
  a COLUMN-WIDTH bar (overlay in the manner of the waveform layers; the
  param mini-controls step aside until release) and the column under the
  pointer gets a kAccentBright outline. The pointer stays captured by the
  column that took the press, so crossing columns paints each at the
  pointer's height as you go; values fall through stylePaintTo ->
  valueChanged -> setStyleWeight (same publish path as the track drag);
  release hides the overlay and restores the mini-controls. Track presses
  keep the precise single-column drag. Geometry is pure and tested:
  `src/ui/ProbBandGeometry.h` (`probColumnFromX` clamps to the 9 columns,
  `probValueFromY` = top→1.0/bottom→0.0 clamp, degenerate band/width
  safe) shared by the columns' applyFromY and the paint mapping; the
  paint col is recovered from the captured column's own rect (bandLeft =
  column left − ordinal × colW).
- Generate timing ribbon (2026-08-29), below the style band on the
  GENERATE page only, order = mode switch → full-width per-mode options →
  interval probability sliders. REDESIGNED the same day per lead-dev
  feedback: the mode switch is now a FULL-WIDTH segmented stick and the
  option row spans the whole panel; the Tape Stop + Filter sweep scope
  selectors were RECALLED off the style columns into this section (they
  ride with the Clock options):
  - Mode switch: two CTextButtons "SLICE LENGTH"/"CLOCK" filling the panel
    width (two segments, 1px seam, tags 1022/1023 = `kTagGenerateModeSL`/
    `kTagGenerateModeClock`, public constexprs in NeditEditor.h so tests
    can drive them), styled by `styleModeButtons`/`styleModeSegment` (same
    palette rules as the REPITCH/TIMESTRETCH toggle: the segment matching
    `GenerateState.generateMode` is accent-filled, the other quiet).
    Handler = pressedEdge + live-model compare (re-selecting the active
    mode is a no-op) → publish-only `setGenerateMode` (accepts ONLY
    sliceLength|clock; sequenced/performance/control rejected — Generate
    hosts just its two random sub-modes) → **`syncGenerateControls()` in
    the same call stack**, so the whole section (segment fills, greys,
    interval band) reacts on the FIRST click, nothing waits for the idle
    tick. Explicit first-click guarantee, not just `lastResetBarsSync_`
    catch-up. **The segment switch drives the SCHEDULER-FACING mode too**:
    `setGenerateMode` also writes the top-level `uiState_.triggerMode` (the
    two Generate sub-modes ARE the sliceLength/clock entries of the
    scheduler's trigger-mode switch; there is no separate "generate"
    wrapper — this is the "mode change doesn't affect the audio" fix), and
    the `kParamTriggerMode` surface fold mirrors it (top-bar menu ⇄ ribbon
    stay in step). Coverage: `shell: generate timing setters persist in
    GenerateState` asserts `setGenerateMode` moves `triggerMode` with it;
    `surface: non-style params` asserts the param fold keeps
    `generate.generateMode` in step.
  - Option row (all `ParamMiniMenu`s, disabled by the mode selection via
    `setGreyed` grey palette + setMouseEnabled(false)): each menu is enabled
    EXACTLY when its mode is selected — the mapping is a pure,
    unit-tested `TimingGreyState`/`timingGreyState(mode)` (RESET EVERY
    enabled under SL, Clock trio enabled under Clock; a 2026-08-29 inversion
    bug greyed the trio under Clock instead and is covered by
    `timing ribbon: option menus enable exactly with their mode`):
    SL = "RESET EVERY" (tag 1024, entries = `kResetBarsValues`
    {1,2,4,8} bars, default index 2) → `setResetBars`,
    greys whenever Clock is active.
    Clock = "CLOCK REFERENCE" (tag 1025, entries = full `kNoteValues`
    palette label list) → `setClockReference`, greys under SL.
    Clock = "TAPE STOP SCOPE" (tag 1020) and "FILTER SWEEP SCOPE"
    (tag 1021) — the recalled expander selectors, still reading/writing
    `GenerateState.tapeStopScope/filterSweepScope` through the same
    `valueChanged` dispatch + `ui::scopeSelectorNorm` used before; they
    grey under SL (the engine honors the scopes only in Clock). Entries
    auto-fill from `paramRowOptionName` via their tags (2 options).
    Entries added via `COptionMenu::addEntry` AFTER construction (generic
    path).
  - `IntervalProbBand` (tag 1026): 20 thin vertical probability bars, one
    per `kNoteValues` slot (labels below), value = raw
    `subdivisionWeights[i]`, paint gesture over the whole band (press/drag
    sets the column under the pointer, bright outline while dragged),
    wheel ±0.05 on the column under the cursor. Clock-mode ONLY: under
    Slice Length the band greys with an "(CLOCK MODE ONLY)" caption and
    rejects input. Writes route through the editor's valueChanged →
    `activeColumnIndex()` + getValueNormalized() → `setSubdivisionWeight`
    (clamps to [0,1], ignores invalid palette indices). Reads state via a
    `NeditProcessor*` owner (PanelView pattern), NOT the editor — only the
    processor exposes `uiStateView()`.
  - Subdivision quick-clears (tags 1027 "n=0" / 1028 "nd=0" / 1029 "nt=0",
    public `kTagClearPlain/Dotted/Triplet` in NeditEditor.h so tests drive
    them): three small CTextButton chips on the interval band's caption row
    (right side; the band's own `kCaptionH` resolves to the file-scope
    `kIntervalCaptionH` so the rows can never drift). MOMENTARY actions —
    each press calls `setSubdivisionGroupZero(NoteValueVariant)` which zeroes
    that variant group's weights for real (one publish, no-op if already
    zero, invalid enum values rejected). No persisted toggle state:
    re-enabling is just painting values back. Variant membership is the
    constexpr `kNoteValueVariant[20]` table in `state/Types.h` (plain
    {128n..1n}=8, dotted {64nd..2nd}=6, triplet {32nt..1nt}=6), unit-tested
    against the palette's name suffixes. The chips are enabled EXACTLY when
    Clock is (they act on CLOCK-mode-only weights), styled/enabled via a
    `lastZeroChipsEnabled_` latch + `styleZeroChips()` (accent-muted text on
    surface-2, mouse-enabled off + flat grey under SL), hidden on non-Generate
    tabs by syncTabBar. valueChanged uses the unconditional pressedEdge rule
    (like the mode switch — the CTextButton min echo resets the latch, a
    short-circuit leaves one dead click per pair), with a per-chip latch;
    band invalidated on the first click for immediate repaint.
  - Geometry: band top 260 + 208 ⇒ mode segments y 484 (36px, segW =
    bandW/2), option menus y 530 (each 36px, caption row 12px + option
    24px, 4 × 210px wide with 16px gaps), interval band y 582..780
    (panel inner bottom = kEditorHeight − kPanelGutter − kCardPad).
  - `syncGenerateControls()` (idle hook alongside syncTabBar/
    syncToolBarControls) dedup-pushes state→controls (latches
    `lastGenerateModeSync_`, `lastResetBarsSync_`, `lastClockRefSync_`,
    `lastResetBarsGreyed_`/`lastClockRefGreyed_` + the scope pair
    `lastTapeScopeSync_`/`lastFilterScopeSync_` (float, sentinel -1.0) and
    `lastTapeScopeGreyed_`/`lastFilterScopeGreyed_`,
    `lastSubdivWeightsSync_` — a sentinel -1.0 fill forces the first
    weights repaint since real weights are ≥ 0).
  - `SyncTabBar` hides all seven timing controls on non-Generate tabs;
    PanelView's GENERATE "style parameters (pending)" hint was REMOVED
    (the timing ribbon fills the space) — the Sequencer "step grid ·
    pattern controls (pending)" hint remains.
  - `paramRowOptionCount`/`paramRowOptionName` now range-guard tag <
    kNumStyleParams before indexing the option table — a 1024/1025-class
    editor-local tag previously fell through to an out-of-bounds
    `styleParamInfo[id]` std::array index (real UB; fixed).
  - `columnParamsFor` no longer emits scope rows (Tape Stop/Filter columns
    slimmed one row; the `StyleParamRow::isScope()` helper was removed);
    `syncStyleProbs` assumes every mini-menu is a StyleParamId.
- Per-style colours: `kStyleColours` mirrors the original's
  PlaybackStylePalette::getStyleColour (forward orange, pingPong purple,
  tapeStop dodgerblue, stretch teal, filterDown red, filterUp gold,
  bitcrush limegreen, scratch hotpink, flanger cyan).
- Live DAW testing (Bitwig) round 1 — findings + fixes, all committed:
  - Bundle binary must be named <bundle>.so (lib*.so => "Not a plug-in
    file"); bundle needs Contents/Resources/moduleinfo.json (generated at
    build time by nedit_moduleinfotool from the real factory); deploy
    target installs to ~/.vst3 as a REAL dir (symlinked bundles flaky,
    and ~/.vst3 is scanned VST3-only). Bitwig caches scan verdicts in
    ~/.BitwigStudio/cache/vst{,3}-metadata-*; its "VST 2.4 metadata"
    errors about the inner .so are harmless noise from recursing into
    custom locations that point AT a bundle.
  - Editor crash on open: two Linux-only requirements — (1)
    VSTGUI_NEEDS_INIT_LIB=1 must be defined on nedit_plugin (SDK's plugin
    cmake helpers normally set it; we bypass them) so vstguieditor.cpp
    calls VSTGUI::init() and registers the runloop host-context callback;
    (2) CFrame::open() needs an X11::FrameConfig carrying the host run
    loop (wrap IPlugFrame via Steinberg::Linux::setupVSTGUIRunloop, then
    pass linuxFactory->getRunLoop()). Without either: null xcb connection
    -> xcb_generate_id SIGSEGV.
  - Load-dialog desktop freeze: VSTGUI's Linux CNewFileSelector blocks
    the UI/run-loop thread on a pipe read while zenity runs -> embedded
    X11 window stops servicing events -> X server wedges. Fixed by
    running zenity/kdialog via popen on a DETACHED background thread with
    shared_ptr state; result marshalled to UI on the idle timer. Also:
    begin/endEdit overrides filter non-parameter tags (Load button tag
    1000 was reaching IComponentHandler).
  - "Loud noise" output: renderer ADDs into host buffers (outAdd +=
    ...), hosts don't zero them. process() now clears outputs first;
    harness poisons buffers pre-process to prove it.
  - Host automation VERIFIED end-to-end (real IParameterChanges double ->
    fold -> engine): manual-tempo override audibly repitches (240bpm =>
    half-rate, zcr halves; disable restores). IMPORTANT DESIGN FACTS
    (faithful to original, see its volume-gain comment): most style
    params are per-STYLE (filter->FilterDown/Up, grain->Stretch,
    crush->Bitcrush, flanger->Flanger, scratch->Scratch); static Volume
    is SEQUENCED-mode-only ("no global dial" for SL/Clock); default
    weights = Forward x1.0 => in default SL+Forward almost NO knob
    affects audio except trigger mode / manual tempo / loop bars.
    PENDING LEAD-DEV UI DECISION: expose styleWeights as automatable
    params (append-only IDs 108..116), optionally apply static volume in
    all modes as deviation, grey out non-applicable controls in UI.
  - Test hooks: NEDIT_TEST_FILE (skip dialog GUI), NEDIT_TEST_AUTOLOAD
    (one-shot runFileSelector), NEDIT_DBG/NEDIT_DBG2 (test stdout
    metrics). tools/host_harness.cpp drives full lifecycle incl. editor
    attach + XTEST clicks + WAV dump (NEDIT_DUMP_WAV).
- Next work: Phase 4b polish — style-param controls (21 sliders over
  ParameterSurface ids 0..20), playhead/step mailbox → UI, sequencer grid
  view; then live DAW testing (Phase 5).
- BUG FIX (2026-08-27): slice playback ignored the trim while the host was
  playing. ROOT CAUSE: the view keeps a SOFT trim (trim drags do NOT rebuild
  the slice list — slices outside are hidden, widening reveals them with
  weights intact), but `setTrimFrames` was published state-only, so the
  audio-thread slot still held the pre-trim slice list and the scheduler
  played it verbatim — outer slices sounded. FIX: the engine mirrors the
  soft trim. `NeditProcessor::process()` clips the shared slice list to
  [trimStart,trimEnd) every block via the pure `clipSlicesToTrim(...)`
  (slices OUTSIDE dropped; a slice straddling a handle cut to the trim;
  rarely degenerate zero-length overlaps skipped) and remaps
  `generate.sliceWeights` in lockstep into pre-reserved scratch
  (`trimSlices_`/`trimWeights_`, reserved in the ctor — no audio-thread
  allocation in steady state). The scheduler gained an optional
  `sliceWeightsOverride` param to `process()`/`Run` (nullptr = default
  `state.generate.sliceWeights` path, so engine tests unchanged); all modes
  (Slice Length/Clock/Sequenced/Performance/Control) pick from the clipped
  list since they all use `r.slices`. MIDI Control's availableSlices count
  is now the trimmed count too. Regression tests: `clipSlicesToTrim` unit
  cases (identity on full-span trim, handle-straddle cuts, wholly-outside
  clips to silence, default-weight padding) + scheduler override plumbing
  (empty override ⇒ zero picks despite full state weights; matching
  override ⇒ byte-identical pick count as the default path).
- CRASH FIXES (2026-08-29, code review):
  1. **Audio-thread publish removed from the audition auto-stop.**
     `process()` used to do `uiState_.ui.auditionEnabled = false; provider_
     .publish (uiState_)` when the transport started while auditioning —
     publish() deep-copies the whole PluginState (manualPoints,
     sliceWeights, grids, banks) while the UI thread may be reallocating
     those exact vectors ⇒ heap corruption, plus allocation on the audio
     thread. Now: process() gates audition rendering on the LIVE transport
     (engine takes over the same block) and only raises
     `auditionAutoStopPending_` (atomic); the editor idle timer drains it
     via `NeditProcessor::pollAuditionAutoStop()` (UI thread owns the fold).
     Related contract repairs: renderAudition/renderSliceAudition now take
     the per-block snapshot's SampleState + the already-acquired
     LoadedSample (they used to read `uiState_` and re-acquire the slot on
     the audio thread); the audition/slice-audition read cursors are
     AUDIO-thread-owned, reseeded on the inactive→active edge inside
     process() (UI writes to them could tear); slice-audition bounds are
     atomics stored BEFORE the active flag's release-store.
  2. **WavDecoder trusted `blockAlign`.** Only `!= 0` was checked; a fmt
     chunk lying about the frame width (e.g. stereo pcm16 claiming
     blockAlign 1, or bits widened without the frame following) inflated
     `frames = dataSize / blockAlign` and read past the end of the data
     chunk — heap OOB on attacker-controlled file input, unreachable by
     the truncation fuzz (needs internally INCONSISTENT fields, not a
     short file). Now rejected when `blockAlign < numChannels *
     bitsPerSample/8` (also guarantees the per-channel stride ≥
     bytesPerSample, so every convertSample read stays inside its frame);
     wider-than-needed (padded) frames stay legal.
  3. **Hostile host output buses.** The buffer-zeroing loop null-checked
     channel pointers but the render paths then wrote through them blindly,
     and a zero-channel bus was promoted to 1 (indexing
     `channelBuffers32[0]` of a possibly empty table). process() now
     validates the bus once up front (bail on empty/null table, clear valid
     channels, bail if ANY channel is null) and passes the true channel
     count through; audition read indices additionally clamp at 0.
  Regression tests: lying-blockAlign/bits fmt cases + padded-frame
  legality (test_sample_pipeline.cpp), audition auto-stop defers the state
  fold to pollAuditionAutoStop while the engine takes over immediately,
  slice-audition render/stop, null-channel-while-sounding bails with zero
  picks, hostile bus shapes return kOk (test_nedit_processor.cpp).
- HARDENING (2026-08-29, follow-up sweep — all latent, none reachable
  through sanitize()-guarded paths today):
  - Scheduler: `resetBarsIndex` clamps into kResetBarsValues at the point
    of use (was the ONE unguarded state index in the file — a negative
    index cast to size_t would wild-read a 4-entry constexpr array, then
    divide by garbage); invalid `clockReferenceIndex` falls back to the
    4n reference instead of noteValueBeats' 0.0 sentinel (windowBeats is
    a divisor → floor(x/0.0) int64 cast is UB; siblings already guarded
    with max(..,1e-6)).
  - `releaseWorkingPatternOverride()` (never yet called) REPLACED by
    `requestWorkingPatternRelease()`: atomic request, drained at the top
    of the next process() block. A direct UI-thread reset() would have
    been a use-after-free the day the shell grew a caller (see the
    Sequenced chunk note above).
  - MIDI note-on velocity clamped to [0,1] BEFORE the uint8 cast
    (float→uint8 of >~2.0 is UB per conv.fpint).
  - PickRenderer/GranularStretcher read positions clamp in DOUBLE space
    before the int64 cast (`clampedSourceIndex`: NaN/±inf/negative → 0,
    ≥frames-1 → last frame) — the cast itself was the UB, so clamping
    after it guarded nothing.
  - WaveformView::draw no longer copies the whole slice vector per frame
    (the `cond ? lvalue : prvalue{}` ternary materialized a prvalue copy;
    now binds a static empty vector).
  - NeditEditor::open's timing-section seed sync ran BEFORE intervalBand_
    existed (dead call — the guard early-returns while any timing control
    is null); moved after the quick-clear chips, where all controls exist.
  Tests: sequenced working-pattern release defers to the next block and
  reverts to the working grid; out-of-range resetBars/clockRef indices
  schedule normally with a finite window clock (test_scheduler.cpp).
- Test totals: default build 212/212 (54 state + 140 engine + 18 ui);
  plugin build 265/265 (54 state + 140 engine + 18 ui + 53 plugin:
  state round-trip gained a v1 forward-compat case,
  the editor gained active-tab + style-weight persistence cases, the
  Generate mode-switch gained a click-replay test proving the FIRST click
  flips the mode via the real valueChanged handler, the palette gained a
  plain/dotted/triplet partition check, the subdivision quick-clears
  gained a click-replay + group-zero test, and the sequencer gained the
  slider-params + scratch-cycle cases — see test_scheduler.cpp below;
  the per-cell override setters gained clean tests (clamping +
  StyleParamId keying + empty-cell/invalid-id rejection + clear pruning)
  in test_sample_pipeline.cpp — see below;
  the sequencer transport setters gained a clean test (index persistence +
  out-of-range rejection + grid resize on pattern-length/grid-interval
  dimension changes, rows untouched) in test_sample_pipeline.cpp;
  the CLEAR/RANDOMIZE buttons gained a click-replay test (real valueChanged
  dispatch: Clear empties the grid, Randomize keeps it monophonic/in-range
  then Clear empties again) in test_sample_pipeline.cpp;
  see `tests/plugin/test_nedit_processor.cpp`; the step grid added the ui
  geometry suite + the sequencer cell-setter/dims cases + the per-cell
  override-menu classification suite (submenu vs mode-submenu vs slider
  per applicableStyleParams) — see below), zero warnings.
- Live DAW testing (Bitwig) round 2 — step-grid follow-ups (2026-08-30):
  - **STALE DEPLOY GOTCHA (re-confirmed).** "Sequencer tab still empty" was
    a stale `~/.vst3` bundle predating the grid, NOT a code bug. `cmake
    --build build` refreshes `build/nedit.vst3` but the DAW loads
    `~/.vst3/nedit.vst3` — you MUST `cmake --build build --target deploy`
    (and rescan; Bitwig caches verdicts in
    `~/.BitwigStudio/cache/vst3-metadata-*`). Always deploy after editor
    changes before asking the user to retest.
  - Empty-state grid: `SequencerGridView::drawEmptyHint` now draws a
    placeholder 16×4 lattice + a centred "SEQUENCER — load a sample…"
    caption so the tab clearly reads as a grid before a sample exists
    (was a near-invisible inset box).
  - Tab switch is now immediate: the `kTagTabBar` valueChanged handler
    calls `syncTabBar()` directly (was idle-timer only).
  - **Grid paint toggle BUG FIXED.** Single notes toggled on at mouse-down
    then back OFF at mouse-up. Cause: `applyPaintAt` used a per-cell toggle
    (`cur==paintStyle_ ? -1 : paintStyle_`) AND `onMouseUp` re-applies at the
    release cell via `onMouseMoved`, so the release re-toggled the just-
    painted cell off. The paint/erase ROLE is already decided once at the
    press (`erase_`), so `applyPaintAt` now just writes `paintStyle_` (paint)
    or `-1` (erase) with no per-cell toggle; `setSequencerCell` already
    treats writing the same style over itself as a no-op, so the mouse-up
    re-apply is harmless. Notes stay put.
- Sequencer scratch BUG FIXED (2026-08-30) — "scratch broken, only in
  sequencer mode". Real cause: `VoiceScheduler::mergeCell` (the Sequenced
  step path) gave EVERY style the same `pickLength =
  min(declaredLengthHostSamples, samplesUntilNextActiveStep)` (the grid
  step's natural duration), so a scratch cell's declared window was its
  step length instead of ONE Rate cycle — the whip never completed within
  its window (the "grainy blip" sound). The Clock/Performance paths always
  used `pickLength = scratchCycleHostSamples` for scratch (capped by their
  tick/segment window); the Sequenced path was the only straggler. FIX:
  scratch now declares `min(scratchCycleHostSamples,
  samplesUntilNextActiveStep)` in `mergeCell` too (the anticipatory-fade
  cap still applies). The lead-dev's "dropdown params default null" lead
  was a red herring: `pick.params = params` is a full copy, the sequencer
  merges valid fallbackParams (`scratchRate`=16n, curves=linear, all
  sanitized), and the mini-dropdowns sync state→menu via `toNormalized`/
  `COptionMenu::setValue`. Regression test: "sequenced: scratch plays one
  Rate cycle, not a grid step" (asserts pickLength == the tempo-computed
  cycle, ~5512 samples, and NOT the 44100 natural length).
- Sequencer follow-ups (2026-08-30, lead-dev trio — "dropdowns blank, scratch
  sounds like fwd/bkwd not pitch, notes ignore the sliders"):
  - **BLANK MINI-DROPDOWN FIXED.** Every `ParamMiniMenu` ("the dropdowns
    don't populate until we change them") started with VSTGUI's
    `COptionMenu::currentIndex == -1`, so `getCurrent()` returned nullptr
    and the row drew with NO label. `syncStyleProbs` only pushes when the
    state value differs from the menu's own value (dedup gate `abs(norm -
    getValueNormalized()) > 1e-3`), so every param whose state default is
    index 0 (all curve/mode/type dropdowns: linear, fixed, lowpass…) never
    got its first `setValue` and stayed blank forever. FIX: the
    `ParamMiniMenu` ctor now ends with `setValue (0.f)` — pre-selects the
    first entry so the row always shows the current option (currentIndex is
    never left at -1); sync then corrects non-zero state defaults. `setValue`
    does not echo a valueChanged, so the publish path is untouched.
  - **SCRATCH DEFAULT CURVES → Ease In-Out.** `scratchForwardCurve` /
    `scratchBackwardCurve` defaulted to `linear` (as the original), which
    scans the fold at constant speed and flips direction at the turnaround —
    reads as "fwd/bkwd position switching", not a pitch whip. Both defaults
    are now `EasingCurve::easeInEaseOut` (smoothstep): velocity 0 at each
    turnaround, speed hump mid-leg — "the speed hump a real scratch stroke
    traces" (see engine/Easing.h). Deliberate lead-dev deviation from the
    original's linear; the easeInEaseOut enum value is 3, so the info-table
    defaults for Forward/Backward Curve moved 0.0f → 3.0f (the info-table-
    agrees-with-struct-defaults test enforces this pair). The curve dropdowns
    populate now (see above), so further tuning is a menu pick away.
  - **SEQUENCER SPEAKS WITH THE SLIDERS.** Sequenced notes used
    `SequencerState.fallbackParams` as their merged base, so painting a
    flanger cell played the DEFAULT flanger even after the user had moved
    the band's sliders. `mergeCell` (Scheduler.cpp) now merges
    `generate.styleParams` (the Generate/Sequence slider+dropdown surface)
    + per-cell overrides on top. This is a READ coupling, not the original's
    aliasing: generate.styleParams has a single writer (the UI sliders), and
    cell overrides (incl. the randomizer's per-cell rolls) still win.
    `fallbackParams` stays serialized/sanitized but is no longer the audio
    default (comments updated in StyleParameters.h / SequencerState.h /
    Scheduler.h). Tests: "sequenced: cell overrides beat the generated
    slider params" (renamed; base is now generate.styleParams) + NEW
    "sequenced: notes without overrides use the generate (slider) params"
    (slider flangerDelayMs 8.5 flows into the pick even though the sequencer
    fallback says 1.0).
- Tab ⇄ trigger-mode coupling (2026-08-30) — "transfer control to a mode by
  selecting its tab". `TriggerMode` is FLAT (no "generate" wrapper — the two
  Generate sub-modes ARE the sliceLength/clock entries), so the tab↔mode map
  is 1:1 except the Generate tab, which resolves through
  `generate.generateMode`. Two orthogonal state fields stay in lockstep:
  `PluginState.triggerMode` (scheduler-facing, the ONLY thing the engine
  dispatches on) and `UiState.activeTab` (visible tab).
  - Single source of truth: pure `state::tabForTriggerMode(mode)` /
    `triggerModeForTab(tab, generateMode)` in `UiState.h` (tested in
    `test_state_defaults.cpp`: full map both ways + tab→mode→tab identity).
  - Tab drives mode: `NeditProcessor::setActiveTab` now also writes
    `triggerMode = triggerModeForTab(tab, generateMode)`. This is what makes
    clicking SEQUENCER actually run `runSequenced` (before, the tab was
    cosmetic and the engine kept the old mode — there is NO other in-editor
    control for sequenced/performance/control; only host automation of
    param 100 reached them).
  - Mode drives tab: the `kParamTriggerMode` fold (`ParameterSurface::
    applyNormalized`) now also sets `ui.activeTab = tabForTriggerMode(tm)`,
    so a host automating the mode moves the visible tab (editor's
    `syncTabBar` picks it up from the published state). `setGenerateMode`
    also pins `activeTab = generate` for invariant completeness (idempotent).
  - Editor: the tab-click handler routes through `setParam(kParamTriggerMode,
    …)` (host edit protocol: begin/setParamNormalized/performEdit/endEdit)
    rather than a publish-only `setActiveTab`, so the host RECORDS/reflects
    the change and can't later re-push a stale value that snaps the mode
    back; the fold sets triggerMode + the Generate sub-mode mirror +
    activeTab together. `setActiveTab` stays a complete standalone API (sets
    both fields) for tests/programmatic use.
  - Tests: `surface: non-style params` asserts activeTab follows the
    automated mode (control/sliceLength/sequenced); `shell: selecting a tab
    transfers control to that mode` asserts each tab sets its mode and that
    returning to Generate restores the remembered sub-mode.
- Sequencer step grid (2026-08-29, Sequence tab) — the piano-roll step grid
  is live (was a "step grid · pattern controls (pending)" placeholder):
  - `src/ui/SequencerGridGeometry.h` (nedit_ui, framework-free, tested in
    `tests/ui/test_sequencer_geometry.cpp`, 10 cases): PROCEDURAL sizing —
    `computeSequencerLayout(viewW, viewH, rows, cols)` spreads rows to fill
    the viewport when they fit at >= `kMinSequencerRowH` (14px), else fixes
    the min height and SCROLLS (`visibleRows`/`maxScroll`); piano-roll
    hit-testing (`rowFromBottomY` = row 0 at the BOTTOM, ceil-band mapping
    with the leftover top strip clamped to the top visible row;
    `columnFromX`); `naturalStepsForSlice` (slice duration quantized to the
    step grid) + `computeCellBars` (the tested bar-span spec: natural
    length raised by the per-cell extension, clamped to the next active
    column = the engine's anticipatory fade). Everything derives from the
    viewport + dims so the editor can retune sizes freely (no hardcoded
    pixels). GOTCHA baked into the tests: the palette's 16n = 0.25 beats
    (a 16th note), NOT 1 beat — natural-length math uses `kNoteValue4n`
    (1.0) for "1 step = 1 beat" reasoning.
  - `engine::seq::computeSequencerDims` / `resizeGrid`
    (`SequenceRandomizer.{h,cpp}`, +2 tests): the sequencer grid
    dimensions were NEVER derived before (the grid was 0x0, so the
    sequencer/randomizer couldn't function). rows = min(sliceCount,
    kMaxSequencerRows); columns = stepsPerBar(stepResolution) *
    patternLengthBars, capped at kMaxSequencerColumns. `resizeGrid` resets
    the grid ONLY when the dims change (the documented
    reset-on-dimension-change contract). `NeditProcessor::
    resizeSequencerGridForSample()` calls it on every sample load
    (publish-only; before the publish in requestSampleLoad).
  - `NeditProcessor` cell setters (UI thread, publish-only, +4 plugin
    tests): `setSequencerCell(row,col,style)` writes/clears a cell and
    ENFORCES MONOPHONY (writing a cell drops every other filled row in that
    column + its overrides/extensions); `setSequencerCellExtension(row,col,
    delta)` grows/shrinks the per-cell extension (clamped 0..256, erased at
    0, needs a filled cell); `setSelectedDrawingStyle(style)`. All
    `[[nodiscard]] bool` where a no-op returns false.
  - `ui::SequencerGridView` (NeditEditor.cpp): a `CControl` rendering the
    grid over `SequencerGridGeometry` + the live `SequencerState`/slices.
    Draws: inset panel, per-bar column dividers + faint per-beat shading
    (stepsPerBar = round(4 / stepBeats)), row separators, active cells as
    style-coloured bars spanning their declared length (mirrors
    `computeCellBars`), per-cell override triangle + extension tick, a
    live `kAccentBright` playhead column from
    `debugScheduler().playingStepIndex()`, and a scroll-knob hint when
    rows are clipped. Interactions: LEFT-drag paints
    `selectedDrawingStyle` (painting the same style erases — toggle decided
    once at press so the drag stays consistent), RIGHT-drag erases,
    SHIFT+drag extends the grabbed cell's declared length, WHEEL scrolls.
    Edits call the processor setters directly (UI thread); the grid never
    uses the host edit protocol (tag `kTagSequencerGrid` = 1030, editor-
    local, filtered by begin/endEdit). The geometry helpers live in
    `nedit::ui` (nedit_ui) but the editor's namespace is
    `nedit::plugin::ui`, so a using-block bridges them.
  - Threading: `VoiceScheduler::playingStepIndex_` is now
    `std::atomic<int>` (relaxed load/store) — it is read from the editor's
    idle timer while the audio thread crosses step boundaries; the plain
    int was a latent data race. The editor repaints the grid on the idle
    tick only when the playing step MOVES (or the sample changed);
    paint/extension edits self-invalidate.
  - Wiring: created in `open()` below the shared 208px style band (band
    bottom + 16px gap to the card inner bottom), `setVisible` toggled by
    `syncTabBar` (Sequence tab only).
  - Style-paint palette (2026-08-30): a thin 18px strip (`kPaletteH`) at the
    top of the grid view, 9 columns aligned to the probability band above
    (same bandW/9, same left — view rect == band rect, so the division
    lands on the same column pitch). Per the lead-dev's "coloured line +
    salmon indicator" ask: each column draws a short horizontal line in its
    style's `kStyleColours` colour as the "label", and the selected drawing
    style gets a `kAccentMutedHi` salmon underline at the strip's bottom.
    A click in the strip calls `NeditProcessor::setSelectedDrawingStyle`
    (the real paint-picker being an open layout call — this is the minimal
    in-grid alternative). The palette always draws (even in the empty
    state, so a style can be picked before a sample loads); `gridRect(r)`
    is the view minus the strip, and ALL grid geometry (layout height, row
    hit-testing in onMouseDown/onMouseMoved, bar/playhead/scroll-knob
    drawing) operates in that sub-rect so the strip never steals a row.
  - Sequencer transport bar (2026-08-30, Sequence tab bottom): the four
    inputs are live — `PatternLengthStepper` (a `BarsStepper`-style spinner
    over `kPatternLengthBarsValues` = {1,2,4}, tag 1031) + three themed
    `COptionMenu` drop-ups (grid interval = `stepResolutionIndex` note value,
    tag 1032; switch timing = `PatternSwitchTiming` ordinal, tag 1033; switch
    interval = `patternSwitchIntervalIndex` note value, tag 1034). A
    `SequencerTransportBar` scrim draws the divider line + all four captions
    (the drop-ups can't draw their own caption row). Editor-local, routed
    through publish-only setters `setSequencerPatternLength` /
    `setSequencerStepResolution` (both grid DIMENSIONS → call
    `resizeSequencerGridForSample()`, the documented reset-on-dimension-change
    contract, so more bars/finer steps widen the grid) / `setSequencerSwitch
    Timing` / `setSequencerSwitchInterval` (clamped + idempotent-false).
    `syncSequencerTransport` (idle + tab-sync + the switch-timing handler,
    which re-arms immediately like the mode switch) pushes state→controls and
    ENABLES the switch-interval dropdown only while timing == Set Interval
    (else mouse-disabled + grey). Grid bottom shrunk to reserve the 48px bar.
    The grid is shown Sequence-only via syncTabBar with the transport bar.
  - BUG FIX (2026-08-30): switching to the Sequence tab crashed (segfault on
    the first transport-bar draw). ROOT CAUSE: `SequencerTransportBar` held a
    `const Caption*` into a stack-local `captions[4]` array in `open()`; the
    pointer dangled the moment `open()` returned, so every draw dereferenced
    freed stack memory. FIX: the scrim now owns a `std::vector<Caption>` copy
    (drawn from the caller's array in its ctor). Two latent bugs found in the
    same review and fixed: (1) the three dropdowns were created but NEVER
    `frame->addView`-ed, so they were invisible/non-functional; (2) the scrim
    was added LAST (on top), so VSTGUI's topmost-view hit-testing would have
    handed the stepper/dropdown clicks to the flat scrim — it is now added
    BEFORE the controls so they stay on top, and its caption band sits above
    the control boxes (no overlap).
  - Clear / Randomize buttons (same pass, right-aligned beyond the four
    controls): two graphite-outlined CTextButtons (tags 1035/1036, public
    header constexprs `kTagSeqClear`/`kTagSeqRandomize` so tests drive them).
    Momentary actions routed through `pressedEdge` (CTextButton double-fires
    under kKickStyle) → `clearSequence()` / `randomizeSequence()`, then the
    grid repaints. Randomize's mouse-enable tracks `hasSample()`
    (dedup'd via `lastSeqRandomizeEnabled_` in syncSequencerTransport); Clear
    always live. Tests: click-replay proving Clear empties the painted grid
    and Randomize keeps the grid monophonic/in-range (contents are RNG-
    seeded per call — the seeded engine tests own placement correctness).
  - NOT YET (next Sequencer pass, needs a lead-dev layout call): The shared
    style-probability band currently shown on the Sequence tab maps to
    `generate.styleWeights` (a Generate concept) — whether it doubles as
    the paint palette or stays separate is moot for now (the dedicated
    in-grid palette strip above covers style selection; the prob band's
    fate is a layout call).
- SequenceRandomizer (2026-08-29, engine, `src/engine/SequenceRandomizer.
  h/.cpp` + `tests/engine/test_sequence_randomizer.cpp`): the "Randomize
  Sequence" generator lifted off the audio thread as a pure, seedable
  function of `SequencerState` + the derived slice list. Faithful port of
  the original (clear-then-rebuild, natural-length-per-slice quantized to
  the step grid, fair round-robin passes over a per-pass Fisher-Yates
  shuffle, style drawn from the SEQUENCER's own `randomizeStyleWeights`,
  per-style `randomizeParametersForStyle` opt-in rolling each owned param
  minus the general Subdivide/Volume, swept params getting their mode
  sibling). Adjustments vs the original (licence to adapt): seeded + 
  deterministic RNG (untestable unseedable member Random before),
  configurable density `kDefaultPlacementProbability = 0.5` (was a hard 0.
  35), spacing-aware span scan (nearestOccupiedDistance picks the free span
  furthest from existing bars instead of "first free from a random start" —
  spreads placements), safe all-zero-weights fallback to Forward, and a
  `RandomizeResult{cellsPlaced,passes}` return for UI/tests. Also exposes
  `clearGrid(state)` (wipes cells + overrides + extensions, keeps dims) —
  shared by Clear Sequence and the randomize entry. Wired as
  `NeditProcessor::randomizeSequence()` (per-call `std::random_device`
  seed, uses the loaded sample's slices + `tempo::calculatedOriginalBpm`;
  no sample ⇒ reduces to a clear) and `NeditProcessor::clearSequence()`,
  both publish-only. 10 tests: determinism, differing seeds, monophony +
  span non-overlap, zero density ⇒ nothing, all-zero weights ⇒ Forward,
  only-weighted style, parameter-randomize excludes Subdivide/Volume +
  clamps ranges, multi-column natural spans, degenerate inputs, clearGrid
  keeps dimensions.
- Per-cell parameter overrides (2026-08-30, Sequencer tab) — right-click a
  filled cell for a parameter popup + in-place value slider, faithful to
  the original SequencerGrid's showParameterMenuForCell/getParameterSlider
  Bounds flow.
  - Turned OFF the right-click-erase gesture (left-drag erase-as-paint-same-
    style already covers clearing notes) and repurposed right-click on an
    OCCUPIED cell to open the override menu (the original's popup gesture;
    empty-cell right-click still erases).
  - Pure menu spec `ui::cellOverrideMenuEntries(style)` in
    `src/ui/SequencerGridGeometry.h` (framework-free, unit-tested):
    `CellOverrideMenuEntry{id, kind}` per applicable param (Subdivide +
    Volume always appended, faithful to the original). Kind from the
    vocabulary flags: `slider` = continuous OR Subdivide (discrete +
    steppedSlider) → a plain entry that opens the in-place drag slider;
    `submenu` = discrete non-stepped (Filter Type, Curve Shape, all *Mode/
    curve dropdowns) → a submenu of the param's own option names that
    writes the override directly; `modeSubmenu` = swept (Sample Rate
    Reduction, Bit Depth, Delay/Mix/Feedback, Volume) → a submenu of the
    paired *Mode (id+1) choices first, picking one writes the mode
    override AND opens the value slider.
  - `NeditProcessor::setSequencerCellOverride(row,col,id,value)` (clamps
    continuous into range, rounds+clamps discrete to an option index,
    keyed by StyleParamId, filled-cell only, publish-only) +
    `clearSequencerCellOverride(row,col,id)` (prunes the cell entry when
    empty, publish-only). The engine's `mergeCell` already merges these
    per-cell maps over `generate.styleParams`, so overrides are live —
    no engine change needed.
  - `SequencerGridView` (NeditEditor.cpp) builds a transient `COptionMenu`
    popup on right-click using `CCommandMenuItem::setActions` leaf lambdas
    capturing (row, col, id, optionIndex). In-place slider = a live
    `OverrideEdit` state drawn in `draw()` as a 120px box centred on the
    step's column at that row (salmon fill fraction = value t, hairline
    border, `name: value` label via `paramReadoutFormat` / option name),
    pointer-capture drag in onMouseDown/Moved, committed + closed on
    onMouseUp/Cancel; reads the current override (or the param's global
    default) via `currentCellValue`. Pressing the palette strip dismisses
    the slider. Discrete (incl. Subdivide) values map 0..numOptions-1 and
    snap via the option-name label; the processor rounds on store.
  - LIFETIME GOTCHA (real, fixed): `CMenuItem::setSubmenu(COptionMenu*)`
    stores a RAW pointer WITHOUT retaining it, and submenu CCommandMenuItems
    are owned by each submenu's own `menuItems` list — so a `new`+`forget`
    submenu would dangle the moment openCellMenu returned. `SequencerGridView`
    now holds the whole tree (top + submenus) in a member
    `std::vector<VSTGUI::SharedPointer<COptionMenu>> menuMenus_`, cleared in
    the popup's callback after the frame removes the top menu; the CCommand
    item lambdas capture `this` (the grid view owns/outlives the popup on the
    UI thread). Also: the positional `popup(CFrame*, CPoint, cb)` overload
    requires the menu NOT attached (fresh heap menu is fine) and needs the
    press point passed through openCellMenu.
  - Tests: +1 ui (`cell override menu: per-style param entries with Subdivide
    + Volume` — kind/modeId classification across styles, Subdivide→slider,
    Volume→modeSubmenu with volumeMode, swept→modeSubmenu with the right
    modeId, every menu ends Subdivide+Volume) + 1 plugin (`per-cell parameter
    override setters clamp and key by cell` — continuous clamp, discrete
    round+clamp, idempotent false, empty-cell/invalid-id rejection, single-
    param clear + last-param prune) in test_sample_pipeline.cpp (needs a
    loaded sample to seed the grid).

- Host-size fit — "Mac missing UI: all UI elements missing after the waveform
  view" (arblickspule/NeditVST#21, 2026-09-01):
  - ROOT CAUSE (verified in the pinned VSTGUI 4.15.x): the editor layout is a
    hardcoded 960x800 design (NeditEditor.cpp `kEditorWidth`/`kEditorHeight`,
    `kEditorRect`) and nothing derives from the frame size. The size path is
    `VSTGUIEditor::attached()` → `plugFrame->resizeView(this, 960x800)` →
    the host may grant LESS → `VSTGUIEditor::onSize` (vstguieditor.cpp:193)
    → `CFrame::setSize` → `NSViewFrame::setSize` → `[nsView setFrame:r]`.
    Children keep absolute y-coords, so a shorter viewport clips every view
    below the cut — the app bar + toolbar + waveform (y0-192) survive and
    "everything after the waveform view" disappears. Cork poppin' y=192 is
    just that tester's viewport height, not a magic number. Ruled out by
    source inspection: the VSTGUI 4.14.x CALayer-resize-redraw bug (#357) is
    already fixed in our submodule (`5db27225`, post-#383; nsviewframe.mm:
    1516-1517 keeps `caLayer.frame` in sync on resize), the `drawLayer`
    coordinate flip (nsviewframe.mm:1389) uses `layer.bounds.size.height`
    which tracks the resized frame, and our `notify` forwards the idle timer
    to `VSTGUIEditor::notify` → `frame->idle()` (NeditEditor.cpp). macOS CI
    never launches the editor (build.yml = build + ctest only), so nothing
    caught it.
  - FIX: fit the design into whatever viewport the host grants, instead of
    clipping. Pure helper `ui::computeEditorFit(frameW, frameH, designW,
    designH)` in `src/ui/FitGeometry.h` (framework-free, tested): smaller
    window → scale down to fit (`scale = min(w/960, h/800)`, clamped ≥ 0.2,
    never upscales; even a clamped scale is pinned inside the window);
    larger window → native 1.0 scale, recentred (margins + the 192x960 gutters
    get the existing `kWindowBase` CFrame background, set at open()).
    Wire-up in `NeditEditor`: `onSize()` override calls
    `VSTGUIEditor::onSize` (applies the host size) THEN `refitToHostSize()`,
    which installs `frame->setTransform(CGraphicsTransform().translate(
    offsetX, offsetY).scale(scale, scale))` + `frame->invalid()`. The CFrame
    transform is the clean seam: `CViewContainer::drawRect` (cviewcontainer.
    cpp:843) pushes `getTransform()` onto the draw context and transforms the
    clip/client rects back by its inverse, and CFrame's mouse dispatch
    inverse-transforms pointer positions (cframe.cpp:780) — so ALL ~50
    absolute-coordinate controls keep operating in design space untouched;
    only presentation + hit-testing are scaled/centred together.
    `SequencerGridView::rebuildLayout` uses its OWN view rect (not the
    frame's), so it stays in design space. Called at the end of `open()`
    (identity at that point — the host hasn't shrunk us yet) and from every
    `onSize`. No allocation on the audio thread (UI-thread-only).
  - Tests: 7 new ui cases (`test_fit_geometry.cpp`): exact-design identity,
    half-height shrink (0.5x, centred), width-bound shrink, 2x window =
    native + centred, wider-not-taller native, min-scale clamp containment,
    degenerate inputs. 272/272 green, zero warnings.
  - Known (minor) trade-off of the fit transform: under a window genuinely
    smaller than the design, `COptionMenu` popups are positioned from frame
    coords and do not account for the scale, so a right-click override menu
    could appear misaligned relative to the shrunk grid; the click targets
    themselves are inverse-transformed correctly. Acceptable; revisit if a
    real host reproduces it.

- macOS editor-open SIGSEGV — "Editor: SIGSEGV on open in
  CParamDisplay::setFont" (arblickspule/NeditVST#25, 2026-09-01). Real
  Ableton Live 12.4.5 Apple-Silicon crash report
  (`docs/ableton_mac_crash_rep.txt`): `EXC_BAD_ACCESS` (pointer-auth
  garbage address) on the main thread at `CParamDisplay::setFont`
  (cparamdisplay.cpp:373) called from `NeditEditor::open` (which also
  explained the y=192 "missing UI" symptom in the field — the editor was
  CRASHING at open on Debug builds, not just clipping). ROOT CAUSE: an
  ODR violation. VSTGUI's own build (`vstgui/cmake/modules/
  vstgui_init.cmake`) defines DEBUG for ITSELF only on `$<CONFIG:Debug>`,
  and DEBUG adds `CView::dumpInfo()` — an `#if DEBUG` VIRTUAL method — to
  every VSTGUI class's vtable. Our plugin TUs never matched that
  condition, so with an unset/ambiguous build type VSTGUI computed one
  EXTRA vtable slot while our code (NeditEditor.cpp, WaveformView.cpp)
  computed the identically-named classes one slot short. Every virtual
  call through those mismatched vtables landed on the neighbour: Live's
  `setFontColor` invoked `CParamDisplay::setFont`, which `forget()`'d a
  garbage `CFontDesc*` (the 0xa0a2-pattern address) → SIGSEGV on open.
  Diagnosed independently by the collaborator on the same crash; fix
  (`3d1e37d`): (1) pin `CMAKE_BUILD_TYPE` project-wide when unset
  (VSTGUI's own self-Debug fallback was directory-scoped and never
  reached our targets); (2) define DEBUG on nedit_plugin EXACTLY when
  VSTGUI does (`$<$<CONFIG:Debug>:DEBUG>`) so both sides always agree on
  vtable layouts; (3) defensive `if (kNormalFont == nullptr)
  CFontDesc::init();` at the top of `NeditEditor::open()` — the
  CParamDisplay ctor dereferences the global kNormalFont unconditionally,
  and a host that scans (load/unload) a bundle before the real session
  could leave it null. The DEFINEs only change behaviour in Debug configs,
  so non-Debug builds are untouched (283/283 green, zero warnings in
  RelWithDebInfo). STILL NEEDS a macOS Debug-config editor-open retest to
  fully confirm (dev machine is Linux/RelWithDebInfo only). Related issue
  #21 (viewport clipping) is orthogonal — both surfaced as "missing UI",
  both need the Mac retest.

- Editor-open smoke test (2026-09-02, `74c3ce7`) — closes the #25 retest
  backlog by automating the macOS (and now Linux/Windows) editor-open
  check on CI, PROVEN to catch the exact crash:
  - `tools/editor_smoke.{cpp,_linux.cpp,_win.cpp,_mac.mm,_platform.h}` — a
    cross-platform tool that loads a real `nedit.vst3` bundle through the
    VST3 `hosting/` helpers (Module::create → factory → createInstance
    kVstAudioEffectClass → initialize → controller->createView(kEditor) →
    setFrame → attached → pump(soak) → removed), mirroring the boot
    sequence host_harness uses. It exercises open()/idle on a REAL native
    parent (xcb window / NSView / HWND) so any vtable-mismatch SIGSEGV in
    `NeditEditor::open()` fails the run. `PlatformHost` (header) implements
    IHostApplication + IPlugFrame + Linux::IRunLoop with manual FUnknown
    (in namespace Steinberg, triple-FUnknown disambiguated via
    `unknownCast()`); the Linux TU adds a poll()-backed IRunLoop that
    services VSTGUI's registered fds + timers. Leak the platform/host ON
    PURPOSE: the plugin .so statically embeds VSTGUI and stores a
    FUnknownPtr<IRunLoop> that outlives main() — freeing it is a UAF at
    process exit (seen live: `IPtr<IRunLoop>::~IPtr` SIGSEGV after the
    `[smoke] OK` line).
  - **Must run in DEBUG.** Only Debug self-defines DEBUG in VSTGUI (adding
    `CView::dumpInfo()`, a virtual), which is what makes the #25 mismatch
    exist at all; Release agrees on both sides and would never crash.
    VERIFIED locally: Debug build of pre-fix `9921bd3` config (DEBUG define
    removed) → smoke SIGSEGVs with the EXACT #25 stack
    (`CParamDisplay::setFont` ← `NeditEditor::open` ←
    `VSTGUIEditor::attached`, exit 139); Debug fixed build → `[smoke] OK`,
    exit 0.
  - The Debug reconfig EXPOSED a latent ODR bug in the plugin TESTS: the
    `nedit_plugin_tests` TU builds VSTGUI subclasses (`FakeControl :
    CControl`) and calls into VSTGUI through virtual dispatch, but never
    defined DEBUG when VSTGUI/plugin did → one-slot-short vtables → the
    edit-click dispatch misdirected (Clear/Randomize/subdivision-chip
    editor tests failed in Debug only). Fixed like #25:
    `target_compile_definitions(nedit_plugin_tests PRIVATE
    $<$<CONFIG:Debug>:DEBUG>)`; 283/283 now green in Debug.
  - Build plumbing: `NEDIT_BUILD_EDITOR_SMOKE` option (default ON, tool
    gating in root now `EDITOR_SMOKE OR TOOLS`); `tools/CMakeLists.txt`
    builds the smoke on all platforms (links only the SDK hosting helpers +
    `nedit_vst3_sdk`/`nedit_warnings`, NOT `nedit_plugin` — the bundle is
    dlopen'd, so a mismatched smoke binary can't mask a broken bundle);
    Linux links `PkgConfig::SMOKE_XCB Threads ${CMAKE_DL_LIBS}`, Win
    `user32 gdi32`, mac `Cocoa CoreFoundation`. host_harness's link line
    lost its dangling `PkgConfig::XCB` (now checked like X11/XTST).
  - CI: `.github/workflows/build.yml` gains an `editor-smoke` job
    (ubuntu/macos/windows) at **Debug** config, building `nedit_vst3` +
    `nedit_editor_smoke`, running under `xvfb-run -a` on Linux. The macOS
    job IS the long-pending #25 Debug retest; Windows now covered too.

- Randomize honors the probability band — "Randomize from probabilities"
  (arblickspule/NeditVST#4, 2026-09-01): Reported as "only 'forward' style is
  picked." ROOT CAUSE: the style-probability band is the ONLY style-probability
  UI and it writes `generate.styleWeights` (NeditEditor.cpp:4860 →
  `NeditProcessor::setStyleWeight`), but the Sequencer Randomize draws its
  style from the sequencer's OWN decoupled table `SequencerState::
  randomizeStyleWeights` (SequenceRandomizer.cpp:261-263) — pitfall fix #1's
  deliberate split. That table is never written by any UI, so it stays at its
  default `{1,0,...}` (Forward only) forever → non-forward styles could never
  be picked. Slice Length/Clock already honored `generate.styleWeights`
  (Scheduler.cpp:379/487), so only Randomize was broken. FIX (per lead-dev
  decision, 2026-09-01): at Randomize press `NeditProcessor::randomizeSequence`
  snapshots the band into the sequencer table —
  `uiState_.sequencer.randomizeStyleWeights = uiState_.generate.styleWeights`
  (one publish, before `randomizeSequence`). This keeps the two tables
  independent (no persistent cross-talk) while Randomize honors whatever the
  band shows at press time; later band edits don't alter a grid already
  generated. The engine's own draw logic was already correct (covered by the
  seeded "only-weighted style is chosen" test); the missing piece was the
  upstream snapshot. Test: `editor: clear and randomize buttons act on the
  sequencer grid` now seeds the band to Flanger-only via the public setters,
  clicks RANDOMIZE, and asserts
  `debugUiState().sequencer.randomizeStyleWeights ==
  debugUiState().generate.styleWeights`. 273/273 green, zero warnings.

- Randomize edge-column spans — "Random lengths exceed slice length"
  (arblickspule/NeditVST#9, 2026-09-01): Random notes must be set to the
  slice length (snapped to grid) and the next random note must respect that
  claimed span. ROOT CAUSE in `SequenceRandomizer::randomizeSequence`: the
  span free-check AND the claim-loop wrapped past the grid's end back to
  column 0 via `(col + k) % columns`. The original clamps the span at the
  grid edge (`jmin(columns, column + naturalSteps)`, PluginProcessor.cpp:
  4315/4401) and never wraps. The wrap made a near-edge bar's claimed length
  extend past its on-grid bar and phantom-claim low columns, so a later note
  couldn't land where it should (the "next note doesn't respect that length"
  symptom), and it wrongly REJECTED a bar that should legally occupy the
  last `natural` columns of the grid when the wrapped partner column was
  already claimed (fragmenting the grid and shortening placements).
  FIX: both the free-check and the claim now clamp the span at `columns`
  (`spanEnd = min(columns, col + natural)`), exactly like the original — a
  near-edge bar occupies `[col, min(col+natural, columns))` and the next
  note can't wrap in from column 0. (`nearestOccupiedDistance`, the
  spacing-aware tiebreaker, is left circular — it only ranks candidate free
  spans, never changes which columns a bar claims.) Test:
  `randomize: a bar can occupy the grid's edge columns` (2 rows / 3 columns
  / 2-step slices, density 1.0): the first bar claims [0,2), the only
  remaining span is the right edge [2, min(3,4)); the fix lets that second
  cell place (cellsPlaced >= 2 and column 2 occupied), while the OLD
  wrap-around free-check rejected it (its wrapped partner is column 0,
  already claimed → cellsPlaced == 1; verified the test FAILS on the old
  code and passes on the fix). 273/273 green, zero warnings.

- Ghost sequencer rows — "Extra empty/silent row"
  (arblickspule/NeditVST#5, 2026-09-01): the sequencer derived one row per
  slice, and two structural artifacts both produced a row the user didn't
  want: (1) a sub-15ms leading sliver `[trimStart, firstOnset)` appeared on
  EVERY full-span loop whose content starts at frame 0 (the detector can't
  fire an onset at position 0 — rising-edge from `i-1` — so `mergeOnsets-
  IntoSlices` prepends `trimStart`, orphaning a few frames before the first
  real onset), and (2) a final slice `[lastOnset, trimEnd)` became a tall
  near-silent top row when the loop's last bar is a true rest (no audible
  content to lose). FIX: new pure engine filter `engine::filterGhostSlices
  (slices, channels, numChannels, trimStart, trimEnd, sampleRate,
  manualPoints)` in `SliceBuilder.{h,cpp}`, called by `SampleManager`
  (`loadFromMemory` + `rebuildSlices`, the two entries to every published
  slice list — weights/goals/re-randomize all remap through
  `rebuildSlicesPreservingWeights` containment, so no downstream change
  needed). Rule 1: a first slice starting exactly at `trimStart` with
  `length < kLeadingSliverMaxMs (15ms)` MERGES into its successor (its
  start moves back to `trimStart`; frames retained, "slices tile the trim"
  preserved) — unless a `manualPoints` boundary sits inside the sliver
  (user-placed cut is deliberate, kept). Rule 2: the final slice (the one
  ending at `trimEnd`) is DROPPED when its own RMS < `kSilentTailRatio
  (0.05)` × the sample's RMS over the trim (real content, however short,
  stays — a snare-then-rest tail is not silent). Guard: never reduce the
  list below one slice (sliver merged AND tail silent ⇒ the tail survives).
  Verified on the lead-dev's real loop (`docs/Ned_Rush_Breakcore_Salon1.wav`,
  72k frames): 10 raw → 9 filtered, `[0,49)` sliver gone, loud final slice
  kept. 8 tests (`ghost filter: *` in test_slice_builder.cpp): sliver
  merge, ≥15ms-first-slice kept, manual-inside guard, silent tail dropped,
  snare+rest tail kept, stereo sum for RMS, degenerate passthrough, never
  empty. 281/281 green, zero warnings.

- Multi-instance editor crash — "Bitwig crashes with 2+ instances loaded on
  Linux" (2026-09-02): loading a second nedit while the first is open kills
  Bitwig's engine process (exit code 1, UI shows `BITWIG_ENGINE_CRASH.txt`,
  ~15 s after instance 2 opened; reproduced on the then-current build, so NOT
  a stale-build bug). ROOT CAUSE: `NeditEditor::open()` called
  `Steinberg::Linux::setupVSTGUIRunloop(plugFrame)` on EVERY editor open,
  but VSTGUI's X11 runloop is a process-global shared object
  (`LinuxFactory::Impl::runLoop`). The SDK's module-level `InitVSTGUI` already
  sets it up ONCE, from the host context, via the host-context callback
  (`vstguieditor.cpp`); our per-open call REPLACED it with one bound to the
  current editor's IPlugFrame, corrupting the timer/X11-fd routing of every
  other concurrently-open instance (they share one `.so` in the engine
  process) — timer/event handlers stop dispatching / route to the wrong host
  context. FIX: the per-editor `setupVSTGUIRunloop` now runs ONLY when
  `linuxFactory->getRunLoop()` is still null (first-setter-wins fallback for
  harnesses that never go through the host-context callback); when the module
  callback has already installed the runloop, editors just read it. This is
  idempotent + multi-instance-safe. Also hardened the #25 `CFontDesc::init()`
  font guard (same block): it is NOT idempotent (overwrites shared GlobalFonts,
  orphaning the old CFontDesc) and must only re-init fonts while the platform
  is alive (a module teardown nulls BOTH; re-initing fonts alone would null-
  deref the later getPlatformFactory) — now gated on `kNormalFont==nullptr` +
  `asLinuxFactory()!=nullptr` under a process-global mutex (double-checked, so
  the common already-initialized path stays lock-free). Regression guard:
  `tools/editor_smoke.cpp` learned an optional `[instances]` arg — it opens N
  editors (one native parent window each, like a DAW engine process) and pumps
  them all through the one shared runloop; CI's `editor-smoke` job now runs
  `nedit_editor_smoke <bundle> 300 2` to exercise the second-instance open on
  every OS. 283/283 green, zero warnings.

- Editor reopen crash — "crashes if I reopen gui multiple times"
  (2026-09-02): closing + reopening a single instance's editor crashes
  (SIGSEGV in `xcb_depth_sizeof` / later a cairo `_get_screen_index` assert,
  exit 134/139). ROOT CAUSE: an upstream VSTGUI-on-Linux regression, NOT our
  plugin logic. `X11::RunLoop::Impl::exit()` (useCount → 0) calls
  `xcb_disconnect()`, tearing down the xcb connection; cairo-xcb keeps a
  process-global cache keyed by `xcb_connection_t*`, so the freed connection's
  screen data stays cached. The next `open()` → `cairo_xcb_surface_create`
  (DrawHandler ctor, x11frame.cpp:171) either lands on the same address
  (stale screen data → SIGSEGV in `xcb_screen_next`/`xcb_depth_sizeof`) or the
  screen lookup fails (`_get_screen_index` assert). This is upstream
  vstgui#249/#334/#335; #335's refactor (moving device teardown into
  CairoGraphicsDevice) reintroduced the reopen crash the #249 fix had closed.
  FIX (patched VSTGUI, not our code): keep the xcb connection + xkb/cursor
  state alive across close/open cycles — `RunLoop::Impl::init()` re-registers
  the SAME fd instead of reconnecting when `xcbConnection != nullptr`, and
  `exit()` only unregisters the fd (no `xcb_disconnect`, no xkb/cursor free).
  One xcb connection leaks per process (accepted trade-off; released at
  process exit). Patch lives in `cmake/vstgui-x11-reopen-crash.patch`, applied
  idempotently at configure time by `src/plugin/CMakeLists.txt` (Linux only,
  marker-comment-guarded) via `git apply` against the FetchContent'd
  `vstgui4` submodule. Regression guard: `tools/editor_smoke.cpp` gained a
  `[reopen-cycles]` arg (create/attach/soak/remove a fresh editor view on the
  SAME component, repeated) — reproduced the crash at cycle ~3 pre-fix, clean
  through 15+ cycles post-fix; CI runs `nedit_editor_smoke <bundle> 300 2 5`.
  GOTCHA: `nedit_editor_smoke` dlopens the bundle and does NOT link
  `nedit_plugin`, so you MUST rebuild `nedit_vst3` (not just the smoke target)
  after touching VSTGUI sources — the smoke target alone leaves a stale
  bundle. Multi-instance "frozen / transparent background" (dead buttons, host
  showing through the `CAIRO_CONTENT_COLOR_ALPHA` backbuffer) is a SEPARATE
  known Bitwig-on-Linux issue (browser unmaps the first window when inserting
  a second instance; affects Vital too) with the "individually sandboxed"
  per-plugin override as workaround — not fixed here. 283/283 green, zero
  warnings.

- Waveform redraw — "Doesn't update after certain ops"
  (arblickspule/NeditVST#10, 2026-09-01): the waveform (and the sequencer
  grid) would not repaint after REPLACING the sample via a second load.
  ROOT CAUSE: the editor's idle timer gated the redraw on a sample
  PRESENCE transition (`samplePresent != lastSampleSync_`). The first
  load flips false->true and refresh fires; a second load is true->true
  (presence unchanged) so `sampleChanged` stayed false and neither
  `waveformView_->refresh()` nor `sequencerGrid_->invalid()` ran — the
  very family of stale-view bugs issue #5's suffix ("certain ops") hinted
  at. FIX: the idle timer now also compares the published sample
  IDENTITY — `NeditEditor` holds the previous `acquireLoadedSample()`
  (`lastLoadedSample_`, a `shared_ptr<const LoadedSample>`) and reports a
  change when `.get()` differs. Every load/rebuild publishes a NEW
  LoadedSample object, so replacement is detected regardless of whether
  `samplePath` changed; holding the old shared_ptr keeps its address
  alive so the `.get()` comparison is exact (no address-reuse aliasing).
  `notifySampleChanged()` is a test hook exposing the decision. Editor
  tests construct a bare `NeditEditor` (no frame — all notify-path sync
  functions null-guard) and drive `notify(nullptr,
  CVSTGUITimer::kMsgTimer)`, which is safe: the base
  `VSTGUIEditor::notify` only calls `frame->idle()` when `frame` exists.
  Test `waveform redraw: replacing the sample via a second load is
  detected`: load A -> tick (changed), tick (quiet), load B -> tick
  (changed), tick (quiet). VERIFIED the test FAILS on the old presence-
  only logic at the second-load assertion and passes on the fix (the
  built-in smoke of a good regression test).
  TWO MORE stale-view members of the same issue #10 family found in the
  follow-up sweep and fixed (all covered by the same 283-test suite):
  (1) the sequencer transport bar's pattern-length / grid-interval
  handlers route through `resizeSequencerGridForSample()` (which resets
  cells when dims change) but never invalidated `sequencerGrid_`, so
  switching from 1→4 bars or 16n→8n left the OLD cells on screen until a
  playing step moved. Fixed in the valueChanged handlers AND in
  `syncSequencerTransport` (the latched plen/grid edges, so external
  dims changes via state restore / host automation repaint the grid too);
  (2) `rebuildSlicesPreservingWeights()` (sensitivity / quantize toggle
  / quantize grid / manual-point + exclude edits) changes the slice
  COUNT, but the sequencer grid's rows = min(sliceCount, …) were only
  derived at load time — a sensitivity edit that split/merged slices left
  the grid one row too many/too few until the next load. Fixed in
  `NeditProcessor` (`rebuildSlicesPreservingWeights` now re-calls
  `resizeSequencerGridForSample()` — `resizeGrid` is a no-op unless dims
  change, per the documented reset-on-dimension-change contract) + the
  editor's sensitivity/quantize/quantize-grid handlers also invalidate
  the grid. Test `shell: slice-count edits re-derive the sequencer grid
  dimensions` (sensitivity 0 → 1 slice → 1 row; back to 0.5 → many
  slices → rows follow) — VERIFIED it FAILS with the old load-time-only
  dims and passes on the fix. 283/283 green, zero warnings.

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
