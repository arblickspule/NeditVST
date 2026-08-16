# UI Layout Decision: "Vue for JUCE" (v2 greenfield editor)

Status: **DECIDED** (2026-08-14). Companion to `AGENTS.md`'s target
architecture `Engine <- Model <-> UI` and `nextsteps.md` 1.4's serialization
work. Rationale and options below are what the review committee asked for;
the four numbered decisions at the bottom are the calls.

**Update (2026-08-15):** the old editor was archived rather than retired
in-place — `docs/archive/` now holds `PluginEditor.cpp/.h` plus the first
Generate-page attempt (`GeneratePage.cpp/.h`), and the build was restarted
from the bare seam (empty editor behind `ui::makeEditor`). Pages are rebuilt
from scratch on the layout DSL; the shared painted widgets that survived
(`WaveformDisplay`, `PlaybackStyleGrid`, `SegmentedButtonRow`, ...) stay in
`Source/` (unbuilt) as the component palette to reuse. Migration step 3
below no longer applies as written.

---

## 1. Why

The plugin's audio core is now cleanly layered (model owns state, engine owns
DSP, processor is a 66-line shell). The UI is the last 90s-JUCE holdout:

- `PluginEditor.cpp`'s constructor is ~100 `addAndMakeVisible(...)` calls
  (lines 41-330) that parent every knob/label/button directly into a giant
  `controlsContent`.
- Layout is ~93 hand-written `setBounds(...)` calls spread across
  `resized()`/`updateWindowSize()` and the per-tab `layout*Tab()` helpers,
  with hard-coded pixel gaps (`layoutGap`, `zoomToWaveformGap`, ...).
- `SectionPanel` is a decorative backdrop that owns no children and positions
  nothing (SectionPanel.h:12-20) — nothing in the tree is actually composable.
- Panels exist, but as *conventions* (e.g. `PlaybackStyleParameterPanel` takes
  `SlicerModel&` + lambda props + an `onStyleChanged` event — already
  Vue-flavoured) rather than as an enforced structure.

The gap between the model/engine layer and the UI is exactly the problem: a
UI change today means editing the one giant editor file, and there is no
"throw the UI away and write a new one" story at all.

## 2. Requirements

1. **UI is a separate concern.** Swapping the entire GUI must never touch the
   engine, ideally not the model, and only one line in the processor
   (`createEditor()`).
2. **Authoring-friendly for a Max/MSP + one-month-C++ dev.** Declarative
   composition beats imperative layout math; the tree must read like markup.
3. **Reusable components.** A file/folder per component (Vue-SFC style), so
   panels are leaf widgets composed into pages.
4. **Keep the aesthetic.** The flat Tungsten/Salmon hand-painted look
   (`NeditPalette`) stays; existing painted widgets are reused, not redrawn.
5. **No new third-party dependency.** JUCE 8 is already fetched; the team is
   small and the "just code it" cost of a small internal layer is preferred
   over learning/owning an external framework.
6. **Low risk to the audio core.** Tests only cover model/engine; the full
   suite must stay green throughout, and the plugin build must compile after
   every slice.

## 3. Options considered

### A. Foleys GUI Magic (the existing "Vue for JUCE")

Real, maintained, used in shipping commercial plugins. Declarative XML
templates with binding expressions and optional JS scripting, plus a visual
editor.

