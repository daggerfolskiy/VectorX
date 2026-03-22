#include "features/esp.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
#include <d3d11.h>
#include <TlHelp32.h>
#include <wincodec.h>
#include <wininet.h>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Windowscodecs.lib")
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

float snap_pixel(float value) {
    return std::floor(value) + 0.5f;
}

ImU32 color_u32_from_array(const float color[4]) {
    if (color == nullptr) {
        return IM_COL32(255, 255, 255, 255);
    }

    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        std::clamp(color[0], 0.0f, 1.0f),
        std::clamp(color[1], 0.0f, 1.0f),
        std::clamp(color[2], 0.0f, 1.0f),
        std::clamp(color[3], 0.0f, 1.0f)));
}

struct EspOffsets {
    uintptr_t dwEntityList = 38445272;
    uintptr_t dwLocalPlayerPawn = 33966816;
    uintptr_t dwViewMatrix = 36744672;
    uintptr_t dwGlobalVars = 33920464;
    uintptr_t dwGameRules = 0;
    uintptr_t dwPlantedC4 = 0;
    uintptr_t m_iTeamNum = 1011;
    uintptr_t m_lifeState = 860;
    uintptr_t m_pGameSceneNode = 824;
    uintptr_t m_modelState = 352;
    uintptr_t m_hPlayerPawn = 2316;
    uintptr_t m_hOwnerEntity = 0;
    uintptr_t m_iHealth = 852;
    uintptr_t m_ArmorValue = 0;
    uintptr_t m_pClippingWeapon = 0;
    uintptr_t m_pWeaponServices = 0;
    uintptr_t m_hActiveWeapon = 0;
    uintptr_t m_hMyWeapons = 0;
    uintptr_t m_AttributeManager = 0;
    uintptr_t m_Item = 0;
    uintptr_t m_iItemDefinitionIndex = 0;
    uintptr_t m_bBombTicking = 0;
    uintptr_t m_bHasExploded = 0;
    uintptr_t m_flC4Blow = 0;
    uintptr_t m_flTimerLength = 0;
    uintptr_t m_nBombSite = 0;
    uintptr_t m_bBombPlanted = 0;

    bool valid() const {
        return dwEntityList != 0 && dwLocalPlayerPawn != 0 && dwViewMatrix != 0 && m_iTeamNum != 0 &&
               m_lifeState != 0 && m_pGameSceneNode != 0 && m_modelState != 0 && m_hPlayerPawn != 0 &&
               m_iHealth != 0;
    }
};

struct BoneConfig {
    std::array<int, 16> ids = {6, 5, 4, 0, 13, 14, 15, 9, 10, 11, 25, 26, 27, 22, 23, 24};
};

bool world_to_screen(const std::array<float, 16>& mtx, float posx, float posy, float posz, float width, float height,
                     Vec2& out) {
    const float screen_w = (mtx[12] * posx) + (mtx[13] * posy) + (mtx[14] * posz) + mtx[15];
    if (screen_w <= 0.001f) {
        return false;
    }
    const float screen_x = (mtx[0] * posx) + (mtx[1] * posy) + (mtx[2] * posz) + mtx[3];
    const float screen_y = (mtx[4] * posx) + (mtx[5] * posy) + (mtx[6] * posz) + mtx[7];
    const float cam_x = width * 0.5f;
    const float cam_y = height * 0.5f;
    out.x = cam_x + (cam_x * screen_x / screen_w);
    out.y = cam_y - (cam_y * screen_y / screen_w);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

size_t skip_ws(const std::string& text, size_t pos, size_t to) {
    while (pos < to && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

const char* weapon_name_from_index(std::uint16_t weapon_index) {
    switch (weapon_index) {
    case 1:
        return "Deagle";
    case 2:
        return "Dual Berettas";
    case 3:
        return "Five-SeveN";
    case 4:
        return "Glock-18";
    case 7:
        return "AK-47";
    case 8:
        return "AUG";
    case 9:
        return "AWP";
    case 10:
        return "FAMAS";
    case 11:
        return "G3SG1";
    case 13:
        return "Galil AR";
    case 14:
        return "M249";
    case 16:
        return "M4A4";
    case 17:
        return "MAC-10";
    case 19:
        return "P90";
    case 23:
        return "MP5-SD";
    case 24:
        return "UMP-45";
    case 25:
        return "XM1014";
    case 26:
        return "PP-Bizon";
    case 27:
        return "MAG-7";
    case 28:
        return "Negev";
    case 29:
        return "Sawed-Off";
    case 30:
        return "Tec-9";
    case 31:
        return "Zeus x27";
    case 32:
        return "P2000";
    case 33:
        return "MP7";
    case 34:
        return "MP9";
    case 35:
        return "Nova";
    case 36:
        return "P250";
    case 38:
        return "SCAR-20";
    case 39:
        return "SG 553";
    case 40:
        return "SSG 08";
    case 42:
    case 59:
    case 80:
        return "Knife";
    case 43:
        return "Flashbang";
    case 44:
        return "HE Grenade";
    case 45:
        return "Smoke";
    case 46:
        return "Molotov";
    case 47:
        return "Decoy";
    case 48:
        return "Incendiary";
    case 49:
        return "C4";
    case 57:
        return "Healthshot";
    case 60:
        return "M4A1-S";
    case 61:
        return "USP-S";
    case 63:
        return "CZ75-Auto";
    case 64:
        return "R8 Revolver";
    case 500:
        return "Bayonet";
    case 503:
        return "Classic Knife";
    case 505:
        return "Flip Knife";
    case 506:
        return "Gut Knife";
    case 507:
        return "Karambit";
    case 508:
        return "M9 Bayonet";
    case 509:
        return "Huntsman";
    case 512:
        return "Falchion";
    case 514:
        return "Bowie Knife";
    case 515:
        return "Butterfly Knife";
    case 516:
        return "Shadow Daggers";
    case 517:
        return "Paracord Knife";
    case 518:
        return "Survival Knife";
    case 519:
        return "Ursus Knife";
    case 520:
        return "Navaja Knife";
    case 521:
        return "Nomad Knife";
    case 522:
        return "Stiletto Knife";
    case 523:
        return "Talon Knife";
    case 525:
        return "Skeleton Knife";
    default:
        return nullptr;
    }
}

const char* weapon_model_from_index(std::uint16_t weapon_index) {
    switch (weapon_index) {
    case 1:
        return "deagle";
    case 2:
        return "elite";
    case 3:
        return "fiveseven";
    case 4:
        return "glock";
    case 7:
        return "ak47";
    case 8:
        return "aug";
    case 9:
        return "awp";
    case 10:
        return "famas";
    case 11:
        return "g3sg1";
    case 13:
        return "galilar";
    case 14:
        return "m249";
    case 16:
        return "m4a1";
    case 17:
        return "mac10";
    case 19:
        return "p90";
    case 23:
        return "mp5sd";
    case 24:
        return "ump45";
    case 25:
        return "xm1014";
    case 26:
        return "bizon";
    case 27:
        return "mag7";
    case 28:
        return "negev";
    case 29:
        return "sawedoff";
    case 30:
        return "tec9";
    case 31:
        return "taser";
    case 32:
        return "hkp2000";
    case 33:
        return "mp7";
    case 34:
        return "mp9";
    case 35:
        return "nova";
    case 36:
        return "p250";
    case 38:
        return "scar20";
    case 39:
        return "sg556";
    case 40:
        return "ssg08";
    case 42:
        return "knife";
    case 43:
        return "flashbang";
    case 44:
        return "hegrenade";
    case 45:
        return "smokegrenade";
    case 46:
        return "molotov";
    case 47:
        return "decoy";
    case 48:
        return "incgrenade";
    case 49:
        return "c4";
    case 57:
        return "healthshot";
    case 59:
        return "knife_t";
    case 60:
        return "m4a1_silencer";
    case 61:
        return "usp_silencer";
    case 63:
        return "cz75a";
    case 64:
        return "revolver";
    case 80:
        return "knife";
    case 500:
        return "bayonet";
    case 503:
        return "knife_css";
    case 505:
        return "knife_flip";
    case 506:
        return "knife_gut";
    case 507:
        return "knife_karambit";
    case 508:
        return "knife_m9_bayonet";
    case 509:
        return "knife_tactical";
    case 512:
        return "knife_falchion";
    case 514:
        return "knife_survival_bowie";
    case 515:
        return "knife_butterfly";
    case 516:
        return "knife_push";
    case 517:
        return "knife_cord";
    case 518:
        return "knife_canis";
    case 519:
        return "knife_ursus";
    case 520:
        return "knife_gypsy_jackknife";
    case 521:
        return "knife_outdoor";
    case 522:
        return "knife_stiletto";
    case 523:
        return "knife_widowmaker";
    case 525:
        return "knife_skeleton";
    default:
        return nullptr;
    }
}

const char* weapon_icon_from_model_name(const char* model_name) {
    if (model_name == nullptr || model_name[0] == '\0') {
        return nullptr;
    }

    static const std::unordered_map<std::string_view, const char*> kWeaponIconTable = {
        {"deagle", u8"\uE001"},
        {"elite", u8"\uE002"},
        {"fiveseven", u8"\uE003"},
        {"glock", u8"\uE004"},
        {"ak47", u8"\uE007"},
        {"aug", u8"\uE008"},
        {"awp", u8"\uE009"},
        {"famas", u8"\uE00A"},
        {"g3sg1", u8"\uE00B"},
        {"galilar", u8"\uE00D"},
        {"m249", u8"\uE03C"},
        {"m4a1", u8"\uE00E"},
        {"mac10", u8"\uE011"},
        {"p90", u8"\uE024"},
        {"ump45", u8"\uE018"},
        {"xm1014", u8"\uE019"},
        {"bizon", u8"\uE01A"},
        {"mag7", u8"\uE01B"},
        {"negev", u8"\uE01C"},
        {"sawedoff", u8"\uE01D"},
        {"tec9", u8"\uE01E"},
        {"taser", u8"\uE01F"},
        {"hkp2000", u8"\uE013"},
        {"mp7", u8"\uE021"},
        {"mp9", u8"\uE022"},
        {"nova", u8"\uE023"},
        {"p250", u8"\uE020"},
        {"scar20", u8"\uE026"},
        {"sg556", u8"\uE027"},
        {"ssg08", u8"\uE028"},
        {"knife", u8"\uE02A"},
        {"flashbang", u8"\uE02B"},
        {"hegrenade", u8"\uE02C"},
        {"smokegrenade", u8"\uE02D"},
        {"molotov", u8"\uE02E"},
        {"decoy", u8"\uE02F"},
        {"incgrenade", u8"\uE030"},
        {"c4", u8"\uE031"},
        {"knife_t", u8"\uE03B"},
        {"m4a1_silencer", u8"\uE010"},
        {"usp_silencer", u8"\uE03D"},
        {"cz75a", u8"\uE03F"},
        {"revolver", u8"\uE040"},
        {"bayonet", u8"\uE1F4"},
        {"knife_css", u8"\uE02A"},
        {"knife_flip", u8"\uE1F9"},
        {"knife_gut", u8"\uE1FA"},
        {"knife_karambit", u8"\uE1FB"},
        {"knife_m9_bayonet", u8"\uE1FC"},
        {"knife_tactical", u8"\uE1FD"},
        {"knife_falchion", u8"\uE200"},
        {"knife_survival_bowie", u8"\uE202"},
        {"knife_butterfly", u8"\uE203"},
        {"knife_push", u8"\uE204"},
        {"knife_cord", u8"\uE02A"},
        {"knife_canis", u8"\uE02A"},
        {"knife_ursus", u8"\uE02A"},
        {"knife_gypsy_jackknife", u8"\uE02A"},
        {"knife_outdoor", u8"\uE02A"},
        {"knife_stiletto", u8"\uE02A"},
        {"knife_widowmaker", u8"\uE02A"},
        {"knife_skeleton", u8"\uE02A"},
    };

    const auto it = kWeaponIconTable.find(model_name);
    if (it == kWeaponIconTable.end()) {
        return nullptr;
    }
    return it->second;
}

struct WeaponIconTexture {
#ifdef _WIN32
    ID3D11ShaderResourceView* shader_resource = nullptr;
#endif
    float width = 0.0f;
    float height = 0.0f;
    bool load_attempted = false;
};

#ifdef _WIN32
IWICImagingFactory* g_weapon_icon_wic_factory = nullptr;
ID3D11Device* g_weapon_icon_device = nullptr;
std::unordered_map<std::string, WeaponIconTexture> g_weapon_icon_cache;
bool g_weapon_icon_com_initialized = false;

std::optional<std::filesystem::path> find_weapon_icon_path(const char* model_name) {
    if (model_name == nullptr || model_name[0] == '\0') {
        return std::nullopt;
    }
    const std::array<const char*, 6> icon_dir_candidates = {
        "assets/weapon_icons",
        "../assets/weapon_icons",
        "../../assets/weapon_icons",
        "../../../assets/weapon_icons",
        "../../../../assets/weapon_icons",
        "../../../../../assets/weapon_icons",
    };
    for (const char* candidate : icon_dir_candidates) {
        std::filesystem::path icon_path = std::filesystem::path(candidate) / (std::string(model_name) + ".webp");
        std::error_code ec;
        if (std::filesystem::exists(icon_path, ec)) {
            return icon_path;
        }
    }
    return std::nullopt;
}

void release_weapon_icon_cache() {
    for (auto& entry : g_weapon_icon_cache) {
        if (entry.second.shader_resource != nullptr) {
            entry.second.shader_resource->Release();
            entry.second.shader_resource = nullptr;
        }
    }
    g_weapon_icon_cache.clear();
}

bool ensure_weapon_icon_wic_factory() {
    if (g_weapon_icon_wic_factory != nullptr) {
        return true;
    }
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com_result)) {
        g_weapon_icon_com_initialized = true;
    } else if (com_result != RPC_E_CHANGED_MODE) {
        return false;
    }
    return SUCCEEDED(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_weapon_icon_wic_factory)));
}

