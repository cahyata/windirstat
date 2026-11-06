// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "TreeMap.h"
#include "WdsListControl.h"
#include "DrawTextCache.h"

namespace
{
    constexpr UINT TEXT_X_MARGIN = 6u; // Horizontal distance of the text from the edge of the item rectangle
    constexpr UINT LABEL_INFLATE_CX = 3u; // How much the label is enlarged, to get the selection and focus rectangle
    constexpr UINT LABEL_Y_MARGIN = 4u;
    constexpr UINT GENERAL_INDENT = 5u;

    class SelectionPreserver
    {
    public:
        explicit SelectionPreserver(CWdsListControl* list)
            : m_list(list)
        {
            assert(m_list != nullptr);

            if (const int i = m_list->GetNextItem(-1, LVNI_FOCUSED); i != -1)
            {
                m_focusedItem = m_list->GetItem(i);
                m_list->SetItemState(i, 0, LVIS_FOCUSED);
            }

            if (const int i = m_list->GetSelectionMark(); i != -1)
            {
                m_selectionMarkItem = m_list->GetItem(i);
            }

            for (int i = m_list->GetNextItem(-1, LVNI_SELECTED); i != -1; i = m_list->GetNextItem(i, LVNI_SELECTED))
            {
                if (auto* item = m_list->GetItem(i)) m_selectedItems.push_back(item);
            }

            m_list->SetItemState(-1, 0, LVIS_SELECTED);
        }

        ~SelectionPreserver()
        {
            int firstSelected = -1;
            for (const auto* item : m_selectedItems)
            {
                if (const int i = m_list->FindListItem(item); i != -1)
                {
                    m_list->SetItemState(i, LVIS_SELECTED, LVIS_SELECTED);
                    if (firstSelected == -1) firstSelected = i;
                }
            }

            RestoreSelectionMark(firstSelected);

            if (const int focused = m_list->FindListItem(m_focusedItem); focused != -1 && IsVisible(focused))
            {
                m_list->SetItemState(focused, LVIS_FOCUSED, LVIS_FOCUSED);
            }
        }

    private:
        void RestoreSelectionMark(const int firstSelected) const
        {
            int selectionMark = -1;
            if (m_selectionMarkItem != nullptr)
            {
                selectionMark = m_list->FindListItem(m_selectionMarkItem);
                if (selectionMark == -1) selectionMark = firstSelected;
            }

            m_list->SetSelectionMark(selectionMark);
        }

        bool IsVisible(const int i) const
        {
            const int top = m_list->GetTopIndex();
            return i >= top && i < top + m_list->GetCountPerPage();
        }

        CWdsListControl* m_list;
        std::vector<CWdsListItem*> m_selectedItems;
        CWdsListItem* m_focusedItem = nullptr;
        CWdsListItem* m_selectionMarkItem = nullptr;
    };
}

/////////////////////////////////////////////////////////////////////////////
// CWdsListItem

int CWdsListItem::CompareSort(const CWdsListItem* other, const SSorting& sorting) const
{
    int r = Compare(other, sorting.subitem1);
    if (std::abs(r) < 2 && !sorting.ascending1)
    {
        r = -r;
    }

    if (r == 0 && sorting.subitem1 != sorting.subitem2)
    {
        r = Compare(other, sorting.subitem2);
        if (std::abs(r) < 2 && !sorting.ascending2)
        {
            r = -r;
        }
    }
    return r;
}

// Draws an item label (icon, text) in all parts of the WinDirStat view;
// the rest is drawn by DrawItem()
void CWdsListItem::DrawLabel(const CWdsListControl* list, CDC* pdc, CRect& rc, const UINT state, int* width, int* focusLeft, const bool indent)
{
    CRect rcRest = rc;

    // Increase indentation according to tree-level
    if (indent)
    {
        rcRest.left += GENERAL_INDENT;
    }

    const CSize sizeImage(list->GetIconSize(), list->GetIconSize());

    if (width == nullptr)
    {
        // Draw the color with transparent background
        if (const auto icon = GetIcon(); icon != nullptr)
        {
            const CPoint pt(rcRest.left, rcRest.top + rcRest.Height() / 2 - sizeImage.cy / 2);
            GetIconHandler()->DrawIcon(pdc, icon, pt, sizeImage);
        }
    }

    // Decrease size of the remainder rectangle from left
    rcRest.left += sizeImage.cx;

    GdiObjectSelection sofont(pdc, list->GetFont());

    rcRest.Deflate(TEXT_X_MARGIN, 0);

    CRect rcLabel = rcRest;
    DrawTextCache::Get().DrawTextCached(pdc, GetText(0), rcLabel, true, true);

    rcLabel.Inflate(LABEL_INFLATE_CX, 0);
    rcLabel.top = rcRest.top + LABEL_Y_MARGIN;
    rcLabel.bottom = rcRest.bottom - LABEL_Y_MARGIN;

    COLORREF textColor = GetItemTextColor();
    if (width == nullptr && (state & ODS_SELECTED) != 0)
    {
        // Color for the text in a highlighted item (usually white)
        textColor = list->GetHighlightTextColor();

        CRect selection = rcLabel;
        if (list->IsFullRowSelection())
        {
            selection.right = rc.right;
        }
        // Fill the selection rectangle background (the full-row capsule is drawn by the list itself)
        if (!list->IsFullRowSelection()) pdc->FillSolidRect(selection, list->GetHighlightColor());
    }

    // Set text color for device context
    ScopedTextColor stc(pdc, textColor);

    if (width == nullptr)
    {
        // Draw the actual text
        DrawTextCache::Get().DrawTextCached(pdc, GetText(0), rcRest, true, false, (state & ODS_SELECTED) == 0);
    }

    rcLabel.Inflate(1, 1);

    *focusLeft = rcLabel.left;

    if ((state & ODS_FOCUS) != 0 && list->HasFocus() && width == nullptr && !list->IsFullRowSelection())
    {
        DarkMode::DrawFocusRect(pdc, rcLabel);
    }

    if (width == nullptr)
    {
        DrawAdditionalState(pdc, rcLabel);
    }

    rcLabel.left = rc.left;
    rc = rcLabel;

    if (width != nullptr)
    {
        *width = rcLabel.Width() + 5; // Don't know why +5
    }
}

void CWdsListItem::DrawSelection(const CWdsListControl* list, CDC* pdc, CRect rc, const UINT state) const
{
    // Full-row selection is drawn as one rounded capsule by the list control
    (void)list; (void)pdc; (void)rc; (void)state;
}

