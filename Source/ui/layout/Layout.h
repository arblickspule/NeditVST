#pragma once

#include <JuceHeader.h>
#include "../NeditPalette.h"
#include <algorithm>
#include <memory>
#include <vector>

//==============================================================================
/** "Vue-for-JUCE" layout primitives -- see docs/ui-layout-decision.md.
    Row/Column own their children (std::unique_ptr) and lay themselves out,
    so UI construction reads like markup and no caller ever writes an
    addAndMakeVisible/setBounds chain again (addAndMakeVisible appears once,
    inside setCells() below).

    FLEXBOX BASE: every container is a ui::Flex over JUCE's real FlexBox
    (the "full flex DSL, not a hand-rolled subset" direction -- docs/ui-layout-
    decision.md Option D). The DSL exposes the full CSS flexbox vocabulary so
    the HTML-like mockup ports ~1:1:

      - container: FlexConfig{ .direction, .wrap, .justify, .align,
                               .alignContent, .gap, .padding }
      - item:      flex-grow / flex-shrink / flex-basis / align-self / order
                   via Cell + chainable .grow()/.shrink()/.basis()/.alignSelf()/
                   .order() on the cell factories' rvalues.
      - gap:       juce::FlexBox has NO gap property, so the container injects
                   spacer FlexItems (no component, fixed main-axis size)
                   between children -- hidden from callers, same result as CSS.

    Sizing contract (port of the old Box contract):
      - Leaves set their own size in their constructors (natural size).
      - A Flex grows along its main axis to fit its content as a FLOOR
        (getPreferredMainSize()); a parent that wants it bigger places it
        with a fill cell and it keeps the larger size.
      - Fixed cells keep their natural size; fill cells (grow > 0) share the
        leftover main-axis space. alignItems defaults to stretch (CSS-like):
        every child fills its cross-axis slot unless overridden.
      - A nested Flex's preferred size is computed from its own fixed content
        (same-orientation parent uses its main-axis floor, cross-orientation
        parent uses its cross-axis extent), so nested boxes converge without
        the caller sizing them.

    The host editor is responsible for sizing the root Flex; nested Flexes
    converge automatically because their preferred sizes depend only on
    leaf constants. */

namespace ui
{

// Full CSS-flex vocabulary, aliased from JUCE's FlexBox so the DSL reads
// like CSS without leaking construction detail. Maps 1:1:
//   Direction      <- flex-direction
//   Wrap           <- flex-wrap
//   Justify        <- justify-content
//   Align          <- align-items
//   AlignContent   <- align-content
//   AlignSelf      <- align-self (per item)
using Direction    = juce::FlexBox::Direction;
using Wrap         = juce::FlexBox::Wrap;
using Justify      = juce::FlexBox::JustifyContent;
using Align        = juce::FlexBox::AlignItems;
using AlignContent = juce::FlexBox::AlignContent;
using AlignSelf    = juce::FlexItem::AlignSelf;

// A flex container's own CSS properties.
struct FlexConfig
{
    Direction direction = Direction::row;
    Wrap wrap = Wrap::noWrap;
    Justify justify = Justify::flexStart;
    Align align = Align::stretch;
    AlignContent alignContent = AlignContent::stretch;
    float gap = 0.0f;                    // between-children spacing along the main axis
    juce::BorderSize<int> padding;       // inset around the whole box
};

// One slot in a Flex: a child plus its CSS flex-item properties.
struct Cell
{
    std::unique_ptr<juce::Component> component;
    float grow = 0.0f;      // flex-grow:  > 0 => absorbs leftover main-axis space
    float shrink = 1.0f;    // flex-shrink: rate at which the item shrinks when tight
    float basis = 0.0f;     // flex-basis:  0 => natural size (width/height or content)
    AlignSelf alignSelf = AlignSelf::autoAlign;
    int order = 0;