bool load_weapon_icon_texture_from_file(const std::filesystem::path& icon_path, WeaponIconTexture& out_texture) {
    if (g_weapon_icon_device == nullptr || !ensure_weapon_icon_wic_factory()) {
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* shader_resource = nullptr;

    bool loaded = false;
    do {
        if (FAILED(g_weapon_icon_wic_factory->CreateDecoderFromFilename(icon_path.c_str(), nullptr, GENERIC_READ,
                                                                        WICDecodeMetadataCacheOnLoad, &decoder))) {
            break;
        }
        if (FAILED(decoder->GetFrame(0, &frame))) {
            break;
        }
        UINT width = 0;
        UINT height = 0;
        if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
            break;
        }
        if (FAILED(g_weapon_icon_wic_factory->CreateFormatConverter(&converter))) {
            break;
        }
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                                         WICBitmapPaletteTypeCustom))) {
            break;
        }

        std::vector<std::uint8_t> pixels((size_t)width * (size_t)height * 4u);
        if (FAILED(converter->CopyPixels(nullptr, width * 4u, (UINT)pixels.size(), pixels.data()))) {
            break;
        }

        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = width;
        texture_desc.Height = height;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initial_data{};
        initial_data.pSysMem = pixels.data();
        initial_data.SysMemPitch = width * 4u;

        if (FAILED(g_weapon_icon_device->CreateTexture2D(&texture_desc, &initial_data, &texture))) {
            break;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = texture_desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = 1;

        if (FAILED(g_weapon_icon_device->CreateShaderResourceView(texture, &srv_desc, &shader_resource))) {
            break;
        }

        out_texture.shader_resource = shader_resource;
        out_texture.width = (float)width;
        out_texture.height = (float)height;
        shader_resource = nullptr;
        loaded = true;
    } while (false);

    if (shader_resource != nullptr) {
        shader_resource->Release();
    }
    if (texture != nullptr) {
        texture->Release();
    }
    if (converter != nullptr) {
        converter->Release();
    }
    if (frame != nullptr) {
        frame->Release();
    }
    if (decoder != nullptr) {
        decoder->Release();
    }

    return loaded;
}

bool try_get_weapon_icon_texture(std::uint16_t weapon_index, WeaponIconTexture*& out_texture) {
    out_texture = nullptr;
    const char* model_name = weapon_model_from_index(weapon_index);
    if (model_name == nullptr || model_name[0] == '\0') {
        return false;
    }

    WeaponIconTexture& entry = g_weapon_icon_cache[model_name];
    if (!entry.load_attempted) {
        entry.load_attempted = true;
        const std::optional<std::filesystem::path> icon_path = find_weapon_icon_path(model_name);
        if (icon_path.has_value()) {
            load_weapon_icon_texture_from_file(*icon_path, entry);
        }
    }

    if (entry.shader_resource == nullptr || entry.width <= 0.0f || entry.height <= 0.0f) {
        return false;
    }

    out_texture = &entry;
    return true;
}
#endif

class EspRuntime {
public:
#ifdef _WIN32
    ~EspRuntime() {
        detach();
    }

    bool ensure_ready() {
        if (!offsets_loaded_) {
            offsets_loaded_ = load_offsets();
        }
        if (!offsets_loaded_ || !offsets_.valid()) {
            return false;
        }

        if (process_handle_ != nullptr && is_process_alive() && client_base_ != 0) {
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < next_attach_attempt_) {
            return false;
        }
        next_attach_attempt_ = now + std::chrono::milliseconds(750);
        detach();

        const DWORD pid = find_process_id(L"cs2.exe");
        if (pid == 0) {
            return false;
        }
        const uintptr_t client_base = find_module_base(pid, L"client.dll");
        if (client_base == 0) {
            return false;
        }
        HANDLE handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (handle == nullptr) {
            return false;
        }
        process_handle_ = handle;
        pid_ = pid;
        client_base_ = client_base;
        return true;
    }

    bool read_view_matrix(std::array<float, 16>& out_view) const {
        if (client_base_ == 0) {
            return false;
        }
        SIZE_T bytes_read = 0;
        return ReadProcessMemory(process_handle_, reinterpret_cast<LPCVOID>(client_base_ + offsets_.dwViewMatrix),
                                 out_view.data(), sizeof(float) * out_view.size(), &bytes_read) != FALSE &&
               bytes_read == sizeof(float) * out_view.size();
    }

    bool read_u64(uintptr_t address, uintptr_t& out) const {
        return read_value(address, out);
    }

    bool read_i32(uintptr_t address, int& out) const {
        return read_value(address, out);
    }

    bool read_u32(uintptr_t address, std::uint32_t& out) const {
        return read_value(address, out);
    }

    bool read_u16(uintptr_t address, std::uint16_t& out) const {
        return read_value(address, out);
    }

    bool read_u8(uintptr_t address, std::uint8_t& out) const {
        return read_value(address, out);
    }

    bool read_f32(uintptr_t address, float& out) const {
        return read_value(address, out);
    }

    bool read_vec3(uintptr_t address, Vec3& out) const {
        return read_value(address, out);
    }

    uintptr_t client_base() const {
        return client_base_;
    }

    const EspOffsets& offsets() const {
        return offsets_;
    }

