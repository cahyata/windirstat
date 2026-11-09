// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once
#include "GlassList.h"

// GPU "liquid glass" overlay. A DirectComposition visual is placed on top of a window (and all of its
// child windows). For each requested region the live window content underneath is captured and re-drawn
// with Direct2D as a refracting rim (displacement map), plus specular highlights and a frosted tint.
// The overlay never receives input; mouse and keyboard fall through to the real controls.
struct GlassRegion
{
    RECT rect;     // in client coordinates of the attached window
    float radius;  // corner radius in pixels
};

// A toolbar glyph drawn from an image list, with animated hover / press feedback.
struct GlassIcon
{
    RECT rect;          // button rectangle in client coordinates
    HIMAGELIST list;    // image list holding the glyph (normal or disabled variant)
    int index;          // image index in `list`
    int id;             // stable key (command id) for the animation state
    bool hot;           // cursor is over the button
    bool pressed;       // button is held down
    bool checked;       // toggled on
    wchar_t glyph = 0;  // Segoe Fluent Icons code point; 0 draws the image list bitmap instead
    bool enabled = true;
    bool accent = false;// primary action: tinted with the accent colour
};

// A line of text drawn with DirectWrite.
struct GlassText
{
    RECT rect;          // in client coordinates
    std::wstring text;
    COLORREF color;
    int fontPx;         // font height in pixels
};

// A list row: the overlay adds a glass finish to selected rows and a hover glow to the row under the cursor.
struct GlassRow
{
    RECT rect;          // row capsule in client coordinates
    RECT clip;          // visible area of the list (below the header)
    float radius;
    int id;             // stable key for the hover animation
    bool selected;
    bool hot;
    bool focused;       // list has keyboard focus (selection is dimmer otherwise)
};

struct GlassLayer
{
    bool dark = false;                 // dark appearance: pills, glyphs and lenses use the dark palette
    float scale = 1.0f;                // DPI scale (1.0 = 96 dpi): decoration sizes are multiplied by it
    std::vector<RECT> groups;          // rounded pills behind each toolbar button group
    std::vector<GlassMenuBar> menuBars;
    std::vector<GlassTabStrip> tabStrips;
    std::vector<GlassSplitter> splitters;
    std::vector<GlassTreeMap> treeMaps;
    std::vector<std::shared_ptr<const GlassListView>> lists;   // lists painted completely by the compositor
    std::vector<GlassRow> rows;
    std::vector<GlassRegion> regions;
    std::vector<GlassIcon> icons;
    std::vector<GlassText> texts;
};

namespace GlassOverlay
{
    using LayerProvider = std::function<GlassLayer()>;

    // Returns false when the required GPU/composition features are not available.
    bool Attach(HWND window, LayerProvider provider);

    // True once an overlay is attached and drawing: classic controls then skip painting what the overlay draws.
    bool IsActive();

    // Draws the overlay of `window` right now (no-op when none is attached). Layout changes that move panes under the
    // overlay call this so the overlay and the classic controls are composed in the same frame instead of lagging apart.
    void Flush(HWND window);
}
