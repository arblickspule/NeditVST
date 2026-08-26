# Nedit Design Language — "Graphite & Salmon"

Dark-mode Material structure with tighter corner radii. Two hues only:
a neutral **graphite** ramp does all the structural work; **salmon** is the
single accent and is spent deliberately (playhead, active state, primary
action). If everything glows, nothing does.

Status: LOCKED v1.0 — mockups in `docs/design/mockups/` are the visual spec;
refine there, then we codify into `src/ui/Theme.h`.

---

## 1. Principles

1. **Structure is lightness, not shadow.** Material-dark elevation rule:
   surfaces rise by getting *lighter*, never by drop shadows. One hairline
   outline (`outline`) separates a surface from its parent when lightness
   alone isn't enough.
2. **Small radii.** Material defaults feel bubbly at plug-in scale. Our scale
   tops out at 8px (window sheet). Everything interactive is 2–4px.
3. **One accent.** Salmon marks *live* things: playhead, playing slice,
   active tab, armed cell, pressed control. Never used decoratively.
4. **4px grid.** All coordinates are multiples of 4. Gutters 8, card padding
   12, section gaps 16.
5. **Views render state.** Every component is a stateless renderer over
   `PluginState` + engine mailboxes, drawn programmatically via
   `CGraphicsPath` — no bitmap assets, resolution-independent under DPI
   scaling. SVGs in this folder are design artifacts, not shipped resources.
6. **Density over chrome.** This is a tool with ~30 visible parameters.
   Compact controls (h24), micro-labels in caps, values in tabular mono.

## 2. Color tokens

### Graphite (neutral ramp)

| Token | Hex | Role |
|---|---|---|
| `graphite-900` | `#14161A` | Window base |
| `graphite-800` | `#1B1E23` | Surface 1 — app bar, cards |
| `graphite-700` | `#22262C` | Surface 2 — cells, inputs, chips |
| `graphite-650` | `#343A43` | Waveform fill |
| `graphite-600` | `#2A2F36` | Surface 3 — hover / active-fill |
| `outline`      | `#33383F` | Hairlines, dividers, 1px strokes |
| `slice-marker` | `#454C56` | Slice marker strokes |
| `text-primary`   | `#E8EAED` | Primary copy |
| `text-secondary` | `#9AA0A6` | Labels, inactive |
| `text-disabled`  | `#5F6368` | Disabled |

### Salmon (accent ramp)

| Token | Hex | Role |
|---|---|---|
| `salmon-300`     | `#FF9A8C` | Playhead, brightest live indicator |
| `salmon-400`     | `#FA8072` | Primary accent — fills, active tab |
| `salmon-500`     | `#E0685A` | Hover on accent |
| `salmon-600`     | `#C25548` | Pressed |
| `salmon-container` | `#3B2622` | Selected-state background tint |
| `on-salmon`      | `#20100D` | Copy on salmon fills |

Interaction overlay rule (Material-style): hover = white @ 6% over the
surface; pressed = white @ 12%. Disabled controls: content
`text-disabled`, surface unchanged (no greying of whole panels).

## 3. Radii

| Token | px | Applied to |
|---|---|---|
| `radius-xs` | 2 | chips, inputs, grid cells, toggles |
| `radius-s`  | 4 | buttons, knobs' base plates, tab container |
| `radius-m`  | 6 | cards, waveform view, panels |
| `radius-l`  | 8 | the window sheet itself |

## 4. Spacing & sizing

| Token | px |
|---|---|
| `space-1` | 4 |
| `space-2` | 8 (gutter between cards) |
| `space-3` | 12 (card padding) |
| `space-4` | 16 (section gaps) |
| `space-6` | 24 (window margins) |

Window: **960 × 800 logical units**, fixed (see §8 for DPI/zoom).
Heights: app bar 48 · tab bar 32 · footer 24 · buttons 24 · chips 18 ·
slider rows 24 · grid cells 32 wide × 18 tall · menu rows 24.

## 5. Typography

Implementation font TBD (bundled vs system — open question, tracked below).
Specified by role, not family:

