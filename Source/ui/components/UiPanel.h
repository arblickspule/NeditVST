#pragma once

#include <JuceHeader.h>

//==============================================================================
/** Base for feature panels/pages behind the ui::contract seam -- anything
    that re-reads its model bindings on the shared 10fps poll
    (docs/ui-layout-decision.md decision 3).

    A pure interface (no Component base) so a panel that is already a
    ui::layout Box -- like GeneratePage -- can implement it without dragging
    in a second Component subobject. gui.cpp owns the one Timer and calls
    syncFromModel() on its page every tick. */
namespace ui
{

class UiPanel
{
public:
    virtual ~UiPanel() = default;

    // Re-read model state the controls bind to (polled UI): undo/redo
    // enablement, audition button state, anything the engine can change out
    // from under the widgets. Cheap enough to run at 10fps.
    virtual void syncFromModel() = 0;
};

} // namespace ui
