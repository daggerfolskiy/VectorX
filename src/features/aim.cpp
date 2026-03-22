#include "features/aim.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "debug/debug_log.h"
#include "imgui.h"
#include "ui/state.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <TlHelp32.h>
#include <wininet.h>
#endif

namespace timmy::features {

namespace {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Target {
    float x = 0.0f;
    float y = 0.0f;
};

constexpr float kFovMin = 15.0f;
constexpr float kFovMax = 320.0f;
constexpr float kLockRadius = 85.0f;
constexpr float kHeadOffsetRatio = 0.12f;
constexpr float kChestOffsetRatio = 0.30f;
constexpr float kStomachOffsetRatio = 0.43f;
constexpr float kBodyOffsetRatio = 0.38f;
constexpr float kStopRadius = 1.4f;
constexpr float kTargetSwitchDist = 55.0f;
constexpr float kTriggerRadius = 9.0f;
constexpr int kHeadBoneId = 6;
constexpr int kLegBoneId = 28;

constexpr int kAk47RawX[] = {0, 0, 0, 0, 0, 40, 40, -40, -90, -30, -20, -20, -20, 0, 80, 30,
                             50, 50, 30, 20, -20, -10, 0, 10, 0, -40, -90, -70, -30, -10, 0};
constexpr int kAk47RawY[] = {0, 40, 40, 80, 80, 80, 80, 20, -10, 20, 0, 0, -10, 20, 30, -10,
                             20, 0, -10, -10, 10, 10, 10, 0, 10, -10, 0, -50, 10, -10, 0};

constexpr int kM4A4RawX[] = {0, 0, 0, 0, 0, -10, 10, 20, 20, 30, -40, -40, -40, -40, -40, -50,
                             0, 30, 30, 20, 60, 30, 40, 20, 10, 0, 0, 10, 10, 0, 0};
constexpr int kM4A4RawY[] = {0, 10, 30, 40, 40, 60, 60, 60, 30, 20, 20, 20, 0, -10, 0, 10,
                             10, 0, 0, 0, 10, 0, 0, 10, 0, 10, 10, 0, 0, 0, 0};

constexpr int kM4A1sRawX[] = {0, 0, 0, 0, 0, -10, 0, 30, 10, 30, -10, -40, -20,
                              -30, -20, -20, -30, -30, 10, -10, 0, 20, 40, 60, 10, 0};
constexpr int kM4A1sRawY[] = {0, 10, 10, 30, 30, 40, 40, 50, 10, 10, 10, 20, 0,
                              -10, 0, 0, -10, 0, 10, 0, 10, 0, 0, 20, 0, 0};

// Adapted from public Galil recoil-control snippets and then tuned to match the broader CS2 Galil spray shape.
constexpr int kGalilRawX[] = {0, 4, -2, 6, -4, 4, -6, -4, 14, 8, 18, -4, -14, -25, -19, -22,
                              1, 8, -9, 6, -14, -24, -13, 15, 17, -6, -20, -3, 0, 0, 0};
constexpr int kGalilRawY[] = {0, 4, 5, 10, 14, 18, 21, 24, 14, 12, 10, 10, 5, 3, 0, -3,
                              3, 3, 1, 8, -3, -14, -1, -5, -2, 3, -2, -1, 0, 0, 0};

struct SpreadPatternConfig {
    std::uint16_t weapon_index = 0;
    const int* raw_x = nullptr;
    const int* raw_y = nullptr;
    int size = 0;
    int smoothness = 1;
    int step_delay_us = 0;
    int settle_delay_us = 0;
};

constexpr SpreadPatternConfig kAk47SpreadPattern{7, kAk47RawX, kAk47RawY, (int)std::size(kAk47RawX), 1, 100000, 0};
constexpr SpreadPatternConfig kGalilSpreadPattern{13, kGalilRawX, kGalilRawY, (int)std::size(kGalilRawX), 2, 22000, 44000};
constexpr SpreadPatternConfig kM4A4SpreadPattern{16, kM4A4RawX, kM4A4RawY, (int)std::size(kM4A4RawX), 1, 90000, 0};
constexpr SpreadPatternConfig kM4A1sSpreadPattern{60, kM4A1sRawX, kM4A1sRawY, (int)std::size(kM4A1sRawX), 1, 90000, 0};
constexpr double kSpreadReadGraceSeconds = 0.20;

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

bool should_log_throttled(const char* key, double now_s, double interval_s) {
    static std::unordered_map<std::string, double> next_time{};
    const std::string k = key ? key : "";
    double& next = next_time[k];
    if (now_s >= next) {
        next = now_s + interval_s;
        return true;
    }
    return false;
}

float random_uniform(float lo, float hi) {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng);
}

bool world_to_screen(const std::array<float, 16>& mtx, float x, float y, float z, float w, float h, Vec2& out) {
    const float sw = (mtx[12] * x) + (mtx[13] * y) + (mtx[14] * z) + mtx[15];
    if (sw <= 0.01f) {
        return false;
    }
    const float sx = (mtx[0] * x) + (mtx[1] * y) + (mtx[2] * z) + mtx[3];
    const float sy = (mtx[4] * x) + (mtx[5] * y) + (mtx[6] * z) + mtx[7];
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    out.x = cx + (cx * sx / sw);
    out.y = cy - (cy * sy / sw);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

const SpreadPatternConfig* find_spread_pattern(std::uint16_t weapon_index) {
    switch (weapon_index) {
    case 7:
        return &kAk47SpreadPattern;
    case 13:
        return &kGalilSpreadPattern;
    case 16:
        return &kM4A4SpreadPattern;
    case 60:
        return &kM4A1sSpreadPattern;
    default:
        return nullptr;
    }
}

float normalize_pattern_value(int raw_value, float sensitivity) {
    const float safe_sensitivity = std::max(0.01f, sensitivity);
    return std::round((static_cast<float>(raw_value) / safe_sensitivity) * 10.0f) / 10.0f;
}

class SmoothAim {
public:
    void reset() {
        carry_x_ = 0.0f;
        carry_y_ = 0.0f;
        reaction_ready_at_ = 0.0;
        burst_end_at_ = 0.0;
        micro_pause_until_ = 0.0;
        persistent_bias_x_ = 0.0f;
        persistent_bias_y_ = 0.0f;
        burst_error_x_ = 0.0f;
        burst_error_y_ = 0.0f;
        drift_phase_x_ = 0.0f;
        drift_phase_y_ = 0.0f;
        drift_freq_x_ = 0.0f;
        drift_freq_y_ = 0.0f;
        drift_amp_x_ = 0.0f;
        drift_amp_y_ = 0.0f;
        tracking_scale_ = 1.0f;
    }

    void mark_target_switch(double now_s, float dx, float dy) {
        reaction_ready_at_ = now_s + (double)random_uniform(0.055f, 0.145f);
        burst_end_at_ = reaction_ready_at_ + (double)random_uniform(0.12f, 0.28f);

        if (random_uniform(0.0f, 1.0f) < 0.20f) {
            micro_pause_until_ = reaction_ready_at_ + (double)random_uniform(0.012f, 0.040f);
        } else {
            micro_pause_until_ = 0.0;
        }

        persistent_bias_x_ = random_uniform(-1.8f, 1.8f);
        persistent_bias_y_ = random_uniform(-3.2f, 2.4f);

        const float dir_x = dx >= 0.0f ? 1.0f : -1.0f;
        const float dir_y = dy >= 0.0f ? 1.0f : -1.0f;
        if (random_uniform(0.0f, 1.0f) < 0.58f) {
            burst_error_x_ = dir_x * random_uniform(1.4f, 7.5f) + random_uniform(-2.0f, 2.0f);
            burst_error_y_ = dir_y * random_uniform(0.8f, 5.0f) + random_uniform(-2.0f, 2.0f);
        } else {
            burst_error_x_ = random_uniform(-3.0f, 3.0f);
            burst_error_y_ = random_uniform(-2.5f, 2.5f);
        }

        drift_phase_x_ = random_uniform(0.0f, 6.2831853f);
        drift_phase_y_ = random_uniform(0.0f, 6.2831853f);
        drift_freq_x_ = random_uniform(4.4f, 7.8f);
        drift_freq_y_ = random_uniform(3.8f, 6.8f);
        drift_amp_x_ = random_uniform(0.12f, 0.95f);
        drift_amp_y_ = random_uniform(0.10f, 0.80f);
        tracking_scale_ = random_uniform(0.92f, 1.08f);
    }

    bool is_ready(double now_s) const {
        return now_s >= reaction_ready_at_;
    }

    std::pair<int, int> step(float dx, float dy, float dt, double now_s) {
        if (micro_pause_until_ > 0.0 && now_s < micro_pause_until_) {
            return {0, 0};
        }

        float burst_decay = 0.0f;
        if (burst_end_at_ > reaction_ready_at_) {
            const float burst_t =
                clampf((float)((now_s - reaction_ready_at_) / (burst_end_at_ - reaction_ready_at_)), 0.0f, 1.0f);
            burst_decay = 1.0f - burst_t;
        }
        const float drift_x = std::sin((float)now_s * drift_freq_x_ + drift_phase_x_) * drift_amp_x_;
        const float drift_y = std::sin((float)now_s * drift_freq_y_ + drift_phase_y_) * drift_amp_y_;
        dx += persistent_bias_x_ + burst_error_x_ * burst_decay + drift_x;
        dy += persistent_bias_y_ + burst_error_y_ * burst_decay + drift_y;

        const float distance = std::hypot(dx, dy);
        if (distance <= kStopRadius) {
            return {0, 0};
        }
        const float distance_ratio = clampf(distance / 180.0f, 0.0f, 1.0f);
        const float time_gain = 0.98f + 0.06f * std::sin((float)now_s * 3.7f + drift_phase_x_);
        const float gain =
            (0.075f + 0.235f * std::pow(distance_ratio, 0.68f)) * clampf(dt * 60.0f, 0.35f, 2.4f) * tracking_scale_ *
            time_gain;
        float move_x = dx * gain;
        float move_y = dy * (gain * (0.97f + 0.05f * std::sin((float)now_s * 2.9f + drift_phase_y_)));
        const float max_step = (2.3f + 11.4f * distance_ratio) * clampf(tracking_scale_, 0.92f, 1.06f);
        const float step_len = std::hypot(move_x, move_y);
        if (step_len > max_step && step_len > 0.0f) {
            const float s = max_step / step_len;
            move_x *= s;
            move_y *= s;
        }
        const float out_fx = move_x + carry_x_;
        const float out_fy = move_y + carry_y_;
        int out_x = (int)std::lround(out_fx);
        int out_y = (int)std::lround(out_fy);
        carry_x_ = out_fx - (float)out_x;
        carry_y_ = out_fy - (float)out_y;
        return {out_x, out_y};
    }

private:
    float carry_x_ = 0.0f;
    float carry_y_ = 0.0f;
    double reaction_ready_at_ = 0.0;
    double burst_end_at_ = 0.0;
    double micro_pause_until_ = 0.0;
    float persistent_bias_x_ = 0.0f;
    float persistent_bias_y_ = 0.0f;
    float burst_error_x_ = 0.0f;
    float burst_error_y_ = 0.0f;
    float drift_phase_x_ = 0.0f;
    float drift_phase_y_ = 0.0f;
    float drift_freq_x_ = 0.0f;
    float drift_freq_y_ = 0.0f;
    float drift_amp_x_ = 0.0f;
    float drift_amp_y_ = 0.0f;
    float tracking_scale_ = 1.0f;
};

#ifdef _WIN32
struct Offsets {
    uintptr_t dwEntityList = 38445272;
    uintptr_t dwLocalPlayerPawn = 33966816;
    uintptr_t dwLocalPlayerController = 0;
    uintptr_t dwSensitivity = 0;
    uintptr_t dwViewMatrix = 36744672;
    uintptr_t m_iTeamNum = 1011;
    uintptr_t m_lifeState = 860;
    uintptr_t m_iHealth = 852;
    uintptr_t m_pGameSceneNode = 824;
    uintptr_t m_modelState = 352;
    uintptr_t m_hPlayerPawn = 2316;
    uintptr_t m_iIDEntIndex = 0;
    uintptr_t m_iCrosshairID = 0;
    uintptr_t m_entitySpottedState = 0;
    uintptr_t m_bSpottedByMask = 0;
    uintptr_t m_aimPunchAngle = 0;
    uintptr_t m_iShotsFired = 0;
    uintptr_t m_pClippingWeapon = 0;
    uintptr_t m_pWeaponServices = 0;
    uintptr_t m_hActiveWeapon = 0;
    uintptr_t m_AttributeManager = 0;
    uintptr_t m_Item = 0;
    uintptr_t m_iItemDefinitionIndex = 0;
    uintptr_t dwSensitivity_sensitivity = 0x40;
    uintptr_t m_bGunGameImmunity = 0;
};

class Runtime {
public:
    ~Runtime() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    bool ensure() {
        if (!offsets_loaded_) {
            load_extra_offsets();
            offsets_loaded_ = true;
        }
        if (handle_ != nullptr && client_base_ != 0) {
            DWORD code = 0;
            if (GetExitCodeProcess(handle_, &code) != FALSE && code == STILL_ACTIVE) {
                return true;
            }
            CloseHandle(handle_);
            handle_ = nullptr;
            client_base_ = 0;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_attach_attempt_) {
            return false;
        }
        next_attach_attempt_ = now + std::chrono::milliseconds(700);

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        DWORD pid = 0;
        if (Process32FirstW(snapshot, &pe) != FALSE) {
            do {
                if (_wcsicmp(pe.szExeFile, L"cs2.exe") == 0) {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &pe) != FALSE);
        }
        CloseHandle(snapshot);
        if (pid == 0) {
            debug::log("aim runtime: cs2.exe not found");
            return false;
        }

        HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (ms == INVALID_HANDLE_VALUE) {
            return false;
        }
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        uintptr_t client = 0;
        if (Module32FirstW(ms, &me) != FALSE) {
            do {
                if (_wcsicmp(me.szModule, L"client.dll") == 0) {
                    client = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                    break;
                }
            } while (Module32NextW(ms, &me) != FALSE);
        }
        CloseHandle(ms);
        if (client == 0) {
            debug::log("aim runtime: client.dll not found");
            return false;
        }

        HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h == nullptr) {
            debug::log("aim runtime: OpenProcess failed");
            return false;
        }
        handle_ = h;
        client_base_ = client;
        debug::log("aim runtime attached: pid=%lu client=0x%llx", (unsigned long)pid,
                   (unsigned long long)client_base_);
        return true;
    }

    template <typename T>
    bool read(uintptr_t addr, T& out) const {
        if (handle_ == nullptr || addr == 0) {
            return false;
        }
        SIZE_T br = 0;
        return ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(addr), &out, sizeof(T), &br) != FALSE && br == sizeof(T);
    }