    const BoneConfig& bone_ids() const {
        return bone_ids_;
    }

    bool read_game_avg_fps(float& out_fps) const {
        if (client_base_ == 0 || offsets_.dwGlobalVars == 0) {
            return false;
        }

        uintptr_t global_vars = 0;
        if (!read_u64(client_base_ + offsets_.dwGlobalVars, global_vars) || global_vars < 0x10000) {
            global_vars = client_base_ + offsets_.dwGlobalVars;
        }

        int frame_count = 0;
        if (!read_i32(global_vars + 0x4, frame_count) || frame_count <= 0) {
            if (!read_i32(global_vars + 0x8, frame_count) || frame_count <= 0) {
                return false;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (!fps_initialized_) {
            fps_initialized_ = true;
            fps_prev_frame_count_ = frame_count;
            fps_prev_time_ = now;
            return false;
        }

        const float dt = std::chrono::duration<float>(now - fps_prev_time_).count();
        if (dt >= 0.10f) {
            const int frame_delta = frame_count - fps_prev_frame_count_;
            fps_prev_frame_count_ = frame_count;
            fps_prev_time_ = now;
            if (frame_delta > 0 && frame_delta < 2000) {
                const float instant_fps = (float)frame_delta / dt;
                if (instant_fps >= 10.0f && instant_fps <= 1000.0f) {
                    if (smoothed_game_fps_ <= 0.0f) {
                        smoothed_game_fps_ = instant_fps;
                    } else {
                        smoothed_game_fps_ = smoothed_game_fps_ * 0.82f + instant_fps * 0.18f;
                    }
                }
            }
        }

        if (smoothed_game_fps_ <= 0.0f) {
            return false;
        }
        out_fps = smoothed_game_fps_;
        return true;
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

        if (!read_u16(weapon_entity + offsets_.m_AttributeManager + offsets_.m_Item + offsets_.m_iItemDefinitionIndex,
                      out_index) ||
            out_index == 0) {
            return false;
        }
        return true;
    }

    bool read_weapon_index_from_entity(uintptr_t weapon_entity, std::uint16_t& out_index) const {
        out_index = 0;
        if (weapon_entity == 0 || offsets_.m_AttributeManager == 0 || offsets_.m_Item == 0 || offsets_.m_iItemDefinitionIndex == 0) {
            return false;
        }

        return read_u16(weapon_entity + offsets_.m_AttributeManager + offsets_.m_Item + offsets_.m_iItemDefinitionIndex, out_index) &&
               out_index != 0;
    }

    bool read_weapon_index_from_handle(uintptr_t entity_list, std::uint32_t weapon_handle, std::uint16_t& out_index) const {
        out_index = 0;
        uintptr_t weapon_entity = 0;
        return resolve_entity_from_handle(entity_list, weapon_handle, weapon_entity) &&
               read_weapon_index_from_entity(weapon_entity, out_index);
    }

    static bool is_valid_handle(std::uint32_t handle) {
        return handle != 0 && handle != 0xFFFFFFFFu;
    }

    bool read_weapon_handle_vector(uintptr_t vector_base, std::vector<std::uint32_t>& out_handles) const {
        out_handles.clear();
        if (vector_base == 0) {
            return false;
        }

        uintptr_t data_ptr = 0;
        int allocation_count = 0;
        int element_count = 0;
        if (read_u64(vector_base, data_ptr) && read_i32(vector_base + 0x8, allocation_count) &&
            read_i32(vector_base + 0xC, element_count) && data_ptr >= 0x10000 && element_count > 0 &&
            element_count <= 64 && allocation_count >= element_count && allocation_count <= 128) {
            out_handles.reserve((size_t)element_count);
            for (int i = 0; i < element_count; ++i) {
                std::uint32_t handle = 0;
                if (!read_u32(data_ptr + (uintptr_t)i * sizeof(std::uint32_t), handle) || !is_valid_handle(handle)) {
                    continue;
                }
                out_handles.push_back(handle);
            }
            if (!out_handles.empty()) {
                return true;
            }
        }

        // Some community code treats network handle vectors as inline count + handle buffer.
        int inline_count = 0;
        if (read_i32(vector_base, inline_count) && inline_count > 0 && inline_count <= 64) {
            out_handles.reserve((size_t)inline_count);
            for (int i = 0; i < inline_count; ++i) {
                std::uint32_t handle = 0;
                if (!read_u32(vector_base + 0x4 + (uintptr_t)i * sizeof(std::uint32_t), handle) || !is_valid_handle(handle)) {
                    continue;
                }
                out_handles.push_back(handle);
            }
        }

        return !out_handles.empty();
    }

    bool resolve_pawn_from_owner_handle(uintptr_t entity_list, std::uint32_t owner_handle, uintptr_t& out_pawn) const {
        out_pawn = 0;
        if (!is_valid_handle(owner_handle)) {
            return false;
        }

        uintptr_t owner_entity = 0;
        if (!resolve_entity_from_handle(entity_list, owner_handle, owner_entity) || owner_entity == 0) {
            return false;
        }

        int owner_health = 0;
        if (offsets_.m_iHealth != 0 && read_i32(owner_entity + offsets_.m_iHealth, owner_health) && owner_health > 0) {
            out_pawn = owner_entity;
            return true;
        }

        if (offsets_.m_hPlayerPawn != 0) {
            uintptr_t owner_pawn_handle = 0;
            if (read_u64(owner_entity + offsets_.m_hPlayerPawn, owner_pawn_handle) && owner_pawn_handle != 0) {
                uintptr_t owner_pawn = 0;
                if (resolve_entity_from_handle(entity_list, (std::uint32_t)owner_pawn_handle, owner_pawn) && owner_pawn != 0) {
                    out_pawn = owner_pawn;
                    return true;
                }
            }
        }

        return false;
    }

    bool find_bomb_carrier_pawn(uintptr_t entity_list, uintptr_t& out_pawn) const {
        constexpr std::uint16_t kC4WeaponIndex = 49;
        constexpr std::uint32_t kMaxEntityIndex = 2048;

        out_pawn = 0;
        if (entity_list == 0 || offsets_.m_hOwnerEntity == 0) {
            return false;
        }

        for (std::uint32_t index = 65; index <= kMaxEntityIndex; ++index) {
            uintptr_t entity = 0;
            if (!resolve_entity_from_index(entity_list, index, entity) || entity == 0) {
                continue;
            }

            std::uint16_t weapon_index = 0;
            if (!read_weapon_index_from_entity(entity, weapon_index) || weapon_index != kC4WeaponIndex) {
                continue;
            }

            std::uint32_t owner_handle = 0;
            if (!read_u32(entity + offsets_.m_hOwnerEntity, owner_handle) || !is_valid_handle(owner_handle)) {
                continue;
            }

            if (resolve_pawn_from_owner_handle(entity_list, owner_handle, out_pawn) && out_pawn != 0) {
                return true;
            }
        }

        return false;
    }

    bool pawn_has_bomb(uintptr_t entity_list, uintptr_t pawn) const {
        constexpr std::uint16_t kC4WeaponIndex = 49;

        uintptr_t bomb_carrier_pawn = 0;
        if (find_bomb_carrier_pawn(entity_list, bomb_carrier_pawn)) {
            return bomb_carrier_pawn == pawn;
        }

        if (entity_list != 0 && pawn != 0 && offsets_.m_pWeaponServices != 0 && offsets_.m_hMyWeapons != 0 &&
            offsets_.m_AttributeManager != 0 && offsets_.m_Item != 0 && offsets_.m_iItemDefinitionIndex != 0) {
            uintptr_t weapon_services = 0;
            if (read_u64(pawn + offsets_.m_pWeaponServices, weapon_services) && weapon_services != 0) {
                std::vector<std::uint32_t> weapon_handles{};
                if (read_weapon_handle_vector(weapon_services + offsets_.m_hMyWeapons, weapon_handles)) {
                    for (std::uint32_t weapon_handle : weapon_handles) {
                        std::uint16_t weapon_index = 0;
                        if (read_weapon_index_from_handle(entity_list, weapon_handle, weapon_index) &&
                            weapon_index == kC4WeaponIndex) {
                            return true;
                        }
                    }
                }

                std::uint32_t active_weapon_handle = 0;
                if (offsets_.m_hActiveWeapon != 0 &&
                    read_u32(weapon_services + offsets_.m_hActiveWeapon, active_weapon_handle) &&
                    is_valid_handle(active_weapon_handle)) {
                    std::uint16_t weapon_index = 0;
                    if (read_weapon_index_from_handle(entity_list, active_weapon_handle, weapon_index) &&
                        weapon_index == kC4WeaponIndex) {
                        return true;
                    }
                }
            }
        }

        std::uint16_t active_weapon_index = 0;
        return read_weapon_index(entity_list, pawn, active_weapon_index) && active_weapon_index == kC4WeaponIndex;
    }

    bool read_bomb_info(float& out_remaining_time, int& out_bomb_site) const {
        out_remaining_time = 0.0f;
        out_bomb_site = -1;
        if (client_base_ == 0 || offsets_.dwGlobalVars == 0 || offsets_.m_bBombTicking == 0 || offsets_.m_flC4Blow == 0) {
            return false;
        }

        if (offsets_.dwGameRules != 0 && offsets_.m_bBombPlanted != 0) {
            uintptr_t game_rules = 0;
            if (!read_u64(client_base_ + offsets_.dwGameRules, game_rules) || game_rules < 0x10000) {
                game_rules = client_base_ + offsets_.dwGameRules;
            }

            std::uint8_t bomb_planted = 0;
            if (read_u8(game_rules + offsets_.m_bBombPlanted, bomb_planted) && bomb_planted == 0) {
                cached_bomb_entity_ = 0;
                cached_bomb_blow_time_ = 0.0f;
                cached_bomb_remaining_base_ = 0.0f;
                cached_bomb_timer_length_ = 0.0f;
                return false;
            }
        }

        uintptr_t global_vars = 0;
        if (!read_u64(client_base_ + offsets_.dwGlobalVars, global_vars) || global_vars < 0x10000) {
            global_vars = client_base_ + offsets_.dwGlobalVars;
        }

        std::array<float, 4> current_time_candidates = {};
        static constexpr std::array<uintptr_t, 4> kCurTimeOffsets = {0x30, 0x34, 0x38, 0x2C};
        for (size_t i = 0; i < kCurTimeOffsets.size(); ++i) {
            float current_time = 0.0f;
            if (read_f32(global_vars + kCurTimeOffsets[i], current_time) && std::isfinite(current_time) && current_time >= 0.0f) {
                current_time_candidates[i] = current_time;
            }
        }

        const auto try_bomb_entity = [&](uintptr_t bomb_entity, float& out_remaining, int& out_site) {
            if (bomb_entity < 0x10000) {
                return false;
            }

            std::uint8_t bomb_ticking = 0;
            if (!read_u8(bomb_entity + offsets_.m_bBombTicking, bomb_ticking) || bomb_ticking == 0) {
                return false;
            }

            if (offsets_.m_bHasExploded != 0) {
                std::uint8_t bomb_exploded = 0;
                if (read_u8(bomb_entity + offsets_.m_bHasExploded, bomb_exploded) && bomb_exploded != 0) {
                    return false;
                }
            }

            float blow_time = 0.0f;
            if (!read_f32(bomb_entity + offsets_.m_flC4Blow, blow_time) || !std::isfinite(blow_time) || blow_time <= 0.0f) {
                return false;
            }

            if (offsets_.m_nBombSite != 0) {
                read_i32(bomb_entity + offsets_.m_nBombSite, out_site);
            }

            const auto now = std::chrono::steady_clock::now();
            float timer_length = 40.0f;
            if (offsets_.m_flTimerLength != 0) {
                float raw_timer_length = 0.0f;
                if (read_f32(bomb_entity + offsets_.m_flTimerLength, raw_timer_length) && std::isfinite(raw_timer_length) &&
                    raw_timer_length > 0.0f && raw_timer_length <= 120.0f) {
                    timer_length = raw_timer_length;
                }
            }

            float best_remaining = -1.0f;
            for (float current_time : current_time_candidates) {
                if (current_time <= 0.0f) {
                    continue;
                }

                const float remaining = blow_time - current_time;
                if (remaining >= -0.25f && remaining <= (timer_length + 1.0f)) {
                    best_remaining = std::max(best_remaining, std::max(remaining, 0.0f));
                }
            }

            const bool bomb_changed = cached_bomb_entity_ != bomb_entity || std::fabs(cached_bomb_blow_time_ - blow_time) > 0.01f;
            if (bomb_changed) {
                cached_bomb_entity_ = bomb_entity;
                cached_bomb_blow_time_ = blow_time;
                cached_bomb_timer_length_ = timer_length;
                cached_bomb_seen_at_ = now;
                cached_bomb_remaining_base_ = timer_length;
            }

            const float fallback_remaining =
                std::max(cached_bomb_remaining_base_ - std::chrono::duration<float>(now - cached_bomb_seen_at_).count(), 0.0f);
            if (best_remaining > 0.25f || fallback_remaining <= 0.25f) {
                cached_bomb_remaining_base_ = best_remaining >= 0.0f ? best_remaining : timer_length;
                cached_bomb_seen_at_ = now;
            }

            out_remaining =
                std::max(cached_bomb_remaining_base_ - std::chrono::duration<float>(now - cached_bomb_seen_at_).count(), 0.0f);
            return true;
        };

        uintptr_t planted_c4 = 0;
        std::array<uintptr_t, 2> bomb_candidates = {};
        if (offsets_.dwPlantedC4 != 0) {
            if (!read_u64(client_base_ + offsets_.dwPlantedC4, planted_c4) || planted_c4 < 0x10000) {
                planted_c4 = 0;
            }
        }
        bomb_candidates[0] = planted_c4;
        bomb_candidates[1] = offsets_.dwPlantedC4 != 0 ? (client_base_ + offsets_.dwPlantedC4) : 0;

        for (uintptr_t bomb_entity : bomb_candidates) {
            if (!try_bomb_entity(bomb_entity, out_remaining_time, out_bomb_site)) {
                continue;
            }
            return true;
        }

        uintptr_t entity_list = 0;
        if (read_u64(client_base_ + offsets_.dwEntityList, entity_list) && entity_list != 0) {
            for (std::uint32_t index = 65; index <= 2048; ++index) {
                uintptr_t entity = 0;
                if (!resolve_entity_from_index(entity_list, index, entity) || entity == 0) {
                    continue;
                }

                if (!try_bomb_entity(entity, out_remaining_time, out_bomb_site)) {
                    continue;
                }
                return true;
            }
        }

        if (cached_bomb_entity_ != 0 && try_bomb_entity(cached_bomb_entity_, out_remaining_time, out_bomb_site)) {
            return true;
        }

        cached_bomb_entity_ = 0;
        cached_bomb_blow_time_ = 0.0f;
        cached_bomb_remaining_base_ = 0.0f;
        cached_bomb_timer_length_ = 0.0f;

        return false;
    }

private:
    template <typename T>
    bool read_value(uintptr_t address, T& out) const {
        if (process_handle_ == nullptr || address == 0) {
            return false;
        }
        SIZE_T bytes_read = 0;
        return ReadProcessMemory(process_handle_, reinterpret_cast<LPCVOID>(address), &out, sizeof(T), &bytes_read) !=
                   FALSE &&
               bytes_read == sizeof(T);
    }

    static DWORD find_process_id(const wchar_t* process_name) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return 0;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        DWORD pid = 0;
        if (Process32FirstW(snapshot, &entry) != FALSE) {
            do {
                if (_wcsicmp(entry.szExeFile, process_name) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry) != FALSE);
        }
        CloseHandle(snapshot);
        return pid;
    }

    static uintptr_t find_module_base(DWORD pid, const wchar_t* module_name) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return 0;
        }
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        uintptr_t base = 0;
        if (Module32FirstW(snapshot, &entry) != FALSE) {
            do {
                if (_wcsicmp(entry.szModule, module_name) == 0) {
                    base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                    break;
                }
            } while (Module32NextW(snapshot, &entry) != FALSE);
        }
        CloseHandle(snapshot);
        return base;
    }

    bool is_process_alive() const {
        if (process_handle_ == nullptr) {
            return false;
        }
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process_handle_, &exit_code) == FALSE) {
            return false;
        }
        return exit_code == STILL_ACTIVE;
    }

    void detach() {
        if (process_handle_ != nullptr) {
            CloseHandle(process_handle_);
            process_handle_ = nullptr;
        }
        pid_ = 0;
        client_base_ = 0;
        fps_initialized_ = false;
        fps_prev_frame_count_ = 0;
        smoothed_game_fps_ = 0.0f;
        cached_bomb_entity_ = 0;
        cached_bomb_blow_time_ = 0.0f;
        cached_bomb_remaining_base_ = 0.0f;
        cached_bomb_timer_length_ = 0.0f;
    }

    static std::optional<std::string> download_text_from_url(const char* url) {
        HINTERNET internet = InternetOpenA("timmy_menu", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (internet == nullptr) {
            return std::nullopt;
        }
        HINTERNET request =
            InternetOpenUrlA(internet, url, nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (request == nullptr) {
            InternetCloseHandle(internet);
            return std::nullopt;
        }
        std::string body;
        std::array<char, 8192> buffer{};
        DWORD bytes_read = 0;
        while (InternetReadFile(request, buffer.data(), (DWORD)buffer.size(), &bytes_read) != FALSE && bytes_read > 0) {
            body.append(buffer.data(), buffer.data() + bytes_read);
            bytes_read = 0;
        }
        InternetCloseHandle(request);
        InternetCloseHandle(internet);
        if (body.empty()) {
            return std::nullopt;
        }
        return body;
    }

    static std::optional<std::string> read_first_existing_file(const std::array<const char*, 5>& candidates) {
        for (const char* candidate : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec)) {
                continue;
            }
            std::ifstream stream(candidate, std::ios::binary);
            if (!stream) {
                continue;
            }
            std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (!content.empty()) {
                return content;
            }
        }
        return std::nullopt;
    }

    static bool extract_uint_value(const std::string& text, const char* key, uintptr_t& out_value, size_t from = 0,
                                   size_t to = std::string::npos) {
        if (to == std::string::npos || to > text.size()) {
            to = text.size();
        }
        const std::string needle = "\"" + std::string(key) + "\"";
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
            if (value_pos >= to) {
                return false;
            }

            size_t end = value_pos;
            if ((end + 1) < to && text[end] == '0' && (text[end + 1] == 'x' || text[end + 1] == 'X')) {
                end += 2;
                while (end < to && std::isxdigit(static_cast<unsigned char>(text[end])) != 0) {
                    ++end;
                }
            } else {
                while (end < to && std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
                    ++end;
                }
            }
            if (end == value_pos) {
                search = key_pos + needle.size();
                continue;
            }
            try {
                out_value = static_cast<uintptr_t>(std::stoull(text.substr(value_pos, end - value_pos), nullptr, 0));
                return true;
            } catch (...) {
                search = key_pos + needle.size();
            }
        }
        return false;
    }

    static bool find_object_range(const std::string& text, const char* object_name, size_t& out_begin,
                                  size_t& out_end, size_t from = 0, size_t to = std::string::npos) {
        if (to == std::string::npos || to > text.size()) {
            to = text.size();
        }
        const std::string needle = "\"" + std::string(object_name) + "\"";
        size_t search = from;
        while (search < to) {
            const size_t object_key = text.find(needle, search);
            if (object_key == std::string::npos || object_key >= to) {
                return false;
            }
            size_t value_pos = skip_ws(text, object_key + needle.size(), to);
            if (value_pos >= to || text[value_pos] != ':') {
                search = object_key + needle.size();
                continue;
            }
            value_pos = skip_ws(text, value_pos + 1, to);
            if (value_pos >= to || text[value_pos] != '{') {
                search = object_key + needle.size();
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

    static bool extract_class_field(const std::string& text, size_t classes_begin, size_t classes_end, const char* class_name,
                                    const char* field_name, uintptr_t& out_value) {
        size_t class_begin = 0;
        size_t class_end = 0;
        if (!find_object_range(text, class_name, class_begin, class_end, classes_begin, classes_end)) {
            return false;
        }
        size_t fields_begin = 0;
        size_t fields_end = 0;
        if (!find_object_range(text, "fields", fields_begin, fields_end, class_begin, class_end)) {
            return false;
        }
        return extract_uint_value(text, field_name, out_value, fields_begin, fields_end);
    }

    bool resolve_weapon_entity(uintptr_t entity_list, uintptr_t pawn, uintptr_t& out_weapon) const {
        out_weapon = 0;
        if (pawn == 0 || entity_list == 0) {
            return false;
        }

        if (offsets_.m_pClippingWeapon != 0 &&
            read_u64(pawn + offsets_.m_pClippingWeapon, out_weapon) &&
            out_weapon != 0) {
            return true;
        }

        if (offsets_.m_pWeaponServices == 0 || offsets_.m_hActiveWeapon == 0) {
            return false;
        }

        uintptr_t weapon_services = 0;
        if (!read_u64(pawn + offsets_.m_pWeaponServices, weapon_services) || weapon_services == 0) {
            return false;
        }

        std::uint32_t active_weapon_handle = 0;
        if (!read_u32(weapon_services + offsets_.m_hActiveWeapon, active_weapon_handle) || active_weapon_handle == 0) {
            return false;
        }

        return resolve_entity_from_handle(entity_list, active_weapon_handle, out_weapon);
    }

    bool resolve_entity_from_handle(uintptr_t entity_list, std::uint32_t handle, uintptr_t& out_entity) const {
        out_entity = 0;
        if (entity_list == 0 || handle == 0) {
            return false;
        }

        uintptr_t entity_bucket = 0;
        if (!read_u64(entity_list + 0x8 * ((handle & 0x7FFF) >> 0x9) + 0x10, entity_bucket) || entity_bucket == 0) {
            return false;
        }

        return read_u64(entity_bucket + 0x70 * (handle & 0x1FF), out_entity) && out_entity != 0;
    }

    bool resolve_entity_from_index(uintptr_t entity_list, std::uint32_t index, uintptr_t& out_entity) const {
        out_entity = 0;
        if (entity_list == 0 || index == 0) {
            return false;
        }

        uintptr_t entity_bucket = 0;
        if (!read_u64(entity_list + 0x8 * (index >> 0x9) + 0x10, entity_bucket) || entity_bucket == 0) {
            return false;
        }

        return read_u64(entity_bucket + 0x70 * (index & 0x1FF), out_entity) && out_entity != 0;
    }

    bool try_parse_bone_ids(const std::string& text) {
        size_t begin = 0;
        size_t end = 0;
        if (!find_object_range(text, "bone_ids", begin, end)) {
            return false;
        }

        BoneConfig parsed = bone_ids_;
        uintptr_t v = 0;
        bool found_any = false;
        if (extract_uint_value(text, "head", v, begin, end)) {
            parsed.ids[0] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "neck", v, begin, end)) {
            parsed.ids[1] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "spine", v, begin, end)) {
            parsed.ids[2] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "pelvis", v, begin, end)) {
            parsed.ids[3] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "left_shoulder", v, begin, end)) {
            parsed.ids[4] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "left_elbow", v, begin, end)) {
            parsed.ids[5] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "left_wrist", v, begin, end)) {
            parsed.ids[6] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "right_shoulder", v, begin, end)) {
            parsed.ids[7] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "right_elbow", v, begin, end)) {
            parsed.ids[8] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "right_wrist", v, begin, end)) {
            parsed.ids[9] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "left_hip", v, begin, end)) {
            parsed.ids[10] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "left_knee", v, begin, end)) {
            parsed.ids[11] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "left_ankle", v, begin, end)) {
            parsed.ids[12] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "right_hip", v, begin, end)) {
            parsed.ids[13] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "right_knee", v, begin, end)) {
            parsed.ids[14] = (int)v;
            found_any = true;
        }
        if (extract_uint_value(text, "right_ankle", v, begin, end)) {
            parsed.ids[15] = (int)v;
            found_any = true;
        }
        if (found_any) {
            bone_ids_ = parsed;
        }
        return found_any;
    }

    bool load_offsets() {
        constexpr const char* kOffsetsUrl =
            "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.json";
        constexpr const char* kClientDllUrl =
            "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.json";

        const std::array<const char*, 5> offsets_candidates = {
            "CS2-GFusion-Python-main/offsets/offsets.json",
            "../CS2-GFusion-Python-main/offsets/offsets.json",
            "../../CS2-GFusion-Python-main/offsets/offsets.json",
            "../../../CS2-GFusion-Python-main/offsets/offsets.json",
            "../../../../CS2-GFusion-Python-main/offsets/offsets.json",
        };
        const std::array<const char*, 5> client_candidates = {
            "CS2-GFusion-Python-main/offsets/client_dll.json",
            "../CS2-GFusion-Python-main/offsets/client_dll.json",
            "../../CS2-GFusion-Python-main/offsets/client_dll.json",
            "../../../CS2-GFusion-Python-main/offsets/client_dll.json",
            "../../../../CS2-GFusion-Python-main/offsets/client_dll.json",
        };

        std::optional<std::string> offsets_text = download_text_from_url(kOffsetsUrl);
        std::optional<std::string> client_text = download_text_from_url(kClientDllUrl);
        if (!offsets_text.has_value()) {
            offsets_text = read_first_existing_file(offsets_candidates);
        }
        if (!client_text.has_value()) {
            client_text = read_first_existing_file(client_candidates);
        }

        if (offsets_text.has_value()) {
            extract_uint_value(*offsets_text, "dwEntityList", offsets_.dwEntityList);
            extract_uint_value(*offsets_text, "dwLocalPlayerPawn", offsets_.dwLocalPlayerPawn);
            extract_uint_value(*offsets_text, "dwViewMatrix", offsets_.dwViewMatrix);
            extract_uint_value(*offsets_text, "dwGlobalVars", offsets_.dwGlobalVars);
            extract_uint_value(*offsets_text, "dwGameRules", offsets_.dwGameRules);
            extract_uint_value(*offsets_text, "dwPlantedC4", offsets_.dwPlantedC4);
            try_parse_bone_ids(*offsets_text);
        }
        if (client_text.has_value()) {
            size_t client_root_begin = 0;
            size_t client_root_end = 0;
            if (find_object_range(*client_text, "client.dll", client_root_begin, client_root_end)) {
                size_t classes_begin = 0;
                size_t classes_end = 0;
                if (find_object_range(*client_text, "classes", classes_begin, classes_end, client_root_begin,
                                      client_root_end)) {
                    extract_class_field(*client_text, classes_begin, classes_end, "C_BaseEntity", "m_iTeamNum",
                                        offsets_.m_iTeamNum);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_BaseEntity", "m_lifeState",
                                        offsets_.m_lifeState);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_BaseEntity", "m_pGameSceneNode",
                                        offsets_.m_pGameSceneNode);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_BaseEntity", "m_hOwnerEntity",
                                        offsets_.m_hOwnerEntity);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_BaseEntity", "m_iHealth",
                                        offsets_.m_iHealth);
                    if (!extract_class_field(*client_text, classes_begin, classes_end, "C_CSPlayerPawn",
                                             "m_ArmorValue", offsets_.m_ArmorValue)) {
                        if (!extract_class_field(*client_text, classes_begin, classes_end, "C_CSPlayerPawnBase",
                                                 "m_ArmorValue", offsets_.m_ArmorValue)) {
                            extract_class_field(*client_text, classes_begin, classes_end, "C_BasePlayerPawn",
                                                "m_ArmorValue", offsets_.m_ArmorValue);
                        }
                    }
                    extract_class_field(*client_text, classes_begin, classes_end, "CSkeletonInstance", "m_modelState",
                                        offsets_.m_modelState);
                    extract_class_field(*client_text, classes_begin, classes_end, "CCSPlayerController", "m_hPlayerPawn",
                                        offsets_.m_hPlayerPawn);
                    if (!extract_class_field(*client_text, classes_begin, classes_end, "C_CSPlayerPawn",
                                             "m_pClippingWeapon", offsets_.m_pClippingWeapon)) {
                        extract_class_field(*client_text, classes_begin, classes_end, "C_CSPlayerPawnBase",
                                            "m_pClippingWeapon", offsets_.m_pClippingWeapon);
                    }
                    if (!extract_class_field(*client_text, classes_begin, classes_end, "C_BasePlayerPawn",
                                             "m_pWeaponServices", offsets_.m_pWeaponServices)) {
                        extract_class_field(*client_text, classes_begin, classes_end, "C_CSPlayerPawnBase",
                                            "m_pWeaponServices", offsets_.m_pWeaponServices);
                    }
                    extract_class_field(*client_text, classes_begin, classes_end, "CPlayer_WeaponServices",
                                        "m_hActiveWeapon", offsets_.m_hActiveWeapon);
                    extract_class_field(*client_text, classes_begin, classes_end, "CPlayer_WeaponServices",
                                        "m_hMyWeapons", offsets_.m_hMyWeapons);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_EconEntity", "m_AttributeManager",
                                        offsets_.m_AttributeManager);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_AttributeContainer", "m_Item",
                                        offsets_.m_Item);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_PlantedC4", "m_bBombTicking",
                                        offsets_.m_bBombTicking);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_PlantedC4", "m_bHasExploded",
                                        offsets_.m_bHasExploded);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_PlantedC4", "m_flC4Blow",
                                        offsets_.m_flC4Blow);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_PlantedC4", "m_flTimerLength",
                                        offsets_.m_flTimerLength);
                    extract_class_field(*client_text, classes_begin, classes_end, "C_PlantedC4", "m_nBombSite",
                                        offsets_.m_nBombSite);
                    if (!extract_class_field(*client_text, classes_begin, classes_end, "C_CSGameRules", "m_bBombPlanted",
                                             offsets_.m_bBombPlanted)) {
                        extract_class_field(*client_text, classes_begin, classes_end, "C_CSGameRulesProxy", "m_bBombPlanted",
                                            offsets_.m_bBombPlanted);
                    }
                    if (!extract_class_field(*client_text, classes_begin, classes_end, "C_EconItemView",
                                             "m_iItemDefinitionIndex", offsets_.m_iItemDefinitionIndex)) {
                        extract_class_field(*client_text, classes_begin, classes_end, "CEconItemView",
                                            "m_iItemDefinitionIndex", offsets_.m_iItemDefinitionIndex);
                    }
                }
            }
            try_parse_bone_ids(*client_text);
        }
        return offsets_.valid();
    }

    HANDLE process_handle_ = nullptr;
    DWORD pid_ = 0;
    uintptr_t client_base_ = 0;
    EspOffsets offsets_{};
    BoneConfig bone_ids_{};
    bool offsets_loaded_ = false;
    std::chrono::steady_clock::time_point next_attach_attempt_{};
    mutable bool fps_initialized_ = false;
    mutable int fps_prev_frame_count_ = 0;
    mutable std::chrono::steady_clock::time_point fps_prev_time_{};
    mutable float smoothed_game_fps_ = 0.0f;
    mutable uintptr_t cached_bomb_entity_ = 0;
    mutable float cached_bomb_blow_time_ = 0.0f;
    mutable float cached_bomb_remaining_base_ = 0.0f;
    mutable float cached_bomb_timer_length_ = 0.0f;
    mutable std::chrono::steady_clock::time_point cached_bomb_seen_at_{};
