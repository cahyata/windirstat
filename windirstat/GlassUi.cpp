// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "GlassUi.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
// The project targets Windows 7 headers; DirectComposition needs the Windows 8 API surface.
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <dcomp.h>
#pragma pop_macro("_WIN32_WINNT")
#include <wrl/client.h>

namespace
{
    using Microsoft::WRL::ComPtr;
    using D2D1::ColorF;

    constexpr UINT_PTR kTimer = 1;
    constexpr DWORD kExNoRedirectionBitmap = 0x00200000L; // WS_EX_NOREDIRECTIONBITMAP (Windows 8+)
    const ColorF kInk(0.11f, 0.11f, 0.13f);
    const ColorF kInkSoft(0.43f, 0.43f, 0.47f);

    using D2D1CreateFactoryFn = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    using DCompCreateDeviceFn = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);
    using DWriteCreateFactoryFn = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

    inline bool Inside(const D2D1_RECT_F& r, const float x, const float y)
    {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }

    // ---------------------------------------------------------------------------------------------
    //  Drawing context
    // ---------------------------------------------------------------------------------------------
    struct Gfx
    {
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1DeviceContext> dc;
        ComPtr<IDWriteFactory> dwrite;
        ComPtr<ID2D1SolidColorBrush> brush;
        ComPtr<ID2D1StrokeStyle> roundStroke;
        std::map<std::tuple<int, int, int>, ComPtr<IDWriteTextFormat>> formats;

        IDWriteTextFormat* Format(const float size, const DWRITE_FONT_WEIGHT weight, const DWRITE_TEXT_ALIGNMENT align)
        {
            const auto key = std::make_tuple(static_cast<int>(size * 10), static_cast<int>(weight), static_cast<int>(align));
            if (const auto found = formats.find(key); found != formats.end()) return found->second.Get();
            ComPtr<IDWriteTextFormat> format;
            if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format)) &&
                FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format))) return nullptr;
            format->SetTextAlignment(align);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            return formats.emplace(key, format).first->second.Get();
        }

        float Measure(const std::wstring& text, const float size)
        {
            IDWriteTextFormat* format = Format(size, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
            if (format == nullptr || text.empty()) return 0.0f;
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 10000.0f, 100.0f, &layout)))
                return 0.0f;
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            return metrics.widthIncludingTrailingWhitespace;
        }

        ComPtr<IDWriteTextLayout> WrapLayout(const std::wstring& text, const float size, const float width, const float height)
        {
            ComPtr<IDWriteTextFormat> format;
            if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format)) &&
                FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format))) return nullptr;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), width, height, &layout)))
                return nullptr;
            return layout;
        }

        float MeasureWrapped(const std::wstring& text, const float size, const float width)
        {
            const auto layout = WrapLayout(text, size, width, 10000.0f);
            if (!layout) return size * 1.4f;
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            return metrics.height;
        }

        void WrapText(const std::wstring& text, const D2D1_RECT_F& r, const float size, const ColorF& c)
        {
            const auto layout = WrapLayout(text, size, r.right - r.left, r.bottom - r.top);
            if (!layout) return;
            brush->SetColor(c);
            dc->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        ComPtr<IDWriteTextLayout> EditLayout(const std::wstring& text, const float size, const float height, const bool multi)
        {
            ComPtr<IDWriteTextFormat> format;
            if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format)) &&
                FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format))) return nullptr;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetParagraphAlignment(multi ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), 100000.0f, height, &layout)))
                return nullptr;
            return layout;
        }

        void Fill(const D2D1_RECT_F& r, const float radius, const ColorF& c)
        {
            brush->SetColor(c);
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
        }

        void Stroke(const D2D1_RECT_F& r, const float radius, const ColorF& c, const float width = 1.0f)
        {
            brush->SetColor(c);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get(), width);
        }

        void Gradient(const D2D1_RECT_F& r, const float radius, const ColorF& top, const ColorF& bottom)
        {
            const D2D1_GRADIENT_STOP stops[] = { { 0.0f, top }, { 1.0f, bottom } };
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> gradient;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &collection)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom)), collection.Get(), &gradient)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), gradient.Get());
        }

        // Specular rim: bright top-left, soft bottom-right
        void Rim(const D2D1_RECT_F& r, const float radius, const float strength = 1.0f)
        {
            const D2D1_GRADIENT_STOP stops[] = {
                { 0.0f, ColorF(1, 1, 1, 0.95f * strength) }, { 0.35f, ColorF(1, 1, 1, 0.35f * strength) },
                { 0.7f, ColorF(1, 1, 1, 0.2f * strength) }, { 1.0f, ColorF(1, 1, 1, 0.8f * strength) } };
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> gradient;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 4, &collection)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)), collection.Get(), &gradient)))
            {
                const D2D1_RECT_F inner = D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius), gradient.Get(), 1.2f);
            }
        }

        void Shadow(const D2D1_RECT_F& r, const float radius, const float strength = 1.0f)
        {
            for (int i = 10; i >= 1; --i)
            {
                const float f = 1.0f - static_cast<float>(i) / 11.0f;
                const D2D1_RECT_F g = D2D1::RectF(r.left - i, r.top - i + 3, r.right + i, r.bottom + i + 3);
                Stroke(g, radius + i, ColorF(0.1f, 0.12f, 0.22f, 0.07f * f * f * strength), 1.0f);
            }
        }

        // Frosted glass surface that lets the acrylic backdrop show through
        void Glass(const D2D1_RECT_F& r, const float radius)
        {
            Shadow(r, radius);
            Gradient(r, radius, ColorF(1, 1, 1, 0.62f), ColorF(0.96f, 0.97f, 1.0f, 0.38f));
            Stroke(D2D1::RectF(r.left - 0.5f, r.top - 0.5f, r.right + 0.5f, r.bottom + 0.5f), radius + 0.5f,
                ColorF(0.2f, 0.22f, 0.35f, 0.10f), 1.4f);
            Rim(r, radius);
        }

        void Text(const std::wstring& text, const D2D1_RECT_F& r, const float size, const ColorF& c,
            const DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
            const DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING)
        {
            IDWriteTextFormat* format = Format(size, weight, align);
            if (format == nullptr) return;
            brush->SetColor(c);
            dc->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, r, brush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    };

    // ---------------------------------------------------------------------------------------------
    //  Widgets
    // ---------------------------------------------------------------------------------------------
    struct GlassWindow;

    struct Widget
    {
        D2D1_RECT_F r{};
        bool interactive = false, hot = false, pressed = false, enabled = true, focused = false, visible = true;
        GlassWindow* host = nullptr;
        std::function<bool()> syncFn;
        float hover = 0, press = 0;
        virtual ~Widget() = default;
        virtual void Draw(Gfx& g) = 0;
        virtual void OnClick(float /*x*/, float /*y*/) {}
        virtual void OnMove(float /*x*/, float /*y*/) {}
        virtual bool Animated() { return false; }
        virtual void Wheel(float /*delta*/) {}
        virtual bool Sync() { return syncFn ? syncFn() : false; }  // pulls state from a mirrored control
        virtual void OnPress(float /*x*/, float /*y*/) {}
        virtual void OnDrag(float /*x*/, float /*y*/) {}
        virtual void OnRelease(float /*x*/, float /*y*/) {}
        virtual bool Focusable() const { return false; }
        virtual void OnChar(wchar_t) {}
        virtual bool OnKey(WPARAM /*vk*/, bool /*ctrl*/) { return false; }
        virtual void OnFocusChanged() {}
        bool Tick()
        {
            const float th = hot && enabled ? 1.0f : 0.0f, tp = pressed && enabled ? 1.0f : 0.0f;
            hover += (th - hover) * 0.28f;
            press += (tp - press) * 0.4f;
            if (std::fabs(th - hover) < 0.01f) hover = th;
            if (std::fabs(tp - press) < 0.01f) press = tp;
            return hover != th || press != tp || Animated();
        }
    };

    struct Panel final : Widget
    {
        void Draw(Gfx& g) override { g.Glass(r, 26.0f); }
    };

    struct Label final : Widget
    {
        std::wstring text;
        float size = 14;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        bool soft = false;
        void Draw(Gfx& g) override { g.Text(text, r, size, soft ? kInkSoft : kInk, weight); }
    };

    struct Button final : Widget
    {
        std::wstring text;
        bool primary = false, hasSwatch = false;
        ColorF swatch{ 0, 0, 0 };
        std::function<void()> action;
        Button() { interactive = true; }
        void OnClick(float, float) override { if (enabled && action) action(); }
        void Draw(Gfx& g) override
        {
            const float alpha = enabled ? 1.0f : 0.45f;
            const float scale = 1.0f + 0.03f * hover - 0.05f * press;
            const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2);
            g.dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, c));
            const float radius = (r.bottom - r.top) / 2;
            g.Shadow(r, radius, (0.8f + 0.5f * hover) * alpha);
            if (primary)
            {
                g.Gradient(r, radius, ColorF(0.30f + 0.1f * hover, 0.66f + 0.06f * hover, 1.0f, alpha),
                    ColorF(0.0f, 0.45f + 0.05f * hover, 0.95f, alpha));
                g.Rim(r, radius, 0.55f * alpha);
                g.Text(text, r, 14, ColorF(1, 1, 1, alpha), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            else
            {
                g.Gradient(r, radius, ColorF(1, 1, 1, 0.92f * alpha), ColorF(0.93f, 0.94f, 0.98f, (0.7f + 0.15f * hover) * alpha));
                g.Stroke(r, radius, ColorF(0.2f, 0.22f, 0.35f, 0.12f * alpha), 1.4f);
                g.Rim(r, radius, alpha);
                g.Text(text, r, 14, ColorF(kInk.r, kInk.g, kInk.b, alpha), DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
                if (hasSwatch)
                {
                    const D2D1_RECT_F chip = D2D1::RectF(r.left + 10, r.top + 8, r.left + 10 + (r.bottom - r.top - 16) * 1.6f, r.bottom - 8);
                    g.Fill(chip, (chip.bottom - chip.top) / 2, ColorF(swatch.r, swatch.g, swatch.b, alpha));
                    g.Stroke(chip, (chip.bottom - chip.top) / 2, ColorF(0, 0, 0, 0.18f), 1.0f);
                    g.Rim(chip, (chip.bottom - chip.top) / 2, 0.8f);
                }
            }
            g.dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    };

    struct Check final : Widget
    {
        std::wstring text;
        bool radio = false, checked = false;
        std::function<void()> changed;
        float on = 0;
        Check() { interactive = true; }
        void OnClick(float, float) override { if (enabled && changed) changed(); }
        bool Animated() override { return std::fabs((checked ? 1.0f : 0.0f) - on) > 0.01f; }
        void Draw(Gfx& g) override
        {
            const float alpha = enabled ? 1.0f : 0.5f;
            const float cy = (r.top + r.bottom) / 2;
            const D2D1_RECT_F box = D2D1::RectF(r.left, cy - 10, r.left + 20, cy + 10);
            on += ((checked ? 1.0f : 0.0f) - on) * 0.3f;
            const float radius = radio ? 10.0f : 6.0f;
            g.Shadow(box, radius, 0.35f * alpha);
            if (checked)
            {
                g.Gradient(box, radius, ColorF(0.30f, 0.66f, 1.0f, alpha), ColorF(0.0f, 0.45f, 0.95f, alpha));
                g.Rim(box, radius, 0.5f);
                g.brush->SetColor(ColorF(1, 1, 1, alpha));
                if (radio)
                    g.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(box.left + 10, cy), 4.0f * on, 4.0f * on), g.brush.Get());
                else
                {
                    const float x = box.left, y = cy;
                    g.dc->DrawLine(D2D1::Point2F(x + 5, y + 0.5f), D2D1::Point2F(x + 8.5f, y + 4), g.brush.Get(), 2.2f, g.roundStroke.Get());
                    g.dc->DrawLine(D2D1::Point2F(x + 8.5f, y + 4), D2D1::Point2F(x + 15, y - 4), g.brush.Get(), 2.2f, g.roundStroke.Get());
                }
            }
            else
            {
                g.Gradient(box, radius, ColorF(1, 1, 1, 0.95f * alpha), ColorF(0.9f, 0.92f, 0.97f, 0.7f * alpha));
                g.Stroke(box, radius, ColorF(0.2f, 0.22f, 0.35f, (0.22f + 0.15f * hover) * alpha), 1.3f);
                g.Rim(box, radius, 0.8f * alpha);
            }
            g.Text(text, D2D1::RectF(r.left + 30, r.top, r.right, r.bottom), 14, ColorF(kInk.r, kInk.g, kInk.b, alpha));
        }
    };

    // Text editor widget: single or multi line, caret, arrows, backspace/delete, paste, click to place the caret
    struct Field final : Widget
    {
        std::wstring placeholder, text;
        bool multiline = false;
        size_t caret = 0;
        float scrollX = 0, scrollY = 0;
        std::function<void()> changed;
        std::function<void()> dropdown; // editable combo box: chevron on the right opens the history
        Field() { interactive = true; }
        bool Focusable() const override { return true; }
        bool Animated() override { return focused; } // caret blink
        void OnFocusChanged() override { if (focused) caret = text.size(); }
        void Changed() { if (changed) changed(); }
        void Insert(const std::wstring& s)
        {
            text.insert(caret, s);
            caret += s.size();
            Changed();
        }
        void OnChar(const wchar_t ch) override
        {
            if (ch == 8) { if (caret > 0) { text.erase(caret - 1, 1); --caret; Changed(); } }
            else if (ch == 13 || ch == 10) { if (multiline) Insert(L"\n"); }
            else if (ch >= 32) Insert(std::wstring(1, ch));
        }
        void Paste()
        {
            if (!OpenClipboard(nullptr)) return;
            std::wstring pasted;
            if (const HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr)
                if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data)); chars != nullptr)
                {
                    for (const wchar_t* p = chars; *p != 0; ++p)
                    {
                        if (*p == L'\r') continue;
                        if (*p == L'\n' && !multiline) continue;
                        if (*p >= 32 || *p == L'\n') pasted.push_back(*p);
                    }
                    GlobalUnlock(data);
                }
            CloseClipboard();
            if (!pasted.empty()) Insert(pasted);
        }
        size_t LineStart(size_t pos) const { while (pos > 0 && text[pos - 1] != L'\n') --pos; return pos; }
        size_t LineEnd(size_t pos) const { while (pos < text.size() && text[pos] != L'\n') ++pos; return pos; }
        bool OnKey(const WPARAM vk, const bool ctrl) override
        {
            switch (vk)
            {
            case VK_LEFT: if (caret > 0) --caret; return true;
            case VK_RIGHT: if (caret < text.size()) ++caret; return true;
            case VK_HOME: caret = LineStart(caret); return true;
            case VK_END: caret = LineEnd(caret); return true;
            case VK_DELETE: if (caret < text.size()) { text.erase(caret, 1); Changed(); } return true;
            case VK_UP:
            case VK_DOWN:
                if (!multiline) return false;
                {
                    const size_t start = LineStart(caret), column = caret - start;
                    if (vk == VK_UP)
                    {
                        if (start == 0) caret = 0;
                        else { const size_t prev = LineStart(start - 1); caret = std::min(prev + column, start - 1); }
                    }
                    else
                    {
                        const size_t end = LineEnd(caret);
                        if (end >= text.size()) caret = text.size();
                        else { const size_t next = end + 1; caret = std::min(next + column, LineEnd(next)); }
                    }
                }
                return true;
            case VK_RETURN: return multiline;
            case 'V': if (ctrl) { Paste(); return true; } return false;
            default: return false;
            }
        }
        void OnClick(float x, float y) override;
        void Draw(Gfx& g) override
        {
            const float radius = multiline ? 18.0f : (r.bottom - r.top) / 2;
            g.Gradient(r, radius, ColorF(1, 1, 1, 0.82f), ColorF(0.95f, 0.96f, 1.0f, 0.62f));
            g.Stroke(r, radius, focused ? ColorF(0.04f, 0.52f, 1.0f, 0.75f) : ColorF(0.2f, 0.22f, 0.35f, 0.16f), focused ? 2.0f : 1.4f);
            g.Rim(r, radius, 0.6f);
            const D2D1_RECT_F inner = D2D1::RectF(r.left + 16, r.top + (multiline ? 10.0f : 0.0f), r.right - (dropdown ? 44.0f : 16.0f), r.bottom - (multiline ? 10.0f : 0.0f));
            if (dropdown)
                g.Text(L"⌄", D2D1::RectF(r.right - 40, r.top - 3, r.right - 12, r.bottom), 16, kInkSoft,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (text.empty() && !focused)
            {
                g.Text(placeholder, inner, 14, kInkSoft);
                return;
            }
            const auto layout = g.EditLayout(text, 14, inner.bottom - inner.top, multiline);
            if (!layout) return;
            caret = std::min(caret, text.size());
            float cx = 0, cy = 0, ch = 18;
            DWRITE_HIT_TEST_METRICS metrics{};
            layout->HitTestTextPosition(static_cast<UINT32>(caret), FALSE, &cx, &cy, &metrics);
            ch = metrics.height > 0 ? metrics.height : 18;
            // Keep the caret in view
            const float viewW = inner.right - inner.left - 4, viewH = inner.bottom - inner.top;
            if (cx - scrollX > viewW) scrollX = cx - viewW;
            if (cx < scrollX) scrollX = cx;
            if (multiline)
            {
                if (cy + ch - scrollY > viewH) scrollY = cy + ch - viewH;
                if (cy < scrollY) scrollY = cy;
            }
            else scrollY = 0;
            g.dc->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            g.brush->SetColor(kInk);
            g.dc->DrawTextLayout(D2D1::Point2F(inner.left - scrollX, inner.top - scrollY), layout.Get(), g.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (focused && (GetTickCount64() / 500) % 2 == 0)
            {
                const float x = inner.left - scrollX + cx, y = inner.top - scrollY + cy;
                g.brush->SetColor(ColorF(0.04f, 0.52f, 1.0f));
                g.dc->FillRectangle(D2D1::RectF(x, y + 1, x + 1.5f, y + ch - 1), g.brush.Get());
            }
            g.dc->PopAxisAlignedClip();
        }
    };

    struct WrapLabel final : Widget
    {
        std::wstring text;
        float size = 14.5f;
        void Draw(Gfx& g) override { g.WrapText(text, r, size, kInk); }
    };

    // Round status glyph: information, question, warning, error
    struct GlyphIcon final : Widget
    {
        int kind = 0;
        void Draw(Gfx& g) override
        {
            ColorF top(0.30f, 0.66f, 1.0f), bottom(0.0f, 0.45f, 0.95f);
            const wchar_t* glyph = L"i";
            if (kind == 1) glyph = L"?";
            if (kind == 2) { top = ColorF(1.0f, 0.78f, 0.30f); bottom = ColorF(1.0f, 0.58f, 0.0f); glyph = L"!"; }
            if (kind == 3) { top = ColorF(1.0f, 0.45f, 0.40f); bottom = ColorF(0.95f, 0.2f, 0.18f); glyph = L"\u00D7"; }
            const float radius = (r.bottom - r.top) / 2;
            g.Shadow(r, radius, 0.9f);
            g.Gradient(r, radius, top, bottom);
            g.Rim(r, radius, 0.6f);
            g.Text(glyph, r, 26, ColorF(1, 1, 1), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    };

    // Scrollable inset well of text lines (list of items inside a message)
    struct TextWell final : Widget
    {
        std::vector<std::wstring> rows;
        float scroll = 0;
        static constexpr float kRow = 30;
        TextWell() { interactive = true; }
        float MaxScroll() const { return std::max(0.0f, kRow * static_cast<float>(rows.size()) - (r.bottom - r.top - 12)); }
        void Wheel(const float delta) override { scroll = std::clamp(scroll - delta, 0.0f, MaxScroll()); }
        void Draw(Gfx& g) override
        {
            g.Gradient(r, 18, ColorF(1, 1, 1, 0.55f), ColorF(0.95f, 0.96f, 1.0f, 0.35f));
            g.Stroke(r, 18, ColorF(0.2f, 0.22f, 0.35f, 0.12f), 1.4f);
            g.Rim(r, 18, 0.7f);
            g.dc->PushAxisAlignedClip(D2D1::RectF(r.left + 4, r.top + 4, r.right - 4, r.bottom - 4), D2D1_ANTIALIAS_MODE_ALIASED);
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const float top = r.top + 8 + static_cast<float>(i) * kRow - scroll;
                g.Text(rows[i], D2D1::RectF(r.left + 16, top, r.right - 16, top + kRow), 13, kInk);
            }
            g.dc->PopAxisAlignedClip();
        }
    };

    struct DriveRow
    {
        std::wstring name, path;
        double total = 0, free = 0;
        bool checked = false, remote = false, subst = false;
    };

    struct DriveList final : Widget
    {
        std::vector<DriveRow> rows;
        int rowHot = -1;
        float scroll = 0;
        bool listEnabled = true; // false while "All Local Drives" is active
        std::function<void()> changed;
        DriveList() { interactive = true; }
        static constexpr float kHeader = 40, kRow = 40;
        float MaxScroll() const { return std::max(0.0f, kRow * static_cast<float>(rows.size()) - (r.bottom - r.top - kHeader - 6)); }
        D2D1_RECT_F RowRect(const int i) const
        {
            const float top = r.top + kHeader + static_cast<float>(i) * kRow - scroll;
            return D2D1::RectF(r.left + 6, top, r.right - 6, top + 36);
        }
        bool RowVisible(const D2D1_RECT_F& row) const { return row.top >= r.top + kHeader - 4 && row.bottom <= r.bottom - 2; }
        void OnMove(float x, float y) override
        {
            rowHot = -1;
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                if (RowVisible(RowRect(i)) && Inside(RowRect(i), x, y)) rowHot = i;
        }
        void OnClick(float x, float y) override
        {
            if (!listEnabled) return;
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                if (RowVisible(RowRect(i)) && Inside(RowRect(i), x, y)) { rows[i].checked = !rows[i].checked; if (changed) changed(); }
        }
        void Wheel(const float delta) override { scroll = std::clamp(scroll - delta, 0.0f, MaxScroll()); }
        static std::wstring Size(const double bytes)
        {
            wchar_t buffer[48]{};
            if (bytes <= 0) return L"-";
            swprintf_s(buffer, L"%.1f GiB", bytes / (1024.0 * 1024.0 * 1024.0));
            return buffer;
        }
        void Draw(Gfx& g) override
        {
            g.Gradient(r, 20, ColorF(1, 1, 1, 0.55f), ColorF(0.95f, 0.96f, 1.0f, 0.35f));
            g.Stroke(r, 20, ColorF(0.2f, 0.22f, 0.35f, 0.12f), 1.4f);
            g.Rim(r, 20, 0.7f);

            const float width = r.right - r.left;
            const D2D1_RECT_F head = D2D1::RectF(r.left + 6, r.top + 6, r.right - 6, r.top + 34);
            g.Fill(head, 14, ColorF(1, 1, 1, 0.55f));
            g.Text(L"Name", D2D1::RectF(head.left + 16, head.top, head.left + 180, head.bottom), 12, kInkSoft, DWRITE_FONT_WEIGHT_MEDIUM);
            g.Text(L"Total", D2D1::RectF(r.left + width - 330, head.top, r.left + width - 262, head.bottom), 12, kInkSoft, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
            g.Text(L"Free", D2D1::RectF(r.left + width - 254, head.top, r.left + width - 186, head.bottom), 12, kInkSoft, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
            g.Text(L"Used", D2D1::RectF(r.left + width - 170, head.top, r.right - 70, head.bottom), 12, kInkSoft, DWRITE_FONT_WEIGHT_MEDIUM);

            // Clip rows to the list body
            g.dc->PushAxisAlignedClip(D2D1::RectF(r.left + 2, r.top + kHeader - 4, r.right - 2, r.bottom - 4), D2D1_ANTIALIAS_MODE_ALIASED);
            const float dim = listEnabled ? 1.0f : 0.55f;
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            {
                const D2D1_RECT_F row = RowRect(i);
                if (row.bottom < r.top || row.top > r.bottom) continue;
                const bool sel = rows[i].checked || !listEnabled;
                if (sel) g.Gradient(row, 18, ColorF(0.30f, 0.66f, 1.0f, dim), ColorF(0.0f, 0.45f, 0.95f, dim));
                else if (i == rowHot) g.Fill(row, 18, ColorF(0.55f, 0.58f, 0.7f, 0.16f));
                const ColorF ink = sel ? ColorF(1, 1, 1) : kInk;
                const float used = rows[i].total > 0 ? static_cast<float>(1.0 - rows[i].free / rows[i].total) : 0.0f;
                g.Text(rows[i].name, D2D1::RectF(row.left + 16, row.top, row.left + 190, row.bottom), 14, ink);
                g.Text(Size(rows[i].total), D2D1::RectF(r.left + width - 340, row.top, r.left + width - 262, row.bottom), 13, ink, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
                g.Text(Size(rows[i].free), D2D1::RectF(r.left + width - 264, row.top, r.left + width - 186, row.bottom), 13, ink, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
                const D2D1_RECT_F track = D2D1::RectF(r.left + width - 170, row.top + 13, r.right - 70, row.top + 23);
                g.Fill(track, 5, sel ? ColorF(1, 1, 1, 0.35f) : ColorF(0.3f, 0.32f, 0.5f, 0.15f));
                D2D1_RECT_F fill = track;
                fill.right = track.left + std::max(10.0f, (track.right - track.left) * used);
                g.Gradient(fill, 5, sel ? ColorF(1, 1, 1, 0.95f) : ColorF(0.55f, 0.52f, 0.95f), sel ? ColorF(1, 1, 1, 0.75f) : ColorF(0.35f, 0.33f, 0.85f));
                wchar_t percent[16]{};
                if (rows[i].total > 0) swprintf_s(percent, L"%.0f%%", used * 100.0f);
                g.Text(percent, D2D1::RectF(r.right - 66, row.top, r.right - 12, row.bottom), 13, ink, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
            g.dc->PopAxisAlignedClip();
        }
    };

    // ---------------------------------------------------------------------------------------------
    //  Window
    // ---------------------------------------------------------------------------------------------
    // Dropdown list drawn above everything else in the window
    struct PopupList final : Widget
    {
        std::vector<std::wstring> items;
        int selected = -1, hotRow = -1;
        float scroll = 0;
        std::function<void(int)> chosen;
        static constexpr float kRow = 34;
        PopupList() { interactive = true; }
        float MaxScroll() const { return std::max(0.0f, kRow * static_cast<float>(items.size()) - (r.bottom - r.top - 12)); }
        int RowAt(const float y) const
        {
            const int row = static_cast<int>((y - r.top - 6 + scroll) / kRow);
            return row >= 0 && row < static_cast<int>(items.size()) ? row : -1;
        }
        void OnMove(float, float y) override { hotRow = RowAt(y); }
        void OnClick(float, float y) override { if (const int row = RowAt(y); row >= 0 && chosen) chosen(row); }
        void Wheel(const float delta) override { scroll = std::clamp(scroll - delta, 0.0f, MaxScroll()); }
        void Draw(Gfx& g) override
        {
            g.Shadow(r, 22, 1.4f);
            g.Gradient(r, 22, ColorF(0.99f, 0.99f, 1.0f, 0.96f), ColorF(0.94f, 0.95f, 0.98f, 0.94f));
            g.Stroke(r, 22, ColorF(0.2f, 0.22f, 0.35f, 0.14f), 1.4f);
            g.Rim(r, 22, 0.9f);
            g.dc->PushAxisAlignedClip(D2D1::RectF(r.left + 2, r.top + 4, r.right - 2, r.bottom - 4), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (size_t i = 0; i < items.size(); ++i)
            {
                const float top = r.top + 6 + static_cast<float>(i) * kRow - scroll;
                const D2D1_RECT_F row = D2D1::RectF(r.left + 8, top, r.right - 8, top + kRow - 2);
                const bool sel = static_cast<int>(i) == selected;
                if (static_cast<int>(i) == hotRow) g.Fill(row, 16, ColorF(0.55f, 0.58f, 0.7f, 0.18f));
                g.Text(items[i], D2D1::RectF(row.left + 14, row.top, row.right - 34, row.bottom), 14,
                    sel ? ColorF(0.04f, 0.52f, 1.0f) : kInk, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
                if (sel) g.Text(L"\u2713", D2D1::RectF(row.right - 34, row.top, row.right - 10, row.bottom), 14,
                    ColorF(0.04f, 0.52f, 1.0f), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            g.dc->PopAxisAlignedClip();
        }
    };

    // Dropdown selector (combo box face)
    struct Combo final : Widget
    {
        std::wstring text;
        std::function<void()> open;
        Combo() { interactive = true; }
        void OnClick(float, float) override { if (enabled && open) open(); }
        void Draw(Gfx& g) override
        {
            const float alpha = enabled ? 1.0f : 0.5f;
            const float radius = (r.bottom - r.top) / 2;
            g.Gradient(r, radius, ColorF(1, 1, 1, 0.9f * alpha), ColorF(0.93f, 0.94f, 0.98f, (0.62f + 0.14f * hover) * alpha));
            g.Stroke(r, radius, ColorF(0.2f, 0.22f, 0.35f, 0.14f * alpha), 1.4f);
            g.Rim(r, radius, 0.7f * alpha);
            g.Text(text, D2D1::RectF(r.left + 16, r.top, r.right - 40, r.bottom), 14, ColorF(kInk.r, kInk.g, kInk.b, alpha));
            g.Text(L"\u2304", D2D1::RectF(r.right - 40, r.top - 3, r.right - 12, r.bottom), 16, ColorF(kInkSoft.r, kInkSoft.g, kInkSoft.b, alpha),
                DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    };

    // Horizontal or vertical slider with a glass thumb
    struct Slider final : Widget
    {
        bool vertical = false;
        int minValue = 0, maxValue = 100, value = 0;
        std::function<void(int, bool)> changed; // (value, final)
        Slider() { interactive = true; }
        float Fraction() const { return maxValue > minValue ? static_cast<float>(value - minValue) / static_cast<float>(maxValue - minValue) : 0.0f; }
        int ValueAt(const float x, const float y) const
        {
            const float t = vertical ? (y - (r.top + 14)) / std::max(1.0f, r.bottom - r.top - 28) : (x - (r.left + 14)) / std::max(1.0f, r.right - r.left - 28);
            return minValue + static_cast<int>(std::lround(std::clamp(t, 0.0f, 1.0f) * static_cast<float>(maxValue - minValue)));
        }
        void Apply(const float x, const float y, const bool final)
        {
            if (!enabled) return;
            const int v = ValueAt(x, y);
            if (v != value || final) { value = v; if (changed) changed(value, final); }
        }
        void OnPress(float x, float y) override { Apply(x, y, false); }
        void OnDrag(float x, float y) override { Apply(x, y, false); }
        void OnRelease(float x, float y) override { Apply(x, y, true); }
        void Draw(Gfx& g) override
        {
            const float alpha = enabled ? 1.0f : 0.5f;
            const float t = Fraction();
            D2D1_RECT_F track, fill;
            D2D1_POINT_2F thumb;
            if (vertical)
            {
                const float cx = (r.left + r.right) / 2;
                track = D2D1::RectF(cx - 4, r.top + 14, cx + 4, r.bottom - 14);
                const float ty = track.top + (track.bottom - track.top) * t;
                fill = D2D1::RectF(track.left, track.top, track.right, ty);
                thumb = D2D1::Point2F(cx, ty);
            }
            else
            {
                const float cy = (r.top + r.bottom) / 2;
                track = D2D1::RectF(r.left + 14, cy - 4, r.right - 14, cy + 4);
                const float tx = track.left + (track.right - track.left) * t;
                fill = D2D1::RectF(track.left, track.top, tx, track.bottom);
                thumb = D2D1::Point2F(tx, cy);
            }
            g.Fill(track, 4, ColorF(0.3f, 0.32f, 0.5f, 0.2f * alpha));
            if (fill.right > fill.left && fill.bottom > fill.top) g.Gradient(fill, 4, ColorF(0.30f, 0.66f, 1.0f, alpha), ColorF(0.0f, 0.45f, 0.95f, alpha));
            const float scale = 1.0f + 0.12f * (std::max(hover, press));
            const D2D1_RECT_F knob = D2D1::RectF(thumb.x - 13 * scale, thumb.y - 13 * scale, thumb.x + 13 * scale, thumb.y + 13 * scale);
            g.Shadow(knob, 13 * scale, 0.9f * alpha);
            g.Gradient(knob, 13 * scale, ColorF(1, 1, 1, alpha), ColorF(0.9f, 0.92f, 0.97f, alpha));
            g.Stroke(knob, 13 * scale, ColorF(0.2f, 0.22f, 0.35f, 0.2f * alpha), 1.2f);
            g.Rim(knob, 13 * scale, alpha);
        }
    };

    // List of selectable text rows (mirrors a list box)
    struct ChoiceList final : Widget
    {
        std::vector<std::wstring> rows;
        int selected = -1, rowHot = -1;
        float scroll = 0;
        std::function<void(int)> chosen;
        static constexpr float kRow = 34;
        ChoiceList() { interactive = true; }
        float MaxScroll() const { return std::max(0.0f, kRow * static_cast<float>(rows.size()) - (r.bottom - r.top - 12)); }
        int RowAt(const float y) const
        {
            const int row = static_cast<int>((y - r.top - 6 + scroll) / kRow);
            return row >= 0 && row < static_cast<int>(rows.size()) ? row : -1;
        }
        void OnMove(float, float y) override { rowHot = RowAt(y); }
        void OnClick(float, float y) override { if (enabled) if (const int row = RowAt(y); row >= 0 && chosen) chosen(row); }
        void Wheel(const float delta) override { scroll = std::clamp(scroll - delta, 0.0f, MaxScroll()); }
        void Draw(Gfx& g) override
        {
            const float alpha = enabled ? 1.0f : 0.55f;
            g.Gradient(r, 18, ColorF(1, 1, 1, 0.55f * alpha), ColorF(0.95f, 0.96f, 1.0f, 0.35f * alpha));
            g.Stroke(r, 18, ColorF(0.2f, 0.22f, 0.35f, 0.12f), 1.4f);
            g.Rim(r, 18, 0.7f);
            g.dc->PushAxisAlignedClip(D2D1::RectF(r.left + 3, r.top + 4, r.right - 3, r.bottom - 4), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const float top = r.top + 6 + static_cast<float>(i) * kRow - scroll;
                const D2D1_RECT_F row = D2D1::RectF(r.left + 6, top, r.right - 6, top + kRow - 2);
                const bool sel = static_cast<int>(i) == selected;
                if (sel) g.Gradient(row, 16, ColorF(0.30f, 0.66f, 1.0f, alpha), ColorF(0.0f, 0.45f, 0.95f, alpha));
                else if (static_cast<int>(i) == rowHot) g.Fill(row, 16, ColorF(0.55f, 0.58f, 0.7f, 0.16f));
                g.Text(rows[i], D2D1::RectF(row.left + 14, row.top, row.right - 10, row.bottom), 14, sel ? ColorF(1, 1, 1) : kInk);
            }
            g.dc->PopAxisAlignedClip();
        }
    };

    // Tab strip: glass capsule with a darker selected capsule
    struct TabBar final : Widget
    {
        std::vector<std::wstring> tabs;
        std::vector<D2D1_RECT_F> rects;
        int selected = 0;
        std::function<void(int)> chosen;
        TabBar() { interactive = true; }
        void OnClick(float x, float y) override
        {
            for (size_t i = 0; i < rects.size(); ++i)
                if (Inside(rects[i], x, y) && chosen) { chosen(static_cast<int>(i)); return; }
        }
        void Draw(Gfx& g) override
        {
            const float radius = (r.bottom - r.top) / 2;
            g.Shadow(r, radius, 0.7f);
            g.Gradient(r, radius, ColorF(1, 1, 1, 0.75f), ColorF(0.94f, 0.95f, 0.99f, 0.5f));
            g.Stroke(r, radius, ColorF(0.2f, 0.22f, 0.35f, 0.1f), 1.4f);
            g.Rim(r, radius, 0.9f);
            std::vector<float> widths;
            float total = 0;
            for (const auto& tab : tabs) { widths.push_back(g.Measure(tab, 14) + 30); total += widths.back(); }
            const float scale = std::min(1.0f, (r.right - r.left - 16) / std::max(1.0f, total));
            rects.clear();
            float x = r.left + 8;
            for (size_t i = 0; i < tabs.size(); ++i)
            {
                const float w = widths[i] * scale;
                const D2D1_RECT_F tab = D2D1::RectF(x, r.top + 5, x + w, r.bottom - 5);
                rects.push_back(tab);
                const bool sel = static_cast<int>(i) == selected;
                if (sel) g.Gradient(tab, (tab.bottom - tab.top) / 2, ColorF(0.82f, 0.82f, 0.86f, 0.95f), ColorF(0.77f, 0.77f, 0.82f, 0.95f));
                g.Text(tabs[i], tab, 14, sel ? ColorF(0.04f, 0.52f, 1.0f) : kInkSoft, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
                x += w;
            }
        }
    };

    // Live picture of a custom-painted control (e.g. the treemap preview), re-captured a few times per second
    struct PixelMirror final : Widget
    {
        HWND real = nullptr;
        ComPtr<ID2D1Bitmap1> bitmap;
        ULONGLONG lastCapture = 0;
        bool Animated() override { return true; }
        void Capture(Gfx& g)
        {
            RECT rc{};
            if (!GetClientRect(real, &rc) || rc.right < 2 || rc.bottom < 2) return;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = rc.right;
            info.bmiHeader.biHeight = -rc.bottom;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            const HDC screen = GetDC(nullptr);
            const HDC mem = CreateCompatibleDC(screen);
            const HBITMAP dib = CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (dib != nullptr && bits != nullptr)
            {
                const HGDIOBJ old = SelectObject(mem, dib);
                PatBlt(mem, 0, 0, rc.right, rc.bottom, WHITENESS);
                // Repaint the control, then copy its own surface (works for hidden windows too)
                InvalidateRect(real, nullptr, TRUE);
                UpdateWindow(real);
                if (const HDC src = GetDC(real); src != nullptr)
                {
                    BitBlt(mem, 0, 0, rc.right, rc.bottom, src, 0, 0, SRCCOPY | CAPTUREBLT);
                    ReleaseDC(real, src);
                }
                GdiFlush();
                const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
                bitmap.Reset();
                g.dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)), bits,
                    static_cast<UINT32>(rc.right) * 4, &props, &bitmap);
                SelectObject(mem, old);
            }
            if (dib != nullptr) DeleteObject(dib);
            DeleteDC(mem);
            ReleaseDC(nullptr, screen);
        }
        void Draw(Gfx& g) override
        {
            if (!bitmap || GetTickCount64() - lastCapture > 300) { lastCapture = GetTickCount64(); Capture(g); }
            g.Shadow(r, 20, 0.6f);
            g.Fill(r, 20, ColorF(1, 1, 1, 0.5f));
            if (bitmap)
            {
                ComPtr<ID2D1RoundedRectangleGeometry> clip;
                if (SUCCEEDED(g.factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, 20, 20), &clip)))
                {
                    g.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.Get()), nullptr);
                    g.dc->DrawBitmap(bitmap.Get(), r, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
                    g.dc->PopLayer();
                }
            }
            g.Stroke(r, 20, ColorF(0.2f, 0.22f, 0.35f, 0.14f), 1.4f);
            g.Rim(r, 20, 0.8f);
        }
    };

    // Two-axis pad (light source direction)
    struct XYPad final : Widget
    {
        int x = 0, y = 0;                 // external position, -range..range
        static constexpr int kRange = 400;
        std::function<void(int, int)> changed;
        XYPad() { interactive = true; }
        void Apply(const float px, const float py)
        {
            if (!enabled) return;
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            const float hw = (r.right - r.left) / 2 - 16, hh = (r.bottom - r.top) / 2 - 16;
            x = static_cast<int>(std::lround(std::clamp((px - cx) / hw, -1.0f, 1.0f) * kRange));
            y = static_cast<int>(std::lround(std::clamp((py - cy) / hh, -1.0f, 1.0f) * kRange));
            if (changed) changed(x, y);
        }
        void OnPress(float px, float py) override { Apply(px, py); }
        void OnDrag(float px, float py) override { Apply(px, py); }
        void Draw(Gfx& g) override
        {
            const float alpha = enabled ? 1.0f : 0.5f;
            g.Gradient(r, 22, ColorF(1, 1, 1, 0.6f * alpha), ColorF(0.93f, 0.95f, 1.0f, 0.38f * alpha));
            g.Stroke(r, 22, ColorF(0.2f, 0.22f, 0.35f, 0.14f), 1.4f);
            g.Rim(r, 22, 0.8f);
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            g.brush->SetColor(ColorF(0.3f, 0.32f, 0.5f, 0.18f));
            g.dc->DrawLine(D2D1::Point2F(r.left + 14, cy), D2D1::Point2F(r.right - 14, cy), g.brush.Get(), 1.0f);
            g.dc->DrawLine(D2D1::Point2F(cx, r.top + 14), D2D1::Point2F(cx, r.bottom - 14), g.brush.Get(), 1.0f);
            const float hw = (r.right - r.left) / 2 - 16, hh = (r.bottom - r.top) / 2 - 16;
            const float tx = cx + static_cast<float>(x) / kRange * hw, ty = cy + static_cast<float>(y) / kRange * hh;
            const float scale = 1.0f + 0.12f * std::max(hover, press);
            const D2D1_RECT_F knob = D2D1::RectF(tx - 11 * scale, ty - 11 * scale, tx + 11 * scale, ty + 11 * scale);
            g.Shadow(knob, 11 * scale, 0.9f * alpha);
            g.Gradient(knob, 11 * scale, ColorF(0.30f, 0.66f, 1.0f, alpha), ColorF(0.0f, 0.45f, 0.95f, alpha));
            g.Rim(knob, 11 * scale, 0.7f);
        }
    };

    // Titled group card behind a block of related controls
    // Determinate / marquee progress bar (mirrors a progress control)
    struct ProgressBar final : Widget
    {
        int minValue = 0, maxValue = 100, value = 0;
        bool marquee = false;
        bool Animated() override { return marquee; }
        void Draw(Gfx& g) override
        {
            const float alpha = enabled ? 1.0f : 0.5f;
            const float cy = (r.top + r.bottom) / 2, h = std::min(14.0f, r.bottom - r.top);
            const D2D1_RECT_F track = D2D1::RectF(r.left, cy - h / 2, r.right, cy + h / 2);
            g.Fill(track, h / 2, ColorF(0.3f, 0.32f, 0.5f, 0.2f * alpha));
            float from = track.left, to = track.left;
            if (marquee)
            {
                const float cycle = static_cast<float>(GetTickCount64() % 1500) / 1500.0f;
                const float block = (track.right - track.left) / 4;
                from = std::clamp(track.left + cycle * (track.right - track.left + block) - block, track.left, track.right);
                to = std::clamp(track.left + cycle * (track.right - track.left + block), track.left, track.right);
            }
            else if (maxValue > minValue)
                to = track.left + (track.right - track.left) * std::clamp(static_cast<float>(value - minValue) / static_cast<float>(maxValue - minValue), 0.0f, 1.0f);
            if (to - from >= 1.0f)
                g.Gradient(D2D1::RectF(from, track.top, std::max(to, from + h), track.bottom), h / 2,
                    ColorF(0.30f, 0.66f, 1.0f, alpha), ColorF(0.0f, 0.45f, 0.95f, alpha));
        }
    };

    struct GroupCard final : Widget
    {
        std::wstring text;
        void Draw(Gfx& g) override
        {
            g.Gradient(r, 22, ColorF(1, 1, 1, 0.42f), ColorF(0.95f, 0.96f, 1.0f, 0.22f));
            g.Stroke(r, 22, ColorF(0.2f, 0.22f, 0.35f, 0.1f), 1.4f);
            g.Rim(r, 22, 0.6f);
            if (!text.empty()) g.Text(text, D2D1::RectF(r.left + 20, r.top + 8, r.right - 20, r.top + 32), 13, kInkSoft, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
    };

    struct SelectModel
    {
        int radio = 0;
        bool accepted = false;
        std::wstring result;
    };

    struct GlassWindow
    {
        HWND hwnd{};
        HWND owner{};
        float dpi = 96;
        int pxWidth = 0, pxHeight = 0;
        bool dirty = true, quitOnClose = false, closed = false;
        std::vector<std::unique_ptr<Widget>> widgets;
        Widget* hotWidget = nullptr;
        Widget* pressedWidget = nullptr;
        Widget* focusWidget = nullptr;
        std::unique_ptr<PopupList> popup;
        std::function<void()> escapeAction;
        std::function<bool()> frameSync;
        ULONGLONG shownAt = 0;
        bool activated = false;
        std::function<void()> defaultAction;
        std::wstring title = L"WinDirStat";
        std::function<D2D1_SIZE_F()> build;
        int resultId = 0, escapeId = 0;
        Widget* wheelTarget = nullptr;
        HWND centerOn = nullptr;
        SelectModel model;
        Button* okButton = nullptr;
        Field* folderField = nullptr;
        DriveList* driveList = nullptr;
        std::array<Check*, 3> radios{};
        Check* dupesCheck = nullptr;
        Check* fastCheck = nullptr;

        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGISwapChain1> swap;
        ComPtr<IDCompositionDevice> dcomp;
        ComPtr<IDCompositionTarget> target;
        ComPtr<IDCompositionVisual> visual;
        ComPtr<ID2D1Bitmap1> targetBitmap;
        Gfx gfx;

        template <class T> T* Add(const float x, const float y, const float w, const float h)
        {
            auto widget = std::make_unique<T>();
            widget->r = D2D1::RectF(x, y, x + w, y + h);
            T* raw = widget.get();
            raw->host = this;
            widgets.push_back(std::move(widget));
            return raw;
        }

        bool Init();
        bool BindTarget();
        void Resize();
        void Render();
        D2D1_SIZE_F BuildSelect();
        void UpdateOk();
        void SetRadio(int radio);
        void Commit();
        void Close();
        Widget* HitTest(float x, float y);
        void SetFocus(Widget* widget);
        void OpenPopup(const D2D1_RECT_F& anchor, std::vector<std::wstring> items, int selected, std::function<void(int)> chosen);
        void ClosePopup() { popup.reset(); dirty = true; }
        float ToDip(const int px) const { return static_cast<float>(px) * 96.0f / dpi; }
    };

    bool GlassWindow::Init()
    {
        const HMODULE d3dModule = LoadLibraryW(L"d3d11.dll"), d2dModule = LoadLibraryW(L"d2d1.dll");
        const HMODULE dcompModule = LoadLibraryW(L"dcomp.dll"), dwriteModule = LoadLibraryW(L"dwrite.dll");
        if (!d3dModule || !d2dModule || !dcompModule || !dwriteModule) return false;
        const auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3dModule, "D3D11CreateDevice"));
        const auto createFactory = reinterpret_cast<D2D1CreateFactoryFn>(GetProcAddress(d2dModule, "D2D1CreateFactory"));
        const auto createDcomp = reinterpret_cast<DCompCreateDeviceFn>(GetProcAddress(dcompModule, "DCompositionCreateDevice"));
        const auto createDwrite = reinterpret_cast<DWriteCreateFactoryFn>(GetProcAddress(dwriteModule, "DWriteCreateFactory"));
        if (!createDevice || !createFactory || !createDcomp || !createDwrite) return false;

        if (FAILED(createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) || FAILED(d3d.As(&dxgi))) return false;
        if (FAILED(createFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
            reinterpret_cast<void**>(gfx.factory.GetAddressOf())))) return false;
        ComPtr<ID2D1Device> d2dDevice;
        if (FAILED(gfx.factory->CreateDevice(dxgi.Get(), &d2dDevice)) ||
            FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &gfx.dc))) return false;
        if (FAILED(createDwrite(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(gfx.dwrite.GetAddressOf())))) return false;
        if (FAILED(gfx.dc->CreateSolidColorBrush(ColorF(0, 0, 0), &gfx.brush))) return false;
        gfx.factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &gfx.roundStroke);

        if (FAILED(createDcomp(dxgi.Get(), __uuidof(IDCompositionDevice), reinterpret_cast<void**>(dcomp.GetAddressOf()))) ||
            FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) || FAILED(dcomp->CreateVisual(&visual))) return false;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        pxWidth = std::max<LONG>(1, rc.right);
        pxHeight = std::max<LONG>(1, rc.bottom);
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(dxgiFactory.GetAddressOf())))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(pxWidth);
        desc.Height = static_cast<UINT>(pxHeight);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(dxgiFactory->CreateSwapChainForComposition(d3d.Get(), &desc, nullptr, &swap)) ||
            FAILED(visual->SetContent(swap.Get())) || FAILED(target->SetRoot(visual.Get())) || FAILED(dcomp->Commit())) return false;
        return BindTarget();
    }

    bool GlassWindow::BindTarget()
    {
        gfx.dc->SetTarget(nullptr);
        targetBitmap.Reset();
        ComPtr<IDXGISurface> surface;
        if (FAILED(swap->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf())))) return false;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (FAILED(gfx.dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &targetBitmap))) return false;
        gfx.dc->SetTarget(targetBitmap.Get());
        gfx.dc->SetDpi(dpi, dpi);
        return true;
    }

    void GlassWindow::Resize()
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0 || !swap) return;
        pxWidth = rc.right;
        pxHeight = rc.bottom;
        gfx.dc->SetTarget(nullptr);
        targetBitmap.Reset();
        if (SUCCEEDED(swap->ResizeBuffers(0, static_cast<UINT>(pxWidth), static_cast<UINT>(pxHeight), DXGI_FORMAT_UNKNOWN, 0)))
            BindTarget();
        dirty = true;
    }

    Widget* GlassWindow::HitTest(const float x, const float y)
    {
        for (auto it = widgets.rbegin(); it != widgets.rend(); ++it)
            if ((*it)->interactive && (*it)->visible && Inside((*it)->r, x, y)) return it->get();
        return nullptr;
    }

    void GlassWindow::SetFocus(Widget* widget)
    {
        if (focusWidget == widget) return;
        if (focusWidget) { focusWidget->focused = false; focusWidget->OnFocusChanged(); }
        focusWidget = widget;
        if (focusWidget) { focusWidget->focused = true; focusWidget->OnFocusChanged(); }
        dirty = true;
    }

    void GlassWindow::OpenPopup(const D2D1_RECT_F& anchor, std::vector<std::wstring> items, const int selected, std::function<void(int)> chosen)
    {
        auto list = std::make_unique<PopupList>();
        list->host = this;
        list->items = std::move(items);
        list->selected = selected;
        list->chosen = std::move(chosen);
        const float clientH = ToDip(pxHeight), clientW = ToDip(pxWidth);
        const float height = std::min(static_cast<float>(list->items.size()), 8.0f) * PopupList::kRow + 14;
        const float width = std::max(anchor.right - anchor.left, 180.0f);
        float top = anchor.bottom + 4;
        if (top + height > clientH - 6) top = std::max(6.0f, anchor.top - 4 - height);
        const float left = std::min(anchor.left, clientW - width - 6);
        list->r = D2D1::RectF(left, top, left + width, top + height);
        if (selected >= 0)
            list->scroll = std::clamp(static_cast<float>(selected) * PopupList::kRow - height / 2, 0.0f, list->MaxScroll());
        popup = std::move(list);
        dirty = true;
    }

    void GlassWindow::Render()
    {
        if (!swap || !gfx.dc || closed) return;
        if (!activated && shownAt != 0 && GetTickCount64() - shownAt > 250)
        {
            activated = true;
            SetForegroundWindow(hwnd);
        }
        bool animating = false;
        for (auto& widget : widgets)
        {
            if (widget->Sync()) dirty = true;
            if (widget->visible) animating |= widget->Tick();
        }
        if (frameSync && frameSync()) dirty = true;
        if (popup) animating |= popup->Tick();
        if (!dirty && !animating) return;
        dirty = animating;
        gfx.dc->BeginDraw();
        gfx.dc->Clear(ColorF(0, 0, 0, 0));
        for (auto& widget : widgets) if (widget->visible) widget->Draw(gfx);
        if (popup) popup->Draw(gfx);
        if (FAILED(gfx.dc->EndDraw())) return;
        swap->Present(0, 0);
    }

    void Field::OnClick(const float x, const float y)
    {
        if (host == nullptr) return;
        if (dropdown && x > r.right - 44) { dropdown(); return; }
        const D2D1_RECT_F inner = D2D1::RectF(r.left + 16, r.top + (multiline ? 10.0f : 0.0f), r.right - (dropdown ? 44.0f : 16.0f), r.bottom - (multiline ? 10.0f : 0.0f));
        const auto layout = host->gfx.EditLayout(text, 14, inner.bottom - inner.top, multiline);
        if (!layout) return;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        layout->HitTestPoint(x - inner.left + scrollX, y - inner.top + scrollY, &trailing, &inside, &metrics);
        caret = std::min(text.size(), static_cast<size_t>(metrics.textPosition) + (trailing ? 1 : 0));
    }

    void GlassWindow::Close()
    {
        if (closed) return;
        DestroyWindow(hwnd);
    }

    void GlassWindow::UpdateOk()
    {
        bool ok = false;
        if (model.radio == 0) ok = !driveList->rows.empty();
        else if (model.radio == 1) ok = std::ranges::any_of(driveList->rows, [](const DriveRow& row) { return row.checked; });
        else ok = !folderField->text.empty();
        okButton->enabled = ok;
        driveList->listEnabled = model.radio == 1;
        dirty = true;
    }

    void GlassWindow::SetRadio(const int radio)
    {
        model.radio = radio;
        for (int i = 0; i < 3; ++i) radios[i]->checked = i == radio;
        UpdateOk();
    }

    // Mirrors CSelectDrivesDlg::SaveSelection so the saved options behave identically.
    void GlassWindow::Commit()
    {
        if (!okButton->enabled) return;
        std::vector<std::wstring> drives, selected;
        std::wstring folders;
        if (model.radio == 2)
        {
            const auto normalized = NormalizeScanPaths(folderField->text);
            if (normalized.empty()) return;
            folders = JoinString(normalized);
            for (const auto& folder : normalized | std::views::reverse)
            {
                if (StorageSource::IsPath(folder)) continue;
                auto& history = COptions::SelectDrivesFolder.Obj();
                std::erase_if(history, [&folder](const std::wstring& previous) {
                    return _wcsicmp(previous.c_str(), folder.c_str()) == 0;
                });
                history.insert(history.begin(), folder);
            }
            std::erase_if(COptions::SelectDrivesFolder.Obj(), StorageSource::IsPath);
            COptions::SelectDrivesFolder.Obj().resize(std::min(static_cast<size_t>(COptions::FolderHistoryCount),
                COptions::SelectDrivesFolder.Obj().size()));
        }
        for (const auto& row : driveList->rows)
        {
            if (row.checked) selected.push_back(row.path);
            if ((model.radio == 0 && !row.remote && !row.subst) || (model.radio == 1 && row.checked)) drives.push_back(row.path);
        }
        COptions::SelectDrivesRadio = model.radio;
        COptions::SelectDrivesDrives = selected;
        COptions::ScanForDuplicates = dupesCheck->checked;
        COptions::UseFastScanEngine = fastCheck->checked;
        model.result = model.radio == 2 ? folders : JoinString(drives);
        model.accepted = true;
        Close();
    }

    D2D1_SIZE_F GlassWindow::BuildSelect()
    {
        auto* title = Add<Label>(28, 18, 504, 36);
        title->text = L"Select Drives";
        title->size = 26;
        title->weight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
        auto* subtitle = Add<Label>(28, 52, 504, 22);
        subtitle->text = L"Choose what WinDirStat should scan";
        subtitle->size = 13;
        subtitle->soft = true;

        Add<Panel>(16, 86, 528, 478);

        const wchar_t* radioText[3] = { L"All Local Drives", L"Individual Drives", L"Individual Folders" };
        const float radioY[3] = { 102, 132, 392 };
        for (int i = 0; i < 3; ++i)
        {
            radios[i] = Add<Check>(40, radioY[i], 480, 28);
            radios[i]->text = radioText[i];
            radios[i]->radio = true;
            radios[i]->changed = [this, i] { SetRadio(i); };
        }

        driveList = Add<DriveList>(40, 166, 480, 218);
        driveList->changed = [this] { UpdateOk(); };
        const DWORD mask = GetLogicalDrives();
        const auto& previous = COptions::SelectDrivesDrives.Obj();
        for (int letter = 0; letter < 26; ++letter)
        {
            if ((mask & (1u << letter)) == 0) continue;
            const std::wstring device = std::wstring(1, static_cast<wchar_t>(L'A' + letter)) + L":";
            const std::wstring root = device + L"\\";
            const UINT type = GetDriveTypeW(root.c_str());
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
            DriveRow row;
            row.path = root;
            row.remote = type == DRIVE_REMOTE;
            wchar_t mapped[MAX_PATH]{};
            row.subst = QueryDosDeviceW(device.c_str(), mapped, MAX_PATH) != 0 && wcsncmp(mapped, L"\\??\\", 4) == 0;
            wchar_t label[64]{};
            GetVolumeInformationW(root.c_str(), label, 64, nullptr, nullptr, nullptr, nullptr, 0);
            if (!row.remote)
            {
                ULARGE_INTEGER freeBytes{}, totalBytes{}, totalFree{};
                if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &totalBytes, &totalFree))
                {
                    row.total = static_cast<double>(totalBytes.QuadPart);
                    row.free = static_cast<double>(freeBytes.QuadPart);
                }
                else if (type == DRIVE_REMOVABLE) continue; // no media
            }
            row.name = std::wstring(label[0] ? label : L"Local Disk") + L" (" + device + L")";
            row.checked = std::ranges::any_of(previous, [&root](const std::wstring& p) { return _wcsicmp(p.c_str(), root.c_str()) == 0; });
            driveList->rows.push_back(std::move(row));
        }

        folderField = Add<Field>(40, 424, 300, 40);
        folderField->placeholder = L"Folders to scan (use | between paths)...";
        folderField->changed = [this] { if (model.radio != 2) SetRadio(2); else UpdateOk(); };
        const auto browse = [this](const bool append)
        {
            const auto folders = CDialog::PickFolders(WindowRef(hwnd));
            if (folders.empty()) return;
            const std::wstring selected = JoinString(folders);
            const std::wstring current = folderField->text;
            folderField->text = JoinString(NormalizeScanPaths(
                append && !current.empty() ? current + wds::chrPipe + selected : selected));
            SetRadio(2);
        };
        auto* browseButton = Add<Button>(352, 424, 82, 40);
        browseButton->text = L"Browse";
        browseButton->action = [browse] { browse(false); };
        auto* addButton = Add<Button>(442, 424, 78, 40);
        addButton->text = L"Add";
        addButton->action = [browse] { browse(true); };

        dupesCheck = Add<Check>(40, 480, 480, 28);
        dupesCheck->text = L"Scan for duplicate files (impacts performance)";
        dupesCheck->checked = COptions::ScanForDuplicates;
        dupesCheck->changed = [this] { dupesCheck->checked = !dupesCheck->checked; dirty = true; };
        fastCheck = Add<Check>(40, 512, 480, 28);
        fastCheck->text = IsElevationActive() ? L"Use accelerated scanning" : L"Use accelerated scanning (inactive - elevation required)";
        fastCheck->checked = COptions::UseFastScanEngine;
        fastCheck->changed = [this] { fastCheck->checked = !fastCheck->checked; dirty = true; };

        okButton = Add<Button>(298, 588, 110, 42);
        okButton->text = L"OK";
        okButton->primary = true;
        okButton->action = [this] { Commit(); };
        auto* cancel = Add<Button>(420, 588, 124, 42);
        cancel->text = L"Cancel";
        cancel->action = [this] { Close(); };
        defaultAction = [this] { Commit(); };

        folderField->text = COptions::SelectDrivesFolder.Obj().empty() ? std::wstring{} : COptions::SelectDrivesFolder.Obj().front();
        SetRadio(std::clamp<int>(COptions::SelectDrivesRadio, 0, 2));
        wheelTarget = driveList;
        return D2D1::SizeF(560, 654);
    }

    LRESULT CALLBACK GlassProc(const HWND hwnd, const UINT message, const WPARAM wParam, const LPARAM lParam)
    {
        auto* window = reinterpret_cast<GlassWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message)
        {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            if (window) { window->dirty = true; window->Render(); }
            return 0;
        }
        case WM_SIZE:
            if (window) { window->Resize(); window->Render(); }
            return 0;
        case WM_TIMER:
            if (window && wParam == kTimer) window->Render();
            return 0;
        case WM_USER + 1:
            SetForegroundWindow(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (window)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&track);
                const float x = window->ToDip(GET_X_LPARAM(lParam)), y = window->ToDip(GET_Y_LPARAM(lParam));
                if (window->popup)
                {
                    if (Inside(window->popup->r, x, y)) window->popup->OnMove(x, y);
                    else window->popup->hotRow = -1;
                    window->dirty = true;
                    return 0;
                }
                if (window->pressedWidget) window->pressedWidget->OnDrag(x, y);
                Widget* hit = window->HitTest(x, y);
                if (hit != window->hotWidget)
                {
                    if (window->hotWidget) window->hotWidget->hot = false;
                    window->hotWidget = hit;
                    if (hit) hit->hot = true;
                }
                if (hit) hit->OnMove(x, y);
                window->dirty = true;
            }
            return 0;
        case WM_MOUSELEAVE:
            if (window)
            {
                if (window->hotWidget) window->hotWidget->hot = false;
                window->hotWidget = nullptr;
                window->dirty = true;
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (window)
            {
                const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / 3.0f;
                POINT cursor{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &cursor);
                const float x = window->ToDip(cursor.x), y = window->ToDip(cursor.y);
                if (window->popup) window->popup->Wheel(delta);
                else if (Widget* hit = window->HitTest(x, y); hit != nullptr) hit->Wheel(delta);
                else if (window->wheelTarget) window->wheelTarget->Wheel(delta);
                window->dirty = true;
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (window)
            {
                const float x = window->ToDip(GET_X_LPARAM(lParam)), y = window->ToDip(GET_Y_LPARAM(lParam));
                if (window->popup) return 0; // handled on release
                Widget* hit = window->HitTest(x, y);
                window->SetFocus(hit != nullptr && hit->Focusable() ? hit : nullptr);
                if (hit) { hit->pressed = true; window->pressedWidget = hit; SetCapture(hwnd); hit->OnPress(x, y); }
                window->dirty = true;
            }
            return 0;
        case WM_LBUTTONUP:
            if (window)
            {
                const float x = window->ToDip(GET_X_LPARAM(lParam)), y = window->ToDip(GET_Y_LPARAM(lParam));
                if (window->popup)
                {
                    const bool inside = Inside(window->popup->r, x, y);
                    auto popup = std::move(window->popup);
                    if (inside) popup->OnClick(x, y);
                    window->dirty = true;
                    return 0;
                }
                ReleaseCapture();
                Widget* pressedWidget = window->pressedWidget;
                window->pressedWidget = nullptr;
                if (pressedWidget)
                {
                    pressedWidget->pressed = false;
                    pressedWidget->OnRelease(x, y);
                    if (Inside(pressedWidget->r, x, y)) pressedWidget->OnClick(x, y);
                }
                window->dirty = true;
            }
            return 0;
        case WM_CHAR:
            if (window && window->focusWidget && wParam != 22 /* Ctrl+V is handled on key down */ && wParam != 27)
            {
                window->focusWidget->OnChar(static_cast<wchar_t>(wParam));
                window->dirty = true;
            }
            return 0;
        case WM_KEYDOWN:
            if (!window) return 0;
            window->dirty = true;
            if (wParam == VK_ESCAPE)
            {
                if (window->popup) window->ClosePopup();
                else if (window->escapeAction) window->escapeAction();
                else { window->resultId = window->escapeId; window->Close(); }
            }
            else if (window->focusWidget && window->focusWidget->OnKey(wParam, (GetKeyState(VK_CONTROL) & 0x8000) != 0)) {}
            else if (wParam == VK_RETURN && window->defaultAction) window->defaultAction();
            return 0;
        case WM_CLOSE:
            if (window && window->escapeAction) { window->escapeAction(); return 0; }
            if (window) window->resultId = window->escapeId;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (window)
            {
                window->closed = true;
                KillTimer(hwnd, kTimer);
                if (window->quitOnClose) PostQuitMessage(0);
            }
            return 0;
        default: break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // Creates and shows the window. When `runLoop` is set it also pumps messages until the window is closed.
    // Returns false if GPU composition is unavailable.
    bool RunWindow(GlassWindow& window, const HWND owner, const bool runLoop = true)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = GlassProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"WdsGlassUiWindow";
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDR_MAINFRAME));
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);

        window.owner = owner;
        if (owner != nullptr) window.dpi = static_cast<float>(GetWindowDpi(owner));
        else
        {
            const HDC screen = GetDC(nullptr);
            window.dpi = static_cast<float>(GetDeviceCaps(screen, LOGPIXELSX));
            ReleaseDC(nullptr, screen);
        }
        const float scale = window.dpi / 96.0f;
        RECT frame{ 0, 0, static_cast<LONG>(560 * scale), static_cast<LONG>(400 * scale) };
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectEx(&frame, style, FALSE, kExNoRedirectionBitmap);
        const int width = frame.right - frame.left, height = frame.bottom - frame.top;
        RECT area{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (const HWND centerOn = window.centerOn != nullptr ? window.centerOn : owner; centerOn != nullptr) GetWindowRect(centerOn, &area);
        window.hwnd = CreateWindowExW(kExNoRedirectionBitmap, wc.lpszClassName, window.title.c_str(), style,
            area.left + (area.right - area.left - width) / 2, std::max<LONG>(0, area.top + (area.bottom - area.top - height) / 2),
            width, height, owner, nullptr, wc.hInstance, nullptr);
        if (window.hwnd == nullptr) return false;
        SetWindowLongPtrW(window.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));

        // Real acrylic behind the whole client area, rounded window corners
        const MARGINS margins{ -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(window.hwnd, &margins);
        const int backdrop = 3; // DWMSBT_TRANSIENTWINDOW (acrylic)
        DwmSetWindowAttribute(window.hwnd, 38, &backdrop, sizeof(backdrop));
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(window.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

        if (!window.Init())
        {
            DestroyWindow(window.hwnd);
            return false;
        }
        {
            // Build the content, then fit and centre the window around it
            const D2D1_SIZE_F content = window.build ? window.build() : D2D1::SizeF(560, 400);
            RECT fitted{ 0, 0, static_cast<LONG>(content.width * scale), static_cast<LONG>(content.height * scale) };
            AdjustWindowRectEx(&fitted, style, FALSE, kExNoRedirectionBitmap);
            const int fw = fitted.right - fitted.left, fh = fitted.bottom - fitted.top;
            SetWindowPos(window.hwnd, nullptr, area.left + (area.right - area.left - fw) / 2,
                std::max<LONG>(0, area.top + (area.bottom - area.top - fh) / 2), fw, fh, SWP_NOZORDER | SWP_NOACTIVATE);
            window.Resize();
        }
        if (runLoop && owner != nullptr) EnableWindow(owner, FALSE);
        ShowWindow(window.hwnd, SW_SHOW);
        SetForegroundWindow(window.hwnd);
        SetTimer(window.hwnd, kTimer, 16, nullptr);
        window.shownAt = GetTickCount64();
        window.Render();
        if (!runLoop) return true;

        MSG msg;
        while (!window.closed && GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (owner != nullptr)
        {
            EnableWindow(owner, TRUE);
            SetForegroundWindow(owner);
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    //  Settings bridge: the classic property sheet keeps running (hidden) so every page keeps its
    //  validation and apply logic; this window mirrors the real controls with glass widgets and forwards input.
    // ---------------------------------------------------------------------------------------------
    std::wstring WindowText(const HWND w)
    {
        const int length = GetWindowTextLengthW(w);
        std::wstring text(static_cast<size_t>(std::max(0, length)), L'\0');
        if (length > 0) GetWindowTextW(w, text.data(), length + 1);
        return text;
    }

    std::wstring StripMnemonic(const std::wstring& text)
    {
        std::wstring result;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'&' && i + 1 < text.size() && text[i + 1] == L'&') { result.push_back(L'&'); ++i; }
            else if (text[i] != L'&') result.push_back(text[i]);
        }
        return result;
    }

    bool SyncBase(Widget& widget, const HWND real)
    {
        bool changed = false;
        const bool shown = (GetWindowLongPtrW(real, GWL_STYLE) & WS_VISIBLE) != 0;
        const bool on = IsWindowEnabled(real) != FALSE;
        if (shown != widget.visible) { widget.visible = shown; changed = true; }
        if (on != widget.enabled) { widget.enabled = on; changed = true; }
        return changed;
    }

    ColorF ReadChildColor(const HWND button)
    {
        const HWND child = GetWindow(button, GW_CHILD);
        RECT rc{};
        if (child == nullptr || !GetClientRect(child, &rc) || rc.right < 2 || rc.bottom < 2) return ColorF(0.5f, 0.5f, 0.5f);
        const HDC screen = GetDC(nullptr);
        const HDC mem = CreateCompatibleDC(screen);
        const HBITMAP bitmap = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
        const HGDIOBJ old = SelectObject(mem, bitmap);
        SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
        const COLORREF c = GetPixel(mem, rc.right / 2, rc.bottom / 2);
        SelectObject(mem, old);
        DeleteObject(bitmap);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f);
    }

    struct SheetHost
    {
        GlassUi::SheetHooks hooks;
        GlassWindow window;
        TabBar* tabs = nullptr;
        size_t fixedCount = 0;
        int builtPage = -1;
        float scaleX = 1.3f, scaleY = 1.65f;
        float originX = 28, originY = 88, contentRight = 760;

        HWND Real(const int id) const { return GetDlgItem(hooks.sheet, id); }

        void BuildPage()
        {
            const int index = hooks.active();
            if (index == builtPage) return;
            builtPage = index;
            window.widgets.resize(fixedCount);
            window.hotWidget = window.pressedWidget = nullptr;
            window.SetFocus(nullptr);
            window.ClosePopup();
            if (tabs != nullptr) tabs->selected = index;
            const HWND page = hooks.page(index);
            if (page == nullptr) return;
            const float k = 96.0f / window.dpi;

            // Group cards first so they sit behind their controls
            std::vector<HWND> children;
            for (HWND child = GetWindow(page, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT)) children.push_back(child);
            const auto classOf = [](const HWND w)
            {
                std::array<WCHAR, 32> name{};
                GetClassNameW(w, name.data(), static_cast<int>(name.size()));
                return std::wstring(name.data());
            };
            const auto rectOf = [&](const HWND w, const float minH, const float minW)
            {
                RECT rc{};
                GetWindowRect(w, &rc);
                MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&rc), 2);
                const float x = originX + static_cast<float>(rc.left) * k * scaleX, y = originY + static_cast<float>(rc.top) * k * scaleY;
                const float w2 = std::max(minW, static_cast<float>(rc.right - rc.left) * k * scaleX);
                const float h = static_cast<float>(rc.bottom - rc.top) * k * scaleY;
                const float height = std::max(minH, h);
                const float left = std::min(x, contentRight - w2);
                return D2D1::RectF(left, y + (h - height) / 2, left + w2, y + (h - height) / 2 + height);
            };
            for (const HWND child : children)
            {
                if (classOf(child) != L"Button" || (GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) != BS_GROUPBOX) continue;
                const D2D1_RECT_F rect = rectOf(child, 0, 0);
                auto* group = window.Add<GroupCard>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                group->syncFn = [group, child] { const std::wstring t = StripMnemonic(WindowText(child)); const bool ch = t != group->text; group->text = t; return SyncBase(*group, child) || ch; };
            }
            for (const HWND child : children)
            {
                const std::wstring cls = classOf(child);
                const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
                const int id = GetDlgCtrlID(child);
                if (cls == L"Button")
                {
                    const LONG_PTR type = style & BS_TYPEMASK;
                    if (type == BS_GROUPBOX) continue;
                    if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON ||
                        type == BS_3STATE || type == BS_AUTO3STATE)
                    {
                        const D2D1_RECT_F rect = rectOf(child, 30, 0);
                        auto* check = window.Add<Check>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                        check->radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
                        check->changed = [child] { SendMessageW(child, BM_CLICK, 0, 0); };
                        check->syncFn = [check, child]
                        {
                            bool ch = SyncBase(*check, child);
                            const std::wstring t = StripMnemonic(WindowText(child));
                            const bool c = SendMessageW(child, BM_GETCHECK, 0, 0) != BST_UNCHECKED;
                            if (t != check->text || c != check->checked) ch = true;
                            check->text = t;
                            check->checked = c;
                            return ch;
                        };
                    }
                    else
                    {
                        RECT realRc{};
                        GetWindowRect(child, &realRc);
                        const float minW = static_cast<float>(realRc.right - realRc.left) * k * scaleX < 70.0f ? 48.0f : 96.0f;
                        const D2D1_RECT_F rect = rectOf(child, 40, minW);
                        auto* button = window.Add<Button>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                        button->hasSwatch = GetWindow(child, GW_CHILD) != nullptr; // colour picker button
                        button->action = [child] { SendMessageW(child, BM_CLICK, 0, 0); };
                        auto lastSwatch = std::make_shared<ULONGLONG>(0);
                        button->syncFn = [button, child, lastSwatch]
                        {
                            bool ch = SyncBase(*button, child);
                            const std::wstring t = StripMnemonic(WindowText(child));
                            if (t != button->text) { button->text = t; ch = true; }
                            if (button->hasSwatch && GetTickCount64() - *lastSwatch > 400)
                            {
                                *lastSwatch = GetTickCount64();
                                const ColorF c = ReadChildColor(child);
                                if (c.r != button->swatch.r || c.g != button->swatch.g || c.b != button->swatch.b) { button->swatch = c; ch = true; }
                            }
                            return ch;
                        };
                    }
                }
                else if (cls == L"Static" && id == IDC_PREVIEW)
                {
                    const D2D1_RECT_F rect = rectOf(child, 0, 0);
                    auto* mirror = window.Add<PixelMirror>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                    mirror->real = child;
                    mirror->syncFn = [mirror, child] { return SyncBase(*mirror, child); };
                }
                else if (cls == L"Static" && id == IDC_LIGHTSOURCE)
                {
                    const D2D1_RECT_F rect = rectOf(child, 0, 0);
                    const float side = std::min(rect.right - rect.left, rect.bottom - rect.top);
                    auto* pad = window.Add<XYPad>(rect.left, rect.top, side, side);
                    pad->changed = [child, id](const int px, const int py)
                    {
                        POINT pt{ px, py };
                        SendMessageW(child, WM_USER + 100 /* XY_SETPOS */, 0, reinterpret_cast<LPARAM>(&pt));
                        NMHDR header{ child, static_cast<UINT_PTR>(id), 0x88 /* XYSLIDER_CHANGED */ };
                        SendMessageW(GetParent(child), WM_NOTIFY, static_cast<WPARAM>(id), reinterpret_cast<LPARAM>(&header));
                    };
                    pad->syncFn = [pad, child]
                    {
                        bool ch = SyncBase(*pad, child);
                        if (!pad->pressed)
                        {
                            POINT pt{};
                            SendMessageW(child, WM_USER + 101 /* XY_GETPOS */, 0, reinterpret_cast<LPARAM>(&pt));
                            if (pt.x != pad->x || pt.y != pad->y) { pad->x = pt.x; pad->y = pt.y; ch = true; }
                        }
                        return ch;
                    };
                }
                else if (cls == L"Static")
                {
                    if ((style & SS_TYPEMASK) != SS_LEFT && (style & SS_TYPEMASK) != SS_CENTER && (style & SS_TYPEMASK) != SS_RIGHT &&
                        (style & SS_TYPEMASK) != SS_LEFTNOWORDWRAP) continue;
                    const D2D1_RECT_F rect = rectOf(child, 26, 0);
                    auto* label = window.Add<WrapLabel>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                    label->text = StripMnemonic(WindowText(child));
                    if (const float needed = window.gfx.MeasureWrapped(label->text, 14.5f, label->r.right - label->r.left) + 6.0f;
                        needed > label->r.bottom - label->r.top) label->r.bottom = label->r.top + needed;
                    label->syncFn = [label, child]
                    {
                        bool ch = SyncBase(*label, child);
                        const std::wstring t = StripMnemonic(WindowText(child));
                        if (t != label->text) { label->text = t; ch = true; }
                        return ch;
                    };
                }
                else if (cls == L"ComboBox" && (style & 3) == CBS_DROPDOWN)
                {
                    // Editable combo box: a text field whose chevron opens the item list
                    const D2D1_RECT_F rect = rectOf(child, 42, 120);
                    auto* field = window.Add<Field>(rect.left, rect.top, rect.right - rect.left, 42);
                    field->r.top = rect.top + (rect.bottom - rect.top - 42) / 2;
                    field->r.bottom = field->r.top + 42;
                    field->changed = [field, child, id]
                    {
                        SetWindowTextW(child, field->text.c_str());
                        SendMessageW(GetParent(child), WM_COMMAND, MAKEWPARAM(id, CBN_EDITCHANGE), reinterpret_cast<LPARAM>(child));
                    };
                    field->syncFn = [field, child]
                    {
                        bool ch = SyncBase(*field, child);
                        if (!field->focused)
                        {
                            const std::wstring t = WindowText(child);
                            if (t != field->text) { field->text = t; ch = true; }
                        }
                        return ch;
                    };
                    Field* raw = field;
                    field->dropdown = [this, raw, child, id]
                    {
                        const int count = static_cast<int>(SendMessageW(child, CB_GETCOUNT, 0, 0));
                        std::vector<std::wstring> items;
                        for (int i = 0; i < count; ++i)
                        {
                            const LRESULT length = SendMessageW(child, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
                            std::wstring text(static_cast<size_t>(std::max<LRESULT>(0, length)), L'\0');
                            if (length > 0) SendMessageW(child, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
                            items.push_back(text);
                        }
                        if (items.empty()) return;
                        window.OpenPopup(raw->r, std::move(items), -1, [raw, child, id](const int chosen)
                        {
                            SendMessageW(child, CB_SETCURSEL, static_cast<WPARAM>(chosen), 0);
                            SendMessageW(GetParent(child), WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(child));
                            raw->text = WindowText(child);
                            raw->caret = raw->text.size();
                        });
                    };
                }
                else if (cls == L"ComboBox")
                {
                    const D2D1_RECT_F rect = rectOf(child, 40, 120);
                    auto* combo = window.Add<Combo>(rect.left, rect.top, rect.right - rect.left, 40);
                    combo->r.top = rect.top + (rect.bottom - rect.top - 40) / 2;
                    combo->r.bottom = combo->r.top + 40;
                    const auto itemText = [](const HWND box, const int i)
                    {
                        const LRESULT length = SendMessageW(box, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
                        std::wstring text(static_cast<size_t>(std::max<LRESULT>(0, length)), L'\0');
                        if (length > 0) SendMessageW(box, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
                        return text;
                    };
                    combo->syncFn = [combo, child, itemText]
                    {
                        bool ch = SyncBase(*combo, child);
                        const int sel = static_cast<int>(SendMessageW(child, CB_GETCURSEL, 0, 0));
                        const std::wstring t = sel >= 0 ? itemText(child, sel) : WindowText(child);
                        if (t != combo->text) { combo->text = t; ch = true; }
                        return ch;
                    };
                    Combo* raw = combo;
                    combo->open = [this, raw, child, itemText, id]
                    {
                        const int count = static_cast<int>(SendMessageW(child, CB_GETCOUNT, 0, 0));
                        std::vector<std::wstring> items;
                        for (int i = 0; i < count; ++i) items.push_back(itemText(child, i));
                        const int sel = static_cast<int>(SendMessageW(child, CB_GETCURSEL, 0, 0));
                        window.OpenPopup(raw->r, std::move(items), sel, [child, id](const int chosen)
                        {
                            SendMessageW(child, CB_SETCURSEL, static_cast<WPARAM>(chosen), 0);
                            SendMessageW(GetParent(child), WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(child));
                        });
                    };
                }
                else if (cls == L"Edit")
                {
                    const bool multi = (style & ES_MULTILINE) != 0;
                    const D2D1_RECT_F rect = rectOf(child, multi ? 60 : 42, 80);
                    auto* field = window.Add<Field>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                    field->multiline = multi;
                    const auto toField = [](std::wstring t) { std::erase(t, L'\r'); return t; };
                    const auto toReal = [](const std::wstring& t)
                    {
                        std::wstring r;
                        for (const wchar_t c : t) { if (c == L'\n') r += L"\r\n"; else r.push_back(c); }
                        return r;
                    };
                    field->changed = [field, child, id, toReal]
                    {
                        SetWindowTextW(child, toReal(field->text).c_str());
                        SendMessageW(GetParent(child), WM_COMMAND, MAKEWPARAM(id, EN_CHANGE), reinterpret_cast<LPARAM>(child));
                    };
                    field->syncFn = [field, child, toField]
                    {
                        bool ch = SyncBase(*field, child);
                        if (!field->focused)
                        {
                            const std::wstring t = toField(WindowText(child));
                            if (t != field->text) { field->text = t; ch = true; }
                        }
                        return ch;
                    };
                }
                else if (cls == L"msctls_progress32")
                {
                    const D2D1_RECT_F rect = rectOf(child, 14, 0);
                    auto* bar = window.Add<ProgressBar>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                    bar->syncFn = [bar, child]
                    {
                        bool ch = SyncBase(*bar, child);
                        const int lo = static_cast<int>(SendMessageW(child, PBM_GETRANGE, TRUE, 0));
                        const int hi = static_cast<int>(SendMessageW(child, PBM_GETRANGE, FALSE, 0));
                        const int pos = static_cast<int>(SendMessageW(child, PBM_GETPOS, 0, 0));
                        const bool marquee = (GetWindowLongPtrW(child, GWL_STYLE) & PBS_MARQUEE) != 0;
                        if (lo != bar->minValue || hi != bar->maxValue || pos != bar->value || marquee != bar->marquee) ch = true;
                        bar->minValue = lo;
                        bar->maxValue = hi;
                        bar->value = pos;
                        bar->marquee = marquee;
                        return ch;
                    };
                }
                else if (cls == L"msctls_trackbar32")
                {
                    const bool vertical = (style & TBS_VERT) != 0;
                    const D2D1_RECT_F rect = rectOf(child, vertical ? 0 : 34, vertical ? 34 : 0);
                    auto* slider = window.Add<Slider>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                    slider->vertical = vertical;
                    slider->changed = [child, vertical](const int value, const bool final)
                    {
                        SendMessageW(child, TBM_SETPOS, TRUE, value);
                        SendMessageW(GetParent(child), vertical ? WM_VSCROLL : WM_HSCROLL,
                            MAKEWPARAM(final ? TB_ENDTRACK : TB_THUMBTRACK, value), reinterpret_cast<LPARAM>(child));
                    };
                    slider->syncFn = [slider, child]
                    {
                        bool ch = SyncBase(*slider, child);
                        const int lo = static_cast<int>(SendMessageW(child, TBM_GETRANGEMIN, 0, 0));
                        const int hi = static_cast<int>(SendMessageW(child, TBM_GETRANGEMAX, 0, 0));
                        const int pos = static_cast<int>(SendMessageW(child, TBM_GETPOS, 0, 0));
                        if (lo != slider->minValue || hi != slider->maxValue || (!slider->pressed && pos != slider->value)) ch = true;
                        slider->minValue = lo;
                        slider->maxValue = hi;
                        if (!slider->pressed) slider->value = pos;
                        return ch;
                    };
                }
                else if (cls == L"ListBox")
                {
                    const D2D1_RECT_F rect = rectOf(child, 60, 100);
                    auto* list = window.Add<ChoiceList>(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
                    list->chosen = [child, id](const int row)
                    {
                        SendMessageW(child, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
                        SendMessageW(GetParent(child), WM_COMMAND, MAKEWPARAM(id, LBN_SELCHANGE), reinterpret_cast<LPARAM>(child));
                    };
                    list->syncFn = [list, child]
                    {
                        bool ch = SyncBase(*list, child);
                        const int count = static_cast<int>(SendMessageW(child, LB_GETCOUNT, 0, 0));
                        std::vector<std::wstring> rows;
                        for (int i = 0; i < count; ++i)
                        {
                            const LRESULT length = SendMessageW(child, LB_GETTEXTLEN, static_cast<WPARAM>(i), 0);
                            std::wstring text(static_cast<size_t>(std::max<LRESULT>(0, length)), L'\0');
                            if (length > 0) SendMessageW(child, LB_GETTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
                            rows.push_back(text);
                        }
                        const int sel = static_cast<int>(SendMessageW(child, LB_GETCURSEL, 0, 0));
                        if (rows != list->rows || sel != list->selected) ch = true;
                        list->rows = std::move(rows);
                        list->selected = sel;
                        return ch;
                    };
                }
            }
        }
    };

    LRESULT CALLBACK SheetSubclass(const HWND sheet, const UINT message, const WPARAM wParam, const LPARAM lParam,
        const UINT_PTR id, const DWORD_PTR ref)
    {
        auto* host = reinterpret_cast<SheetHost*>(ref);
        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(sheet, SheetSubclass, id);
            if (host->window.hwnd != nullptr && IsWindow(host->window.hwnd)) DestroyWindow(host->window.hwnd);
            delete host;
            return DefSubclassProc(sheet, message, wParam, lParam);
        }
        if (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE && host->window.hwnd != nullptr)
            PostMessageW(host->window.hwnd, WM_USER + 1, 0, 0);
        return DefSubclassProc(sheet, message, wParam, lParam);
    }
}

int GlassUi::RunDemo()
{
    GlassWindow window;
    window.title = L"WinDirStat - Select Drives";
    window.build = [&window] { return window.BuildSelect(); };
    window.quitOnClose = true;
    return RunWindow(window, nullptr) ? 0 : 1;
}

bool GlassUi::SelectScanRoots(const HWND owner, std::wstring& path, bool& cancelled)
{
    GlassWindow window;
    window.title = L"WinDirStat - Select Drives";
    window.build = [&window] { return window.BuildSelect(); };
    if (!RunWindow(window, owner)) return false;
    cancelled = !window.model.accepted;
    if (!cancelled) path = window.model.result;
    return true;
}

bool GlassUi::ShowMessage(const HWND owner, const std::wstring& title, const std::wstring& message, const int iconKind,
    const std::vector<MessageButton>& buttons, const std::vector<std::wstring>& items,
    const std::wstring& checkboxText, bool& checkboxValue, int& resultId)
{
    // Menu-style mnemonics ("&Yes") are not drawn by this UI: drop the marker, keep literal "&&"
    const auto plain = [](std::wstring text)
    {
        std::wstring result;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'&' && i + 1 < text.size() && text[i + 1] == L'&') { result.push_back(L'&'); ++i; }
            else if (text[i] != L'&') result.push_back(text[i]);
        }
        return result;
    };
    GlassWindow window;
    window.title = title;
    window.escapeId = IDCANCEL;
    window.resultId = IDCANCEL;
    bool checked = checkboxValue;
    window.build = [&]() -> D2D1_SIZE_F
    {
        constexpr float width = 500, margin = 28, iconSize = 48, buttonHeight = 42;
        const float textX = margin + iconSize + 18, textW = width - textX - margin;
        const float textH = std::max(iconSize, window.gfx.MeasureWrapped(message, 14.5f, textW) + 6);
        const float wellH = items.empty() ? 0.0f : static_cast<float>(std::min<size_t>(items.size(), 6)) * TextWell::kRow + 16;
        float height = margin + textH + 22;
        if (wellH > 0) height += wellH + 16;
        if (!checkboxText.empty()) height += 44;
        height += buttonHeight + margin;

        window.Add<Panel>(14, 14, width - 28, height - 28);
        auto* icon = window.Add<GlyphIcon>(margin, margin, iconSize, iconSize);
        icon->kind = iconKind;
        auto* text = window.Add<WrapLabel>(textX, margin, textW, textH);
        text->text = message;

        float y = margin + textH + 22;
        if (wellH > 0)
        {
            auto* well = window.Add<TextWell>(margin, y, width - 2 * margin, wellH);
            well->rows = items;
            window.wheelTarget = well;
            y += wellH + 16;
        }
        Check* box = nullptr;
        if (!checkboxText.empty())
        {
            box = window.Add<Check>(margin, y, width - 2 * margin, 28);
            box->text = plain(checkboxText);
            box->checked = checked;
            box->changed = [box, &window] { box->checked = !box->checked; window.dirty = true; };
            y += 44;
        }

        // Buttons are right aligned, the default one is the prominent capsule
        std::vector<float> widths;
        float total = 0;
        for (const auto& button : buttons)
        {
            const float w = std::max(104.0f, window.gfx.Measure(plain(button.text), 14) + 48);
            widths.push_back(w);
            total += w;
        }
        total += 12.0f * static_cast<float>(buttons.empty() ? 0 : buttons.size() - 1);
        float x = width - margin - total;
        int defaultId = buttons.empty() ? IDOK : buttons.back().id;
        for (size_t i = 0; i < buttons.size(); ++i)
        {
            auto* button = window.Add<Button>(x, y, widths[i], buttonHeight);
            button->text = plain(buttons[i].text);
            button->primary = buttons[i].isDefault;
            if (buttons[i].isDefault) defaultId = buttons[i].id;
            const int id = buttons[i].id;
            button->action = [id, box, &window, &checked]
            {
                window.resultId = id;
                if (box) checked = box->checked;
                window.Close();
            };
            x += widths[i] + 12;
        }
        window.defaultAction = [&window, defaultId, box, &checked]
        {
            window.resultId = defaultId;
            if (box) checked = box->checked;
            window.Close();
        };
        return D2D1::SizeF(width, height);
    };
    if (!RunWindow(window, owner)) return false;
    resultId = window.resultId;
    checkboxValue = checked;
    return true;
}

bool GlassUi::AttachSheet(const SheetHooks& hooks)
{
    if (hooks.sheet == nullptr || hooks.pageCount <= 0) return false;
    auto host = std::make_unique<SheetHost>();
    host->hooks = hooks;
    SheetHost* raw = host.get();
    GlassWindow& window = host->window;
    window.title = hooks.caption;
    window.centerOn = GetWindow(hooks.sheet, GW_OWNER);
    window.dpi = static_cast<float>(GetWindowDpi(hooks.sheet));
    window.build = [raw, &window]() -> D2D1_SIZE_F
    {
        const SheetHooks& h = raw->hooks;
        const float k = 96.0f / window.dpi;
        const HWND page = h.page(h.active());
        RECT pageRc{ 0, 0, 480, 300 };
        if (page != nullptr) GetWindowRect(page, &pageRc);
        const float pageW = static_cast<float>(pageRc.right - pageRc.left) * k, pageH = static_cast<float>(pageRc.bottom - pageRc.top) * k;
        constexpr float targetW = 800;
        raw->scaleX = (targetW - 56) / std::max(1.0f, pageW);
        raw->scaleY = 1.65f;
        const float contentH = pageH * raw->scaleY;
        const float width = targetW, height = 28 + 52 + 20 + contentH + 24 + 44 + 28;

        window.Add<Panel>(14, 90, width - 28, contentH + 24);
        raw->originX = 28;
        raw->originY = 102;
        raw->contentRight = width - 40;

        raw->tabs = window.Add<TabBar>(14, 24, width - 28, 52);
        for (int i = 0; i < h.pageCount; ++i) raw->tabs->tabs.push_back(h.title(i));
        raw->tabs->chosen = [raw](const int i) { raw->hooks.select(i); };

        // OK / Cancel / Apply drive the real (hidden) sheet buttons
        const auto realButton = [raw](const int id) { return GetDlgItem(raw->hooks.sheet, id); };
        float x = width - 28;
        const std::array<std::pair<int, const wchar_t*>, 3> buttons{ { { ID_APPLY_NOW, L"Apply" }, { IDCANCEL, L"Cancel" }, { IDOK, L"OK" } } };
        for (const auto& [id, fallback] : buttons)
        {
            const HWND real = realButton(id);
            std::wstring label = real != nullptr ? StripMnemonic(WindowText(real)) : fallback;
            if (label.empty() || label.rfind(L"IDS_", 0) == 0) label = fallback;
            const float w = 112;
            x -= w;
            auto* button = window.Add<Button>(x, height - 28 - 42, w, 42);
            button->text = label;
            button->primary = id == IDOK;
            button->action = [real] { if (real) SendMessageW(real, BM_CLICK, 0, 0); };
            button->syncFn = [button, real]
            {
                if (real == nullptr) return false;
                const bool on = IsWindowEnabled(real) != FALSE;
                if (on == button->enabled) return false;
                button->enabled = on;
                return true;
            };
            x -= 12;
        }
        window.escapeAction = [raw] { if (const HWND c = GetDlgItem(raw->hooks.sheet, IDCANCEL)) SendMessageW(c, BM_CLICK, 0, 0); };
        window.defaultAction = [raw] { if (const HWND c = GetDlgItem(raw->hooks.sheet, IDOK)) SendMessageW(c, BM_CLICK, 0, 0); };
        raw->fixedCount = window.widgets.size();
        window.frameSync = [raw] { const int before = raw->builtPage; raw->BuildPage(); return before != raw->builtPage; };
        raw->BuildPage();
        return D2D1::SizeF(width, height);
    };

    // Hide the classic sheet: fully transparent, click-through and out of the taskbar
    LONG_PTR ex = GetWindowLongPtrW(hooks.sheet, GWL_EXSTYLE);
    SetWindowLongPtrW(hooks.sheet, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetLayeredWindowAttributes(hooks.sheet, 0, 0, LWA_ALPHA);

    if (!RunWindow(window, hooks.sheet, false))
    {
        SetWindowLongPtrW(hooks.sheet, GWL_EXSTYLE, ex);
        return false;
    }
    SetWindowSubclass(hooks.sheet, SheetSubclass, 0x6C73, reinterpret_cast<DWORD_PTR>(host.release()));
    return true;
}

bool GlassUi::AttachDialog(const HWND dialog, const std::wstring& caption)
{
    if (dialog == nullptr) return false;
    auto host = std::make_unique<SheetHost>();
    host->hooks.sheet = dialog;
    host->hooks.caption = caption;
    host->hooks.pageCount = 1;
    host->hooks.active = [] { return 0; };
    host->hooks.page = [dialog](const int) { return dialog; };
    host->hooks.select = [](const int) { return true; };
    host->hooks.title = [](const int) { return std::wstring(); };
    SheetHost* raw = host.get();
    GlassWindow& window = host->window;
    window.title = caption;
    window.centerOn = GetWindow(dialog, GW_OWNER);
    window.dpi = static_cast<float>(GetWindowDpi(dialog));
    window.build = [raw, &window]() -> D2D1_SIZE_F
    {
        const float k = 96.0f / window.dpi;
        RECT rc{};
        GetClientRect(raw->hooks.sheet, &rc);
        constexpr float scale = 1.3f, margin = 28;
        raw->scaleX = raw->scaleY = scale;
        const float contentW = std::max(320.0f, static_cast<float>(rc.right) * k * scale);
        const float contentH = static_cast<float>(rc.bottom) * k * scale;
        const float width = contentW + 2 * (margin + 14), height = contentH + 2 * (margin + 14);
        window.Add<Panel>(14, 14, width - 28, height - 28);
        raw->originX = margin + 14;
        raw->originY = margin + 14;
        raw->contentRight = width - margin - 14;
        raw->fixedCount = window.widgets.size();
        const auto click = [raw](const int id) { if (const HWND c = GetDlgItem(raw->hooks.sheet, id)) SendMessageW(c, BM_CLICK, 0, 0); };
        window.escapeAction = [click] { click(IDCANCEL); };
        window.defaultAction = [click] { click(IDOK); };
        window.frameSync = [raw] { const int before = raw->builtPage; raw->BuildPage(); return before != raw->builtPage; };
        raw->BuildPage();
        return D2D1::SizeF(width, height);
    };

    LONG_PTR ex = GetWindowLongPtrW(dialog, GWL_EXSTYLE);
    SetWindowLongPtrW(dialog, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetLayeredWindowAttributes(dialog, 0, 0, LWA_ALPHA);
    if (!RunWindow(window, dialog, false))
    {
        SetWindowLongPtrW(dialog, GWL_EXSTYLE, ex);
        return false;
    }
    SetWindowSubclass(dialog, SheetSubclass, 0x6C73, reinterpret_cast<DWORD_PTR>(host.release()));
    return true;
}