    uintptr_t client() const {
        return client_base_;
    }

    const Offsets& o() const {
        return offsets_;
    }

    bool read_weapon_index(uintptr_t entity_list, uintptr_t pawn, std::uint16_t& out_index) const {
        out_index = 0;
        if (offsets_.m_AttributeManager == 0 || offsets_.m_Item == 0 || offsets_.m_iItemDefinitionIndex == 0) {
            return false;
        }

        uintptr_t weapon_entity = 0;
        if (!resolve_weapon_entity(entity_list, pawn, weapon_entity) || weapon_entity == 0) {
            return false;
        }

        return read(weapon_entity + offsets_.m_AttributeManager + offsets_.m_Item + offsets_.m_iItemDefinitionIndex, out_index) &&
               out_index != 0;
    }

    bool resolve_entity_index(uintptr_t entity_list, int entity_index, uintptr_t& out_entity) const {
        out_entity = 0;
        if (entity_list == 0 || entity_index <= 0) {
            return false;
        }

        uintptr_t entity_page = 0;
        if (!read(entity_list + 0x8 * ((entity_index & 0x7FFF) >> 9) + 0x10, entity_page) || entity_page == 0) {
            return false;
        }

        if (read(entity_page + 0x78 * (entity_index & 0x1FF), out_entity) && out_entity != 0) {
            return true;
        }

        return read(entity_page + 0x70 * (entity_index & 0x1FF), out_entity) && out_entity != 0;
    }

