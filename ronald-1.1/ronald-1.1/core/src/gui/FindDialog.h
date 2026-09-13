// Modeless "Find" dialog supporting hex-byte and ASCII-text search,
// forward and backward, over whatever document the host currently has
// open. Talks to the host purely through the small IFindHost interface
// so it has zero knowledge of MainWindow's internals.
#pragma once

#include <windows.h>
#include <hexcore/HexEditorCore.h>
#include <cstdint>

class IFindHost {
public:
    virtual ~IFindHost() = default;
    virtual HHEXDOC FindHost_GetDocument() = 0;
    virtual uint64_t FindHost_GetSelectionOffset() = 0;
    virtual void FindHost_GoToOffset(uint64_t offset) = 0;
};

class FindDialog {
public:
    static void Show(HWND parent, HINSTANCE hInst, IFindHost* host);
    static HWND GetHwnd() { return s_hwnd; }

private:
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
    static void DoFind(HWND hDlg, bool forward);
    static void UpdateModeUi(HWND hDlg);

    static HWND s_hwnd;
    static IFindHost* s_findHost;
};
