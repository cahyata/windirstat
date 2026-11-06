// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "GlassOverlay.h"

// -----------------------------------------------------------------------------
//  "Liquid glass" panel drawing (frosted gradient, specular rim, soft border)
// -----------------------------------------------------------------------------
namespace
{
    void DrawGlassPanel(HDC hdc, const CRect& rect, const COLORREF base, const int radius, const bool dark)
    {
        if (rect.Width() <= 2 * radius || rect.Height() <= 2) return;

        using namespace Gdiplus;
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        const REAL x = static_cast<REAL>(rect.left), y = static_cast<REAL>(rect.top);
        const REAL w = static_cast<REAL>(rect.Width() - 1), h = static_cast<REAL>(rect.Height() - 1);
        const REAL d = static_cast<REAL>(std::min<int>(radius, rect.Height() / 2)) * 2.0f;

        GraphicsPath path;
        path.AddArc(x, y, d, d, 180, 90);
        path.AddArc(x + w - d, y, d, d, 270, 90);
        path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        path.AddArc(x, y + h - d, d, d, 90, 90);
        path.CloseFigure();

        // Body: translucent white wash fading downwards, with a cool tint at the bottom
        const Color top = dark ? Color(64, 255, 255, 255) : Color(235, 255, 255, 255);
        const Color bottom = dark ? Color(14, 200, 205, 215) : Color(60, 205, 210, 222);
        const LinearGradientBrush body(PointF(x, y), PointF(x, y + h + 1), top, bottom);
        g.FillPath(&body, &path);

        // Soft outer rim and darker lower edge for depth
        const Pen outline(dark ? Color(90, 0, 0, 0) : Color(70, 70, 82, 110), 2.2f);
        g.DrawPath(&outline, &path);
        const Pen rim(dark ? Color(120, 255, 255, 255) : Color(235, 255, 255, 255), 1.0f);
        g.DrawPath(&rim, &path);
        const Pen shade(dark ? Color(60, 0, 0, 0) : Color(40, 40, 60, 90), 1.0f);
        g.DrawLine(&shade, x + d / 2, y + h, x + w - d / 2, y + h);

        // Specular highlight just inside the top edge
        const Pen spec(Color(dark ? 120 : 255, 255, 255, 255), 1.0f);
        g.DrawLine(&spec, x + d / 2 + 2, y + 1.5f, x + w - d / 2 - 2, y + 1.5f);
        (void)base;
    }

    void RoundedPath(Gdiplus::GraphicsPath& path, const CRect& rect, const int radius)
    {
        const Gdiplus::REAL x = static_cast<Gdiplus::REAL>(rect.left), y = static_cast<Gdiplus::REAL>(rect.top);
        const Gdiplus::REAL w = static_cast<Gdiplus::REAL>(rect.Width() - 1);
        const Gdiplus::REAL h = static_cast<Gdiplus::REAL>(rect.Height() - 1);
        const Gdiplus::REAL d = static_cast<Gdiplus::REAL>(std::min<int>(radius, rect.Height() / 2)) * 2.0f;
        path.AddArc(x, y, d, d, 180, 90);
        path.AddArc(x + w - d, y, d, d, 270, 90);
        path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        path.AddArc(x, y + h - d, d, d, 90, 90);
        path.CloseFigure();
    }

    void AddBlob(Gdiplus::Graphics& g, const float cx, const float cy, const float rx, const float ry,
        const Gdiplus::Color color)
    {
        Gdiplus::GraphicsPath path;
        path.AddEllipse(cx - rx, cy - ry, rx * 2, ry * 2);
        Gdiplus::PathGradientBrush brush(&path);
        brush.SetCenterColor(color);
        const Gdiplus::Color edge(0, color.GetR(), color.GetG(), color.GetB());
        int count = 1;
        brush.SetSurroundColors(&edge, &count);
        g.FillPath(&brush, &path);
    }

    // Soft shadow and bright rim around a content card
    void DrawCardFrame(HDC hdc, const CRect& rect, const bool dark)
    {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (int i = 9; i >= 1; --i)
        {
            CRect r = rect;
            r.Inflate(i, i);
            r.top += 2;
            r.bottom += 2;
            Gdiplus::GraphicsPath path;
            RoundedPath(path, r, GlassTheme::CardRadius + i);
            const Gdiplus::Pen pen(Gdiplus::Color(static_cast<BYTE>((dark ? 34 : 18) - i * 2), 20, 30, 70), 1.0f);
            g.DrawPath(&pen, &path);
        }
        CRect r = rect;
        r.Inflate(1, 1);
        Gdiplus::GraphicsPath path;
        RoundedPath(path, r, GlassTheme::CardRadius + 1);
        const Gdiplus::Pen rim(dark ? Gdiplus::Color(120, 255, 255, 255) : Gdiplus::Color(235, 255, 255, 255), 1.6f);
        g.DrawPath(&rim, &path);
    }
}

// -----------------------------------------------------------------------------
//  Aurora backdrop: soft colour blobs, drawn once at low resolution and stretched
// -----------------------------------------------------------------------------
void GlassTheme::PaintBackdrop(HDC hdc, HWND window)
{
    RECT rc{};
    if (GetClientRect(window, &rc)) PaintBackdropAt(hdc, window, rc);
}

void GlassTheme::FillWash(HDC hdc, const RECT& rect, const COLORREF color, const int alpha)
{
    Gdiplus::Graphics g(hdc);
    const Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)),
        GetRValue(color), GetGValue(color), GetBValue(color)));
    g.FillRectangle(&brush, static_cast<int>(rect.left), static_cast<int>(rect.top),
        static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top));
}

void GlassTheme::FillRounded(HDC hdc, const RECT& rect, const int radius, const COLORREF color, const int alpha)
{
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return;
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    RoundedPath(path, CRect(rect), std::min(radius, h / 2));
    const Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)),
        GetRValue(color), GetGValue(color), GetBValue(color)));
    g.FillPath(&brush, &path);
}

void GlassTheme::PaintBackdropAt(HDC hdc, HWND window, const RECT& area)
{
    const HWND root = GetAncestor(window, GA_ROOT);
    RECT rootRc{}, rc = area;
    if (root == nullptr || !GetClientRect(root, &rootRc)) return;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || rootRc.right <= 0 || rootRc.bottom <= 0) return;
    MapWindowPoints(window, root, reinterpret_cast<POINT*>(&rc), 2);
    rc.top = std::clamp<LONG>(rc.top, 0, rootRc.bottom - 1);
    rc.left = std::clamp<LONG>(rc.left, 0, rootRc.right - 1);

    constexpr int kW = 192, kH = 108;
    static std::unique_ptr<Gdiplus::Bitmap> s_bitmap;
    static int s_dark = -1;
    const bool dark = DarkMode::IsDarkModeActive();
    if (s_bitmap == nullptr || s_dark != static_cast<int>(dark))
    {
        s_dark = dark;
        s_bitmap = std::make_unique<Gdiplus::Bitmap>(kW, kH, PixelFormat32bppARGB);
        Gdiplus::Graphics bg(s_bitmap.get());
        bg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        using C = Gdiplus::Color;
        // Neutral frosted palette: soft light/dark gradient with gentle highlights, no hue
        const Gdiplus::LinearGradientBrush base(Gdiplus::PointF(0, 0),
            Gdiplus::PointF(static_cast<float>(kW), static_cast<float>(kH)),
            dark ? C(255, 20, 22, 28) : C(255, 233, 233, 238), dark ? C(255, 32, 34, 42) : C(255, 221, 221, 228));
        bg.FillRectangle(&base, 0, 0, kW, kH);
        if (dark)
        {
            AddBlob(bg, 30, 20, 85, 60, C(120, 78, 84, 100));
            AddBlob(bg, 150, 30, 80, 55, C(100, 64, 68, 84));
            AddBlob(bg, 100, 95, 95, 55, C(110, 86, 92, 108));
            AddBlob(bg, 185, 100, 60, 45, C(90, 58, 62, 78));
        }
        else
        {
            AddBlob(bg, 25, 15, 85, 60, C(210, 255, 255, 255));
            AddBlob(bg, 150, 25, 80, 55, C(120, 196, 203, 218));
            AddBlob(bg, 95, 100, 95, 55, C(170, 255, 255, 255));
            AddBlob(bg, 185, 95, 60, 45, C(110, 190, 198, 212));
        }
    }

    Gdiplus::Graphics g(hdc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    const float sx = static_cast<float>(kW) / static_cast<float>(rootRc.right);
    const float sy = static_cast<float>(kH) / static_cast<float>(rootRc.bottom);
    g.DrawImage(s_bitmap.get(), Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)), static_cast<float>(rc.left) * sx,
        static_cast<float>(rc.top) * sy, static_cast<float>(w) * sx, static_cast<float>(h) * sy,
        Gdiplus::UnitPixel, &attributes);
}

