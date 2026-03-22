#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace timmy::window {

#ifdef _WIN32
using NativeWindowHandle = HWND;
#else
using NativeWindowHandle = void*;
#endif

void apply_rounded_window_region(NativeWindowHandle window, int radius_px);
void clear_window_region(NativeWindowHandle window);
void make_window_topmost(NativeWindowHandle window);
void set_window_clickthrough(NativeWindowHandle window, bool enabled, bool use_colorkey);
void set_overlay_window_mode(NativeWindowHandle window);
void set_menu_window_mode(NativeWindowHandle window);
void extend_frame_into_client_area(NativeWindowHandle window);

} // namespace timmy::window
