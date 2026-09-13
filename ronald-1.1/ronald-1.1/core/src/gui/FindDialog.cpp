#include "FindDialog.h"
#include "resource.h"

#include <string>
#include <vector>

HWND FindDialog::s_hwnd = nullptr;
IFindHost* FindDialog::s_findHost = nullptr;

void FindDialog::Show(HWND parent, HINSTANCE hInst, IFindHost* host) {
    s_findHost = host;
    if (s_hwnd) {
        ShowWindow(s_hwnd, SW_SHOW);
        SetForegroundWindow(s_hwnd);
        SetFocus(GetDlgItem(s_hwnd, IDC_FIND_PATTERN));
        return;
    }
    s_hwnd = CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_FIND), parent, DlgProc, 0);
    if (s_hwnd) ShowWindow(s_hwnd, SW_SHOW);
}

void FindDialog::UpdateModeUi(HWND hDlg) {
    bool textMode = IsDlgButtonChecked(hDlg, IDC_FIND_MODE_TEXT) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_FIND_CASESENS), textMode ? TRUE : FALSE);
}

void FindDialog::DoFind(HWND hDlg, bool forward) {
    if (!s_findHost) return;
    HHEXDOC doc = s_findHost->FindHost_GetDocument();
    if (!doc) {
        SetDlgItemTextW(hDlg, IDC_FIND_STATUS, L"Open a file before searching.");
        return;
    }

    wchar_t patternW[512];
    GetDlgItemTextW(hDlg, IDC_FIND_PATTERN, patternW, 512);
    if (wcslen(patternW) == 0) {
        SetDlgItemTextW(hDlg, IDC_FIND_STATUS, L"Enter a value to search for.");
        return;
    }

    bool hexMode = IsDlgButtonChecked(hDlg, IDC_FIND_MODE_HEX) == BST_CHECKED;
    uint64_t selection = s_findHost->FindHost_GetSelectionOffset();
    uint64_t startOffset = forward ? selection + 1 : selection;

    HexSearchResult result{};
    if (hexMode) {
        // Narrow to ASCII for the parser (hex digits are always ASCII).
        char narrow[512];
        WideCharToMultiByte(CP_ACP, 0, patternW, -1, narrow, 512, nullptr, nullptr);

        uint8_t bytes[256];
        size_t len = 0;
        if (!HexCore_ParseHexString(narrow, bytes, 256, &len) || len == 0) {
            SetDlgItemTextW(hDlg, IDC_FIND_STATUS, L"Invalid hex string. Use pairs like: DE AD BE EF");
            return;
        }
        result = HexCore_FindHex(doc, bytes, len, startOffset, forward ? 1 : 0);
    } else {
        char narrow[512];
        WideCharToMultiByte(CP_ACP, 0, patternW, -1, narrow, 512, nullptr, nullptr);
        bool caseSensitive = IsDlgButtonChecked(hDlg, IDC_FIND_CASESENS) == BST_CHECKED;
        result = HexCore_FindText(doc, narrow, startOffset, caseSensitive ? 1 : 0, forward ? 1 : 0);
    }

    if (result.found) {
        wchar_t msg[128];
        swprintf(msg, 128, L"Found at offset 0x%08llX.", static_cast<unsigned long long>(result.offset));
        SetDlgItemTextW(hDlg, IDC_FIND_STATUS, msg);
        s_findHost->FindHost_GoToOffset(result.offset);
    } else {
        SetDlgItemTextW(hDlg, IDC_FIND_STATUS,
                        forward ? L"Not found (reached end of file)." : L"Not found (reached start of file).");
    }
}

INT_PTR CALLBACK FindDialog::DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            CheckRadioButton(hDlg, IDC_FIND_MODE_HEX, IDC_FIND_MODE_TEXT, IDC_FIND_MODE_HEX);
            UpdateModeUi(hDlg);
            SetFocus(GetDlgItem(hDlg, IDC_FIND_PATTERN));
            return FALSE; // we set focus ourselves

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_FIND_MODE_HEX:
                case IDC_FIND_MODE_TEXT:
                    UpdateModeUi(hDlg);
                    return TRUE;
                case IDC_FIND_NEXT:
                    DoFind(hDlg, true);
                    return TRUE;
                case IDC_FIND_PREV:
                    DoFind(hDlg, false);
                    return TRUE;
                case IDC_FIND_CLOSE:
                    DestroyWindow(hDlg);
                    return TRUE;
                default:
                    break;
            }
            return FALSE;

        case WM_CLOSE:
            DestroyWindow(hDlg);
            return TRUE;

        case WM_DESTROY:
            s_hwnd = nullptr;
            return TRUE;

        default:
            break;
    }
    return FALSE;
}