| Role | Size | Weight | Notes |
|---|---|---|---|
| Display | 18 | semibold | wordmark only |
| Section | 11 | medium | CAPS, +5% tracking, `text-secondary` |
| Body    | 12 | regular | menus, options |
| Label   | 10.5 | regular | control captions |
| Numeric | 11 | regular | **tabular figures**, mono-leaning; all value readouts |
| Micro   | 9.5 | regular | mono, in-panel values, percentage labels |

Font: Helvetica for the NEDIT wordmark (bundled or system — Helvetica is
available on macOS, may need a substitute on Windows/Linux). Body/label
fonts remain the sans fallback stack (`Inter, Roboto, system-ui, sans-serif`)
for now; Helvetica may be trialled for other roles later.

## 6. Layout — shell anatomy (see `mockups/shell-layout.svg`)

```
┌──────────────────────────────────────────────────────┐
│ app-bar h48          trigger · tempo … load · gear   │  graphite-800
├──────────────────────────────────────────────────────┤
│  [ waveform card ]         h240 = ⅓ inner column     │  graphite-800, r6
├──────────────────────────────────────────────────────┤
│ global strip h60     tempo · loop · quantize         │  mode-independent
├──────────────────────────────────────────────────────┤
│ tool cards 2×2 h60   detector · envelope ·           │  sample-level,
│                      pitch mode · trim & edits       │  mode-independent
├──────────────────────────────────────────────────────┤
│ tab bar h32          GENERATE SEQUENCER PERF CONTROL │  modes → panels only
├──────────────────────────────────────────────────────┤
│  panel area h228      one card-set per mode          │  per-mode layout
├──────────────────────────────────────────────────────┤
│ footer h24            sample meta · engine status    │  hairline top
└──────────────────────────────────────────────────────┘
```

Vertical stack: `48+8+240+8+60+8+60+8+60+8+32+8+228+24 = 800`.
The waveform card is exactly **⅓ of the inner column** (240 of the 720
between app bar and footer). Global strip + tool cards sit ABOVE the tab
bar on purpose: they edit the loaded sample and are meaningful in every
mode; the tabs then swap only the panel area. Control representation is
semantic: **chips for discrete selectors, slider rows for continuous**.

- Window margins 24; single column of sections separated by 8 gutters
  (no nested scroll panes except the sequencer grid).
- App bar: logo + mode chip + tempo readout left; sample pill + LOAD
  SAMPLE (primary, salmon fill) + settings right.
- Waveform card owns zoom/pan; ruler strip lives *inside* the card's
  bottom edge; transport/playhead is an overlay layer, not a layout child.
- Tab bar: underline tabs (Material), active = salmon text + 2px salmon
  indicator; inactive `text-secondary`. Container-less — sits on base.
- Panel area swaps entirely per tab; cross-tab persistence comes from
  `UiState.activeTab` (views re-render from state, never own layout).
- Footer: micro mono sample metadata left; engine status right.

### Vertical side-tabs (GENERATE panel)

The GENERATE tab uses a 24px-wide vertical side-tab strip on the left edge
to switch between two sub-views:

| View | Content |
|---|---|
| STYLE | Style weights (9 vertical bars) + style parameters (21 automatable) |
| PROBABILITY | Style chance (9 bars with % labels) + loop length probability (11 timing rows) |

Active tab: salmon text + 3px indicator bar. Inactive: `text-disabled`.
Side-tabs sit flush to the card edge (no padding/gap).

### Tab panel layouts

All panels occupy a 912×228 area (window width minus 24px margins on each
side; height per the shell stack). Two-card layout within each panel.

**GENERATE** — vertical side-tabs (STYLE / PROBABILITY):

| View | Left card (260px) | Right card (612px) |
|---|---|---|
| STYLE | Style weights (9 vertical bars, `salmon-400` fill, `%` labels below) | Style parameters (21 automatable, 3 cols × 7 rows) |
| PROBABILITY | Style chance (9 vertical bars + `%` labels) | Loop length probability (11 timing rows: chance bar + `%` + cumulative total) |