    bool read_current_sensitivity(float& out_sensitivity) const {
        out_sensitivity = 1.0f;
        if (client_base_ == 0 || offsets_.dwSensitivity == 0 || offsets_.dwSensitivity_sensitivity == 0) {
            return false;
        }

        float direct_value = 0.0f;
        if (read(client_base_ + offsets_.dwSensitivity + offsets_.dwSensitivity_sensitivity, direct_value) &&
            std::isfinite(direct_value) && direct_value > 0.001f && direct_value < 100.0f) {
            out_sensitivity = direct_value;
            return true;
        }

        uintptr_t sensitivity_ptr = 0;
        if (read(client_base_ + offsets_.dwSensitivity, sensitivity_ptr) && sensitivity_ptr != 0 &&
            read(sensitivity_ptr + offsets_.dwSensitivity_sensitivity, direct_value) &&
            std::isfinite(direct_value) && direct_value > 0.001f && direct_value < 100.0f) {
            out_sensitivity = direct_value;
            return true;
        }

        return false;
    }

private:
    static std::optional<std::string> read_text_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.good()) {
            return std::nullopt;
        }
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    static std::optional<std::string> download_text_from_url(const char* url) {
        HINTERNET internet = InternetOpenA("timmy_aim_offsets", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (internet == nullptr) {
            return std::nullopt;
        }
        HINTERNET request =
            InternetOpenUrlA(internet, url, nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (request == nullptr) {
            InternetCloseHandle(internet);
            return std::nullopt;
        }
        std::string body{};
        std::array<char, 8192> buf{};
        DWORD bytes_read = 0;
        while (InternetReadFile(request, buf.data(), (DWORD)buf.size(), &bytes_read) != FALSE && bytes_read > 0) {
            body.append(buf.data(), buf.data() + bytes_read);
            bytes_read = 0;
        }
        InternetCloseHandle(request);
        InternetCloseHandle(internet);
        if (body.empty()) {
            return std::nullopt;
        }
        return body;
    }

    static size_t skip_ws(const std::string& text, size_t pos, size_t to) {
        while (pos < to && std::isspace((unsigned char)text[pos]) != 0) {
            ++pos;
        }
        return pos;
    }

    static bool find_object_range(const std::string& text, const char* object_name, size_t& out_begin, size_t& out_end,
                                  size_t from = 0, size_t to = std::string::npos) {
        if (to == std::string::npos || to > text.size()) {
            to = text.size();
        }
        const std::string needle = "\"" + std::string(object_name) + "\"";
        size_t search = from;
        while (search < to) {
            const size_t key_pos = text.find(needle, search);
            if (key_pos == std::string::npos || key_pos >= to) {
                return false;
            }
            size_t value_pos = skip_ws(text, key_pos + needle.size(), to);
            if (value_pos >= to || text[value_pos] != ':') {
                search = key_pos + needle.size();
                continue;
            }
            value_pos = skip_ws(text, value_pos + 1, to);
            if (value_pos >= to || text[value_pos] != '{') {
                search = key_pos + needle.size();
                continue;
            }

            int depth = 0;
            bool in_string = false;
            bool escaped = false;
            for (size_t i = value_pos; i < to; ++i) {
                const char c = text[i];
                if (in_string) {
                    if (escaped) {
                        escaped = false;
                    } else if (c == '\\') {
                        escaped = true;
                    } else if (c == '"') {
                        in_string = false;
                    }
                    continue;
                }
                if (c == '"') {
                    in_string = true;
                    continue;
                }
                if (c == '{') {
                    ++depth;
                } else if (c == '}') {
                    --depth;
                    if (depth == 0) {
                        out_begin = value_pos;
                        out_end = i + 1;
                        return true;
                    }
                }
            }
            return false;
        }
        return false;
    }

    static bool extract_uint_value(const std::string& text, const char* key, uintptr_t& out, size_t from = 0,
                                   size_t to = std::string::npos) {
        if (to == std::string::npos || to > text.size()) {
            to = text.size();
        }
        const std::string needle = "\"" + std::string(key) + "\"";
        size_t search = from;
        while (search < to) {
            const size_t pos = text.find(needle, search);
            if (pos == std::string::npos || pos >= to) {
                return false;
            }

            size_t value_pos = skip_ws(text, pos + needle.size(), to);
            if (value_pos >= to || text[value_pos] != ':') {
                search = pos + needle.size();
                continue;
            }
            value_pos = skip_ws(text, value_pos + 1, to);
            if (value_pos >= to) {
                return false;
            }

            size_t end = value_pos;
            if ((end + 1) < to && text[end] == '0' && (text[end + 1] == 'x' || text[end + 1] == 'X')) {
                end += 2;
                while (end < to && std::isxdigit((unsigned char)text[end]) != 0) {
                    ++end;
                }
            } else {
                while (end < to && std::isdigit((unsigned char)text[end]) != 0) {
                    ++end;
                }
            }
            if (end == value_pos) {
                search = pos + needle.size();
                continue;
            }

            try {
                out = (uintptr_t)std::stoull(text.substr(value_pos, end - value_pos), nullptr, 0);
                return true;
            } catch (...) {
                search = pos + needle.size();
            }
        }
        return false;
    }

    bool resolve_weapon_entity(uintptr_t entity_list, uintptr_t pawn, uintptr_t& out_weapon) const {
        out_weapon = 0;
        if (pawn == 0 || entity_list == 0) {
            return false;
        }

        if (offsets_.m_pClippingWeapon != 0 &&
            read(pawn + offsets_.m_pClippingWeapon, out_weapon) &&
            out_weapon != 0) {
            return true;
        }

        if (offsets_.m_pWeaponServices == 0 || offsets_.m_hActiveWeapon == 0) {
            return false;
        }

        uintptr_t weapon_services = 0;
        if (!read(pawn + offsets_.m_pWeaponServices, weapon_services) || weapon_services == 0) {
            return false;
        }

        std::uint32_t active_weapon_handle = 0;
        if (!read(weapon_services + offsets_.m_hActiveWeapon, active_weapon_handle) || active_weapon_handle == 0) {
            return false;
        }

        uintptr_t weapon_list = 0;
        if (!read(entity_list + 0x8 * ((active_weapon_handle & 0x7FFF) >> 9) + 0x10, weapon_list) || weapon_list == 0) {
            return false;
        }

        return read(weapon_list + 0x70 * (active_weapon_handle & 0x1FF), out_weapon) && out_weapon != 0;
    }

    void load_extra_offsets() {
        constexpr const char* kOffsetsUrl =
            "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.json";
        constexpr const char* kClientDllUrl =
            "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.json";

        std::optional<std::string> offsets_text = download_text_from_url(kOffsetsUrl);
        std::optional<std::string> client_text = download_text_from_url(kClientDllUrl);

        if (!offsets_text.has_value()) {
            offsets_text = read_text_file("offsets.json");
        }
        if (!client_text.has_value()) {
            client_text = read_text_file("client_dll.json");
        }

        if (!offsets_text.has_value() || !client_text.has_value()) {
            debug::log("aim offsets: failed to download fresh offsets from github");
            return;
        }

        size_t client_block_begin = 0;
        size_t client_block_end = 0;
        if (find_object_range(*offsets_text, "client.dll", client_block_begin, client_block_end)) {
            extract_uint_value(*offsets_text, "dwEntityList", offsets_.dwEntityList, client_block_begin, client_block_end);
            extract_uint_value(*offsets_text, "dwLocalPlayerPawn", offsets_.dwLocalPlayerPawn, client_block_begin, client_block_end);
            extract_uint_value(*offsets_text, "dwLocalPlayerController", offsets_.dwLocalPlayerController,
                               client_block_begin, client_block_end);
            extract_uint_value(*offsets_text, "dwSensitivity", offsets_.dwSensitivity, client_block_begin, client_block_end);
            extract_uint_value(*offsets_text, "dwSensitivity_sensitivity", offsets_.dwSensitivity_sensitivity,
                               client_block_begin, client_block_end);
            extract_uint_value(*offsets_text, "dwViewMatrix", offsets_.dwViewMatrix, client_block_begin, client_block_end);
        }

        size_t client_root_begin = 0;
        size_t client_root_end = 0;
        if (!find_object_range(*client_text, "client.dll", client_root_begin, client_root_end)) {
            debug::log("aim offsets: client.dll block not found in client_dll.json");
            return;
        }

        size_t classes_begin = 0;
        size_t classes_end = 0;
        if (!find_object_range(*client_text, "classes", classes_begin, classes_end, client_root_begin, client_root_end)) {
            debug::log("aim offsets: classes block not found in client_dll.json");
            return;
        }

        auto extract_field = [&](const char* class_name, const char* field_name, uintptr_t& out) -> bool {
            size_t class_begin = 0;
            size_t class_end = 0;
            if (!find_object_range(*client_text, class_name, class_begin, class_end, classes_begin, classes_end)) {
                return false;
            }
            size_t fields_begin = 0;
            size_t fields_end = 0;
            if (!find_object_range(*client_text, "fields", fields_begin, fields_end, class_begin, class_end)) {
                return false;
            }
            return extract_uint_value(*client_text, field_name, out, fields_begin, fields_end);
        };

        extract_field("C_BaseEntity", "m_iTeamNum", offsets_.m_iTeamNum);
        extract_field("C_BaseEntity", "m_lifeState", offsets_.m_lifeState);
        extract_field("C_BaseEntity", "m_iHealth", offsets_.m_iHealth);
        extract_field("C_BaseEntity", "m_pGameSceneNode", offsets_.m_pGameSceneNode);
        extract_field("CSkeletonInstance", "m_modelState", offsets_.m_modelState);
        extract_field("CCSPlayerController", "m_hPlayerPawn", offsets_.m_hPlayerPawn);
        extract_field("EntitySpottedState_t", "m_bSpottedByMask", offsets_.m_bSpottedByMask);
        if (!extract_field("C_CSPlayerPawn", "m_iIDEntIndex", offsets_.m_iIDEntIndex)) {
            extract_field("C_CSPlayerPawnBase", "m_iIDEntIndex", offsets_.m_iIDEntIndex);
        }
        if (!extract_field("C_CSPlayerPawn", "m_iCrosshairID", offsets_.m_iCrosshairID)) {
            extract_field("C_CSPlayerPawnBase", "m_iCrosshairID", offsets_.m_iCrosshairID);
        }

        if (!extract_field("C_CSPlayerPawn", "m_entitySpottedState", offsets_.m_entitySpottedState)) {
            extract_field("C_CSPlayerPawnBase", "m_entitySpottedState", offsets_.m_entitySpottedState);
        }
        if (!extract_field("C_CSPlayerPawn", "m_aimPunchAngle", offsets_.m_aimPunchAngle)) {
            extract_field("C_CSPlayerPawnBase", "m_aimPunchAngle", offsets_.m_aimPunchAngle);
        }
        if (!extract_field("C_CSPlayerPawn", "m_iShotsFired", offsets_.m_iShotsFired)) {
            extract_field("C_CSPlayerPawnBase", "m_iShotsFired", offsets_.m_iShotsFired);
        }
        if (!extract_field("C_CSPlayerPawn", "m_pClippingWeapon", offsets_.m_pClippingWeapon)) {
            extract_field("C_CSPlayerPawnBase", "m_pClippingWeapon", offsets_.m_pClippingWeapon);
        }
        if (!extract_field("C_CSPlayerPawn", "m_pWeaponServices", offsets_.m_pWeaponServices)) {
            extract_field("C_CSPlayerPawnBase", "m_pWeaponServices", offsets_.m_pWeaponServices);
        }
        extract_field("CPlayer_WeaponServices", "m_hActiveWeapon", offsets_.m_hActiveWeapon);
        if (!extract_field("C_EconEntity", "m_AttributeManager", offsets_.m_AttributeManager)) {
            extract_field("C_BasePlayerWeapon", "m_AttributeManager", offsets_.m_AttributeManager);
        }
        if (!extract_field("C_AttributeContainer", "m_Item", offsets_.m_Item)) {
            extract_field("C_EconEntity", "m_Item", offsets_.m_Item);
        }
        if (!extract_field("C_EconItemView", "m_iItemDefinitionIndex", offsets_.m_iItemDefinitionIndex)) {
            extract_field("C_BasePlayerWeapon", "m_iItemDefinitionIndex", offsets_.m_iItemDefinitionIndex);
        }
        if (!extract_field("C_CSPlayerPawn", "m_bGunGameImmunity", offsets_.m_bGunGameImmunity)) {
            extract_field("C_CSPlayerPawnBase", "m_bGunGameImmunity", offsets_.m_bGunGameImmunity);
        }

        debug::log("aim offsets: entity=%llu localPawn=%llu localCtrl=%llu vm=%llu team=%llu life=%llu hp=%llu "
                   "scene=%llu model=%llu hPawn=%llu idEnt=%llu crosshairId=%llu spottedState=%llu spottedMask=%llu punch=%llu shots=%llu clip=%llu "
                   "immune=%llu",
                   (unsigned long long)offsets_.dwEntityList,
                   (unsigned long long)offsets_.dwLocalPlayerPawn,
                   (unsigned long long)offsets_.dwLocalPlayerController,
                   (unsigned long long)offsets_.dwViewMatrix,
                   (unsigned long long)offsets_.m_iTeamNum,
                   (unsigned long long)offsets_.m_lifeState,
                   (unsigned long long)offsets_.m_iHealth,
                   (unsigned long long)offsets_.m_pGameSceneNode,
                   (unsigned long long)offsets_.m_modelState,
                   (unsigned long long)offsets_.m_hPlayerPawn,
                   (unsigned long long)offsets_.m_iIDEntIndex,
                   (unsigned long long)offsets_.m_iCrosshairID,
                   (unsigned long long)offsets_.m_entitySpottedState,
                   (unsigned long long)offsets_.m_bSpottedByMask,
                   (unsigned long long)offsets_.m_aimPunchAngle,
                   (unsigned long long)offsets_.m_iShotsFired,
                   (unsigned long long)offsets_.m_pClippingWeapon,
                   (unsigned long long)offsets_.m_bGunGameImmunity);
    }

    HANDLE handle_ = nullptr;
    uintptr_t client_base_ = 0;
    Offsets offsets_{};
    bool offsets_loaded_ = false;
    std::chrono::steady_clock::time_point next_attach_attempt_{};
};

struct AimState {
    Runtime runtime{};
    SmoothAim smooth{};
    std::optional<Target> locked{};
    std::optional<Target> last_locked{};
    bool shift_pressed_by_bot = false;
    bool left_pressed_by_bot = false;
    bool trigger_pending = false;
    double trigger_fire_at = 0.0;
    double shift_hold_until = 0.0;
    bool has_last = false;
    std::chrono::steady_clock::time_point last_frame{};
    const SpreadPatternConfig* spread_pattern = nullptr;
    std::uint16_t spread_weapon_index = 0;
    int spread_pattern_index = 1;
    int spread_substep_index = 0;
    double spread_next_step_at = 0.0;
    bool spread_pattern_exhausted = false;
    float spread_carry_x = 0.0f;
    float spread_carry_y = 0.0f;
    int spread_total_x = 0;
    int spread_total_y = 0;
    float spread_last_sensitivity = 1.0f;
    double spread_last_weapon_seen_at = 0.0;
};

AimState& aim_state() {
    static AimState s{};
    return s;
}

bool is_key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool is_left_shift_down() {
    return is_key_down(VK_LSHIFT);
}

bool is_target_visible_for_aim(Runtime& runtime, uintptr_t entity_pawn_addr, const Offsets& offsets) {
    if (entity_pawn_addr == 0 || offsets.m_entitySpottedState == 0 || offsets.m_bSpottedByMask == 0) {
        return false;
    }
    const uintptr_t spotted_addr = entity_pawn_addr + offsets.m_entitySpottedState + offsets.m_bSpottedByMask;
    std::uint8_t spotted = 0;
    if (runtime.read(spotted_addr, spotted)) {
        return spotted != 0;
    }
    int spotted_fallback = 0;
    if (runtime.read(spotted_addr, spotted_fallback)) {
        return (spotted_fallback & 1) != 0;
    }
    return false;
}

void set_left_shift(bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = (WORD)MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(INPUT));
}

