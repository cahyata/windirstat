// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "../GlassUi.h"

CMessageBoxDlg::CMessageBoxDlg(const std::wstring& message, const std::wstring& title, const UINT type, WindowRef pParent,
    const std::vector<std::wstring>& listViewItems, const std::wstring& checkBoxText, const bool checkBoxValue)
    : MessageTarget(IDD_MESSAGEBOX, &m_windowRect, pParent)
    , m_message(message)
    , m_title(title)
    , m_checkboxText(checkBoxText)
    , m_listViewItems(listViewItems)
    , m_checkboxChecked(checkBoxValue)
{
    const std::unordered_map<UINT, ButtonContext> buttonTypeContexts
    {
        // m_buttonType          btnLeftID  btnMidID   btnRightID  btnLeftIDS         btnMidIDS          btnRightIDS         btnFocus
        { MB_OK,               { 0,         0,         IDOK,       IDS_GENERIC_BLANK, IDS_GENERIC_BLANK, IDS_GENERIC_OK,     &m_buttonRight  } },
        { MB_OKCANCEL,         { 0,         IDOK,      IDCANCEL,   IDS_GENERIC_BLANK, IDS_GENERIC_OK,    IDS_GENERIC_CANCEL, &m_buttonMiddle } },
        { MB_YESNO,            { 0,         IDYES,     IDNO,       IDS_GENERIC_BLANK, IDS_GENERIC_YES,   IDS_GENERIC_NO,     &m_buttonMiddle } },
        { MB_YESNOCANCEL,      { IDYES,     IDNO,      IDCANCEL,   IDS_GENERIC_YES,   IDS_GENERIC_NO,    IDS_GENERIC_CANCEL, &m_buttonLeft   } },
        // These MB types are not used by WinDirStat, but are included for completeness and use
        // IDS_GENERIC_BLANK as a placeholder for button labels. Please add the required IDS to
        // the localization engine upon use.
        { MB_RETRYCANCEL,      { 0,         IDRETRY,   IDCANCEL,   IDS_GENERIC_BLANK, IDS_GENERIC_BLANK, IDS_GENERIC_CANCEL, &m_buttonMiddle } },
        { MB_ABORTRETRYIGNORE, { IDABORT,   IDRETRY,   IDIGNORE,   IDS_GENERIC_BLANK, IDS_GENERIC_BLANK, IDS_GENERIC_BLANK,  &m_buttonLeft   } },
    };

    const auto buttonType = type & MB_TYPEMASK;
    assert(buttonTypeContexts.contains(buttonType));
    m_buttonContext = buttonTypeContexts.at(buttonType);

    // Set icon based on message box type
    static const std::array<std::pair<UINT, LPCWSTR>, 4> iconMap
    {{
        { MB_ICONERROR,       IDI_ERROR },
        { MB_ICONQUESTION,    IDI_QUESTION },
        { MB_ICONWARNING,     IDI_WARNING },
        { MB_ICONINFORMATION, IDI_INFORMATION },
    }};

    const auto iconType = type & MB_ICONMASK;
    const auto iconIter = std::ranges::find(iconMap, iconType, &std::pair<UINT, LPCWSTR>::first);
    m_iconName = iconIter != iconMap.end() ? iconIter->second : IDI_INFORMATION;
}

WdsMessageBoxResult CMessageBoxDlg::Show(const std::wstring& message, const std::vector<std::wstring>& listViewItems, const std::wstring& checkboxText, const bool checkboxValue, const UINT type, WindowRef pParent, const CSize& initialSize, const std::wstring& title)
{
    WindowRef parent = pParent != nullptr ? pParent : WindowRef(GetMainWindow());

    CMessageBoxDlg dlg(message, title, type, parent, listViewItems, checkboxText, checkboxValue);

    if (initialSize.cx > 0 || initialSize.cy > 0)
        dlg.SetInitialWindowSize(initialSize);

    return { static_cast<int>(dlg.ShowModal()), dlg.IsCheckboxChecked() };
}

