// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "TreeMapView.h"
#include "GlassOverlay.h"

void CTreeMapView::DrawEmptyPlaceholder(CDC* pDC, const CRect& rect)
{
    CTreeMap::Options options = COptions::TreeMapOptions;
    options.showExtensions = false;
    options.showFolderFrames = false;
    if (!DarkMode::IsDarkModeActive())
    {
        options.brightness = CColorSpace::GraphPaletteBrightness;
        options.ambientLight = 1.0;
    }

    struct Tile { int x, y, w, h, shade; };
    static constexpr std::array tiles = {
        Tile{  0,  0, 25, 58, 58 }, Tile{ 25,  0, 13, 34, 72 }, Tile{ 25, 34, 13, 24, 48 },
        Tile{  0, 58, 18, 42, 76 }, Tile{ 18, 58, 20, 25, 54 }, Tile{ 18, 83, 20, 17, 88 },
        Tile{ 38,  0, 28, 44, 66 }, Tile{ 38, 44, 14, 31, 82 }, Tile{ 52, 44, 14, 31, 52 },
        Tile{ 38, 75, 28, 25, 92 }, Tile{ 66,  0, 19, 62, 60 }, Tile{ 85,  0, 15, 38, 78 },
        Tile{ 66, 62, 16, 38, 50 }, Tile{ 82, 38, 18, 36, 86 }, Tile{ 82, 74, 18, 26, 68 },
    };

    for (const auto& [x, y, w, h, shade] : tiles)
    {
        CRect tile(
            rect.left + rect.Width()  * x / 100,
            rect.top  + rect.Height() * y / 100,
            rect.left + rect.Width()  * (x + w) / 100,
            rect.top  + rect.Height() * (y + h) / 100);

        const int tileShade = DarkMode::IsDarkModeActive() ? shade : 255 - shade / 2;
        if (tile.Width() > 0 && tile.Height() > 0)
            m_treeMap.DrawColorPreview(pDC->Handle(), tile, RGB(tileShade, tileShade, tileShade), &options);
    }
}

bool CTreeMapView::CreateRenderBitmap(CDC* pDC, const CSize size)
{
    // A top-down DIB lets the treemap renderer write directly into the cached frame.
    const BITMAPINFO bitmapInfo{ .bmiHeader = { .biSize = sizeof(BITMAPINFOHEADER),
        .biWidth = size.cx, .biHeight = -size.cy, .biPlanes = 1,
        .biBitCount = 32, .biCompression = BI_RGB } };
    void* bits = nullptr;
    if (CBitmap bitmap((CreateDIBSection(pDC->Handle(), &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0))); bitmap)
    {
        m_bitmap = std::move(bitmap);
        return true;
    }
    return CGraphView::CreateRenderBitmap(pDC, size);
}

void CTreeMapView::RenderVisualization(CDC* pDC, CRect rect)
{
    if (CWinDirStatModel::Get()->IsZoomed()) DrawZoomFrame(pDC, rect);
    m_treeMap.DrawTreeMap(pDC->Handle(), rect,
        CWinDirStatModel::Get()->GetZoomItem(), &COptions::TreeMapOptions);
}

void CTreeMapView::DrawZoomFrame(CDC* pdc, CRect& rc) const
{
    CRect r  = rc;
    r.bottom = r.top + ZoomFrameWidth;
    pdc->FillSolidRect(r, CWinDirStatModel::Get()->GetZoomColor());

    r = rc;
    r.top = r.bottom - ZoomFrameWidth;
    pdc->FillSolidRect(r, CWinDirStatModel::Get()->GetZoomColor());

    r = rc;
    r.right = r.left + ZoomFrameWidth;
    pdc->FillSolidRect(r, CWinDirStatModel::Get()->GetZoomColor());

    r = rc;
    r.left = r.right - ZoomFrameWidth;
    pdc->FillSolidRect(r, CWinDirStatModel::Get()->GetZoomColor());

    rc.Deflate(ZoomFrameWidth, ZoomFrameWidth);
}

void CTreeMapView::DrawHighlightExtension(CDC* pdc)
{
    CWaitCursor wc;

    const CWinDirStatModel* model = CWinDirStatModel::Get();
    const bool isZoomed = model->IsZoomed();

    const CPoint offset = isZoomed
        ? CPoint(ZoomFrameWidth, ZoomFrameWidth) : CPoint();
    for (const CTreeMap::VisibleItem& visible : m_treeMap.GetVisibleItems())
    {
        const CItem* item = visible.item;
        if (!item->TmiIsLeaf() || !IsExtensionHighlighted(item)) continue;

        CRect rc = visible.rectangle + offset;
        RenderHighlightRectangle(pdc, rc);
    }
}

void CTreeMapView::DrawHover(CDC* pdc)
{
    HighlightSelectedItem(pdc, GetDisplayItem(m_hoverItem), false, true);
}

void CTreeMapView::DrawSelection(CDC* pdc)
{
    for (const auto& selectedItems = CWinDirStatModel::Get()->GetAllSelected(); const CItem* item : selectedItems)
    {
        // Ignore if not a child of the current zoomed item
        if (!CWinDirStatModel::Get()->GetZoomItem()->IsAncestorOf(item)) continue;

        HighlightSelectedItem(pdc, GetDisplayItem(item), selectedItems.size() == 1);
    }
}