    // Chainable item modifiers -- rvalue-only so they read like CSS in a row():
    //   ui::cell (c).withGrow (1.0f).withAlignSelf (AlignSelf::center)
    Cell withGrow (float amount) &&     { grow = amount;       return std::move (*this); }
    Cell withShrink (float amount) &&   { shrink = amount;     return std::move (*this); }
    Cell withBasis (float amount) &&    { basis = amount;      return std::move (*this); }
    Cell withAlignSelf (AlignSelf a) && { alignSelf = a;       return std::move (*this); }
    Cell withOrder (int o) &&           { order = o;           return std::move (*this); }
};

// A fixed-size child (keeps whatever size its constructor set).
inline Cell cell (std::unique_ptr<juce::Component> child)
{
    Cell c;
    c.component = std::move (child);
    return c;
}

// A child that absorbs all leftover main-axis space (flex-grow: 1).
inline Cell fill (std::unique_ptr<juce::Component> child)
{
    Cell c;
    c.component = std::move (child);
    c.grow = 1.0f;
    return c;
}

// Promotes a nested box/gap to a flex-grow slot.
inline Cell fill (Cell c) { c.grow = 1.0f; return c; }

// A fixed gap of `size` pixels along the main axis.
inline Cell spacer (int size)
{
    auto blank = std::make_unique<juce::Component>();
    blank->setSize (size, size);
    Cell c;
    c.component = std::move (blank);
    return c;
}

// A fixed-size leaf (the DSL's "leaf sets own size in ctor" rule, applied to
// a component that doesn't self-size).
inline Cell sized (std::unique_ptr<juce::Component> child, int width, int height)
{
    child->setSize (width, height);
    Cell c;
    c.component = std::move (child);
    return c;
}

//==============================================================================
class Flex : public juce::Component
{
public:
    explicit Flex (FlexConfig newConfig = {}) : config (newConfig) {}

    void setCells (std::vector<Cell> newCells)
    {
        cells = std::move (newCells);

        // Capture each leaf's natural size ONCE, before any layout clobbers
        // its bounds (the degenerate 0x0 layout at construction zeroes every
        // child, so reading bounds live in makeItem()/getPreferredMainSize()
        // would shrink the box to nothing). Nested Flexes have no natural
        // entry -- their preferred sizes are computed recursively on demand.
        natural.clear();
        natural.reserve (cells.size());
        for (const auto& c : cells)
        {
            if (auto* child = dynamic_cast<const Flex*> (c.component.get()))
                natural.push_back (juce::Point<float> (-1.0f, -1.0f));
            else
                natural.push_back (c.component != nullptr
                    ? juce::Point<float> ((float) c.component->getWidth(), (float) c.component->getHeight())
                    : juce::Point<float> (0.0f, 0.0f));

            if (c.component != nullptr)
                addAndMakeVisible (*c.component); // the ONLY addAndMakeVisible in the codebase
        }

        resized();
    }

    void setDirection (Direction d)     { config.direction = d;     resized(); }
    void setWrap (Wrap w)               { config.wrap = w;          resized(); }
    void setJustify (Justify j)         { config.justify = j;       resized(); }
    void setAlign (Align a)             { config.align = a;         resized(); }
    void setAlignContent (AlignContent a) { config.alignContent = a; resized(); }
    void setGap (float gapPx)           { config.gap = gapPx;       resized(); }
    void setPadding (juce::BorderSize<int> newPadding) { config.padding = newPadding; resized(); }

    // Main-axis size this box needs to fit its fixed children (the growth
    // floor used by the parent's layout and by this box's own resized()).
    float getPreferredMainSize() const
    {
        float total = isHorizontal() ? config.padding.getLeftAndRight()
                                     : config.padding.getTopAndBottom();

        int fixedCount = 0;
        for (int i = 0; i < (int) cells.size(); ++i)
        {
            const auto& c = cells[i];
            if (c.component == nullptr || c.grow > 0.0f)
                continue;
            total += mainSizeOf (i);
            ++fixedCount;
        }

        if (fixedCount > 0)
            total += config.gap * (fixedCount - 1);

        return total;
    }

    // Size this box needs along its CROSS axis (perpendicular to its own
    // main axis): the widest child for a Column, the tallest child for a
    // Row. What a nested box's opposite-orientation parent uses as its
    // slot size -- without this, a Row inside a Column was sized from its
    // horizontal content width, not its content height.
    float getPreferredCrossSize() const
    {
        float best = isHorizontal() ? config.padding.getTopAndBottom()
                                    : config.padding.getLeftAndRight();

        for (int i = 0; i < (int) cells.size(); ++i)
            if (cells[i].component != nullptr)
                best = std::max (best, crossSizeOf (i));

        return best;
    }