void CMessageBoxDlg::ShiftControls(const std::vector<CWnd*>& controls, const int shiftAmount)
{
    for (auto* pCtrl : controls)
    {
        CRect rect = GetChildWindowRect(*pCtrl);
        rect.Offset(0, shiftAmount);
        pCtrl->MoveWindow(rect);
    }

    // Resize dialog
    CRect dialogRect(Handle());
    dialogRect.bottom += shiftAmount;
    MoveWindow(dialogRect);
}

void CMessageBoxDlg::ShiftControlsIfHidden(const CWnd* pTargetControl, const std::vector<CWnd*>& controlsToShift, const int padding)
{
    // Expand checkbox width to the leftmost visible button
    if (pTargetControl == &m_listView && m_checkbox.GetStyle() & WS_VISIBLE)
    {
        for (const CWnd* pBtn : { &m_buttonLeft, &m_buttonMiddle, &m_buttonRight })
        {
            if (pBtn->Handle() && pBtn->GetStyle() & WS_VISIBLE)
            {
                CRect cbRect = GetChildWindowRect(m_checkbox.Handle());
                const CRect btnRect = GetChildWindowRect(pBtn->Handle());

                cbRect.right = btnRect.left;
                if (cbRect.right > cbRect.left) m_checkbox.MoveWindow(cbRect);
                break;
            }
        }
    }

    if (pTargetControl->GetStyle() & WS_VISIBLE) return;

    const CRect targetRect = GetChildWindowRect(pTargetControl->Handle());

    // Find nearest control below target
    const int minYBelow = std::ranges::fold_left(controlsToShift, INT_MAX,
        [&](const int currentMin, const CWnd* ctrl) {
            const CRect ctrlRect = GetChildWindowRect(ctrl->Handle());
            return ctrlRect.top > targetRect.top ? std::min(currentMin, static_cast<int>(ctrlRect.top)) : currentMin;
        });

    // Calculate shift: control height + spacing to next control, and optional padding
    const int shiftAmount = std::max<int>(0, (minYBelow != INT_MAX ?
        minYBelow - targetRect.top : targetRect.Height()) - ScaleForDpi(padding));

    // Shift controls below target upward
    ShiftControls(controlsToShift, -shiftAmount);
}

