#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <memory>
#include <vector>

//==============================================================================
/** "Vue-for-JUCE" layout primitives -- see docs/ui-layout-decision.md.
    Row/Column own their children (std::unique_ptr) and lay themselves out,
    so UI construction reads like markup and no caller ever writes an
    addAndMakeVisible/setBounds chain again (addAndMakeVisible appears once,
    inside setCells() below).

    Sizing contract:
      - Leaves set their own size in their constructors (fixed size).
      - A Box grows along its main axis to fit its content as a FLOOR
        (getPreferredMainSize()); a parent that wants it bigger places it
        with a fill/weight cell and it keeps the larger size.
      - Fixed cells keep their main-axis size; fill cells (weight > 0)
        share the leftover main-axis space. Every child stretches to the
        full cross-axis size of its slot.

    The host editor is responsible for sizing the root Box; nested Boxes
    converge automatically because their preferred sizes depend only on
    leaf constants. */

namespace ui
{

// One slot in a Row/Column: a child plus layout hints.
struct Cell
{
    std::unique_ptr<juce::Component> component;
    float weight = 0.0f; // > 0 => this child absorbs leftover main-axis space
};

// A fixed-size child (keeps whatever size its constructor set).
inline Cell cell (std::unique_ptr<juce::Component> child) { return Cell { std::move (child), 0.0f }; }

// A child that absorbs all leftover main-axis space (flex-grow).
inline Cell fill (std::unique_ptr<juce::Component> child) { return Cell { std::move (child), 1.0f }; }

// Promotes a nested box/gap to a flex-grow slot.
inline Cell fill (Cell c) { c.weight = 1.0f; return c; }

// A fixed gap of `size` pixels along the main axis.
inline Cell spacer (int size)
{
    auto blank = std::make_unique<juce::Component>();
    blank->setSize (size, size);
    return Cell { std::move (blank), 0.0f };
}

//==============================================================================
class Box : public juce::Component
{
public:
    explicit Box (bool isVertical)
        : vertical (isVertical)
    {
    }

    void setCells (std::vector<Cell> newCells)
    {
        cells = std::move (newCells);
        for (const auto& c : cells)
            if (c.component != nullptr)
                addAndMakeVisible (*c.component); // the ONLY addAndMakeVisible in the codebase
        resized();
    }

    void setGap (int gapPx)                     { gap = gapPx; }
    void setPadding (juce::BorderSize<int> newPadding) { padding = newPadding; }

    // Main-axis size this box needs to fit its fixed children (the growth
    // floor used by the parent's layout and by this box's own resized()).
    int getPreferredMainSize() const
    {
        int total = vertical ? padding.getTopAndBottom() : padding.getLeftAndRight();

        for (const auto& c : cells)
        {
            if (c.weight > 0.0f)
                continue;
            total += mainSizeOf (c.component);
        }

        if (! cells.empty())
            total += gap * static_cast<int> (cells.size() - 1);

        return total;
    }

    void resized() override
    {
        const auto inner = padding.subtractedFrom (getLocalBounds());
        if (cells.empty())
            return;

        const int available = vertical ? inner.getHeight() : inner.getWidth();

        int fixedTotal = 0;
        float weightSum = 0.0f;
        for (const auto& c : cells)
        {
            if (c.weight > 0.0f)
                weightSum += c.weight;
            else
                fixedTotal += mainSizeOf (c.component);
        }

        const int totalGap = gap * static_cast<int> (cells.size() - 1);
        const int leftover = std::max (0, available - fixedTotal - totalGap);

        int pos = vertical ? inner.getY() : inner.getX();
        for (const auto& c : cells)
        {
            const int slotMain = (c.weight > 0.0f)
                ? (weightSum > 0.0f ? juce::roundToInt (leftover * (c.weight / weightSum)) : 0)
                : mainSizeOf (c.component);

            if (c.component != nullptr)
            {
                if (vertical)
                    c.component->setBounds (juce::Rectangle<int> (inner.getX(), pos, inner.getWidth(), slotMain));
                else
                    c.component->setBounds (juce::Rectangle<int> (pos, inner.getY(), slotMain, inner.getHeight()));
            }

            pos += slotMain + gap;
        }

        // Grow to fit content as a floor -- never shrink below what the
        // fixed children need (a parent may still stretch this box bigger
        // via a fill cell, which this box then keeps).
        const int want = getPreferredMainSize();
        if (vertical)
        {
            if (getHeight() < want)
                setSize (getWidth(), want);
        }
        else
        {
            if (getWidth() < want)
                setSize (want, getHeight());
        }
    }

private:
    // A nested Box's fixed main size is its content floor, not its stale
    // current size; leaves use whatever size their constructor set.
    int mainSizeOf (const std::unique_ptr<juce::Component>& child) const
    {
        if (auto* childBox = dynamic_cast<const Box*> (child.get()))
            return childBox->getPreferredMainSize();

        return vertical ? child->getHeight() : child->getWidth();
    }

    bool vertical;
    int gap = 4;
    juce::BorderSize<int> padding;
    std::vector<Cell> cells;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Box)
};

//==============================================================================
class Row : public Box
{
public:
    explicit Row (std::vector<Cell> items)
        : Box (false)
    {
        setCells (std::move (items));
    }
};

class Column : public Box
{
public:
    explicit Column (std::vector<Cell> items)
        : Box (true)
    {
        setCells (std::move (items));
    }
};

// Everything a composition can contain is a Cell -- leaves, gaps, and whole
// nested boxes all return Cells, so nesting is direct:
//     ui::column (ui::row (ui::cell (a), ui::fill (b)))
// The host editor holds the top-level Cell (its .component owns the tree).
// Variadic (not brace-init) so the move-only Cells forward into the vector
// without copying.
template <typename... Cells>
Cell row (Cells&&... items)
{
    std::vector<Cell> cells;
    cells.reserve (sizeof...(Cells));
    (cells.push_back (std::forward<Cells> (items)), ...);
    return Cell { std::make_unique<Row> (std::move (cells)), 0.0f };
}

template <typename... Cells>
Cell column (Cells&&... items)
{
    std::vector<Cell> cells;
    cells.reserve (sizeof...(Cells));
    (cells.push_back (std::forward<Cells> (items)), ...);
    return Cell { std::make_unique<Column> (std::move (cells)), 0.0f };
}

} // namespace ui
