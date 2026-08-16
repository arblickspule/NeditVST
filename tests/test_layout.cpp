// Headless layout tests for the ui::Flex DSL (Source/ui/layout/Layout.h) --
// exercises the CSS-flex contract the page builders rely on without any
// plugin client or GUI: fixed/natural sizes, grow/fill, gap, stretch vs
// explicit cross alignment, nested cross-orientation slotting, wrap, the
// auto-grow floor, justify, and TitledPanel sizing.
//
// JUCE components are created on the test thread; in the Release build (no
// jasserts) setSize()/resized() run synchronously, so bounds are checked
// immediately after the layout call.

#include <JuceHeader.h>
#include "ui/layout/Layout.h"
#include "doctest/doctest.h"

namespace
{

// A fixed-size leaf with a raw pointer retained for bounds inspection.
std::unique_ptr<juce::Component> leaf (juce::Component*& out, int w, int h)
{
    auto c = std::make_unique<juce::Component>();
    c->setSize (w, h);
    out = c.get();
    return c;
}

// Build a single-child vector the move-only way (no braced-init-lists).
std::vector<ui::Cell> cells (ui::Cell&& c)
{
    std::vector<ui::Cell> out;
    out.push_back (std::move (c));
    return out;
}

} // namespace

TEST_CASE ("Layout: row lays out fixed leaves at natural widths, stretched height")
{
    juce::Component* a = nullptr;
    juce::Component* b = nullptr;
    auto tree = ui::row (ui::cell (leaf (a, 40, 20)),
                         ui::cell (leaf (b, 60, 20)));

    auto* row = static_cast<ui::Flex*> (tree.component.get());
    row->setSize (200, 50);

    CHECK (a->getX() == 0);
    CHECK (a->getY() == 0);
    CHECK (a->getWidth() == 40);
    CHECK (a->getHeight() == 50);      // default align: stretch fills the cross axis
    CHECK (b->getX() == 40);
    CHECK (b->getWidth() == 60);
}

TEST_CASE ("Layout: gap between children along the main axis")
{
    juce::Component* a = nullptr;
    juce::Component* b = nullptr;
    auto tree = ui::row (ui::cell (leaf (a, 40, 20)),
                         ui::cell (leaf (b, 60, 20)));

    auto* row = static_cast<ui::Flex*> (tree.component.get());
    row->setGap (8.0f);
    row->setSize (200, 50);

    CHECK (b->getX() == 48);
}

TEST_CASE ("Layout: fill cell absorbs leftover main-axis space")
{
    juce::Component* a = nullptr;
    juce::Component* f = nullptr;
    auto tree = ui::row (ui::cell (leaf (a, 40, 20)),
                         ui::fill (leaf (f, 40, 20)));

    auto* row = static_cast<ui::Flex*> (tree.component.get());
    row->setSize (200, 50);

    CHECK (a->getWidth() == 40);
    CHECK (f->getX() == 40);
    CHECK (f->getWidth() == 160);
}

TEST_CASE ("Layout: column lays out fixed leaves at natural heights, stretched width")
{
    juce::Component* a = nullptr;
    juce::Component* b = nullptr;
    auto tree = ui::column (ui::cell (leaf (a, 40, 20)),
                            ui::cell (leaf (b, 60, 30)));

    auto* col = static_cast<ui::Flex*> (tree.component.get());
    col->setSize (200, 100);

    CHECK (a->getY() == 0);
    CHECK (a->getHeight() == 20);
    CHECK (a->getWidth() == 200);      // cross axis stretched
    CHECK (b->getY() == 20);
    CHECK (b->getHeight() == 30);
}

