// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"
#include "GlassList.h"

// CMenuBar - the main window's menu bar living inside the client area (the native, non-client bar is detached).
// It owns the menu handle, lays the top-level items out, tracks hover / clicks and opens the real popup menus, so
// every command, state update and accelerator keeps working. The GPU compositor paints it (GlassOverlay); when the
// compositor is unavailable it paints plain text itself.
class CMenuBar final : public MessageTarget<CMenuBar, CWnd>
{
public:
    CMenuBar() { Live().push_back(this); }
    ~CMenuBar() override
    {
        std::erase(Live(), this);
        if (m_menu != nullptr) DestroyMenu(m_menu);
    }
    static std::vector<CMenuBar*>& Live() { static std::vector<CMenuBar*> live; return live; }

    bool Create(CWnd* parent);
    void Attach(HMENU menu);                 // takes ownership of the menu
    HMENU Menu() const { return m_menu; }

    CSize PreferredSize() const override;
    bool OpenByMnemonic(wchar_t ch);         // Alt + letter
    void OpenFirst();                        // Alt alone / F10
    bool BuildGlassBar(HWND frame, GlassMenuBar& out);

    static std::span<const RouteEntry> Routes();

protected:
    void OnPaint();
    bool OnEraseBkgnd(CDC*) { return true; }
    void OnSize(UINT, int, int) { m_layoutValid = false; Invalidate(false); }
    void OnMouseMove(UINT nFlags, CPoint point);
    void OnLButtonDown(UINT nFlags, CPoint point);
    LRESULT OnMouseLeave(WPARAM, LPARAM);

private:
    struct Item
    {
        std::wstring label;       // without the mnemonic marker
        wchar_t mnemonic = 0;     // upper case, 0 if none
        CRect rect;               // client coordinates
    };

    void Refresh();               // re-read labels and lay the items out
    int ItemAt(CPoint point) const;
    void Popup(int index);
    static LRESULT CALLBACK MenuFilter(int code, WPARAM wParam, LPARAM lParam);

    HMENU m_menu = nullptr;
    std::vector<Item> m_items;
    bool m_layoutValid = false;
    int m_hot = -1;
    int m_open = -1;
    int m_pending = -1;           // item to open after the current popup closed
    HHOOK m_hook = nullptr;
    static inline CMenuBar* s_tracking = nullptr;
};

inline std::span<const RouteEntry> CMenuBar::Routes()
{
    static constexpr std::array entries
    {
        Route::Window<&OnPaint>(WM_PAINT),
        Route::Window<&OnEraseBkgnd>(WM_ERASEBKGND),
        Route::Window<&OnSize>(WM_SIZE),
        Route::Window<&OnMouseMove>(WM_MOUSEMOVE),
        Route::Window<&OnLButtonDown>(WM_LBUTTONDOWN),
        Route::Window<&OnMouseLeave>(WM_MOUSELEAVE),
    };
    return entries;
}