    void resized() override
    {
        juce::FlexBox box;
        box.flexDirection  = config.direction;
        box.flexWrap       = config.wrap;
        box.justifyContent = config.justify;
        box.alignItems     = config.align;
        box.alignContent   = config.alignContent;

        bool first = true;
        for (int i = 0; i < (int) cells.size(); ++i)
        {
            const auto& c = cells[i];
            if (c.component == nullptr)
                continue;

            // juce::FlexBox has no gap -- inject an invisible spacer item
            // with a fixed main-axis size between real children.
            if (! first && config.gap > 0.0f)
                box.items.add (juce::FlexItem (config.gap, config.gap));

            box.items.add (makeItem (c, i));
            first = false;
        }

        box.performLayout (config.padding.subtractedFrom (getLocalBounds()));

        // Grow to fit content as a floor -- never shrink below what the
        // fixed children need (a parent may still stretch this box bigger
        // via a fill cell, which this box then keeps). Suppressed for
        // wrapping containers: their whole point is a constrained main axis
        // that content wraps within, not one that inflates to fit.
        if (config.wrap == Wrap::noWrap)
        {
            const float want = getPreferredMainSize();
            if (isHorizontal())
            {
                if (getWidth() < want)
                    setSize (juce::roundToInt (want), getHeight());
            }
            else
            {
                if (getHeight() < want)
                    setSize (getWidth(), juce::roundToInt (want));
            }
        }
    }

private:
    bool isHorizontal() const
    {
        return config.direction == Direction::row
            || config.direction == Direction::rowReverse;
    }

    // Translate a Cell into the FlexItem juce::FlexBox will lay out. Fixed
    // cells get their natural/preferred size along the MAIN axis so FlexBox
    // uses that as the flex-basis; grow cells leave both axes unassigned with
    // basis 0 so they grow from nothing (CSS flex: 1).
    //
    // The CROSS axis is only pinned to the natural/preferred size when the
    // effective alignment is not stretch (flexStart/center/flexEnd) -- JUCE
    // only stretches an item whose cross size is UNASSIGNED, so under the
    // default align: stretch we leave it alone and the item fills its slot
    // exactly like a CSS div.
    juce::FlexItem makeItem (const Cell& c, int index) const
    {
        juce::FlexItem item (*c.component);
        item.flexGrow   = c.grow;
        item.flexShrink = c.shrink;
        item.flexBasis  = c.basis;
        item.alignSelf  = c.alignSelf;
        item.order      = c.order;

        if (c.grow <= 0.0f)
        {
            // stretch only makes sense in a single-line container (the line
            // cross IS the container cross there); in a wrapping container the
            // item must keep its natural cross so JUCE can compute per-line
            // sizes from it.
            const bool crossStretches = (effectiveAlign (c) == Align::stretch)
                                     && (config.wrap == Wrap::noWrap);

            if (auto* child = dynamic_cast<const Flex*> (c.component.get()))
            {
                if (isHorizontal())
                {
                    item.width  = mainSizeOf (child);
                    if (! crossStretches)
                        item.height = crossSizeOf (child);
                }
                else
                {
                    item.height = mainSizeOf (child);
                    if (! crossStretches)
                        item.width = crossSizeOf (child);
                }
            }
            else
            {
                if (isHorizontal())
                {
                    item.width = natural[index].getX();
                    if (! crossStretches)
                        item.height = natural[index].getY();
                }
                else
                {
                    item.height = natural[index].getY();
                    if (! crossStretches)
                        item.width = natural[index].getX();
                }
            }
        }

        return item;
    }

    // The effective cross-axis alignment for a cell: its own align-self if
    // set, otherwise the container's align-items (alignSelf::autoAlign).
    Align effectiveAlign (const Cell& c) const
    {
        switch (c.alignSelf)
        {
            case AlignSelf::flexStart: return Align::flexStart;
            case AlignSelf::flexEnd:   return Align::flexEnd;
            case AlignSelf::center:    return Align::center;
            case AlignSelf::stretch:   return Align::stretch;
            case AlignSelf::autoAlign:
            default:                   return config.align;
        }
    }

    // A child's size along THIS box's main axis. Same orientation
    // (Column-in-Column) -> the child's own main-axis floor; cross
    // orientation (Row-in-Column) -> the child's cross-axis extent, since
    // that's the number this box slots the child into. Leaves use the
    // natural size captured at setCells().
    float mainSizeOf (int index) const
    {
        if (auto* child = dynamic_cast<const Flex*> (cells[index].component.get()))
            return mainSizeOf (child);
        return isHorizontal() ? natural[index].getX() : natural[index].getY();
    }