- Pros: actual declarative framework; data binding; template library; proven.
- Cons: heavy FetchContent dependency; brings its own painting/look layer
  that *replaces* hand-painted widgets (SequencerGrid's overlays, drag bars,
  the whole Tungsten language would be rebuilt inside Foleys' system); a
  paradigm shift for the whole team; retrofitting the existing widgets is a
  bigger lift than rebuilding the shell.
- Verdict: **rejected** — solves the wrong 80%. Our widgets are already
  components; what we lack is a composable shell and a swap seam, not a
  templating engine.

### B. Codegen / template compiler (JSX/XML/JSON -> generated C++)

Write UI in a DSL, generate the `addAndMakeVisible` + bounds code.

- Pros: markup ergonomics; could be checked into CI.
- Cons: a whole toolchain for a two-dev shop; generated code is harder to
  read and debug than a thin runtime library; the moment the DSL needs to
  express custom painting it leaks C++ anyway.
- Verdict: **rejected** — the far end of the spectrum, not the sweet spot.

### C. Conventions only (keep `setBounds`, enforce folders)

No new code; just adopt `ui/` structure + `contract.h` discipline.

- Pros: zero build risk.
- Cons: the 90s layout math stays; panels stay backdrops; composition is
  still manual; the seam does not actually guarantee decoupling.
- Verdict: **rejected** — the goal is a system that *makes* the right thing
  easy, not rules that must be remembered.

### D. Hand-rolled layout DSL over JUCE FlexBox (DECIDED)

A small internal layer (target ~200-300 lines) of container components —
`Row`, `Column`, `Spacer`, `Stack` — that *own* their children
(`std::unique_ptr<Component>`) and lay them out in `resized()` with one
FlexBox pass. Composition replaces both the `addAndMakeVisible` chains and
the `setBounds` math:

```cpp
auto panel = ui::column (
    ui::row (ui::label ("Grain Size"), ui::slider (grainSizeSlider)),
    ui::row (ui::label ("Window"),     ui::segmented (windowShape)),
    ui::spacer (8));
```

- Every composition element is a `ui::Cell` — leaves, gaps, and whole nested
  boxes all compose directly (variadic helpers, so nothing copies).
- Fixed-size children keep their construction size (leaf widgets self-size in
  their constructors); weight cells (`ui::fill`) absorb leftover space.
- `TitledPanel` (replaces `SectionPanel`'s backdrop role) actually owns its
  content box.
- Header-only, no dependency, full control of the hand-painted aesthetic.

## 4. The contract / swap seam

`PluginProcessor::createEditor()` (PluginProcessor.cpp:43-45) is the single
line that knows the UI exists. It becomes:

```cpp
// Source/ui/contract.h
namespace ui
{
    struct UiCallbacks
    {
        // Host interactions the UI is allowed to ask for (JUCE editor/host
        // plumbing). Grows only when a real host need appears.
        std::function<void (juce::Rectangle<int> newBounds)> requestResize;
    };

    std::unique_ptr<juce::AudioProcessorEditor> makeEditor (
        juce::AudioProcessor& host,        // for JUCE host plumbing only
        SlicerModel& model,
        SlicerEngine& engine,
        const UiCallbacks& callbacks);
}
```

`gui.cpp` implements `makeEditor`; everything it builds talks to model/engine
only through their public members (the de-facto contract). The whole UI can
be thrown away and rebuilt by writing a new `gui.cpp` — nothing else changes.

## 5. Directory layout (folders = components)

```
Source/ui/
  gui.cpp            // makeEditor(): the whole editor shell        (App.vue)
  contract.h         // UiCallbacks + factory                       (props/events)
  layout/            // Row, Column, Stack, Spacer, TitledPanel     (the DSL)
  components/        // leaves: LabeledSlider, ToggleRow, SegmentedButtonRow,
                     //   PlaybackStylePalette, PlaybackStyleGrid, WaveformDisplay,
                     //   SequencerGrid, ... (existing painted widgets move here)
  panels/            // feature panels: Sample, TrimTempo, Detection, Fade,
                     //   PitchMode, PlaybackStyle, Timing          (SFCs)
  pages/             // tab compositions: Generate, Sequence, Perform
  theme/             // NeditPalette + spacing/type tokens
```

- Existing painted widgets (`SegmentedButtonRow`, `PlaybackStylePalette`,
  `PlaybackStyleGrid`, `SubdivisionProbabilityGrid`, `SequencerGrid`,
  `WaveformDisplay`) are already aesthetic-correct and move verbatim into
  `components/`.
- Panels follow the existing `PlaybackStyleParameterPanel` pattern: model
  ref + lambda props + events.
- Each panel derives from `UiPanel : Component` and implements
  `virtual void syncFromModel()`. One 10fps timer in `gui.cpp` walks the tree
  calling `syncFromModel()`, replacing the editor's current scattered
  per-widget polling (PluginEditor.cpp:1396-1422).

## 6. Migration strategy (greenfield behind the seam)

1. **Seam** — add `contract.h` + `gui.cpp` (minimal placeholder editor) +
   DSL header; repoint `createEditor()`. Old `PluginEditor.*` stays in the
   build, unused, until the new UI replaces it. Plugin still opens a window.
2. **Generate page** — the straightforward panel rows first.
3. **Sequence / Perform pages** — the heavy custom components last.
4. **Retire** — delete `PluginEditor.*` (and `SectionPanel`'s backdrop role).

Each slice: plugin build + full test suite must stay green. Model/engine are
untouched by construction.

## 7. Risks

- **Sizing policy is the design's crux.** Leaves must self-size so the DSL can
  do fixed-vs-fill math; wrong defaults produce squashy layouts. Mitigate with
  a strict "leaf sets own size in ctor" rule and the `TitledPanel` padding
  tokens in `theme/`.
- **Polled sync won't catch every MIDI-driven change instantly.** Accepted —
  it matches the current 10fps design and is the cheap, debug-friendly choice
  (decision 3).
- **Golden-handcuffs regression.** The old editor had 30 passes of pixel-perfect
  tweaks; the greenfield Generate page must visually match or beat it, or the
  swap stalls. Keep `PluginEditor.*` buildable until the new one wins.

## 8. Decisions

1. **Greenfield `gui.cpp`** behind the contract, old editor parked until the
   new one is ready (not an in-place refactor).
2. **Hand-rolled layout DSL** over JUCE FlexBox (no Foleys, no codegen).
3. **Polled sync** (`UiPanel::syncFromModel()` on a 10fps timer).
4. **Keep the Tungsten/Salmon aesthetic** — reuse existing painted widgets.