void CWdsListItem::DrawPercentage(CDC* pdc, const CRect rc, const double fraction, const COLORREF color) const
{
    // Capsule progress bar: soft track with a glossy rounded fill
    using namespace Gdiplus;
    const bool dark = DarkMode::IsDarkModeActive();
    const int barHeight = std::max(6, rc.Height() * 55 / 100);
    const REAL top = static_cast<REAL>(rc.top + (rc.Height() - barHeight) / 2);
    const REAL left = static_cast<REAL>(rc.left) + 1.0f;
    const REAL width = static_cast<REAL>(rc.Width()) - 2.0f;
    const REAL height = static_cast<REAL>(barHeight);
    Graphics g(pdc->Handle());
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    const auto pill = [](GraphicsPath& path, const REAL x, const REAL y, const REAL w, const REAL h)
    {
        const REAL d = std::min(h, w);
        path.AddArc(x, y, d, d, 90, 180);
        path.AddArc(x + w - d, y, d, d, 270, 180);
        path.CloseFigure();
    };
    GraphicsPath track;
    pill(track, left, top, width, height);
    const SolidBrush trackBrush(Color(dark ? 50 : 46, dark ? 255 : 60, dark ? 255 : 70, dark ? 255 : 100));
    g.FillPath(&trackBrush, &track);
    const REAL fillWidth = width * static_cast<REAL>(fraction);
    if (fillWidth >= 1.0f)
    {
        GraphicsPath fill;
        pill(fill, left, top, std::max(fillWidth, height), height);
        const BYTE r = GetRValue(color), gr = GetGValue(color), b = GetBValue(color);
        const LinearGradientBrush fillBrush(PointF(0, top), PointF(0, top + height),
            Color(255, static_cast<BYTE>(std::min(255, r + 48)), static_cast<BYTE>(std::min(255, gr + 48)),
                static_cast<BYTE>(std::min(255, b + 48))), Color(255, r, gr, b));
        g.FillPath(&fillBrush, &fill);
    }
}

/////////////////////////////////////////////////////////////////////////////
// CWdsListControl

CWdsListControl::CWdsListControl(std::vector<int>* columnOrder, std::vector<int>* columnWidths, std::vector<int>* columnVisibility)
    : m_columnOrder(columnOrder)
    , m_columnWidths(columnWidths)
    , m_columnVisibility(columnVisibility)
{
    assert(m_columnOrder != nullptr);
    assert(m_columnWidths != nullptr);
    assert(m_columnVisibility != nullptr);
    InitializeColors();
    Live().push_back(this);
}

// This method MUST be called before the Control is shown.
void CWdsListControl::OnColumnsInserted(
    const std::initializer_list<int> requiredColumns,
    const std::initializer_list<int> defaultHiddenColumns)
{
    // Cache the column count
    m_columnCount = GetHeader().GetItemCount();
    m_defaultColumnWidths.assign_range(std::views::iota(0, m_columnCount)
        | std::views::transform([this](const int column) { return GetColumnWidth(column); }));

    m_requiredColumns.assign(requiredColumns);
    if (m_columnCount > 0)
    {
        const int first = ColumnToSubItem(0);
        if (!std::ranges::contains(m_requiredColumns, first))
        {
            m_requiredColumns.push_back(first);
        }
    }

    auto& visibility = *m_columnVisibility;
    const size_t previousSize = visibility.size();
    for (const int column : std::views::iota(0, m_columnCount))
    {
        const int subitem = ColumnToSubItem(column);
        if (subitem >= static_cast<int>(visibility.size()))
        {
            visibility.resize(subitem + 1, 1);
        }
        if (subitem >= static_cast<int>(previousSize))
        {
            visibility[subitem] = !std::ranges::contains(defaultHiddenColumns, subitem);
        }
        if (IsColumnRequired(subitem))
        {
            visibility[subitem] = 1;
        }
    }

    // The pacman shall not draw over our header control.
    ModifyStyle(0, WS_CLIPCHILDREN);
    ModifyStyle(0, LVS_OWNERDATA);
    LoadPersistentAttributes();
    for (const int column : std::views::iota(0, m_columnCount))
    {
        ApplyColumnVisibility(column);
    }

    if (!IsColumnVisible(m_sorting.subitem1))
    {
        const bool ascending = GetAscendingDefault(ColumnToSubItem(0));
        SetSorting(0, ascending, 0, ascending);
    }

    // Calculate row height now that window is created
    CalculateRowHeight();

    // Force the list control to register a new row height
    // This is necessary for controls embedded in a dialog resource.
    SetRowHeight(m_rowHeight);
}

void CWdsListControl::OnFontSizeChanged(const int oldPercent, const int newPercent)
{
    if (oldPercent == newPercent) return;

    for (int& width : m_defaultColumnWidths) width = MulDiv(width, newPercent, oldPercent);
    for (const int column : std::views::iota(0, m_columnCount))
    {
        const int width = GetColumnWidth(column);
        if (width <= 0) continue;

        const int scaledWidth = MulDiv(width, newPercent, oldPercent);
        SetColumnWidth(column, scaledWidth);
        if (std::cmp_less(column, m_columnWidths->size())) (*m_columnWidths)[column] = scaledWidth;
    }
}

void CWdsListControl::SysColorChanged()
{
    InitializeColors();
    CalculateRowHeight();
}

void CWdsListControl::CalculateRowHeight()
{
    // Create a device context to get font metrics
    if (!IsWindow(m_hWnd)) return;
    m_iconSize = ScaleForDpi(16);
    CClientDC dc(this);
    GdiObjectSelection sofont(&dc, GetFont());

    if (const auto metrics = dc.GetTextMetrics())
    {
        // Row height accommodates both the text and icon, plus padding.
        m_rowHeight = (std::max<int>(metrics->tmHeight, m_iconSize) + (LABEL_Y_MARGIN * 2) + 1) | 1;
    }
}

void CWdsListControl::ShowGrid(const bool show)
{
    m_showGrid = show;
    if (IsWindow(m_hWnd))
    {
        Invalidate();
    }
}

void CWdsListControl::ShowStripes(const bool show)
{
    m_showStripes = show;
    if (IsWindow(m_hWnd))
    {
        Invalidate();
    }
}

void CWdsListControl::ShowFullRowSelection(const bool show)
{
    m_showFullRowSelect = show;
    if (IsWindow(m_hWnd))
    {
        Invalidate();
    }
}

COLORREF CWdsListControl::GetHighlightColor() const
{
    if (HasFocus() || DarkMode::IsHighContrastActive())
    {
        return DarkMode::SystemColor(COLOR_HIGHLIGHT);
    }

    return DarkMode::IsDarkModeActive() ? RGB(90, 90, 90) : RGB(190, 190, 190);
}

