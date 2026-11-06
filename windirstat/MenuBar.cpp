// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "MenuBar.h"
#include "GlassOverlay.h"
#include "GlassUi.h"

bool CMenuBar::Create(CWnd* parent)
{
    static ATOM s_atom = 0;
    if (s_atom == 0)
    {
        const WNDCLASSEX wc = {
            .cbSize        = sizeof(WNDCLASSEX),
            .lpfnWndProc   = ::DefWindowProc,
            .hInstance     = GetAppInstance(),
            .hCursor       = LoadCursor(nullptr, IDC_ARROW),
            .hbrBackground = nullptr,
            .lpszClassName = L"WdsMenuBar",
        };
        s_atom = ::RegisterClassEx(&wc);
        if (s_atom == 0) return false;
    }
    return CreateEx(0, L"WdsMenuBar", nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, CRect(0, 0, 0, 0), parent, 0x7F10);
}

void CMenuBar::Attach(HMENU menu)
{
    if (m_menu != nullptr && m_menu != menu) DestroyMenu(m_menu);
    m_menu = menu;
    m_layoutValid = false;
    if (IsWindow(m_hWnd)) Invalidate(false);
}

CSize CMenuBar::PreferredSize() const
{
    const CRect rcParent = GetParent().GetClientRect();
    return CSize(rcParent.Width(), ::ScaleForDpi(30, m_hWnd));
}

// Re-reads the labels (they change with the language) and lays the top-level items out left to right
void CMenuBar::Refresh()
{
    std::vector<Item> items;
    if (m_menu != nullptr)
    {
        const int count = GetMenuItemCount(m_menu);
        const HDC dc = GetDC(m_hWnd);
        const HGDIOBJ old = SelectObject(dc, GetAppFont(m_hWnd));
        const int padX = ::ScaleForDpi(12, m_hWnd);
        const CRect client = GetClientRect();
        int x = ::ScaleForDpi(4, m_hWnd);
        for (int i = 0; i < count; ++i)
        {
            std::array<WCHAR, 128> text{};
            GetMenuStringW(m_menu, static_cast<UINT>(i), text.data(), static_cast<int>(text.size()), MF_BYPOSITION);
            Item item;
            for (size_t c = 0; text[c] != L'\0'; ++c)
            {
                if (text[c] == L'&' && text[c + 1] == L'&') { item.label.push_back(L'&'); ++c; }
                else if (text[c] == L'&' && text[c + 1] != L'\0') { if (item.mnemonic == 0) item.mnemonic = static_cast<wchar_t>(towupper(text[c + 1])); }
                else item.label.push_back(text[c]);
            }
            SIZE extent{};
            GetTextExtentPoint32W(dc, item.label.c_str(), static_cast<int>(item.label.size()), &extent);
            item.rect = CRect(x, client.top + ::ScaleForDpi(3, m_hWnd), x + extent.cx + 2 * padX, client.bottom - ::ScaleForDpi(3, m_hWnd));
            x = item.rect.right;
            items.push_back(std::move(item));
        }
        SelectObject(dc, old);
        ReleaseDC(m_hWnd, dc);
    }
    m_items = std::move(items);
    m_layoutValid = true;
}

int CMenuBar::ItemAt(const CPoint point) const
{
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].rect.Contains(point)) return static_cast<int>(i);
    return -1;
}

void CMenuBar::OnPaint()
{
    CPaintDC dc(this);
    GlassTheme::PaintBackdrop(dc.Handle(), m_hWnd);
    if (GlassOverlay::IsActive()) return; // the compositor draws the labels and hover lens
    Refresh();
    GdiObjectSelection font(&dc, GetAppFont(m_hWnd));
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(DarkMode::SystemColor(COLOR_MENUTEXT));
    for (const auto& item : m_items)
    {
        CRect rc = item.rect;
        dc.DrawText(item.label.c_str(), static_cast<int>(item.label.size()), &rc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }
}

void CMenuBar::OnMouseMove(UINT, const CPoint point)
{
    if (!m_layoutValid) Refresh();
    if (const int hit = ItemAt(point); hit != m_hot) { m_hot = hit; Invalidate(false); }
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hWnd, 0 };
    TrackMouseEvent(&tme);
}

LRESULT CMenuBar::OnMouseLeave(WPARAM, LPARAM)
{
    if (m_hot != -1) { m_hot = -1; Invalidate(false); }
    return 0;
}

void CMenuBar::OnLButtonDown(UINT, const CPoint point)
{
    if (!m_layoutValid) Refresh();
    if (const int hit = ItemAt(point); hit >= 0) Popup(hit);
}

bool CMenuBar::OpenByMnemonic(const wchar_t ch)
{
    Refresh();
    const wchar_t key = static_cast<wchar_t>(towupper(ch));
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].mnemonic == key) { Popup(static_cast<int>(i)); return true; }
    return false;
}

void CMenuBar::OpenFirst()
{
    Refresh();
    if (!m_items.empty()) Popup(0);
}