#else
    bool ensure_ready() {
        return false;
    }
    bool read_view_matrix(std::array<float, 16>&) const {
        return false;
    }
    bool read_u64(uintptr_t, uintptr_t&) const {
        return false;
    }
    bool read_i32(uintptr_t, int&) const {
        return false;
    }
    bool read_u32(uintptr_t, std::uint32_t&) const {
        return false;
    }
    bool read_u16(uintptr_t, std::uint16_t&) const {
        return false;
    }
    bool read_f32(uintptr_t, float&) const {
        return false;
    }
    bool read_vec3(uintptr_t, Vec3&) const {
        return false;
    }
    uintptr_t client_base() const {
        return 0;
    }
    const EspOffsets& offsets() const {
        static EspOffsets dummy{};
        return dummy;
    }
    const BoneConfig& bone_ids() const {
        static BoneConfig dummy{};
        return dummy;
    }
    bool read_game_avg_fps(float&) const {
        return false;
    }
    bool read_weapon_index(uintptr_t, uintptr_t, std::uint16_t&) const {
        return false;
    }
#endif
};

EspRuntime& runtime_instance() {
    static EspRuntime runtime;
    return runtime;
}

ImFont* g_esp_weapon_font = nullptr;
ImFont* g_esp_weapon_icon_font = nullptr;

} // namespace

