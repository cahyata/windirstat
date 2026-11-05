// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

// Plain data describing a list control for the GPU compositor (GlassOverlay). The classic list keeps its items,
// sorting, expansion and input handling; each frame it snapshots what is visible into a GlassListView and the
// overlay paints it with Direct2D / DirectWrite. All rectangles are in client coordinates of the main frame.

struct GlassCell
{
    enum class Kind : unsigned char
    {
        Text,    // plain text
        Label,   // optional tree expander, icon and text
        Bar,     // size proportion bar
        Swatch,  // colour chip
    };
    Kind kind = Kind::Text;
    std::wstring text;
    COLORREF textColor = CLR_NONE;   // CLR_NONE: use the list text colour

    // Label
    HICON icon = nullptr;
    bool tree = false;               // draw the tree indentation and expander
    bool hasChildren = false;
    bool expanded = false;
    int indent = 0;

    // Bar
    double fraction = 0.0;           // share of the parent
    double absolute = 0.0;           // share of the whole scan
    bool busy = false;               // still reading: show an indeterminate bar
    int barIndent = 0;               // pixels to indent the bar (tree depth)

    // Bar and Swatch
    COLORREF color = 0;
};

struct GlassListColumn
{
    int left = 0, right = 0;
    bool rightAligned = false;
    std::wstring title;
    int sort = 0;                    // 0 none, 1 ascending, 2 descending
};

struct GlassListRow
{
    int top = 0, bottom = 0;
    bool selected = false, hot = false, striped = false;
    std::vector<GlassCell> cells;    // one per column
};

struct GlassScrollBar
{
    bool visible = false;
    RECT track{};
    RECT thumb{};
};

struct GlassListView
{
    RECT card{};                     // rounded pane the list lives in
    RECT list{};                     // list window
    RECT client{};                   // list client area (header and rows)
    RECT header{};
    int cardRadius = 22;
    std::vector<GlassListColumn> columns;
    std::vector<GlassListRow> rows;

    int fontPx = 14;
    int iconPx = 16;
    int rowHeight = 24;
    int indentStep = 21;
    int generalIndent = 0;

    COLORREF text = 0, highlight = 0, highlightText = 0;
    bool dark = false;
    bool focused = false;

    GlassScrollBar vscroll, hscroll;
    size_t hash = 0;
};