// Opens the popup under item `index`; hovering or pressing Left / Right over another item swaps it while open
void CMenuBar::Popup(int index)
{
    if (m_menu == nullptr || s_tracking != nullptr) return;
    const HWND owner = ::GetParent(m_hWnd);
    s_tracking = this;
    while (index >= 0 && index < static_cast<int>(m_items.size()))
    {
        const HMENU sub = GetSubMenu(m_menu, index);
        if (sub == nullptr) break;
        m_open = index;
        Invalidate(false);
        POINT pt{ m_items[index].rect.left, m_items[index].rect.bottom };
        ::ClientToScreen(m_hWnd, &pt);
        m_pending = -1;
        int request = GlassUi::kSwitchNone;
        const int count = static_cast<int>(m_items.size());
        if (GlassUi::TrackMenu(owner, sub, pt, index, [this](POINT p) { ::ScreenToClient(m_hWnd, &p); return ItemAt(p); }, request))
        {
            // Glass popup: translate the switch request into the next item to open
            if (request == GlassUi::kSwitchNext) m_pending = (index + 1) % std::max(count, 1);
            else if (request == GlassUi::kSwitchPrev) m_pending = (index + count - 1) % std::max(count, 1);
            else if (request >= 0) m_pending = request;
        }
        else
        {
            // Classic popup fallback
            m_hook = SetWindowsHookExW(WH_MSGFILTER, MenuFilter, nullptr, GetCurrentThreadId());
            TrackPopupMenu(sub, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, owner, nullptr);
            if (m_hook != nullptr) { UnhookWindowsHookEx(m_hook); m_hook = nullptr; }
        }
        index = m_pending;
    }
    s_tracking = nullptr;
    m_open = -1;
    m_pending = -1;
    Invalidate(false);
}

LRESULT CALLBACK CMenuBar::MenuFilter(const int code, const WPARAM wParam, const LPARAM lParam)
{
    CMenuBar* bar = s_tracking;
    if (code == MSGF_MENU && bar != nullptr && bar->m_open >= 0)
    {
        const MSG* msg = reinterpret_cast<const MSG*>(lParam);
        int target = -1;
        if (msg->message == WM_MOUSEMOVE)
        {
            POINT p = msg->pt;
            ::ScreenToClient(bar->m_hWnd, &p);
            if (const int hit = bar->ItemAt(p); hit >= 0 && hit != bar->m_open) target = hit;
        }
        else if (msg->message == WM_KEYDOWN && (msg->wParam == VK_LEFT || msg->wParam == VK_RIGHT))
        {
            // A highlighted item that opens a nested popup keeps the default handling
            const HMENU sub = GetSubMenu(bar->m_menu, bar->m_open);
            bool nested = false;
            for (int i = 0; sub != nullptr && i < GetMenuItemCount(sub); ++i)
                if (const UINT state = GetMenuState(sub, static_cast<UINT>(i), MF_BYPOSITION); (state & MF_HILITE) && (state & MF_POPUP)) nested = true;
            if (!nested)
            {
                const int count = static_cast<int>(bar->m_items.size());
                target = (bar->m_open + (msg->wParam == VK_RIGHT ? 1 : count - 1)) % std::max(count, 1);
            }
        }
        if (target >= 0)
        {
            bar->m_pending = target;
            EndMenu();
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool CMenuBar::BuildGlassBar(const HWND frame, GlassMenuBar& out)
{
    if (!IsWindow(m_hWnd) || !::IsWindowVisible(m_hWnd) || !::IsChild(frame, m_hWnd)) return false;
    Refresh();
    RECT origin{ 0, 0, 0, 0 };
    const CRect client = GetClientRect();
    out.bar = client;
    ::MapWindowPoints(m_hWnd, frame, reinterpret_cast<POINT*>(&out.bar), 2);
    ::MapWindowPoints(m_hWnd, frame, reinterpret_cast<POINT*>(&origin), 1);
    LOGFONT font{};
    ::GetObjectW(GetAppFont(m_hWnd), sizeof(font), &font);
    out.fontPx = std::max(8, std::abs(static_cast<int>(font.lfHeight)));
    out.text = DarkMode::SystemColor(COLOR_MENUTEXT);
    out.dark = DarkMode::IsDarkModeActive();
    size_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const size_t v) { hash = (hash ^ v) * 1099511628211ull; };
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        GlassMenuItem item;
        item.rect = m_items[i].rect;
        item.rect.left += origin.left; item.rect.right += origin.left;
        item.rect.top += origin.top; item.rect.bottom += origin.top;
        item.label = m_items[i].label;
        item.hot = static_cast<int>(i) == m_hot;
        item.open = static_cast<int>(i) == m_open;
        mix(static_cast<size_t>(item.rect.left)); mix(static_cast<size_t>(item.rect.right)); mix(static_cast<size_t>(item.rect.top));
        mix(static_cast<size_t>(item.hot) | static_cast<size_t>(item.open) << 1);
        mix(std::hash<std::wstring>{}(item.label));
        out.items.push_back(std::move(item));
    }
    mix(static_cast<size_t>(out.dark));
    out.hash = hash;
    return !out.items.empty();
}
