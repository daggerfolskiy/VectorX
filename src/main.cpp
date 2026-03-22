#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#endif

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include "app_config.h"
#include "debug/debug_log.h"
#include "features/aim.h"
#include "features/esp.h"
#include "ui/menu.h"
#include "ui/state.h"
#include "window/window_utils.h"

#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_pd3d_device = nullptr;
ID3D11DeviceContext* g_pd3d_device_context = nullptr;
IDXGISwapChain* g_p_swap_chain = nullptr;
ID3D11RenderTargetView* g_main_render_target_view = nullptr;
UINT g_pending_resize_width = 0;
UINT g_pending_resize_height = 0;
bool g_clickthrough_enabled = true;

bool is_process_foreground(const wchar_t* process_name) {
    if (process_name == nullptr || process_name[0] == L'\0') {
        return false;
    }
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || IsIconic(foreground)) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    if (pid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    wchar_t image_path[MAX_PATH]{};
    DWORD path_len = static_cast<DWORD>(_countof(image_path));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, image_path, &path_len);
    CloseHandle(process);
    if (ok == FALSE || path_len == 0) {
        return false;
    }
    const wchar_t* image_name = std::wcsrchr(image_path, L'\\');
    image_name = (image_name != nullptr) ? (image_name + 1) : image_path;
    return _wcsicmp(image_name, process_name) == 0;
}

bool is_process_foreground_cached(const wchar_t* process_name) {
    using clock = std::chrono::steady_clock;
    static std::wstring cached_name{};
    static bool cached_value = false;
    static clock::time_point next_refresh{};

    const auto now = clock::now();
    const std::wstring requested_name = process_name != nullptr ? process_name : L"";
    if (requested_name != cached_name || now >= next_refresh) {
        cached_name = requested_name;
        cached_value = is_process_foreground(process_name);
        next_refresh = now + std::chrono::milliseconds(120);
    }
    return cached_value;
}

ImFont* try_add_weapon_label_font(ImGuiIO& io, float size_px) {
    const std::array<const char*, 6> font_candidates = {
        "build/_deps/imgui-src/misc/fonts/Roboto-Medium.ttf",
        "_deps/imgui-src/misc/fonts/Roboto-Medium.ttf",
        "../_deps/imgui-src/misc/fonts/Roboto-Medium.ttf",
        "../build/_deps/imgui-src/misc/fonts/Roboto-Medium.ttf",
        "../../_deps/imgui-src/misc/fonts/Roboto-Medium.ttf",
        "../../build/_deps/imgui-src/misc/fonts/Roboto-Medium.ttf",
    };
    for (const char* path : font_candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        ImFont* font = io.Fonts->AddFontFromFileTTF(path, size_px);
        if (font != nullptr) {
            return font;
        }
    }
    return nullptr;
}

bool path_filename_contains(std::string_view text, std::string_view needle) {
    const auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                                [](char lhs, char rhs) {
                                    return std::tolower(static_cast<unsigned char>(lhs)) ==
                                           std::tolower(static_cast<unsigned char>(rhs));
                                });
    return it != text.end();
}

std::optional<std::filesystem::path> find_weapon_icon_font_path() {
    const std::array<const char*, 6> font_candidates = {
        "assets/fontl/obs_icons.ttf",
        "../assets/fontl/obs_icons.ttf",
        "../../assets/fontl/obs_icons.ttf",
        "../../../assets/fontl/obs_icons.ttf",
        "../../../../assets/fontl/obs_icons.ttf",
        "../../../../../assets/fontl/obs_icons.ttf",
    };

    for (const char* candidate : font_candidates) {
        const std::filesystem::path font_path = candidate;
        std::error_code ec;
        if (std::filesystem::exists(font_path, ec)) {
            return font_path;
        }
    }

    return std::nullopt;
}

ImFont* try_add_weapon_icon_font(ImGuiIO& io, float size_px) {
    const std::optional<std::filesystem::path> font_path = find_weapon_icon_font_path();
    if (!font_path.has_value()) {
        return nullptr;
    }

    const std::string font_path_string = font_path->string();
    static const ImWchar icon_ranges[] = {0xE000, 0xE300, 0};
    ImFontConfig config{};
    config.PixelSnapH = true;
    return io.Fonts->AddFontFromFileTTF(font_path_string.c_str(), size_px, &config, icon_ranges);
}

