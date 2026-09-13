#include "HexGridControl.h"

#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kClassName[] = L"HexGridControlClass";

// Windows 11-ish palette. Kept as plain constants (no theme system yet)
// since this is a single-purpose control; see README for extension notes.
constexpr COLORREF kColorBackground   = RGB(255, 255, 255);
constexpr COLORREF kColorHeaderBg     = RGB(243, 243, 243);
constexpr COLORREF kColorHeaderText   = RGB(90, 90, 90);
constexpr COLORREF kColorOffsetText   = RGB(120, 120, 120);
constexpr COLORREF kColorNormalText   = RGB(32, 32, 32);
constexpr COLORREF kColorModifiedText = RGB(196, 43, 28);
constexpr COLORREF kColorSelectionBg  = RGB(0, 120, 215);
constexpr COLORREF kColorSelectionTx  = RGB(255, 255, 255);
constexpr COLORREF kColorAltRow       = RGB(248, 248, 248);
constexpr COLORREF kColorSeparator    = RGB(225, 225, 225);

bool IsPrintableAscii(uint8_t b) { return b >= 0x20 && b < 0x7F; }

int HexDigitValue(WPARAM ch) {
    if (ch >= '0' && ch <= '9') return static_cast<int>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<int>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return static_cast<int>(ch - 'A' + 10);
    return -1;
}

} // namespace

void HexGridControl::RegisterWindowClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &HexGridControl::WndProcStatic;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hbrBackground = nullptr; // we paint everything ourselves
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND HexGridControl::Create(HWND parent, HINSTANCE hInst, int controlId) {
    m_hwnd = CreateWindowExW(
        WS_EX_CLIENTEDGE, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), hInst, this);
    return m_hwnd;
}

