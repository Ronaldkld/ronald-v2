// MainWindow - the application's top-level window. Owns the toolbar,
// tab strip, status bar, menu and the (single, reused) HexGridControl,
// and is the only place that talks to HexEditorCore.dll's document
// lifecycle (open/save/undo/redo). The hex grid and find dialog never
// touch files directly.
//
// Multiple files can be open at once, each as its own tab (Sublime
// Text-style) - see m_docs below. Only the active tab's document is
// bound to the single shared HexGridControl at any time; switching
// tabs captures the outgoing view (scroll/selection) and restores the
// incoming one.
#pragma once

#include <windows.h>
#include <commctrl.h>
#include <hexcore/HexEditorCore.h>
#include <string>
#include <thread>
#include <vector>

#include "HexGridControl.h"
#include "FindDialog.h"
#include "TabBar.h"

class MainWindow : public IHexGridHost, public IFindHost, public ITabBarHost {
public:
    bool Create(HINSTANCE hInst, int nCmdShow);
    HWND GetHwnd() const { return m_hwnd; }

    // IHexGridHost
    void OnHexGridEdited() override;
    void OnHexGridSelectionChanged(uint64_t offset) override;

    // IFindHost
    HHEXDOC FindHost_GetDocument() override { return ActiveDoc(); }
    uint64_t FindHost_GetSelectionOffset() override { return m_grid.GetSelectionOffset(); }
    void FindHost_GoToOffset(uint64_t offset) override;

    // ITabBarHost
    void OnTabActivated(int index) override;
    void OnTabCloseRequested(int index) override;

private:
    struct OpenDocument {
        HHEXDOC doc = nullptr;
        std::wstring path;
        HexGridViewState view;
    };

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void OnCreate(HWND hwnd);
    void OnSize();
    void OnCommand(int id);
    bool OnClose();

    void CreateToolbar(HWND parent);
    void CreateStatusBarControl(HWND parent);
    void LayoutChildren();
    void UpdateUiState();
    void UpdateTitle();
    void ShowErrorStatus(HexCoreStatus status, const wchar_t* fallbackContext, HHEXDOC doc = nullptr);

    HHEXDOC ActiveDoc() const;
    std::wstring ActivePath() const;
    int FindOpenIndexByPath(const std::wstring& path) const;
    static std::wstring FileNameOf(const std::wstring& path);

    // Switches the shared HexGridControl to a different already-open
    // tab, saving the outgoing tab's scroll/selection state first.
    void SwitchToTab(int index);

    // Opens each path as a new tab (does not touch already-open tabs).
    void OpenFiles(const std::vector<std::wstring>& paths);

    // Closes one tab, prompting to save first if modified. Returns
    // false if the user cancelled (so callers can abort a larger
    // operation, e.g. closing the app).
    bool CloseTab(int index);

    bool SaveInternal();

    void CmdOpen();
    void CmdSave();
    void CmdSaveAll();
    void CmdSaveAs();
    void CmdClose();
    void CmdUndo();
    void CmdRedo();
    void CmdFind();
    void CmdAbout();
    void CmdSelectAll();
    void CmdCopy();
    void CmdPaste();
    void CmdDelete();
    void CmdNextTab();
    void CmdPrevTab();
    void CmdWipeEditorTemps();
    void CmdWipeFreeSpace();

    // Tools > Wipe free space runs Wiper::WipeFreeSpace on a worker
    // thread (it can take a long time) with a small modeless progress
    // window that polls Wiper::GetBytesWrittenSoFar() and can cancel it.
    static LRESULT CALLBACK WipeProgressWndProc(HWND, UINT, WPARAM, LPARAM);
    void CreateWipeProgressWindow(const std::wstring& volumeRoot);
    void OnWipeProgressTick();
    void OnWipeFreeSpaceDone(int64_t written);

    HINSTANCE m_hInst = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_hToolbar = nullptr;
    HWND m_hStatusBar = nullptr;
    HMENU m_hMenu = nullptr;

    TabBar m_tabBar;
    HexGridControl m_grid;

    std::vector<OpenDocument> m_docs;
    int m_activeIndex = -1;

    HWND m_hWipeProgress = nullptr;
    HWND m_hWipeProgressLabel = nullptr;
    std::thread m_wipeThread;
    std::wstring m_wipeVolumeRoot;
};