bool initialize_esp_weapon_icons(ID3D11Device* device) {
#ifdef _WIN32
    if (device == nullptr) {
        return false;
    }
    if (g_weapon_icon_device == device && g_weapon_icon_wic_factory != nullptr) {
        return true;
    }
    release_weapon_icon_cache();
    if (g_weapon_icon_device != nullptr) {
        g_weapon_icon_device->Release();
        g_weapon_icon_device = nullptr;
    }
    if (g_weapon_icon_wic_factory != nullptr) {
        g_weapon_icon_wic_factory->Release();
        g_weapon_icon_wic_factory = nullptr;
    }
    g_weapon_icon_device = device;
    g_weapon_icon_device->AddRef();
    return ensure_weapon_icon_wic_factory();
#else
    (void)device;
    return false;
#endif
}

void shutdown_esp_weapon_icons() {
#ifdef _WIN32
    release_weapon_icon_cache();
    if (g_weapon_icon_wic_factory != nullptr) {
        g_weapon_icon_wic_factory->Release();
        g_weapon_icon_wic_factory = nullptr;
    }
    if (g_weapon_icon_device != nullptr) {
        g_weapon_icon_device->Release();
        g_weapon_icon_device = nullptr;
    }
    if (g_weapon_icon_com_initialized) {
        CoUninitialize();
        g_weapon_icon_com_initialized = false;
    }
#endif
}

