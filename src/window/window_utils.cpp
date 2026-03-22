#include "window/window_utils.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

#include "app_config.h"

namespace timmy::window {

void apply_rounded_window_region(NativeWindowHandle window, int radius_px) {
#ifdef _WIN32
    if (!window) {
        return;
    }
    RECT client_rect{};
    if (!GetClientRect(window, &client_rect)) {
        return;
    }
    const int width = client_rect.right - client_rect.left;
    const int height = client_rect.bottom - client_rect.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    const int diameter = std::max(2, radius_px * 2);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (region != nullptr) {
        SetWindowRgn(window, region, TRUE);
    }
#else
    (void)window;
    (void)radius_px;
#endif
}

void clear_window_region(NativeWindowHandle window) {
#ifdef _WIN32
    if (!window) {
        return;
    }
    SetWindowRgn(window, nullptr, TRUE);
#else
    (void)window;
#endif
}

void make_window_topmost(NativeWindowHandle window) {
#ifdef _WIN32
    if (!window) {
        return;
    }
    const LONG_PTR ex_style = GetWindowLongPtr(window, GWL_EXSTYLE);
    if ((ex_style & WS_EX_TOPMOST) != 0) {
        return;
    }
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
#else
    (void)window;
#endif
}

void set_window_clickthrough(NativeWindowHandle window, bool enabled, bool use_colorkey) {
#ifdef _WIN32
    if (!window) {
        return;
    }
    LONG_PTR ex_style = GetWindowLongPtr(window, GWL_EXSTYLE);
    ex_style |= WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (enabled) {
        ex_style |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    } else {
        ex_style &= ~WS_EX_TRANSPARENT;
        ex_style &= ~WS_EX_NOACTIVATE;
    }
    SetWindowLongPtr(window, GWL_EXSTYLE, ex_style);
    if (use_colorkey) {
        SetLayeredWindowAttributes(window, RGB(0, 0, 0), 255, LWA_ALPHA | LWA_COLORKEY);
    } else {
        SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
    }
    const UINT pos_flags =
        SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW;
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, pos_flags);
#else
    (void)window;
    (void)enabled;
    (void)use_colorkey;
#endif
}

void set_overlay_window_mode(NativeWindowHandle window) {
#ifdef _WIN32
    if (!window) {
        return;
    }
    const int screen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int screen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int screen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int screen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(window, HWND_TOPMOST, screen_x, screen_y, screen_w, screen_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    clear_window_region(window);
#else
    (void)window;
#endif
}

void set_menu_window_mode(NativeWindowHandle window) {
#ifdef _WIN32
    if (!window) {
        return;
    }

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    RECT monitor_rect{};
    if (GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor_info)) {
        monitor_rect = monitor_info.rcWork;
    } else {
        monitor_rect.left = 0;
        monitor_rect.top = 0;
        monitor_rect.right = GetSystemMetrics(SM_CXSCREEN);
        monitor_rect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const int screen_w = monitor_rect.right - monitor_rect.left;
    const int screen_h = monitor_rect.bottom - monitor_rect.top;
    const int menu_w = (int)kWindowWidth;
    const int menu_h = (int)kWindowHeight;
    const int menu_x = monitor_rect.left + std::max(0, (screen_w - menu_w) / 2);
    const int menu_y = monitor_rect.top + std::max(0, (screen_h - menu_h) / 2);
    SetWindowPos(window, HWND_TOPMOST, menu_x, menu_y, menu_w, menu_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    apply_rounded_window_region(window, kWindowCornerRadius);
#else
    (void)window;
#endif
}

void extend_frame_into_client_area(NativeWindowHandle window) {
#ifdef _WIN32
    if (!window) {
        return;
    }
    BOOL composition_enabled = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&composition_enabled)) && composition_enabled) {
        const MARGINS margins{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(window, &margins);
    }
#else
    (void)window;
#endif
}

} // namespace timmy::window
