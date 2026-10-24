#pragma once
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include "common/resource.h"
#include <algorithm>

// The frame belongs to our window; WebView2 receives only the content rectangle.
class WindowFrame {
    bool dark_ = false;
    HWND window_{};
    HWND buttons_[3]{};
    bool hot_[3]{};
    HFONT font_{};
    static constexpr int first_button = 310;
    int px(int value) const { return MulDiv(value, GetDpiForWindow(window_), 96); }
    static LRESULT CALLBACK button_proc(HWND button, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
        auto& frame = *reinterpret_cast<WindowFrame*>(data);
        const auto index = static_cast<int>(id);
        if (message == WM_MOUSEMOVE && !frame.hot_[index]) {
            frame.hot_[index] = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, button, 0};
            TrackMouseEvent(&track); InvalidateRect(button, nullptr, FALSE);
        } else if (message == WM_MOUSELEAVE) {
            frame.hot_[index] = false; InvalidateRect(button, nullptr, FALSE);
        } else if (message == WM_NCDESTROY) RemoveWindowSubclass(button, button_proc, id);
        return DefSubclassProc(button, message, wp, lp);
    }
public:
    ~WindowFrame() { if (font_) DeleteObject(font_); }
    COLORREF background() const { return dark_ ? RGB(33,28,40) : RGB(247,248,245); }
    bool dark() const { return dark_; }
    void set_dark(bool dark) {
        dark_=dark;
        const COLORREF border=dark_?RGB(73,60,86):RGB(205,218,209);
        DwmSetWindowAttribute(window_,DWMWA_BORDER_COLOR,&border,sizeof(border));
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
    }
    void attach(HWND window) { window_ = window; }
    void initialize(HINSTANCE instance, HWND window) {
        window_ = window;
        const wchar_t* labels[] = {L"最小化", L"最大化", L"关闭"};
        for (int i = 0; i < 3; ++i) {
            buttons_[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(first_button + i)), instance, nullptr);
            SetWindowSubclass(buttons_[i], button_proc, i, reinterpret_cast<DWORD_PTR>(this));
        }
        const COLORREF border = (dark_ ? RGB(73,60,86) : RGB(205,218,209));
        DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &border, sizeof(border));
        const auto rounded = DWMWCP_ROUND;
        DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &rounded, sizeof(rounded));
        // Force recalculation after removing the non-client frame.
        SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        resize();
    }
    RECT content() const {
        RECT bounds{}; GetClientRect(window_, &bounds);
        // A WebView child must not cover the resize band: mouse hit testing
        // otherwise goes to Chromium's child HWND instead of our frame.
        const auto border = IsZoomed(window_) ? 0 : px(6);
        bounds.left += border; bounds.top = px(42); bounds.right -= border; bounds.bottom -= border;
        bounds.right = std::max(bounds.left, bounds.right); bounds.bottom = std::max(bounds.top, bounds.bottom);
        return bounds;
    }
    void resize() {
        if (!window_) return;
        RECT bounds{}; GetClientRect(window_, &bounds);
        const auto border = IsZoomed(window_) ? 0 : px(6);
        for (int i = 0; i < 3; ++i)
            if (buttons_[i]) SetWindowPos(buttons_[i], nullptr, bounds.right - border - px(46) * (3 - i), border,
                px(46), px(42) - border * 2, SWP_NOZORDER | SWP_NOACTIVATE);
        if (buttons_[1]) SetWindowTextW(buttons_[1], IsZoomed(window_) ? L"还原" : L"最大化");
        if (font_) DeleteObject(font_);
        font_ = CreateFontW(-px(13), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        InvalidateRect(window_, nullptr, FALSE);
    }
    LRESULT hit_test(LPARAM position) const {
        POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)}; ScreenToClient(window_, &point);
        RECT bounds{}; GetClientRect(window_, &bounds);
        if (!IsZoomed(window_)) {
            const auto edge = px(6);
            const bool left = point.x < edge, right = point.x >= bounds.right - edge;
            const bool top = point.y < edge, bottom = point.y >= bounds.bottom - edge;
            if (top) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
            if (bottom) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
        }
        if (point.y < px(42) && point.x < bounds.right - px(46) * 3) return HTCAPTION;
        return HTCLIENT;
    }
    void minmax(MINMAXINFO& info) const {
        info.ptMinTrackSize = {px(680), px(520)};
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            info.ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top};
            info.ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
        }
    }
    void paint(HDC dc) const {
        RECT bounds{}; GetClientRect(window_, &bounds);
        RECT bar{0, 0, bounds.right, px(42)};
        if (!IsZoomed(window_)) {
            const auto border = px(6);
            const RECT bands[] = {{0, px(42), border, bounds.bottom}, {bounds.right - border, px(42), bounds.right, bounds.bottom},
                {0, bounds.bottom - border, bounds.right, bounds.bottom}};
            auto background = CreateSolidBrush(this->background());
            for (const auto& band : bands) FillRect(dc, &band, background);
            DeleteObject(background);
        }
        auto brush = CreateSolidBrush((dark_ ? RGB(41,33,48) : RGB(241,245,239))); FillRect(dc, &bar, brush); DeleteObject(brush);
        brush = CreateSolidBrush((dark_ ? RGB(73,60,86) : RGB(205,218,209))); FrameRect(dc, &bounds, brush); DeleteObject(brush);
        HPEN pen = CreatePen(PS_SOLID, px(1), (dark_ ? RGB(73,60,86) : RGB(226,232,227))); auto old_pen = SelectObject(dc, pen);
        MoveToEx(dc, 0, px(41), nullptr); LineTo(dc, bounds.right, px(41)); SelectObject(dc, old_pen); DeleteObject(pen);
        auto icon = reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_EUPHONY), IMAGE_ICON, px(24), px(24), LR_SHARED));
        DrawIconEx(dc, px(15), px(9), icon, px(24), px(24), 0, nullptr, DI_NORMAL);
        auto old_font = SelectObject(dc, font_); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, (dark_ ? RGB(238,231,245) : RGB(29,48,43)));
        RECT text{px(48), 0, bounds.right - px(150), px(42)};
        DrawTextW(dc, L"Euphony · 设置", -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(dc, old_font);
    }
    bool draw_button(const DRAWITEMSTRUCT& draw) const {
        const int index = static_cast<int>(draw.CtlID) - first_button;
        if (index < 0 || index > 2) return false;
        const bool active = hot_[index] || (draw.itemState & ODS_SELECTED);
        auto brush = CreateSolidBrush(active ? index == 2 ? RGB(180, 51, 46) : (dark_ ? RGB(68,49,79) : RGB(221,233,222)) : (dark_ ? RGB(41,33,48) : RGB(241,245,239)));
        FillRect(draw.hDC, &draw.rcItem, brush); DeleteObject(brush);
        const int x = (draw.rcItem.right - px(12)) / 2, y = (draw.rcItem.bottom - px(12)) / 2;
        auto pen = CreatePen(PS_SOLID, std::max(1, px(1)), active && index == 2 ? RGB(255, 255, 255) : (dark_ ? RGB(238,231,245) : RGB(29,48,43)));
        auto old_pen = SelectObject(draw.hDC, pen); auto old_brush = SelectObject(draw.hDC, GetStockObject(HOLLOW_BRUSH));
        if (index == 0) { MoveToEx(draw.hDC, x, y + px(6), nullptr); LineTo(draw.hDC, x + px(12), y + px(6)); }
        else if (index == 1) {
            if (IsZoomed(window_)) { Rectangle(draw.hDC, x + px(3), y, x + px(12), y + px(9)); Rectangle(draw.hDC, x, y + px(3), x + px(9), y + px(12)); }
            else Rectangle(draw.hDC, x, y, x + px(12), y + px(12));
        } else { MoveToEx(draw.hDC, x, y, nullptr); LineTo(draw.hDC, x + px(12), y + px(12)); MoveToEx(draw.hDC, x + px(12), y, nullptr); LineTo(draw.hDC, x, y + px(12)); }
        SelectObject(draw.hDC, old_brush); SelectObject(draw.hDC, old_pen); DeleteObject(pen);
        if (draw.itemState & ODS_FOCUS) { RECT focus = draw.rcItem; InflateRect(&focus, -px(6), -px(6)); DrawFocusRect(draw.hDC, &focus); }
        return true;
    }
    bool command(WPARAM command) const {
        const auto index = LOWORD(command) - first_button;
        if (HIWORD(command) != BN_CLICKED || index < 0 || index > 2) return false;
        const UINT actions[] = {SC_MINIMIZE, static_cast<UINT>(IsZoomed(window_) ? SC_RESTORE : SC_MAXIMIZE), SC_CLOSE};
        SendMessageW(window_, WM_SYSCOMMAND, actions[index], 0); return true;
    }
    bool controls_ready() const { return IsWindow(buttons_[0]) && IsWindow(buttons_[1]) && IsWindow(buttons_[2]); }
    void paint_controls(HDC dc) const {
        for (int i = 0; i < 3; ++i) {
            RECT bounds{}; GetWindowRect(buttons_[i], &bounds); MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
            const auto saved = SaveDC(dc); SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
            DRAWITEMSTRUCT draw{}; draw.CtlID = first_button + i; draw.hDC = dc;
            draw.rcItem = {0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
            draw_button(draw); RestoreDC(dc, saved);
        }
    }
};
