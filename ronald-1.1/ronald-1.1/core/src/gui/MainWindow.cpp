#include "MainWindow.h"
#include "resource.h"
#include "../Wiper.h"

#include <windowsx.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>

namespace {
constexpr wchar_t kMainWndClass[] = L"HexEditorMainWindowClass";
}

bool MainWindow::Create(HINSTANCE hInst, int nCmdShow) {
    m_hInst = hInst;
    HexGridControl::RegisterWindowClass(hInst);
    TabBar::RegisterWindowClass(hInst);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WndProcStatic;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE) + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = kMainWndClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(0, kMainWndClass, L"Hex Editor",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1024, 680,
                              nullptr, nullptr, hInst, this);
    if (!m_hwnd) return false;

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    return true;
}

LRESULT CALLBACK MainWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: OnCreate(hwnd); return 0;
        case WM_SIZE: OnSize(); return 0;
        case WM_COMMAND: OnCommand(LOWORD(wParam)); return 0;
        case WM_CLOSE: OnClose(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MainWindow::OnCreate(HWND hwnd) {
    m_hMenu = GetMenu(hwnd);
    CreateToolbar(hwnd);

    m_tabBar.Create(hwnd, m_hInst, IDC_TABBAR);
    m_tabBar.SetHost(this);

    CreateStatusBarControl(hwnd);

    m_grid.Create(hwnd, m_hInst, IDC_HEXGRID);
    m_grid.SetHost(this);

    LayoutChildren();
    UpdateUiState();
}

void MainWindow::CreateToolbar(HWND parent) {
    m_hToolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | CCS_NODIVIDER,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TOOLBAR)), m_hInst, nullptr);

    SendMessageW(m_hToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    TBBUTTON buttons[8]{};
    int n = 0;

    auto addButton = [&](int cmd, const wchar_t* text) {
        buttons[n].iBitmap = I_IMAGENONE;
        buttons[n].idCommand = cmd;
        buttons[n].fsState = TBSTATE_ENABLED;
        buttons[n].fsStyle = BTNS_AUTOSIZE | BTNS_SHOWTEXT;
        buttons[n].iString = reinterpret_cast<INT_PTR>(text);
        ++n;
    };
    auto addSeparator = [&]() {
        buttons[n].fsStyle = BTNS_SEP;
        ++n;
    };

    addButton(IDM_FILE_OPEN, L"Open");
    addButton(IDM_FILE_SAVE, L"Save");
    addButton(IDM_FILE_SAVEAS, L"Save As");
    addSeparator();
    addButton(IDM_EDIT_UNDO, L"Undo");
    addButton(IDM_EDIT_REDO, L"Redo");
    addSeparator();
    addButton(IDM_EDIT_FIND, L"Find");

    SendMessageW(m_hToolbar, TB_ADDBUTTONSW, n, reinterpret_cast<LPARAM>(buttons));
    SendMessageW(m_hToolbar, TB_AUTOSIZE, 0, 0);
}

void MainWindow::CreateStatusBarControl(HWND parent) {
    m_hStatusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUSBAR)), m_hInst, nullptr);
}

void MainWindow::LayoutChildren() {
    if (!m_hwnd) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    int cw = rc.right - rc.left;

    SendMessageW(m_hToolbar, TB_AUTOSIZE, 0, 0);
    RECT tbRc{};
    GetWindowRect(m_hToolbar, &tbRc);
    int toolbarHeight = tbRc.bottom - tbRc.top;

    SendMessageW(m_hStatusBar, WM_SIZE, 0, 0);
    RECT sbRc{};
    GetWindowRect(m_hStatusBar, &sbRc);
    int statusHeight = sbRc.bottom - sbRc.top;

    int tabBarHeight = TabBar::kHeight;
    MoveWindow(m_tabBar.GetHwnd(), 0, toolbarHeight, cw, tabBarHeight, TRUE);

    int p0 = std::max(140, cw - 360);
    int p1 = p0 + 140;
    int p2 = p1 + 140;
    int widths[4] = {p0, p1, p2, -1};
    SendMessageW(m_hStatusBar, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(widths));

    int gridY = toolbarHeight + tabBarHeight;
    int gridHeight = (rc.bottom - rc.top) - gridY - statusHeight;
    if (gridHeight < 0) gridHeight = 0;
    MoveWindow(m_grid.GetHwnd(), 0, gridY, cw, gridHeight, TRUE);
}