    // A child's size along THIS box's cross axis (the child's own cross-axis
    // extent): the widest child for a Column, tallest for a Row.
    float crossSizeOf (int index) const
    {
        if (auto* child = dynamic_cast<const Flex*> (cells[index].component.get()))
            return crossSizeOf (child);
        return isHorizontal() ? natural[index].getY() : natural[index].getX();
    }

    float mainSizeOf (const Flex* child) const
    {
        if (child->isHorizontal() == isHorizontal())
            return child->getPreferredMainSize();
        return child->getPreferredCrossSize();
    }

    float crossSizeOf (const Flex* child) const
    {
        if (child->isHorizontal() == isHorizontal())
            return child->getPreferredCrossSize();
        return child->getPreferredMainSize();
    }

    FlexConfig config;
    std::vector<Cell> cells;
    std::vector<juce::Point<float>> natural; // per-cell natural (w, h); (-1,-1) for nested Flexes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Flex)
};

//==============================================================================
// Everything a composition can contain is a Cell -- leaves, gaps, and whole
// nested boxes all return Cells, so nesting is direct:
//     ui::column (ui::row (ui::cell (a), ui::fill (b)))
// The host editor holds the top-level Cell (its .component owns the tree).
// Variadic (not brace-init) so the move-only Cells forward into the vector
// without copying.

// A flex container with explicit CSS-style properties:
//     ui::flex (FlexConfig{ .direction = Direction::column, .gap = 8,
//                            .align = Align::center }, ...)
template <typename... Cells>
Cell flex (FlexConfig config, Cells&&... items)
{
    std::vector<Cell> cells;
    cells.reserve (sizeof...(Cells));
    (cells.push_back (std::forward<Cells> (items)), ...);

    auto box = std::make_unique<Flex> (config);
    box->setCells (std::move (cells));
    return Cell { std::move (box) };
}

template <typename... Cells>
Cell row (Cells&&... items)
{
    return flex (FlexConfig{ .direction = Direction::row }, std::forward<Cells> (items)...);
}

template <typename... Cells>
Cell column (Cells&&... items)
{
    return flex (FlexConfig{ .direction = Direction::column }, std::forward<Cells> (items)...);
}

//==============================================================================
/** A titled group that OWNS its content -- replaces SectionPanel's backdrop
    role (see docs/ui-layout-decision.md section 3D). Just a vertical Flex whose
    top 26px are a painted title strip; the content cells sit below it in the
    inset area, so section height is "26 + content + padding", computed by the
    same getPreferredMainSize() growth rule every other Flex uses.

    The title is decoration, not a child -- callers rebuild the content any
    time a selection/toggle changes the section's rows. */
class TitledPanel : public Flex
{
public:
    explicit TitledPanel (juce::String titleText)
        : Flex (FlexConfig{ .direction = Direction::column,
                            .gap = 4.0f,
                            .padding = juce::BorderSize<int> (titleBarHeight + 6, 8, 6, 8) }),
          title (std::move (titleText))
    {
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.setColour (NeditPalette::tungsten);
        g.fillRect (bounds);

        auto titleBar = bounds.removeFromTop (titleBarHeight);
        g.setColour (NeditPalette::tungstenTint (1.35f));
        g.fillRect (titleBar);

        g.setColour (NeditPalette::salmon);
        g.fillRect (titleBar.removeFromBottom (2));

        g.setColour (NeditPalette::textOnTungsten);
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        g.drawFittedText (title, titleBar.reduced (8, 0), juce::Justification::centredLeft, 1);

        g.setColour (NeditPalette::tungstenTint (1.6f));
        g.drawRect (getLocalBounds(), 1);
    }

    static constexpr int titleBarHeight = 26;

private:
    juce::String title;
};

// A titled section composed directly from cells -- same shape as row()/
// column()/flex(): everything inside is a Cell, and the section itself is a
// Cell that nests wherever the parent places it.
template <typename... Cells>
Cell section (juce::String title, Cells&&... items)
{
    std::vector<Cell> cells;
    cells.reserve (sizeof...(Cells));
    (cells.push_back (std::forward<Cells> (items)), ...);

    auto panel = std::make_unique<TitledPanel> (std::move (title));
    panel->setCells (std::move (cells));
    return Cell { std::move (panel) };
}

} // namespace ui
