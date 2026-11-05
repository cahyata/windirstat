// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

// GlassUi: a small retained-mode UI toolkit drawn entirely with Direct2D / DirectWrite on a
// DirectComposition swap chain with premultiplied alpha. The window sits on a real DWM acrylic
// backdrop, so every widget can be truly translucent (no GDI controls involved).
namespace GlassUi
{
    // Runs the "Select Drives" screen standalone (no owner window) in its own message loop.
    int RunDemo();

    // Modal "Select Drives" replacement. Returns false when the GPU UI is unavailable (the caller should
    // fall back to the classic dialog). When it returns true, `cancelled` tells whether the user dismissed it;
    // otherwise `path` holds the scan target specification and the scan options were saved to COptions.
    bool SelectScanRoots(HWND owner, std::wstring& path, bool& cancelled);

    struct MessageButton
    {
        int id;
        std::wstring text;
        bool isDefault;
    };

    // Modal message box. iconKind: 0 information, 1 question, 2 warning, 3 error. Returns false when the GPU UI
    // is unavailable (the caller should fall back to the classic dialog).
    bool ShowMessage(HWND owner, const std::wstring& title, const std::wstring& message, int iconKind,
        const std::vector<MessageButton>& buttons, const std::vector<std::wstring>& items,
        const std::wstring& checkboxText, bool& checkboxValue, int& resultId);

    // Callbacks into the classic property sheet that AttachSheet mirrors.
    struct SheetHooks
    {
        HWND sheet = nullptr;
        std::wstring caption;
        int pageCount = 0;
        std::function<std::wstring(int)> title;
        std::function<bool(int)> select;
        std::function<int()> active;
        std::function<HWND(int)> page;
    };

    // Turns the (now hidden) property sheet into a glass window mirroring its pages. Returns false if unavailable.
    bool AttachSheet(const SheetHooks& hooks);

    // Turns a (now hidden) classic single-page dialog into a glass window mirroring its controls. The dialog keeps
    // running all of its logic; the glass buttons click the real ones. Returns false if the GPU UI is unavailable.
    bool AttachDialog(HWND dialog, const std::wstring& caption);
}
