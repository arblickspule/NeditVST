// Nedit -- Plugin layer.
//
// The VSTGUI editor shell: a programmatic CFrame (no uidescription XML)
// whose views are stateless renderers over NeditProcessor's state and
// sample slot. All geometry/zoom/pan math lives in ui::WaveformGeometry
// (tested headless); this file only pushes pixels and routes input.

#pragma once

#include "public.sdk/source/vst/vstguieditor.h"

#include "vstgui/lib/controls/icontrollistener.h"

namespace nedit::plugin {

class NeditProcessor;
class WaveformView;

class NeditEditor : public Steinberg::Vst::VSTGUIEditor,
                    public VSTGUI::IControlListener
{
public:
    explicit NeditEditor (NeditProcessor* owner);

    // Control tags. Values < 1000 are ParamIDs routed to the host edit
    // protocol; the rest are editor-local actions.
    static constexpr std::int32_t kTagLoadSample = 1000;

    //--- VSTGUIEditor --------------------------------------------------------
    bool PLUGIN_API open (void* parent, const VSTGUI::PlatformType& platformType) override;
    void PLUGIN_API close() override;

    //--- IControlListener ----------------------------------------------------
    void valueChanged (VSTGUI::CControl* control) override;

    //--- CBaseObject (idle timer) --------------------------------------------
    VSTGUI::CMessageResult notify (VSTGUI::CBaseObject* sender,
                                   VSTGUI::IdStringPtr message) override;

private:
    void runFileSelector();
    void applyParamFromControl (VSTGUI::CControl& control);

    NeditProcessor* owner_ = nullptr;
    WaveformView* waveform_ = nullptr;        // owned by the frame
    const void* lastSampleIdentity_ = nullptr; // change detection for redraw
};

} // namespace nedit::plugin
