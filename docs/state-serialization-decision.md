# State Serialization: Format & Scope Decision

## Summary

`SlicerAudioProcessor::getStateInformation()` / `setStateInformation()` are
empty stubs (nextsteps.md 1.4). The model owns ~30 stored parameters plus the
probability tables, the 128-slot pattern bank, the 128-slot performance state
bank, and the sequencer grid, but nothing ever writes them to a
`MemoryBlock`. This doc lays out the format and scope options so the decision
can be signed off; the developer's working assumption is XML for the dev
stage.

Ground truth that constrains every option:

- The sample buffer is **transient**. The model keeps only the bare file
  name (`loadedFileName`), never the path, and never the audio. So a
  restored preset is only fully meaningful once the matching sample is
  re-loaded.
- Slice-indexed state (sequencer grid rows, pattern/performance bank grids,
  trim markers, slice probabilities) is meaningless against a *different*
  sample and must be guarded on restore regardless of format.
- Serialization runs on the host message thread; the restore path must take
  `model.sampleLock` and end with `onPickStateInvalidated()` so the engine
  re-reads the new state.

## Decision 1: Serialization format

### Option A — XML (`juce::XmlElement`)

Write a versioned tree (one element per section: globals, probabilities,
grid, pattern bank, performance bank) and convert it to/from `MemoryBlock`
as the XML text. `MemoryBlock` stays the JUCE-standard carrier across the
processor boundary; XML is the on-disk encoding.

Pros:

- **Human-readable.** A preset is `cat`-able, diffable, and greppable —
  invaluable while behaviour is still changing daily and presets can be
  inspected by hand to debug a wrong value.
- **Self-describing + tolerant.** Named tags survive field reordering; a
  future field is an additive tag; unknown/missing tags are skipped or
  fall back to defaults, so old presets keep loading and new presets
  degrade gracefully on old builds. Versioning is one attribute on the root.
- Zero new dependencies; the whole tree is plain JUCE.
- Escape-hatch: XML is trivially editable when a session preset misbehaves
  in a host without rebuilding.

Cons:

- Verbose. The 128-slot performance bank alone is 128 x 21 parameter values
  plus trim/style/loop/sync — a session file is a few hundred KB of text
  rather than tens of KB.
- Slightly slower to parse than a flat binary blob (irrelevant here: state
  loads once per instance, not per block).
- Not a compressed/obfuscated shipping format.

### Option B — Versioned binary (`MemoryOutputStream`/`MemoryInputStream`)

Hand-rolled fields behind a version header: one `writeInt`/`writeFloat` per
value, one version bump per schema change.

Pros:

- Compact and fast; the classic released-plugin preset format.
- No parsing; values map 1:1 onto the atomics.

Cons:

- **Opaque.** A corrupt or hand-edited file fails opaquely; there is no way
  to eyeball a preset.
- **Brittle.** Field reordering or insertion requires a version bump and
  per-version read logic *up front*; forgetting one is silent corruption.
  During an active dev stage with a schema that changes weekly, this is a
  real foot-gun.
- Debugging a wrong restored value means instrumenting the reader.

### Option C — Binary now, XML during dev (hybrid)

Ship binary, but keep the XML tree as a dev/debug facility (e.g. a build
flag or a "dump XML" console command).

Pros: production compactness + dev readability.
Cons: two encodings to maintain; the dev XML and shipping binary can drift;
the whole point of dev-stage readability is only served if the *default*
path is the readable one.

**Working recommendation:** Option A for the dev stage. The model API
(`saveState`/`restoreState`) will hide the encoding, so if the "big man"
wants compactness at release time, Option B slots in as a serializer swap
behind the same API and the same version attribute discipline — XML's
tolerant tag-based reader is exactly the dev-stage advantage a weekly-
changing schema wants.

## Decision 2: Persistence scope

The sample buffer is never persisted in any option (it is transient by
design, and the model does not even keep the file path). The question is how
much of the *settings* state to persist.

### Option 1 — All settings + banks, no audio

Every atomic global (trigger mode, clock, trims, fades, sensitivity,
probabilities, pitch/grain/quantize, all style globals), the sequencer grid
+ per-cell overrides, the full 128-slot pattern bank, the full 128-slot
performance bank, and the performance working state.

Pros:

- One preset = the whole instrument. MIDI-learned banks are user-created
  data (potentially hours of clicking); losing them on session close would
  be the most surprising outcome.
- Matches standard sampler behaviour (preset = settings; sample = separate).

Cons:

- Sample-relative values (trim markers, grid row counts, bank grid indices)
  are only valid against the matching sample; a restore with a different or
  no sample must size-guard and clamp them rather than trust them.

### Option 2 — Settings only, skip the banks

Globals + probabilities + grid, but not the pattern/performance banks or
working state.

Pros: smallest coherent slice of work; nothing sample-relative beyond the
grid/trims.
Cons: the two banks — the largest, most user-invested state — silently don't
survive a session. The 1.4 spec explicitly names "pattern bank, performance
bank" as things worth persisting.

### Option 3 — Also re-load the sample by path

Additionally store the full file path (currently only the name is kept) and
re-load the audio on restore.

Pros: a preset brings its sample along — fully self-contained.
Cons: new file-I/O error surface (missing/moved file, slow network mounts,
permission), and a path is machine-specific; sharing a preset across
machines silently fails to find the sample anyway. The file chooser
reload flow already exists in the UI, so this is convenience layered on a
guaranteed-fragile assumption.

**Working recommendation:** Option 1. Persist everything except the audio
buffer; size-guard the slice-indexed tables on restore. If the "big man"
wants the self-contained behaviour later, Option 3 is an additive feature on
top (persist the path; reuse `loadSample()`).

## Risks & constraints (any option)

- Restore must clamp enum/int fields to their valid ranges (the setters
  already `jlimit`; the loader must too, since a hand-edited XML could put
  `triggerMode = 99`).
- Slice-indexed tables (slice probabilities) apply only when the stored size
  matches the current slice count; otherwise they are skipped to avoid
  index mismatches with a different sample.
- Unknown XML tags must be ignored; missing tags must leave current state
  unchanged (never wipe).
- The restore path must call `onPickStateInvalidated()` under the lock last,
  or the engine keeps playing from stale pick state.
- Editor/engine members that are pure runtime telemetry (playhead,
  currently-playing indices, audition state, undo history) are intentionally
  out of scope.

## Recommendation at a glance

| | Choice | Why |
|---|---|---|
| Format | XML (`XmlElement`), dev stage | readable + tolerant while schema churns; binary is a later drop-in swap |
| Scope | All settings + banks, no audio | banks are the highest-investment user data; sample-relative fields are size-guarded on restore |
