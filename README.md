# NeditVST

A generative audio slicer — a VST3 plugin built with JUCE that loads a sample, detects transients, slices it at those boundaries, and generatively plays back slices with per-slice probability weights.

## Features

- **Transient-based slicing** — automatic onset detection with adjustable sensitivity, plus manual slice point placement
- **Two trigger modes**
  - *Slice Length* — each slice plays in full, then chains to a weighted-random next slice
  - *Clock* — a fixed outer window (configurable, e.g. 1 bar) picks a slice + subdivision rate, retriggering on each tick
- **Two pitch modes**
  - *Repitch* — classic varispeed, pitch follows tempo
  - *Time-Stretch* — granular overlap-add synthesis, pitch preserved regardless of tempo
- **Interactive waveform** — click to set per-slice probability, double-click to add manual slice points, drag to move them
- **Subdivision probability grid** — 20 note-value rows (128n–1n), each with a draggable weight slider
- **Undo/redo** for all slice edits
- **Drag-and-drop** sample loading (WAV/AIFF/FLAC)
- **Fade in/out** controls (0–100 ms)
- 8-voice polyphony, MIDI-triggered

## Building

### CMake (cross-platform CI)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Requires CMake 3.22+ and a C++20 compiler. JUCE 8.0.14 is fetched automatically via `FetchContent`.

### Projucer (local Mac dev)

Open `NeditVST/NeditVST.jucer` in Projucer, save to regenerate the Xcode project, and build from Xcode.

## CI

GitHub Actions builds on macOS, Windows, and Ubuntu on every push. Artifacts (`.vst3` bundles) are uploaded for each platform.

## Usage

Load the VST3 into a track in your DAW as an instrument. Load a sample via the **Load Sample** button, then play MIDI notes to trigger slices. Adjust transient sensitivity to control how many slices are detected, and tweak per-slice probabilities to shape the generative playback.