void GlassTheme::ApplyRoundedRegion(HWND window, const int radius)
{
    RECT rc{};
    if (!GetClientRect(window, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    if (HRGN region = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, radius * 2, radius * 2);
        region != nullptr && SetWindowRgn(window, region, TRUE) == 0) DeleteObject(region);
}

// -----------------------------------------------------------------------------
//  Glass column header
// -----------------------------------------------------------------------------
void GlassTheme::DrawHeaderItem(HDC hdc, const HWND header, const int index, const RECT& rect)
{
    const bool dark = DarkMode::IsDarkModeActive();
    std::array<WCHAR, 128> text{};
    HDITEMW item{};
    item.mask = HDI_TEXT | HDI_FORMAT;
    item.pszText = text.data();
    item.cchTextMax = static_cast<int>(text.size());
    if (!SendMessageW(header, HDM_GETITEMW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item))) return;

    // The sort chevron gets its own slot beside the text so it never overlaps the label
    const bool sorted = (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0;
    const bool rightAligned = (item.fmt & HDF_RIGHT) != 0;
    const float scale = static_cast<float>(GetWindowDpi(header)) / 96.0f;
    const int slot = sorted ? static_cast<int>(18.0f * scale) : 0;

    const auto font = reinterpret_cast<HFONT>(SendMessageW(header, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = font != nullptr ? SelectObject(hdc, font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, dark ? RGB(176, 182, 196) : RGB(98, 104, 118));
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
    flags |= rightAligned ? DT_RIGHT : (item.fmt & HDF_CENTER) ? DT_CENTER : DT_LEFT;
    RECT textRect = rect;
    textRect.left += 8;
    textRect.right -= 8;
    if (sorted) { if (rightAligned) textRect.left += slot; else textRect.right -= slot; }
    DrawTextW(hdc, text.data(), -1, &textRect, flags);
    if (oldFont != nullptr) SelectObject(hdc, oldFont);

    if (sorted)
    {
        const float cy = static_cast<float>(rect.top + rect.bottom) * 0.5f;
        const float cx = rightAligned ? static_cast<float>(rect.left) + 13.0f * scale : static_cast<float>(rect.right) - 13.0f * scale;
        const float half = 4.0f * scale, rise = 2.2f * scale;
        const bool up = (item.fmt & HDF_SORTUP) != 0;
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Gdiplus::Color(220, dark ? 235 : 60, dark ? 238 : 66, dark ? 245 : 78), 1.5f * scale);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        const float tip = up ? cy - rise : cy + rise, base = up ? cy + rise : cy - rise;
        const Gdiplus::PointF points[3] = { { cx - half, base }, { cx, tip }, { cx + half, base } };
        g.DrawLines(&pen, points, 3);
    }
}

// -----------------------------------------------------------------------------
//  Main frame: caption colours, glass menu bar (undocumented UAH messages)
// -----------------------------------------------------------------------------
namespace
{
    constexpr UINT WM_UAHDRAWMENU = 0x0091;
    constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;

    struct UahMenu { HMENU hmenu; HDC hdc; DWORD flags; };
    struct UahDrawMenuItem { DRAWITEMSTRUCT dis; UahMenu um; int position; };

    bool GetMenuBarRectInWindow(const HWND frame, RECT& bar)
    {
        MENUBARINFO info{ .cbSize = sizeof(info) };
        if (!GetMenuBarInfo(frame, OBJID_MENU, 0, &info)) return false;
        RECT window{};
        GetWindowRect(frame, &window);
        bar = info.rcBar;
        OffsetRect(&bar, -window.left, -window.top);
        return true;
    }

    void PaintMenuBackdrop(const HDC hdc, const HWND frame, const RECT& dest)
    {
        const int w = dest.right - dest.left, h = dest.bottom - dest.top;
        if (w <= 0 || h <= 0) return;
        const int saved = SaveDC(hdc);
        SetViewportOrgEx(hdc, dest.left, dest.top, nullptr);
        GlassTheme::PaintBackdropAt(hdc, frame, RECT{ dest.left, 0, dest.right, h });
        RestoreDC(hdc, saved);
    }

    LRESULT CALLBACK MainFrameSubclass(const HWND frame, const UINT message, const WPARAM wParam,
        const LPARAM lParam, const UINT_PTR id, DWORD_PTR)
    {
        const bool dark = DarkMode::IsDarkModeActive();
        if (message == WM_UAHDRAWMENU)
        {
            const auto* menu = reinterpret_cast<const UahMenu*>(lParam);
            if (RECT bar{}; GetMenuBarRectInWindow(frame, bar)) PaintMenuBackdrop(menu->hdc, frame, bar);
            return TRUE;
        }
        if (message == WM_UAHDRAWMENUITEM)
        {
            const auto* item = reinterpret_cast<const UahDrawMenuItem*>(lParam);
            const HDC hdc = item->um.hdc;
            const RECT rect = item->dis.rcItem;
            PaintMenuBackdrop(hdc, frame, rect);
            const bool hot = (item->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
            if (hot)
            {
                Gdiplus::Graphics g(hdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const float x = static_cast<float>(rect.left) + 1.5f, y = static_cast<float>(rect.top) + 2.5f;
                const float w = static_cast<float>(rect.right - rect.left) - 3.0f;
                const float h = static_cast<float>(rect.bottom - rect.top) - 5.0f;
                Gdiplus::GraphicsPath pill;
                pill.AddArc(x, y, h, h, 90, 180);
                pill.AddArc(x + w - h, y, h, h, 270, 180);
                pill.CloseFigure();
                const Gdiplus::LinearGradientBrush body(Gdiplus::PointF(0, y), Gdiplus::PointF(0, y + h),
                    dark ? Gdiplus::Color(90, 255, 255, 255) : Gdiplus::Color(235, 255, 255, 255),
                    dark ? Gdiplus::Color(30, 255, 255, 255) : Gdiplus::Color(120, 255, 255, 255));
                g.FillPath(&body, &pill);
                const Gdiplus::Pen rim(Gdiplus::Color(dark ? 120 : 230, 255, 255, 255), 1.0f);
                g.DrawPath(&rim, &pill);
            }
            std::array<WCHAR, 64> text{};
            MENUITEMINFOW mii{ .cbSize = sizeof(mii), .fMask = MIIM_STRING };
            mii.dwTypeData = text.data();
            mii.cch = static_cast<UINT>(text.size() - 1);
            GetMenuItemInfoW(item->um.hmenu, static_cast<UINT>(item->position), TRUE, &mii);
            NONCLIENTMETRICSW metrics{ .cbSize = sizeof(metrics) };
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
            const HFONT font = CreateFontIndirectW(&metrics.lfMenuFont);
            const HGDIOBJ old = SelectObject(hdc, font);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, dark ? RGB(236, 238, 244) : RGB(34, 38, 48));
            RECT textRect = rect;
            DrawTextW(hdc, text.data(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_HIDEPREFIX);
            SelectObject(hdc, old);
            DeleteObject(font);
            return TRUE;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(frame, MainFrameSubclass, id);

        const LRESULT result = DefSubclassProc(frame, message, wParam, lParam);
        if (message == WM_NCPAINT || message == WM_NCACTIVATE || message == WM_ACTIVATE)
        {
            // Cover the 1px line Windows draws below the menu bar
            if (RECT bar{}; GetMenuBarRectInWindow(frame, bar))
            {
                const HDC hdc = GetWindowDC(frame);
                PaintMenuBackdrop(hdc, frame, RECT{ bar.left, bar.bottom, bar.right, bar.bottom + 1 });
                ReleaseDC(frame, hdc);
            }
        }
        return result;
    }
}

void GlassTheme::StyleMainFrame(const HWND frame)
{
    const bool dark = DarkMode::IsDarkModeActive();
    // Title bar takes the colour of the glass backdrop so caption and content read as one surface
    const COLORREF caption = dark ? RGB(21, 23, 29) : RGB(236, 238, 243);
    const COLORREF text = dark ? RGB(236, 238, 244) : RGB(34, 38, 48);
    DwmSetWindowAttribute(frame, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    DwmSetWindowAttribute(frame, 36 /*DWMWA_TEXT_COLOR*/, &text, sizeof(text));
    DwmSetWindowAttribute(frame, 34 /*DWMWA_BORDER_COLOR*/, &caption, sizeof(caption));
    SetWindowSubclass(frame, MainFrameSubclass, 0x6C70, 0);
    if (!dark)
        EnumChildWindows(frame, [](const HWND window, LPARAM) -> BOOL
        {
            if ((GetWindowLongPtrW(window, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)) != 0)
                SetWindowTheme(window, L"Explorer", nullptr);
            return TRUE;
        }, 0);
}

// -----------------------------------------------------------------------------
//  Dialog styling: glass push buttons, uniform button rows and the GPU overlay
// -----------------------------------------------------------------------------
namespace
{
    bool IsGlassPushButton(const HWND window)
    {
        std::array<WCHAR, 16> className{};
        GetClassNameW(window, className.data(), static_cast<int>(className.size()));
        if (_wcsicmp(className.data(), L"Button") != 0) return false;
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const LONG_PTR type = style & BS_TYPEMASK;
        return (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) && (style & (BS_ICON | BS_BITMAP)) == 0;
    }

    void PaintGlassButton(const HWND window, const HDC target)
    {
        CRect rc;
        GetClientRect(window, &rc);
        const int width = rc.Width(), height = rc.Height();
        if (width <= 4 || height <= 4) return;

        const HDC mem = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        const HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
        GlassTheme::PaintBackdrop(mem, window);

        const bool dark = DarkMode::IsDarkModeActive();
        const LRESULT state = SendMessageW(window, BM_GETSTATE, 0, 0);
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const bool pressed = enabled && (state & BST_PUSHED) != 0;
        const bool hot = enabled && (state & BST_HOT) != 0;
        const bool focused = enabled && GetFocus() == window;
        const bool primary = (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON;

        {
            using namespace Gdiplus;
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            const REAL x = 1.5f, y = 1.5f + (pressed ? 1.0f : 0.0f);
            const REAL w = static_cast<REAL>(width) - 3.0f, h = static_cast<REAL>(height) - 3.0f;
            const REAL d = h;
            GraphicsPath pill;
            pill.AddArc(x, y, d, d, 90, 180);
            pill.AddArc(x + w - d, y, d, d, 270, 180);
            pill.CloseFigure();

            // Soft drop shadow under the pill
            for (int i = 3; i >= 1; --i)
            {
                GraphicsPath shadow;
                shadow.AddArc(x - i, y - i + 2, d + 2 * i, d + 2 * i, 90, 180);
                shadow.AddArc(x + w - d - i, y - i + 2, d + 2 * i, d + 2 * i, 270, 180);
                shadow.CloseFigure();
                const Pen pen(Color(static_cast<BYTE>(enabled ? 16 - i * 3 : 4), 20, 30, 70), 1.0f);
                g.DrawPath(&pen, &shadow);
            }

            Color top, bottom;
            if (primary && enabled)
            {
                // Prominent graphite glass instead of a coloured accent
                top = Color(255, hot ? 92 : 64, hot ? 176 : 158, 255);
                bottom = Color(255, 0, hot ? 128 : 114, hot ? 242 : 230);
                if (pressed) { top = Color(255, 20, 118, 235); bottom = Color(255, 0, 86, 204); }
            }
            else if (dark)
            {
                top = Color(static_cast<BYTE>(enabled ? (pressed ? 40 : hot ? 110 : 80) : 28), 255, 255, 255);
                bottom = Color(static_cast<BYTE>(enabled ? (pressed ? 18 : hot ? 56 : 34) : 12), 200, 220, 255);
            }
            else
            {
                top = Color(static_cast<BYTE>(enabled ? (pressed ? 170 : hot ? 255 : 235) : 120), 255, 255, 255);
                bottom = Color(static_cast<BYTE>(enabled ? (pressed ? 120 : hot ? 190 : 150) : 70), 220, 232, 255);
            }
            const LinearGradientBrush body(PointF(0, y), PointF(0, y + h), top, bottom);
            g.FillPath(&body, &pill);

            const Pen outline(Color(static_cast<BYTE>(enabled ? (dark ? 90 : 60) : 25), dark ? 0 : 60, dark ? 0 : 70, dark ? 0 : 100), 2.2f);
            g.DrawPath(&outline, &pill);
            const Pen rim(Color(static_cast<BYTE>(enabled ? (dark ? 140 : 235) : 80), 255, 255, 255), 1.1f);
            g.DrawPath(&rim, &pill);
            if (enabled)
            {
                const Pen spec(Color(primary ? 150 : 230, 255, 255, 255), 1.0f);
                g.DrawLine(&spec, x + d * 0.55f, y + 1.6f, x + w - d * 0.55f, y + 1.6f);
            }
            if (focused)
            {
                GraphicsPath ring;
                ring.AddArc(x - 1.0f, y - 1.0f, d + 2.0f, d + 2.0f, 90, 180);
                ring.AddArc(x + w - d - 1.0f, y - 1.0f, d + 2.0f, d + 2.0f, 270, 180);
                ring.CloseFigure();
                const Pen focus(Color(230, 255, 255, 255), 1.6f);
                g.DrawPath(&focus, &ring);
            }
        }

        // Label
        std::array<WCHAR, 256> text{};
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        const HGDIOBJ oldFont = font != nullptr ? SelectObject(mem, font) : nullptr;
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, !enabled ? DarkMode::SystemColor(COLOR_GRAYTEXT) : primary ? RGB(255, 255, 255) :
            DarkMode::SystemColor(COLOR_BTNTEXT));
        CRect textRect = rc;
        textRect.Deflate(10, 0, 10, pressed ? 0 : 1);
        if (pressed) { textRect.top += 1; textRect.bottom += 1; }
        DrawTextW(mem, text.data(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_HIDEPREFIX);
        if (oldFont != nullptr) SelectObject(mem, oldFont);

        BitBlt(target, 0, 0, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(mem);
    }

    LRESULT CALLBACK GlassButtonSubclass(const HWND window, const UINT message, const WPARAM wParam,
        const LPARAM lParam, const UINT_PTR id, DWORD_PTR)
    {
        switch (message)
        {
        case WM_NCDESTROY: RemoveWindowSubclass(window, GlassButtonSubclass, id); break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(window, &ps);
            PaintGlassButton(window, dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: PaintGlassButton(window, reinterpret_cast<HDC>(wParam)); return 0;
        default: break;
        }
        const bool mouse = message == WM_MOUSEMOVE || message == WM_MOUSELEAVE;
        const LRESULT before = mouse ? SendMessageW(window, BM_GETSTATE, 0, 0) : 0;
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == BM_SETSTATE || message == BM_SETSTYLE || message == WM_ENABLE || message == WM_SETFOCUS ||
            message == WM_KILLFOCUS || message == WM_SETTEXT || message == WM_SETFONT ||
            message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
            (mouse && before != SendMessageW(window, BM_GETSTATE, 0, 0)))
            InvalidateRect(window, nullptr, FALSE);
        return result;
    }

    // Icon labels are drawn over the backdrop with their own alpha instead of over a solid brush
    LRESULT CALLBACK GlassIconSubclass(const HWND window, const UINT message, const WPARAM wParam,
        const LPARAM lParam, const UINT_PTR id, DWORD_PTR)
    {
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, GlassIconSubclass, id);
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT)
        {
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(window, &ps);
            CRect rc;
            GetClientRect(window, &rc);
            const HDC mem = CreateCompatibleDC(dc);
            const HBITMAP bitmap = CreateCompatibleBitmap(dc, rc.Width(), rc.Height());
            const HGDIOBJ old = SelectObject(mem, bitmap);
            GlassTheme::PaintBackdrop(mem, window);
            if (const auto icon = reinterpret_cast<HICON>(SendMessageW(window, STM_GETICON, 0, 0)); icon != nullptr)
            {
                // GDI+ honours the icon's alpha channel, unlike DrawIconEx on icons without a proper mask
                Gdiplus::Bitmap image(icon);
                Gdiplus::Graphics g(mem);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                const int iw = static_cast<int>(image.GetWidth()), ih = static_cast<int>(image.GetHeight());
                const bool centered = (GetWindowLongPtrW(window, GWL_STYLE) & SS_CENTERIMAGE) != 0;
                g.DrawImage(&image, centered ? (rc.Width() - iw) / 2 : 0, centered ? (rc.Height() - ih) / 2 : 0, iw, ih);
            }
            BitBlt(dc, 0, 0, rc.Width(), rc.Height(), mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bitmap);
            DeleteDC(mem);
            EndPaint(window, &ps);
            return 0;
        }
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == STM_SETICON || message == STM_SETIMAGE) InvalidateRect(window, nullptr, FALSE);
        return result;
    }

    // ---- Glass check boxes, radio buttons and group boxes ----------------------------------------
    LONG_PTR ButtonType(const HWND window) { return GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK; }

    bool IsButtonClass(const HWND window)
    {
        std::array<WCHAR, 16> className{};
        GetClassNameW(window, className.data(), static_cast<int>(className.size()));
        return _wcsicmp(className.data(), L"Button") == 0;
    }

    bool IsGlassCheck(const HWND window)
    {
        if (!IsButtonClass(window)) return false;
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        if ((style & (BS_PUSHLIKE | BS_ICON | BS_BITMAP)) != 0) return false;
        const LONG_PTR type = style & BS_TYPEMASK;
        return type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    }

    void PaintGlassCheck(const HWND window, const HDC target)
    {
        CRect rc;
        GetClientRect(window, &rc);
        const int width = rc.Width(), height = rc.Height();
        if (width <= 4 || height <= 4) return;
        const HDC mem = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        const HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
        GlassTheme::PaintBackdrop(mem, window);

        const bool dark = DarkMode::IsDarkModeActive();
        const LONG_PTR type = ButtonType(window);
        const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
        const LRESULT state = SendMessageW(window, BM_GETSTATE, 0, 0);
        const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool enabled = IsWindowEnabled(window) != FALSE;
        const bool hot = enabled && (state & BST_HOT) != 0;
        const bool pressed = enabled && (state & BST_PUSHED) != 0;
        const bool focused = enabled && GetFocus() == window;

        const int box = ScaleForDpi(18, window);
        const int gap = ScaleForDpi(9, window);
        const float bx = 1.5f, by = static_cast<float>((height - box) / 2) + 0.5f, bs = static_cast<float>(box) - 3.0f;
        {
            using namespace Gdiplus;
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            GraphicsPath shape;
            if (radio) shape.AddEllipse(bx, by, bs, bs);
            else
            {
                const REAL d = 8.0f;
                shape.AddArc(bx, by, d, d, 180, 90);
                shape.AddArc(bx + bs - d, by, d, d, 270, 90);
                shape.AddArc(bx + bs - d, by + bs - d, d, d, 0, 90);
                shape.AddArc(bx, by + bs - d, d, d, 90, 90);
                shape.CloseFigure();
            }
            Color top, bottom;
            if (checked && enabled)
            {
                top = Color(255, hot ? 92 : 64, hot ? 176 : 158, 255);
                bottom = Color(255, 0, hot ? 128 : 114, hot ? 242 : 230);
            }
            else if (dark)
            {
                top = Color(static_cast<BYTE>(enabled ? (hot ? 120 : 84) : 30), 255, 255, 255);
                bottom = Color(static_cast<BYTE>(enabled ? 36 : 12), 200, 215, 255);
            }
            else
            {
                top = Color(static_cast<BYTE>(enabled ? (pressed ? 190 : 245) : 120), 255, 255, 255);
                bottom = Color(static_cast<BYTE>(enabled ? (hot ? 200 : 150) : 70), 214, 222, 238);
            }
            const LinearGradientBrush body(PointF(0, by), PointF(0, by + bs), top, bottom);
            g.FillPath(&body, &shape);
            const Pen outline(Color(static_cast<BYTE>(enabled ? 90 : 35), dark ? 0 : 60, dark ? 0 : 70, dark ? 0 : 100), 2.0f);
            g.DrawPath(&outline, &shape);
            const Pen rim(Color(static_cast<BYTE>(enabled ? (dark ? 140 : 235) : 80), 255, 255, 255), 1.0f);
            g.DrawPath(&rim, &shape);
            if (checked)
            {
                const Pen mark(Color(static_cast<BYTE>(enabled ? 255 : 120), 255, 255, 255), 2.0f);
                if (radio)
                {
                    const SolidBrush dot(Color(static_cast<BYTE>(enabled ? 255 : 120), 255, 255, 255));
                    g.FillEllipse(&dot, bx + bs * 0.31f, by + bs * 0.31f, bs * 0.38f, bs * 0.38f);
                }
                else
                {
                    Pen tick(Color(static_cast<BYTE>(enabled ? 255 : 120), 255, 255, 255), 2.0f);
                    tick.SetStartCap(LineCapRound);
                    tick.SetEndCap(LineCapRound);
                    tick.SetLineJoin(LineJoinRound);
                    const PointF pts[3] = { PointF(bx + bs * 0.24f, by + bs * 0.52f), PointF(bx + bs * 0.43f, by + bs * 0.70f),
                        PointF(bx + bs * 0.76f, by + bs * 0.30f) };
                    g.DrawLines(&tick, pts, 3);
                }
                (void)mark;
            }
            if (focused)
            {
                const Pen focus(Color(200, dark ? 235 : 70, dark ? 238 : 80, dark ? 245 : 100), 1.2f);
                if (radio) g.DrawEllipse(&focus, bx - 2.0f, by - 2.0f, bs + 4.0f, bs + 4.0f);
                else g.DrawRectangle(&focus, bx - 2.0f, by - 2.0f, bs + 4.0f, bs + 4.0f);
            }
        }

        std::array<WCHAR, 512> text{};
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        const HGDIOBJ oldFont = font != nullptr ? SelectObject(mem, font) : nullptr;
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, !enabled ? DarkMode::SystemColor(COLOR_GRAYTEXT) : (dark ? RGB(236, 238, 244) : RGB(30, 34, 44)));
        CRect textRect(box + gap, 0, width, height);
        const bool multiline = (GetWindowLongPtrW(window, GWL_STYLE) & BS_MULTILINE) != 0;
        DrawTextW(mem, text.data(), -1, &textRect,
            multiline ? (DT_WORDBREAK | DT_LEFT | DT_HIDEPREFIX) : (DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_HIDEPREFIX | DT_END_ELLIPSIS));
        if (oldFont != nullptr) SelectObject(mem, oldFont);

        BitBlt(target, 0, 0, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(mem);
    }

    LRESULT CALLBACK GlassCheckSubclass(const HWND window, const UINT message, const WPARAM wParam,
        const LPARAM lParam, const UINT_PTR id, DWORD_PTR)
    {
        switch (message)
        {
        case WM_NCDESTROY: RemoveWindowSubclass(window, GlassCheckSubclass, id); break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(window, &ps);
            PaintGlassCheck(window, dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: PaintGlassCheck(window, reinterpret_cast<HDC>(wParam)); return 0;
        default: break;
        }
        const bool mouse = message == WM_MOUSEMOVE || message == WM_MOUSELEAVE;
        const LRESULT before = mouse ? SendMessageW(window, BM_GETSTATE, 0, 0) : 0;
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == BM_SETSTATE || message == BM_SETCHECK || message == WM_ENABLE || message == WM_SETFOCUS ||
            message == WM_KILLFOCUS || message == WM_SETTEXT || message == WM_SETFONT || message == WM_LBUTTONDOWN ||
            message == WM_LBUTTONUP || message == WM_KEYUP || (mouse && before != SendMessageW(window, BM_GETSTATE, 0, 0)))
            InvalidateRect(window, nullptr, FALSE);
        return result;
    }

    void PaintGlassGroup(const HWND window, const HDC target)
    {
        CRect rc;
        GetClientRect(window, &rc);
        const int width = rc.Width(), height = rc.Height();
        if (width <= 8 || height <= 8) return;
        const HDC mem = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        const HGDIOBJ oldBitmap = SelectObject(mem, bitmap);
        GlassTheme::PaintBackdrop(mem, window);

        const bool dark = DarkMode::IsDarkModeActive();
        std::array<WCHAR, 256> text{};
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        const HGDIOBJ oldFont = font != nullptr ? SelectObject(mem, font) : nullptr;
        SIZE extent{};
        GetTextExtentPoint32W(mem, text.data(), static_cast<int>(wcslen(text.data())), &extent);
        const int pad = ScaleForDpi(10, window);
        const CRect textRect(pad + 4, 0, pad + 4 + extent.cx + 4, extent.cy);

        const int save = SaveDC(mem);
        if (text[0] != 0) ExcludeClipRect(mem, textRect.left - 2, 0, textRect.right + 2, extent.cy + 1);
        {
            using namespace Gdiplus;
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            const REAL x = 1.5f, y = static_cast<REAL>(extent.cy) / 2.0f + 1.0f;
            const REAL w = static_cast<REAL>(width) - 3.0f, h = static_cast<REAL>(height) - y - 1.5f;
            const REAL d = 16.0f;
            GraphicsPath path;
            path.AddArc(x, y, d, d, 180, 90);
            path.AddArc(x + w - d, y, d, d, 270, 90);
            path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            path.AddArc(x, y + h - d, d, d, 90, 90);
            path.CloseFigure();
            const SolidBrush wash(Color(dark ? 14 : 70, 255, 255, 255));
            g.FillPath(&wash, &path);
            const Pen outline(Color(dark ? 80 : 55, dark ? 0 : 60, dark ? 0 : 70, dark ? 0 : 100), 2.0f);
            g.DrawPath(&outline, &path);
            const Pen rim(Color(dark ? 110 : 230, 255, 255, 255), 1.0f);
            g.DrawPath(&rim, &path);
        }
        RestoreDC(mem, save);

        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, dark ? RGB(236, 238, 244) : RGB(30, 34, 44));
        CRect drawRect = textRect;
        DrawTextW(mem, text.data(), -1, &drawRect, DT_SINGLELINE | DT_LEFT | DT_TOP | DT_HIDEPREFIX | DT_NOCLIP);
        if (oldFont != nullptr) SelectObject(mem, oldFont);

        BitBlt(target, 0, 0, width, height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(mem);
    }

    LRESULT CALLBACK GlassGroupSubclass(const HWND window, const UINT message, const WPARAM wParam,
        const LPARAM lParam, const UINT_PTR id, DWORD_PTR)
    {
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, GlassGroupSubclass, id);
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT)
        {
            PAINTSTRUCT ps;
            const HDC dc = BeginPaint(window, &ps);
            PaintGlassGroup(window, dc);
            EndPaint(window, &ps);
            return 0;
        }
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == WM_SETTEXT || message == WM_SETFONT || message == WM_ENABLE) InvalidateRect(window, nullptr, FALSE);
        return result;
    }

    // Keeps a rounded window region in sync with the control size (layout managers resize controls later).
    // radius 0 means a full capsule (half the height)
    LRESULT CALLBACK RoundRegionSubclass(const HWND window, const UINT message, const WPARAM wParam,
        const LPARAM lParam, const UINT_PTR id, const DWORD_PTR radius)
    {
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, RoundRegionSubclass, id);
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == WM_SIZE)
        {
            RECT rc{};
            GetClientRect(window, &rc);
            GlassTheme::ApplyRoundedRegion(window, radius != 0 ? static_cast<int>(radius) : rc.bottom / 2);
        }
        return result;
    }

    void InstallRoundRegion(const HWND window, const int radius)
    {
        SetWindowSubclass(window, RoundRegionSubclass, 0x6C65, static_cast<DWORD_PTR>(radius));
        RECT rc{};
        GetClientRect(window, &rc);
        GlassTheme::ApplyRoundedRegion(window, radius != 0 ? radius : rc.bottom / 2);
    }

    BOOL CALLBACK StyleButtonCallback(const HWND window, LPARAM)
    {
        const bool dark = DarkMode::IsDarkModeActive();
        std::array<WCHAR, 16> className{};
        GetClassNameW(window, className.data(), static_cast<int>(className.size()));
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);

        // Thin modern scrollbars instead of the classic ones
        if ((style & (WS_VSCROLL | WS_HSCROLL)) != 0 && !dark) SetWindowTheme(window, L"Explorer", nullptr);

        if (IsGlassPushButton(window)) SetWindowSubclass(window, GlassButtonSubclass, 0x6C61, 0);
        else if (IsGlassCheck(window)) SetWindowSubclass(window, GlassCheckSubclass, 0x6C63, 0);
        else if (IsButtonClass(window) && ButtonType(window) == BS_GROUPBOX) SetWindowSubclass(window, GlassGroupSubclass, 0x6C64, 0);
        else if (_wcsicmp(className.data(), L"Static") == 0 && (style & SS_TYPEMASK) == SS_ICON)
            SetWindowSubclass(window, GlassIconSubclass, 0x6C62, 0);
        else if (_wcsicmp(className.data(), L"Edit") == 0 || _wcsicmp(className.data(), L"ComboBox") == 0)
        {
            // Softly rounded input fields
            RECT rc{};
            GetClientRect(window, &rc);
            if (rc.bottom > 14) InstallRoundRegion(window, 9);
        }
        else if (_wcsicmp(className.data(), L"SysListView32") == 0 || _wcsicmp(className.data(), L"SysTreeView32") == 0)
        {
            // Lists and trees sit in rounded cards
            RECT rc{};
            GetClientRect(window, &rc);
            if (rc.bottom > 40 && rc.right > 40) InstallRoundRegion(window, 14);
        }
        else if (_wcsicmp(className.data(), L"msctls_progress32") == 0)
        {
            RECT rc{};
            GetClientRect(window, &rc);
            if (rc.bottom > 2) InstallRoundRegion(window, 0);
        }
        return TRUE;
    }

    // Give the bottom row(s) of push buttons one consistent, roomy size, even spacing and a safe margin
    // from the dialog edges (the dialog grows when there is not enough room underneath).
    void TidyDialogButtons(const HWND dialog)
    {
        const auto scaled = [dialog](const int value) { return ScaleForDpi(value, dialog); };
        const int edgeMargin = scaled(20), bottomMargin = scaled(10);
        const int minWidth = scaled(104), minHeight = scaled(34), gap = scaled(12), pad = scaled(30);

        CRect client;
        GetClientRect(dialog, &client);
        struct Entry { HWND window; CRect rect; };
        std::vector<Entry> buttons;
        std::vector<CRect> others;
        for (HWND child = GetWindow(dialog, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
        {
            if ((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) == 0) continue; // parent may not be shown yet
            CRect rect(child);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
            if (IsGlassPushButton(child) && rect.Width() >= scaled(40) &&
                rect.top >= client.Height() * 55 / 100) buttons.push_back({ child, rect });
            else if (rect.Width() > scaled(26) || rect.Height() > scaled(26)) others.push_back(rect); // ignores the size grip
        }
        if (buttons.empty()) return;
        std::ranges::sort(buttons, {}, [](const Entry& e) { return e.rect.top; });

        // Make room below the lowest button row so it never touches the border
        const int lowest = std::ranges::max(buttons, {}, [](const Entry& e) { return e.rect.bottom; }).rect.bottom;
        const int wanted = lowest + std::max(0, minHeight - buttons.back().rect.Height()) + bottomMargin;
        if (wanted > client.bottom)
        {
            CRect window(dialog);
            SetWindowPos(dialog, nullptr, 0, 0, window.Width(), window.Height() + (wanted - client.bottom),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            GetClientRect(dialog, &client);
            // Re-read positions: a layout manager may have moved the buttons
            for (auto& entry : buttons)
            {
                entry.rect = CRect(entry.window);
                MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&entry.rect), 2);
            }
        }

        const HDC dc = GetDC(dialog);
        for (size_t first = 0; first < buttons.size();)
        {
            size_t last = first + 1;
            while (last < buttons.size() && std::abs(buttons[last].rect.top - buttons[first].rect.top) <= scaled(6)) ++last;
            std::vector<Entry> row(buttons.begin() + first, buttons.begin() + last);
            first = last;
            std::ranges::sort(row, {}, [](const Entry& e) { return e.rect.left; });

            int width = minWidth, height = minHeight, bottom = 0, left = INT_MAX, right = INT_MIN;
            for (const auto& button : row)
            {
                std::array<WCHAR, 256> text{};
                GetWindowTextW(button.window, text.data(), static_cast<int>(text.size()));
                std::wstring label = text.data();
                std::erase(label, L'&');
                const auto font = reinterpret_cast<HFONT>(SendMessageW(button.window, WM_GETFONT, 0, 0));
                const HGDIOBJ old = font != nullptr ? SelectObject(dc, font) : nullptr;
                SIZE extent{};
                GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &extent);
                if (old != nullptr) SelectObject(dc, old);
                width = std::max<int>(width, extent.cx + 2 * pad);
                height = std::max<int>(height, button.rect.Height());
                bottom = std::max<int>(bottom, button.rect.bottom);
                left = std::min<int>(left, button.rect.left);
                right = std::max<int>(right, button.rect.right);
            }

            // Keep the row clear of the bottom and side borders
            bottom = std::min<int>(bottom, client.bottom - bottomMargin);
            const int count = static_cast<int>(row.size());
            const int total = count * width + (count - 1) * gap;
            const bool anchorRight = client.right - right <= left - client.left;
            int startX = anchorRight ? std::min<int>(right, client.right - edgeMargin) - total : std::max<int>(left, client.left + edgeMargin);
            startX = std::max<int>(startX, client.left + edgeMargin);
            CRect span(startX, bottom - height, startX + total, bottom);

            // Widen the dialog when the larger buttons would run into other controls on their row
            int shortfall = 0;
            for (const auto& other : others)
            {
                RECT overlap{};
                if (::IntersectRect(&overlap, &span, &other))
                    shortfall = std::max<int>(shortfall, other.right + 2 * gap - span.left);
            }
            if (shortfall > 0 && anchorRight)
            {
                const CRect window(dialog);
                SetWindowPos(dialog, nullptr, 0, 0, window.Width() + shortfall, window.Height(),
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                GetClientRect(dialog, &client);
                startX = client.right - edgeMargin - total;
                span = CRect(startX, bottom - height, startX + total, bottom);
                shortfall = 0;
            }

            // Skip the row when it would not fit
            const bool fits = shortfall == 0 && span.right <= client.right - edgeMargin && span.top >= client.top;
            if (!fits)
            {
                // Still lift the row off the border without resizing
                for (const auto& button : row)
                    if (button.rect.bottom > client.bottom - bottomMargin)
                        SetWindowPos(button.window, nullptr, button.rect.left,
                            client.bottom - bottomMargin - button.rect.Height(), 0, 0,
                            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                continue;
            }
            for (int i = 0; i < count; ++i)
                SetWindowPos(row[i].window, nullptr, startX + i * (width + gap), span.top, width, height,
                    SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ReleaseDC(dialog, dc);
    }
}

void GlassTheme::StyleDialog(const HWND dialog)
{
    if (!IsWindow(dialog)) return;
    const bool child = (GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CHILD) != 0;
    if (!child)
    {
        // Title bar blends into the glass like the main window
        const bool dark = DarkMode::IsDarkModeActive();
        const COLORREF caption = dark ? RGB(21, 23, 29) : RGB(236, 238, 243);
        const COLORREF text = dark ? RGB(236, 238, 244) : RGB(34, 38, 48);
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(dialog, 35, &caption, sizeof(caption));
        DwmSetWindowAttribute(dialog, 36, &text, sizeof(text));
        DwmSetWindowAttribute(dialog, 34, &caption, sizeof(caption));
        DwmSetWindowAttribute(dialog, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        TidyDialogButtons(dialog);
    }
    EnumChildWindows(dialog, StyleButtonCallback, 0);
    if (child || (GetWindowLongPtrW(dialog, GWL_EXSTYLE) & WS_EX_LAYERED) != 0) return;

    // GPU glass rim over every visible push button, tracked live
    GlassOverlay::Attach(dialog, [dialog]
    {
        std::vector<GlassRegion> regions;
        std::pair<HWND, std::vector<GlassRegion>*> context{ dialog, &regions };
        EnumChildWindows(dialog, [](const HWND window, const LPARAM param)
        {
            auto* list = reinterpret_cast<std::pair<HWND, std::vector<GlassRegion>*>*>(param);
            if (!IsWindowVisible(window) || !IsGlassPushButton(window)) return TRUE;
            RECT rc{};
            GetClientRect(window, &rc);
            MapWindowPoints(window, list->first, reinterpret_cast<POINT*>(&rc), 2);
            rc.left += 1; rc.top += 1; rc.right -= 1; rc.bottom -= 1;
            list->second->push_back({ rc, (rc.bottom - rc.top) * 0.5f });
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        GlassLayer layer;
        layer.regions = std::move(regions);
        return layer;
    });
}

// -----------------------------------------------------------------------------
//  Class-owned theme drawing
// -----------------------------------------------------------------------------
void CSplitterWnd::DrawBackground(CDC& dc, const CRect& rect) const
{
    GlassTheme::PaintBackdrop(dc.Handle(), m_hWnd);
    const bool dark = DarkMode::IsDarkModeActive();
    for (const int index : { 0, 1 })
    {
        const CWnd* pane = GetPane(index);
        if (pane == nullptr || !IsWindow(pane->m_hWnd) || pane->IsSplitterWindow())
            continue;
        DrawCardFrame(dc.Handle(), GetChildWindowRect(pane->Handle()), dark);
    }
    (void)rect;
}

bool CSplitterWnd::BuildGlassSplitter(const HWND frame, GlassSplitter& out) const
{
    if (!IsWindow(m_hWnd) || !::IsWindowVisible(m_hWnd) || !::IsChild(frame, m_hWnd)) return false;
    if (GetPane(0) == nullptr || GetPane(1) == nullptr) return false;
    RECT bar = SplitterRect();
    if (bar.right - bar.left <= 0 || bar.bottom - bar.top <= 0) return false;
    ::MapWindowPoints(m_hWnd, frame, reinterpret_cast<POINT*>(&bar), 2);
    out.bar = bar;
    out.vertical = IsVertical();
    out.tracking = m_bTracking;
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::ScreenToClient(frame, &cursor);
    out.hot = m_bTracking || (cursor.x >= bar.left && cursor.x < bar.right && cursor.y >= bar.top && cursor.y < bar.bottom &&
        ::WindowFromPoint([&] { POINT p = cursor; ::ClientToScreen(frame, &p); return p; }()) == m_hWnd);
    out.id = static_cast<int>(0x20000000 | ((reinterpret_cast<uintptr_t>(m_hWnd) >> 2) & 0xFFFF));
    size_t hash = 1469598103934665603ull;
    for (const int v : { static_cast<int>(bar.left), static_cast<int>(bar.top), static_cast<int>(bar.right), static_cast<int>(bar.bottom),
        static_cast<int>(out.hot), static_cast<int>(out.tracking) })
        hash = (hash ^ static_cast<size_t>(v)) * 1099511628211ull;
    out.hash = hash;
    return true;
}

void CSplitterWnd::OnPaint()
{
    CPaintDC dc(this);
    const CRect rect = GetClientRect();
    DrawBackground(dc, rect);
}

HBRUSH CToolBar::OnCtlColor(CDC* dc, WindowRef, const UINT type)
{
    const HBRUSH brush = DarkMode::OnCtlColor(dc, type);
    return brush != nullptr ? brush : reinterpret_cast<HBRUSH>(CallDefaultHandler());
}

void CToolBar::OnCustomDraw(NMHDR* pNMHDR, LRESULT* pResult) const
{
    switch (auto* customDraw = reinterpret_cast<LPNMTBCUSTOMDRAW>(pNMHDR); customDraw->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
    {
        const CRect rect = GetClientRect();
        auto dc = CDC::Borrow(customDraw->nmcd.hdc);
        const bool dark = DarkMode::IsDarkModeActive();
        const COLORREF background = dark ?
            DarkMode::SystemColor(COLOR_MENUBAR) : GetSysColor(COLOR_BTNFACE);
        GlassTheme::PaintBackdrop(dc.Handle(), m_hWnd);
        if (!GlassOverlay::IsActive()) // the compositor paints group pills instead of one long panel
        {
            CRect glass = rect;
            glass.Deflate(1, 1);
            DrawGlassPanel(dc.Handle(), glass, background, glass.Height() / 2, dark);
        }
        *pResult = CDRF_NOTIFYITEMDRAW;
        break;
    }
    case CDDS_ITEMPREPAINT:
        if (GlassOverlay::IsActive() && customDraw->nmcd.dwItemSpec == 0)
        {
            *pResult = CDRF_SKIPDEFAULT; // separators: groups are separated by space, not by a line
            break;
        }
        if (GlassOverlay::IsActive())
        {
            // The GPU overlay draws glyph-only buttons (with hover / press animation); text buttons stay classic
            TBBUTTONINFOW info{ .cbSize = sizeof(info), .dwMask = TBIF_IMAGE };
            if (::SendMessageW(m_hWnd, TB_GETBUTTONINFOW, customDraw->nmcd.dwItemSpec, reinterpret_cast<LPARAM>(&info)) >= 0 &&
                info.iImage != I_IMAGENONE)
            {
                *pResult = CDRF_SKIPDEFAULT;
                break;
            }
        }
        customDraw->clrText = DarkMode::SystemColor(COLOR_BTNTEXT);
        *pResult = TBCDRF_USECDCOLORS | TBCDRF_NOEDGES | TBCDRF_NOBACKGROUND | CDRF_DODEFAULT;
        break;
    default:
        *pResult = CDRF_DODEFAULT;
        break;
    }
}

void CStatusBar::DrawPaneBorder(CDC& dc, const CRect& rect)
{
    const COLORREF border = DarkMode::IsDarkModeActive()
        ? DarkMode::SystemColor(COLOR_WINDOWFRAME) : GetSysColor(COLOR_3DSHADOW);
    if (DarkMode::IsDarkModeActive()) dc.FillSolidRect(rect.left, rect.top, rect.Width(), 1, border);
    dc.FillSolidRect(rect.right - 1, rect.top, 1, rect.Height(), border);
}

void CStatusBar::OnPaint()
{
    CPaintDC paintDC(this);
    CBufferedDC dc(paintDC, this);
    const CRect rcClient = GetClientRect();
    const COLORREF barFace = DarkMode::SystemColor(
        DarkMode::IsDarkModeActive() ? COLOR_MENUBAR : COLOR_BTNFACE);
    GlassTheme::PaintBackdrop(dc.Handle(), m_hWnd);
    GdiObjectSelection selectFont(&dc, GetAppFont(m_hWnd));
    dc.SetBkMode(TRANSPARENT);

    const auto rects = LayoutPanes();
    for (const auto& [pane, rectLayout] : std::views::zip(m_panes, rects))
    {
        CRect rect = rectLayout;
        const bool dark = DarkMode::IsDarkModeActive();
        rect.Deflate(2, 2);
        DrawGlassPanel(dc.Handle(), rect, barFace, 9, dark);
        CRect textRect = rect;
        textRect.Deflate(4, 0, 4, 0);
        dc.SetTextColor(DarkMode::SystemColor(COLOR_BTNTEXT));
        if (!GlassOverlay::IsActive()) // the GPU overlay draws the pane text with DirectWrite
            dc.DrawText(pane.text.c_str(), static_cast<int>(pane.text.size()), &textRect,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void CTabControl::SetLocation(const Location loc)
{
    m_location = loc;
    if (!IsWindow(m_hWnd)) return;

    ModifyStyle(m_location == Location::Bottom ? 0 : TCS_BOTTOM,
        m_location == Location::Bottom ? TCS_BOTTOM : 0, SWP_FRAMECHANGED);
    LayoutPanes();
    Invalidate(false);
}

std::wstring_view CTabControl::GetTabLabel(const int index) const
{
    return index >= 0 && index < GetTabCount() ? m_tabs[index].label : std::wstring_view();
}

void CTabControl::SetTabLabel(const int i, const std::wstring_view label)
{
    if (i < 0 || i >= GetTabCount()) return;

    m_tabs[i].label = label;
    if (const int native = NativeIndexFromLogical(i); native >= 0)
    {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(m_tabs[i].label.c_str());
        SendNativeMessage(TCM_SETITEMW, static_cast<WPARAM>(native), &item);
    }
    if (IsWindow(m_hWnd)) Invalidate(false);
}

bool CTabControl::Create(const RECT& rect, CWnd* pParentWnd, const UINT nID, const bool focusTabStrip)
{
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TAB_CLASSES };
    ::InitCommonControlsEx(&icc);
    m_focusTabStrip = focusTabStrip;
    DWORD tabStyle = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
        TCS_TABS | TCS_SINGLELINE | TCS_RAGGEDRIGHT;
    if (m_focusTabStrip)
        tabStyle |= WS_TABSTOP;
    if (m_location == Location::Bottom)
        tabStyle |= TCS_BOTTOM;
    if (!CreateEx(0, WC_TABCONTROLW, nullptr, tabStyle, rect, pParentWnd, nID)) return false;
    SendNativeMessage(TCM_SETUNICODEFORMAT, true, 0);
    return true;
}

int CTabControl::AddTab(CWnd* window, const std::wstring_view label)
{
    m_tabs.push_back({ window, std::wstring(label) });
    if (m_activeTab < 0) m_activeTab = static_cast<int>(m_tabs.size()) - 1;
    RebuildNativeTabs();
    LayoutPanes();
    Invalidate(false);
    return static_cast<int>(m_tabs.size()) - 1;
}

bool CTabControl::PreprocessMessage(MSG* pMsg)
{
    if (pMsg != nullptr && pMsg->hwnd == m_hWnd &&
        (pMsg->message == WM_KEYDOWN || pMsg->message == WM_SYSKEYDOWN ||
            pMsg->message == WM_CHAR || pMsg->message == WM_SYSCHAR) &&
        !IsPlainTabTraversal(*pMsg))
    {
        if (ForwardKeyboardMessageToActiveTab(*pMsg)) return true;
    }

    return CWnd::PreprocessMessage(pMsg);
}

void CTabControl::SetTabVisible(const int i, const bool show)
{
    if (i < 0 || i >= GetTabCount()) return;
    if (m_tabs[i].visible == show) return;

    const int previousActiveTab = m_activeTab;
    const bool moveFocusToActiveTab = previousActiveTab == i &&
        ShouldMoveFocusOnTabActivation(previousActiveTab);
    int activeTab = previousActiveTab;
    if (!show && previousActiveTab == i)
    {
        activeTab = -1;
        for (const auto [k, tab] : std::views::enumerate(m_tabs))
        {
            if (static_cast<int>(k) != i && tab.visible) { activeTab = static_cast<int>(k); break; }
        }
    }
    else if (show && previousActiveTab < 0) activeTab = i;

    if (activeTab != previousActiveTab)
    {
        if (const auto p = GetParent())
            ::SendMessageW(p.Handle(), WM_WDS_TAB_CHANGING, static_cast<WPARAM>(activeTab), 0);
    }

    m_tabs[i].visible = show;
    m_activeTab = activeTab;
    RebuildNativeTabs();
    LayoutPanes();
    if (moveFocusToActiveTab) FocusActiveTabWindow();
    if (m_activeTab != previousActiveTab) NotifyParentOfTabChange(m_activeTab);
    Invalidate(false);
}

LRESULT CTabControl::OnNcHitTest(const CPoint point)
{
    const CPoint clientPt = ToClient(point);
    if (TabStripRect().Contains(clientPt))
        return HTCLIENT;
    return CallDefaultHandler();
}

void CTabControl::OnSize(UINT, int, int)
{
    CallDefaultHandler(); LayoutPanes(); Invalidate(false);
}

void CTabControl::OnFontSizeChanged(int, int)
{
    RebuildNativeTabs();
    LayoutPanes();
    Invalidate(false);
}

void CTabControl::OnLButtonDown(UINT, const CPoint point)
{
    const CRect rcStrip = TabStripRect();
    UpdatePaintedTabRects(rcStrip, m_location == Location::Bottom, UsesLabelOnlyTabs());

    int logical = -1;
    if (m_activeTab >= 0 && m_activeTab < GetTabCount() && m_tabs[m_activeTab].paintedRect.Contains(point))
    {
        logical = m_activeTab;
    }
    else
    {
        for (int native = static_cast<int>(m_visibleToLogical.size()) - 1; native >= 0; --native)
        {
            const int candidate = m_visibleToLogical[static_cast<size_t>(native)];
            if (candidate < 0 || candidate >= GetTabCount() || candidate == m_activeTab) continue;
            if (m_tabs[candidate].paintedRect.Contains(point)) { logical = candidate; break; }
        }
    }

    m_handledPaintedTabMouseDown = logical >= 0;
    if (logical >= 0 && logical != m_activeTab) ActivateTab(logical, true);
    if (!m_handledPaintedTabMouseDown)
        CallDefaultHandler();
    if (m_focusTabStrip) SetFocus();
    else RedirectFocusAwayFromTabControl();
}

void CTabControl::OnLButtonUp(UINT, CPoint)
{
    if (!m_handledPaintedTabMouseDown)
        CallDefaultHandler();
    m_handledPaintedTabMouseDown = false;
    RedirectFocusAwayFromTabControl();
}

void CTabControl::OnSetFocus(WindowRef)
{
    if (m_focusTabStrip) CallDefaultHandler();
    else RedirectFocusAwayFromTabControl();
    Invalidate(false);
}

void CTabControl::OnKillFocus(WindowRef)
{
    if (m_focusTabStrip) CallDefaultHandler();
    Invalidate(false);
}

void CTabControl::OnKeyDown(const UINT nChar, const UINT nRepCnt, const UINT nFlags)
{
    MSG msg{};
    msg.hwnd = m_hWnd;
    msg.message = WM_KEYDOWN;
    msg.wParam = nChar;
    msg.lParam = MAKELPARAM(nRepCnt, nFlags);
    if (!IsPlainTabTraversal(msg) && ForwardKeyboardMessageToActiveTab(msg)) return;
    CallDefaultHandler();
}

void CTabControl::OnTabSelectionChanged(NMHDR*, LRESULT* pResult)
{
    const int native = static_cast<int>(SendNativeMessage(TCM_GETCURSEL));
    if (native >= 0 && std::cmp_less(native, m_visibleToLogical.size()))
    {
        ActivateTab(m_visibleToLogical[static_cast<size_t>(native)], false);
    }
    if (pResult) *pResult = 0;
}

void CTabControl::OnPaint()
{
    CPaintDC paintDC(this);
    CBufferedDC dc(paintDC, this);

    const CRect rcClient = GetClientRect();
    const COLORREF tabBorder = GetSysColor(COLOR_3DSHADOW);
    const COLORREF tabPane = DarkMode::IsDarkModeActive()
        ? DarkMode::SystemColor(COLOR_WINDOW) : GetSysColor(COLOR_3DHILIGHT);
    const COLORREF tabStrip = DarkMode::IsDarkModeActive()
        ? DarkMode::SystemColor(COLOR_WINDOW) : GetSysColor(COLOR_3DFACE);
    const COLORREF buttonFace = DarkMode::SystemColor(COLOR_BTNFACE);

    const int tabH = TabStripHeight();
    const bool bottomTabs = m_location == Location::Bottom;
    const bool labelOnlyTabs = UsesLabelOnlyTabs();
    const bool dark = IsDarkColor(tabStrip) || IsDarkColor(tabPane);
    const COLORREF controlBackground = labelOnlyTabs ? buttonFace : tabPane;
    (void)controlBackground;
    GlassTheme::PaintBackdrop(dc.Handle(), m_hWnd);

    CRect rcStrip = bottomTabs ?
        CRect(rcClient.left, std::max(rcClient.top, rcClient.bottom - tabH), rcClient.right, rcClient.bottom) :
        CRect(rcClient.left, rcClient.top, rcClient.right, std::min(rcClient.bottom, rcClient.top + tabH));
    const COLORREF stripBg = dark ? tabStrip
                                  : (labelOnlyTabs ? buttonFace : tabStrip);
    const COLORREF stripBorder = dark ? RGB(95, 95, 95) : tabBorder;
    const COLORREF activeTabBg = dark ? RGB(190, 190, 190)
                                      : (labelOnlyTabs ? buttonFace : RGB(255, 255, 255));
    const COLORREF inactiveTabBg = dark ? RGB(31, 31, 31)
                                        : BlendColor(tabStrip, RGB(0, 0, 0), 0.05);
    const COLORREF activeText = RGB(10, 132, 255);
    const COLORREF inactiveText = dark ? RGB(200, 202, 210) : RGB(80, 84, 94);
    (void)stripBg;
    {
        CRect capsule = rcStrip;
        capsule.Deflate(0, 2);
        DrawGlassPanel(dc.Handle(), capsule, 0, capsule.Height() / 2, dark);
    }

    HFONT font = GetFont();
    if (font == nullptr) font = GetAppFont(m_hWnd);
    GdiObjectSelection selectFont(&dc, font);
    CFont boldFont(font, FW_BOLD);
    dc.SetBkMode(TRANSPARENT);

    struct TabPaintInfo
    {
        int logical = -1;
        CRect rect;
    };

    TabPaintInfo activeTab;
    bool hasActiveTab = false;

    UpdatePaintedTabRects(rcStrip, bottomTabs, labelOnlyTabs);

    const int dpi = GetWindowDpi(m_hWnd);
    const auto scale = [dpi](const int value) { return MulDiv(value, dpi, 96); };
    const int textInsetX = scale(5);
    const int textInsetY = scale(1);
    const int minSlant = scale(4);
    const int maxSlant = scale(8);
    const bool drawFocus = m_focusTabStrip && HasFocus() &&
        (SendMessage(WM_QUERYUISTATE) & UISF_HIDEFOCUS) == 0;
    CBrush activeBrush(activeTabBg);
    CBrush inactiveBrush(inactiveTabBg);
    CPen borderPen(PS_SOLID, 1, stripBorder);
    CPen focusPen(PS_DOT, 1, activeText);

    auto drawTab = [&](const TabPaintInfo& tab, const bool active)
        {
            if (tab.logical < 0 || tab.logical >= GetTabCount()) return;
            const CRect rcTab = tab.rect;
            const int slant = std::clamp(rcTab.Width() / 5, minSlant, maxSlant);
            POINT points[4] = {};
            if (bottomTabs)
            {
                points[0] = { rcTab.left, rcTab.top };
                points[1] = { rcTab.left + slant, rcTab.bottom - 1 };
                points[2] = { rcTab.right - slant, rcTab.bottom - 1 };
                points[3] = { rcTab.right, rcTab.top };
            }
            else
            {
                points[0] = { rcTab.left, rcTab.bottom - 1 };
                points[1] = { rcTab.left + slant, rcTab.top };
                points[2] = { rcTab.right - slant, rcTab.top };
                points[3] = { rcTab.right, rcTab.bottom - 1 };
            }

            (void)points; (void)activeBrush; (void)inactiveBrush; (void)borderPen; (void)focusPen; (void)drawFocus;
            {
                // Glass pill tab
                using namespace Gdiplus;
                Graphics g(dc.Handle());
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                CRect pill = rcTab;
                pill.Deflate(2, 3);
                const REAL px = static_cast<REAL>(pill.left), py = static_cast<REAL>(pill.top);
                const REAL pw = static_cast<REAL>(pill.Width() - 1), ph = static_cast<REAL>(pill.Height() - 1);
                GraphicsPath path;
                path.AddArc(px, py, ph, ph, 90, 180);
                path.AddArc(px + pw - ph, py, ph, ph, 270, 180);
                path.CloseFigure();
const Color top = active ? (dark ? Color(110, 255, 255, 255) : Color(255, 210, 210, 218)) : Color(0, 255, 255, 255);
                const Color bottom = active ? (dark ? Color(50, 220, 225, 240) : Color(255, 198, 198, 208)) : Color(0, 255, 255, 255);
                const LinearGradientBrush body(PointF(0, py), PointF(0, py + ph), top, bottom);
                g.FillPath(&body, &path);
                const Pen rim(Color(static_cast<BYTE>(active ? (dark ? 120 : 0) : 0), 255, 255, 255), 1.0f);
                g.DrawPath(&rim, &path);
            }

            dc.SetTextColor(active ? activeText : inactiveText);
            const HGDIOBJ textFont = active && boldFont ? boldFont.Handle() : font;
            GdiObjectSelection selectTextFont(&dc, textFont);
            CRect rcText = rcTab;
            rcText.Deflate(textInsetX, textInsetY, textInsetX, textInsetY);
            const std::wstring& text = m_tabs[tab.logical].label;
            const int oldBk = dc.SetBkMode(TRANSPARENT);
            const CSize textExtent = dc.GetTextExtent(text);
            const auto textMetrics = dc.GetTextMetrics();

            const int x = static_cast<int>(rcText.left) +
                std::max<int>(0, (static_cast<int>(rcText.Width()) - textExtent.cx) / 2);
            const int y = static_cast<int>(rcText.top) +
                std::max<int>(0, (static_cast<int>(rcText.Height()) -
                    (textMetrics ? textMetrics->tmHeight : 0)) / 2);
            ExtTextOutW(dc, x, y, ETO_CLIPPED, &rcText, text.c_str(),
                static_cast<UINT>(text.size()), nullptr);
            dc.SetBkMode(oldBk);
        };

    for (const int logical : m_visibleToLogical)
    {
        if (logical < 0 || logical >= GetTabCount()) continue;

        const CRect rcTab = m_tabs[logical].paintedRect;
        if (rcTab.IsEmpty()) continue;

        if (logical == m_activeTab)
        {
            activeTab.logical = logical;
            activeTab.rect = rcTab;
            hasActiveTab = true;
            continue;
        }

        drawTab({ logical, rcTab }, false);
    }

    if (hasActiveTab)
    {
        drawTab(activeTab, true);
    }
}

std::span<const RouteEntry> CTabControl::Routes()
{
    static constexpr std::array entries
    {
        Route::Window<&OnNcHitTest>(WM_NCHITTEST),
        Route::Window<&OnSize>(WM_SIZE),
        Route::Window<&OnEraseBkgnd>(WM_ERASEBKGND),
        Route::Window<&OnLButtonDown>(WM_LBUTTONDOWN),
        Route::Window<&OnLButtonUp>(WM_LBUTTONUP),
        Route::Window<&OnKeyDown>(WM_KEYDOWN),
        Route::Window<&OnSetFocus>(WM_SETFOCUS),
        Route::Window<&OnKillFocus>(WM_KILLFOCUS),
        Route::Window<&OnPaint>(WM_PAINT),
        Route::ReflectNotify<&OnTabSelectionChanged>(TCN_SELCHANGE),
    };
    return entries;
}

bool CTabControl::IsPlainTabTraversal(const MSG& msg)
{
    if (msg.message != WM_KEYDOWN || msg.wParam != VK_TAB) return false;
    return !IsKeyDown(VK_CONTROL) && !IsKeyDown(VK_MENU);
}

bool CTabControl::IsDarkColor(const COLORREF color)
{
    const int luminance = GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114;
    return luminance < 128000;
}

int CTabControl::NativeIndexFromLogical(const int logical) const
{
    const auto it = std::ranges::find(m_visibleToLogical, logical);
    return it != m_visibleToLogical.end() ?
        static_cast<int>(std::ranges::distance(m_visibleToLogical.begin(), it)) : -1;
}

void CTabControl::RebuildNativeTabs()
{
    if (!IsWindow(m_hWnd)) return;

    UpdateNativePadding();
    SendNativeMessage(TCM_DELETEALLITEMS);
    m_visibleToLogical.clear();
    m_visibleToLogical.reserve(m_tabs.size());

    int native = 0;
    for (const auto [logical, tab] : std::views::enumerate(m_tabs))
    {
        if (!tab.visible)
        {
            tab.paintedRect.Clear();
            continue;
        }

        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(tab.label.c_str());
        SendNativeMessage(TCM_INSERTITEMW, static_cast<WPARAM>(native), &item);
        m_visibleToLogical.push_back(static_cast<int>(logical));
        ++native;
    }

    SyncNativeSelection();
}

void CTabControl::UpdateNativePadding()
{
    if (!IsWindow(m_hWnd)) return;

    const bool labelOnlyTabs = UsesLabelOnlyTabs();
    SendNativeMessage(TCM_SETPADDING, 0, MAKELPARAM(
        ::ScaleForDpi(labelOnlyTabs ? 8 : 12, m_hWnd),
        ::ScaleForDpi(labelOnlyTabs ? 2 : 3, m_hWnd)));
}

void CTabControl::SyncNativeSelection()
{
    if (!IsWindow(m_hWnd)) return;
    if (const int native = NativeIndexFromLogical(m_activeTab); native >= 0)
        SendNativeMessage(TCM_SETCURSEL, static_cast<WPARAM>(native));
}

bool CTabControl::ActivateTab(const int i, const bool syncNative)
{
    if (i < 0 || i >= GetTabCount() || !m_tabs[i].visible) return false;
    if (i == m_activeTab) return true;

    const int previousActiveTab = m_activeTab;
    const bool moveFocusToActiveTab = ShouldMoveFocusOnTabActivation(previousActiveTab);

    if (const auto p = GetParent())
        ::SendMessageW(p.Handle(), WM_WDS_TAB_CHANGING, static_cast<WPARAM>(i), 0);

    m_activeTab = i;
    if (syncNative) SyncNativeSelection();
    LayoutPanes();
    if (moveFocusToActiveTab) FocusActiveTabWindow();

    NotifyParentOfTabChange(i);
    Invalidate(false);
    return true;
}

void CTabControl::NotifyParentOfTabChange(const int activeTab) const
{
    if (const auto p = GetParent())
        ::SendMessageW(p.Handle(), WM_WDS_TAB_CHANGED, static_cast<WPARAM>(activeTab), 0);
}

bool CTabControl::ShouldMoveFocusOnTabActivation(const int previousActiveTab) const
{
    const HWND focus = ::GetFocus();
    if (focus == nullptr) return false;
    if (focus == m_hWnd || IsChild(focus)) return true;

    const CWnd* previous = GetTabWindow(previousActiveTab);
    return previous != nullptr && IsWindow(previous->m_hWnd) &&
        (focus == previous->m_hWnd || previous->IsChild(focus));
}

bool CTabControl::ForwardKeyboardMessageToActiveTab(const MSG& msg)
{
    CWnd* p = GetTabWindow(m_activeTab);
    if (p == nullptr || !IsWindow(p->m_hWnd) || !::IsWindowVisible(p->m_hWnd) || !::IsWindowEnabled(p->m_hWnd)) return false;

    p->SetFocus();
    HWND target = ::GetFocus();
    if (target == nullptr || (target != p->m_hWnd && !p->IsChild(target))) target = p->m_hWnd;
    ::SendMessageW(target, msg.message, msg.wParam, msg.lParam);
    return true;
}

bool CTabControl::FocusActiveTabWindow()
{
    CWnd* p = GetTabWindow(m_activeTab);
    if (p == nullptr || !IsWindow(p->m_hWnd) || !::IsWindowVisible(p->m_hWnd) || !::IsWindowEnabled(p->m_hWnd)) return false;

    p->SetFocus();
    const HWND focus = ::GetFocus();
    return focus == p->m_hWnd || p->IsChild(focus);
}

bool CTabControl::RedirectFocusAwayFromTabControl()
{
    if (m_focusTabStrip) return false;
    if (FocusActiveTabWindow()) return true;

    if (const HWND parent = ::GetParent(m_hWnd);
        parent != nullptr && IsWindow(parent) && ::IsWindowVisible(parent) && ::IsWindowEnabled(parent))
    {
        ::SetFocus(parent);
        return ::GetFocus() != m_hWnd;
    }
    return false;
}

bool CTabControl::GetNativeItemRect(const int native, CRect& rc) const
{
    RECT r{};
    if (SendNativeMessage(TCM_GETITEMRECT, static_cast<WPARAM>(native), &r) == 0) return false;
    rc = r;
    return true;
}

bool CTabControl::BuildGlassStrip(const HWND frame, GlassTabStrip& out)
{
    if (!IsWindow(m_hWnd) || !::IsWindowVisible(m_hWnd) || !::IsChild(frame, m_hWnd) || UsesLabelOnlyTabs()) return false;
    const CRect strip = TabStripRect();
    UpdatePaintedTabRects(strip, m_location == Location::Bottom, false);
    const auto toFrame = [&](RECT rc)
    {
        ::MapWindowPoints(m_hWnd, frame, reinterpret_cast<POINT*>(&rc), 2);
        return rc;
    };
    out.strip = toFrame(strip);
    out.pane = out.strip;
    if (const HWND pane = ::GetParent(m_hWnd); pane != nullptr)
    {
        RECT pr{};
        ::GetWindowRect(pane, &pr);
        ::MapWindowPoints(nullptr, frame, reinterpret_cast<POINT*>(&pr), 2);
        out.pane = pr;
    }
    out.paneRadius = GlassTheme::CardRadius;
    out.dark = DarkMode::IsDarkModeActive();
    LOGFONT font{};
    ::GetObjectW(GetFont() != nullptr ? GetFont() : GetAppFont(m_hWnd), sizeof(font), &font);
    out.fontPx = std::max(8, std::abs(static_cast<int>(font.lfHeight))) * 11 / 10; // tab labels read a little larger
    size_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const size_t v) { hash = (hash ^ v) * 1099511628211ull; };
    for (const int logical : m_visibleToLogical)
    {
        if (logical < 0 || logical >= GetTabCount() || !m_tabs[logical].visible) continue;
        GlassTab tab;
        tab.rect = toFrame(m_tabs[logical].paintedRect);
        tab.label = m_tabs[logical].label;
        tab.active = logical == m_activeTab;
        mix(static_cast<size_t>(tab.rect.left)); mix(static_cast<size_t>(tab.rect.right)); mix(static_cast<size_t>(tab.active));
        mix(std::hash<std::wstring>{}(tab.label));
        out.tabs.push_back(std::move(tab));
    }
    mix(static_cast<size_t>(out.strip.left)); mix(static_cast<size_t>(out.strip.top)); mix(static_cast<size_t>(out.strip.right));
    mix(static_cast<size_t>(out.dark));
    out.hash = hash;
    return !out.tabs.empty();
}

CRect CTabControl::TabStripRect() const
{
    const CRect rcClient = GetClientRect();
    const int tabH = TabStripHeight();
    return (m_location == Location::Bottom) ?
        CRect(rcClient.left, std::max(rcClient.top, rcClient.bottom - tabH), rcClient.right, rcClient.bottom) :
        CRect(rcClient.left, rcClient.top, rcClient.right, std::min(rcClient.bottom, rcClient.top + tabH));
}

void CTabControl::UpdatePaintedTabRects(const CRect& rcStrip, const bool bottomTabs, const bool labelOnlyTabs)
{
    for (TabInfo& tab : m_tabs)
        tab.paintedRect.Clear();

    const int dpi = GetWindowDpi(m_hWnd);
    const auto scale = [dpi](const int value) { return MulDiv(value, dpi, 96); };
    const int tabVisualHeight = std::clamp(rcStrip.Height() - scale(4), 0, ::ScaleForDpi(18, m_hWnd));
    const int leftInset = scale(labelOnlyTabs ? 2 : 3);
    const int overlap = scale(labelOnlyTabs ? 1 : 2);
    const int rightExpansion = scale(labelOnlyTabs ? 2 : 4);

    for (const auto [native, logical] : std::views::enumerate(m_visibleToLogical))
    {
        if (logical < 0 || logical >= GetTabCount()) continue;

        CRect rcTab;
        if (!GetNativeItemRect(static_cast<int>(native), rcTab)) continue;

        if (bottomTabs)
        {
            rcTab.top = rcStrip.top;
            rcTab.bottom = std::min(rcStrip.bottom, rcTab.top + tabVisualHeight);
        }
        else
        {
            rcTab.bottom = rcStrip.bottom;
            rcTab.top = std::max(rcStrip.top, rcTab.bottom - tabVisualHeight);
        }
        rcTab.left = std::max(rcStrip.left + leftInset, rcTab.left - (native == 0 ? 0 : overlap));
        rcTab.right += rightExpansion;
        m_tabs[logical].paintedRect = rcTab;
    }
}

bool CTabControl::UsesLabelOnlyTabs() const
{
    return !std::ranges::any_of(m_tabs, [](const TabInfo& tab)
        {
            return tab.visible && tab.window != nullptr;
        });
}

void CTabControl::LayoutPanes()
{
    if (!IsWindow(m_hWnd)) return;
    const CRect rc = GetClientRect();
    const int tabH = TabStripHeight();
    CRect rcPane = (m_location == Location::Bottom) ?
        CRect(rc.left, rc.top, rc.right, std::max(rc.top, rc.bottom - tabH)) :
        CRect(rc.left, rc.top + tabH, rc.right, rc.bottom);
    for (const auto [i, tab] : std::views::enumerate(m_tabs))
    {
        CWnd* p = tab.window;
        if (p == nullptr || !IsWindow(p->m_hWnd)) continue;
        if (static_cast<int>(i) == m_activeTab) { p->MoveWindow(rcPane); p->ShowWindow(SW_SHOW); }
        else p->ShowWindow(SW_HIDE);
    }
}

// -----------------------------------------------------------------------------
//  Cold CWnd and native-dialog helpers
// -----------------------------------------------------------------------------
int CToolBar::AddImage(const CBitmap& bmp)
{
    if (m_imageList == nullptr || m_disabledImageList == nullptr) RecreateImageLists();
    if (m_imageList == nullptr || m_disabledImageList == nullptr || bmp.Handle() == nullptr) return -1;
    if (ImageList_GetImageCount(m_imageList) != ImageList_GetImageCount(m_disabledImageList)) return -1;

    // Images are rendered at the toolbar's current size before insertion.
    const auto hbmToAdd = static_cast<HBITMAP>(bmp.m_hObject);
    BITMAP bitmap{};
    if (GetObjectW(hbmToAdd, sizeof(bitmap), &bitmap) == 0 || m_imageSize <= 0) return -1;
    const int64_t heightValue = bitmap.bmHeight < 0 ? -static_cast<int64_t>(bitmap.bmHeight) : bitmap.bmHeight;
    if (bitmap.bmWidth != m_imageSize || heightValue != m_imageSize) return -1;
    const int width = bitmap.bmWidth;
    const int height = static_cast<int>(heightValue);
    const uint64_t pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixelCount > SIZE_MAX / 4) return -1;

    const int index = ImageList_Add(m_imageList, hbmToAdd, nullptr);
    if (index < 0) return -1;

    const CBitmap disabled([&]() -> HBITMAP
        {
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = width;
            bitmapInfo.bmiHeader.biHeight = -height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            void* newBits = nullptr;
            CBitmap result(static_cast<HBITMAP>(
                CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &newBits, nullptr, 0)));
            if (!result || newBits == nullptr) return nullptr;

            const CClientDC dc(nullptr);
            if (dc.Handle() == nullptr ||
                GetDIBits(dc, hbmToAdd, 0, static_cast<UINT>(height), newBits,
                    &bitmapInfo, DIB_RGB_COLORS) != height)
                return nullptr;

            const COLORREF disabledText = GetSysColor(COLOR_GRAYTEXT);
            const BYTE disabledR = GetRValue(disabledText);
            const BYTE disabledG = GetGValue(disabledText);
            const BYTE disabledB = GetBValue(disabledText);
            auto* pixels = static_cast<RGBQUAD*>(newBits);
            const std::span pixelSpan(pixels, static_cast<size_t>(pixelCount));
            const bool sourceHasAlpha = bitmap.bmPlanes == 1 && bitmap.bmBitsPixel == 32 &&
                std::ranges::any_of(pixelSpan,
                    [](const RGBQUAD& pixel) { return pixel.rgbReserved != 0; });

            for (RGBQUAD& pixel : pixelSpan)
            {
                const BYTE alpha = sourceHasAlpha ? pixel.rgbReserved : 0xFF;

                // Grayscale using NTSC weights
                const BYTE gray = static_cast<BYTE>(
                    (pixel.rgbRed * 299 + pixel.rgbGreen * 587 + pixel.rgbBlue * 114) / 1000);
                pixel.rgbBlue = static_cast<BYTE>((disabledB * 3 + gray) / 4);
                pixel.rgbGreen = static_cast<BYTE>((disabledG * 3 + gray) / 4);
                pixel.rgbRed = static_cast<BYTE>((disabledR * 3 + gray) / 4);
                pixel.rgbReserved = static_cast<BYTE>((static_cast<int>(alpha) * 3) / 4);
            }

            return static_cast<HBITMAP>(result.Detach());
        }());
    const HBITMAP hbmDisabled = disabled.Handle() != nullptr ?
        static_cast<HBITMAP>(disabled.m_hObject) : hbmToAdd;
    const int disabledIndex = ImageList_Add(m_disabledImageList, hbmDisabled, nullptr);
    if (disabledIndex != index)
    {
        ImageList_Remove(m_imageList, index);
        if (disabledIndex >= 0) ImageList_Remove(m_disabledImageList, disabledIndex);
        return -1;
    }
    return index;
}