void set_esp_weapon_font(ImFont* font) {
    g_esp_weapon_font = font;
}

void set_esp_weapon_icon_font(ImFont* font) {
    g_esp_weapon_icon_font = font;
}

ImFont* get_esp_weapon_font() {
    return g_esp_weapon_font;
}

ImFont* get_esp_weapon_icon_font() {
    return g_esp_weapon_icon_font;
}

const char* get_esp_preview_weapon_icon() {
    return weapon_icon_from_model_name("ak47");
}

const char* get_esp_preview_bomb_icon() {
    return weapon_icon_from_model_name("c4");
}

void draw_esp_overlay(ImDrawList* draw_list, const ui::State& state, int screen_width, int screen_height,
                      bool overlay_mode) {
    const bool draw_player_esp = state.esp_box || state.esp_hp_bar || state.esp_armor || state.esp_head || state.esp_skeleton ||
                                 state.esp_weapon || state.esp_bomb;
    const bool draw_bomb_info = state.bomb_info_enabled;
    if (draw_list == nullptr || !overlay_mode || (!draw_player_esp && !draw_bomb_info) || screen_width <= 0 ||
        screen_height <= 0) {
        return;
    }
#ifdef _WIN32
    EspRuntime& runtime = runtime_instance();
    if (!runtime.ensure_ready()) {
        return;
    }

    const EspOffsets& o = runtime.offsets();
    if (draw_bomb_info) {
        float bomb_remaining_time = 0.0f;
        int bomb_site = -1;
        if (runtime.read_bomb_info(bomb_remaining_time, bomb_site)) {
            const char* bomb_icon = weapon_icon_from_model_name("c4");
            char bomb_text[32] = {};
            if (bomb_site >= 0 && bomb_site != 1) {
                std::snprintf(bomb_text, sizeof(bomb_text), "A %.1fs", bomb_remaining_time);
            } else if (bomb_site == 1) {
                std::snprintf(bomb_text, sizeof(bomb_text), "B %.1fs", bomb_remaining_time);
            } else {
                std::snprintf(bomb_text, sizeof(bomb_text), "%.1fs", bomb_remaining_time);
            }

            ImFont* text_font = g_esp_weapon_font != nullptr ? g_esp_weapon_font : ImGui::GetFont();
            const float text_font_size = g_esp_weapon_font != nullptr ? 18.0f : (ImGui::GetFontSize() + 2.0f);
            const ImVec2 text_size = text_font->CalcTextSizeA(text_font_size, FLT_MAX, 0.0f, bomb_text);
            const float icon_font_size = 20.0f;
            const float icon_width = (bomb_icon != nullptr && g_esp_weapon_icon_font != nullptr)
                                         ? g_esp_weapon_icon_font->CalcTextSizeA(icon_font_size, FLT_MAX, 0.0f, bomb_icon).x
                                         : 16.0f;
            const float box_width = icon_width + text_size.x + 28.0f;
            const float box_height = std::max(text_size.y, 18.0f) + 14.0f;
            const float box_x = snap_pixel(screen_width * 0.5f - box_width * 0.5f);
            const float box_y = 24.0f;
            const ImVec2 box_min(box_x, box_y);
            const ImVec2 box_max(box_x + box_width, box_y + box_height);
            const ImU32 panel_fill = IM_COL32(16, 18, 24, 215);
            const ImU32 panel_border = IM_COL32(255, 110, 105, 235);
            const ImU32 shadow = IM_COL32(0, 0, 0, 255);

            draw_list->AddRectFilled(box_min, box_max, panel_fill, 0.0f);
            draw_list->AddRect(box_min, box_max, panel_border, 0.0f, 0, 1.0f);

            float cursor_x = box_min.x + 8.0f;
            if (bomb_icon != nullptr && g_esp_weapon_icon_font != nullptr) {
                const ImVec2 icon_pos(cursor_x, snap_pixel(box_min.y + (box_height - icon_font_size) * 0.5f - 1.0f));
                draw_list->AddText(g_esp_weapon_icon_font, icon_font_size, ImVec2(icon_pos.x + 1.0f, icon_pos.y + 1.0f), shadow,
                                   bomb_icon);
                draw_list->AddText(g_esp_weapon_icon_font, icon_font_size, icon_pos, panel_border, bomb_icon);
                cursor_x += icon_width + 8.0f;
            }

            const ImVec2 text_pos(snap_pixel(cursor_x), snap_pixel(box_min.y + (box_height - text_size.y) * 0.5f - 1.0f));
            draw_list->AddText(text_font, text_font_size, ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f), shadow, bomb_text);
            draw_list->AddText(text_font, text_font_size, text_pos, IM_COL32(245, 245, 245, 245), bomb_text);
        }
    }

    if (!draw_player_esp) {
        return;
    }

    std::array<float, 16> view_matrix{};
    if (!runtime.read_view_matrix(view_matrix)) {
        return;
    }

    uintptr_t local_player_pawn = 0;
    if (!runtime.read_u64(runtime.client_base() + o.dwLocalPlayerPawn, local_player_pawn) || local_player_pawn == 0) {
        return;
    }

    int local_player_team = 0;
    if (!runtime.read_i32(local_player_pawn + o.m_iTeamNum, local_player_team) || local_player_team == 0) {
        return;
    }

    uintptr_t entity_list = 0;
    if (!runtime.read_u64(runtime.client_base() + o.dwEntityList, entity_list) || entity_list == 0) {
        return;
    }

    uintptr_t entity_ptr = 0;
    if (!runtime.read_u64(entity_list + 0x10, entity_ptr) || entity_ptr == 0) {
        return;
    }

    uintptr_t bomb_carrier_pawn = 0;
    if (state.esp_bomb) {
        runtime.find_bomb_carrier_pawn(entity_list, bomb_carrier_pawn);
    }

    const std::array<int, 16>& kBoneIds = runtime.bone_ids().ids;
    static constexpr std::array<std::pair<int, int>, 15> kBoneConnections = {{
        {0, 1},   {1, 2},   {2, 3},   {3, 10},  {10, 11}, {11, 12}, {3, 13}, {13, 14},
        {14, 15}, {1, 4},   {4, 5},   {5, 6},   {1, 7},   {7, 8},   {8, 9},
    }};

    for (int i = 1; i <= 64; ++i) {
        uintptr_t entity_controller = 0;
        if (!runtime.read_u64(entity_ptr + 0x70 * (i & 0x1FF), entity_controller) || entity_controller == 0) {
            continue;
        }

        uintptr_t entity_controller_pawn = 0;
        if (!runtime.read_u64(entity_controller + o.m_hPlayerPawn, entity_controller_pawn) ||
            entity_controller_pawn == 0) {
            continue;
        }

        uintptr_t entity_list_pawn = 0;
        if (!runtime.read_u64(entity_list + 0x8 * ((entity_controller_pawn & 0x7FFF) >> 0x9) + 0x10, entity_list_pawn) ||
            entity_list_pawn == 0) {
            continue;
        }

        uintptr_t entity_pawn_addr = 0;
        if (!runtime.read_u64(entity_list_pawn + 0x70 * (entity_controller_pawn & 0x1FF), entity_pawn_addr) ||
            entity_pawn_addr == 0 || entity_pawn_addr == local_player_pawn) {
            continue;
        }

        int entity_hp = 0;
        if (!runtime.read_i32(entity_pawn_addr + o.m_iHealth, entity_hp) || entity_hp <= 0) {
            continue;
        }
        int entity_armor = 0;
        if (state.esp_armor && o.m_ArmorValue != 0) {
            runtime.read_i32(entity_pawn_addr + o.m_ArmorValue, entity_armor);
            entity_armor = std::clamp(entity_armor, 0, 100);
        }

        int entity_life = 0;
        if (!runtime.read_i32(entity_pawn_addr + o.m_lifeState, entity_life) || entity_life != 256) {
            continue;
        }

        int entity_team = 0;
        if (!runtime.read_i32(entity_pawn_addr + o.m_iTeamNum, entity_team)) {
            continue;
        }
        const bool is_ally = entity_team == local_player_team;
        if (is_ally && !state.esp_team) {
            continue;
        }

        uintptr_t game_scene = 0;
        if (!runtime.read_u64(entity_pawn_addr + o.m_pGameSceneNode, game_scene) || game_scene == 0) {
            continue;
        }

        uintptr_t bone_matrix = 0;
        if (!runtime.read_u64(game_scene + o.m_modelState + 0x80, bone_matrix) || bone_matrix == 0) {
            continue;
        }

        const int head_bone = kBoneIds[0];
        const int neck_bone = kBoneIds[1];
        const int left_ankle_bone = kBoneIds[12];
        const int right_ankle_bone = kBoneIds[15];
        if (head_bone < 0 || neck_bone < 0 || left_ankle_bone < 0 || right_ankle_bone < 0) {
            continue;
        }
        Vec3 head_world{};
        Vec3 neck_world{};
        Vec3 left_ankle_world{};
        Vec3 right_ankle_world{};
        if (!runtime.read_vec3(bone_matrix + (uintptr_t)head_bone * 0x20, head_world) ||
            !runtime.read_vec3(bone_matrix + (uintptr_t)neck_bone * 0x20, neck_world) ||
            !runtime.read_vec3(bone_matrix + (uintptr_t)left_ankle_bone * 0x20, left_ankle_world) ||
            !runtime.read_vec3(bone_matrix + (uintptr_t)right_ankle_bone * 0x20, right_ankle_world)) {
            continue;
        }
        Vec3 head_top_world = head_world;
        head_top_world.z += 8.0f;

        Vec2 head_pos{};
        Vec2 neck_pos{};
        Vec2 left_ankle_pos{};
        Vec2 right_ankle_pos{};
        if (!world_to_screen(view_matrix, head_top_world.x, head_top_world.y, head_top_world.z, (float)screen_width,
                             (float)screen_height, head_pos)) {
            continue;
        }
        const bool neck_projected =
            world_to_screen(view_matrix, neck_world.x, neck_world.y, neck_world.z, (float)screen_width,
                            (float)screen_height, neck_pos);
        Vec2 leg_pos{};
        bool leg_projected =
            world_to_screen(view_matrix, left_ankle_world.x, left_ankle_world.y, left_ankle_world.z,
                            (float)screen_width, (float)screen_height, left_ankle_pos) &&
            world_to_screen(view_matrix, right_ankle_world.x, right_ankle_world.y, right_ankle_world.z,
                            (float)screen_width, (float)screen_height, right_ankle_pos);
        if (leg_projected) {
            leg_pos.x = (left_ankle_pos.x + right_ankle_pos.x) * 0.5f;
            leg_pos.y = std::max(left_ankle_pos.y, right_ankle_pos.y);
        } else {
            float fallback_leg_z = 0.0f;
            if (!runtime.read_f32(bone_matrix + 28 * 0x20 + 0x8, fallback_leg_z) ||
                !world_to_screen(view_matrix, head_world.x, head_world.y, fallback_leg_z, (float)screen_width,
                                 (float)screen_height, leg_pos)) {
                continue;
            }
        }

        if (head_pos.y < -10.0f || head_pos.x < -100.0f || head_pos.x > (float)screen_width + 100.0f) {
            continue;
        }

        const float delta_z = leg_pos.y - head_pos.y;
        if (delta_z < 2.0f) {
            continue;
        }
        const float box_center_x = (head_pos.x + leg_pos.x) * 0.5f;
        const float left_x = box_center_x - delta_z / 4.0f;
        const float right_x = box_center_x + delta_z / 4.0f;
        const float top_y = head_pos.y;
        const float bottom_y = leg_pos.y;
        if (state.esp_box) {
            const ImU32 box_color = color_u32_from_array(state.esp_box_color);
            draw_list->AddRect(ImVec2(snap_pixel(left_x), snap_pixel(top_y)), ImVec2(snap_pixel(right_x), snap_pixel(bottom_y)),
                               box_color, 0.0f, 0, 1.0f);
        }

        if (state.esp_head && neck_projected) {
            const float head_diameter = std::max(0.0f, neck_pos.y - head_pos.y);
            const float head_radius = std::clamp(head_diameter * 0.5f, 3.5f, 18.0f);
            const ImVec2 head_circle_center(snap_pixel(head_pos.x), snap_pixel(head_pos.y + head_radius));
            draw_list->AddCircle(head_circle_center, head_radius, IM_COL32(245, 245, 245, 245), 24, 1.6f);
        }

        if (state.esp_hp_bar) {
            const float hp_ratio = std::clamp((float)entity_hp / 100.0f, 0.0f, 1.0f);
            const float hp_bar_x = left_x - 3.0f;
            const float hp_top = top_y;
            const float hp_height = delta_z;
            const float hp_start = hp_top + hp_height * (1.0f - hp_ratio);
            const ImU32 hp_bg = IM_COL32(12, 12, 12, 230);
            const ImU32 hp_fg = color_u32_from_array(state.esp_hp_color);
            draw_list->AddLine(ImVec2(std::round(hp_bar_x), std::round(hp_top)),
                               ImVec2(std::round(hp_bar_x), std::round(hp_top + hp_height)), hp_bg, 3.0f);
            draw_list->AddLine(ImVec2(std::round(hp_bar_x), std::round(hp_start)),
                               ImVec2(std::round(hp_bar_x), std::round(hp_top + hp_height)), hp_fg, 3.0f);

            char hp_text[8] = {};
            std::snprintf(hp_text, sizeof(hp_text), "%d", entity_hp);
            ImFont* hp_font = g_esp_weapon_font != nullptr ? g_esp_weapon_font : ImGui::GetFont();
            const float hp_font_size = g_esp_weapon_font != nullptr ? 15.0f : ImGui::GetFontSize();
            const ImVec2 hp_text_size = hp_font->CalcTextSizeA(hp_font_size, FLT_MAX, 0.0f, hp_text);
            const ImVec2 hp_text_pos(snap_pixel(hp_bar_x - hp_text_size.x * 0.5f),
                                     snap_pixel(std::max(0.0f, hp_top - hp_text_size.y - 3.0f)));
            const ImU32 hp_text_shadow = IM_COL32(0, 0, 0, 255);
            const ImU32 hp_text_color = IM_COL32(245, 245, 245, 245);
            draw_list->AddText(hp_font, hp_font_size, ImVec2(hp_text_pos.x - 1.0f, hp_text_pos.y), hp_text_shadow, hp_text);
            draw_list->AddText(hp_font, hp_font_size, ImVec2(hp_text_pos.x + 1.0f, hp_text_pos.y), hp_text_shadow, hp_text);
            draw_list->AddText(hp_font, hp_font_size, ImVec2(hp_text_pos.x, hp_text_pos.y - 1.0f), hp_text_shadow, hp_text);
            draw_list->AddText(hp_font, hp_font_size, ImVec2(hp_text_pos.x, hp_text_pos.y + 1.0f), hp_text_shadow, hp_text);
            draw_list->AddText(hp_font, hp_font_size, hp_text_pos, hp_text_color, hp_text);
        }

        if (state.esp_armor && o.m_ArmorValue != 0) {
            const float armor_ratio = std::clamp((float)entity_armor / 100.0f, 0.0f, 1.0f);
            const float armor_bar_x = left_x - (state.esp_hp_bar ? 7.0f : 3.0f);
            const float armor_top = top_y;
            const float armor_height = delta_z;
            const float armor_start = armor_top + armor_height * (1.0f - armor_ratio);
            const ImU32 armor_bg = IM_COL32(12, 12, 12, 230);
            const ImU32 armor_fg = color_u32_from_array(state.esp_armor_color);
            draw_list->AddLine(ImVec2(std::round(armor_bar_x), std::round(armor_top)),
                               ImVec2(std::round(armor_bar_x), std::round(armor_top + armor_height)), armor_bg, 3.0f);
            draw_list->AddLine(ImVec2(std::round(armor_bar_x), std::round(armor_start)),
                               ImVec2(std::round(armor_bar_x), std::round(armor_top + armor_height)), armor_fg, 3.0f);
        }

        std::uint16_t weapon_index = 0;
        const bool has_weapon_index =
            (state.esp_weapon || state.esp_bomb) && runtime.read_weapon_index(entity_list, entity_pawn_addr, weapon_index) &&
            weapon_index != 0;
        const bool has_bomb =
            state.esp_bomb && ((bomb_carrier_pawn != 0 && bomb_carrier_pawn == entity_pawn_addr) ||
                               (bomb_carrier_pawn == 0 && runtime.pawn_has_bomb(entity_list, entity_pawn_addr)));

        if (state.esp_bomb) {
            const char* bomb_icon = weapon_icon_from_model_name("c4");
            if (has_bomb && bomb_icon != nullptr && g_esp_weapon_icon_font != nullptr) {
                const float bomb_font_size = std::clamp(delta_z * 0.12f, 18.0f, 28.0f);
                const ImVec2 bomb_pos(snap_pixel(right_x + 4.0f), snap_pixel(top_y - 2.0f));
                const ImU32 shadow = IM_COL32(0, 0, 0, 255);
                const ImU32 bomb_color = IM_COL32(255, 110, 105, 245);
                draw_list->AddText(g_esp_weapon_icon_font, bomb_font_size, ImVec2(bomb_pos.x - 1.0f, bomb_pos.y), shadow,
                                   bomb_icon);
                draw_list->AddText(g_esp_weapon_icon_font, bomb_font_size, ImVec2(bomb_pos.x + 1.0f, bomb_pos.y), shadow,
                                   bomb_icon);
                draw_list->AddText(g_esp_weapon_icon_font, bomb_font_size, ImVec2(bomb_pos.x, bomb_pos.y - 1.0f), shadow,
                                   bomb_icon);
                draw_list->AddText(g_esp_weapon_icon_font, bomb_font_size, ImVec2(bomb_pos.x, bomb_pos.y + 1.0f), shadow,
                                   bomb_icon);
                draw_list->AddText(g_esp_weapon_icon_font, bomb_font_size, bomb_pos, bomb_color, bomb_icon);
            }
        }

        if (state.esp_weapon) {
            if (has_weapon_index) {
                const char* weapon_model = weapon_model_from_index(weapon_index);
                const char* weapon_icon = weapon_icon_from_model_name(weapon_model);
                if (weapon_icon != nullptr && g_esp_weapon_icon_font != nullptr) {
                    const float icon_font_size = std::clamp(delta_z * 0.12f, 18.0f, 28.0f);
                    const ImVec2 icon_size = g_esp_weapon_icon_font->CalcTextSizeA(icon_font_size, FLT_MAX, 0.0f, weapon_icon);
                    const ImVec2 icon_pos(snap_pixel(box_center_x - icon_size.x * 0.5f),
                                          snap_pixel(bottom_y + 6.0f));
                    const ImU32 shadow = IM_COL32(0, 0, 0, 255);
                    const ImU32 icon_color = IM_COL32(250, 250, 250, 245);
                    draw_list->AddText(g_esp_weapon_icon_font, icon_font_size, ImVec2(icon_pos.x - 1.0f, icon_pos.y), shadow,
                                       weapon_icon);
                    draw_list->AddText(g_esp_weapon_icon_font, icon_font_size, ImVec2(icon_pos.x + 1.0f, icon_pos.y), shadow,
                                       weapon_icon);
                    draw_list->AddText(g_esp_weapon_icon_font, icon_font_size, ImVec2(icon_pos.x, icon_pos.y - 1.0f), shadow,
                                       weapon_icon);
                    draw_list->AddText(g_esp_weapon_icon_font, icon_font_size, ImVec2(icon_pos.x, icon_pos.y + 1.0f), shadow,
                                       weapon_icon);
                    draw_list->AddText(g_esp_weapon_icon_font, icon_font_size, icon_pos, icon_color, weapon_icon);
                } else {
                    const char* weapon_name = weapon_name_from_index(weapon_index);
                    if (weapon_name != nullptr) {
                        ImFont* font = g_esp_weapon_font != nullptr ? g_esp_weapon_font : ImGui::GetFont();
                        const float font_size =
                            g_esp_weapon_font != nullptr ? g_esp_weapon_font->FontSize : (ImGui::GetFontSize() + 2.0f);
                        const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, weapon_name);
                        const float text_x = box_center_x - text_size.x * 0.5f;
                        const float text_y = bottom_y + 6.0f;
                        const ImVec2 text_pos(snap_pixel(text_x), snap_pixel(text_y));
                        const ImU32 shadow = IM_COL32(0, 0, 0, 255);
                        const ImU32 text_color = IM_COL32(250, 250, 250, 245);
                        draw_list->AddText(font, font_size, ImVec2(text_pos.x - 1.0f, text_pos.y), shadow, weapon_name);
                        draw_list->AddText(font, font_size, ImVec2(text_pos.x + 1.0f, text_pos.y), shadow, weapon_name);
                        draw_list->AddText(font, font_size, ImVec2(text_pos.x, text_pos.y - 1.0f), shadow, weapon_name);
                        draw_list->AddText(font, font_size, ImVec2(text_pos.x, text_pos.y + 1.0f), shadow, weapon_name);
                        draw_list->AddText(font, font_size, text_pos, text_color, weapon_name);
                    }
                }
            }
        }

        if (state.esp_skeleton) {
            std::array<Vec2, 512> bone_screen{};
            std::array<bool, 512> bone_visible{};
            for (int bone_id : kBoneIds) {
                if (bone_id < 0 || bone_id >= (int)bone_screen.size()) {
                    continue;
                }
                Vec3 bone_world{};
                if (!runtime.read_vec3(bone_matrix + bone_id * 0x20, bone_world)) {
                    continue;
                }
                Vec2 screen{};
                if (world_to_screen(view_matrix, bone_world.x, bone_world.y, bone_world.z, (float)screen_width,
                                    (float)screen_height, screen)) {
                    bone_screen[(size_t)bone_id] = screen;
                    bone_visible[(size_t)bone_id] = true;
                }
            }
            const ImU32 skeleton_color = color_u32_from_array(state.esp_skeleton_color);
            for (const auto& connection : kBoneConnections) {
                const int from = kBoneIds[(size_t)connection.first];
                const int to = kBoneIds[(size_t)connection.second];
                if (from < 0 || to < 0 || from >= (int)bone_screen.size() || to >= (int)bone_screen.size()) {
                    continue;
                }
                if (bone_visible[(size_t)from] && bone_visible[(size_t)to]) {
                    draw_list->AddLine(
                        ImVec2(snap_pixel(bone_screen[(size_t)from].x), snap_pixel(bone_screen[(size_t)from].y)),
                        ImVec2(snap_pixel(bone_screen[(size_t)to].x), snap_pixel(bone_screen[(size_t)to].y)),
                        skeleton_color, 1.0f);
                }
            }
        }
    }
#else
    (void)draw_list;
    (void)state;
    (void)screen_width;
    (void)screen_height;
    (void)overlay_mode;
#endif
}

bool query_game_avg_fps(float& out_fps) {
#ifdef _WIN32
    EspRuntime& runtime = runtime_instance();
    if (!runtime.ensure_ready()) {
        return false;
    }
    return runtime.read_game_avg_fps(out_fps);
#else
    (void)out_fps;
    return false;
#endif
}

} // namespace timmy::features