bool CMessageBoxDlg::OnInitDialog()
{
    CLayoutDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);

    m_iconCtrl.SubclassDlgItem(IDC_MESSAGE_ICON, this);
    m_messageCtrl.SubclassDlgItem(IDC_MESSAGE_TEXT, this);
    m_buttonLeft.SubclassDlgItem(IDC_MESSAGE_BUTTONLEFT, this);
    m_buttonMiddle.SubclassDlgItem(IDC_MESSAGE_BUTTONMIDDLE, this);
    m_buttonRight.SubclassDlgItem(IDC_MESSAGE_BUTTONRIGHT, this);
    m_checkbox.SubclassDlgItem(IDC_MESSAGE_CHECKBOX, this);
    m_listView.SubclassDlgItem(IDC_MESSAGE_LISTVIEW, this);

    // Set window title and message
    SetText(m_title);
    m_messageCtrl.SetText(m_message);

    // Configure buttons
    m_buttonLeft.ShowWindow(m_buttonContext.btnLeftID != 0 ? SW_SHOW : SW_HIDE);
    m_buttonMiddle.ShowWindow(m_buttonContext.btnMidID != 0 ? SW_SHOW : SW_HIDE);
    m_buttonRight.ShowWindow(m_buttonContext.btnRightID != 0 ? SW_SHOW : SW_HIDE);

    // Set button texts
    m_buttonLeft.SetText(Localization::Lookup(m_buttonContext.btnLeftIDS));
    m_buttonMiddle.SetText(Localization::Lookup(m_buttonContext.btnMidIDS));
    m_buttonRight.SetText(Localization::Lookup(m_buttonContext.btnRightIDS));

    // Set display icon
    const CSize iconSize = m_iconCtrl.GetClientRect().Size();
    m_icon.Release();
    LoadIconWithScaleDown(nullptr, m_iconName, iconSize.cx, iconSize.cy, &m_icon);
    m_iconCtrl.ModifyStyle(0, SS_REALSIZECONTROL);
    m_iconCtrl.SetIcon(m_icon.IsValid() ? m_icon.Get() : LoadIconW(nullptr, m_iconName));

    // Configure optional owner-data list view
    m_listView.ShowWindow(m_listViewItems.empty() ? SW_HIDE : SW_SHOW);
    if (!m_listViewItems.empty())
    {
        std::ranges::sort(m_listViewItems);
        m_listView.SetExtendedStyle(m_listView.GetExtendedStyle() | LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
        m_listView.InsertColumn(0, L"");
        m_listView.SetItemCountEx(static_cast<int>(m_listViewItems.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    }

    // Hide checkbox if no text set
    m_checkbox.SetText(m_checkboxText);
    SetChecked(IDC_MESSAGE_CHECKBOX, m_checkboxChecked);
    m_checkbox.ShowWindow(m_checkboxText.empty() ? SW_HIDE : SW_SHOW);

    // Apply dark mode
    DarkMode::AdjustControls(*this);
    if (DarkMode::IsDarkModeActive())
    {
        const COLORREF listBackColor = DarkMode::SystemColor(COLOR_WINDOW);
        m_listView.SetBkColor(listBackColor);
        m_listView.SetTextBkColor(listBackColor);
        m_listView.SetTextColor(DarkMode::SystemColor(COLOR_WINDOWTEXT));
    }

    // Collapse hidden controls vertically and add padding to vertically center the message area
    ShiftControlsIfHidden(&m_listView, { &m_checkbox, &m_buttonLeft, &m_buttonMiddle, &m_buttonRight }, 16);

    // Measure message text
    const CRect rectMessage = GetChildWindowRect(m_messageCtrl.Handle());

    // Account for control borders/margins
    const CRect rectMessageClient = m_messageCtrl.GetClientRect();
    const int messageBorders = rectMessage.Width() - rectMessageClient.Width();

    // Calculate scaling for initial size requirements
    const CSize scaledInitialSize(
        ScaleForDpi(m_initialSize.cx),
        ScaleForDpi(m_initialSize.cy)
    );

    CRect rectWindow(Handle());
    const int initialWidthExpansion = std::max<int>(0, scaledInitialSize.cx - rectWindow.Width());

    CClientDC dc(&m_messageCtrl);
    GdiObjectSelection selectFont(&dc, m_messageCtrl.GetFont());

    CRect rectTextCalc = rectMessage;
    constexpr UINT baseFlags = DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS;

    if (m_autoWidth)
    {
        // Don't wrap words; calculate full width
        rectTextCalc.right = LONG_MAX;
        dc.DrawText(m_message, &rectTextCalc, baseFlags);
    }
    else
    {
        // Wrap words within the allowed width (current + initial expansion)
        rectTextCalc.right += initialWidthExpansion;

        // Ensure we respect borders when calculating available text width
        rectTextCalc.right -= messageBorders;
        dc.DrawText(m_message, &rectTextCalc, baseFlags | DT_WORDBREAK);
    }

    // Restore border width to the calculated rect for layout consistency
    rectTextCalc.right += messageBorders;

    // Determine Expansion Needed
    int deltaWidth = initialWidthExpansion;
    if (m_autoWidth)
    {
        // If auto-width, ensure we expand enough for the text
        const int textRequiredExpanded = std::max<int>(0, rectTextCalc.Width() - rectMessage.Width());
        deltaWidth = std::max(deltaWidth, textRequiredExpanded);
    }

    // Apply Vertical Expansion
    if (const int deltaHeight = std::max<int>(0, rectTextCalc.Height() - rectMessage.Height()); deltaHeight > 0)
    {
        const int padding = ScaleForDpi(16);
        // Expand message control
        m_messageCtrl.SetWindowPos(nullptr, 0, 0, rectMessage.Width(), rectMessage.Height() + deltaHeight + padding, SWP_NOMOVE | SWP_NOZORDER);

        // Push everything else down
        ShiftControls({ &m_listView, &m_checkbox, &m_buttonLeft, &m_buttonMiddle, &m_buttonRight }, deltaHeight + padding);
    }

    // Activate automatic layout management (snapshots current positions)
    m_layout.AddControl(IDC_MESSAGE_ICON, 0, 0, 0, 0);
    m_layout.AddControl(IDC_MESSAGE_TEXT, 0, 0, 1, 0);
    m_layout.AddControl(IDC_MESSAGE_LISTVIEW, 0, 0, 1, 1);
    m_layout.AddControl(IDC_MESSAGE_CHECKBOX, 0, 1, 0, 0);
    m_layout.AddControl(IDC_MESSAGE_BUTTONLEFT, 1, 1, 0, 0);
    m_layout.AddControl(IDC_MESSAGE_BUTTONMIDDLE, 1, 1, 0, 0);
    m_layout.AddControl(IDC_MESSAGE_BUTTONRIGHT, 1, 1, 0, 0);
    m_layout.OnInitDialog(true);

    // Apply width and final height expansion
    rectWindow = GetWindowRect();
    const int newWidth = rectWindow.Width() + deltaWidth;
    int newHeight = rectWindow.Height();

    // Ensure minimum height from initial size
    newHeight = std::max(static_cast<int>(scaledInitialSize.cy), newHeight);
    if (newWidth != rectWindow.Width() || newHeight != rectWindow.Height())
    {
        SetWindowPos(nullptr, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
    UpdateListViewColumnWidth();

    // Remove resizable border
    if (m_autoWidth) ModifyStyle(WS_THICKFRAME, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    // Center dialog
    CenterWindow();

    // Set focus to default button
    if (m_buttonContext.btnFocus)
    {
        SendMessage(DM_SETDEFID, m_buttonContext.btnFocus->GetDlgCtrlID());
        m_buttonContext.btnFocus->SetFocus();
        return false;
    }

    return true;
}

void CMessageBoxDlg::UpdateListViewColumnWidth()
{
    LVCOLUMN column{ .mask = LVCF_WIDTH };
    if (!m_listView.Handle() || !m_listView.GetColumn(0, &column))
    {
        return;
    }

    const CRect rect = m_listView.GetClientRect();
    m_listView.SetColumnWidth(0, rect.Width());
}

void CMessageBoxDlg::OnSize(const UINT nType, const int cx, const int cy)
{
    CLayoutDialog::OnSize(nType, cx, cy);
    UpdateListViewColumnWidth();
}

void CMessageBoxDlg::OnListViewCustomDraw(NMHDR* pNMHDR, LRESULT* pResult)
{
    *pResult = CDRF_DODEFAULT;

    if (!DarkMode::IsDarkModeActive())
    {
        return;
    }

    if (auto* customDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(pNMHDR);
        customDraw->nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        *pResult = CDRF_NOTIFYITEMDRAW;
    }
    else if (customDraw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
    {
        customDraw->clrText = DarkMode::SystemColor(COLOR_WINDOWTEXT);
        customDraw->clrTextBk = DarkMode::SystemColor(COLOR_WINDOW);
        *pResult = CDRF_DODEFAULT;
    }
}

void CMessageBoxDlg::OnListViewGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult) const
{
    *pResult = false;

    const auto* displayInfo = reinterpret_cast<NMLVDISPINFO*>(pNMHDR);
    const int item = displayInfo->item.iItem;
    if ((displayInfo->item.mask & LVIF_TEXT) == 0 || displayInfo->item.iSubItem != 0 ||
        item < 0 || item >= static_cast<int>(m_listViewItems.size()) ||
        displayInfo->item.pszText == nullptr || displayInfo->item.cchTextMax <= 0)
    {
        return;
    }

    wcsncpy_s(displayInfo->item.pszText, displayInfo->item.cchTextMax,
        m_listViewItems[item].c_str(), _TRUNCATE);
}

void CMessageBoxDlg::OnListViewItemChanging(NMHDR* pNMHDR, LRESULT* pResult)
{
    const auto* listView = reinterpret_cast<NMLISTVIEW*>(pNMHDR);
    *pResult = (listView->uChanged & LVIF_STATE) != 0 &&
        ((listView->uNewState ^ listView->uOldState) & LVIS_SELECTED) != 0;
}

void CMessageBoxDlg::OnButtonLeft()
{
    m_checkboxChecked = IsChecked(IDC_MESSAGE_CHECKBOX);
    CloseModal(m_buttonContext.btnLeftID);
}

void CMessageBoxDlg::OnButtonMiddle()
{
    m_checkboxChecked = IsChecked(IDC_MESSAGE_CHECKBOX);
    CloseModal(m_buttonContext.btnMidID);
}

void CMessageBoxDlg::OnButtonRight()
{
    m_checkboxChecked = IsChecked(IDC_MESSAGE_CHECKBOX);
    CloseModal(m_buttonContext.btnRightID);
}

INT_PTR CMessageBoxDlg::ShowModal()
{
    // Direct2D glass message box; falls back to the classic dialog when GPU composition is unavailable
    std::vector<GlassUi::MessageButton> buttons;
    const auto addButton = [&](const BYTE id, const std::wstring_view name, const CButton* button)
    {
        if (id != 0) buttons.push_back({ id, Localization::Lookup(name), m_buttonContext.btnFocus == button });
    };
    addButton(m_buttonContext.btnLeftID, m_buttonContext.btnLeftIDS, &m_buttonLeft);
    addButton(m_buttonContext.btnMidID, m_buttonContext.btnMidIDS, &m_buttonMiddle);
    addButton(m_buttonContext.btnRightID, m_buttonContext.btnRightIDS, &m_buttonRight);
    int kind = 0;
    if (m_iconName == IDI_QUESTION) kind = 1;
    else if (m_iconName == IDI_WARNING) kind = 2;
    else if (m_iconName == IDI_ERROR) kind = 3;
    int result = IDCANCEL;
    if (GlassUi::ShowMessage(GetDialogOwner(m_parent), m_title, m_message, kind, buttons, m_listViewItems,
        m_checkboxText, m_checkboxChecked, result)) return result;

    return CLayoutDialog::ShowModal();
}

HBRUSH CMessageBoxDlg::OnCtlColor(CDC* pDC, WindowRef pWnd, const UINT nCtlColor)
{
    // Labels, icon and checkbox all sit on the glass backdrop (handled by the base dialog)
    return CLayoutDialog::OnCtlColor(pDC, pWnd, nCtlColor);
}

bool CMessageBoxDlg::OnEraseBkgnd(CDC* pDC) const
{
    const CRect rect = GetClientRect();
    GlassTheme::PaintBackdrop(pDC->Handle(), m_hWnd);

    int lineY = rect.bottom - ScaleForDpi(46);
    if (m_buttonRight.Handle())
    {
        const CRect btnRect = GetChildWindowRect(m_buttonRight.Handle());
        lineY = btnRect.top - ScaleForDpi(12);
    }

    // Etched glass separator above the button row
    const bool dark = DarkMode::IsDarkModeActive();
    pDC->FillSolidRect(CRect(rect.left, lineY, rect.right, lineY + 1), dark ? RGB(70, 80, 130) : RGB(255, 255, 255));
    pDC->FillSolidRect(CRect(rect.left, lineY + 1, rect.right, lineY + 2), dark ? RGB(10, 12, 30) : RGB(196, 208, 236));
    return true;
}

// Global wrapper functions
int ShowMessageBox(const std::wstring& message, const UINT type)
{
    return ShowMessageBox(nullptr, message, wds::strWinDirStat, type);
}

int ShowMessageBox(const HWND wnd, const std::wstring& message, const std::wstring& title, const UINT type)
{
    CMessageBoxDlg dlg(message, title, type, WindowRef(wnd));
    return static_cast<int>(dlg.ShowModal());
}
