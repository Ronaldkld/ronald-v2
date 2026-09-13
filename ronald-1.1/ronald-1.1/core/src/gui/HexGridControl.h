// HexGridControl - a self-contained, owner-drawn Win32 child control
// that renders a classic "offset | hex bytes | ascii" hex-editor view,
// with Sublime Text-style range selection (drag, shift+click,
// shift+arrows), Select All, and hex clipboard copy/paste.
//
// It never loads more than one screen's worth of bytes from the
// document at a time (via HexCore_ReadBytes), so it scales to very
// large files without ballooning memory use. All editing intent goes
// straight through the HexEditorCore C API - this class holds no
// document state of its own beyond the read-only handle, the current
// scroll position, and the current selection/cursor.
//
// One instance is reused across all open tabs (see MainWindow); when
// switching tabs, the outgoing tab's view (scroll position, selection)
// is captured with CaptureViewState() and the incoming tab's is
// restored via the SetDocument() overload that takes a HexGridViewState.
#pragma once

#include <windows.h>
#include <hexcore/HexEditorCore.h>
#include <cstdint>

// Implemented by whoever hosts this control (MainWindow) to react to
// edits and selection/navigation changes.
class IHexGridHost {
public:
    virtual ~IHexGridHost() = default;
    virtual void OnHexGridEdited() = 0;
    virtual void OnHexGridSelectionChanged(uint64_t offset) = 0;
};

// A tab's saved view state, captured/restored across tab switches.
struct HexGridViewState {
    uint64_t topRow = 0;
    uint64_t anchor = 0;
    uint64_t caret = 0;
    int nibbleIndex = 0;
    bool focusInAscii = false;
};

class HexGridControl {
public:
    static void RegisterWindowClass(HINSTANCE hInst);

    HWND Create(HWND parent, HINSTANCE hInst, int controlId);

    void SetHost(IHexGridHost* host) { m_host = host; }

    // Called when opening a document into this view. `initialState`,
    // if given, restores a previously captured scroll/selection (used
    // when switching back to an already-open tab); otherwise the view
    // resets to the top of the file with nothing selected.
    void SetDocument(HHEXDOC doc, const HexGridViewState* initialState = nullptr);

    HexGridViewState CaptureViewState() const;

    // Called after external state changes (undo/redo/save triggered by
    // the menu/toolbar) so the view repaints with fresh data.
    void Refresh();

    void GoToOffset(uint64_t offset);
    uint64_t GetSelectionOffset() const { return m_caret; }
    void GetSelectionRange(uint64_t& lo, uint64_t& hi) const;

    void SelectAll();
    void CopySelectionToClipboard();
    void PasteFromClipboard();
    // Deletes the selected range (or, if nothing is selected beyond the
    // caret itself, just the byte at the caret), shrinking the file.
    void DeleteSelection();

    HWND GetHwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnDestroy();
    void OnPaint();
    void OnSize();
    void OnVScroll(WPARAM wParam);
    void OnMouseWheel(WPARAM wParam);
    void OnLButtonDown(WPARAM keys, int x, int y);
    void OnMouseMove(WPARAM keys, int x, int y);
    void OnLButtonUp();
    void OnKeyDown(WPARAM key);
    void OnChar(WPARAM ch);

    void EnsureSelectionVisible();
    void UpdateScrollBar();
    void MoveCaretTo(uint64_t offset, bool extendSelection, bool resetNibble);
    void InvalidateAll();

    int ByteColumnX(int col) const;
    int AsciiColumnX(int col) const;
    int RowY(int visibleRowIndex) const;

    struct HitTestResult {
        bool valid = false;
        uint64_t offset = 0;
        bool inAsciiPane = false;
    };
    HitTestResult HitTest(int x, int y) const;
    // Like HitTest, but clamps to the nearest valid byte instead of
    // failing outside the grid - used while drag-selecting.
    uint64_t HitTestNearest(int x, int y) const;

    uint64_t GetFileSize() const;
    uint64_t GetTotalRows() const;
    uint64_t VisibleRowCount() const;

    HHEXDOC m_doc = nullptr;
    IHexGridHost* m_host = nullptr;
    HWND m_hwnd = nullptr;

    HFONT m_hexFont = nullptr;
    HFONT m_headerFont = nullptr;
    int m_charWidth = 8;
    int m_charHeight = 16;
    int m_rowHeight = 18;
    int m_headerHeight = 22;
    int m_topMargin = 4;
    int m_leftMargin = 8;

    uint64_t m_topRow = 0;
    uint64_t m_anchor = 0;     // where the current selection started
    uint64_t m_caret = 0;      // active cursor / edit point
    int m_nibbleIndex = 0;     // 0 = expecting high nibble, 1 = expecting low nibble
    bool m_focusInAscii = false;
    bool m_isDragging = false;

    static constexpr int kBytesPerRow = 16;
};
