// Nedit -- Plugin layer.
//
// The VSTGUI editor shell: a programmatic CFrame (no uidescription XML)
// whose views are stateless renderers over NeditProcessor's state.
// All geometry derives from Theme.h tokens and getViewSize(); nothing
// hardcodes absolute pixel positions.

#pragma once

#include "public.sdk/source/vst/vstguieditor.h"

#include "vstgui/lib/controls/icontrollistener.h"

namespace VSTGUI { class CTextLabel; class CTextButton; }

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

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

    // Only forward real parameter IDs to the host's edit protocol; editor-
    // local control tags (e.g. the Load button) must never reach it, or the
    // host logs "beginEdit() with an invalid parameter ID".
    void beginEdit (VSTGUI_INT32 index) override;
    void endEdit (VSTGUI_INT32 index) override;

    //--- IControlListener ----------------------------------------------------
    void valueChanged (VSTGUI::CControl* control) override;

    //--- CBaseObject (idle timer) --------------------------------------------
    VSTGUI::CMessageResult notify (VSTGUI::CBaseObject* sender,
                                   VSTGUI::IdStringPtr message) override;

private:
    void runFileSelector();
    void applyParamFromControl (VSTGUI::CControl& control);
    void styleAuditionButton();

    NeditProcessor* owner_ = nullptr;
    WaveformView* waveformView_ = nullptr;
    VSTGUI::CTextLabel* sampleNameLabel_ = nullptr; // app-bar sample pill
    VSTGUI::CTextButton* auditionBtn_ = nullptr;    // app-bar audition toggle
    VSTGUI::CTextButton* loadBtn_ = nullptr;         // app-bar load button

    // Async native file dialog. Runs on a detached background thread so the
    // UI/run-loop thread keeps servicing X events (a blocking dialog on the
    // embedded X11 window thread deadlocks the X server -> desktop freeze).
    // Shared state outlives the editor via shared_ptr, so a still-open
    // dialog can't write into a destroyed editor.
    struct FileDialogState
    {
        std::mutex mutex;
        std::string path;
        std::atomic<bool> ready { false };
        std::atomic<bool> running { false };
    };
    std::shared_ptr<FileDialogState> fileDialog_;
    bool testAutoloadFired_ = false;   // NEDIT_TEST_AUTOLOAD one-shot
    bool lastAuditionSync_ = false;    // idle-timer dedup for audition btn
    bool lastSampleSync_  = false;     // idle-timer dedup for sample presence
    std::string lastSamplePath_;        // idle-timer dedup for sample name
};

} // namespace nedit::plugin
