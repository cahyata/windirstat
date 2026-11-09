// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "GlassOverlay.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
// The project targets Windows 7 headers; DirectComposition needs the Windows 8 API surface.
// It is loaded dynamically and only used when present.
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <dcomp.h>
#pragma pop_macro("_WIN32_WINNT")
#include <dwrite.h>
#include <wrl/client.h>

namespace
{
    using Microsoft::WRL::ComPtr;

    // Effect CLSIDs (defined locally so no import library for GUIDs is required)
    constexpr GUID kClsidAffine = { 0x6aa97485, 0x6354, 0x4cfc, { 0x90, 0x8c, 0xe4, 0xa7, 0x4f, 0x62, 0xc9, 0x6c } };
    constexpr GUID kClsidDisplacement = { 0xedc48364, 0x0417, 0x4111, { 0x94, 0x50, 0x43, 0x84, 0x5f, 0xa9, 0xf8, 0x90 } };

    constexpr UINT_PTR kTimerId = 0x47A5;
    constexpr UINT_PTR kSubclassId = 0x474C;
    constexpr UINT kFrameMs = 16;
    constexpr int kCaptureMargin = 28;    // pixels captured around a region so edges can sample outside it
    constexpr float kBand = 13.0f;        // width of the refracting rim in pixels
    constexpr float kRefraction = 30.0f;  // displacement scale (max shift is half of this)

    std::atomic<int> g_active{ 0 };