**SEQUENCER** — no side-tabs:

| Left card (584px) | Right card (320px) |
|---|---|
| Sequencer grid: 16 cols × 8 rows, row labels (1/4..1/64), step numbers, armed cells (`salmon-container` + `salmon-400` stroke), playing cell (`salmon-400` fill), dashed playhead | Pattern controls: bank selector (4 chips), subdivision, step length, loop, recall button |

**PERFORMANCE** — no side-tabs:

| Left card (400px) | Right card (504px) |
|---|---|
| Bank slots: 4×4 grid (92×48, `rx=4`), focused slot (`salmon-container` + stroke), pagination hint | Controls: focused slot badge, quantize recall (interval chip), loop, sync, frozen snapshot preview |

**CONTROL** — no side-tabs:

| Left card (280px) | Right card (624px) |
|---|---|
| Settings: base note, gate mode, num slices, velocity gain, retrigger, MIDI channel | Keyswitch legend: 9 rows (key → style → weight bar), active row highlighted, slice-note hint |

Per-mode panel layouts are specced with their components (below).

## 7. Component inventory

Detailed specs land here as each family is mocked. Order of work:

1. **Containers/layout** ← current (shell, card, section header, tab bar,
   footer, vertical side-tabs).
2. Buttons & chips (primary/secondary/icon, selectable chip).
3. Sliders (linear h24 row: caption + track + tabular value) & knob
   (compact rotary, arc-only, no bitmap).
4. Value fields (drag-number: drag = adjust, click = edit).
5. Waveform overlays (slice markers, manual/excluded points, trim handles,
   playhead).
6. Sequencer grid (steps × styles cells, drag-fill, Shift+drag extend,
   subdivide affordance, pattern recall lane).
7. Menus (note-value palette, playback styles) — native COptionMenu restyled
   or custom popup, decided during mockup pass.

### Interaction grammar

| Input type | Control | Visual |
|---|---|---|
| Discrete selector | Chip (tap to cycle) | `graphite-700` fill, `outline` stroke, `text-primary` label |
| Continuous value | Horizontal slider row | `graphite-600` track, `salmon-400` fill, mono value right-aligned |
| Probability weight | Vertical bar chart | `graphite-600` skeleton, `salmon-400` fill from bottom |
| Toggle | Chip (ON/OFF) | Same as chip, salmon fill when ON |
| Primary action | Button | `salmon-400` fill, `on-salmon` text, `rx=4` |

## 8. Scaling & DPI

The editor is **960×800 logical units**, fixed. Logical size ≠ physical
pixels:

- **macOS**: hosts size views in points; Retina supplies a 2× backing
  store automatically. Same physical size, twice the sharpness. Nothing
  to do.
- **Windows/Linux**: the host signals DPI scale via
  `IPlugViewContentScaleSupport`. We implement it and map the reported
  factor onto `CFrame::setZoom()`, so the whole vector tree scales.
  Ignoring it yields blur (drawn 1× into a scaled surface), not shrinkage.
- All drawing is `CGraphicsPath`/`CDrawContext` — resolution independent,
  no bitmap assets to go soft at 2×+.
- Every view derives geometry from `getViewSize()` (the WaveformGeometry
  pattern); nothing may hardcode absolute pixel positions.
- Later: user-facing zoom control (0.75–2.0) persisted in `UiState`,
  applied through the same `setZoom` path. Type floor stays ≥10.5 logical px.

## 9. Implementation mapping (after sign-off)

- `src/ui/Theme.h` — all tokens as `constexpr` (colors as `CColor`,
  radii, sizes, type roles). No hardcoded hex outside Theme.h, enforced
  by review.
- Components subclass `CControl` (interactive) or `CView` (passive),
  drawing exclusively through `CGraphicsPath`/`CDrawContext`.
- Expensive layers (waveform peaks) cached via `COffscreenContext`,
  invalidated by state-version bumps only.
- Headless unit tests assert geometry/paint calls against the theme
  (pattern already proven by `tests/ui/`).