LRESULT CALLBACK HexGridControl::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HexGridControl* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<HexGridControl*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<HexGridControl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT HexGridControl::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: OnCreate(hwnd); return 0;
        case WM_DESTROY: OnDestroy(); return 0;
        case WM_PAINT: OnPaint(); return 0;
        case WM_SIZE: OnSize(); return 0;
        case WM_VSCROLL: OnVScroll(wParam); return 0;
        case WM_MOUSEWHEEL: OnMouseWheel(wParam); return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_MOUSEMOVE: OnMouseMove(wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_LBUTTONUP: OnLButtonUp(); return 0;
        case WM_KEYDOWN: OnKeyDown(wParam); return 0;
        case WM_CHAR: OnChar(wParam); return 0;
        case WM_SETFOCUS: InvalidateAll(); return 0;
        case WM_KILLFOCUS: InvalidateAll(); return 0;
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_ERASEBKGND: return 1; // avoid flicker, we paint the whole client area ourselves
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void HexGridControl::OnCreate(HWND hwnd) {
    HDC hdc = GetDC(hwnd);

    m_hexFont = CreateFontW(
        -MulDiv(10, GetDeviceCaps(hdc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    m_headerFont = CreateFontW(
        -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");

    HFONT old = static_cast<HFONT>(SelectObject(hdc, m_hexFont));
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    m_charHeight = tm.tmHeight;
    m_rowHeight = tm.tmHeight + 4;

    SIZE sz{};
    GetTextExtentPoint32W(hdc, L"0", 1, &sz);
    m_charWidth = sz.cx;

    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
}

void HexGridControl::OnDestroy() {
    if (m_hexFont) { DeleteObject(m_hexFont); m_hexFont = nullptr; }
    if (m_headerFont) { DeleteObject(m_headerFont); m_headerFont = nullptr; }
}

uint64_t HexGridControl::GetFileSize() const {
    return m_doc ? HexCore_GetSize(m_doc) : 0;
}

uint64_t HexGridControl::GetTotalRows() const {
    uint64_t size = GetFileSize();
    return size == 0 ? 0 : (size + kBytesPerRow - 1) / kBytesPerRow;
}

uint64_t HexGridControl::VisibleRowCount() const {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    int avail = (rc.bottom - rc.top) - m_headerHeight - m_topMargin;
    if (avail <= 0 || m_rowHeight <= 0) return 0;
    return static_cast<uint64_t>(avail / m_rowHeight);
}

int HexGridControl::ByteColumnX(int col) const {
    int offsetColumnWidth = 10 * m_charWidth;
    int hexStart = m_leftMargin + offsetColumnWidth;
    int extraGroupGap = (col >= 8) ? m_charWidth : 0;
    return hexStart + col * 3 * m_charWidth + extraGroupGap;
}

int HexGridControl::AsciiColumnX(int col) const {
    int hexBlockEnd = ByteColumnX(kBytesPerRow - 1) + 3 * m_charWidth + 2 * m_charWidth;
    return hexBlockEnd + col * m_charWidth;
}

int HexGridControl::RowY(int visibleRowIndex) const {
    return m_headerHeight + m_topMargin + visibleRowIndex * m_rowHeight;
}

void HexGridControl::SetDocument(HHEXDOC doc, const HexGridViewState* initialState) {
    m_doc = doc;
    if (initialState) {
        m_topRow = initialState->topRow;
        m_anchor = initialState->anchor;
        m_caret = initialState->caret;
        m_nibbleIndex = initialState->nibbleIndex;
        m_focusInAscii = initialState->focusInAscii;
    } else {
        m_topRow = 0;
        m_anchor = 0;
        m_caret = 0;
        m_nibbleIndex = 0;
        m_focusInAscii = false;
    }
    UpdateScrollBar();
    InvalidateAll();
}

HexGridViewState HexGridControl::CaptureViewState() const {
    HexGridViewState state;
    state.topRow = m_topRow;
    state.anchor = m_anchor;
    state.caret = m_caret;
    state.nibbleIndex = m_nibbleIndex;
    state.focusInAscii = m_focusInAscii;
    return state;
}

void HexGridControl::Refresh() {
    UpdateScrollBar();
    InvalidateAll();
}

void HexGridControl::GoToOffset(uint64_t offset) {
    MoveCaretTo(offset, /*extendSelection=*/false, /*resetNibble=*/true);
    EnsureSelectionVisible();
    UpdateScrollBar();
    InvalidateAll();
}

void HexGridControl::GetSelectionRange(uint64_t& lo, uint64_t& hi) const {
    lo = std::min(m_anchor, m_caret);
    hi = std::max(m_anchor, m_caret);
}

void HexGridControl::InvalidateAll() {
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HexGridControl::UpdateScrollBar() {
    uint64_t totalRows = GetTotalRows();
    uint64_t visible = VisibleRowCount();

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = totalRows > 0 ? static_cast<int>(totalRows - 1) : 0;
    si.nPage = static_cast<UINT>(std::max<uint64_t>(1, visible));
    si.nPos = static_cast<int>(std::min(m_topRow, totalRows));
    SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);
}

void HexGridControl::OnSize() {
    UpdateScrollBar();
    InvalidateAll();
}

void HexGridControl::OnVScroll(WPARAM wParam) {
    uint64_t totalRows = GetTotalRows();
    uint64_t visible = VisibleRowCount();
    if (totalRows == 0) return;

    int64_t newTop = static_cast<int64_t>(m_topRow);
    int action = LOWORD(wParam);

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_TRACKPOS;
    GetScrollInfo(m_hwnd, SB_VERT, &si);

    switch (action) {
        case SB_LINEUP: newTop -= 1; break;
        case SB_LINEDOWN: newTop += 1; break;
        case SB_PAGEUP: newTop -= static_cast<int64_t>(std::max<uint64_t>(1, visible)); break;
        case SB_PAGEDOWN: newTop += static_cast<int64_t>(std::max<uint64_t>(1, visible)); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: newTop = si.nTrackPos; break;
        case SB_TOP: newTop = 0; break;
        case SB_BOTTOM: newTop = static_cast<int64_t>(totalRows - 1); break;
        default: return;
    }

    int64_t maxTop = static_cast<int64_t>(totalRows) - 1;
    if (maxTop < 0) maxTop = 0;
    newTop = std::clamp<int64_t>(newTop, 0, maxTop);

    m_topRow = static_cast<uint64_t>(newTop);
    UpdateScrollBar();
    InvalidateAll();
}

void HexGridControl::OnMouseWheel(WPARAM wParam) {
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    int64_t rows = -3 * (delta / WHEEL_DELTA);
    uint64_t totalRows = GetTotalRows();
    int64_t maxTop = static_cast<int64_t>(totalRows) - 1;
    if (maxTop < 0) maxTop = 0;

    int64_t newTop = std::clamp<int64_t>(static_cast<int64_t>(m_topRow) + rows, 0, maxTop);
    m_topRow = static_cast<uint64_t>(newTop);
    UpdateScrollBar();
    InvalidateAll();
}

HexGridControl::HitTestResult HexGridControl::HitTest(int x, int y) const {
    HitTestResult result;
    if (!m_doc) return result;
    if (y < m_headerHeight + m_topMargin) return result;

    int rowIndex = (y - m_headerHeight - m_topMargin) / m_rowHeight;
    uint64_t row = m_topRow + static_cast<uint64_t>(rowIndex);
    uint64_t rowStart = row * kBytesPerRow;
    uint64_t size = GetFileSize();
    if (rowStart >= size) return result;

    for (int col = 0; col < kBytesPerRow; ++col) {
        int hx = ByteColumnX(col);
        if (x >= hx && x < hx + 2 * m_charWidth) {
            uint64_t off = rowStart + static_cast<uint64_t>(col);
            if (off < size) { result.valid = true; result.offset = off; result.inAsciiPane = false; return result; }
        }
        int ax = AsciiColumnX(col);
        if (x >= ax && x < ax + m_charWidth) {
            uint64_t off = rowStart + static_cast<uint64_t>(col);
            if (off < size) { result.valid = true; result.offset = off; result.inAsciiPane = true; return result; }
        }
    }
    return result;
}

uint64_t HexGridControl::HitTestNearest(int x, int y) const {
    uint64_t size = GetFileSize();
    if (size == 0) return 0;

    int clampedY = std::max(y, m_headerHeight + m_topMargin);
    int rowIndex = (clampedY - m_headerHeight - m_topMargin) / m_rowHeight;
    uint64_t row = m_topRow + static_cast<uint64_t>(std::max(0, rowIndex));
    uint64_t rowStart = std::min(row * kBytesPerRow, ((size - 1) / kBytesPerRow) * kBytesPerRow);

    int col = (x < ByteColumnX(0)) ? 0 : (kBytesPerRow - 1);
    for (int c = 0; c < kBytesPerRow; ++c) {
        if (x < ByteColumnX(c) + 3 * m_charWidth) { col = c; break; }
    }
    uint64_t off = rowStart + static_cast<uint64_t>(col);
    return std::min(off, size - 1);
}

void HexGridControl::OnLButtonDown(WPARAM keys, int x, int y) {
    SetFocus(m_hwnd);
    HitTestResult ht = HitTest(x, y);
    if (!ht.valid) return;

    m_focusInAscii = ht.inAsciiPane;
    bool extend = (keys & MK_SHIFT) != 0;
    MoveCaretTo(ht.offset, extend, /*resetNibble=*/true);

    m_isDragging = true;
    SetCapture(m_hwnd);
    InvalidateAll();
}

void HexGridControl::OnMouseMove(WPARAM keys, int x, int y) {
    if (!m_isDragging || !(keys & MK_LBUTTON) || !m_doc) return;
    uint64_t off = HitTestNearest(x, y);
    MoveCaretTo(off, /*extendSelection=*/true, /*resetNibble=*/true);
    EnsureSelectionVisible();
    UpdateScrollBar();
    InvalidateAll();
}

void HexGridControl::OnLButtonUp() {
    if (m_isDragging) {
        m_isDragging = false;
        ReleaseCapture();
    }
}

void HexGridControl::EnsureSelectionVisible() {
    uint64_t row = m_caret / kBytesPerRow;
    uint64_t visible = VisibleRowCount();
    if (visible == 0) return;

    if (row < m_topRow) {
        m_topRow = row;
    } else if (row >= m_topRow + visible) {
        m_topRow = row - visible + 1;
    }
}

void HexGridControl::MoveCaretTo(uint64_t offset, bool extendSelection, bool resetNibble) {
    uint64_t size = GetFileSize();
    if (size == 0) { m_anchor = m_caret = 0; return; }
    if (offset >= size) offset = size - 1;

    m_caret = offset;
    if (!extendSelection) m_anchor = offset;
    if (resetNibble) m_nibbleIndex = 0;
    if (m_host) m_host->OnHexGridSelectionChanged(m_caret);
}

void HexGridControl::SelectAll() {
    uint64_t size = GetFileSize();
    if (size == 0) return;
    m_anchor = 0;
    m_caret = size - 1;
    m_nibbleIndex = 0;
    if (m_host) m_host->OnHexGridSelectionChanged(m_caret);
    InvalidateAll();
}

void HexGridControl::CopySelectionToClipboard() {
    if (!m_doc) return;
    uint64_t lo, hi;
    GetSelectionRange(lo, hi);
    uint64_t count = hi - lo + 1;

    std::vector<uint8_t> bytes(static_cast<size_t>(count));
    size_t got = 0;
    HexCore_ReadBytes(m_doc, lo, bytes.data(), bytes.size(), &got);
    if (got == 0) return;

    std::wstring text;
    text.reserve(got * 3);
    wchar_t pair[4];
    for (size_t i = 0; i < got; ++i) {
        swprintf(pair, 4, L"%02X", bytes[i]);
        text += pair;
        if (i + 1 < got) text += L' ';
    }

    if (!OpenClipboard(m_hwnd)) return;
    EmptyClipboard();
    size_t bufBytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, bufBytes);
    if (hGlobal) {
        void* ptr = GlobalLock(hGlobal);
        if (ptr) {
            memcpy(ptr, text.c_str(), bufBytes);
            GlobalUnlock(hGlobal);
            SetClipboardData(CF_UNICODETEXT, hGlobal);
        } else {
            GlobalFree(hGlobal);
        }
    }
    CloseClipboard();
}

void HexGridControl::PasteFromClipboard() {
    if (!m_doc) return;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (!OpenClipboard(m_hwnd)) return;

    std::wstring wide;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        wchar_t* wtext = static_cast<wchar_t*>(GlobalLock(hData));
        if (wtext) wide = wtext;
        GlobalUnlock(hData);
    }
    CloseClipboard();
    if (wide.empty()) return;

    int narrowLen = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (narrowLen <= 0) return;
    std::vector<char> narrow(static_cast<size_t>(narrowLen));
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, narrow.data(), narrowLen, nullptr, nullptr);

    std::vector<uint8_t> bytes(narrow.size() / 2 + 1);
    size_t len = 0;
    if (!HexCore_ParseHexString(narrow.data(), bytes.data(), bytes.size(), &len) || len == 0) {
        MessageBeep(MB_ICONERROR);
        return;
    }

    // Standard text-editor paste semantics: if a range is actually
    // selected (not just a single-byte caret position), replace it -
    // delete it, then insert the pasted bytes in its place. Otherwise,
    // insert at the caret without touching anything. Either way the
    // file grows or shrinks to fit exactly what was pasted, just like
    // pasting text in Sublime Text.
    uint64_t lo, hi;
    GetSelectionRange(lo, hi);
    bool hasRealSelection = (m_anchor != m_caret);
    uint64_t insertAt = lo;

    if (hasRealSelection) {
        HexCoreStatus delStatus = HexCore_DeleteRange(m_doc, lo, hi - lo + 1);
        if (delStatus != HEXCORE_OK) return;
    } else {
        insertAt = m_caret;
    }

    HexCoreStatus insStatus = HexCore_InsertBytes(m_doc, insertAt, bytes.data(), len);
    if (insStatus != HEXCORE_OK) {
        MessageBeep(MB_ICONERROR);
        return;
    }
    if (m_host) m_host->OnHexGridEdited();

    m_anchor = insertAt;
    m_caret = insertAt + (len > 0 ? len - 1 : 0);
    m_nibbleIndex = 0;
    EnsureSelectionVisible();
    UpdateScrollBar();
    InvalidateAll();
}

void HexGridControl::DeleteSelection() {
    if (!m_doc || GetFileSize() == 0) return;

    uint64_t lo, hi;
    GetSelectionRange(lo, hi);
    if (HexCore_DeleteRange(m_doc, lo, hi - lo + 1) != HEXCORE_OK) return;

    uint64_t newSize = GetFileSize();
    m_anchor = m_caret = (newSize == 0) ? 0 : std::min(lo, newSize - 1);
    m_nibbleIndex = 0;
    if (m_host) {
        m_host->OnHexGridEdited();
        m_host->OnHexGridSelectionChanged(m_caret);
    }
    EnsureSelectionVisible();
    UpdateScrollBar();
    InvalidateAll();
}

void HexGridControl::OnKeyDown(WPARAM key) {
    if (!m_doc || GetFileSize() == 0) return;

    if (key == VK_DELETE) {
        DeleteSelection();
        return;
    }

    uint64_t size = GetFileSize();
    uint64_t sel = m_caret;
    uint64_t visible = std::max<uint64_t>(1, VisibleRowCount());
    bool moved = false;

    switch (key) {
        case VK_LEFT:
            if (sel > 0) { sel -= 1; moved = true; }
            break;
        case VK_RIGHT:
            if (sel + 1 < size) { sel += 1; moved = true; }
            break;
        case VK_UP:
            if (sel >= kBytesPerRow) { sel -= kBytesPerRow; moved = true; }
            break;
        case VK_DOWN:
            if (sel + kBytesPerRow < size) { sel += kBytesPerRow; moved = true; }
            else if ((sel / kBytesPerRow) != (size - 1) / kBytesPerRow) { sel = size - 1; moved = true; }
            break;
        case VK_PRIOR: {
            uint64_t delta = visible * kBytesPerRow;
            sel = (sel > delta) ? sel - delta : 0;
            moved = true;
            break;
        }
        case VK_NEXT: {
            uint64_t delta = visible * kBytesPerRow;
            sel = (sel + delta < size) ? sel + delta : size - 1;
            moved = true;
            break;
        }
        case VK_HOME:
            sel = (GetKeyState(VK_CONTROL) & 0x8000) ? 0 : (sel - (sel % kBytesPerRow));
            moved = true;
            break;
        case VK_END:
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                sel = size - 1;
            } else {
                uint64_t rowStart = sel - (sel % kBytesPerRow);
                uint64_t rowEnd = std::min(rowStart + kBytesPerRow - 1, size - 1);
                sel = rowEnd;
            }
            moved = true;
            break;
        default:
            break;
    }

    if (moved) {
        bool extend = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        MoveCaretTo(sel, extend, /*resetNibble=*/true);
        EnsureSelectionVisible();
        UpdateScrollBar();
        InvalidateAll();
    }
}

void HexGridControl::OnChar(WPARAM ch) {
    if (!m_doc || GetFileSize() == 0) return;
    if (ch < 0x20 && ch != VK_BACK) return; // ignore stray control chars (Ctrl+A/C/V etc.)

    if (ch == VK_BACK) {
        if (m_anchor != m_caret) {
            // A range is selected: Backspace deletes it, like a text editor.
            DeleteSelection();
        } else if (m_caret > 0) {
            // No selection: delete the byte immediately before the caret.
            uint64_t target = m_caret - 1;
            if (HexCore_DeleteRange(m_doc, target, 1) == HEXCORE_OK) {
                m_anchor = m_caret = target;
                m_nibbleIndex = 0;
                if (m_host) {
                    m_host->OnHexGridEdited();
                    m_host->OnHexGridSelectionChanged(m_caret);
                }
                EnsureSelectionVisible();
                UpdateScrollBar();
            }
        }
        InvalidateAll();
        return;
    }
    if (ch == VK_TAB) return; // let normal focus navigation happen elsewhere

    // Typing a hex/ASCII character while a multi-byte range is selected
    // overwrites starting from the beginning of that range (the format
    // doesn't grow/shrink from plain typing - use Delete/Backspace/Paste
    // for that).
    if (m_anchor != m_caret) {
        MoveCaretTo(std::min(m_anchor, m_caret), false, true);
    }

    if (!m_focusInAscii) {
        int digit = HexDigitValue(ch);
        if (digit < 0) return;

        uint8_t current = 0;
        size_t got = 0;
        HexCore_ReadBytes(m_doc, m_caret, &current, 1, &got);

        uint8_t newValue;
        if (m_nibbleIndex == 0) {
            newValue = static_cast<uint8_t>((digit << 4) | (current & 0x0F));
            m_nibbleIndex = 1;
        } else {
            newValue = static_cast<uint8_t>((current & 0xF0) | digit);
            m_nibbleIndex = 0;
        }
        HexCore_WriteByte(m_doc, m_caret, newValue);
        if (m_host) m_host->OnHexGridEdited();

        if (m_nibbleIndex == 0) {
            uint64_t size = GetFileSize();
            if (m_caret + 1 < size) MoveCaretTo(m_caret + 1, false, true);
        }
    } else {
        if (ch < 0x20 || ch > 0x7E) return; // printable ASCII only
        HexCore_WriteByte(m_doc, m_caret, static_cast<uint8_t>(ch));
        if (m_host) m_host->OnHexGridEdited();
        uint64_t size = GetFileSize();
        if (m_caret + 1 < size) MoveCaretTo(m_caret + 1, false, true);
    }

    EnsureSelectionVisible();
    UpdateScrollBar();
    InvalidateAll();
}

void HexGridControl::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT rc{};
    GetClientRect(m_hwnd, &rc);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH bgBrush = CreateSolidBrush(kColorBackground);
    FillRect(mem, &rc, bgBrush);
    DeleteObject(bgBrush);

    RECT headerRc{rc.left, rc.top, rc.right, rc.top + m_headerHeight};
    HBRUSH headerBrush = CreateSolidBrush(kColorHeaderBg);
    FillRect(mem, &headerRc, headerBrush);
    DeleteObject(headerBrush);

    SetBkMode(mem, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(mem, m_headerFont));
    SetTextColor(mem, kColorHeaderText);

    wchar_t buf[32];
    swprintf(buf, 32, L"Offset");
    TextOutW(mem, m_leftMargin, (m_headerHeight - m_charHeight) / 2, buf, static_cast<int>(wcslen(buf)));
    for (int col = 0; col < kBytesPerRow; ++col) {
        swprintf(buf, 32, L"%02X", col);
        TextOutW(mem, ByteColumnX(col), (m_headerHeight - m_charHeight) / 2, buf, 2);
    }
    swprintf(buf, 32, L"ASCII");
    TextOutW(mem, AsciiColumnX(0), (m_headerHeight - m_charHeight) / 2, buf, static_cast<int>(wcslen(buf)));

    SelectObject(mem, m_hexFont);

    if (m_doc) {
        uint64_t size = GetFileSize();
        uint64_t visible = VisibleRowCount() + 1;
        uint8_t rowBytes[kBytesPerRow];
        uint64_t selLo, selHi;
        GetSelectionRange(selLo, selHi);

        for (uint64_t r = 0; r < visible; ++r) {
            uint64_t row = m_topRow + r;
            uint64_t rowStart = row * kBytesPerRow;
            if (rowStart >= size) break;

            int y = RowY(static_cast<int>(r));
            RECT rowRc{rc.left, y, rc.right, y + m_rowHeight};
            if (r % 2 == 1) {
                HBRUSH altBrush = CreateSolidBrush(kColorAltRow);
                FillRect(mem, &rowRc, altBrush);
                DeleteObject(altBrush);
            }

            size_t got = 0;
            HexCore_ReadBytes(m_doc, rowStart, rowBytes, kBytesPerRow, &got);

            SetTextColor(mem, kColorOffsetText);
            swprintf(buf, 32, L"%08llX", static_cast<unsigned long long>(rowStart));
            TextOutW(mem, m_leftMargin, y + (m_rowHeight - m_charHeight) / 2, buf, 8);

            for (size_t c = 0; c < got; ++c) {
                uint64_t off = rowStart + c;
                uint8_t b = rowBytes[c];
                bool modified = HexCore_IsByteModified(m_doc, off) != 0;
                bool selected = (off >= selLo && off <= selHi);

                int hx = ByteColumnX(static_cast<int>(c));
                int ax = AsciiColumnX(static_cast<int>(c));

                if (selected) {
                    RECT selHex{hx - 1, y, hx + 2 * m_charWidth + 1, y + m_rowHeight};
                    RECT selAscii{ax - 1, y, ax + m_charWidth + 1, y + m_rowHeight};
                    HBRUSH selBrush = CreateSolidBrush(kColorSelectionBg);
                    FillRect(mem, &selHex, selBrush);
                    FillRect(mem, &selAscii, selBrush);
                    DeleteObject(selBrush);
                    SetTextColor(mem, kColorSelectionTx);
                } else {
                    SetTextColor(mem, modified ? kColorModifiedText : kColorNormalText);
                }

                swprintf(buf, 32, L"%02X", b);
                TextOutW(mem, hx, y + (m_rowHeight - m_charHeight) / 2, buf, 2);

                wchar_t asciiCh = IsPrintableAscii(b) ? static_cast<wchar_t>(b) : L'.';
                TextOutW(mem, ax, y + (m_rowHeight - m_charHeight) / 2, &asciiCh, 1);

                if (!selected) SetTextColor(mem, modified ? kColorModifiedText : kColorNormalText);
            }
        }

        // Vertical separator between the two 8-byte hex groups, and
        // between the hex block and the ASCII block, for readability.
        HPEN pen = CreatePen(PS_SOLID, 1, kColorSeparator);
        HPEN oldPen = static_cast<HPEN>(SelectObject(mem, pen));
        int sepX1 = ByteColumnX(8) - m_charWidth / 2;
        int sepX2 = AsciiColumnX(0) - m_charWidth;
        MoveToEx(mem, sepX1, m_headerHeight, nullptr); LineTo(mem, sepX1, rc.bottom);
        MoveToEx(mem, sepX2, m_headerHeight, nullptr); LineTo(mem, sepX2, rc.bottom);
        SelectObject(mem, oldPen);
        DeleteObject(pen);
    } else {
        SetTextColor(mem, kColorOffsetText);
        const wchar_t* msg = L"Open a file to start editing (File > Open, or Ctrl+O).";
        TextOutW(mem, m_leftMargin, m_headerHeight + m_topMargin, msg, static_cast<int>(wcslen(msg)));
    }

    SelectObject(mem, oldFont);
    BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(m_hwnd, &ps);
}