void format_local_date_time(char* out_date, size_t out_date_size, char* out_time, size_t out_time_size) {
    if (out_date == nullptr || out_time == nullptr || out_date_size == 0 || out_time_size == 0) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_tt = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    if (localtime_s(&local_tm, &now_tt) != 0) {
        std::snprintf(out_date, out_date_size, "--.--.----");
        std::snprintf(out_time, out_time_size, "--:--:--");
        return;
    }
    std::strftime(out_date, out_date_size, "%d.%m.%Y", &local_tm);
    std::strftime(out_time, out_time_size, "%H:%M:%S", &local_tm);
}

ImVec2 calc_watermark_size(const char* brand_text, const char* stamp_text) {
    const ImVec2 brand_size = ImGui::CalcTextSize(brand_text ? brand_text : "");
    const ImVec2 stamp_size = ImGui::CalcTextSize(stamp_text ? stamp_text : "");
    const float width = std::max(220.0f, std::max(brand_size.x, stamp_size.x) + 54.0f);
    const float height = 44.0f;
    return ImVec2(width, height);
}

void draw_watermark_card(ImDrawList* draw_list, float pos_x, float pos_y, const char* brand_text, const char* stamp_text,
                         bool drag_active) {
    if (draw_list == nullptr) {
        return;
    }
    const ImDrawListFlags old_flags = draw_list->Flags;
    draw_list->Flags |= ImDrawListFlags_AntiAliasedFill;
    draw_list->Flags |= ImDrawListFlags_AntiAliasedLines;

    const ImVec2 size = calc_watermark_size(brand_text, stamp_text);
    const ImVec2 min(pos_x, pos_y);
    const ImVec2 max(pos_x + size.x, pos_y + size.y);
    const float rounding = 9.0f;

    draw_list->AddRectFilled(ImVec2(min.x + 1.0f, min.y + 2.0f), ImVec2(max.x + 1.0f, max.y + 2.0f), IM_COL32(0, 0, 0, 130),
                             rounding);
    draw_list->AddRectFilled(min, max, IM_COL32(13, 20, 25, 232), rounding);
    draw_list->AddRectFilled(ImVec2(min.x + 1.0f, min.y + 1.0f), ImVec2(max.x - 1.0f, max.y - 1.0f), IM_COL32(18, 30, 36, 160),
                             rounding - 1.0f);
    draw_list->AddRect(min, max, drag_active ? IM_COL32(144, 236, 220, 255) : IM_COL32(186, 212, 222, 228), rounding, 0,
                       1.2f);
    // Keep accent line away from rounded corners so it never leaks outside the radius.
    const float accent_inset = rounding - 0.5f;
    draw_list->AddRectFilled(ImVec2(min.x + accent_inset, min.y + 1.2f), ImVec2(max.x - accent_inset, min.y + 4.1f),
                             IM_COL32(95, 236, 206, 238), 1.5f);

    draw_list->AddCircleFilled(ImVec2(min.x + 13.0f, min.y + 15.0f), 3.4f, IM_COL32(126, 241, 211, 255));
    draw_list->AddText(ImVec2(min.x + 22.0f, min.y + 7.0f), IM_COL32(240, 248, 255, 255), brand_text);
    draw_list->AddText(ImVec2(min.x + 22.0f, min.y + 23.0f), IM_COL32(168, 196, 208, 255), stamp_text);

    draw_list->Flags = old_flags;
}

struct VirtualScreenRect {
    int left = 0;
    int top = 0;
    int width = 1;
    int height = 1;
};

VirtualScreenRect get_virtual_screen_rect() {
    VirtualScreenRect rect{};
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    rect.height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    return rect;
}

void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    if (g_p_swap_chain && SUCCEEDED(g_p_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
        g_pd3d_device->CreateRenderTargetView(back_buffer, nullptr, &g_main_render_target_view);
        back_buffer->Release();
    }
}

void cleanup_render_target() {
    if (g_main_render_target_view) {
        g_main_render_target_view->Release();
        g_main_render_target_view = nullptr;
    }
}

