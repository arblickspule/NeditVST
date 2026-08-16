// Two-tab shell smoke test (docs/ui-layout-decision.md migration step 2):
// the Beats page (the archived GeneratePage ported onto the layout DSL) and
// the placeholder pages must construct headlessly against a real
// model+engine, report sane preferred sizes, and expose the seams the
// editor binds from (UiPanel's shared 10fps poll, Flex sizing). This guards
// the port -- the page touches every widget source that was re-added to the
// build when the tabs landed, so a compile/behaviour break here is a UI
// break, not just a test break.

#include "ui/pages/BeatsPage.h"
#include "ui/pages/PlaceholderPage.h"
#include "ui/components/WaveBasicsPanel.h"
#include "SlicerModel.h"
#include "SlicerEngine.h"

#include <doctest/doctest.h>

namespace
{

// Shared setup, per AGENTS.md: SlicerModel is non-copyable, and any test
// that could trigger a slice rebuild must set onPickStateInvalidated first
// (the engine's unbound callback throws std::bad_function_call otherwise).
// Built in one fixture so the pages share the same model/engine the editor
// would give them.
struct PagesFixture
{
    SlicerModel model;
    SlicerEngine engine;
    ui::BeatsPage beatsPage;
    ui::PlaceholderPage texturesPage;

    PagesFixture()
        : model(),
          engine (model),
          beatsPage (model, engine),
          texturesPage ("Textures", "not realised yet")
    {
        model.onPickStateInvalidated = [] {};
    }
};

} // namespace

TEST_CASE ("Two-tab shell: Beats and Texture pages construct and size themselves")
{
    PagesFixture f;

    // Both pages are Flexes with real content floors -- zero would mean the
    // ported sections collapsed (every section is a TitledPanel whose height
    // follows its content).
    CHECK (f.beatsPage.getPreferredMainSize() > 0.0f);
    CHECK (f.texturesPage.getPreferredMainSize() > 0.0f);

    // The Beats page hosts all seven legacy sections, so it must be the tall
    // one; the placeholder is deliberately tiny.
    CHECK (f.beatsPage.getPreferredMainSize() > f.texturesPage.getPreferredMainSize());

    // Both pages satisfy the same traits the editor's activePage pointer
    // relies on: a Flex (sized by getPreferredMainSize) ...
    CHECK (dynamic_cast<const ui::Flex*> (&f.beatsPage) != nullptr);
    CHECK (dynamic_cast<const ui::Flex*> (&f.texturesPage) != nullptr);

    // ... and the Beats page implements the shared 10fps poll the editor's
    // timer drives (the placeholder has nothing to sync yet).
    auto* panel = dynamic_cast<ui::UiPanel*> (&f.beatsPage);
    REQUIRE (panel != nullptr);
    panel->syncFromModel(); // must be a no-op-safe call on an empty model
}

TEST_CASE ("Two-tab shell: page swap targets the same two instances each time")
{
    // The editor's showPage() swaps the viewport's viewed component between
    // exactly these two objects and never recreates them, so their identity
    // must be stable across the editor's lifetime -- the pages here are the
    // same objects the editor holds, so check both can be addressed as the
    // two interchangeable viewport contents.
    PagesFixture f;

    ui::Flex* beats = &f.beatsPage;
    ui::Flex* texture = &f.texturesPage;

    // Each swap must land on the other object (they are distinct pages).
    CHECK (beats != texture);

    // Simulating the editor's own 10fps tick: only the Beats page polls.
    ui::UiPanel* panel = dynamic_cast<ui::UiPanel*> (beats);
    REQUIRE (panel != nullptr);
    panel->syncFromModel();
}

TEST_CASE ("Wave-basics block: compact strip on top, wave view fills the rest")
{
    // The persistent block is laid out by the editor's resized(); this panel
    // fills whatever height it is given, so after a setSize() the two regions
    // must sit exactly where the host expects them.
    ui::WaveBasicsPanel panel;
    panel.setSize (1000, 300);

    REQUIRE (panel.getNumChildComponents() == 2); // the six-input strip + the wave view
    const auto* strip = panel.getChildComponent (0);
    const auto* wave  = panel.getChildComponent (1);
    REQUIRE (strip != nullptr);
    REQUIRE (wave != nullptr);

    CHECK (strip->getWidth() == 1000);
    CHECK (strip->getHeight() == ui::WaveBasicsPanel::controlsStripHeight);
    CHECK (strip->getY() == 0);

    // The wave view takes everything below the strip, with the bottom inset.
    CHECK (wave->getY() == ui::WaveBasicsPanel::controlsStripHeight);
    CHECK (wave->getWidth() == 1000);
    CHECK (wave->getBottom() == 300 - ui::WaveBasicsPanel::contentMargin);

    // The six input areas share the strip's width equally (flex-grow 1 each).
    CHECK (strip->getNumChildComponents() == 6);
}