void move_mouse_relative(int dx, int dy) {
    if (dx == 0 && dy == 0) {
        return;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

void set_left_mouse_button(bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void draw_fov_circle(ImDrawList* draw_list, int w, int h, float radius) {
    draw_list->AddCircle(ImVec2(w * 0.5f, h * 0.5f), radius, IM_COL32(240, 247, 255, 242), 96, 2.2f);
}
#endif

} // namespace

void draw_aim_overlay(ImDrawList* draw_list, const ui::State& state, int screen_width, int screen_height, bool overlay_mode) {
#ifndef _WIN32
    (void)draw_list;
    (void)state;
    (void)screen_width;
    (void)screen_height;
    (void)overlay_mode;
#else
    AimState& s = aim_state();
    auto release_trigger_if_needed = [&]() {
        if (s.left_pressed_by_bot) {
            set_left_mouse_button(false);
            s.left_pressed_by_bot = false;
        }
        s.trigger_pending = false;
        s.trigger_fire_at = 0.0;
    };
    auto reset_spread_state = [&]() {
        s.spread_pattern = nullptr;
        s.spread_weapon_index = 0;
        s.spread_pattern_index = 1;
        s.spread_substep_index = 0;
        s.spread_next_step_at = 0.0;
        s.spread_pattern_exhausted = false;
        s.spread_carry_x = 0.0f;
        s.spread_carry_y = 0.0f;
        s.spread_total_x = 0;
        s.spread_total_y = 0;
        s.spread_last_weapon_seen_at = 0.0;
    };
    if (!overlay_mode || draw_list == nullptr || screen_width <= 0 || screen_height <= 0) {
        if (s.shift_pressed_by_bot) {
            set_left_shift(false);
            s.shift_pressed_by_bot = false;
        }
        release_trigger_if_needed();
        reset_spread_state();
        return;
    }
    auto reset_state = [&](bool release_shift) {
        if (release_shift && s.shift_pressed_by_bot) {
            set_left_shift(false);
            s.shift_pressed_by_bot = false;
            debug::log("shift: force release");
        }
        release_trigger_if_needed();
        s.shift_hold_until = 0.0;
        s.locked.reset();
        s.last_locked.reset();
        s.smooth.reset();
        reset_spread_state();
    };
    const auto now_tp = std::chrono::steady_clock::now();
    float dt = 1.0f / 144.0f;
    if (s.has_last) {
        dt = std::chrono::duration<float>(now_tp - s.last_frame).count();
        dt = clampf(dt, 1.0f / 240.0f, 0.12f);
    } else {
        s.has_last = true;
    }
    s.last_frame = now_tp;
    const double now_s = std::chrono::duration<double>(now_tp.time_since_epoch()).count();

    const bool aim_enabled = state.aimbot_enabled;
    const bool spread_enabled = state.spread_control_enabled;
    const bool trigger_enabled = state.triggerbot_enabled;
    if (!aim_enabled && !spread_enabled && !trigger_enabled) {
        if (should_log_throttled("aim_disabled", now_s, 1.5)) {
            debug::log("aim: disabled in UI");
        }
        reset_state(true);
        return;
    }

    const float current_fov = clampf(state.aimbot_fov, kFovMin, kFovMax);
    if (aim_enabled) {
        draw_fov_circle(draw_list, screen_width, screen_height, current_fov);
    }

    const bool lmb_down = is_key_down(VK_LBUTTON);
    const bool fire_active = lmb_down || s.left_pressed_by_bot;
    const bool aim_active = aim_enabled && fire_active;
    const bool spread_active = spread_enabled && fire_active;
    if (!s.runtime.ensure() || (!aim_active && !spread_active && !trigger_enabled)) {
        if (!aim_active && !spread_active && !trigger_enabled) {
            if (should_log_throttled("aim_wait_lmb", now_s, 1.0)) {
                debug::log("aim: waiting for LMB hold");
            }
        } else if (should_log_throttled("aim_runtime_fail", now_s, 1.0)) {
            debug::log("aim: runtime not ready");
        }
        reset_state(true);
        return;
    }

    const Offsets& o = s.runtime.o();
    uintptr_t entity_list = 0;
    const bool have_entity_list = s.runtime.read(s.runtime.client() + o.dwEntityList, entity_list) && entity_list != 0;

    uintptr_t local_pawn = 0;
    int local_team = 0;
    bool local_ok = false;
    if (s.runtime.read(s.runtime.client() + o.dwLocalPlayerPawn, local_pawn) && local_pawn != 0 &&
        s.runtime.read(local_pawn + o.m_iTeamNum, local_team) && local_team > 0) {
        local_ok = true;
    }
    if (!local_ok && have_entity_list && o.dwLocalPlayerController != 0 && o.m_hPlayerPawn != 0) {
        uintptr_t local_controller = 0;
        uintptr_t local_controller_pawn = 0;
        uintptr_t local_list_pawn = 0;
        uintptr_t local_resolved_pawn = 0;
        if (s.runtime.read(s.runtime.client() + o.dwLocalPlayerController, local_controller) && local_controller != 0 &&
            s.runtime.read(local_controller + o.m_hPlayerPawn, local_controller_pawn) && local_controller_pawn != 0 &&
            s.runtime.read(entity_list + 0x8 * ((local_controller_pawn & 0x7FFF) >> 9) + 0x10, local_list_pawn) &&
            local_list_pawn != 0 &&
            s.runtime.read(local_list_pawn + 0x70 * (local_controller_pawn & 0x1FF), local_resolved_pawn) &&
            local_resolved_pawn != 0 && s.runtime.read(local_resolved_pawn + o.m_iTeamNum, local_team) &&
            local_team > 0) {
            local_pawn = local_resolved_pawn;
            local_ok = true;
            if (should_log_throttled("aim_local_ctrl_ok", now_s, 2.0)) {
                debug::log("aim: local pawn resolved via controller");
            }
        }
    }
    if (!local_ok) {
        if (should_log_throttled("aim_local_fail", now_s, 1.0)) {
            debug::log("aim: failed reading local pawn/team (localPawn=0x%llx team=%d)", (unsigned long long)local_pawn,
                       local_team);
        }
        reset_state(true);
        return;
    }

    int recoil_move_x = 0;
    int recoil_move_y = 0;
    if (spread_active) {
        float sensitivity = 0.0f;
        if (s.runtime.read_current_sensitivity(sensitivity) && std::isfinite(sensitivity) && sensitivity > 0.01f) {
            s.spread_last_sensitivity = sensitivity;
        }
        const float active_sensitivity = std::max(0.01f, s.spread_last_sensitivity);

        std::uint16_t detected_weapon_index = 0;
        const bool have_weapon = have_entity_list && s.runtime.read_weapon_index(entity_list, local_pawn, detected_weapon_index);
        const SpreadPatternConfig* detected_pattern = have_weapon ? find_spread_pattern(detected_weapon_index) : nullptr;
        if (detected_pattern != nullptr) {
            s.spread_last_weapon_seen_at = now_s;
        }

        const bool keep_cached_pattern = s.spread_pattern != nullptr &&
                                         (now_s - s.spread_last_weapon_seen_at) <= kSpreadReadGraceSeconds;
        const SpreadPatternConfig* pattern = detected_pattern != nullptr ? detected_pattern : (keep_cached_pattern ? s.spread_pattern : nullptr);
        const std::uint16_t active_weapon_index =
            detected_pattern != nullptr ? detected_weapon_index : (keep_cached_pattern ? s.spread_weapon_index : 0);

        if (pattern == nullptr) {
            reset_spread_state();
        } else {
            if (s.spread_pattern != pattern || s.spread_weapon_index != active_weapon_index) {
                reset_spread_state();
                s.spread_pattern = pattern;
                s.spread_weapon_index = active_weapon_index;
                s.spread_last_weapon_seen_at = now_s;
                s.spread_next_step_at = now_s + static_cast<double>(pattern->step_delay_us) / 1000000.0;
                if (should_log_throttled("spread_pattern_start", now_s, 0.8)) {
                    debug::log("spread: start pattern weapon=%u size=%d", (unsigned)active_weapon_index, pattern->size);
                }
            }

            int processed_steps = 0;
            while (!s.spread_pattern_exhausted && s.spread_pattern != nullptr && now_s >= s.spread_next_step_at &&
                   processed_steps < 32) {
                ++processed_steps;
                if (s.spread_pattern_index >= s.spread_pattern->size) {
                    s.spread_pattern_exhausted = true;
                    if (should_log_throttled("spread_pattern_end", now_s, 0.8)) {
                        debug::log("spread: pattern exhausted weapon=%u", (unsigned)s.spread_weapon_index);
                    }
                    break;
                }

                const float normalized_x =
                    normalize_pattern_value(s.spread_pattern->raw_x[s.spread_pattern_index], active_sensitivity);
                const float normalized_y =
                    normalize_pattern_value(s.spread_pattern->raw_y[s.spread_pattern_index], active_sensitivity);
                const float step_x =
                    (normalized_x / static_cast<float>(s.spread_pattern->smoothness)) + s.spread_carry_x;
                const float step_y =
                    (normalized_y / static_cast<float>(s.spread_pattern->smoothness)) + s.spread_carry_y;

                const int out_x = static_cast<int>(std::lround(step_x));
                const int out_y = static_cast<int>(std::lround(step_y));
                recoil_move_x += out_x;
                recoil_move_y += out_y;
                s.spread_total_x += out_x;
                s.spread_total_y += out_y;
                s.spread_carry_x = step_x - static_cast<float>(out_x);
                s.spread_carry_y = step_y - static_cast<float>(out_y);

                ++s.spread_substep_index;
                const double step_delay_s = static_cast<double>(s.spread_pattern->step_delay_us) / 1000000.0;
                const double settle_delay_s = static_cast<double>(s.spread_pattern->settle_delay_us) / 1000000.0;
                if (s.spread_substep_index < s.spread_pattern->smoothness) {
                    s.spread_next_step_at += step_delay_s;
                } else {
                    s.spread_substep_index = 0;
                    ++s.spread_pattern_index;
                    s.spread_next_step_at += step_delay_s + settle_delay_s;
                }
            }
        }
    } else {
        reset_spread_state();
    }

    if (!aim_active && !trigger_enabled) {
        if (recoil_move_x != 0 || recoil_move_y != 0) {
            move_mouse_relative(recoil_move_x, recoil_move_y);
        }
        return;
    }

    std::array<float, 16> vm{};
    uintptr_t entity_ptr = 0;
    if (!have_entity_list ||
        !s.runtime.read(s.runtime.client() + o.dwViewMatrix, vm) ||
        !s.runtime.read(entity_list + 0x10, entity_ptr) ||
        entity_ptr == 0) {
        s.locked.reset();
        s.last_locked.reset();
        s.smooth.reset();
        if (recoil_move_x != 0 || recoil_move_y != 0) {
            move_mouse_relative(recoil_move_x, recoil_move_y);
        }
        release_trigger_if_needed();
        if (should_log_throttled("aim_vm_elist_fail", now_s, 1.0)) {
            debug::log("aim: failed reading view matrix/entity list");
        }
        return;
    }

    std::vector<Target> targets{};
    std::vector<Target> trigger_targets{};
    for (int i = 1; i <= 64; ++i) {
        uintptr_t controller = 0;
        uintptr_t controller_pawn = 0;
        uintptr_t list_pawn = 0;
        uintptr_t pawn = 0;
        if (!s.runtime.read(entity_ptr + 0x70 * (i & 0x1FF), controller) || controller == 0 ||
            !s.runtime.read(controller + o.m_hPlayerPawn, controller_pawn) || controller_pawn == 0 ||
            !s.runtime.read(entity_list + 0x8 * ((controller_pawn & 0x7FFF) >> 9) + 0x10, list_pawn) || list_pawn == 0 ||
            !s.runtime.read(list_pawn + 0x70 * (controller_pawn & 0x1FF), pawn) || pawn == 0 || pawn == local_pawn) {
            continue;
        }

        int team = 0, hp = 0, life = 0;
        if (!s.runtime.read(pawn + o.m_iTeamNum, team) || !s.runtime.read(pawn + o.m_iHealth, hp) ||
            !s.runtime.read(pawn + o.m_lifeState, life) || team == local_team || hp <= 0 || life != 256) {
            continue;
        }
        if (o.m_bGunGameImmunity != 0) {
            int immunity = 0;
            if (s.runtime.read(pawn + o.m_bGunGameImmunity, immunity) && (immunity == 1 || immunity == 257)) {
                continue;
            }
        }

        const bool target_visible = is_target_visible_for_aim(s.runtime, pawn, o);
        const bool allow_aim_target = target_visible;
        const bool allow_trigger_target =
            trigger_enabled && (!state.triggerbot_wall_check || target_visible);
        if (!allow_aim_target && !allow_trigger_target) {
            continue;
        }

        uintptr_t game_scene = 0;
        uintptr_t bone_matrix = 0;
        if (!s.runtime.read(pawn + o.m_pGameSceneNode, game_scene) || game_scene == 0 ||
            !s.runtime.read(game_scene + o.m_modelState + 0x80, bone_matrix) || bone_matrix == 0) {
            continue;
        }

        Vec3 head{};
        float leg_z = 0.0f;
        if (!s.runtime.read(bone_matrix + (uintptr_t)kHeadBoneId * 0x20, head) ||
            !s.runtime.read(bone_matrix + (uintptr_t)kLegBoneId * 0x20 + 0x8, leg_z)) {
            continue;
        }

        Vec2 head2d{};
        Vec2 leg2d{};
        if (!world_to_screen(vm, head.x, head.y, head.z + 8.0f, (float)screen_width, (float)screen_height, head2d) ||
            !world_to_screen(vm, head.x, head.y, leg_z, (float)screen_width, (float)screen_height, leg2d)) {
            continue;
        }
        const float top = std::min(head2d.y, leg2d.y);
        const float bottom = std::max(head2d.y, leg2d.y);
        const float box_h = bottom - top;
        if (box_h < 6.0f) {
            continue;
        }
        const int hitbox_priority = std::clamp(state.aimbot_hitbox_priority, 0, 2);
        const auto push_target = [&](float x, float y) {
            Target t{};
            t.x = x;
            t.y = y;
            targets.push_back(t);
        };
        const auto push_trigger_target = [&](float x, float y) {
            Target t{};
            t.x = x;
            t.y = y;
            trigger_targets.push_back(t);
        };
        const auto push_trigger_body_line = [&](float center_x, float y, float span) {
            push_trigger_target(center_x, y);
            push_trigger_target(center_x - span, y);
            push_trigger_target(center_x + span, y);
        };
        const float head_target_y = top + box_h * kHeadOffsetRatio;
        const float chest_target_y = top + box_h * kChestOffsetRatio;
        const float stomach_target_y = top + box_h * kStomachOffsetRatio;
        const float body_target_y = top + box_h * kBodyOffsetRatio;
        const float trigger_body_span = clampf(box_h * 0.16f, 4.0f, 24.0f);
        if (allow_aim_target) {
            if (hitbox_priority == 0) {
                push_target(head2d.x, head_target_y);
            } else if (hitbox_priority == 1) {
                push_target(head2d.x, body_target_y);
            } else {
                push_target(head2d.x, head_target_y);
                push_target(head2d.x, body_target_y);
            }
        }

        if (allow_trigger_target) {
            switch (std::clamp(state.triggerbot_hitgroup, 0, 4)) {
            case 0:
                push_trigger_target(head2d.x, head_target_y);
                break;
            case 1:
                push_trigger_body_line(head2d.x, chest_target_y, trigger_body_span);
                break;
            case 2:
                push_trigger_body_line(head2d.x, stomach_target_y, trigger_body_span);
                break;
            case 3:
                push_trigger_body_line(head2d.x, chest_target_y, trigger_body_span);
                push_trigger_body_line(head2d.x, stomach_target_y, trigger_body_span);
                break;
            case 4:
            default:
                push_trigger_target(head2d.x, head_target_y);
                push_trigger_body_line(head2d.x, chest_target_y, trigger_body_span);
                push_trigger_body_line(head2d.x, stomach_target_y, trigger_body_span);
                break;
            }
        }
    }

    const float cx = screen_width * 0.5f;
    const float cy = screen_height * 0.5f;
    const float effective_center_x = cx - static_cast<float>(s.spread_total_x);
    const float effective_center_y = cy - static_cast<float>(s.spread_total_y);
    s.locked = std::nullopt;
    if (s.last_locked.has_value()) {
        float best = (std::min(kLockRadius, current_fov * 0.85f));
        best *= best;
        for (const Target& t : targets) {
            const float dx = t.x - s.last_locked->x;
            const float dy = t.y - s.last_locked->y;
            const float d = dx * dx + dy * dy;
            if (d <= best) {
                best = d;
                s.locked = t;
            }
        }
    }
    if (!s.locked.has_value()) {
        float best = current_fov * current_fov;
        for (const Target& t : targets) {
            const float dx = t.x - effective_center_x;
            const float dy = t.y - effective_center_y;
            const float d = dx * dx + dy * dy;
            if (d <= best) {
                best = d;
                s.locked = t;
            }
        }
    }

    bool trigger_on_target = false;
    if (trigger_enabled) {
        const float trigger_radius_sq = kTriggerRadius * kTriggerRadius;
        for (const Target& t : trigger_targets) {
            const float dx = t.x - effective_center_x;
            const float dy = t.y - effective_center_y;
            const float dist_sq = dx * dx + dy * dy;
            if (dist_sq <= trigger_radius_sq) {
                trigger_on_target = true;
                if (should_log_throttled("trigger_target", now_s, 0.12)) {
                    debug::log("trigger: target detected center=(%.1f,%.1f) point=(%.1f,%.1f) dist=%.2f wall=%d targets=%d",
                               effective_center_x, effective_center_y, t.x, t.y, std::sqrt(dist_sq),
                               state.triggerbot_wall_check ? 1 : 0, (int)trigger_targets.size());
                }
                break;
            }
        }
        if (trigger_on_target) {
            const double trigger_delay_s = static_cast<double>(std::max(0.0f, state.triggerbot_delay_ms)) / 1000.0;
            if (!s.left_pressed_by_bot && !lmb_down) {
                if (!s.trigger_pending) {
                    s.trigger_pending = true;
                    s.trigger_fire_at = now_s + trigger_delay_s;
                }
                if (now_s >= s.trigger_fire_at) {
                    set_left_mouse_button(true);
                    s.left_pressed_by_bot = true;
                    s.trigger_pending = false;
                    s.trigger_fire_at = 0.0;
                }
            }
        } else {
            release_trigger_if_needed();
        }
    } else {
        release_trigger_if_needed();
    }

    const bool want_shift = state.auto_shift_enabled && fire_active;
    if (want_shift) {
        s.shift_hold_until = std::max(s.shift_hold_until, now_s + 0.3);
        if (!s.shift_pressed_by_bot && !is_left_shift_down()) {
            set_left_shift(true);
            s.shift_pressed_by_bot = true;
            debug::log("shift: press");
        }
    } else if (s.shift_pressed_by_bot && now_s >= s.shift_hold_until) {
        set_left_shift(false);
        s.shift_pressed_by_bot = false;
        s.shift_hold_until = 0.0;
        debug::log("shift: release");
    }

    if (!s.locked.has_value()) {
        s.last_locked.reset();
        s.smooth.reset();
        if (recoil_move_x != 0 || recoil_move_y != 0) {
            move_mouse_relative(recoil_move_x, recoil_move_y);
        }
        if (should_log_throttled("aim_no_target", now_s, 0.6)) {
            debug::log("aim: no target in fov (targets=%d fov=%.1f)", (int)targets.size(), current_fov);
        }
        return;
    }

    bool switched = !s.last_locked.has_value();
    if (!switched) {
        const float dx = s.locked->x - s.last_locked->x;
        const float dy = s.locked->y - s.last_locked->y;
        switched = (dx * dx + dy * dy) > (kTargetSwitchDist * kTargetSwitchDist);
    }
    const float aim_dx = s.locked->x - effective_center_x;
    const float aim_dy = s.locked->y - effective_center_y;

    if (switched) {
        s.smooth.mark_target_switch(now_s, aim_dx, aim_dy);
    }

    if (!aim_active) {
        s.last_locked.reset();
        s.smooth.reset();
        if (recoil_move_x != 0 || recoil_move_y != 0) {
            move_mouse_relative(recoil_move_x, recoil_move_y);
        }
        return;
    }

    if (s.smooth.is_ready(now_s)) {
        auto [mx, my] = s.smooth.step(aim_dx, aim_dy, dt, now_s);
        mx += recoil_move_x;
        my += recoil_move_y;
        if (should_log_throttled("aim_move", now_s, 0.2)) {
            debug::log("aim: lock=(%.1f,%.1f) center=(%.1f,%.1f) spreadTotal=(%d,%d) recoilStep=(%d,%d) aimDelta=(%.1f,%.1f) move=(%d,%d) targets=%d",
                       s.locked->x, s.locked->y, cx, cy, s.spread_total_x, s.spread_total_y, recoil_move_x, recoil_move_y,
                       aim_dx, aim_dy, mx, my, (int)targets.size());
        }
        move_mouse_relative(mx, my);
    } else if (recoil_move_x != 0 || recoil_move_y != 0) {
        move_mouse_relative(recoil_move_x, recoil_move_y);
    }
    s.last_locked = s.locked;
#endif
}

} // namespace timmy::features
