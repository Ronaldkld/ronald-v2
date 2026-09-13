#include "TabBar.h"

#include <windowsx.h>
#include <algorithm>

namespace {

constexpr wchar_t kClassName[] = L"HexEditorTabBarClass";

constexpr COLORREF kBarBackground     = RGB(219, 219, 219);
constexpr COLORREF kActiveTabBg       = RGB(255, 255, 255);
constexpr COLORREF kInactiveTabBg     = RGB(228, 228, 228);
constexpr COLORREF kInactiveTabHoverBg = RGB(236, 236, 236);
constexpr COLORREF kActiveTabText     = RGB(20, 20, 20);
constexpr COLORREF kInactiveTabText   = RGB(90, 90, 90);
constexpr COLORREF kAccentBar         = RGB(0, 120, 215);
constexpr COLORREF kCloseHoverBg      = RGB(232, 17, 35);
constexpr COLORREF kCloseGlyph        = RGB(90, 90, 90);
constexpr COLORREF kCloseGlyphHover   = RGB(255, 255, 255);
constexpr COLORREF kModifiedDot       = RGB(90, 90, 90);
constexpr COLORREF kSeparator         = RGB(200, 200, 200);

constexpr int kClosePad = 10;
constexpr int kCloseSize = 16;

} // namespace

void TabBar::RegisterWindowClass(HINSTANCE hInst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &TabBar::WndProcStatic;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND TabBar::Create(HWND parent, HINSTANCE hInst, int controlId) {
    m_hwnd = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE,
                              0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
                              hInst, this);
    return m_hwnd;
}

LRESULT CALLBACK TabBar::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TabBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<TabBar*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<TabBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TabBar::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: OnCreate(hwnd); return 0;
        case WM_DESTROY: OnDestroy(); return 0;
        case WM_PAINT: OnPaint(); return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_MBUTTONDOWN: OnMButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); return 0;
        case WM_MOUSELEAVE: OnMouseLeave(); return 0;
        case WM_MOUSEWHEEL: OnMouseWheel(wParam); return 0;
        case WM_ERASEBKGND: return 1;
        default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void TabBar::OnCreate(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    m_font = CreateFontW(
        -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old = static_cast<HFONT>(SelectObject(hdc, m_font));
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    m_charHeight = tm.tmHeight;
    SelectObject(hdc, old);
    ReleaseDC(hwnd, hdc);
}

void TabBar::OnDestroy() {
    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
}

void TabBar::InvalidateAll() {
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

int TabBar::TabLeft(int index) const {
    return index * TabWidth();
}

int TabBar::AddTab(const std::wstring& displayName) {
    TabItem item;
    item.displayName = displayName;
    m_tabs.push_back(item);
    InvalidateAll();
    return static_cast<int>(m_tabs.size()) - 1;
}

void TabBar::RemoveTab(int index) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size())) return;
    m_tabs.erase(m_tabs.begin() + index);
    if (m_activeIndex == index) {
        m_activeIndex = -1;
    } else if (m_activeIndex > index) {
        m_activeIndex--;
    }
    if (m_hoverTab == index) m_hoverTab = -1;
    InvalidateAll();
}

void TabBar::SetActive(int index) {
    m_activeIndex = index;
    EnsureTabVisible(index);
    InvalidateAll();
}

void TabBar::SetModified(int index, bool modified) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size())) return;
    m_tabs[static_cast<size_t>(index)].modified = modified;
    InvalidateAll();
}

void TabBar::SetDisplayName(int index, const std::wstring& displayName) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size())) return;
    m_tabs[static_cast<size_t>(index)].displayName = displayName;
    InvalidateAll();
}

void TabBar::EnsureTabVisible(int index) {
    if (index < 0 || !m_hwnd) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    int clientWidth = rc.right - rc.left;

    int left = TabLeft(index);
    int right = left + TabWidth();
    if (left < m_scrollPx) {
        m_scrollPx = left;
    } else if (right > m_scrollPx + clientWidth) {
        m_scrollPx = right - clientWidth;
    }
    if (m_scrollPx < 0) m_scrollPx = 0;
}

TabBar::HitResult TabBar::HitTest(int x, int y) const {
    HitResult result;
    if (y < 0) return result;
    int logicalX = x + m_scrollPx;
    int index = logicalX / TabWidth();
    if (index < 0 || index >= static_cast<int>(m_tabs.size())) return result;

    int tabLeft = TabLeft(index) - m_scrollPx;
    int tabRight = tabLeft + TabWidth();
    result.tabIndex = index;

    int closeLeft = tabRight - kClosePad - kCloseSize;
    int closeRight = closeLeft + kCloseSize;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    int closeTop = (rc.bottom - kCloseSize) / 2;
    int closeBottom = closeTop + kCloseSize;
    if (x >= closeLeft && x < closeRight && y >= closeTop && y < closeBottom) {
        result.onCloseButton = true;
    }
    return result;
}

void TabBar::OnLButtonDown(int x, int y) {
    HitResult ht = HitTest(x, y);
    if (ht.tabIndex < 0) return;
    if (ht.onCloseButton) {
        if (m_host) m_host->OnTabCloseRequested(ht.tabIndex);
    } else if (m_host) {
        m_host->OnTabActivated(ht.tabIndex);
    }
}

