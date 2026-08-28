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
- Test totals: default build 180/180 (51 state + 122 engine + 7 ui);
  plugin build 215/215 (state round-trip gained a v1 forward-compat case
  and the editor gained an active-tab persistence case), zero warnings.

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