    using DWriteCreateFactoryFn = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
    using D2D1CreateFactoryFn = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    using DCompCreateDeviceFn = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);

    struct MapKey
    {
        int w, h;
        float radius;
        bool operator==(const MapKey&) const = default;
    };

    // Four edges chasing a rectangle with critical damping (smooth, no overshoot)
    struct Edges
    {
        std::array<float, 4> x{}, v{}, t{};
        std::array<float, 4> p{};          // lens left behind on the previous control
        float prevAlpha = 0;
        bool init = false;
        // Stay on the control: a new target replaces the lens in place, the previous one fades away where it was
        void Jump(const RECT& r, float& alpha)
        {
            const std::array<float, 4> n{ static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom) };
            if (!init) { Snap(r); return; }
            if (n == t) return;
            if (alpha > 0.03f) { p = x; prevAlpha = alpha; }
            Snap(r);
            alpha = 0.0f;
        }
        void StepPrev() { prevAlpha *= 0.68f; if (prevAlpha < 0.02f) prevAlpha = 0.0f; }
        D2D1_RECT_F PrevRect() const { return D2D1::RectF(p[0], p[1], p[2], p[3]); }
        void Snap(const RECT& r) { x = t = { static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom) }; v = {}; init = true; }
        void Target(const RECT& r) { t = { static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom) }; if (!init) Snap(r); }
        void Step()
        {
            const float dx = (t[0] + t[2]) / 2 - (x[0] + x[2]) / 2, dy = (t[1] + t[3]) / 2 - (x[1] + x[3]) / 2;
            constexpr float lead = 24.0f, trail = 19.0f, dt = 1.0f / 60.0f;
            const std::array<float, 4> w{ dx < 0 ? lead : trail, dy < 0 ? lead : trail, dx > 0 ? lead : trail, dy > 0 ? lead : trail };
            for (size_t i = 0; i < 4; ++i)
            {
                v[i] += (w[i] * w[i] * (t[i] - x[i]) - 2.0f * w[i] * v[i]) * dt;
                x[i] += v[i] * dt;
            }
        }
        bool Moving() const
        {
            for (size_t i = 0; i < 4; ++i) if (std::fabs(t[i] - x[i]) > 0.2f || std::fabs(v[i]) > 4.0f) return true;
            return false;
        }
        D2D1_RECT_F Rect() const { return D2D1::RectF(x[0], x[1], x[2], x[3]); }
    };

    struct Overlay
    {
        HWND window{};
        GlassOverlay::LayerProvider provider;
        ComPtr<IDWriteFactory> dwrite;
        std::vector<std::pair<std::pair<HIMAGELIST, int>, ComPtr<ID2D1Bitmap1>>> icons;
        std::map<int, std::pair<float, float>> anim; // command id -> (hover, press)
        std::vector<std::pair<int, ComPtr<IDWriteTextFormat>>> formats;
        std::vector<std::pair<HICON, ComPtr<ID2D1Bitmap1>>> hicons;
        bool animating = false;
        ComPtr<ID2D1StrokeStyle> stroke_;
        ID2D1StrokeStyle* roundStroke()
        {
            if (stroke_ == nullptr)
                factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &stroke_);
            return stroke_.Get();
        }
        size_t lastSignature = 0;
        bool dark = false;
        float scale = 1.0f;
        float S(const float v) const { return v * scale; }   // 96-dpi design value -> device pixels
        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGISwapChain1> swap;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1DeviceContext> dc;
        ComPtr<IDCompositionDevice> dcomp;
        ComPtr<IDCompositionTarget> target;
        ComPtr<IDCompositionVisual> visual;
        ComPtr<ID2D1Bitmap1> targetBitmap;
        std::vector<std::pair<MapKey, ComPtr<ID2D1Bitmap1>>> maps;
        int width = 0, height = 0;
        HDC memDc{};
        HBITMAP dib{};
        void* bits{};
        int dibW = 0, dibH = 0;
        bool failed = false;
        float curX = 0, curY = 0;   // smoothed cursor position (client coordinates)
        bool seeded = false;
        int idle = 0;

        ~Overlay()
        {
            if (dib != nullptr) DeleteObject(dib);
            if (memDc != nullptr) DeleteDC(memDc);
        }

        bool Init();
        bool BindTarget();
        void Resize(int w, int h);
        bool EnsureDib(int w, int h);
        ID2D1Bitmap1* GetMap(int w, int h, float radius);
        void DrawRegion(const GlassRegion& region, HDC source, float cursorX, float cursorY);
        void Render();
        ID2D1Bitmap1* GetIcon(HIMAGELIST list, int index);
        IDWriteTextFormat* GetFormat(int px, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
        ID2D1Bitmap1* GetHIcon(HICON icon);
        void DrawList(const GlassListView& view);
        void DrawTabStrip(const GlassTabStrip& strip);
        void DrawTreeMap(const GlassTreeMap& map);
        void DrawSplitter(const GlassSplitter& bar);
        void DrawMenuBar(const GlassMenuBar& bar);
        void DrawGroups(const std::vector<RECT>& groups);
        void DrawToolbarLens(const D2D1_RECT_F& r, float a);
        bool StepToolbar(const GlassLayer& layer);
        IDWriteTextFormat* GetGlyphFormat(int px);
        float TextWidth(const std::wstring& text, int px);
        std::vector<std::pair<int, ComPtr<IDWriteTextFormat>>> glyphFormats;
        std::wstring glyphFamily;
        bool glyphChecked = false;
        Edges toolLens;
        float toolAlpha = 0;
        bool StepMenuBar(const std::vector<GlassMenuBar>& bars);
        Edges menuLens;
        float menuAlpha = 0, menuStrength = 0;
        bool StepSplitters(const std::vector<GlassSplitter>& bars);
        bool StepTreeMap(const GlassTreeMap& map);
        Edges selectSpring, hoverSpring;
        float selectAlpha = 0, hoverAlpha = 0;
        void DrawIcon(const GlassIcon& icon);
        void DrawLabel(const GlassText& text);
        bool StepAnimation(const std::vector<GlassIcon>& list);
        bool StepRows(const std::vector<GlassRow>& rows);
        void DrawHoverRow(const GlassRow& row);
        static size_t Signature(const GlassLayer& layer);
    };

    bool Overlay::Init()
    {
        const HMODULE d3dModule = LoadLibraryW(L"d3d11.dll");
        const HMODULE d2dModule = LoadLibraryW(L"d2d1.dll");
        const HMODULE dcompModule = LoadLibraryW(L"dcomp.dll");
        if (d3dModule == nullptr || d2dModule == nullptr || dcompModule == nullptr) return false;

        const auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
            GetProcAddress(d3dModule, "D3D11CreateDevice"));
        const auto createFactory = reinterpret_cast<D2D1CreateFactoryFn>(
            GetProcAddress(d2dModule, "D2D1CreateFactory"));
        const auto createDcomp = reinterpret_cast<DCompCreateDeviceFn>(
            GetProcAddress(dcompModule, "DCompositionCreateDevice"));
        if (createDevice == nullptr || createFactory == nullptr || createDcomp == nullptr) return false;
        if (const HMODULE dwriteModule = LoadLibraryW(L"dwrite.dll"); dwriteModule != nullptr)
            if (const auto createDwrite = reinterpret_cast<DWriteCreateFactoryFn>(GetProcAddress(dwriteModule, "DWriteCreateFactory")))
                createDwrite(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));

        if (FAILED(createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr))) return false;
        if (FAILED(d3d.As(&dxgi))) return false;
        if (FAILED(createFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
            reinterpret_cast<void**>(factory.GetAddressOf())))) return false;

        ComPtr<ID2D1Device> d2dDevice;
        if (FAILED(factory->CreateDevice(dxgi.Get(), &d2dDevice))) return false;
        if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
        dc->SetDpi(96.0f, 96.0f);

        if (FAILED(createDcomp(dxgi.Get(), __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(dcomp.GetAddressOf())))) return false;
        if (FAILED(dcomp->CreateTargetForHwnd(window, TRUE, &target))) return false;
        if (FAILED(dcomp->CreateVisual(&visual))) return false;

        RECT rc{};
        GetClientRect(window, &rc);
        width = std::max<LONG>(1, rc.right);
        height = std::max<LONG>(1, rc.bottom);

        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> dxgiFactory;
        if (FAILED(dxgi->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(dxgiFactory.GetAddressOf()))))
            return false;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(dxgiFactory->CreateSwapChainForComposition(d3d.Get(), &desc, nullptr, &swap))) return false;
        if (FAILED(visual->SetContent(swap.Get()))) return false;
        if (FAILED(target->SetRoot(visual.Get()))) return false;
        if (FAILED(dcomp->Commit())) return false;
        return BindTarget();
    }

    bool Overlay::BindTarget()
    {
        dc->SetTarget(nullptr);
        targetBitmap.Reset();
        ComPtr<IDXGISurface> surface;
        if (FAILED(swap->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.GetAddressOf()))))
            return false;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &targetBitmap))) return false;
        dc->SetTarget(targetBitmap.Get());
        return true;
    }

    void Overlay::Resize(const int w, const int h)
    {
        dc->SetTarget(nullptr);
        targetBitmap.Reset();
        width = w;
        height = h;
        if (FAILED(swap->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0)) ||
            !BindTarget()) failed = true;
    }

    bool Overlay::EnsureDib(const int w, const int h)
    {
        if (dib != nullptr && dibW >= w && dibH >= h) return true;
        if (dib != nullptr) { DeleteObject(dib); dib = nullptr; }
        dibW = std::max(dibW, w);
        dibH = std::max(dibH, h);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = dibW;
        info.bmiHeader.biHeight = -dibH; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (memDc == nullptr) memDc = CreateCompatibleDC(nullptr);
        dib = CreateDIBSection(memDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib == nullptr) return false;
        SelectObject(memDc, dib);
        return true;
    }

    // Displacement map for a rounded rectangle lens: offsets point away from the shape and grow towards its edge.
    ID2D1Bitmap1* Overlay::GetMap(const int w, const int h, const float radius)
    {
        const MapKey key{ w, h, radius };
        for (const auto& [k, bitmap] : maps) if (k == key) return bitmap.Get();
        if (maps.size() > 12) maps.clear();

        std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
        const float hw = w * 0.5f, hh = h * 0.5f;
        const float r = std::min({ radius, hw, hh });
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const float px = x + 0.5f - hw, py = y + 0.5f - hh;
                const float qx = std::fabs(px) - (hw - r), qy = std::fabs(py) - (hh - r);
                const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
                const float outer = std::sqrt(ox * ox + oy * oy);
                const float sdf = outer + std::min(std::max(qx, qy), 0.0f) - r; // negative inside
                const float depth = std::max(-sdf, 0.0f);
                float t = std::clamp(1.0f - depth / kBand, 0.0f, 1.0f);
                t *= t;

                float nx, ny;
                if (qx > 0 && qy > 0 && outer > 0.0001f)
                {
                    nx = ox / outer * (px < 0 ? -1.0f : 1.0f);
                    ny = oy / outer * (py < 0 ? -1.0f : 1.0f);
                }
                else if (qx > qy) { nx = px < 0 ? -1.0f : 1.0f; ny = 0; }
                else { nx = 0; ny = py < 0 ? -1.0f : 1.0f; }

                const auto red = static_cast<uint32_t>(std::clamp(0.5f + 0.5f * nx * t, 0.0f, 1.0f) * 255.0f + 0.5f);
                const auto green = static_cast<uint32_t>(std::clamp(0.5f + 0.5f * ny * t, 0.0f, 1.0f) * 255.0f + 0.5f);
                pixels[static_cast<size_t>(y) * w + x] = 0xFF000000u | (red << 16) | (green << 8);
            }
        }

        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        ComPtr<ID2D1Bitmap1> bitmap;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), pixels.data(),
            static_cast<UINT32>(w) * 4, &props, &bitmap))) return nullptr;
        maps.emplace_back(key, bitmap);
        return maps.back().second.Get();
    }

    void Overlay::DrawRegion(const GlassRegion& region, HDC source, const float cursorX, const float cursorY)
    {
        const int rw = region.rect.right - region.rect.left, rh = region.rect.bottom - region.rect.top;
        if (rw < 8 || rh < 8) return;

        // Capture the live content around the region from the window's own surface
        const RECT clientRc{ 0, 0, width, height };
        RECT cap{ region.rect.left - kCaptureMargin, region.rect.top - kCaptureMargin,
            region.rect.right + kCaptureMargin, region.rect.bottom + kCaptureMargin };
        if (!IntersectRect(&cap, &cap, &clientRc)) return;
        const int cw = cap.right - cap.left, ch = cap.bottom - cap.top;
        if (!EnsureDib(cw, ch)) return;
        BitBlt(memDc, 0, 0, cw, ch, source, cap.left, cap.top, SRCCOPY);

        const D2D1_BITMAP_PROPERTIES1 srcProps = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        ComPtr<ID2D1Bitmap1> captured;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(cw), static_cast<UINT32>(ch)), bits,
            static_cast<UINT32>(dibW) * 4, &srcProps, &captured))) return;

        ID2D1Bitmap1* map = GetMap(rw, rh, region.radius);
        if (map == nullptr) return;

        // Everything below is in coordinates local to the captured rectangle
        const float lx = static_cast<float>(region.rect.left - cap.left);
        const float ly = static_cast<float>(region.rect.top - cap.top);
        const D2D1_RECT_F local = D2D1::RectF(lx, ly, lx + rw, ly + rh);
        const float radius = std::min(region.radius, std::min(rw, rh) * 0.5f);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(cap.left), static_cast<float>(cap.top)));

        // 1) Refracting rim: displaced copy of the content, drawn only inside a ring along the edge
        ComPtr<ID2D1Effect> mapShift, displacement;
        // Small controls (buttons) only get light and rim: a lagging refracted copy of a hovered button looks broken
        if (rh >= 56 && SUCCEEDED(dc->CreateEffect(kClsidAffine, &mapShift)) &&
            SUCCEEDED(dc->CreateEffect(kClsidDisplacement, &displacement)))
        {
            mapShift->SetInput(0, map);
            mapShift->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, D2D1::Matrix3x2F::Translation(lx, ly));
            displacement->SetInput(0, captured.Get());
            displacement->SetInputEffect(1, mapShift.Get());
            displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE, kRefraction);
            displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_R);
            displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_G);

            ComPtr<ID2D1RoundedRectangleGeometry> outer, inner;
            const D2D1_RECT_F innerRect = D2D1::RectF(local.left + kBand, local.top + kBand,
                local.right - kBand, local.bottom - kBand);
            // Keep the ring thin on short regions
            const float innerInsetY = std::min(kBand, rh * 0.32f);
            const D2D1_RECT_F innerRectSafe = D2D1::RectF(innerRect.left, local.top + innerInsetY,
                innerRect.right, local.bottom - innerInsetY);
            if (SUCCEEDED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(local, radius, radius), &outer)) &&
                SUCCEEDED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(innerRectSafe,
                    std::max(radius - kBand, 1.0f), std::max(radius - kBand, 1.0f)), &inner)))
            {
                ID2D1Geometry* geometries[] = { outer.Get(), inner.Get() };
                ComPtr<ID2D1GeometryGroup> ring;
                if (SUCCEEDED(factory->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, geometries, 2, &ring)))
                {
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), ring.Get()), nullptr);
                    dc->DrawImage(displacement.Get());
                    dc->PopLayer();
                }
            }
        }

        // 2) Frosted tint: very light so controls underneath stay crisp
        const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(local, radius, radius);
        {
            const D2D1_GRADIENT_STOP stops[] = {
                { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f) },
                { 0.55f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f) },
                { 1.0f, D2D1::ColorF(0.35f, 0.38f, 0.48f, 0.12f) } };
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> brush;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &collection)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(0, local.top), D2D1::Point2F(0, local.bottom)), collection.Get(), &brush)))
                dc->FillRoundedRectangle(shape, brush.Get());
        }

        // 2b) Interactive light: a soft glow and a lit rim that follow the cursor
        {
            const float dx = std::max({ static_cast<float>(region.rect.left) - cursorX, 0.0f, cursorX - static_cast<float>(region.rect.right) });
            const float dy = std::max({ static_cast<float>(region.rect.top) - cursorY, 0.0f, cursorY - static_cast<float>(region.rect.bottom) });
            const float proximity = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy) / 70.0f, 0.0f, 1.0f);
            if (proximity > 0.01f)
            {
                const D2D1_POINT_2F center = D2D1::Point2F(cursorX - cap.left, cursorY - cap.top);
                ComPtr<ID2D1RoundedRectangleGeometry> clip;
                ComPtr<ID2D1GradientStopCollection> glowStops, rimStops;
                ComPtr<ID2D1RadialGradientBrush> glow, rimLight;
                const D2D1_GRADIENT_STOP g1[] = {
                    { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f * proximity) },
                    { 0.45f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f * proximity) },
                    { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f) } };
                const D2D1_GRADIENT_STOP g2[] = {
                    { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f * proximity) },
                    { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f) } };
                if (SUCCEEDED(factory->CreateRoundedRectangleGeometry(shape, &clip)) &&
                    SUCCEEDED(dc->CreateGradientStopCollection(g1, 3, &glowStops)) &&
                    SUCCEEDED(dc->CreateGradientStopCollection(g2, 2, &rimStops)) &&
                    SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center,
                        D2D1::Point2F(0, 0), 78.0f, 78.0f), glowStops.Get(), &glow)) &&
                    SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(center,
                        D2D1::Point2F(0, 0), 96.0f, 96.0f), rimStops.Get(), &rimLight)))
                {
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.Get()), nullptr);
                    dc->FillRectangle(local, glow.Get());
                    dc->PopLayer();
                    D2D1_ROUNDED_RECT lit = shape;
                    lit.rect = D2D1::RectF(local.left + 1.0f, local.top + 1.0f, local.right - 1.0f, local.bottom - 1.0f);
                    dc->DrawRoundedRectangle(lit, rimLight.Get(), 2.4f);
                }
            }
        }

        // 2c) Shaded edge: gives the glass thickness on light backgrounds (opposite side of the highlight)
        {
            const D2D1_GRADIENT_STOP stops[] = {
                { 0.0f, D2D1::ColorF(0.15f, 0.17f, 0.25f, 0.0f) },
                { 0.45f, D2D1::ColorF(0.15f, 0.17f, 0.25f, 0.10f) },
                { 1.0f, D2D1::ColorF(0.15f, 0.17f, 0.25f, 0.42f) } };
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> brush;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, &collection)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(local.left, local.top), D2D1::Point2F(local.right, local.bottom)),
                    collection.Get(), &brush)))
            {
                D2D1_ROUNDED_RECT edge = shape;
                edge.rect = D2D1::RectF(local.left - 0.2f, local.top - 0.2f, local.right + 0.2f, local.bottom + 0.2f);
                dc->DrawRoundedRectangle(edge, brush.Get(), 1.2f);
            }
        }

        // 3) Specular rim: bright where light hits (top-left), softer at the opposite edge
        {
            const D2D1_GRADIENT_STOP stops[] = {
                { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f) },
                { 0.3f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f) },
                { 0.7f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) },
                { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.75f) } };
            ComPtr<ID2D1GradientStopCollection> collection;
            ComPtr<ID2D1LinearGradientBrush> brush;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 4, &collection)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(local.left, local.top), D2D1::Point2F(local.right, local.bottom)),
                    collection.Get(), &brush)))
            {
                D2D1_ROUNDED_RECT rim = shape;
                rim.rect = D2D1::RectF(local.left + 0.7f, local.top + 0.7f, local.right - 0.7f, local.bottom - 0.7f);
                dc->DrawRoundedRectangle(rim, brush.Get(), 1.4f);

                D2D1_ROUNDED_RECT glow = shape;
                glow.rect = D2D1::RectF(local.left + 2.6f, local.top + 2.6f, local.right - 2.6f, local.bottom - 2.6f);
                glow.radiusX = glow.radiusY = std::max(radius - 2.0f, 1.0f);
                brush->SetOpacity(0.35f);
                dc->DrawRoundedRectangle(glow, brush.Get(), 1.6f);
            }
        }
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // Image list glyph -> D2D bitmap (cached). Image list bitmaps are 32 bpp; straight alpha is premultiplied here.
    ID2D1Bitmap1* Overlay::GetIcon(const HIMAGELIST list, const int index)
    {
        for (const auto& [key, bitmap] : icons) if (key.first == list && key.second == index) return bitmap.Get();
        if (icons.size() > 96) icons.clear();
        IMAGEINFO info{};
        if (list == nullptr || !ImageList_GetImageInfo(list, index, &info) || info.hbmImage == nullptr) return nullptr;
        BITMAP bm{};
        if (GetObjectW(info.hbmImage, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) return nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> full(static_cast<size_t>(bm.bmWidth) * static_cast<size_t>(bm.bmHeight));
        const HDC screen = GetDC(nullptr);
        const int rows = GetDIBits(screen, info.hbmImage, 0, static_cast<UINT>(bm.bmHeight), full.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, screen);
        if (rows != bm.bmHeight) return nullptr;

        const int w = info.rcImage.right - info.rcImage.left, h = info.rcImage.bottom - info.rcImage.top;
        if (w <= 0 || h <= 0) return nullptr;
        std::vector<uint32_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h));
        bool straight = false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const uint32_t p = full[static_cast<size_t>(info.rcImage.top + y) * bm.bmWidth + info.rcImage.left + x];
                pixels[static_cast<size_t>(y) * w + x] = p;
                const uint32_t a = p >> 24;
                if (((p >> 16) & 0xFF) > a + 1 || ((p >> 8) & 0xFF) > a + 1 || (p & 0xFF) > a + 1) straight = true;
            }
        if (straight)
            for (auto& p : pixels)
            {
                const uint32_t a = p >> 24;
                p = (a << 24) | ((((p >> 16) & 0xFF) * a / 255) << 16) | ((((p >> 8) & 0xFF) * a / 255) << 8) | ((p & 0xFF) * a / 255);
            }
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        ComPtr<ID2D1Bitmap1> bitmap;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), pixels.data(),
            static_cast<UINT32>(w) * 4, &props, &bitmap))) return nullptr;
        icons.emplace_back(std::make_pair(list, index), bitmap);
        return icons.back().second.Get();
    }

    IDWriteTextFormat* Overlay::GetFormat(const int px, const DWRITE_TEXT_ALIGNMENT align)
    {
        if (dwrite == nullptr) return nullptr;
        const int formatKey = px * 4 + static_cast<int>(align);
        for (const auto& [key, format] : formats) if (key == formatKey) return format.Get();
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(px), L"", &format))) return nullptr;
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetTextAlignment(align);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format.Get(), &sign)))
        {
            const DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            format->SetTrimming(&trimming, sign.Get());
        }
        formats.emplace_back(formatKey, format);
        return formats.back().second.Get();
    }

    // Eases hover / press towards their targets. Returns true while anything is still moving.
    bool Overlay::StepAnimation(const std::vector<GlassIcon>& list)
    {
        bool moving = false;
        for (const auto& icon : list)
        {
            auto& [hover, press] = anim[icon.id];
            const float th = icon.hot ? 1.0f : 0.0f, tp = icon.pressed ? 1.0f : 0.0f;
            hover += (th - hover) * 0.28f;
            press += (tp - press) * 0.4f;
            if (std::fabs(th - hover) < 0.01f) hover = th;
            if (std::fabs(tp - press) < 0.01f) press = tp;
            moving |= hover != th || press != tp;
        }
        return moving;
    }

    void Overlay::DrawIcon(const GlassIcon& icon)
    {
        const auto [hover, press] = anim[icon.id];
        if (icon.glyph != 0)
        {
            const float cx = static_cast<float>(icon.rect.left + icon.rect.right) * 0.5f, cy = static_cast<float>(icon.rect.top + icon.rect.bottom) * 0.5f;
            const int px = std::max(12, static_cast<int>(std::lround((icon.rect.bottom - icon.rect.top) * 0.50f)));
            if (IDWriteTextFormat* format = GetGlyphFormat(px))
            {
                const float scale = 1.0f + 0.12f * hover - 0.10f * press;
                ComPtr<ID2D1SolidColorBrush> brush;
                // Ink for available commands, a faint ink when unavailable, accent for the primary / active ones
                const D2D1_COLOR_F inkBase = dark ? D2D1::ColorF(0.91f, 0.93f, 0.97f) : D2D1::ColorF(0.13f, 0.14f, 0.19f);
                const D2D1_COLOR_F ink = !icon.enabled ? D2D1::ColorF(inkBase.r, inkBase.g, inkBase.b, 0.32f)
                    : (icon.accent || icon.checked) ? (dark ? D2D1::ColorF(0.42f, 0.68f, 1.0f) : D2D1::ColorF(0.04f, 0.46f, 0.98f)) : inkBase;
                if (SUCCEEDED(dc->CreateSolidColorBrush(ink, &brush)))
                {
                    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy)));
                    const wchar_t text[2] = { icon.glyph, L'\0' };
                    dc->DrawText(text, 1, format, D2D1::RectF(cx - px, cy - px, cx + px, cy + px), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                    dc->SetTransform(D2D1::Matrix3x2F::Identity());
                    return;
                }
            }
        }
        ID2D1Bitmap1* bitmap = GetIcon(icon.list, icon.index);
        if (bitmap == nullptr) return;
        const D2D1_SIZE_F size = bitmap->GetSize();
        const float cx = static_cast<float>(icon.rect.left + icon.rect.right) * 0.5f;
        const float cy = static_cast<float>(icon.rect.top + icon.rect.bottom) * 0.5f;
        const float scale = 1.0f + 0.16f * hover - 0.12f * press;
        if (icon.checked)
        {
            const float side = std::min(static_cast<float>(icon.rect.right - icon.rect.left),
                static_cast<float>(icon.rect.bottom - icon.rect.top)) - 2.0f;
            ComPtr<ID2D1SolidColorBrush> fill, line;
            if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f), &fill)) &&
                SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.22f, 0.35f, 0.25f), &line)))
            {
                const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(D2D1::RectF(cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2), 8, 8);
                dc->FillRoundedRectangle(pill, fill.Get());
                dc->DrawRoundedRectangle(pill, line.Get(), 1.2f);
            }
        }
        const float w = size.width * scale, h = size.height * scale;
        dc->DrawBitmap(bitmap, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), 1.0f,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }

    void Overlay::DrawLabel(const GlassText& text)
    {
        IDWriteTextFormat* format = GetFormat(text.fontPx);
        if (format == nullptr || text.text.empty()) return;
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(GetRValue(text.color) / 255.0f, GetGValue(text.color) / 255.0f,
            GetBValue(text.color) / 255.0f), &brush))) return;
        const D2D1_RECT_F rect = D2D1::RectF(static_cast<float>(text.rect.left), static_cast<float>(text.rect.top),
            static_cast<float>(text.rect.right), static_cast<float>(text.rect.bottom));
        dc->DrawText(text.text.c_str(), static_cast<UINT32>(text.text.size()), format, rect, brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    bool Overlay::StepRows(const std::vector<GlassRow>& rows)
    {
        bool moving = false;
        for (const auto& row : rows)
        {
            auto& [hover, unused] = anim[row.id];
            const float target = row.hot ? 1.0f : 0.0f;
            hover += (target - hover) * 0.3f;
            if (std::fabs(target - hover) < 0.01f) hover = target;
            moving |= hover != target;
        }
        return moving;
    }

    // Soft frosted highlight for the row under the cursor
    void Overlay::DrawHoverRow(const GlassRow& row)
    {
        const float hover = anim[row.id].first;
        if (hover <= 0.01f || row.selected) return;
        const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(D2D1::RectF(static_cast<float>(row.rect.left), static_cast<float>(row.rect.top),
            static_cast<float>(row.rect.right), static_cast<float>(row.rect.bottom)), row.radius, row.radius);
        ComPtr<ID2D1SolidColorBrush> fill, rim;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f * hover), &fill)) ||
            FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f * hover), &rim))) return;
        dc->FillRoundedRectangle(shape, fill.Get());
        D2D1_ROUNDED_RECT inner = shape;
        inner.rect = D2D1::RectF(shape.rect.left + 0.7f, shape.rect.top + 0.7f, shape.rect.right - 0.7f, shape.rect.bottom - 0.7f);
        dc->DrawRoundedRectangle(inner, rim.Get(), 1.2f);
        ComPtr<ID2D1SolidColorBrush> edge;
        if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.22f, 0.35f, 0.14f * hover), &edge)))
            dc->DrawRoundedRectangle(shape, edge.Get(), 1.0f);
    }

    size_t Overlay::Signature(const GlassLayer& layer)
    {
        size_t h = 1469598103934665603ull;
        const auto mix = [&h](const size_t v) { h = (h ^ v) * 1099511628211ull; };
        const auto mixRect = [&](const RECT& r)
        {
            mix(static_cast<size_t>(r.left)); mix(static_cast<size_t>(r.top));
            mix(static_cast<size_t>(r.right)); mix(static_cast<size_t>(r.bottom));
        };
        for (const auto& r : layer.regions) { mixRect(r.rect); mix(static_cast<size_t>(r.radius * 8)); }
        for (const auto& i : layer.icons)
        {
            mixRect(i.rect);
            mix(static_cast<size_t>(i.index)); mix(reinterpret_cast<size_t>(i.list));
            mix(static_cast<size_t>(i.hot) | static_cast<size_t>(i.pressed) << 1 | static_cast<size_t>(i.checked) << 2);
        }
        for (const auto& r : layer.rows)
        {
            mixRect(r.rect); mixRect(r.clip);
            mix(static_cast<size_t>(r.selected) | static_cast<size_t>(r.hot) << 1 | static_cast<size_t>(r.focused) << 2);
        }
        for (const auto& t : layer.texts) { mixRect(t.rect); mix(std::hash<std::wstring>{}(t.text)); }
        for (const auto& l : layer.lists) mix(l->hash);
        for (const auto& t : layer.tabStrips) mix(t.hash);
        for (const auto& m : layer.treeMaps) mix(m.hash);
        for (const auto& b : layer.splitters) mix(b.hash);
        for (const auto& b : layer.menuBars) mix(b.hash);
        for (const auto& g : layer.groups) mixRect(g);
        for (const auto& i : layer.icons) mix(static_cast<size_t>(i.enabled) | static_cast<size_t>(i.accent) << 1);
        mix(static_cast<size_t>(layer.dark));
        mix(static_cast<size_t>(layer.scale * 100.0f));
        return h;
    }

    // HICON -> D2D bitmap (cached). GDI blends alpha icons into a zeroed 32 bpp DIB with premultiplied results.
    ID2D1Bitmap1* Overlay::GetHIcon(const HICON icon)
    {
        if (icon == nullptr) return nullptr;
        for (const auto& [key, bitmap] : hicons) if (key == icon) return bitmap.Get();
        if (hicons.size() > 256) hicons.clear();
        ICONINFO info{};
        if (!GetIconInfo(icon, &info)) return nullptr;
        BITMAP bm{};
        const HBITMAP probe = info.hbmColor != nullptr ? info.hbmColor : info.hbmMask;
        const bool ok = probe != nullptr && GetObjectW(probe, sizeof(bm), &bm) != 0;
        if (info.hbmColor != nullptr) DeleteObject(info.hbmColor);
        if (info.hbmMask != nullptr) DeleteObject(info.hbmMask);
        if (!ok) return nullptr;
        const int w = bm.bmWidth, h = info.hbmColor != nullptr ? bm.bmHeight : bm.bmHeight / 2;
        if (w <= 0 || h <= 0 || w > 256 || h > 256) return nullptr;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HDC mem = CreateCompatibleDC(nullptr);
        const HBITMAP dibSection = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dibSection == nullptr || bits == nullptr) { DeleteDC(mem); return nullptr; }
        const HGDIOBJ old = SelectObject(mem, dibSection);
        std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        DrawIconEx(mem, 0, 0, icon, w, h, 0, nullptr, DI_NORMAL);
        std::vector<uint32_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h));
        std::memcpy(pixels.data(), bits, pixels.size() * 4);
        SelectObject(mem, old);
        DeleteObject(dibSection);
        DeleteDC(mem);

        // Old style icons leave alpha at zero: make every painted pixel opaque
        const bool hasAlpha = std::ranges::any_of(pixels, [](const uint32_t p) { return (p >> 24) != 0; });
        if (!hasAlpha)
            for (auto& p : pixels) if ((p & 0x00FFFFFFu) != 0) p |= 0xFF000000u;

        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        ComPtr<ID2D1Bitmap1> bitmap;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), pixels.data(),
            static_cast<UINT32>(w) * 4, &props, &bitmap))) return nullptr;
        hicons.emplace_back(icon, bitmap);
        return hicons.back().second.Get();
    }

    // Paints a whole list: card, header, rows (text, tree, bars, swatches), selection and slim scrollbars
    void Overlay::DrawList(const GlassListView& v)
    {
        const auto rf = [](const RECT& r) { return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom)); };
        const auto color = [](const COLORREF c, const float a = 1.0f) { return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a); };
        const auto mixColor = [](const COLORREF a, const COLORREF b, const float t)
        {
            const auto ch = [t](const int x, const int y) { return static_cast<int>(x + (y - x) * t + 0.5f); };
            return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
        };
        const D2D1_RECT_F listF = rf(v.list);
        const COLORREF cardBack = v.dark ? RGB(31, 33, 40) : RGB(252, 252, 254);
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) return;
        const auto fill = [&](const D2D1_RECT_F& r, const float radius, const D2D1_COLOR_F& c)
        {
            brush->SetColor(c);
            if (radius > 0.5f) dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
            else dc->FillRectangle(r, brush.Get());
        };
        const auto stroke = [&](const D2D1_RECT_F& r, const float radius, const D2D1_COLOR_F& c, const float width)
        {
            brush->SetColor(c);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get(), width);
        };

        // Clip to the rounded pane and to the list window
        ComPtr<ID2D1RoundedRectangleGeometry> cardGeometry;
        if (FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rf(v.card), static_cast<float>(v.cardRadius), static_cast<float>(v.cardRadius)), &cardGeometry))) return;
        dc->PushAxisAlignedClip(listF, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), cardGeometry.Get()), nullptr);
        fill(listF, 0, color(cardBack));

        // Header: quiet capsule, soft titles, sort chevron in its own slot
        const float rowH = static_cast<float>(v.rowHeight);
        const D2D1_RECT_F headerF = rf(v.header);
        const float headerH = headerF.bottom - headerF.top;
        {
            D2D1_RECT_F capsule = D2D1::RectF(static_cast<float>(v.client.left) + S(4), headerF.top + S(3), static_cast<float>(v.client.right) - S(4), headerF.bottom - S(3));
            fill(capsule, (capsule.bottom - capsule.top) / 2, v.dark ? D2D1::ColorF(1, 1, 1, 0.06f) : D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.06f));
        }
        const COLORREF softText = v.dark ? RGB(176, 182, 196) : RGB(98, 104, 118);
        dc->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(v.client.left), headerF.top, static_cast<float>(v.client.right), headerF.bottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t ci = 0; ci < v.columns.size(); ++ci)
        {
            GlassListColumn col = v.columns[ci];
            if (!v.rows.empty() && ci < v.rows.front().cells.size() && v.rows.front().cells[ci].kind == GlassCell::Kind::Bar) col.rightAligned = false;
            const D2D1_RECT_F tr = D2D1::RectF(static_cast<float>(col.left) + S(col.rightAligned ? 6.0f : 8.0f), headerF.top, static_cast<float>(col.right) - S(6), headerF.bottom);
            const float labelWidth = std::min(TextWidth(col.title, v.fontPx), tr.right - tr.left);
            // A sorted column keeps room for the chevron beside its label
            D2D1_RECT_F textRect = tr;
            if (col.sort != 0 && col.rightAligned) textRect.right -= 0;
            if (IDWriteTextFormat* format = GetFormat(v.fontPx, col.rightAligned ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING))
            {
                brush->SetColor(color(softText));
                dc->DrawText(col.title.c_str(), static_cast<UINT32>(col.title.size()), format, textRect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (col.sort != 0)
            {
                const float cy = (headerF.top + headerF.bottom) * 0.5f;
                const float cx = col.rightAligned ? std::max(static_cast<float>(col.left) + S(8.0f), tr.right - labelWidth - S(11.0f))
                                                  : std::min(static_cast<float>(col.right) - S(8.0f), tr.left + labelWidth + S(11.0f));
                const float tip = col.sort == 1 ? cy - S(2.2f) : cy + S(2.2f), base = col.sort == 1 ? cy + S(2.2f) : cy - S(2.2f);
                brush->SetColor(color(softText));
                dc->DrawLine(D2D1::Point2F(cx - S(4), base), D2D1::Point2F(cx, tip), brush.Get(), S(1.5f), roundStroke());
                dc->DrawLine(D2D1::Point2F(cx, tip), D2D1::Point2F(cx + S(4), base), brush.Get(), S(1.5f), roundStroke());
            }
        }
        dc->PopAxisAlignedClip();

        // Rows
        const float bodyTop = headerF.bottom;
        dc->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(v.client.left), bodyTop, static_cast<float>(v.client.right), static_cast<float>(v.client.bottom)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float iconPx = static_cast<float>(v.iconPx);
        for (const auto& row : v.rows)
        {
            const float top = static_cast<float>(row.top), bottom = static_cast<float>(row.bottom);
            if (bottom <= bodyTop) continue;
            if (!row.selected && !row.hot)
                fill(D2D1::RectF(static_cast<float>(v.client.left) + S(16), bottom - 0.5f * scale, static_cast<float>(v.client.right) - S(16), bottom + 0.5f * scale), 0.5f,
                    v.dark ? D2D1::ColorF(1, 1, 1, 0.05f) : D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.06f));
            if (row.striped) fill(D2D1::RectF(static_cast<float>(v.client.left), top, static_cast<float>(v.client.right), bottom), 0, v.dark ? D2D1::ColorF(1, 1, 1, 0.025f) : D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.03f));
            const D2D1_RECT_F capsule = D2D1::RectF(static_cast<float>(v.client.left) + S(5), top + S(2), static_cast<float>(v.client.right) - S(5), bottom - S(2));
            const float radius = (capsule.bottom - capsule.top) / 2 + 1;
            COLORREF textColor = v.text;
            if (row.selected)
            {
                const COLORREF base = v.highlight;
                ComPtr<ID2D1GradientStopCollection> stops;
                ComPtr<ID2D1LinearGradientBrush> gradient;
                const D2D1_GRADIENT_STOP gs[] = { { 0.0f, color(mixColor(base, RGB(255, 255, 255), 0.22f)) }, { 1.0f, color(base) } };
                if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 2, &stops)) &&
                    SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, capsule.top), D2D1::Point2F(0, capsule.bottom)), stops.Get(), &gradient)))
                    dc->FillRoundedRectangle(D2D1::RoundedRect(capsule, radius, radius), gradient.Get());
                stroke(D2D1::RectF(capsule.left + 0.7f, capsule.top + 0.7f, capsule.right - 0.7f, capsule.bottom - 0.7f), radius, D2D1::ColorF(1, 1, 1, 0.45f), 1.2f);
                textColor = v.highlightText;
            }
            else if (row.hot)
            {
                fill(capsule, radius, v.dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0.24f, 0.32f, 0.6f, 0.07f));
                stroke(capsule, radius, v.dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0.24f, 0.32f, 0.6f, 0.12f), 1.0f);
            }

            for (size_t c = 0; c < v.columns.size() && c < row.cells.size(); ++c)
            {
                const auto& col = v.columns[c];
                const auto& cell = row.cells[c];
                const float x0 = static_cast<float>(col.left), x1 = static_cast<float>(col.right);
                COLORREF cellColor = cell.textColor != CLR_NONE ? cell.textColor : v.text;
                if (row.selected) cellColor = textColor;
                else if (c > 0 && cell.kind == GlassCell::Kind::Text)
                {
                    // Leading figures keep full ink; the rest recede, the least important the most
                    const int weight = col.weight;
                    if (weight < 1) cellColor = mixColor(cellColor, cardBack, weight < 0 ? 0.54f : 0.36f);
                }
                float textLeft = x0 + S(6), textRight = x1 - S(6);

                switch (cell.kind)
                {
                case GlassCell::Kind::Bar:
                {
                    D2D1_RECT_F bar = D2D1::RectF(x0 + S(2) + static_cast<float>(cell.barIndent), top + S(5), x1 - S(2), bottom - S(5));
                    if (bar.right - bar.left <= S(4)) break;
                    const float h = bar.bottom - bar.top, r = h / 2;
                    fill(bar, r, v.dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.10f));
                    if (cell.busy)
                    {
                        const float cycle = static_cast<float>(GetTickCount64() % 1500) / 1500.0f, w = bar.right - bar.left, block = w / 4;
                        const float a = std::clamp(bar.left + cycle * (w + block) - block, bar.left, bar.right), b = std::clamp(bar.left + cycle * (w + block), bar.left, bar.right);
                        if (b - a > 1) fill(D2D1::RectF(a, bar.top, b, bar.bottom), r, D2D1::ColorF(0.30f, 0.60f, 1.0f, 0.85f));
                        break;
                    }
                    const COLORREF track = v.dark ? RGB(60, 62, 70) : RGB(232, 234, 240);
                    const float w = bar.right - bar.left;
                    const float subtree = w * static_cast<float>(std::clamp(cell.fraction, 0.0, 1.0));
                    const float absolute = w * static_cast<float>(std::clamp(cell.absolute, 0.0, 1.0));
                    (void)track;
                    const D2D1_COLOR_F share = v.dark ? D2D1::ColorF(0.30f, 0.42f, 0.66f) : D2D1::ColorF(0.72f, 0.83f, 1.0f);
                    const D2D1_COLOR_F whole = v.dark ? D2D1::ColorF(0.38f, 0.64f, 1.0f) : D2D1::ColorF(0.17f, 0.52f, 1.0f);
                    if (subtree >= 1.0f)
                        fill(D2D1::RectF(bar.left, bar.top, bar.left + std::max(subtree, std::min(h, S(6.0f))), bar.bottom), r, share);
                    if (absolute >= 1.0f)
                        fill(D2D1::RectF(bar.left, bar.top, bar.left + std::max(absolute, std::min(h, S(6.0f))), bar.bottom), r, whole);
                    break;
                }
                case GlassCell::Kind::Swatch:
                {
                    D2D1_RECT_F chip = D2D1::RectF(x0 + S(4), top + S(4), x1 - S(4), bottom - S(4));
                    if (chip.right - chip.left < S(6)) break;
                    const float r = (chip.bottom - chip.top) / 2;
                    ComPtr<ID2D1RadialGradientBrush> glow;
                    ComPtr<ID2D1GradientStopCollection> stops;
                    const D2D1_GRADIENT_STOP gs[] = { { 0.0f, color(mixColor(cell.color, RGB(255, 255, 255), 0.55f)) }, { 1.0f, color(cell.color) } };
                    const D2D1_POINT_2F centre = D2D1::Point2F((chip.left + chip.right) / 2, chip.top + (chip.bottom - chip.top) * 0.38f);
                    if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 2, &stops)) &&
                        SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(0, 0), (chip.right - chip.left) * 0.55f, (chip.bottom - chip.top) * 0.9f), stops.Get(), &glow)))
                        dc->FillRoundedRectangle(D2D1::RoundedRect(chip, r, r), glow.Get());
                    stroke(chip, r, D2D1::ColorF(0, 0, 0, 0.14f), 1.0f);
                    break;
                }
                case GlassCell::Kind::Label:
                {
                    float x = x0 + static_cast<float>(v.generalIndent);
                    if (cell.tree)
                    {
                        x += S(3);
                        if (cell.indent > 0)
                        {
                            x += static_cast<float>((cell.indent - 1) * v.indentStep);
                            if (cell.hasChildren)
                            {
                                const float cx = x + rowH / 2, cy = (top + bottom) / 2;
                                brush->SetColor(color(row.selected ? v.highlightText : softText));
                                const float a = cell.expanded ? 0.0f : 1.0f;
                                if (cell.expanded)
                                {
                                    dc->DrawLine(D2D1::Point2F(cx - S(4), cy - S(2)), D2D1::Point2F(cx, cy + S(2)), brush.Get(), S(1.5f), roundStroke());
                                    dc->DrawLine(D2D1::Point2F(cx, cy + S(2)), D2D1::Point2F(cx + S(4), cy - S(2)), brush.Get(), S(1.5f), roundStroke());
                                }
                                else
                                {
                                    dc->DrawLine(D2D1::Point2F(cx - S(2) + a * 0, cy - S(4)), D2D1::Point2F(cx + S(2), cy), brush.Get(), S(1.5f), roundStroke());
                                    dc->DrawLine(D2D1::Point2F(cx + S(2), cy), D2D1::Point2F(cx - S(2), cy + S(4)), brush.Get(), S(1.5f), roundStroke());
                                }
                            }
                            x += rowH;
                        }
                    }
                    if (cell.glyph != 0)
                    {
                        if (IDWriteTextFormat* gf = GetGlyphFormat(static_cast<int>(iconPx)))
                        {
                            const float cy = (top + bottom) / 2;
                            const wchar_t text[2] = { cell.glyph, L'\0' };
                            brush->SetColor(row.selected ? color(v.highlightText) : D2D1::ColorF(0.92f, 0.58f, 0.04f));
                            dc->DrawText(text, 1, gf, D2D1::RectF(x, cy - iconPx, x + iconPx, cy + iconPx), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                        }
                    }
                    else if (ID2D1Bitmap1* iconBitmap = GetHIcon(cell.icon))
                    {
                        const float cy = (top + bottom) / 2;
                        dc->DrawBitmap(iconBitmap, D2D1::RectF(x, cy - iconPx / 2, x + iconPx, cy + iconPx / 2), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    }
                    textLeft = x + iconPx + S(6);
                    [[fallthrough]];
                }
                case GlassCell::Kind::Text:
                default:
                {
                    if (cell.text.empty() || textRight <= textLeft) break;
                    const bool right = col.rightAligned && cell.kind == GlassCell::Kind::Text;
                    if (IDWriteTextFormat* format = GetFormat(v.fontPx, right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING))
                    {
                        brush->SetColor(color(cellColor));
                        dc->DrawText(cell.text.c_str(), static_cast<UINT32>(cell.text.size()), format, D2D1::RectF(textLeft, top, textRight, bottom), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    break;
                }
                }
            }
        }
        dc->PopAxisAlignedClip();

        // Slim scrollbars over the real (invisible-to-the-eye, still functional) ones
        for (const GlassScrollBar* bar : { &v.vscroll, &v.hscroll })
        {
            if (!bar->visible) continue;
            const bool vertical = bar == &v.vscroll;
            D2D1_RECT_F thumb = rf(bar->thumb);
            if (vertical) { const float cx = (thumb.left + thumb.right) / 2; thumb.left = cx - S(3); thumb.right = cx + S(3); thumb.top += S(2); thumb.bottom -= S(2); }
            else { const float cy = (thumb.top + thumb.bottom) / 2; thumb.top = cy - S(3); thumb.bottom = cy + S(3); thumb.left += S(2); thumb.right -= S(2); }
            if (thumb.right - thumb.left > 1 && thumb.bottom - thumb.top > 1)
                fill(thumb, S(3), v.dark ? D2D1::ColorF(1, 1, 1, 0.30f) : D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.38f));
        }
        dc->PopLayer();
        dc->PopAxisAlignedClip();

        // Fine rim around the pane
        stroke(rf(v.card), static_cast<float>(v.cardRadius), v.dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.12f), 1.2f);
    }

    // Segmented tab strip: soft capsule track, raised white active tab, accent label
    void Overlay::DrawTabStrip(const GlassTabStrip& s)
    {
        const auto rf = [](const RECT& r) { return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom)); };
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) return;
        ComPtr<ID2D1RoundedRectangleGeometry> paneGeometry;
        if (FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rf(s.pane), static_cast<float>(s.paneRadius), static_cast<float>(s.paneRadius)), &paneGeometry))) return;
        const D2D1_RECT_F strip = rf(s.strip);
        dc->PushAxisAlignedClip(strip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), paneGeometry.Get()), nullptr);
        brush->SetColor(s.dark ? D2D1::ColorF(0.122f, 0.129f, 0.157f) : D2D1::ColorF(0.988f, 0.988f, 0.996f));
        dc->FillRectangle(strip, brush.Get());

        // Track hugging the tabs
        float left = 1e9f, right = -1e9f;
        for (const auto& t : s.tabs) { left = std::min(left, static_cast<float>(t.rect.left)); right = std::max(right, static_cast<float>(t.rect.right)); }
        const D2D1_RECT_F track = D2D1::RectF(left + S(2), strip.top + S(3), right - S(2), strip.bottom - S(3));
        const float trackR = (track.bottom - track.top) / 2;
        brush->SetColor(s.dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.08f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(track, trackR, trackR), brush.Get());

        for (const auto& t : s.tabs)
        {
            const D2D1_RECT_F pill = D2D1::RectF(static_cast<float>(t.rect.left) + S(3), strip.top + S(5), static_cast<float>(t.rect.right) - S(3), strip.bottom - S(5));
            const float r = (pill.bottom - pill.top) / 2;
            if (t.active)
            {
                brush->SetColor(D2D1::ColorF(0.15f, 0.18f, 0.3f, 0.10f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pill.left, pill.top + 1.2f, pill.right, pill.bottom + 1.2f), r, r), brush.Get());
                brush->SetColor(s.dark ? D2D1::ColorF(0.30f, 0.32f, 0.40f) : D2D1::ColorF(1, 1, 1));
                dc->FillRoundedRectangle(D2D1::RoundedRect(pill, r, r), brush.Get());
                brush->SetColor(D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.14f));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(pill, r, r), brush.Get(), 1.0f);
            }
            if (IDWriteTextFormat* format = GetFormat(s.fontPx, DWRITE_TEXT_ALIGNMENT_CENTER))
            {
                brush->SetColor(t.active ? D2D1::ColorF(0.04f, 0.52f, 1.0f) : (s.dark ? D2D1::ColorF(0.78f, 0.8f, 0.84f) : D2D1::ColorF(0.31f, 0.33f, 0.37f)));
                dc->DrawText(t.label.c_str(), static_cast<UINT32>(t.label.size()), format, pill, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        dc->PopLayer();
        dc->PopAxisAlignedClip();
    }

    // Advances the selection outline and hover lens; true while anything still moves
    bool Overlay::StepTreeMap(const GlassTreeMap& m)
    {
        bool moving = false;
        const bool hasSelection = m.single && m.selected.size() == 1;
        if (hasSelection)
        {
            if (selectAlpha < 0.05f) selectSpring.Snap(m.selected[0]);
            selectSpring.Target(m.selected[0]);
        }
        selectAlpha += ((hasSelection ? 1.0f : 0.0f) - selectAlpha) * 0.25f;
        if (std::fabs((hasSelection ? 1.0f : 0.0f) - selectAlpha) < 0.01f) selectAlpha = hasSelection ? 1.0f : 0.0f;
        if (selectSpring.init && selectAlpha > 0.0f) { selectSpring.Step(); moving |= selectSpring.Moving(); }
        moving |= selectAlpha != (hasSelection ? 1.0f : 0.0f);

        if (m.hasHover)
        {
            hoverSpring.Jump(m.hover, hoverAlpha);
        }
        hoverAlpha += ((m.hasHover ? 1.0f : 0.0f) - hoverAlpha) * 0.22f;
        if (std::fabs((m.hasHover ? 1.0f : 0.0f) - hoverAlpha) < 0.01f) hoverAlpha = m.hasHover ? 1.0f : 0.0f;
        if (hoverSpring.init && hoverAlpha > 0.0f) { hoverSpring.Step(); moving |= hoverSpring.Moving(); }
        hoverSpring.StepPrev();
        moving |= hoverSpring.prevAlpha > 0.0f;
        moving |= hoverAlpha != (m.hasHover ? 1.0f : 0.0f);
        return moving;
    }

    // Selection becomes a glowing glass outline that glides between items; the hovered item gets a lit glass lens
    void Overlay::DrawTreeMap(const GlassTreeMap& m)
    {
        const auto rf = [](const RECT& r) { return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom)); };
        const auto colorOf = [](const COLORREF c, const float a) { return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a); };
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) return;
        ComPtr<ID2D1RoundedRectangleGeometry> paneGeometry;
        if (FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rf(m.pane), static_cast<float>(m.paneRadius), static_cast<float>(m.paneRadius)), &paneGeometry))) return;
        dc->PushAxisAlignedClip(rf(m.clip), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), paneGeometry.Get()), nullptr);

        const auto outline = [&](const D2D1_RECT_F& r, const float alpha)
        {
            const float radius = std::clamp(std::min(r.right - r.left, r.bottom - r.top) * 0.18f, 2.0f, 9.0f);
            if (std::min(r.right - r.left, r.bottom - r.top) > 12.0f)
            {
                // A soft wash lifts a dull tile and ties it to the outline
                brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f * alpha));
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
            }
            if (std::min(r.right - r.left, r.bottom - r.top) <= 5.0f)
            {
                brush->SetColor(colorOf(m.color, alpha));
                dc->FillRectangle(r, brush.Get());
                return;
            }
            brush->SetColor(colorOf(m.color, 0.28f * alpha));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 1, r.top - 1, r.right + 1, r.bottom + 1), radius + 1, radius + 1), brush.Get(), S(5.0f));
            brush->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * alpha));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), radius, radius), brush.Get(), S(2.2f));
            brush->SetColor(colorOf(m.color, alpha));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 2.6f, r.top + 2.6f, r.right - 2.6f, r.bottom - 2.6f), std::max(radius - 1.5f, 1.0f), std::max(radius - 1.5f, 1.0f)), brush.Get(), S(1.6f));
        };

        // Hover lens (under the selection outline)
        const auto drawHover = [&](const D2D1_RECT_F& r, const float a)
        {
            const float radius = std::clamp(std::min(r.right - r.left, r.bottom - r.top) * 0.18f, 2.0f, 12.0f);
            if (r.right - r.left > 4 && r.bottom - r.top > 4)
            {
                ComPtr<ID2D1RoundedRectangleGeometry> shape;
                ComPtr<ID2D1GradientStopCollection> glowStops, rimStops;
                ComPtr<ID2D1RadialGradientBrush> glow, rimLight;
                const D2D1_POINT_2F light = D2D1::Point2F(std::clamp(curX, r.left, r.right), std::clamp(curY, r.top, r.bottom));
                const float reach = std::max(std::max(r.right - r.left, r.bottom - r.top), 90.0f) * 0.75f;
                const D2D1_GRADIENT_STOP glowGradient[] = { { 0.0f, D2D1::ColorF(1, 1, 1, 0.42f * a) }, { 0.45f, D2D1::ColorF(1, 1, 1, 0.14f * a) }, { 1.0f, D2D1::ColorF(1, 1, 1, 0.0f) } };
                const D2D1_GRADIENT_STOP rimGradient[] = { { 0.0f, D2D1::ColorF(1, 1, 1, 1.0f * a) }, { 1.0f, D2D1::ColorF(1, 1, 1, 0.12f * a) } };
                if (SUCCEEDED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &shape)) &&
                    SUCCEEDED(dc->CreateGradientStopCollection(glowGradient, 3, &glowStops)) &&
                    SUCCEEDED(dc->CreateGradientStopCollection(rimGradient, 2, &rimStops)) &&
                    SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(light, D2D1::Point2F(0, 0), reach * 0.8f, reach * 0.8f), glowStops.Get(), &glow)) &&
                    SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(light, D2D1::Point2F(0, 0), reach, reach), rimStops.Get(), &rimLight)))
                {
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.Get()), nullptr);
                    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.10f * a));
                    dc->FillRectangle(r, brush.Get());
                    dc->FillRectangle(r, glow.Get());
                    dc->PopLayer();
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.8f, r.top + 0.8f, r.right - 0.8f, r.bottom - 0.8f), radius, radius), rimLight.Get(), 1.8f);
                }
            }
        };
        if (hoverSpring.init)
        {
            if (hoverSpring.prevAlpha > 0.02f) drawHover(hoverSpring.PrevRect(), hoverSpring.prevAlpha);
            if (hoverAlpha > 0.01f) drawHover(hoverSpring.Rect(), hoverAlpha);
        }

        if (m.single && selectSpring.init && selectAlpha > 0.01f) outline(selectSpring.Rect(), selectAlpha);
        else if (!m.single) for (const RECT& r : m.selected) outline(rf(r), 1.0f);

        dc->PopLayer();
        dc->PopAxisAlignedClip();
    }

    bool Overlay::StepSplitters(const std::vector<GlassSplitter>& bars)
    {
        bool moving = false;
        for (const auto& bar : bars)
        {
            auto& [swell, glow] = anim[bar.id];
            const float th = bar.hot ? 1.0f : 0.0f, tg = bar.tracking ? 1.0f : 0.0f;
            swell += (th - swell) * 0.25f;
            glow += (tg - glow) * 0.3f;
            if (std::fabs(th - swell) < 0.01f) swell = th;
            if (std::fabs(tg - glow) < 0.01f) glow = tg;
            moving |= swell != th || glow != tg;
        }
        return moving;
    }

    // Grip pill in the middle of the bar: calm when idle, swells when hovered, turns accent blue while dragging
    void Overlay::DrawSplitter(const GlassSplitter& bar)
    {
        const auto [swell, glow] = anim[bar.id];
        const float cx = static_cast<float>(bar.bar.left + bar.bar.right) * 0.5f, cy = static_cast<float>(bar.bar.top + bar.bar.bottom) * 0.5f;
        const float length = S(36.0f + 16.0f * swell), thick = S(4.0f + 2.0f * swell);
        const D2D1_RECT_F pill = bar.vertical
            ? D2D1::RectF(cx - thick / 2, cy - length / 2, cx + thick / 2, cy + length / 2)
            : D2D1::RectF(cx - length / 2, cy - thick / 2, cx + length / 2, cy + thick / 2);
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) return;
        const float r = thick / 2;
        if (swell > 0.01f || glow > 0.01f)
        {
            brush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f * std::max(swell, glow)));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pill.left - 1.5f, pill.top - 1.5f, pill.right + 1.5f, pill.bottom + 1.5f), r + 1.5f, r + 1.5f), brush.Get());
        }
        const float a = 0.22f + 0.28f * swell;
        const float rr = 0.24f + (0.04f - 0.24f) * glow, gg = 0.28f + (0.52f - 0.28f) * glow, bb = 0.42f + (1.0f - 0.42f) * glow;
        if (dark) brush->SetColor(D2D1::ColorF(0.72f + (0.04f - 0.72f) * glow, 0.76f + (0.52f - 0.76f) * glow, 0.88f + (1.0f - 0.88f) * glow, (0.20f + 0.30f * swell) + (0.9f - (0.20f + 0.30f * swell)) * glow));
        else brush->SetColor(D2D1::ColorF(rr, gg, bb, a + (0.9f - a) * glow));
        dc->FillRoundedRectangle(D2D1::RoundedRect(pill, r, r), brush.Get());
    }

    bool Overlay::StepMenuBar(const std::vector<GlassMenuBar>& bars)
    {
        if (bars.empty()) { menuAlpha = 0; return false; }
        const GlassMenuBar& bar = bars[0];
        int target = -1;
        bool open = false;
        for (size_t i = 0; i < bar.items.size(); ++i) if (bar.items[i].open) { target = static_cast<int>(i); open = true; break; }
        if (target < 0) for (size_t i = 0; i < bar.items.size(); ++i) if (bar.items[i].hot) { target = static_cast<int>(i); break; }
        const bool has = target >= 0;
        if (has)
        {
            RECT r = bar.items[static_cast<size_t>(target)].rect;
            menuLens.Jump(r, menuAlpha);
        }
        const float strengthTarget = open ? 1.0f : 0.0f;
        menuStrength += (strengthTarget - menuStrength) * 0.25f;
        const float alphaTarget = has ? 1.0f : 0.0f;
        menuAlpha += (alphaTarget - menuAlpha) * 0.22f;
        if (std::fabs(alphaTarget - menuAlpha) < 0.01f) menuAlpha = alphaTarget;
        if (std::fabs(strengthTarget - menuStrength) < 0.01f) menuStrength = strengthTarget;
        bool moving = menuAlpha != alphaTarget || menuStrength != strengthTarget;
        if (menuLens.init && menuAlpha > 0.0f) { menuLens.Step(); moving |= menuLens.Moving(); }
        menuLens.StepPrev();
        moving |= menuLens.prevAlpha > 0.0f;
        return moving;
    }

    // Plain labels; a glass lens glides to the hovered or opened item and carries the cursor light
    void Overlay::DrawMenuBar(const GlassMenuBar& bar)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) return;
        const auto drawLens = [&](const D2D1_RECT_F& r, const float a)
        {
            const float radius = (r.bottom - r.top) / 2, s = menuStrength;
            brush->SetColor(D2D1::ColorF(1, 1, 1, (dark ? 0.10f + 0.10f * s : 0.30f + 0.28f * s) * a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
            ComPtr<ID2D1RoundedRectangleGeometry> shape;
            ComPtr<ID2D1GradientStopCollection> glowStops, rimStops;
            ComPtr<ID2D1RadialGradientBrush> glow, rimLight;
            const D2D1_POINT_2F light = D2D1::Point2F(std::clamp(curX, r.left, r.right), std::clamp(curY, r.top, r.bottom));
            const float reach = std::max(r.right - r.left, 90.0f) * 0.75f;
            const D2D1_GRADIENT_STOP glowGradient[] = { { 0.0f, D2D1::ColorF(1, 1, 1, 0.55f * a) }, { 0.4f, D2D1::ColorF(1, 1, 1, 0.2f * a) }, { 1.0f, D2D1::ColorF(1, 1, 1, 0.0f) } };
            const D2D1_GRADIENT_STOP rimGradient[] = { { 0.0f, D2D1::ColorF(1, 1, 1, 1.0f * a) }, { 1.0f, D2D1::ColorF(1, 1, 1, 0.0f) } };
            if (SUCCEEDED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &shape)) &&
                SUCCEEDED(dc->CreateGradientStopCollection(glowGradient, 3, &glowStops)) &&
                SUCCEEDED(dc->CreateGradientStopCollection(rimGradient, 2, &rimStops)) &&
                SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(light, D2D1::Point2F(0, 0), reach * 0.8f, reach * 0.8f), glowStops.Get(), &glow)) &&
                SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(light, D2D1::Point2F(0, 0), reach, reach), rimStops.Get(), &rimLight)))
            {
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.Get()), nullptr);
                dc->FillRectangle(r, glow.Get());
                dc->PopLayer();
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.8f, r.top + 0.8f, r.right - 0.8f, r.bottom - 0.8f), radius, radius), rimLight.Get(), 1.8f);
            }
            brush->SetColor(D2D1::ColorF(0.2f, 0.22f, 0.35f, 0.10f * a));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get(), 1.0f);
        };
        if (menuLens.init)
        {
            if (menuLens.prevAlpha > 0.02f) drawLens(menuLens.PrevRect(), menuLens.prevAlpha);
            if (menuAlpha > 0.01f) drawLens(menuLens.Rect(), menuAlpha);
        }
        if (IDWriteTextFormat* format = GetFormat(bar.fontPx, DWRITE_TEXT_ALIGNMENT_CENTER))
        {
            brush->SetColor(D2D1::ColorF(GetRValue(bar.text) / 255.0f, GetGValue(bar.text) / 255.0f, GetBValue(bar.text) / 255.0f));
            for (const auto& item : bar.items)
                dc->DrawText(item.label.c_str(), static_cast<UINT32>(item.label.size()), format,
                    D2D1::RectF(static_cast<float>(item.rect.left), static_cast<float>(item.rect.top), static_cast<float>(item.rect.right), static_cast<float>(item.rect.bottom)),
                    brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    // Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10); empty when neither is installed
    IDWriteTextFormat* Overlay::GetGlyphFormat(const int px)
    {
        if (dwrite == nullptr) return nullptr;
        if (!glyphChecked)
        {
            glyphChecked = true;
            ComPtr<IDWriteFontCollection> collection;
            if (SUCCEEDED(dwrite->GetSystemFontCollection(&collection, FALSE)))
                for (const wchar_t* family : { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" })
                {
                    UINT32 index = 0;
                    BOOL exists = FALSE;
                    if (SUCCEEDED(collection->FindFamilyName(family, &index, &exists)) && exists) { glyphFamily = family; break; }
                }
        }
        if (glyphFamily.empty()) return nullptr;
        for (const auto& [key, format] : glyphFormats) if (key == px) return format.Get();
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwrite->CreateTextFormat(glyphFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(px), L"", &format))) return nullptr;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        glyphFormats.emplace_back(px, format);
        return glyphFormats.back().second.Get();
    }

    float Overlay::TextWidth(const std::wstring& text, const int px)
    {
        IDWriteTextFormat* format = GetFormat(px);
        if (format == nullptr || text.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 10000.0f, 100.0f, &layout))) return 0.0f;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return metrics.width;
    }

    // One soft pill per button group
    void Overlay::DrawGroups(const std::vector<RECT>& groups)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) return;
        for (const RECT& g : groups)
        {
            const D2D1_RECT_F r = D2D1::RectF(static_cast<float>(g.left), static_cast<float>(g.top), static_cast<float>(g.right), static_cast<float>(g.bottom));
            const float radius = (r.bottom - r.top) / 2;
            for (int i = 6; i >= 1; --i) // soft shadow
            {
                brush->SetColor(dark ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.045f * static_cast<float>(7 - i)) : D2D1::ColorF(0.1f, 0.12f, 0.22f, 0.012f * static_cast<float>(7 - i)));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - i, r.top - i + 2, r.right + i, r.bottom + i + 2), radius + i, radius + i), brush.Get(), 1.0f);
            }
            ComPtr<ID2D1GradientStopCollection> stops;
            ComPtr<ID2D1LinearGradientBrush> fill;
            const D2D1_GRADIENT_STOP gs[] = {
                { 0.0f, dark ? D2D1::ColorF(0.23f, 0.24f, 0.30f, 0.94f) : D2D1::ColorF(1, 1, 1, 0.90f) },
                { 1.0f, dark ? D2D1::ColorF(0.17f, 0.18f, 0.23f, 0.92f) : D2D1::ColorF(0.95f, 0.96f, 0.99f, 0.78f) } };
            if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 2, &stops)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom)), stops.Get(), &fill)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), fill.Get());
            brush->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0.24f, 0.28f, 0.42f, 0.10f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get(), 1.0f);
            brush->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(1, 1, 1, 0.9f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), radius - 1, radius - 1), brush.Get(), 1.0f);
        }
    }

    // Lens that glides between hovered toolbar buttons
    bool Overlay::StepToolbar(const GlassLayer& layer)
    {
        const GlassIcon* hot = nullptr;
        for (const auto& icon : layer.icons) if (icon.hot) { hot = &icon; break; }
        if (hot != nullptr)
        {
            RECT r = hot->rect;
            const int side = std::min(r.right - r.left, r.bottom - r.top) - 2;
            const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            const RECT lens{ cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2 };
            toolLens.Jump(lens, toolAlpha);
        }
        const float target = hot != nullptr ? 1.0f : 0.0f;
        toolAlpha += (target - toolAlpha) * 0.25f;
        if (std::fabs(target - toolAlpha) < 0.01f) toolAlpha = target;
        bool moving = toolAlpha != target;
        if (toolLens.init && toolAlpha > 0.0f) { toolLens.Step(); moving |= toolLens.Moving(); }
        toolLens.StepPrev();
        moving |= toolLens.prevAlpha > 0.0f;
        return moving;
    }

    void Overlay::DrawToolbarLens(const D2D1_RECT_F& r, const float a)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush))) return;
        const float radius = std::min((r.bottom - r.top) / 2, 12.0f);
        brush->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.07f * a) : D2D1::ColorF(0.24f, 0.30f, 0.52f, 0.10f * a));
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
        ComPtr<ID2D1RoundedRectangleGeometry> shape;
        ComPtr<ID2D1GradientStopCollection> glowStops, rimStops;
        ComPtr<ID2D1RadialGradientBrush> glow, rimLight;
        const D2D1_POINT_2F light = D2D1::Point2F(std::clamp(curX, r.left, r.right), std::clamp(curY, r.top, r.bottom));
        const float reach = std::max(r.right - r.left, 70.0f) * 0.8f;
        const float lift = dark ? 0.45f : 1.0f; // white highlights are toned down on dark surfaces
        const D2D1_GRADIENT_STOP glowGradient[] = { { 0.0f, D2D1::ColorF(1, 1, 1, 0.85f * a * lift) }, { 0.5f, D2D1::ColorF(1, 1, 1, 0.25f * a * lift) }, { 1.0f, D2D1::ColorF(1, 1, 1, 0.0f) } };
        const D2D1_GRADIENT_STOP rimGradient[] = { { 0.0f, D2D1::ColorF(1, 1, 1, 1.0f * a * lift) }, { 1.0f, D2D1::ColorF(1, 1, 1, 0.1f * a * lift) } };
        if (SUCCEEDED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &shape)) &&
            SUCCEEDED(dc->CreateGradientStopCollection(glowGradient, 3, &glowStops)) &&
            SUCCEEDED(dc->CreateGradientStopCollection(rimGradient, 2, &rimStops)) &&
            SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(light, D2D1::Point2F(0, 0), reach, reach), glowStops.Get(), &glow)) &&
            SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(light, D2D1::Point2F(0, 0), reach * 1.2f, reach * 1.2f), rimStops.Get(), &rimLight)))
        {
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), shape.Get()), nullptr);
            dc->FillRectangle(r, glow.Get());
            dc->PopLayer();
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.8f, r.top + 0.8f, r.right - 0.8f, r.bottom - 0.8f), radius, radius), rimLight.Get(), 1.6f);
        }
        brush->SetColor(D2D1::ColorF(0.2f, 0.22f, 0.35f, 0.09f * a));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get(), 1.0f);
    }

    void Overlay::Render()
    {
        if (failed || dc == nullptr || IsIconic(window) || !IsWindowVisible(window)) return;
        RECT rc{};
        GetClientRect(window, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return;
        if (rc.right != width || rc.bottom != height) Resize(rc.right, rc.bottom);
        if (failed) return;

        // Smooth the cursor. While nothing moves or animates, skip the (costly) layer snapshot
        // on most ticks and only wake at ~10 Hz so live content under the glass stays fresh.
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(window, &cursor);
        const auto tx = static_cast<float>(cursor.x), ty = static_cast<float>(cursor.y);
        if (!seeded) { curX = tx; curY = ty; seeded = true; }
        const bool moving = std::fabs(tx - curX) > 0.4f || std::fabs(ty - curY) > 0.4f;
        curX += (tx - curX) * 0.30f;
        curY += (ty - curY) * 0.30f;
        if (!moving) { curX = tx; curY = ty; }
        bool periodic = false;
        if (!moving && !animating)
        {
            if ((++idle % 6) != 0) return;
            periodic = true;
        }

        const GlassLayer layer = provider ? provider() : GlassLayer{};
        dark = layer.dark;
        scale = std::clamp(layer.scale, 1.0f, 4.0f);
        const std::vector<GlassRegion>& regions = layer.regions;
        animating = StepAnimation(layer.icons);
        animating |= StepRows(layer.rows);
        for (const auto& m : layer.treeMaps) animating |= StepTreeMap(m);
        animating |= StepSplitters(layer.splitters);
        animating |= StepMenuBar(layer.menuBars);
        animating |= StepToolbar(layer);
        if (layer.treeMaps.empty()) { selectAlpha = hoverAlpha = 0; }
        const size_t signature = Signature(layer);
        const bool layerChanged = signature != lastSignature;

        bool nearGlass = false;
        for (const auto& r : regions)
            if (cursor.x > r.rect.left - 90 && cursor.x < r.rect.right + 90 &&
                cursor.y > r.rect.top - 90 && cursor.y < r.rect.bottom + 90) { nearGlass = true; break; }
        if (!(moving && nearGlass) && !animating && !layerChanged && !periodic) return;
        if ((moving && nearGlass) || animating || layerChanged) idle = 0;
        lastSignature = signature;

        const HDC source = GetDC(window);
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        DrawGroups(layer.groups);
        if (source != nullptr) for (const auto& region : regions) DrawRegion(region, source, curX, curY);
        for (const auto& list : layer.lists) DrawList(*list);
        for (const auto& strip : layer.tabStrips) DrawTabStrip(strip);
        for (const auto& map : layer.treeMaps) DrawTreeMap(map);
        for (const auto& bar : layer.splitters) DrawSplitter(bar);
        for (const auto& bar : layer.menuBars) DrawMenuBar(bar);
        for (const auto& row : layer.rows)
        {
            const D2D1_RECT_F clip = D2D1::RectF(static_cast<float>(row.clip.left), static_cast<float>(row.clip.top),
                static_cast<float>(row.clip.right), static_cast<float>(row.clip.bottom));
            dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            DrawHoverRow(row);
            // Selected rows get the glass finish: tint, cursor light, shaded edge and specular rim
            if (source != nullptr && row.selected) DrawRegion({ row.rect, row.radius }, source, curX, curY);
            dc->PopAxisAlignedClip();
        }
        if (toolLens.init)
        {
            if (toolLens.prevAlpha > 0.02f) DrawToolbarLens(toolLens.PrevRect(), toolLens.prevAlpha);
            if (toolAlpha > 0.01f) DrawToolbarLens(toolLens.Rect(), toolAlpha);
        }
        for (const auto& icon : layer.icons) DrawIcon(icon);
        for (const auto& text : layer.texts) DrawLabel(text);
        const HRESULT hr = dc->EndDraw();
        if (source != nullptr) ReleaseDC(window, source);
        if (FAILED(hr))
        {
            failed = true;
            OutputDebugStringW((L"GlassOverlay: EndDraw failed 0x" + std::to_wstring(static_cast<unsigned>(hr))).c_str());
            return;
        }
        swap->Present(0, 0);
    }

    LRESULT CALLBACK OverlaySubclass(const HWND hWnd, const UINT message, const WPARAM wParam, const LPARAM lParam,
        const UINT_PTR id, const DWORD_PTR ref)
    {
        auto* overlay = reinterpret_cast<Overlay*>(ref);
        if (message == WM_TIMER && wParam == kTimerId)
        {
            overlay->Render();
            return 0;
        }
        if (message == WM_NCDESTROY)
        {
            KillTimer(hWnd, kTimerId);
            --g_active;
            RemoveWindowSubclass(hWnd, OverlaySubclass, id);
            delete overlay;
            return DefSubclassProc(hWnd, message, wParam, lParam);
        }
        return DefSubclassProc(hWnd, message, wParam, lParam);
    }
}

bool GlassOverlay::IsActive() { return g_active.load() > 0; }

bool GlassOverlay::Attach(const HWND window, LayerProvider provider)
{
    auto overlay = std::make_unique<Overlay>();
    overlay->window = window;
    overlay->provider = std::move(provider);
    if (!overlay->Init()) return false;
    if (!SetWindowSubclass(window, OverlaySubclass, kSubclassId, reinterpret_cast<DWORD_PTR>(overlay.get())))
        return false;
    overlay.release();
    ++g_active;
    SetTimer(window, kTimerId, kFrameMs, nullptr);
    return true;
}