void TabBar::OnMButtonDown(int x, int y) {
    HitResult ht = HitTest(x, y);
    if (ht.tabIndex >= 0 && m_host) m_host->OnTabCloseRequested(ht.tabIndex);
}

void TabBar::OnMouseMove(int x, int y) {
    if (!m_trackingLeave) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_hwnd;
        TrackMouseEvent(&tme);
        m_trackingLeave = true;
    }

    HitResult ht = HitTest(x, y);
    bool changed = (ht.tabIndex != m_hoverTab) || (ht.onCloseButton != m_hoverClose);
    m_hoverTab = ht.tabIndex;
    m_hoverClose = ht.onCloseButton;
    if (changed) InvalidateAll();
}

void TabBar::OnMouseLeave() {
    m_trackingLeave = false;
    if (m_hoverTab != -1 || m_hoverClose) {
        m_hoverTab = -1;
        m_hoverClose = false;
        InvalidateAll();
    }
}

void TabBar::OnMouseWheel(WPARAM wParam) {
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    int totalWidth = TabLeft(static_cast<int>(m_tabs.size()));
    int maxScroll = std::max(0, static_cast<int>(totalWidth - (rc.right - rc.left)));

    m_scrollPx = std::clamp(m_scrollPx - (delta / WHEEL_DELTA) * 60, 0, maxScroll);
    InvalidateAll();
}

void TabBar::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT rc{};
    GetClientRect(m_hwnd, &rc);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH barBrush = CreateSolidBrush(kBarBackground);
    FillRect(mem, &rc, barBrush);
    DeleteObject(barBrush);

    SetBkMode(mem, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(mem, m_font));

    for (size_t i = 0; i < m_tabs.size(); ++i) {
        int index = static_cast<int>(i);
        int left = TabLeft(index) - m_scrollPx;
        int right = left + TabWidth();
        if (right < 0 || left > rc.right) continue;

        bool active = (index == m_activeIndex);
        bool hover = (index == m_hoverTab);

        RECT tabRc{left, rc.top, right, rc.bottom};
        COLORREF bg = active ? kActiveTabBg : (hover ? kInactiveTabHoverBg : kInactiveTabBg);
        HBRUSH bgBrush = CreateSolidBrush(bg);
        FillRect(mem, &tabRc, bgBrush);
        DeleteObject(bgBrush);

        // Thin separator between tabs.
        HPEN sepPen = CreatePen(PS_SOLID, 1, kSeparator);
        HPEN oldPen = static_cast<HPEN>(SelectObject(mem, sepPen));
        MoveToEx(mem, right, rc.top, nullptr);
        LineTo(mem, right, rc.bottom);
        SelectObject(mem, oldPen);
        DeleteObject(sepPen);

        if (active) {
            RECT accentRc{left, rc.bottom - 3, right, rc.bottom};
            HBRUSH accentBrush = CreateSolidBrush(kAccentBar);
            FillRect(mem, &accentRc, accentBrush);
            DeleteObject(accentBrush);
        }

        int closeLeft = right - kClosePad - kCloseSize;
        int closeTop = (rc.bottom - kCloseSize) / 2;
        RECT closeRc{closeLeft, closeTop, closeLeft + kCloseSize, closeTop + kCloseSize};

        bool closeHover = hover && m_hoverClose;
        if (closeHover) {
            HBRUSH closeBg = CreateSolidBrush(kCloseHoverBg);
            HRGN rgn = CreateEllipticRgn(closeRc.left, closeRc.top, closeRc.right, closeRc.bottom);
            FillRgn(mem, rgn, closeBg);
            DeleteObject(rgn);
            DeleteObject(closeBg);
        }

        // Draw the close "x".
        HPEN xPen = CreatePen(PS_SOLID, 1, closeHover ? kCloseGlyphHover : kCloseGlyph);
        HPEN oldXPen = static_cast<HPEN>(SelectObject(mem, xPen));
        int pad = 5;
        MoveToEx(mem, closeRc.left + pad, closeRc.top + pad, nullptr);
        LineTo(mem, closeRc.right - pad, closeRc.bottom - pad);
        MoveToEx(mem, closeRc.right - pad, closeRc.top + pad, nullptr);
        LineTo(mem, closeRc.left + pad, closeRc.bottom - pad);
        SelectObject(mem, oldXPen);
        DeleteObject(xPen);

        int textRight = closeLeft - 4;
        const TabItem& item = m_tabs[i];
        if (item.modified) {
            int dotSize = 6;
            int dotLeft = textRight - dotSize;
            int dotTop = (rc.bottom - dotSize) / 2;
            HBRUSH dotBrush = CreateSolidBrush(kModifiedDot);
            HRGN dotRgn = CreateEllipticRgn(dotLeft, dotTop, dotLeft + dotSize, dotTop + dotSize);
            FillRgn(mem, dotRgn, dotBrush);
            DeleteObject(dotRgn);
            DeleteObject(dotBrush);
            textRight = dotLeft - 6;
        }

        RECT textRc{left + 12, rc.top, textRight, rc.bottom};
        SetTextColor(mem, active ? kActiveTabText : kInactiveTabText);
        DrawTextW(mem, item.displayName.c_str(), -1, &textRc,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SelectObject(mem, oldFont);
    BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);

    EndPaint(m_hwnd, &ps);
}