// Draws the highlight rectangle of item. If single, the rectangle is slightly
// bigger than the item rect; otherwise, it fits inside.
//
bool CTreeMapView::GetHighlightRect(const CItem* item, const bool single, CRect& rc) const
{
    if (!m_treeMap.TryGetItemRectangle(item, rc)) return false;

    // Offset the display rectangle if zoomed
    if (CWinDirStatModel::Get()->IsZoomed())
    {
        rc.Offset(ZoomFrameWidth, ZoomFrameWidth);
    }

    if (single)
    {
        const CRect rcClient = GetClientRect();
        if (m_treeMap.GetOptions().grid)
        {
            rc.right++;
            rc.bottom++;
        }

        if (rcClient.left < rc.left) rc.left--;
        if (rcClient.top < rc.top) rc.top--;
        if (rc.right < rcClient.right) rc.right++;
        if (rc.bottom < rcClient.bottom) rc.bottom++;
    }
    return rc.Width() > 0 && rc.Height() > 0;
}

void CTreeMapView::HighlightSelectedItem(CDC* pdc, const CItem* item, const bool single,
    const bool hover) const
{
    // The GPU compositor draws selection and hover as animated glass
    if (GlassOverlay::IsActive()) return;

    CRect rc;
    if (!GetHighlightRect(item, single, rc)) return;
    RenderHighlightRectangle(pdc, rc, hover);
}

bool CTreeMapView::BuildGlassHighlights(const HWND frame, GlassTreeMap& out)
{
    if (!IsWindow(m_hWnd) || !::IsWindowVisible(m_hWnd) || !::IsChild(frame, m_hWnd)) return false;
    CWinDirStatModel* model = CWinDirStatModel::Get();
    if (model == nullptr || model->GetRootItem() == nullptr || !HasValidLayout()) return false;

    const auto toFrame = [&](RECT rc)
    {
        ::MapWindowPoints(m_hWnd, frame, reinterpret_cast<POINT*>(&rc), 2);
        return rc;
    };
    out.clip = toFrame(GetClientRect());
    out.pane = out.clip;
    if (const HWND pane = ::GetParent(m_hWnd); pane != nullptr)
    {
        RECT pr{};
        ::GetWindowRect(pane, &pr);
        ::MapWindowPoints(nullptr, frame, reinterpret_cast<POINT*>(&pr), 2);
        out.pane = pr;
    }
    out.paneRadius = GlassTheme::CardRadius;
    out.dark = DarkMode::IsDarkModeActive();
    out.color = COptions::TreeMapHighlightColor;

    size_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const size_t v) { hash = (hash ^ v) * 1099511628211ull; };
    const auto mixRect = [&](const RECT& r)
    {
        mix(static_cast<size_t>(r.left)); mix(static_cast<size_t>(r.top)); mix(static_cast<size_t>(r.right)); mix(static_cast<size_t>(r.bottom));
    };

    const auto& selectedItems = model->GetAllSelected();
    out.single = selectedItems.size() == 1;
    for (const CItem* item : selectedItems)
    {
        if (!model->GetZoomItem()->IsAncestorOf(item)) continue;
        CRect rc;
        if (!GetHighlightRect(GetDisplayItem(item), out.single, rc)) continue;
        out.selected.push_back(toFrame(rc));
        mixRect(out.selected.back());
    }
    if (m_hoverItem != nullptr)
    {
        CRect rc;
        if (GetHighlightRect(GetDisplayItem(m_hoverItem), false, rc))
        {
            out.hasHover = true;
            out.hover = toFrame(rc);
            mixRect(out.hover);
        }
    }
    mix(static_cast<size_t>(out.color)); mix(static_cast<size_t>(out.dark));
    mixRect(out.clip);
    out.hash = hash;
    return true;
}

CItem* CTreeMapView::FindItemAtPoint(const CPoint point)
{
    // Offset the click point if zoomed
    CPoint pointClicked = point;

    if (CWinDirStatModel::Get()->IsZoomed())
    {
        pointClicked.Offset(-ZoomFrameWidth, -ZoomFrameWidth);
    }

    return m_treeMap.FindItemByPoint(
        CWinDirStatModel::Get()->GetZoomItem(), pointClicked);
}

bool CTreeMapView::HasValidLayout() const
{
    return m_treeMap.HasValidLayout(CWinDirStatModel::Get()->GetZoomItem());
}

void CTreeMapView::ClearVisualizationLayout()
{
    m_treeMap.ClearLayout();
}

void CTreeMapView::OnRenderCacheTrimmed()
{
    m_treeMap.TrimMemory();
}

void CTreeMapView::DrillDown(CItem* item)
{
    if (item != CWinDirStatModel::Get()->GetZoomItem())
    {
        NotifyOtherPanes(MODEL_CHANGE_SELECTION_ACTION, item);
        GetMainWindow()->SendMessage(WM_COMMAND, ID_TREEMAP_ZOOMIN);
    }
}

std::span<const UINT> CTreeMapView::GetPersistentContextCommands() const
{
    static constexpr std::array<UINT, 15> persistentCommands{
        ID_TREEMAP_ZOOMIN,
        ID_TREEMAP_ZOOMOUT,
        ID_TREEMAP_SELECT_PARENT,
        ID_TREEMAP_RESELECT_CHILD,
        ID_VIEW_GROUP_TYPES,
        ID_TREEMAP_SHOW_FOLDER_FRAMES,
        ID_TREEMAP_SHOW_EXTENSIONS,
        ID_TREEMAP_LOGICAL_SIZE,
        ID_TREEMAP_PHYSICAL_SIZE,
        ID_VIEW_GRAPH_PRESET_CLASSIC,
        ID_VIEW_GRAPH_PRESET_CALM,
        ID_VIEW_GRAPH_PRESET_FLAT,
        ID_VIEW_GRAPH_PRESET_PASTEL,
        ID_VIEW_GRAPH_PRESET_HIGH_CONTRAST,
        ID_VIEW_GRAPH_PRESET_CUSTOM,
    };
    return persistentCommands;
}
