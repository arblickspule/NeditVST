#pragma once

#include <JuceHeader.h>

namespace ui
{
namespace drawable
{

// Parse an inline SVG document into a Drawable tree (nullptr on failure).
// The JUCE SVG parser (juce_gui_basics/juce_SVGParser.cpp) handles the
// presentation attributes used by the artwork below -- viewBox, path/rect/
// circle, fill/stroke, stroke-width, rx/ry -- but not CSS style="" rules or
// <defs> markers, so the art is deliberately authored inside that subset.
inline std::unique_ptr<juce::Drawable> drawableFromSVG (const juce::String& svg)
{
    if (auto xml = juce::XmlDocument::parse (svg))
        return juce::Drawable::createFromSVG (*xml);

    return {};
}

//==============================================================================
// Palette hexes (NeditPalette.h) are baked into the strings so the art stays
// self-contained: tungsten #383838, salmon #FF7E79, dark groove #262626,
// mid stroke #555555, panel tile #4A4A4A (= tungstenTint 1.32), text #2A2020.
namespace art
{

// A 72x72 knob face: tungsten body, mid ring, salmon pointer from the centre
// to the top edge. Rotating this whole Drawable IS the knob's value sweep;
// the art stays inside the viewBox at every angle so nothing clips.
inline constexpr const char* knob =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="72" height="72" viewBox="0 0 72 72">)"
    R"(<circle cx="36" cy="36" r="28" fill="#383838"/>)"
    R"(<circle cx="36" cy="36" r="28" fill="none" stroke="#555555" stroke-width="2"/>)"
    R"(<circle cx="36" cy="36" r="21" fill="none" stroke="#555555" stroke-width="1"/>)"
    R"(<path d="M36 36 L36 20" stroke="#FF7E79" stroke-width="6"/>)"
    R"(</svg>)";

// A 240x14 horizontal groove with rounded ends; the thumb is drawn on top of
// it at the value's position.
inline constexpr const char* sliderTrack =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="14" viewBox="0 0 240 14">)"
    R"(<rect x="1" y="3" width="238" height="8" rx="4" ry="4" fill="#262626"/>)"
    R"(<rect x="1" y="3" width="238" height="8" rx="4" ry="4" fill="none" stroke="#555555" stroke-width="1"/>)"
    R"(</svg>)";

// A 20x20 salmon thumb with a near-black rim.
inline constexpr const char* sliderThumb =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20">)"
    R"(<circle cx="10" cy="10" r="8" fill="#FF7E79"/>)"
    R"(<circle cx="10" cy="10" r="8" fill="none" stroke="#2A2020" stroke-width="1.5"/>)"
    R"(</svg>)";

// 9-slice panel art (tileSize 30): four 30x30 corner tiles with one rounded
// corner each, four 30x30 edge tiles carrying one straight border line, and
// one plain centre tile. Tiled edge-to-edge they form a rounded panel with a
// salmon outline at ANY size: corners are drawn at native size so they stay
// crisp, only the edge/centre tiles stretch. Every tile fills its whole
// 30x30 square (corners are L-shaped fills), so the interior is continuous.
// The border stroke is centred on the tile's outer edge (y=1 / x=1) with
// stroke-width 2, so neighbouring tiles' lines join exactly.
inline constexpr const char* panelCornerTL =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<path d="M10 0 H30 V30 H0 V10 Q0 0 10 0 Z" fill="#4A4A4A"/>)"
    R"(<path d="M10 1 H29 V29 H1 V10 Q1 1 10 1 Z" fill="none" stroke="#FF7E79" stroke-width="2"/>)"
    R"(</svg>)";

inline constexpr const char* panelCornerTR =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<path d="M0 0 H20 Q30 0 30 10 V30 H0 Z" fill="#4A4A4A"/>)"
    R"(<path d="M1 1 H20 Q29 1 29 10 V29 H1 Z" fill="none" stroke="#FF7E79" stroke-width="2"/>)"
    R"(</svg>)";

inline constexpr const char* panelCornerBR =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<path d="M0 0 H30 V20 Q30 30 20 30 H0 Z" fill="#4A4A4A"/>)"
    R"(<path d="M1 1 H29 V20 Q29 29 20 29 H1 Z" fill="none" stroke="#FF7E79" stroke-width="2"/>)"
    R"(</svg>)";

inline constexpr const char* panelCornerBL =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<path d="M30 0 H0 V20 Q0 30 10 30 H30 Z" fill="#4A4A4A"/>)"
    R"(<path d="M29 1 H1 V20 Q1 29 10 29 H29 Z" fill="none" stroke="#FF7E79" stroke-width="2"/>)"
    R"(</svg>)";

inline constexpr const char* panelEdgeTop =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<rect width="30" height="30" fill="#4A4A4A"/>)"
    R"(<path d="M1 1 H29" fill="none" stroke="#FF7E79" stroke-width="2"/>)"
    R"(</svg>)";

inline constexpr const char* panelEdgeBottom =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<rect width="30" height="30" fill="#4A4A4A"/>)"
    R"(<path d="M1 29 H29" fill="none" stroke="#FF7E79" stroke-width="2"/>)"
    R"(</svg>)";

inline constexpr const char* panelEdgeLeft =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<rect width="30" height="30" fill="#4A4A4A"/>)"
    R"(<path d="M1 1 V29" fill="none" stroke="#FF7E79" stroke-width="2"/>)"
    R"(</svg>)";

inline constexpr const char* panelEdgeRight =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<rect width="30" height="30" fill="#4A4A4A"/>)"
    R"(<path d="M29 1 V29" fill="none" stroke="#FF7E79" stroke-width="2"/>)"
    R"(</svg>)";

inline constexpr const char* panelCentre =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">)"
    R"(<rect width="30" height="30" fill="#4A4A4A"/>)"
    R"(</svg>)";

// The 9 tiles in draw order (corners first, then edges, then centre).
inline constexpr const char* panelSlices[9] =
{
    panelCornerTL, panelCornerTR, panelCornerBR, panelCornerBL,
    panelEdgeTop,  panelEdgeBottom, panelEdgeLeft, panelEdgeRight,
    panelCentre,
};

} // namespace art
} // namespace drawable
} // namespace ui