TEST_CASE ("Layout: nested cross-orientation box slots by its cross extent, stretches to width")
{
    juce::Component* x = nullptr;
    juce::Component* y = nullptr;
    auto innerRow = ui::row (ui::cell (leaf (x, 40, 20)),
                             ui::cell (leaf (y, 60, 30)));
    auto* inner = static_cast<ui::Flex*> (innerRow.component.get());

    auto tree = ui::column (std::move (innerRow));
    auto* col = static_cast<ui::Flex*> (tree.component.get());
    col->setSize (200, 100);

    // The row's slot height = its cross extent (tallest child, 30), NOT its
    // preferred main size (40+60=100) -- the classic Row-in-Column bug.
    CHECK (inner->getY() == 0);
    CHECK (inner->getHeight() == 30);
    CHECK (inner->getWidth() == 200);  // stretched by the column's align: stretch
    CHECK (y->getX() == 40);
    CHECK (y->getHeight() == 30);
}

TEST_CASE ("Layout: auto-grows to its content floor when sized too small")
{
    juce::Component* a = nullptr;
    juce::Component* b = nullptr;
    auto tree = ui::column (ui::cell (leaf (a, 40, 20)),
                            ui::cell (leaf (b, 60, 30)));

    auto* col = static_cast<ui::Flex*> (tree.component.get());
    col->setSize (200, 10);            // far too small for two fixed leaves

    CHECK (col->getHeight() == 50);    // 20 + 30 content floor
    CHECK (b->getY() == 20);
}

TEST_CASE ("Layout: justify-content centers fixed content on the main axis")
{
    juce::Component* a = nullptr;
    juce::Component* b = nullptr;
    auto tree = ui::flex (ui::FlexConfig{ .direction = ui::Direction::row,
                                          .justify = ui::Justify::center },
                          ui::cell (leaf (a, 40, 20)),
                          ui::cell (leaf (b, 60, 20)));

    auto* row = static_cast<ui::Flex*> (tree.component.get());
    row->setSize (200, 50);

    CHECK (a->getX() == 50);           // (200 - 100) / 2
    CHECK (b->getX() == 90);
}

TEST_CASE ("Layout: align-items center pins the cross axis instead of stretching")
{
    juce::Component* a = nullptr;
    auto tree = ui::flex (ui::FlexConfig{ .direction = ui::Direction::row,
                                          .align = ui::Align::center },
                          ui::cell (leaf (a, 40, 20)));

    auto* row = static_cast<ui::Flex*> (tree.component.get());
    row->setSize (200, 50);

    CHECK (a->getHeight() == 20);      // not stretched
    CHECK (a->getY() == 15);           // (50 - 20) / 2
}

TEST_CASE ("Layout: align-self overrides the container's align-items")
{
    juce::Component* a = nullptr;
    juce::Component* b = nullptr;
    auto tree = ui::row (ui::cell (leaf (a, 40, 20)),
                         ui::cell (leaf (b, 60, 20)).withAlignSelf (ui::AlignSelf::flexStart));

    auto* row = static_cast<ui::Flex*> (tree.component.get());
    row->setSize (200, 50);

    CHECK (a->getHeight() == 50);      // stretches with the default
    CHECK (b->getHeight() == 20);      // opts out
    CHECK (b->getY() == 0);
}

TEST_CASE ("Layout: wrap stacks overflowing items onto a new line")
{
    juce::Component* a = nullptr;
    juce::Component* b = nullptr;
    auto tree = ui::flex (ui::FlexConfig{ .direction = ui::Direction::row,
                                          .wrap = ui::Wrap::wrap,
                                          .alignContent = ui::AlignContent::flexStart },
                          ui::cell (leaf (a, 60, 20)),
                          ui::cell (leaf (b, 60, 20)));

    auto* row = static_cast<ui::Flex*> (tree.component.get());
    row->setSize (90, 60);

    CHECK (a->getY() == 0);
    CHECK (b->getY() == 20);           // wrapped to the second line
    CHECK (b->getX() == 0);
}

TEST_CASE ("Layout: TitledPanel preferred size includes title bar, padding, content")
{
    juce::Component* a = nullptr;
    auto panel = std::make_unique<ui::TitledPanel> ("S");
    panel->setCells (cells (ui::cell (leaf (a, 100, 30))));

    const int expected = ui::TitledPanel::titleBarHeight + 6 + 30 + 6; // top inset + content + bottom inset
    CHECK (panel->getPreferredMainSize() == expected);
}