COLORREF CWdsListControl::GetHighlightTextColor() const
{
    if (HasFocus() || DarkMode::IsHighContrastActive())
    {
        return DarkMode::SystemColor(COLOR_HIGHLIGHTTEXT);
    }

    return DarkMode::IsDarkModeActive() ? RGB(255, 255, 255) : RGB(0, 0, 0);
}

COLORREF CWdsListControl::GetItemSelectionBackgroundColor(const int i) const
{
    const bool selected = (GetItemState(i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    if (selected && IsFullRowSelection())
    {
        return GetHighlightColor();
    }

    return GetItemBackgroundColor(i);
}

COLORREF CWdsListControl::GetItemSelectionTextColor(const int i) const
{
    const bool selected = (GetItemState(i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    if (selected && IsFullRowSelection())
    {
        return GetHighlightTextColor();
    }

    return DarkMode::SystemColor(COLOR_WINDOWTEXT);
}

int CWdsListControl::GetGeneralLeftIndent() const
{
    return GENERAL_INDENT;
}

CWdsListItem* CWdsListControl::GetItem(const int i) const
{
    if (i < 0 || i >= static_cast<int>(m_items.size()))
    {
        return nullptr;
    }

    return m_items[i];
}

int CWdsListControl::FindListItem(const CWdsListItem* item) const
{
    if (const auto it = m_itemMap.find(const_cast<CWdsListItem*>(item)); it != m_itemMap.end())
    {
        return it->second;
    }
    return -1;
}

void CWdsListControl::InitializeColors()
{
    // I try to find a good contrast to COLOR_WINDOW (usually white or light grey).
    // This is a result of experiments.

    constexpr double diff = 0.07; // Try to alter the brightness by diff.
    constexpr double threshold = 1.04; // If result would be brighter, make color darker.

    m_windowColor = DarkMode::SystemColor(COLOR_WINDOW);

    double b = CColorSpace::GetColorBrightness(m_windowColor);

    if (b + diff > threshold)
    {
        b -= diff;
    }
    else
    {
        b += diff;
        b = std::min<double>(b, 1.0);
    }

    m_stripeColor = DarkMode::IsHighContrastActive() ? m_windowColor :
        DarkMode::IsDarkModeActive() ? DarkMode::SystemColor(COLOR_WINDOWFRAME) :
        CColorSpace::MakeBrightColor(m_windowColor, b);
}

LRESULT CWdsListControl::OnPaint(WPARAM, LPARAM)
{
    // Share column geometry and a row buffer across the native control's draw callbacks.
    PaintContext context;
    const ScopedValue painting(m_paintContext, &context);
    return CallDefaultHandler();
}

void CWdsListControl::DrawItem(const LPDRAWITEMSTRUCT pdis)
{
    auto* item = GetItem(static_cast<int>(pdis->itemID));
    if (item == nullptr) return;

    auto dc = CDC::Borrow(pdis->hDC);
    auto* pdc = &dc;
    const CRect rcItem(pdis->rcItem);
    if (rcItem.IsEmpty()) return;

    // Direct draw requests have their own context; nested painting cannot reuse an active row buffer.
    PaintContext direct;
    auto& paint = m_paintContext != nullptr ? *m_paintContext : direct;
    const ScopedValue drawing(m_paintContext, static_cast<PaintContext*>(nullptr));
    if (paint.columns.empty())
    {
        const auto header = GetHeader();
        paint.columns.reserve(m_columnCount);
        for (const int column : std::views::iota(0, m_columnCount))
        {
            LVCOLUMN info{ .mask = LVCF_SUBITEM | LVCF_FMT };
            CRect rect;
            if (GetColumn(column, &info) && IsColumnVisible(info.iSubItem) &&
                header.SendMessage(HDM_GETITEMRECT, column, &rect) != 0)
                paint.columns.push_back({ info.iSubItem, (info.fmt & LVCFMT_RIGHT) == 0, rect });
        }
    }
    if (!paint.dc) paint.dc.Attach(CreateCompatibleDC(pdis->hDC));
    if (paint.size != rcItem.Size())
    {
        if (!paint.bitmap.CreateCompatible(pdc, rcItem.Width(), rcItem.Height())) return;
        paint.size = rcItem.Size();
    }
    if (!paint.dc || !paint.bitmap) return;
    auto& dcMem = paint.dc;
    const ScopedDcState restore(&dcMem);
    GdiObjectSelection sobm(&dcMem, &paint.bitmap);

    const COLORREF backColor = GetItemBackgroundColor(static_cast<int>(pdis->itemID));
    {
        // Rows are frosted glass over the backdrop; a selected row becomes a rounded capsule
        const RECT rowRc = rcItem;
        GlassTheme::PaintBackdropAt(dcMem.Handle(), m_hWnd, rowRc);
        const bool dark = DarkMode::IsDarkModeActive();
        const RECT local{ 0, 0, rcItem.Width(), rcItem.Height() };
        GlassTheme::FillWash(dcMem.Handle(), local, RGB(255, 255, 255), dark ? 14 : 150);
        if ((pdis->itemState & ODS_SELECTED) != 0 && IsFullRowSelection())
        {
            // Keep the capsule inside the visible width so its right end is rounded too
            const int visibleRight = std::min<int>(rcItem.Width(), GetClientRect().right - rcItem.left);
            const RECT capsule{ 5, 2, std::max(40, visibleRight - 5), rcItem.Height() - 2 };
            GlassTheme::FillRounded(dcMem.Handle(), capsule, (capsule.bottom - capsule.top) / 2 + 1,
                GetItemSelectionBackgroundColor(static_cast<int>(pdis->itemID)), 255);
        }
        else if (backColor != DarkMode::SystemColor(COLOR_WINDOW) && (pdis->itemState & ODS_SELECTED) == 0)
            GlassTheme::FillWash(dcMem.Handle(), local, dark ? RGB(255, 255, 255) : RGB(60, 70, 100), dark ? 10 : 14);
        dcMem.SetBkMode(TRANSPARENT);
    }

    // Set defaults for all text drawing
    ScopedBkColor bkColor(&dcMem, backColor);
    GdiObjectSelection sofont(&dcMem, GetFont());

    int focusLeft = 0;
    CPen pen;
    for (const auto& [subitem, leftAlign, rect] : paint.columns)
    {
        const CRect rcDraw(rect.left, 0, rect.right, rcItem.Height());

        if (!item->DrawSubItem(subitem, &dcMem, rcDraw, pdis->itemState, nullptr, &focusLeft))
        {
            item->DrawSelection(this, &dcMem, rcDraw, pdis->itemState);

            CRect rcText = rcDraw;
            rcText.Deflate(TEXT_X_MARGIN, 0);
            const std::wstring s = item->GetText(subitem);

            // Get the correct color in case of compressed or encrypted items
            COLORREF textColor = item->GetItemTextColor();

            // Except if the item is selected - in this case just use standard colors
            COLORREF backColorSub = backColor;
            if (pdis->itemState & ODS_SELECTED && IsFullRowSelection())
            {
                backColorSub = GetItemSelectionBackgroundColor(pdis->itemID);
                textColor = GetItemSelectionTextColor(pdis->itemID);
            }

            // Secondary columns recede so the name column leads
            if (subitem != 0 && !(pdis->itemState & ODS_SELECTED && IsFullRowSelection()))
            {
                const auto mixChannel = [](const int a, const int b) { return (a * 62 + b * 38) / 100; };
                textColor = RGB(mixChannel(GetRValue(textColor), GetRValue(backColor)),
                    mixChannel(GetGValue(textColor), GetGValue(backColor)),
                    mixChannel(GetBValue(textColor), GetBValue(backColor)));
            }

            // Set the text color
            ScopedTextColor tc(&dcMem, textColor);
            ScopedBkColor backColorObj(&dcMem, backColorSub);

            // Draw the (sub)item text
            DrawTextCache::Get().DrawTextCached(&dcMem, s, rcText, leftAlign, false,
                !(pdis->itemState & ODS_SELECTED && IsFullRowSelection()));
        }

        if (m_showGrid)
        {
            constexpr COLORREF gridColor = RGB(212, 208, 200);
            constexpr COLORREF gridColorDark = RGB(99, 99, 99);
            if (!pen) pen = CPen(PS_SOLID, 1, DarkMode::IsDarkModeActive() ? gridColorDark : gridColor);
            GdiObjectSelection sopen(&dcMem, &pen);

            // Draw top line for first item
            if (pdis->itemID == 0)
            {
                dcMem.MoveTo(rcDraw.left, rcDraw.top);
                dcMem.LineTo(rcDraw.right, rcDraw.top);
            }

            dcMem.MoveTo(rcDraw.right - 1, rcDraw.top);
            dcMem.LineTo(rcDraw.right - 1, rcDraw.bottom);
            dcMem.MoveTo(rcDraw.left, rcDraw.bottom - 1);
            dcMem.LineTo(rcDraw.right, rcDraw.bottom - 1);
        }
    }

    // No dotted focus rectangle: the rounded selection capsule already marks the row
    (void)focusLeft;

    pdc->BitBlt(rcItem.left, rcItem.top,
        rcItem.Width(), rcItem.Height(), &dcMem, 0, 0, SRCCOPY);
}

bool CWdsListControl::BuildGlassView(const HWND frame, std::shared_ptr<const GlassListView>& out)
{
    const HWND list = m_hWnd;
    if (!::IsWindow(list) || !::IsWindowVisible(list) || !IsFullRowSelection() || !::IsChild(frame, list)) return false;
    const HWND header = ListView_GetHeader(list);
    if (header == nullptr) return false;

    const auto toFrame = [&](RECT rc, const HWND from)
    {
        ::MapWindowPoints(from, frame, reinterpret_cast<POINT*>(&rc), 2);
        return rc;
    };
    const auto screenToFrame = [&](RECT rc)
    {
        ::MapWindowPoints(nullptr, frame, reinterpret_cast<POINT*>(&rc), 2);
        return rc;
    };

    RECT windowScreen{}, clientRc{}, headerScreen{};
    ::GetWindowRect(list, &windowScreen);
    ::GetClientRect(list, &clientRc);
    ::GetWindowRect(header, &headerScreen);
    const RECT listRc = screenToFrame(windowScreen);
    const RECT client = toFrame(clientRc, list);
    const RECT headerRc = screenToFrame(headerScreen);

    POINT cursor{};
    ::GetCursorPos(&cursor);
    POINT local = cursor;
    ::ScreenToClient(list, &local);
    const bool overList = ::WindowFromPoint(cursor) == list;
    const int count = ListView_GetItemCount(list);
    const int first = ListView_GetTopIndex(list);
    const int last = std::min(count, first + ListView_GetCountPerPage(list) + 2);
    const int columnCount = Header_GetItemCount(header);

    // Cheap fingerprint: rebuild only when something visible moved, otherwise reuse the snapshot for a while
    size_t signature = 1469598103934665603ull;
    const auto mix = [&signature](const size_t v) { signature = (signature ^ v) * 1099511628211ull; };
    mix(static_cast<size_t>(listRc.left)); mix(static_cast<size_t>(listRc.top));
    mix(static_cast<size_t>(listRc.right)); mix(static_cast<size_t>(listRc.bottom));
    mix(static_cast<size_t>(first)); mix(static_cast<size_t>(count));
    mix(static_cast<size_t>(::GetFocus() == list)); mix(static_cast<size_t>(DarkMode::IsDarkModeActive()));
    for (int c = 0; c < columnCount; ++c)
    {
        RECT hr{};
        HDITEMW hd{ .mask = HDI_FORMAT };
        Header_GetItemRect(header, c, &hr);
        Header_GetItem(header, c, &hd);
        mix(static_cast<size_t>(hr.left)); mix(static_cast<size_t>(hr.right)); mix(static_cast<size_t>(hd.fmt));
    }
    for (int i = first; i < last; ++i)
    {
        RECT r{};
        ListView_GetItemRect(list, i, &r, LVIR_BOUNDS);
        mix(static_cast<size_t>(r.top));
        mix(static_cast<size_t>(ListView_GetItemState(list, i, LVIS_SELECTED) & LVIS_SELECTED));
        mix(static_cast<size_t>(overList && local.y >= r.top && local.y < r.bottom && local.x >= 0 && local.x < clientRc.right));
    }
    for (const int bar : { OBJID_VSCROLL, OBJID_HSCROLL })
    {
        SCROLLBARINFO info{ .cbSize = sizeof(info) };
        if (::GetScrollBarInfo(list, static_cast<LONG>(bar), &info))
        {
            mix(static_cast<size_t>(info.xyThumbTop)); mix(static_cast<size_t>(info.xyThumbBottom));
            mix(static_cast<size_t>(info.rgstate[0]));
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (m_glassView != nullptr && signature == m_glassSignature && now - m_glassBuilt < 300)
    {
        out = m_glassView;
        return true;
    }

    auto view = std::make_shared<GlassListView>();
    view->list = listRc;
    view->client = client;
    view->header = headerRc;
    if (const HWND pane = ::GetParent(list); pane != nullptr)
    {
        RECT pr{};
        ::GetWindowRect(pane, &pr);
        view->card = screenToFrame(pr);
    }
    else view->card = listRc;
    view->cardRadius = GlassTheme::CardRadius;
    view->dark = DarkMode::IsDarkModeActive();
    view->focused = ::GetFocus() == list || DarkMode::IsHighContrastActive();
    view->text = DarkMode::SystemColor(COLOR_WINDOWTEXT);
    view->highlight = GetHighlightColor();
    view->highlightText = GetHighlightTextColor();
    view->rowHeight = GetRowHeight();
    view->iconPx = GetIconSize();
    view->indentStep = GetRowHeight() * 7 / 8;
    view->generalIndent = GetGeneralLeftIndent();
    LOGFONT font{};
    ::GetObjectW(GetFont(), sizeof(font), &font);
    view->fontPx = std::max(8, std::abs(static_cast<int>(font.lfHeight)));

    // Columns in display order with their real (scrolled) positions
    std::vector<int> subitems;
    const int headerLeft = headerRc.left;
    for (int c = 0; c < columnCount; ++c)
    {
        RECT hr{};
        if (!Header_GetItemRect(header, c, &hr) || hr.right - hr.left <= 1) continue;
        std::array<WCHAR, 128> title{};
        LVCOLUMNW column{ .mask = LVCF_SUBITEM | LVCF_FMT | LVCF_TEXT, .pszText = title.data(), .cchTextMax = static_cast<int>(title.size()) };
        if (!ListView_GetColumn(list, c, &column)) continue;
        HDITEMW hd{ .mask = HDI_FORMAT };
        Header_GetItem(header, c, &hd);
        GlassListColumn out_col;
        out_col.left = headerLeft + hr.left;
        out_col.right = headerLeft + hr.right;
        out_col.rightAligned = (column.fmt & LVCFMT_RIGHT) != 0;
        out_col.title = title.data();
        out_col.weight = GlassColumnWeight(column.iSubItem);
        out_col.sort = (hd.fmt & HDF_SORTUP) ? 1 : (hd.fmt & HDF_SORTDOWN) ? 2 : 0;
        view->columns.push_back(std::move(out_col));
        subitems.push_back(column.iSubItem);
    }

    const POINT origin = [&] { POINT p{ 0, 0 }; ::MapWindowPoints(list, frame, &p, 1); return p; }();
    for (int i = first; i < last; ++i)
    {
        RECT r{};
        if (!ListView_GetItemRect(list, i, &r, LVIR_BOUNDS) || r.bottom <= 0 || r.top >= clientRc.bottom) continue;
        CWdsListItem* item = GetItem(i);
        if (item == nullptr) continue;
        GlassListRow row;
        row.top = origin.y + r.top;
        row.bottom = origin.y + r.bottom;
        row.selected = (ListView_GetItemState(list, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        row.hot = overList && local.y >= r.top && local.y < r.bottom && local.x >= 0 && local.x < clientRc.right;
        row.striped = IsItemStripColor(i);
        row.cells.reserve(subitems.size());
        for (const int subitem : subitems)
        {
            GlassCell cell;
            item->GetGlassCell(subitem, cell);
            row.cells.push_back(std::move(cell));
        }
        view->rows.push_back(std::move(row));
    }

    for (const int bar : { OBJID_VSCROLL, OBJID_HSCROLL })
    {
        SCROLLBARINFO info{ .cbSize = sizeof(info) };
        if (!::GetScrollBarInfo(list, static_cast<LONG>(bar), &info) || (info.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_UNAVAILABLE)) != 0) continue;
        GlassScrollBar& target = bar == OBJID_VSCROLL ? view->vscroll : view->hscroll;
        target.visible = true;
        target.track = screenToFrame(info.rcScrollBar);
        target.thumb = target.track;
        if (bar == OBJID_VSCROLL)
        {
            target.thumb.bottom = target.track.top + info.xyThumbBottom;
            target.thumb.top = target.track.top + info.xyThumbTop;
        }
        else
        {
            target.thumb.right = target.track.left + info.xyThumbBottom;
            target.thumb.left = target.track.left + info.xyThumbTop;
        }
    }

    view->hash = signature;
    for (const auto& row : view->rows)
        for (const auto& cell : row.cells) view->hash = (view->hash ^ std::hash<std::wstring>{}(cell.text)) * 1099511628211ull;
    m_glassView = view;
    m_glassSignature = signature;
    m_glassBuilt = now;
    out = std::move(view);
    return true;
}

CRect CWdsListControl::GetWholeSubitemRect(const int item, const int subitem) const
{
    CRect rc;
    if (subitem == 0)
    {
        // Special case column 0:
        // If we did GetSubItemRect(item, 0, rc)
        // and we have an icon list, then we would get the rectangle
        // excluding the icon.
        HDITEM hditem = { .mask = HDI_WIDTH };
        GetHeader().GetItem(0, &hditem);

        [[maybe_unused]] const bool gotItemRect = GetItemRect(item, rc, LVIR_LABEL);
        assert(gotItemRect);
        rc.left = rc.right - hditem.cxy;
    }
    else
    {
        [[maybe_unused]] const bool gotSubItemRect = GetSubItemRect(item, subitem, rc);
        assert(gotSubItemRect);
    }

    return rc;
}

HFONT CWdsListControl::GetFont() const
{
    if (m_cachedFont == nullptr) m_cachedFont = CWnd::GetFont();
    return m_cachedFont;
}

LRESULT CWdsListControl::OnSetFont(WPARAM /*wParam*/, LPARAM /*lParam*/)
{
    m_cachedFont = nullptr;
    const LRESULT result = CallDefaultHandler();
    m_cachedFont = nullptr;
    DrawTextCache::Get().ClearCache();
    CalculateRowHeight();
    SetRowHeight(m_rowHeight);
    Invalidate(false);
    return result;
}

void CWdsListControl::OnSettingChange(UINT /*uFlags*/, LPCTSTR /*lpszSection*/)
{
    m_cachedFont = nullptr;
    CallDefaultHandler();
    m_cachedFont = nullptr;
    DrawTextCache::Get().ClearCache();
    CalculateRowHeight();
    Invalidate(false);
}

int CWdsListControl::GetSubItemWidth(CWdsListItem* item, const int subitem, CDC* pDC)
{
    if (pDC == nullptr)
    {
        CClientDC dc(this);
        GdiObjectSelection sofont(&dc, GetFont());
        return GetSubItemWidth(item, subitem, &dc);
    }

    const CRect rc(0, 0, 3500, 20);

    int width;
    int dummy = rc.left;
    if (item->DrawSubItem(subitem, pDC, rc, 0, &width, &dummy))
    {
        return width;
    }

    const std::wstring s = item->GetText(subitem);
    if (s.empty())
    {
        return 0;
    }

    return TEXT_X_MARGIN + pDC->GetTextExtent(s).cx;
}

/////////////////////////////////////////////////////////////////////////////
// Sorting functionality (merged from CSortingListControl)

void CWdsListControl::LoadPersistentAttributes()
{
    // Fetch cast column count to avoid signed comparison warnings
    const auto columnCount = static_cast<size_t>(m_columnCount);

    // Load default column order values from resource
    if (m_columnOrder->size() != columnCount)
    {
        m_columnOrder->resize(columnCount);
        GetColumnOrder(*m_columnOrder);
    }

    // Load default column width values from resource
    if (m_columnWidths->size() != columnCount)
    {
        m_columnWidths->resize(columnCount, 0);
        for (const auto [i, width] : std::views::enumerate(*m_columnWidths))
        {
            width = GetColumnWidth(static_cast<int>(i));
        }
    }

    // Set based on persisted values
    SetColumnOrder(*m_columnOrder);
    for (const auto [i, width] : std::views::enumerate(*m_columnWidths))
    {
        SetColumnWidth(static_cast<int>(i), std::min(width, width * 2));
    }
}

void CWdsListControl::SavePersistentAttributes() const
{
    if (!IsWindow(m_hWnd) || m_columnCount == 0) return;

    GetColumnOrder(*m_columnOrder);
    for (const auto [i, width] : std::views::enumerate(*m_columnWidths))
    {
        const int col = static_cast<int>(i);
        if (IsColumnVisible(ColumnToSubItem(col)))
        {
            width = GetColumnWidth(col);
        }
    }
}

int CWdsListControl::ColumnToSubItem(const int col) const
{
    LVCOLUMN column_info{ .mask = LVCF_SUBITEM };
    GetColumn(col, &column_info);
    return column_info.iSubItem;
}

int CWdsListControl::SubItemToColumn(const int subitem) const
{
    for (const int column : std::views::iota(0, m_columnCount))
    {
        if (ColumnToSubItem(column) == subitem) return column;
    }
    return -1;
}

bool CWdsListControl::IsColumnVisible(const int subitem) const
{
    return COptions::IsColumnVisible(*m_columnVisibility, subitem);
}

bool CWdsListControl::IsColumnRequired(const int subitem) const
{
    return std::ranges::contains(m_requiredColumns, subitem);
}

void CWdsListControl::ApplyColumnVisibility(const int column)
{
    const int subitem = ColumnToSubItem(column);
    const bool visible = IsColumnRequired(subitem) || IsColumnVisible(subitem);
    LVCOLUMN columnInfo{ .mask = LVCF_FMT };
    const bool hasColumnInfo = GetColumn(column, &columnInfo);
    if (hasColumnInfo)
    {
        columnInfo.fmt &= ~LVCFMT_FIXED_WIDTH;
        SetColumn(column, &columnInfo);
    }

    const int persistedWidth = column < static_cast<int>(m_columnWidths->size()) ? (*m_columnWidths)[column] : 0;
    const int width = visible ? (persistedWidth > 0 ? persistedWidth : m_defaultColumnWidths[column]) : 0;
    SetColumnWidth(column, width);

    if (!visible && hasColumnInfo)
    {
        columnInfo.fmt |= LVCFMT_FIXED_WIDTH;
        SetColumn(column, &columnInfo);
    }
}

void CWdsListControl::SetColumnVisible(const int subitem, const bool visible, const bool force)
{
    const int column = SubItemToColumn(subitem);
    if (column < 0 || (!visible && IsColumnRequired(subitem)) || (!force && visible == IsColumnVisible(subitem))) return;

    if (!visible && column < static_cast<int>(m_columnWidths->size()))
    {
        (*m_columnWidths)[column] = GetColumnWidth(column);
    }

    COptions::SetColumnVisible(*m_columnVisibility, subitem, visible);
    ApplyColumnVisibility(column);

    bool sortingChanged = false;
    if (!visible && m_sorting.column1 == column)
    {
        const bool ascending = GetAscendingDefault(ColumnToSubItem(0));
        SetSorting(0, ascending, 0, ascending);
        sortingChanged = true;
    }
    else if (!visible && m_sorting.column2 == column)
    {
        SetSorting(m_sorting.column1, m_sorting.ascending1);
        sortingChanged = true;
    }

    if (sortingChanged) SortItems();
    else Invalidate();
}

void CWdsListControl::SetSorting(const int sortColumn1, const bool ascending1, const int sortColumn2, const bool ascending2)
{
    m_sorting = {
        .column1 = sortColumn1,
        .column2 = sortColumn2,
        .subitem1 = ColumnToSubItem(sortColumn1),
        .subitem2 = ColumnToSubItem(sortColumn2),
        .ascending1 = ascending1,
        .ascending2 = ascending2
    };
}

void CWdsListControl::SetSorting(const int sortColumn, const bool ascending)
{
    m_sorting.column2 = m_sorting.column1;
    m_sorting.subitem2 = m_sorting.subitem1;
    m_sorting.ascending2 = m_sorting.ascending1;
    m_sorting.column1 = sortColumn;
    m_sorting.ascending1 = ascending;
    m_sorting.subitem1 = ColumnToSubItem(sortColumn);
}

void CWdsListControl::InsertListItem(const int i, std::span<CWdsListItem* const> items)
{
    if (items.empty()) return;

    assert(i >= 0 && i <= GetItemCount());

    SelectionPreserver preserve(this);

    m_items.insert(m_items.begin() + i, items.begin(), items.end());
    const int itemCount = static_cast<int>(m_items.size());

    for (const auto [offset, item] : std::views::enumerate(std::span(m_items).subspan(i)))
    {
        m_itemMap[item] = static_cast<int>(i + offset);
    }

    SetItemCountEx(itemCount, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    RedrawItems(i, itemCount - 1);
}

void CWdsListControl::InsertSortedListItems(std::span<CWdsListItem* const> items)
{
    if (items.empty()) return;

    auto sorted = items | std::ranges::to<std::vector>();
    const auto compare = [this](const CWdsListItem* first, const CWdsListItem* second)
    {
        return first->CompareSort(second, m_sorting) < 0;
    };
    std::ranges::stable_sort(sorted, compare);

    SelectionPreserver preserve(this);
    const auto oldSize = m_items.size();
    m_items.insert(m_items.end(), sorted.begin(), sorted.end());
    std::inplace_merge(m_items.begin(), m_items.begin() + oldSize, m_items.end(), compare);
    for (const auto [i, item] : std::views::enumerate(m_items)) m_itemMap[item] = static_cast<int>(i);
    SetItemCountEx(GetItemCount(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    Invalidate();
    UpdateSortIndicator();
}

/*
 * Sorts the list control's items and updates the header to display the correct sorting indicator.
 * This method reorders the list control's items based on the current sorting column and direction.
 * It then updates the header control by using native Windows header flags (HDF_SORTUP and HDF_SORTDOWN)
 * to display a platform-consistent sorting arrow.
 */
void CWdsListControl::SortItems()
{
    SelectionPreserver preserve(this);

    std::ranges::stable_sort(m_items, [this](const CWdsListItem* item1, const CWdsListItem* item2)
    {
        return item1->CompareSort(item2, m_sorting) < 0;
    });

    for (const auto [i, item] : std::views::enumerate(m_items))
    {
        m_itemMap[item] = static_cast<int>(i);
    }

    Invalidate();
    UpdateSortIndicator();
}

void CWdsListControl::UpdateSortIndicator()
{
    auto header = GetHeader();
    HDITEM hditem{ .mask = HDI_FORMAT };
    if (m_indicatedColumn == m_sorting.column1 && header.GetItem(m_indicatedColumn, &hditem) &&
        (hditem.fmt & (HDF_SORTUP | HDF_SORTDOWN)) == (m_sorting.ascending1 ? HDF_SORTUP : HDF_SORTDOWN)) return;

    // Remove the sort indicator from the previously sorted column if one exists.
    if (m_indicatedColumn != -1)
    {
        header.GetItem(m_indicatedColumn, &hditem);
        // Use a bitwise operation to clear both the UP and DOWN sort flags.
        hditem.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        header.SetItem(m_indicatedColumn, &hditem);
    }

    // Retrieve the newly sorted column's current format flags.
    header.GetItem(m_sorting.column1, &hditem);
    // Clear any existing sort flags to ensure a clean state before applying the new one.
    hditem.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);

    // Apply the correct native sorting indicator based on the sort direction.
    hditem.fmt |= m_sorting.ascending1 ? HDF_SORTUP : HDF_SORTDOWN;

    header.SetItem(m_sorting.column1, &hditem);

    // Store the current sorted column's index to be cleared next time.
    m_indicatedColumn = m_sorting.column1;
}

void CWdsListControl::PostSelectionChanged()
{
    // Only post if there isn't already a pending message
    if (!m_selectionChangePending)
    {
        m_selectionChangePending = true;
        PostMessage(WM_SELECTION_CHANGED);
    }
}

void CWdsListControl::DeselectAll()
{
    SetItemState(-1, 0, LVIS_SELECTED);
}

/////////////////////////////////////////////////////////////////////////////
// Message Map

void CWdsListControl::OnContextMenu(WindowRef /*pWnd*/, const CPoint point)
{
    if (point != CPoint(-1, -1))
    {
        const CRect headerRect = GetHeader().GetWindowRect();
        if (headerRect.Contains(point))
        {
            ShowColumnContextMenu(point);
            return;
        }
    }

    OnItemContextMenu(point);
}

void CWdsListControl::ShowColumnContextMenu(const CPoint point)
{
    CMenu menu = CMenu::CreatePopup();
    if (!menu) return;

    for (const int column : std::views::iota(0, m_columnCount))
    {
        std::array<wchar_t, 256> text{};
        LVCOLUMN item{
            .mask = LVCF_TEXT,
            .pszText = text.data(),
            .cchTextMax = static_cast<int>(text.size())
        };
        if (!GetColumn(column, &item)) continue;

        const int subitem = ColumnToSubItem(column);
        const bool required = IsColumnRequired(subitem);
        const UINT flags = MF_STRING |
            (required ? MF_GRAYED : MF_ENABLED) |
            (IsColumnVisible(subitem) ? MF_CHECKED : MF_UNCHECKED);
        menu.Append(flags, static_cast<UINT>(column + 1), text.data());
    }

    const UINT command = menu.ShowPopup(
        TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
        point, this);
    if (command > 0 && command <= static_cast<UINT>(m_columnCount))
    {
        const int subitem = ColumnToSubItem(command - 1);
        SetColumnVisible(subitem, !IsColumnVisible(subitem));
    }
}

void CWdsListControl::OnCustomDraw(NMHDR* pNMHDR, LRESULT* pResult) const
{
    // Check if this is a notification from the header control
    *pResult = CDRF_DODEFAULT;
    if (const HWND header = GetHeader().Handle(); header != nullptr && pNMHDR->hwndFrom == header)
    {
        const auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(pNMHDR);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
            GlassTheme::PaintBackdrop(draw->hdc, header);
            RECT capsule = draw->rc;
            capsule.left += 4; capsule.right -= 4; capsule.top += 3; capsule.bottom -= 3;
            GlassTheme::FillRounded(draw->hdc, capsule, (capsule.bottom - capsule.top) / 2,
                RGB(255, 255, 255), DarkMode::IsDarkModeActive() ? 18 : 120);
            *pResult = CDRF_NOTIFYITEMDRAW;
        }
        else if (draw->dwDrawStage == CDDS_ITEMPREPAINT)
        {
            GlassTheme::DrawHeaderItem(draw->hdc, header, static_cast<int>(draw->dwItemSpec), draw->rc);
            *pResult = CDRF_SKIPDEFAULT;
        }
        return;
    }
    if (!DarkMode::IsDarkModeActive())
    {
        return;
    }

    // Handle custom text color for headers in dark mode
    const NMCUSTOMDRAW* pCustomDraw = reinterpret_cast<NMCUSTOMDRAW*>(pNMHDR);
    if (pCustomDraw->dwDrawStage == CDDS_PREPAINT)
    {
        *pResult = CDRF_NOTIFYITEMDRAW;
    }
    else if (pCustomDraw->dwDrawStage == CDDS_ITEMPREPAINT && pNMHDR->hwndFrom == GetHeader().Handle())
    {
        ::SetTextColor(pCustomDraw->hdc, DarkMode::SystemColor(COLOR_BTNTEXT));
    }
}

bool CWdsListControl::OnEraseBkgnd(CDC* pDC) const
{
    // Fetch coordinate of the last item
    CRect lastRect;
    if (const int itemCount = GetItemCount(); itemCount > 0)
    {
        GetItemRect(itemCount - 1, lastRect, LVIR_BOUNDS);
    }

    // Unused area is glass too
    const CRect rectClient = GetClientRect();
    GlassTheme::PaintBackdropAt(pDC->Handle(), m_hWnd, rectClient);
    GlassTheme::FillWash(pDC->Handle(), rectClient, RGB(255, 255, 255), DarkMode::IsDarkModeActive() ? 14 : 150);
    (void)lastRect;
    return true;
}

void CWdsListControl::OnHdnBeginTrack(NMHDR* pNMHDR, LRESULT* pResult)
{
    const int column = reinterpret_cast<LPNMHEADER>(pNMHDR)->iItem;
    *pResult = column >= 0 && column < m_columnCount && !IsColumnVisible(ColumnToSubItem(column));
}

void CWdsListControl::OnHdnDividerdblclick(NMHDR* pNMHDR, LRESULT* pResult)
{
    OnHdnBeginTrack(pNMHDR, pResult);
    if (*pResult) return;

    const int column = reinterpret_cast<LPNMHEADER>(pNMHDR)->iItem;
    const int subitem = ColumnToSubItem(column);

    // fetch size of rendered column header text
    // temporarily insert a false column so the final column does
    // not autosize to fit the whole control width
    const ScopedRedrawPause lock(this);
    const int falseColumn = InsertColumn(m_columnCount + 1, L"");
    SetColumnWidth(column, LVSCW_AUTOSIZE_USEHEADER);
    int width = GetColumnWidth(column);
    DeleteColumn(falseColumn);

    CClientDC dc(this);
    GdiObjectSelection sofont(&dc, GetFont());

    // fetch size of sub-elements
    for (const auto item : m_items)
    {
        width = std::max(width, GetSubItemWidth(item, subitem, &dc));
    }

    // update final column width
    constexpr int padding = 3;
    SetColumnWidth(column, width + padding);
    *pResult = false;
}

void CWdsListControl::OnHdnItemchanging(NMHDR* pNMHDR, LRESULT* pResult)
{
    const auto* header = reinterpret_cast<LPNMHEADER>(pNMHDR);
    if (header->pitem != nullptr && (header->pitem->mask & HDI_WIDTH) != 0 && header->pitem->cxy != 0)
    {
        OnHdnBeginTrack(pNMHDR, pResult);
        if (*pResult) return;
    }

    CallDefaultHandler();
    Invalidate();

    *pResult = false;
}

void CWdsListControl::OnLvnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult) const
{
    auto* displayInfo = reinterpret_cast<NMLVDISPINFO*>(pNMHDR);
    *pResult = false;

    auto* item = GetItem(displayInfo->item.iItem);
    if (item == nullptr) return;

    if (displayInfo->item.mask & LVIF_PARAM)
    {
        displayInfo->item.lParam = reinterpret_cast<LPARAM>(item);
    }

    if ((displayInfo->item.mask & LVIF_TEXT) != 0 && displayInfo->item.cchTextMax > 0)
    {
        // The passed subitem value is actually the column id so translate it
        const int subitem = ColumnToSubItem(displayInfo->item.iSubItem);

        // Copy maximum allowed to the provided buffer
        wcsncpy_s(displayInfo->item.pszText, displayInfo->item.cchTextMax,
            item->GetText(subitem).c_str(), _TRUNCATE);
    }
}

void CWdsListControl::OnHdnItemClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    const auto* phdr = reinterpret_cast<LPNMHEADER>(pNMHDR);
    *pResult = false;

    if (const int col = phdr->iItem; col == m_sorting.column1)
    {
        m_sorting.ascending1 = !m_sorting.ascending1;
    }
    else
    {
        SetSorting(col, GetAscendingDefault(ColumnToSubItem(col)));
    }

    SortItems();
}

void CWdsListControl::OnHdnItemDblClick(NMHDR* pNMHDR, LRESULT* pResult)
{
    OnHdnItemClick(pNMHDR, pResult);
}

void CWdsListControl::OnDestroy()
{
    for (const auto* item : m_items) GetIconHandler()->ForgetAsyncShellInfoLookup(item);
    SavePersistentAttributes();
    CallDefaultHandler();
}

LRESULT CWdsListControl::OnSelectionChanged(const WPARAM wParam, const LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    m_selectionChangePending = false;
    CWinDirStatModel::Get()->NotifyPanes(MODEL_CHANGE_SELECTION_REFRESH);

    return 0;
}

void CWdsListControl::RemoveListItem(const int i, const int c)
{
    if (c <= 0) return;

    int itemCount = GetItemCount();
    assert(i >= 0 && i < itemCount);
    assert(i + c <= itemCount);

    std::vector<std::unique_ptr<CWdsListItem>> removedItems;
    if (m_ownsItems)
    {
        removedItems.reserve(c);
    }

    SelectionPreserver preserve(this);

    for (CWdsListItem* item : std::span(m_items).subspan(i, c))
    {
        GetIconHandler()->ForgetAsyncShellInfoLookup(item);
        m_itemMap.erase(item);
        if (m_ownsItems)
        {
            removedItems.emplace_back(item);
        }
    }

    m_items.erase(m_items.begin() + i, m_items.begin() + i + c);

    itemCount -= c;
    for (const auto [offset, item] : std::views::enumerate(std::span(m_items).subspan(i)))
    {
        m_itemMap[item] = static_cast<int>(i + offset);
    }

    SetItemCountEx(itemCount, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (i < itemCount)
    {
        RedrawItems(i, itemCount - 1);
    }
    else
    {
        Invalidate();
    }
}

void CWdsListControl::RemoveListItems(const std::function<bool(CWdsListItem*)>& shouldRemove)
{
    std::vector<std::unique_ptr<CWdsListItem>> removedItems;
    SelectionPreserver preserve(this);

    // Compact rows once, keeping removed objects alive until selection has been restored.
    std::erase_if(m_items, [&](CWdsListItem* item)
    {
        if (!shouldRemove(item)) return false;
        GetIconHandler()->ForgetAsyncShellInfoLookup(item);
        m_itemMap.erase(item);
        if (m_ownsItems) removedItems.emplace_back(item);
        return true;
    });
    for (const auto [i, item] : std::views::enumerate(m_items)) m_itemMap[item] = static_cast<int>(i);
    SetItemCountEx(GetItemCount(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    Invalidate();
}

bool CWdsListControl::DeleteItem(const int i)
{
    RemoveListItem(i);
    return true;
}

bool CWdsListControl::DeleteAllItems()
{
    for (const auto* item : m_items) GetIconHandler()->ForgetAsyncShellInfoLookup(item);
    if (m_ownsItems) for (const auto* item : m_items) delete item;
    m_items.clear();
    m_itemMap.clear();
    SetItemCountEx(0, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    return true;
}