bool create_device_d3d(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    swap_chain_desc.BufferCount = 2;
    swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferDesc.RefreshRate.Numerator = 60;
    swap_chain_desc.BufferDesc.RefreshRate.Denominator = 1;
    swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.OutputWindow = hwnd;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SampleDesc.Quality = 0;
    swap_chain_desc.Windowed = TRUE;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT create_device_flags = 0;
#ifdef _DEBUG
    create_device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL feature_level{};
    constexpr D3D_FEATURE_LEVEL feature_level_array[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    const HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        create_device_flags,
        feature_level_array,
        2,
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        &g_p_swap_chain,
        &g_pd3d_device,
        &feature_level,
        &g_pd3d_device_context);
    if (result == DXGI_ERROR_UNSUPPORTED) {
        const HRESULT warp_result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            create_device_flags,
            feature_level_array,
            2,
            D3D11_SDK_VERSION,
            &swap_chain_desc,
            &g_p_swap_chain,
            &g_pd3d_device,
            &feature_level,
            &g_pd3d_device_context);
        if (FAILED(warp_result)) {
            return false;
        }
    } else if (FAILED(result)) {
        return false;
    }

    create_render_target();
    return true;
}

void cleanup_device_d3d() {
    cleanup_render_target();
    if (g_p_swap_chain) {
        g_p_swap_chain->Release();
        g_p_swap_chain = nullptr;
    }
    if (g_pd3d_device_context) {
        g_pd3d_device_context->Release();
        g_pd3d_device_context = nullptr;
    }
    if (g_pd3d_device) {
        g_pd3d_device->Release();
        g_pd3d_device = nullptr;
    }
}

void queue_resize_from_client_rect(HWND hwnd) {
    RECT client_rect{};
    if (GetClientRect(hwnd, &client_rect)) {
        const LONG width = client_rect.right - client_rect.left;
        const LONG height = client_rect.bottom - client_rect.top;
        if (width > 0 && height > 0) {
            g_pending_resize_width = static_cast<UINT>(width);
            g_pending_resize_height = static_cast<UINT>(height);
        }
    }
}

void apply_pending_resize() {
    if (g_pending_resize_width == 0 || g_pending_resize_height == 0 || !g_p_swap_chain) {
        return;
    }
    cleanup_render_target();
    const HRESULT resize_result =
        g_p_swap_chain->ResizeBuffers(0, g_pending_resize_width, g_pending_resize_height, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(resize_result)) {
        create_render_target();
    }
    g_pending_resize_width = 0;
    g_pending_resize_height = 0;
}

void set_clickthrough(HWND hwnd, bool enabled) {
    g_clickthrough_enabled = enabled;
    timmy::window::set_window_clickthrough(hwnd, enabled, false);
}

