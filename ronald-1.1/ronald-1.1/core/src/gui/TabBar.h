// TabBar - a Sublime Text-style horizontal strip of document tabs.
//
// Each tab shows a display name, a small dot when the document has
// unsaved changes, and a close ("x") button. Click a tab to activate
// it, click its close button (or middle-click the tab) to close it.
// When tabs overflow the available width, the mouse wheel scrolls the
// strip horizontally.
//
// This control owns no document state - it just renders labels and
// reports clicks through ITabBarHost. MainWindow is the source of
// truth for which documents are open.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

class ITabBarHost {
public:
    virtual ~ITabBarHost() = default;
    virtual void OnTabActivated(int index) = 0;
    virtual void OnTabCloseRequested(int index) = 0;
};

class TabBar {
public:
    static void RegisterWindowClass(HINSTANCE hInst);

    HWND Create(HWND parent, HINSTANCE hInst, int controlId);
    void SetHost(ITabBarHost* host) { m_host = host; }

    int AddTab(const std::wstring& displayName);
    void RemoveTab(int index);
    void SetActive(int index);
    void SetModified(int index, bool modified);
    void SetDisplayName(int index, const std::wstring& displayName);
    int GetActiveIndex() const { return m_activeIndex; }
    int GetTabCount() const { return static_cast<int>(m_tabs.size()); }

    HWND GetHwnd() const { return m_hwnd; }
    static constexpr int kHeight = 34;

private:
    struct TabItem {
        std::wstring displayName;
        bool modified = false;
    };

    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnDestroy();
    void OnPaint();
    void OnLButtonDown(int x, int y);
    void OnMButtonDown(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnMouseWheel(WPARAM wParam);
    void InvalidateAll();
    void EnsureTabVisible(int index);

    int TabWidth() const { return 180; }
    int TabLeft(int index) const;

    struct HitResult {
        int tabIndex = -1;
        bool onCloseButton = false;
    };
    HitResult HitTest(int x, int y) const;

    std::vector<TabItem> m_tabs;
    int m_activeIndex = -1;
    int m_hoverTab = -1;
    bool m_hoverClose = false;
    int m_scrollPx = 0;
    bool m_trackingLeave = false;

    HWND m_hwnd = nullptr;
    ITabBarHost* m_host = nullptr;
    HFONT m_font = nullptr;
    int m_charHeight = 14;
};
