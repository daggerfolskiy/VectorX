#include "debug/debug_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace timmy::debug {

namespace {

std::mutex g_log_mutex;
bool g_console_initialized = false;

void print_line(const char* text) {
#ifdef _WIN32
    if (g_console_initialized) {
        std::fputs(text, stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    std::string out = std::string(text) + "\n";
    OutputDebugStringA(out.c_str());
#else
    std::fputs(text, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
#endif
}

} // namespace

void init_console() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
#ifdef _WIN32
    if (g_console_initialized) {
        return;
    }

    if (GetConsoleWindow() == nullptr) {
        if (AllocConsole() == FALSE) {
            return;
        }
    }

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleA("timmy_menu debug");
    g_console_initialized = true;
    print_line("[debug] console initialized");
#endif
}

void shutdown_console() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
#ifdef _WIN32
    if (!g_console_initialized) {
        return;
    }
    print_line("[debug] console shutdown");
    g_console_initialized = false;
    FreeConsole();
#endif
}

void log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (fmt == nullptr) {
        return;
    }

    char msg[2048];
    msg[0] = '\0';
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[sizeof(msg) - 1] = '\0';

#ifdef _WIN32
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[2304];
    std::snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    print_line(line);
#else
    print_line(msg);
#endif
}

} // namespace timmy::debug