void MainWindow::OnSize() {
    LayoutChildren();
}

void MainWindow::OnCommand(int id) {
    switch (id) {
        case IDM_FILE_OPEN: CmdOpen(); break;
        case IDM_FILE_SAVE: CmdSave(); break;
        case IDM_FILE_SAVEALL: CmdSaveAll(); break;
        case IDM_FILE_SAVEAS: CmdSaveAs(); break;
        case IDM_FILE_CLOSE: CmdClose(); break;
        case IDM_FILE_EXIT: PostMessageW(m_hwnd, WM_CLOSE, 0, 0); break;
        case IDM_EDIT_UNDO: CmdUndo(); break;
        case IDM_EDIT_REDO: CmdRedo(); break;
        case IDM_EDIT_SELECTALL: CmdSelectAll(); break;
        case IDM_EDIT_COPY: CmdCopy(); break;
        case IDM_EDIT_PASTE: CmdPaste(); break;
        case IDM_EDIT_DELETE: CmdDelete(); break;
        case IDM_EDIT_FIND: CmdFind(); break;
        case IDM_FILE_NEXTTAB: CmdNextTab(); break;
        case IDM_FILE_PREVTAB: CmdPrevTab(); break;
        case IDM_TOOLS_WIPE_EDITOR_TEMPS: CmdWipeEditorTemps(); break;
        case IDM_TOOLS_WIPE_FREE_SPACE: CmdWipeFreeSpace(); break;
        case IDM_HELP_ABOUT: CmdAbout(); break;
        default: break;
    }
}

HHEXDOC MainWindow::ActiveDoc() const {
    return (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_docs.size()))
               ? m_docs[static_cast<size_t>(m_activeIndex)].doc
               : nullptr;
}

std::wstring MainWindow::ActivePath() const {
    return (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_docs.size()))
               ? m_docs[static_cast<size_t>(m_activeIndex)].path
               : std::wstring();
}

std::wstring MainWindow::FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