LRESULT WINAPI window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    if (!g_clickthrough_enabled && ::ImGui_ImplWin32_WndProcHandler(hwnd, msg, w_param, l_param)) {
        return TRUE;
    }

    switch (msg) {
    case WM_SIZE:
        if (w_param != SIZE_MINIMIZED) {
            g_pending_resize_width = static_cast<UINT>(LOWORD(l_param));
            g_pending_resize_height = static_cast<UINT>(HIWORD(l_param));
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((w_param & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DWMCOMPOSITIONCHANGED:
        timmy::window::extend_frame_into_client_area(hwnd);
        return 0;
    case WM_MOUSEACTIVATE:
        if (g_clickthrough_enabled) {
            return MA_NOACTIVATE;
        }
        break;
    case WM_NCHITTEST:
        if (g_clickthrough_enabled) {
            return HTTRANSPARENT;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, w_param, l_param);
}

} // namespace
#endif

int run_overlay_app() {
    using namespace timmy;

#ifndef _WIN32
    std::fprintf(stderr, "This build currently supports Windows only.\n");
    return 1;
#else
    debug::init_console();
    debug::log("app start");
    ImGui_ImplWin32_EnableDpiAwareness();

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t kOverlayClassName[] = L"TimmyBlackOverlayWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = kOverlayClassName;
    if (!RegisterClassExW(&window_class)) {
        debug::log("RegisterClassExW failed");
        std::fprintf(stderr, "Failed to register window class\n");
        return 1;
    }

    const DWORD ex_style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE;
    const DWORD style = WS_POPUP;
    HWND window = CreateWindowExW(
        ex_style,
        kOverlayClassName,
        L"TIMMY BLACK Menu (C++)",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        (int)kWindowWidth,
        (int)kWindowHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        debug::log("CreateWindowExW failed");
        std::fprintf(stderr, "Failed to create overlay window\n");
        UnregisterClassW(kOverlayClassName, instance);
        return 1;
    }

    if (!create_device_d3d(window)) {
        debug::log("create_device_d3d failed");
        std::fprintf(stderr, "Failed to init D3D11\n");
        DestroyWindow(window);
        UnregisterClassW(kOverlayClassName, instance);
        return 1;
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ui::apply_style();
    if (!ui::try_add_font(io, 18.0f)) {
        io.Fonts->AddFontDefault();
    }
    ui::initialize_visual_assets(g_pd3d_device);
    features::set_esp_weapon_font(try_add_weapon_label_font(io, 21.0f));
    features::set_esp_weapon_icon_font(try_add_weapon_icon_font(io, 24.0f));

    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_pd3d_device, g_pd3d_device_context);

    ui::State state{};
    bool window_drag_active = false;
    bool prev_left_down = false;
    bool menu_visible = false;
    bool overlay_mode = true;
    bool prev_insert_down = false;
    float drag_offset_x = 0.0f;
    float drag_offset_y = 0.0f;
    bool watermark_drag_active = false;
    float watermark_drag_offset_x = 0.0f;
    float watermark_drag_offset_y = 0.0f;
    bool prev_watermark_left_down = false;
    bool menu_position_seeded = false;

    auto apply_runtime_window_mode = [&]() {
        if (menu_visible) {
            if (!overlay_mode) {
                window::set_overlay_window_mode(window);
                overlay_mode = true;
                queue_resize_from_client_rect(window);
            }
            set_clickthrough(window, false);
            window::make_window_topmost(window);
            window::extend_frame_into_client_area(window);
            SetForegroundWindow(window);
            SetFocus(window);
            window_drag_active = false;
            prev_left_down = false;
        } else {
            if (!overlay_mode) {
                window::set_overlay_window_mode(window);
                overlay_mode = true;
                queue_resize_from_client_rect(window);
            }
            set_clickthrough(window, true);
            window::make_window_topmost(window);
            window::extend_frame_into_client_area(window);
            window_drag_active = false;
            prev_left_down = false;
        }
    };

    window::set_overlay_window_mode(window);
    window::extend_frame_into_client_area(window);
    apply_runtime_window_mode();
    queue_resize_from_client_rect(window);

    bool done = false;
    while (!done) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        apply_pending_resize();
        if (!g_main_render_target_view) {
            Sleep(10);
            continue;
        }

        const bool insert_down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insert_down && !prev_insert_down) {
            menu_visible = !menu_visible;
            debug::log("Insert toggle: menu_visible=%d", menu_visible ? 1 : 0);
            if (menu_visible && !menu_position_seeded) {
                RECT client_rect{};
                GetClientRect(window, &client_rect);
                const float display_w = (float)std::max(0L, client_rect.right - client_rect.left);
                const float display_h = (float)std::max(0L, client_rect.bottom - client_rect.top);
                const float menu_w = (kWindowWidth - kRootMargin * 2.0f);
                const float menu_h = (kWindowHeight - kRootMargin * 2.0f);
                state.menu_x = std::max(0.0f, (display_w - menu_w) * 0.5f);
                state.menu_y = std::max(0.0f, (display_h - menu_h) * 0.5f);
                menu_position_seeded = true;
            }
            apply_runtime_window_mode();
        }
        prev_insert_down = insert_down;

        if (menu_visible) {
            RECT drag_client_rect{};
            GetClientRect(window, &drag_client_rect);
            const float drag_display_w = (float)std::max(0L, drag_client_rect.right - drag_client_rect.left);
            const float drag_display_h = (float)std::max(0L, drag_client_rect.bottom - drag_client_rect.top);
            const float menu_w = (kWindowWidth - kRootMargin * 2.0f);
            const float menu_h = (kWindowHeight - kRootMargin * 2.0f);
            const float menu_max_x = std::max(0.0f, drag_display_w - menu_w);
            const float menu_max_y = std::max(0.0f, drag_display_h - menu_h);
            state.menu_x = std::clamp(state.menu_x, 0.0f, menu_max_x);
            state.menu_y = std::clamp(state.menu_y, 0.0f, menu_max_y);

            POINT cursor_screen{};
            GetCursorPos(&cursor_screen);
            POINT cursor_client = cursor_screen;
            ScreenToClient(window, &cursor_client);
            const bool left_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

            const float close_x = state.menu_x + std::max(2.0f, menu_w - kTitlebarCloseW - kTitlebarClosePadX);
            const float close_y = state.menu_y + kTitlebarClosePadY;
            const bool in_titlebar = ((float)cursor_client.x >= state.menu_x &&
                                      (float)cursor_client.x <= (state.menu_x + menu_w) &&
                                      (float)cursor_client.y >= state.menu_y &&
                                      (float)cursor_client.y <= (state.menu_y + kTitlebarHeight));
            const bool in_close =
                ((float)cursor_client.x >= close_x && (float)cursor_client.x <= (close_x + kTitlebarCloseW) &&
                 (float)cursor_client.y >= close_y && (float)cursor_client.y <= (close_y + kTitlebarCloseH));

            if (!window_drag_active && left_down && !prev_left_down && in_titlebar && !in_close) {
                drag_offset_x = (float)cursor_client.x - state.menu_x;
                drag_offset_y = (float)cursor_client.y - state.menu_y;
                window_drag_active = true;
            }

            if (window_drag_active) {
                if (left_down) {
                    state.menu_x = std::clamp((float)cursor_client.x - drag_offset_x, 0.0f, menu_max_x);
                    state.menu_y = std::clamp((float)cursor_client.y - drag_offset_y, 0.0f, menu_max_y);
                } else {
                    window_drag_active = false;
                }
            }
            prev_left_down = left_down;
        } else {
            window_drag_active = false;
            prev_left_down = false;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RECT client_rect{};
        GetClientRect(window, &client_rect);
        const int display_w = std::max(0L, client_rect.right - client_rect.left);
        const int display_h = std::max(0L, client_rect.bottom - client_rect.top);
        const VirtualScreenRect virtual_rect = get_virtual_screen_rect();
        RECT window_rect{};
        if (!GetWindowRect(window, &window_rect)) {
            window_rect.left = 0;
            window_rect.top = 0;
            window_rect.right = display_w;
            window_rect.bottom = display_h;
        }
        const bool can_draw_game_overlay = overlay_mode && is_process_foreground_cached(L"cs2.exe");
        const bool can_draw_watermark = state.watermark_enabled && (can_draw_game_overlay || menu_visible);
        const char* kWatermarkBrand = "TIMMYBLACK";
        char date_text[32] = {};
        char time_text[32] = {};
        char watermark_stamp[64] = {};
        const bool left_down_global = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool watermark_just_pressed = left_down_global && !prev_watermark_left_down;
        if (can_draw_watermark) {
            format_local_date_time(date_text, sizeof(date_text), time_text, sizeof(time_text));
            std::snprintf(watermark_stamp, sizeof(watermark_stamp), "%s  %s", date_text, time_text);
            const ImVec2 watermark_size = calc_watermark_size(kWatermarkBrand, watermark_stamp);
            const float max_x = std::max(0.0f, (float)virtual_rect.width - watermark_size.x);
            const float max_y = std::max(0.0f, (float)virtual_rect.height - watermark_size.y);
            state.watermark_x = std::clamp(state.watermark_x, 0.0f, max_x);
            state.watermark_y = std::clamp(state.watermark_y, 0.0f, max_y);

            if (menu_visible) {
                POINT cursor_screen{};
                GetCursorPos(&cursor_screen);
                const float watermark_screen_x = (float)virtual_rect.left + state.watermark_x;
                const float watermark_screen_y = (float)virtual_rect.top + state.watermark_y;
                const bool in_watermark = (float)cursor_screen.x >= watermark_screen_x &&
                                          (float)cursor_screen.x <= (watermark_screen_x + watermark_size.x) &&
                                          (float)cursor_screen.y >= watermark_screen_y &&
                                          (float)cursor_screen.y <= (watermark_screen_y + watermark_size.y);
                if (!watermark_drag_active && watermark_just_pressed && in_watermark) {
                    watermark_drag_active = true;
                    watermark_drag_offset_x = (float)cursor_screen.x - watermark_screen_x;
                    watermark_drag_offset_y = (float)cursor_screen.y - watermark_screen_y;
                }
                if (watermark_drag_active) {
                    if (left_down_global) {
                        const float next_x = (float)cursor_screen.x - (float)virtual_rect.left - watermark_drag_offset_x;
                        const float next_y = (float)cursor_screen.y - (float)virtual_rect.top - watermark_drag_offset_y;
                        state.watermark_x = std::clamp(next_x, 0.0f, max_x);
                        state.watermark_y = std::clamp(next_y, 0.0f, max_y);
                    } else {
                        watermark_drag_active = false;
                    }
                }
            } else {
                watermark_drag_active = false;
            }
        } else {
            watermark_drag_active = false;
        }

        features::draw_esp_overlay(ImGui::GetBackgroundDrawList(), state, display_w, display_h, can_draw_game_overlay);
        features::draw_aim_overlay(ImGui::GetBackgroundDrawList(), state, display_w, display_h, can_draw_game_overlay);
        if (can_draw_game_overlay && state.show_fps) {
            float avg_fps = 0.0f;
            const bool has_game_fps = features::query_game_avg_fps(avg_fps);
            char fps_text[64] = {};
            if (has_game_fps) {
                std::snprintf(fps_text, sizeof(fps_text), "AVG FPS: %.0f", avg_fps);
            } else {
                std::snprintf(fps_text, sizeof(fps_text), "AVG FPS: --");
            }
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const ImVec2 fps_pos(15.0f, 14.0f);
            fg->AddText(ImVec2(fps_pos.x + 1.0f, fps_pos.y + 1.0f), IM_COL32(0, 0, 0, 220), fps_text);
            fg->AddText(fps_pos, IM_COL32(228, 255, 228, 255), fps_text);
        }
        if (can_draw_watermark) {
            const float draw_x = ((float)virtual_rect.left + state.watermark_x) - (float)window_rect.left;
            const float draw_y = ((float)virtual_rect.top + state.watermark_y) - (float)window_rect.top;
            draw_watermark_card(ImGui::GetForegroundDrawList(), draw_x, draw_y, kWatermarkBrand, watermark_stamp,
                                watermark_drag_active && menu_visible);
        }
        prev_watermark_left_down = left_down_global;

        bool close_requested = false;
        if (menu_visible) {
            close_requested = ui::draw_menu(state);
        }
        if (close_requested) {
            menu_visible = false;
            apply_runtime_window_mode();
        }

        ImGui::Render();
        const float clear_color[4] = {
            overlay_mode ? 0.0f : 16.0f / 255.0f,
            overlay_mode ? 0.0f : 18.0f / 255.0f,
            overlay_mode ? 0.0f : 18.0f / 255.0f,
            overlay_mode ? 0.0f : 1.0f,
        };
        g_pd3d_device_context->OMSetRenderTargets(1, &g_main_render_target_view, nullptr);
        g_pd3d_device_context->ClearRenderTargetView(g_main_render_target_view, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const bool prefer_low_latency_present = window_drag_active || watermark_drag_active;
        const UINT sync_interval = (menu_visible && !prefer_low_latency_present) ? 1U : 0U;
        const HRESULT present_result = g_p_swap_chain->Present(sync_interval, 0U);
        if (present_result == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ui::shutdown_visual_assets();
    ImGui::DestroyContext();
    cleanup_device_d3d();
    DestroyWindow(window);
    UnregisterClassW(kOverlayClassName, instance);
    debug::log("app shutdown");
    debug::shutdown_console();
    return 0;
#endif
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return run_overlay_app();
}
#endif

int main() {
    return run_overlay_app();
}
