#pragma once

#include <JuceHeader.h>
#include <functional>
#include <memory>

//==============================================================================
/** The single seam between the JUCE plugin shell and the entire UI -- see
    docs/ui-layout-decision.md (greenfield editor, "Vue for JUCE" goals).

    Everything the UI is allowed to touch lives here:
      - SlicerModel / SlicerEngine (forward-declared only, so this header is
        the *contract*, not an implementation): panels read/write their
        public members, exactly as the Phase-3 editor already does.
      - UiCallbacks: the handful of host interactions a GUI legitimately
        needs from the plugin shell (currently just resizing the host
        window). This is the only place such plumbing may appear -- panels
        and pages must not reach for the processor.
      - makeEditor(): the factory. PluginProcessor::createEditor() calls it
        and nothing else; a new GUI means writing a new gui.cpp that
        implements this function. The engine and model are never touched. */

class SlicerModel;
class SlicerEngine;

namespace ui
{

// Host interactions the UI is allowed to request. Grows only when a real
// host need appears (e.g. MIDI-learn state, host transport queries).
struct UiCallbacks
{
    // Ask the host to resize its window around the editor to newBounds
    // (component-local coordinates are fine -- the editor wraps them).
    std::function<void (juce::Rectangle<int> newBounds)> requestResize;
};

// Builds the complete plugin UI. `host` exists only for JUCE's own
// editor<->host plumbing (the returned editor must know its processor);
// the UI logic below the factory talks to model/engine exclusively.
std::unique_ptr<juce::AudioProcessorEditor> makeEditor (juce::AudioProcessor& host,
                                                        SlicerModel& model,
                                                        SlicerEngine& engine,
                                                        const UiCallbacks& callbacks);

} // namespace ui