int MainWindow::FindOpenIndexByPath(const std::wstring& path) const {
    wchar_t fullNew[32768];
    DWORD lenNew = GetFullPathNameW(path.c_str(), 32768, fullNew, nullptr);
    if (lenNew == 0 || lenNew >= 32768) return -1;

    for (size_t i = 0; i < m_docs.size(); ++i) {
        wchar_t fullExisting[32768];
        DWORD lenExisting = GetFullPathNameW(m_docs[i].path.c_str(), 32768, fullExisting, nullptr);
        if (lenExisting > 0 && lenExisting < 32768 && _wcsicmp(fullNew, fullExisting) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void MainWindow::SwitchToTab(int index) {
    if (index == m_activeIndex) return;

    if (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_docs.size())) {
        m_docs[static_cast<size_t>(m_activeIndex)].view = m_grid.CaptureViewState();
    }

    m_activeIndex = index;
    if (index >= 0 && index < static_cast<int>(m_docs.size())) {
        m_grid.SetDocument(m_docs[static_cast<size_t>(index)].doc, &m_docs[static_cast<size_t>(index)].view);
        m_tabBar.SetActive(index);
    } else {
        m_grid.SetDocument(nullptr);
        m_tabBar.SetActive(-1);
    }
    UpdateUiState();

    // Move keyboard focus to the grid so the user can start typing hex
    // digits immediately - after Open, after switching tabs, after a
    // tab closes and another becomes active, etc. Without this, the
    // grid never receives WM_CHAR until the user clicks a cell first.
    SetFocus(m_grid.GetHwnd());
}

void MainWindow::OnTabActivated(int index) {
    SwitchToTab(index);
}

void MainWindow::OnTabCloseRequested(int index) {
    CloseTab(index);
}

void MainWindow::OpenFiles(const std::vector<std::wstring>& paths) {
    int firstIndex = -1;

    for (const auto& path : paths) {
        int existing = FindOpenIndexByPath(path);
        if (existing >= 0) {
            if (firstIndex < 0) firstIndex = existing;
            continue;
        }

        HexCoreStatus st = HEXCORE_OK;
        HHEXDOC doc = HexCore_Open(path.c_str(), &st);
        if (!doc) {
            ShowErrorStatus(st, L"opening the file");
            continue;
        }

        OpenDocument od;
        od.doc = doc;
        od.path = path;
        m_docs.push_back(od);
        int newIndex = static_cast<int>(m_docs.size()) - 1;
        m_tabBar.AddTab(FileNameOf(path));
        if (firstIndex < 0) firstIndex = newIndex;
    }

    if (firstIndex >= 0) {
        m_activeIndex = -1; // force SwitchToTab to actually apply below
        SwitchToTab(firstIndex);
    }
}

bool MainWindow::CloseTab(int index) {
    if (index < 0 || index >= static_cast<int>(m_docs.size())) return true;

    HHEXDOC doc = m_docs[static_cast<size_t>(index)].doc;
    if (HexCore_IsModified(doc)) {
        std::wstring msg = L"\"" + m_docs[static_cast<size_t>(index)].path +
                            L"\" has unsaved changes.\n\nSave them now?";
        int r = MessageBoxW(m_hwnd, msg.c_str(), L"Hex Editor", MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return false;
        if (r == IDYES) {
            HexCoreStatus st = HexCore_Save(doc);
            if (st != HEXCORE_OK && st != HEXCORE_ERROR_NOT_MODIFIED) {
                ShowErrorStatus(st, L"saving the file", doc);
                return false;
            }
        }
    }

    bool wasActive = (index == m_activeIndex);
    HexCore_Close(doc);
    m_docs.erase(m_docs.begin() + index);
    m_tabBar.RemoveTab(index);

    if (m_docs.empty()) {
        m_activeIndex = -1;
        m_grid.SetDocument(nullptr);
    } else if (wasActive) {
        int newActive = std::min(index, static_cast<int>(m_docs.size()) - 1);
        m_activeIndex = -1; // force SwitchToTab to actually apply
        SwitchToTab(newActive);
    } else if (m_activeIndex > index) {
        m_activeIndex--;
    }
    UpdateUiState();
    return true;
}

void MainWindow::UpdateUiState() {
    HHEXDOC doc = ActiveDoc();
    bool hasDoc = (doc != nullptr);
    bool modified = hasDoc && HexCore_IsModified(doc);
    bool canUndo = hasDoc && HexCore_CanUndo(doc);
    bool canRedo = hasDoc && HexCore_CanRedo(doc);

    SendMessageW(m_hToolbar, TB_ENABLEBUTTON, IDM_FILE_SAVE, MAKELONG(modified, 0));
    SendMessageW(m_hToolbar, TB_ENABLEBUTTON, IDM_FILE_SAVEAS, MAKELONG(hasDoc, 0));
    SendMessageW(m_hToolbar, TB_ENABLEBUTTON, IDM_EDIT_UNDO, MAKELONG(canUndo, 0));
    SendMessageW(m_hToolbar, TB_ENABLEBUTTON, IDM_EDIT_REDO, MAKELONG(canRedo, 0));
    SendMessageW(m_hToolbar, TB_ENABLEBUTTON, IDM_EDIT_FIND, MAKELONG(hasDoc, 0));

    bool anyModified = false;
    for (const auto& d : m_docs) {
        if (HexCore_IsModified(d.doc)) { anyModified = true; break; }
    }

    EnableMenuItem(m_hMenu, IDM_FILE_SAVE, MF_BYCOMMAND | (modified ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_FILE_SAVEALL, MF_BYCOMMAND | (anyModified ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_FILE_SAVEAS, MF_BYCOMMAND | (hasDoc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_FILE_CLOSE, MF_BYCOMMAND | (hasDoc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_EDIT_UNDO, MF_BYCOMMAND | (canUndo ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_EDIT_REDO, MF_BYCOMMAND | (canRedo ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_EDIT_SELECTALL, MF_BYCOMMAND | (hasDoc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_EDIT_COPY, MF_BYCOMMAND | (hasDoc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_EDIT_PASTE, MF_BYCOMMAND | (hasDoc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_EDIT_DELETE, MF_BYCOMMAND | (hasDoc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_hMenu, IDM_EDIT_FIND, MF_BYCOMMAND | (hasDoc ? MF_ENABLED : MF_GRAYED));

    if (m_activeIndex >= 0) {
        m_tabBar.SetModified(m_activeIndex, modified);
    }

    wchar_t buf[128];
    if (hasDoc) {
        std::wstring path = ActivePath();
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(path.c_str()));

        swprintf(buf, 128, L"Size: %llu bytes", static_cast<unsigned long long>(HexCore_GetSize(doc)));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(buf));

        wchar_t offBuf[64];
        swprintf(offBuf, 64, L"Offset: 0x%08llX", static_cast<unsigned long long>(m_grid.GetSelectionOffset()));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(offBuf));

        SendMessageW(m_hStatusBar, SB_SETTEXTW, 3, reinterpret_cast<LPARAM>(modified ? L"● Modified" : L"Saved"));
    } else {
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"No file open"));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L""));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(L""));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 3, reinterpret_cast<LPARAM>(L""));
    }

    UpdateTitle();
}

void MainWindow::UpdateTitle() {
    std::wstring title = L"Hex Editor";
    HHEXDOC doc = ActiveDoc();
    if (doc) {
        std::wstring name = FileNameOf(ActivePath());
        title = name + (HexCore_IsModified(doc) ? L" * - Hex Editor" : L" - Hex Editor");
    }
    SetWindowTextW(m_hwnd, title.c_str());
}

void MainWindow::ShowErrorStatus(HexCoreStatus status, const wchar_t* fallbackContext, HHEXDOC doc) {
    const wchar_t* detail = HexCore_GetLastErrorMessage(doc);
    std::wstring msg = std::wstring(L"An error occurred while ") + fallbackContext + L".\n\n" +
                        HexCore_DescribeStatus(status);
    if (detail && detail[0] != L'\0') {
        msg += L"\n\nDetails: ";
        msg += detail;
    }
    MessageBoxW(m_hwnd, msg.c_str(), L"Hex Editor", MB_OK | MB_ICONERROR);
}

void MainWindow::CmdOpen() {
    std::vector<wchar_t> fileBuf(65536, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = static_cast<DWORD>(fileBuf.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    // Parse the (possibly multi-select) buffer: a single selection is
    // just the full path; multiple selections come back as
    // "directory\0file1\0file2\0...\0\0".
    std::vector<std::wstring> paths;
    wchar_t* p = fileBuf.data();
    std::wstring first = p;
    p += first.size() + 1;
    if (*p == L'\0') {
        paths.push_back(first);
    } else {
        std::wstring dir = first;
        while (*p) {
            std::wstring name = p;
            p += name.size() + 1;
            std::wstring full = dir;
            if (!full.empty() && full.back() != L'\\') full += L'\\';
            full += name;
            paths.push_back(full);
        }
    }

    OpenFiles(paths);
}

void MainWindow::CmdSave() {
    HHEXDOC doc = ActiveDoc();
    if (!doc || !HexCore_IsModified(doc)) return;

    std::wstring msg = L"Save changes and overwrite the original file?\n\n" + ActivePath() +
                        L"\n\nThe file will keep its current name, extension and location.";
    int r = MessageBoxW(m_hwnd, msg.c_str(), L"Confirm Save", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) return;

    SaveInternal();
}

bool MainWindow::SaveInternal() {
    HHEXDOC doc = ActiveDoc();
    if (!doc) return true;
    HexCoreStatus st = HexCore_Save(doc);
    if (st == HEXCORE_ERROR_NOT_MODIFIED) return true;
    if (st != HEXCORE_OK) {
        ShowErrorStatus(st, L"saving the file", doc);
        return false;
    }
    m_grid.Refresh();
    UpdateUiState();
    return true;
}

void MainWindow::CmdSaveAll() {
    std::vector<int> modifiedIndices;
    for (size_t i = 0; i < m_docs.size(); ++i) {
        if (HexCore_IsModified(m_docs[i].doc)) modifiedIndices.push_back(static_cast<int>(i));
    }
    if (modifiedIndices.empty()) return;

    std::wstring msg = L"Save changes and overwrite the following files?\n\n";
    for (int idx : modifiedIndices) msg += m_docs[static_cast<size_t>(idx)].path + L"\n";
    msg += L"\nEach file will keep its current name, extension and location.";
    int r = MessageBoxW(m_hwnd, msg.c_str(), L"Confirm Save All", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) return;

    for (int idx : modifiedIndices) {
        HHEXDOC doc = m_docs[static_cast<size_t>(idx)].doc;
        HexCoreStatus st = HexCore_Save(doc);
        if (st != HEXCORE_OK && st != HEXCORE_ERROR_NOT_MODIFIED) {
            ShowErrorStatus(st, L"saving the file", doc);
        }
    }
    m_grid.Refresh();
    UpdateUiState();
}

void MainWindow::CmdSaveAs() {
    HHEXDOC doc = ActiveDoc();
    if (!doc) return;

    wchar_t fileBuf[MAX_PATH] = {0};
    wcsncpy_s(fileBuf, ActivePath().c_str(), MAX_PATH - 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    HexCoreStatus st = HexCore_SaveAs(doc, fileBuf, TRUE);
    if (st != HEXCORE_OK) {
        ShowErrorStatus(st, L"saving the file", doc);
        return;
    }

    m_docs[static_cast<size_t>(m_activeIndex)].path = fileBuf;
    m_tabBar.SetDisplayName(m_activeIndex, FileNameOf(fileBuf));
    m_grid.Refresh();
    UpdateUiState();
}

void MainWindow::CmdClose() {
    CloseTab(m_activeIndex);
}

void MainWindow::CmdUndo() {
    HHEXDOC doc = ActiveDoc();
    if (!doc) return;
    if (HexCore_Undo(doc) == HEXCORE_OK) {
        m_grid.Refresh();
        UpdateUiState();
    }
}

void MainWindow::CmdRedo() {
    HHEXDOC doc = ActiveDoc();
    if (!doc) return;
    if (HexCore_Redo(doc) == HEXCORE_OK) {
        m_grid.Refresh();
        UpdateUiState();
    }
}

void MainWindow::CmdSelectAll() {
    if (!ActiveDoc()) return;
    m_grid.SelectAll();
    SetFocus(m_grid.GetHwnd());
}

void MainWindow::CmdCopy() {
    if (!ActiveDoc()) return;
    m_grid.CopySelectionToClipboard();
}

void MainWindow::CmdPaste() {
    if (!ActiveDoc()) return;
    m_grid.PasteFromClipboard();
    SetFocus(m_grid.GetHwnd());
}

void MainWindow::CmdDelete() {
    if (!ActiveDoc()) return;
    m_grid.DeleteSelection();
    SetFocus(m_grid.GetHwnd());
}

void MainWindow::CmdNextTab() {
    if (m_docs.empty()) return;
    int next = (m_activeIndex + 1) % static_cast<int>(m_docs.size());
    SwitchToTab(next);
}

void MainWindow::CmdPrevTab() {
    if (m_docs.empty()) return;
    int count = static_cast<int>(m_docs.size());
    int prev = (m_activeIndex - 1 + count) % count;
    SwitchToTab(prev);
}

void MainWindow::CmdFind() {
    if (!ActiveDoc()) return;
    FindDialog::Show(m_hwnd, m_hInst, this);
}

void MainWindow::CmdAbout() {
    MessageBoxW(m_hwnd,
        L"Hex Editor 1.0\n\n"
        L"A professional hexadecimal file editor.\n"
        L"Core editing engine: HexEditorCore.dll\n\n"
        L"Open several files at once as tabs, select ranges (drag or "
        L"Shift+click), copy/paste hex, and Select All per file - "
        L"inspired by Sublime Text's multi-document workflow.\n\n"
        L"Files are treated strictly as binary data and are never executed. "
        L"Changes are only written to disk when you choose Save or Save As.",
        L"About Hex Editor", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::CmdWipeEditorTemps() {
    std::wstring dir;
    if (ActiveDoc()) {
        std::wstring path = ActivePath();
        size_t slash = path.find_last_of(L"\\/");
        dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    } else {
        wchar_t cwd[MAX_PATH] = {0};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        dir = cwd;
    }

    std::wstring msg = L"This permanently overwrites and deletes any leftover "
                        L"hxe*.TMP safe-save temp files in:\n\n" + dir +
                        L"\n\nThis cannot be undone. Continue?";
    int r = MessageBoxW(m_hwnd, msg.c_str(), L"Wipe Editor Temp Files", MB_YESNO | MB_ICONWARNING);
    if (r != IDYES) return;

    int count = hexcore::Wiper::WipeEditorTemps(dir);
    wchar_t buf[256];
    swprintf(buf, 256, L"Wiped and deleted %d temp file(s).", count);
    MessageBoxW(m_hwnd, buf, L"Wipe Editor Temp Files", MB_OK | MB_ICONINFORMATION);
}

void MainWindow::CmdWipeFreeSpace() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t picked[MAX_PATH] = {0};
    BROWSEINFOW bi{};
    bi.hwndOwner = m_hwnd;
    bi.lpszTitle = L"Select a drive or folder whose free space should be wiped";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    bool haveFolder = pidl && SHGetPathFromIDListW(pidl, picked);
    if (pidl) CoTaskMemFree(pidl);
    CoUninitialize();
    if (!haveFolder) return;

    wchar_t volumeRoot[MAX_PATH] = {0};
    if (!GetVolumePathNameW(picked, volumeRoot, MAX_PATH)) {
        wcsncpy_s(volumeRoot, picked, MAX_PATH - 1);
    }

    std::wstring msg = L"This fills ALL free space on:\n\n" + std::wstring(volumeRoot) +
                        L"\n\nwith zeros so deleted files there become unrecoverable, then "
                        L"removes the temporary fill file. The drive will report as full "
                        L"while this runs, and it can take a long time on a large drive.\n\n"
                        L"Continue?";
    int r = MessageBoxW(m_hwnd, msg.c_str(), L"Wipe Free Space", MB_YESNO | MB_ICONWARNING);
    if (r != IDYES) return;

    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    int64_t written = hexcore::Wiper::WipeFreeSpace(volumeRoot, m_hwnd);
    SetCursor(oldCursor);

    wchar_t buf[256];
    if (written < 0) {
        swprintf(buf, 256, L"Could not wipe free space on %s.", volumeRoot);
        MessageBoxW(m_hwnd, buf, L"Wipe Free Space", MB_OK | MB_ICONERROR);
    } else {
        swprintf(buf, 256, L"Wiped %.1f MB of free space on %s.",
                 written / (1024.0 * 1024.0), volumeRoot);
        MessageBoxW(m_hwnd, buf, L"Wipe Free Space", MB_OK | MB_ICONINFORMATION);
    }
}

bool MainWindow::OnClose() {
    while (!m_docs.empty()) {
        if (!CloseTab(0)) return false;
    }
    DestroyWindow(m_hwnd);
    return true;
}

void MainWindow::OnHexGridEdited() {
    UpdateUiState();
}

void MainWindow::OnHexGridSelectionChanged(uint64_t /*offset*/) {
    UpdateUiState();
}

void MainWindow::FindHost_GoToOffset(uint64_t offset) {
    m_grid.GoToOffset(offset);
    SetFocus(m_grid.GetHwnd());
    UpdateUiState();
}
