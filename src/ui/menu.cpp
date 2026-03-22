#include "ui/menu.h"

#include "app_config.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <objidl.h>
#include <propidl.h>
#include <wincodec.h>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Windowscodecs.lib")
#endif

#include "imgui.h"
#include "imgui_internal.h"

#include "features/esp.h"
#include "ui/state.h"

#include "../../menu phobia/examples/example_win32_directx9/bytearray.h"

namespace timmy::ui {

namespace {

struct PreviewState {
    int active_tab = 0;

    bool particles = false;
    bool enable_assist = false;
    bool auto_fire = false;
    bool auto_wall = false;
    bool safe_point = false;
    bool silent_aim = false;
    bool lag_peek = false;
    bool hide_shots = false;
    bool double_tap = false;

    bool player_box = false;
    bool player_hp = false;
    bool player_armor = false;
    bool player_weapon = false;
    bool player_skeleton = false;
    bool world_grenades = false;
    bool world_bomb = false;
    bool view_fov = false;
    bool misc_bhop = false;
    bool misc_hit_sound = false;

    float fov = 58.0f;
    float hitchance = 72.0f;
    float min_damage = 24.0f;
    float viewmodel = 68.0f;

    int override_type = 0;
    int accuracy_type = 0;
    int config_slot = 0;
    char search[64] = "phobia_preview";
};

PreviewState& preview_state() {
    static PreviewState state;
    return state;
}

ImFont* g_menu_body_font = nullptr;
ImFont* g_menu_label_font = nullptr;
ImFont* g_menu_icon_font = nullptr;
ImFont* g_menu_title_font = nullptr;

#ifdef _WIN32
struct PreviewOverlayCalibration {
    bool valid = false;
    float alpha_left = 0.0f;
    float alpha_top = 0.0f;
    float alpha_right = 0.0f;
    float alpha_bottom = 0.0f;
    float box_left = 0.0f;
    float box_top = 0.0f;
    float box_right = 0.0f;
    float box_bottom = 0.0f;
};

struct MenuTexture {
    ID3D11ShaderResourceView* shader_resource = nullptr;
    float width = 0.0f;
    float height = 0.0f;
    std::vector<ID3D11ShaderResourceView*> animation_frames;
    std::vector<float> animation_delays;
    size_t current_frame = 0;
    float frame_elapsed = 0.0f;
    PreviewOverlayCalibration preview_overlay{};
};

ID3D11Device* g_menu_device = nullptr;
IWICImagingFactory* g_menu_wic_factory = nullptr;
bool g_menu_com_initialized = false;
MenuTexture g_menu_background{};
MenuTexture g_menu_logo{};
MenuTexture g_menu_logo_alt{};
MenuTexture g_menu_user{};
MenuTexture g_menu_players_preview{};
bool g_menu_custom_logo_loaded = false;

void release_texture(MenuTexture& texture) {
    if (!texture.animation_frames.empty()) {
        for (ID3D11ShaderResourceView* frame : texture.animation_frames) {
            if (frame != nullptr) {
                frame->Release();
            }
        }
        texture.animation_frames.clear();
        texture.animation_delays.clear();
        texture.current_frame = 0;
        texture.frame_elapsed = 0.0f;
        texture.shader_resource = nullptr;
    } else if (texture.shader_resource != nullptr) {
        texture.shader_resource->Release();
        texture.shader_resource = nullptr;
    }
    texture.width = 0.0f;
    texture.height = 0.0f;
    texture.preview_overlay = {};
}

float sample_percentile(std::vector<float>& values, float percentile) {
    if (values.empty()) {
        return 0.0f;
    }

    percentile = std::clamp(percentile, 0.0f, 1.0f);
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(std::lround(percentile * static_cast<float>(values.size() - 1u)));
    return values[index];
}

PreviewOverlayCalibration compute_preview_overlay_calibration(const std::vector<std::uint8_t>& pixels, UINT width, UINT height) {
    PreviewOverlayCalibration calibration{};
    if (pixels.empty() || width == 0u || height == 0u) {
        return calibration;
    }

    constexpr std::uint8_t kAlphaThreshold = 20u;
    int alpha_left = static_cast<int>(width);
    int alpha_top = static_cast<int>(height);
    int alpha_right = -1;
    int alpha_bottom = -1;
    std::vector<float> row_lefts;
    std::vector<float> row_rights;
    std::vector<float> row_positions;
    row_lefts.reserve(height);
    row_rights.reserve(height);
    row_positions.reserve(height);

    for (UINT y = 0; y < height; ++y) {
        int row_left = -1;
        int row_right = -1;
        const size_t row_start = static_cast<size_t>(y) * static_cast<size_t>(width) * 4u;
        for (UINT x = 0; x < width; ++x) {
            const size_t pixel_index = row_start + static_cast<size_t>(x) * 4u;
            if (pixels[pixel_index + 3u] <= kAlphaThreshold) {
                continue;
            }

            if (row_left < 0) {
                row_left = static_cast<int>(x);
            }
            row_right = static_cast<int>(x);
        }

        if (row_left < 0 || row_right < row_left) {
            continue;
        }

        alpha_left = std::min(alpha_left, row_left);
        alpha_top = std::min(alpha_top, static_cast<int>(y));
        alpha_right = std::max(alpha_right, row_right);
        alpha_bottom = std::max(alpha_bottom, static_cast<int>(y));
        row_lefts.push_back(static_cast<float>(row_left));
        row_rights.push_back(static_cast<float>(row_right));
        row_positions.push_back(static_cast<float>(y));
    }

    if (alpha_right < alpha_left || alpha_bottom < alpha_top) {
        return calibration;
    }

    const float content_top = static_cast<float>(alpha_top);
    const float content_bottom = static_cast<float>(alpha_bottom);
    const float content_height = std::max(1.0f, content_bottom - content_top);
    std::vector<float> body_lefts;
    std::vector<float> body_rights;
    body_lefts.reserve(row_lefts.size());
    body_rights.reserve(row_rights.size());

    const float body_min_y = content_top + content_height * 0.08f;
    const float body_max_y = content_top + content_height * 0.96f;
    for (size_t i = 0; i < row_positions.size(); ++i) {
        if (row_positions[i] < body_min_y || row_positions[i] > body_max_y) {
            continue;
        }

        body_lefts.push_back(row_lefts[i]);
        body_rights.push_back(row_rights[i]);
    }

    if (body_lefts.empty() || body_rights.empty()) {
        body_lefts = row_lefts;
        body_rights = row_rights;
    }

    calibration.valid = true;
    calibration.alpha_left = static_cast<float>(alpha_left);
    calibration.alpha_top = static_cast<float>(alpha_top);
    calibration.alpha_right = static_cast<float>(alpha_right);
    calibration.alpha_bottom = static_cast<float>(alpha_bottom);
    calibration.box_left = std::clamp(sample_percentile(body_lefts, 0.20f) - 1.0f, calibration.alpha_left, calibration.alpha_right);
    calibration.box_top = calibration.alpha_top;
    calibration.box_right = std::clamp(sample_percentile(body_rights, 0.90f) + 1.0f, calibration.alpha_left, calibration.alpha_right);
    calibration.box_bottom = calibration.alpha_bottom;

    if (calibration.box_right <= calibration.box_left) {
        calibration.box_left = calibration.alpha_left;
        calibration.box_right = calibration.alpha_right;
    }

    return calibration;
}

bool ensure_wic_factory() {
    if (g_menu_wic_factory != nullptr) {
        return true;
    }

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com_result)) {
        g_menu_com_initialized = true;
    } else if (com_result != RPC_E_CHANGED_MODE) {
        return false;
    }

    return SUCCEEDED(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_menu_wic_factory)));
}

bool create_shader_resource_from_pixels(const std::uint8_t* pixels, UINT width, UINT height, ID3D11ShaderResourceView** out_shader_resource) {
    if (g_menu_device == nullptr || pixels == nullptr || out_shader_resource == nullptr || width == 0 || height == 0) {
        return false;
    }

    ID3D11Texture2D* texture = nullptr;
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
    initial_data.pSysMem = pixels;
    initial_data.SysMemPitch = width * 4u;

    if (FAILED(g_menu_device->CreateTexture2D(&texture_desc, &initial_data, &texture))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = texture_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;

    const HRESULT srv_result = g_menu_device->CreateShaderResourceView(texture, &srv_desc, out_shader_resource);
    texture->Release();
    return SUCCEEDED(srv_result);
}

bool load_texture_from_memory(const unsigned char* bytes, size_t size, MenuTexture& out_texture) {
    if (g_menu_device == nullptr || bytes == nullptr || size == 0 || !ensure_wic_factory()) {
        return false;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory == nullptr) {
        return false;
    }

    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, bytes, size);
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11ShaderResourceView* shader_resource = nullptr;

    bool loaded = false;
    do {
        if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
            break;
        }
        memory = nullptr;

        if (FAILED(g_menu_wic_factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
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

        if (FAILED(g_menu_wic_factory->CreateFormatConverter(&converter))) {
            break;
        }
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                                         WICBitmapPaletteTypeCustom))) {
            break;
        }

        std::vector<std::uint8_t> pixels((size_t)width * (size_t)height * 4u);
        if (FAILED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(pixels.size()), pixels.data()))) {
            break;
        }

        if (!create_shader_resource_from_pixels(pixels.data(), width, height, &shader_resource)) {
            break;
        }

        out_texture.shader_resource = shader_resource;
        out_texture.width = static_cast<float>(width);
        out_texture.height = static_cast<float>(height);
        out_texture.animation_frames.clear();
        out_texture.animation_delays.clear();
        out_texture.current_frame = 0;
        out_texture.frame_elapsed = 0.0f;
        out_texture.preview_overlay = compute_preview_overlay_calibration(pixels, width, height);
        shader_resource = nullptr;
        loaded = true;
    } while (false);

    if (shader_resource != nullptr) {
        shader_resource->Release();
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
    if (stream != nullptr) {
        stream->Release();
    }
    if (memory != nullptr) {
        GlobalFree(memory);
    }

    return loaded;
}

float gif_frame_delay_seconds(IWICBitmapFrameDecode* frame) {
    if (frame == nullptr) {
        return 0.10f;
    }

    IWICMetadataQueryReader* metadata_reader = nullptr;
    if (FAILED(frame->GetMetadataQueryReader(&metadata_reader)) || metadata_reader == nullptr) {
        return 0.10f;
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    float delay_seconds = 0.10f;

    if (SUCCEEDED(metadata_reader->GetMetadataByName(L"/grctlext/Delay", &value))) {
        unsigned int delay_centiseconds = 0;
        if (value.vt == VT_UI2) {
            delay_centiseconds = value.uiVal;
        } else if (value.vt == VT_UI4) {
            delay_centiseconds = value.ulVal;
        }

        if (delay_centiseconds == 0u) {
            delay_centiseconds = 10u;
        }

        delay_seconds = std::max(0.02f, static_cast<float>(delay_centiseconds) / 100.0f);
    }

    PropVariantClear(&value);
    metadata_reader->Release();
    return delay_seconds;
}

bool metadata_uint_value(IWICMetadataQueryReader* metadata_reader, const wchar_t* name, UINT& out_value) {
    if (metadata_reader == nullptr || name == nullptr) {
        return false;
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = metadata_reader->GetMetadataByName(name, &value);
    if (FAILED(result)) {
        PropVariantClear(&value);
        return false;
    }

    bool converted = true;
    switch (value.vt) {
    case VT_UI1:
        out_value = value.bVal;
        break;
    case VT_UI2:
        out_value = value.uiVal;
        break;
    case VT_UI4:
        out_value = value.ulVal;
        break;
    case VT_I2:
        out_value = value.iVal >= 0 ? static_cast<UINT>(value.iVal) : 0u;
        break;
    case VT_I4:
        out_value = value.lVal >= 0 ? static_cast<UINT>(value.lVal) : 0u;
        break;
    default:
        converted = false;
        break;
    }

    PropVariantClear(&value);
    return converted;
}

void composite_pbgra_pixels(std::vector<std::uint8_t>& canvas, UINT canvas_width, UINT canvas_height, const std::vector<std::uint8_t>& frame_pixels,
                            UINT frame_width, UINT frame_height, UINT offset_x, UINT offset_y) {
    if (canvas.empty() || frame_pixels.empty() || canvas_width == 0u || canvas_height == 0u || frame_width == 0u || frame_height == 0u) {
        return;
    }

    for (UINT y = 0; y < frame_height; ++y) {
        const UINT canvas_y = offset_y + y;
        if (canvas_y >= canvas_height) {
            break;
        }

        for (UINT x = 0; x < frame_width; ++x) {
            const UINT canvas_x = offset_x + x;
            if (canvas_x >= canvas_width) {
                break;
            }

            const size_t src_index = (static_cast<size_t>(y) * frame_width + x) * 4u;
            const size_t dst_index = (static_cast<size_t>(canvas_y) * canvas_width + canvas_x) * 4u;
            const std::uint8_t src_alpha = frame_pixels[src_index + 3u];
            if (src_alpha == 0u) {
                continue;
            }

            if (src_alpha == 255u) {
                canvas[dst_index + 0u] = frame_pixels[src_index + 0u];
                canvas[dst_index + 1u] = frame_pixels[src_index + 1u];
                canvas[dst_index + 2u] = frame_pixels[src_index + 2u];
                canvas[dst_index + 3u] = 255u;
                continue;
            }

            const std::uint8_t inv_alpha = static_cast<std::uint8_t>(255u - src_alpha);
            canvas[dst_index + 0u] = static_cast<std::uint8_t>(frame_pixels[src_index + 0u] + ((canvas[dst_index + 0u] * inv_alpha) / 255u));
            canvas[dst_index + 1u] = static_cast<std::uint8_t>(frame_pixels[src_index + 1u] + ((canvas[dst_index + 1u] * inv_alpha) / 255u));
            canvas[dst_index + 2u] = static_cast<std::uint8_t>(frame_pixels[src_index + 2u] + ((canvas[dst_index + 2u] * inv_alpha) / 255u));
            canvas[dst_index + 3u] = static_cast<std::uint8_t>(src_alpha + ((canvas[dst_index + 3u] * inv_alpha) / 255u));
        }
    }
}

bool load_animated_texture_from_memory(const unsigned char* bytes, size_t size, MenuTexture& out_texture) {
    if (g_menu_device == nullptr || bytes == nullptr || size == 0 || !ensure_wic_factory()) {
        return false;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory == nullptr) {
        return false;
    }

    void* destination = GlobalLock(memory);
    if (destination == nullptr) {
        GlobalFree(memory);
        return false;
    }
    std::memcpy(destination, bytes, size);
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    bool loaded = false;
    std::vector<ID3D11ShaderResourceView*> frames;
    std::vector<float> delays;
    float texture_width = 0.0f;
    float texture_height = 0.0f;

    do {
        if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
            break;
        }
        memory = nullptr;

        if (FAILED(g_menu_wic_factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
            break;
        }

        UINT frame_count = 0;
        if (FAILED(decoder->GetFrameCount(&frame_count)) || frame_count == 0u) {
            break;
        }

        if (frame_count == 1u) {
            loaded = load_texture_from_memory(bytes, size, out_texture);
            break;
        }

        UINT canvas_width = 0u;
        UINT canvas_height = 0u;
        IWICMetadataQueryReader* decoder_metadata = nullptr;
        if (SUCCEEDED(decoder->GetMetadataQueryReader(&decoder_metadata)) && decoder_metadata != nullptr) {
            metadata_uint_value(decoder_metadata, L"/logscrdesc/Width", canvas_width);
            metadata_uint_value(decoder_metadata, L"/logscrdesc/Height", canvas_height);
            decoder_metadata->Release();
        }

        std::vector<std::uint8_t> canvas;

        for (UINT frame_index = 0; frame_index < frame_count; ++frame_index) {
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* converter = nullptr;
            ID3D11ShaderResourceView* shader_resource = nullptr;

            if (FAILED(decoder->GetFrame(frame_index, &frame)) || frame == nullptr) {
                if (converter != nullptr) converter->Release();
                if (frame != nullptr) frame->Release();
                break;
            }

            UINT width = 0;
            UINT height = 0;
            if (FAILED(frame->GetSize(&width, &height)) || width == 0u || height == 0u) {
                frame->Release();
                break;
            }

            IWICMetadataQueryReader* frame_metadata = nullptr;
            UINT frame_left = 0u;
            UINT frame_top = 0u;
            UINT frame_width = width;
            UINT frame_height = height;
            if (SUCCEEDED(frame->GetMetadataQueryReader(&frame_metadata)) && frame_metadata != nullptr) {
                metadata_uint_value(frame_metadata, L"/imgdesc/Left", frame_left);
                metadata_uint_value(frame_metadata, L"/imgdesc/Top", frame_top);
                metadata_uint_value(frame_metadata, L"/imgdesc/Width", frame_width);
                metadata_uint_value(frame_metadata, L"/imgdesc/Height", frame_height);
            }

            frame_width = frame_width == 0u ? width : frame_width;
            frame_height = frame_height == 0u ? height : frame_height;
            if (canvas_width == 0u) canvas_width = std::max(width, frame_left + frame_width);
            if (canvas_height == 0u) canvas_height = std::max(height, frame_top + frame_height);
            if (canvas.empty()) {
                canvas.resize(static_cast<size_t>(canvas_width) * canvas_height * 4u, 0u);
                texture_width = static_cast<float>(canvas_width);
                texture_height = static_cast<float>(canvas_height);
            }

            if (FAILED(g_menu_wic_factory->CreateFormatConverter(&converter))) {
                if (frame_metadata != nullptr) frame_metadata->Release();
                frame->Release();
                break;
            }

            if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f,
                                             WICBitmapPaletteTypeCustom))) {
                converter->Release();
                if (frame_metadata != nullptr) frame_metadata->Release();
                frame->Release();
                break;
            }

            std::vector<std::uint8_t> pixels((size_t)width * (size_t)height * 4u);
            if (FAILED(converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(pixels.size()), pixels.data())) ||
                pixels.empty()) {
                converter->Release();
                if (frame_metadata != nullptr) frame_metadata->Release();
                frame->Release();
                break;
            }

            composite_pbgra_pixels(canvas, canvas_width, canvas_height, pixels, width, height, frame_left, frame_top);

            if (!create_shader_resource_from_pixels(canvas.data(), canvas_width, canvas_height, &shader_resource)) {
                converter->Release();
                if (frame_metadata != nullptr) frame_metadata->Release();
                frame->Release();
                if (shader_resource != nullptr) {
                    shader_resource->Release();
                }
                break;
            }

            frames.push_back(shader_resource);
            delays.push_back(gif_frame_delay_seconds(frame));

            converter->Release();
            if (frame_metadata != nullptr) frame_metadata->Release();
            frame->Release();
        }

        if (frames.size() != frame_count) {
            break;
        }

        out_texture.animation_frames = std::move(frames);
        out_texture.animation_delays = std::move(delays);
        out_texture.current_frame = 0;
        out_texture.frame_elapsed = 0.0f;
        out_texture.shader_resource = out_texture.animation_frames.front();
        out_texture.width = texture_width;
        out_texture.height = texture_height;
        out_texture.preview_overlay = {};
        loaded = true;
    } while (false);

    if (!loaded) {
        for (ID3D11ShaderResourceView* frame : frames) {
            if (frame != nullptr) {
                frame->Release();
            }
        }
    }

    if (decoder != nullptr) {
        decoder->Release();
    }
    if (stream != nullptr) {
        stream->Release();
    }
    if (memory != nullptr) {
        GlobalFree(memory);
    }

    return loaded;
}

std::filesystem::path find_custom_menu_logo_path() {
    static const std::array<const char*, 6> candidates = {
        "assets/ico/mainico.png",
        "../assets/ico/mainico.png",
        "../../assets/ico/mainico.png",
        "../../../assets/ico/mainico.png",
        "../../../../assets/ico/mainico.png",
        "../../../../../assets/ico/mainico.png",
    };

    for (const char* candidate : candidates) {
        const std::filesystem::path path = candidate;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return path;
        }
    }

    return {};
}

std::filesystem::path find_custom_user_avatar_path() {
    static const std::array<const char*, 6> candidates = {
        "assets/ico/avico.gif",
        "../assets/ico/avico.gif",
        "../../assets/ico/avico.gif",
        "../../../assets/ico/avico.gif",
        "../../../../assets/ico/avico.gif",
        "../../../../../assets/ico/avico.gif",
    };

    for (const char* candidate : candidates) {
        const std::filesystem::path path = candidate;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return path;
        }
    }

    return {};
}

std::filesystem::path find_players_preview_image_path() {
    static const std::array<const char*, 6> candidates = {
        "assets/ico/EsPl.png",
        "../assets/ico/EsPl.png",
        "../../assets/ico/EsPl.png",
        "../../../assets/ico/EsPl.png",
        "../../../../assets/ico/EsPl.png",
        "../../../../../assets/ico/EsPl.png",
    };

    for (const char* candidate : candidates) {
        const std::filesystem::path path = candidate;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return path;
        }
    }

    return {};
}

bool load_texture_from_file(const std::filesystem::path& path, MenuTexture& out_texture) {
    if (path.empty()) {
        return false;
    }

    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return false;
    }

    const std::streamsize size = stream.tellg();
    if (size <= 0) {
        return false;
    }

    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return false;
    }

    return load_texture_from_memory(bytes.data(), bytes.size(), out_texture);
}

bool load_animated_texture_from_file(const std::filesystem::path& path, MenuTexture& out_texture) {
    if (path.empty()) {
        return false;
    }

    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return false;
    }

    const std::streamsize size = stream.tellg();
    if (size <= 0) {
        return false;
    }

    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return false;
    }

    return load_animated_texture_from_memory(bytes.data(), bytes.size(), out_texture);
}

ID3D11ShaderResourceView* current_texture_view(MenuTexture& texture) {
    if (texture.animation_frames.empty()) {
        return texture.shader_resource;
    }

    if (texture.current_frame >= texture.animation_frames.size()) {
        texture.current_frame = 0;
        texture.frame_elapsed = 0.0f;
    }

    texture.frame_elapsed += ImGui::GetIO().DeltaTime;
    while (!texture.animation_delays.empty() && texture.frame_elapsed >= texture.animation_delays[texture.current_frame]) {
        texture.frame_elapsed -= texture.animation_delays[texture.current_frame];
        texture.current_frame = (texture.current_frame + 1u) % texture.animation_frames.size();
    }

    texture.shader_resource = texture.animation_frames[texture.current_frame];
    return texture.shader_resource;
}
#endif

ImVec4 phobia_accent_vec4(float alpha = 1.0f) {
    return ImVec4(80.0f / 255.0f, 123.0f / 255.0f, 252.0f / 255.0f, alpha);
}

ImU32 phobia_accent_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_accent_vec4(alpha));
}

ImVec4 phobia_hover_vec4(float alpha = 1.0f) {
    return ImVec4(106.0f / 255.0f, 146.0f / 255.0f, 252.0f / 255.0f, alpha);
}

ImU32 phobia_hover_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_hover_vec4(alpha));
}

ImVec4 phobia_active_vec4(float alpha = 1.0f) {
    return ImVec4(64.0f / 255.0f, 108.0f / 255.0f, 242.0f / 255.0f, alpha);
}

ImU32 phobia_active_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_active_vec4(alpha));
}

ImVec4 phobia_gradient_vec4(float alpha = 1.0f) {
    return ImVec4(14.0f / 255.0f, 20.0f / 255.0f, 34.0f / 255.0f, alpha);
}

ImU32 phobia_gradient_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_gradient_vec4(alpha));
}

ImVec4 phobia_panel_vec4(float alpha = 1.0f) {
    return ImVec4(21.0f / 255.0f, 20.0f / 255.0f, 29.0f / 255.0f, alpha);
}

ImU32 phobia_panel_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_panel_vec4(alpha));
}

ImVec4 phobia_panel_alt_vec4(float alpha = 1.0f) {
    return ImVec4(23.0f / 255.0f, 32.0f / 255.0f, 51.0f / 255.0f, alpha);
}

ImU32 phobia_panel_alt_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_panel_alt_vec4(alpha));
}

ImVec4 phobia_border_vec4(float alpha = 1.0f) {
    return ImVec4(28.0f / 255.0f, 37.0f / 255.0f, 54.0f / 255.0f, alpha);
}

ImU32 phobia_border_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_border_vec4(alpha));
}

ImVec4 phobia_frame_vec4(float alpha = 1.0f) {
    return ImVec4(12.0f / 255.0f, 14.0f / 255.0f, 18.0f / 255.0f, alpha);
}

ImU32 phobia_frame_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_frame_vec4(alpha));
}

ImVec4 phobia_muted_text_vec4(float alpha = 1.0f) {
    return ImVec4(154.0f / 255.0f, 164.0f / 255.0f, 178.0f / 255.0f, alpha);
}

ImU32 phobia_muted_text_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_muted_text_vec4(alpha));
}

ImVec4 phobia_idle_text_vec4(float alpha = 1.0f) {
    return ImVec4(91.0f / 255.0f, 101.0f / 255.0f, 117.0f / 255.0f, alpha);
}

ImU32 phobia_idle_text_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_idle_text_vec4(alpha));
}

ImVec4 phobia_panel_active_vec4(float alpha = 1.0f) {
    return ImVec4(27.0f / 255.0f, 38.0f / 255.0f, 59.0f / 255.0f, alpha);
}

ImU32 phobia_panel_active_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_panel_active_vec4(alpha));
}

ImVec4 phobia_inner_panel_vec4(float alpha = 1.0f) {
    return ImVec4(20.0f / 255.0f, 19.0f / 255.0f, 28.0f / 255.0f, alpha);
}

ImU32 phobia_inner_panel_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_inner_panel_vec4(alpha));
}

ImVec4 phobia_inner_border_vec4(float alpha = 1.0f) {
    return ImVec4(26.0f / 255.0f, 35.0f / 255.0f, 51.0f / 255.0f, alpha);
}

ImU32 phobia_inner_border_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_inner_border_vec4(alpha));
}

ImVec4 phobia_soft_border_vec4(float alpha = 1.0f) {
    return ImVec4(34.0f / 255.0f, 48.0f / 255.0f, 74.0f / 255.0f, alpha);
}

ImU32 phobia_soft_border_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_soft_border_vec4(alpha));
}

ImVec4 phobia_sidebar_bg_vec4(float alpha = 1.0f) {
    return ImVec4(10.0f / 255.0f, 14.0f / 255.0f, 20.0f / 255.0f, alpha);
}

ImU32 phobia_sidebar_bg_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_sidebar_bg_vec4(alpha));
}

ImVec4 phobia_sidebar_item_vec4(float alpha = 1.0f) {
    return ImVec4(19.0f / 255.0f, 27.0f / 255.0f, 43.0f / 255.0f, alpha);
}

ImU32 phobia_sidebar_item_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_sidebar_item_vec4(alpha));
}

ImVec4 phobia_sidebar_glow_vec4(float alpha = 1.0f) {
    return ImVec4(10.0f / 255.0f, 26.0f / 255.0f, 47.0f / 255.0f, alpha);
}

ImU32 phobia_sidebar_glow_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_sidebar_glow_vec4(alpha));
}

ImVec4 phobia_button_default_vec4(float alpha = 1.0f) {
    return ImVec4(20.0f / 255.0f, 19.0f / 255.0f, 28.0f / 255.0f, alpha);
}

ImU32 phobia_button_default_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_button_default_vec4(alpha));
}

ImVec4 phobia_button_hover_vec4(float alpha = 1.0f) {
    return ImVec4(38.0f / 255.0f, 50.0f / 255.0f, 74.0f / 255.0f, alpha);
}

ImU32 phobia_button_hover_u32(float alpha = 1.0f) {
    return ImGui::GetColorU32(phobia_button_hover_vec4(alpha));
}

ImFont* choose_font(ImFont* preferred) {
    return preferred != nullptr ? preferred : ImGui::GetFont();
}

float choose_font_size(ImFont* preferred, float size) {
    return preferred != nullptr ? size : ImGui::GetFontSize();
}

ImVec2 offset_vec2(const ImVec2& value, float x, float y) {
    return ImVec2(value.x + x, value.y + y);
}

ImVec2 lerp_vec2(const ImVec2& from, const ImVec2& to, float amount) {
    return ImVec2(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount);
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

ImRect fit_texture_rect(const ImVec2& min_pos, const ImVec2& max_pos, float texture_width, float texture_height) {
    if (texture_width <= 0.0f || texture_height <= 0.0f) {
        return ImRect(min_pos, min_pos);
    }

    const float box_width = std::max(1.0f, max_pos.x - min_pos.x);
    const float box_height = std::max(1.0f, max_pos.y - min_pos.y);
    const float scale = std::min(box_width / texture_width, box_height / texture_height);
    const float draw_width = texture_width * scale;
    const float draw_height = texture_height * scale;
    const ImVec2 draw_min(min_pos.x + (box_width - draw_width) * 0.5f, min_pos.y + (box_height - draw_height) * 0.5f);
    return ImRect(draw_min, ImVec2(draw_min.x + draw_width, draw_min.y + draw_height));
}

void draw_texture_fit(ImDrawList* draw, ImTextureID texture_id, const ImVec2& min_pos, const ImVec2& max_pos, float texture_width,
                      float texture_height) {
    if (draw == nullptr || texture_width <= 0.0f || texture_height <= 0.0f) {
        return;
    }

    const ImRect draw_rect = fit_texture_rect(min_pos, max_pos, texture_width, texture_height);
    draw->AddImage(texture_id, draw_rect.Min, draw_rect.Max);
}

void draw_players_preview_overlay(ImDrawList* draw, const ImRect& image_rect, const State& state) {
#ifndef _WIN32
    (void)draw;
    (void)image_rect;
    (void)state;
#else
    if (draw == nullptr) {
        return;
    }

    const MenuTexture& texture = g_menu_players_preview;
    const float screen_width = image_rect.Max.x - image_rect.Min.x;
    const float screen_height = image_rect.Max.y - image_rect.Min.y;
    if (screen_width <= 0.0f || screen_height <= 0.0f || texture.width <= 0.0f || texture.height <= 0.0f) {
        return;
    }

    const PreviewOverlayCalibration& calibration = texture.preview_overlay;
    const float tex_left = calibration.valid ? calibration.box_left : texture.width * 0.19f;
    const float tex_top = calibration.valid ? calibration.box_top : texture.height * 0.03f;
    const float tex_right = calibration.valid ? calibration.box_right : texture.width * 0.88f;
    const float tex_bottom = calibration.valid ? calibration.box_bottom : texture.height * 0.98f;
    const float scale_x = screen_width / texture.width;
    const float scale_y = screen_height / texture.height;
    const float raw_left_x = image_rect.Min.x + tex_left * scale_x;
    const float raw_top_y = image_rect.Min.y + tex_top * scale_y;
    const float raw_right_x = image_rect.Min.x + tex_right * scale_x;
    const float raw_bottom_y = image_rect.Min.y + tex_bottom * scale_y;
    const float raw_box_width = std::max(1.0f, raw_right_x - raw_left_x);
    const float raw_box_height = std::max(1.0f, raw_bottom_y - raw_top_y);
    const float horizontal_padding = raw_box_width * 0.035f;
    const float top_padding = raw_box_height * 0.018f;
    const float bottom_padding = raw_box_height * 0.022f;
    const float left_x = raw_left_x - horizontal_padding;
    const float top_y = raw_top_y - top_padding;
    const float right_x = raw_right_x + horizontal_padding;
    const float bottom_y = raw_bottom_y + bottom_padding;
    const float center_x = (left_x + right_x) * 0.5f;
    const float box_width = std::max(1.0f, right_x - left_x);
    const float box_height = std::max(1.0f, bottom_y - top_y);

    const ImU32 box_color = color_u32_from_array(state.esp_box_color);
    const ImU32 shadow = IM_COL32(0, 0, 0, 210);

    if (state.esp_box) {
        draw->AddRect(ImVec2(std::round(left_x - 1.0f), std::round(top_y - 1.0f)),
                      ImVec2(std::round(right_x + 1.0f), std::round(bottom_y + 1.0f)),
                      shadow, 0.0f, 0, 2.0f);
        draw->AddRect(ImVec2(std::round(left_x), std::round(top_y)),
                      ImVec2(std::round(right_x), std::round(bottom_y)),
                      box_color, 0.0f, 0, 1.0f);
    }

    if (state.esp_hp_bar) {
        const float hp_ratio = 0.78f;
        const float hp_bar_x = left_x - 4.0f;
        const float hp_start = top_y + box_height * (1.0f - hp_ratio);
        const ImU32 hp_fg = color_u32_from_array(state.esp_hp_color);
        draw->AddLine(ImVec2(std::round(hp_bar_x), std::round(top_y)), ImVec2(std::round(hp_bar_x), std::round(bottom_y)), shadow, 4.0f);
        draw->AddLine(ImVec2(std::round(hp_bar_x), std::round(hp_start)), ImVec2(std::round(hp_bar_x), std::round(bottom_y)), hp_fg, 3.0f);

        const char* hp_text = "78";
        ImFont* hp_font = choose_font(g_menu_body_font);
        const float hp_font_size = choose_font_size(g_menu_body_font, 14.0f);
        const ImVec2 hp_text_size = hp_font->CalcTextSizeA(hp_font_size, FLT_MAX, 0.0f, hp_text);
        const ImVec2 hp_text_pos(std::round(hp_bar_x - hp_text_size.x * 0.5f), std::round(top_y - hp_text_size.y - 3.0f));
        draw->AddText(hp_font, hp_font_size, ImVec2(hp_text_pos.x + 1.0f, hp_text_pos.y + 1.0f), shadow, hp_text);
        draw->AddText(hp_font, hp_font_size, hp_text_pos, IM_COL32(245, 245, 245, 245), hp_text);
    }

    if (state.esp_armor) {
        const float armor_ratio = 0.62f;
        const float armor_bar_x = left_x - (state.esp_hp_bar ? 8.0f : 4.0f);
        const float armor_start = top_y + box_height * (1.0f - armor_ratio);
        const ImU32 armor_fg = color_u32_from_array(state.esp_armor_color);
        draw->AddLine(ImVec2(std::round(armor_bar_x), std::round(top_y)), ImVec2(std::round(armor_bar_x), std::round(bottom_y)), shadow, 4.0f);
        draw->AddLine(ImVec2(std::round(armor_bar_x), std::round(armor_start)), ImVec2(std::round(armor_bar_x), std::round(bottom_y)), armor_fg, 3.0f);
    }

    if (state.esp_weapon) {
        ImFont* weapon_icon_font = features::get_esp_weapon_icon_font();
        const char* weapon_icon = features::get_esp_preview_weapon_icon();
        if (weapon_icon_font != nullptr && weapon_icon != nullptr) {
            const float weapon_icon_size = 22.0f;
            const ImVec2 icon_size = weapon_icon_font->CalcTextSizeA(weapon_icon_size, FLT_MAX, 0.0f, weapon_icon);
            const ImVec2 icon_pos(std::round(center_x - icon_size.x * 0.5f), std::round(bottom_y + 7.0f));
            draw->AddText(weapon_icon_font, weapon_icon_size, ImVec2(icon_pos.x + 1.0f, icon_pos.y + 1.0f), shadow, weapon_icon);
            draw->AddText(weapon_icon_font, weapon_icon_size, icon_pos, IM_COL32(245, 245, 245, 245), weapon_icon);
        }
    }

    if (state.esp_bomb) {
        ImFont* weapon_icon_font = features::get_esp_weapon_icon_font();
        const char* bomb_icon = features::get_esp_preview_bomb_icon();
        if (weapon_icon_font != nullptr && bomb_icon != nullptr) {
            const float bomb_icon_size = 22.0f;
            const ImVec2 bomb_pos(std::round(right_x + 4.0f), std::round(top_y - 2.0f));
            draw->AddText(weapon_icon_font, bomb_icon_size, ImVec2(bomb_pos.x + 1.0f, bomb_pos.y + 1.0f), shadow, bomb_icon);
            draw->AddText(weapon_icon_font, bomb_icon_size, bomb_pos, IM_COL32(255, 110, 105, 245), bomb_icon);
        }
    }

    if (state.esp_skeleton) {
        const auto texture_point = [&](float x_ratio, float y_ratio) {
            return ImVec2(image_rect.Min.x + texture.width * x_ratio * scale_x,
                          image_rect.Min.y + texture.height * y_ratio * scale_y);
        };

        const ImVec2 neck = texture_point(0.484f, 0.173f);
        const ImVec2 pelvis = texture_point(0.498f, 0.551f);
        const ImVec2 left_shoulder = texture_point(0.308f, 0.237f);
        const ImVec2 right_shoulder = texture_point(0.596f, 0.235f);
        const ImVec2 left_elbow = texture_point(0.317f, 0.398f);
        const ImVec2 right_elbow = texture_point(0.692f, 0.399f);
        const ImVec2 left_hand = texture_point(0.308f, 0.611f);
        const ImVec2 right_hand = texture_point(0.707f, 0.582f);
        const ImVec2 left_knee = texture_point(0.416f, 0.765f);
        const ImVec2 right_knee = texture_point(0.582f, 0.767f);
        const ImVec2 left_foot = texture_point(0.380f, 0.965f);
        const ImVec2 right_foot = texture_point(0.618f, 0.968f);
        const ImU32 skeleton_color = color_u32_from_array(state.esp_skeleton_color);

        draw->AddLine(neck, pelvis, skeleton_color, 1.5f);
        draw->AddLine(neck, left_shoulder, skeleton_color, 1.5f);
        draw->AddLine(neck, right_shoulder, skeleton_color, 1.5f);
        draw->AddLine(left_shoulder, left_elbow, skeleton_color, 1.5f);
        draw->AddLine(left_elbow, left_hand, skeleton_color, 1.5f);
        draw->AddLine(right_shoulder, right_elbow, skeleton_color, 1.5f);
        draw->AddLine(right_elbow, right_hand, skeleton_color, 1.5f);
        draw->AddLine(pelvis, left_knee, skeleton_color, 1.5f);
        draw->AddLine(left_knee, left_foot, skeleton_color, 1.5f);
        draw->AddLine(pelvis, right_knee, skeleton_color, 1.5f);
        draw->AddLine(right_knee, right_foot, skeleton_color, 1.5f);
    }
#endif
}

float scaled_anim_alpha() {
    return std::min(ImGui::GetStyle().Alpha * 1.2f, 1.0f);
}

void add_colored_text(const ImVec2& pos, ImU32 color1, ImU32 color2, const char* text) {
    if (text == nullptr) {
        return;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const char* start = text;
    const char* end = text;
    ImVec2 current_pos = pos;

    while (*end != '\0') {
        if (*end == '(') {
            if (start != end) {
                const float part_width = ImGui::CalcTextSize(start, end).x;
                draw->AddText(current_pos, color1, start, end);
                current_pos.x += part_width;
            }
            start = end + 1;
        } else if (*end == ')') {
            if (start != end) {
                draw->AddText(current_pos, color2, start, end);
                current_pos.x += ImGui::CalcTextSize(start, end).x;
            }
            start = end + 1;
        }
        ++end;
    }

    if (start != end) {
        draw->AddText(current_pos, color1, start, end);
    }
}

bool phobia_tab_button(const char* id, const char* icon, const char* label, bool selected) {
    const ImVec2 size(135.0f, 30.0f);
    const ImGuiID button_id = ImGui::GetID(id);
    ImGui::InvisibleButton(id, size);

    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    const ImVec2 min_pos = ImGui::GetItemRectMin();
    const ImVec2 max_pos = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float delta_time = ImGui::GetIO().DeltaTime;
    const float alpha_scale = scaled_anim_alpha();

    static std::map<ImGuiID, float> hover_animation;
    static std::map<ImGuiID, float> filled_animation;
    static std::map<ImGuiID, float> selected_animation;

    float& hover_value = hover_animation[button_id];
    float& filled_value = filled_animation[button_id];
    float& selected_value = selected_animation[button_id];

    hover_value = std::clamp(hover_value + (0.2f * delta_time * ((hovered || ImGui::IsItemActive()) ? 1.0f : -1.0f)), 0.0f, 0.15f);
    hover_value *= alpha_scale;
    filled_value = std::clamp(filled_value + (2.55f * delta_time * (hovered ? 1.0f : -1.0f)), hover_value, 1.0f);
    filled_value *= alpha_scale;
    selected_value = std::clamp(selected_value + (2.55f * delta_time * (selected ? 1.0f : -1.0f)), hover_value, 1.0f);
    selected_value *= alpha_scale;

    const ImU32 idle_text = phobia_muted_text_u32(ImGui::GetStyle().Alpha);

    ImFont* icon_font_ptr = choose_font(g_menu_icon_font);
    ImFont* label_font_ptr = choose_font(g_menu_label_font);
    draw->AddText(icon_font_ptr, choose_font_size(g_menu_icon_font, 18.0f), ImVec2(min_pos.x + 10.0f, min_pos.y + 5.0f),
                  idle_text, icon);
    draw->AddText(label_font_ptr, choose_font_size(g_menu_label_font, 19.0f), ImVec2(min_pos.x + 40.0f, min_pos.y + 5.0f),
                  idle_text, label);

    if (selected) {
        draw->AddRectFilled(min_pos, max_pos, phobia_sidebar_item_u32(selected_value), 5.0f);
        draw->AddRectFilled(min_pos, max_pos, phobia_accent_u32(selected_value * 0.18f), 5.0f);
        draw->AddRectFilled(ImVec2(min_pos.x, min_pos.y + 5.0f), ImVec2(min_pos.x + (selected_value * 2.0f), min_pos.y + 25.0f),
                            phobia_accent_u32(), 10.0f, ImDrawFlags_RoundCornersRight);
        draw->AddText(icon_font_ptr, choose_font_size(g_menu_icon_font, 18.0f), ImVec2(min_pos.x + 10.0f, min_pos.y + 5.0f),
                      IM_COL32(255, 255, 255, static_cast<int>(255.0f * selected_value)), icon);
        draw->AddText(label_font_ptr, choose_font_size(g_menu_label_font, 19.0f), ImVec2(min_pos.x + 40.0f, min_pos.y + 5.0f),
                      IM_COL32(255, 255, 255, static_cast<int>(255.0f * selected_value)), label);
    }

    if (hovered) {
        draw->AddRectFilled(min_pos, max_pos, phobia_panel_active_u32(filled_value * 0.45f), 5.0f);
        draw->AddRect(ImVec2(min_pos.x - 1.0f, min_pos.y), max_pos, phobia_soft_border_u32(filled_value * 0.75f), 5.0f);
    }

    return clicked;
}

bool phobia_checkbox(const char* label, bool* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems || value == nullptr) {
        return false;
    }

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const float square_sz = ImGui::GetFrameHeight();
    const float spacing_x = style.ItemInnerSpacing.x - 5.0f;
    const float spacing_y = style.ItemInnerSpacing.y - 20.0f;
    const ImVec2 pos = offset_vec2(window->DC.CursorPos, square_sz + spacing_x, square_sz + spacing_y);
    const ImRect total_bb(pos, offset_vec2(pos, 20.0f, 20.0f));

    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, id)) {
        return false;
    }

    bool hovered = false;
    bool held = false;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed) {
        *value = !(*value);
        ImGui::MarkItemEdited(id);
    }

    const float delta_time = ImGui::GetIO().DeltaTime;
    const float alpha_scale = scaled_anim_alpha();
    static std::map<ImGuiID, float> hover_animation;
    static std::map<ImGuiID, float> filled_animation;

    float& hover_value = hover_animation[id];
    float& filled_value = filled_animation[id];
    hover_value = std::clamp(hover_value + (0.2f * delta_time * ((hovered || ImGui::IsItemActive()) ? 1.0f : -1.0f)), 0.0f, 0.15f);
    hover_value *= alpha_scale;
    filled_value = std::clamp(filled_value + (2.55f * delta_time * (hovered ? 1.0f : -1.0f)), hover_value, 1.0f);
    filled_value *= alpha_scale;

    const ImRect check_bb(pos, offset_vec2(pos, square_sz, square_sz));
    ImGui::RenderNavHighlight(total_bb, id);
    const ImU32 checkbox_fill = phobia_button_default_u32(ImGui::GetStyle().Alpha);
    window->DrawList->AddRectFilled(ImVec2(check_bb.Min.x - 1.0f, check_bb.Min.y - 1.0f), ImVec2(check_bb.Min.x + 20.0f, check_bb.Min.y + 20.0f),
                                    checkbox_fill, 3.0f);
    window->DrawList->AddRect(ImVec2(check_bb.Min.x - 1.0f, check_bb.Min.y - 1.0f), ImVec2(check_bb.Min.x + 20.0f, check_bb.Min.y + 20.0f),
                              phobia_soft_border_u32((hovered ? filled_value : 0.55f) * 0.85f), 3.0f);

    if (*value) {
        ImGui::RenderCheckMark(window->DrawList, offset_vec2(check_bb.Min, 4.0f, 3.0f),
                               phobia_accent_u32(ImGui::GetStyle().Alpha),
                               square_sz - 4.0f);
    }

    const ImVec2 text_pos(check_bb.Max.x + spacing_x + 10.0f, check_bb.Min.y);
    window->DrawList->AddText(text_pos, phobia_muted_text_u32(ImGui::GetStyle().Alpha), label);
    window->DrawList->AddText(text_pos, IM_COL32(255, 255, 255, static_cast<int>(255.0f * filled_value)), label);
    if (*value) {
        window->DrawList->AddText(text_pos, IM_COL32(255, 255, 255, static_cast<int>(255.0f * ImGui::GetStyle().Alpha)), label);
    }

    return pressed;
}

void phobia_color_circle_button(const char* id, float color[4]) {
    if (id == nullptr || color == nullptr) {
        return;
    }

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) {
        return;
    }

    const ImRect last_bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const float square_sz = ImGui::GetFrameHeight();
    const ImVec2 pos(window->WorkRect.Max.x - 35.0f, last_bb.Min.y);
    const ImRect bb(pos, offset_vec2(pos, square_sz, square_sz));
    const ImRect swatch_bb(ImVec2(bb.Min.x - 1.0f, bb.Min.y - 1.0f), ImVec2(bb.Min.x + 20.0f, bb.Min.y + 20.0f));
    const ImVec2 center((swatch_bb.Min.x + swatch_bb.Max.x) * 0.5f, (swatch_bb.Min.y + swatch_bb.Max.y) * 0.5f);
    const ImGuiID button_id = window->GetID(id);
    if (!ImGui::ItemAdd(bb, button_id)) {
        return;
    }

    bool hovered = false;
    bool held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, button_id, &hovered, &held);
    const std::string popup_id = std::string(id) + "_popup";
    if (pressed) {
        ImGui::OpenPopup(popup_id.c_str());
    }

    static std::map<ImGuiID, float> hover_animation;
    static std::map<ImGuiID, float> filled_animation;
    float& hover_value = hover_animation[button_id];
    float& filled_value = filled_animation[button_id];
    const float alpha_scale = scaled_anim_alpha();
    hover_value = std::clamp(hover_value + (0.2f * ImGui::GetIO().DeltaTime * ((hovered || ImGui::IsItemActive()) ? 1.0f : -1.0f)), 0.0f, 0.15f);
    hover_value *= alpha_scale;
    filled_value = std::clamp(filled_value + (2.55f * ImGui::GetIO().DeltaTime * (hovered ? 1.0f : -1.0f)), hover_value, 1.0f);
    filled_value *= alpha_scale;

    ImDrawList* draw = window->DrawList;
    const ImVec4 display_color = ImVec4(color[0], color[1], color[2], color[3] * ImGui::GetStyle().Alpha);
    draw->AddRectFilled(swatch_bb.Min, swatch_bb.Max, ImGui::ColorConvertFloat4ToU32(display_color), 3.0f);

    ImGui::SetNextWindowPos(ImVec2(bb.Max.x + 8.0f, bb.Min.y - 6.0f), ImGuiCond_Appearing);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, phobia_panel_vec4());
    ImGui::PushStyleColor(ImGuiCol_Border, phobia_inner_border_vec4());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    if (ImGui::BeginPopup(popup_id.c_str(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoSavedSettings)) {
        const auto hex_digit = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
            if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
            return -1;
        };
        const auto parse_hex_color = [&](const char* text) -> bool {
            if (text == nullptr) {
                return false;
            }

            const char* begin = text;
            while (*begin == ' ' || *begin == '\t') {
                ++begin;
            }
            if (*begin == '#') {
                ++begin;
            }

            char digits[9]{};
            int digit_count = 0;
            for (const char* cursor = begin; *cursor != '\0' && digit_count < 8; ++cursor) {
                if (*cursor == ' ' || *cursor == '\t') {
                    continue;
                }
                if (hex_digit(*cursor) < 0) {
                    return false;
                }
                digits[digit_count++] = *cursor;
            }

            if (!(digit_count == 6 || digit_count == 8)) {
                return false;
            }

            auto byte_from_pair = [&](int index) -> float {
                const int hi = hex_digit(digits[index]);
                const int lo = hex_digit(digits[index + 1]);
                return static_cast<float>((hi << 4) | lo) / 255.0f;
            };

            color[0] = byte_from_pair(0);
            color[1] = byte_from_pair(2);
            color[2] = byte_from_pair(4);
            if (digit_count == 8) {
                color[3] = byte_from_pair(6);
            }
            return true;
        };
        const auto write_hex_color = [&](char* buffer, size_t size) {
            if (buffer == nullptr || size == 0) {
                return;
            }

            const int r = std::clamp(static_cast<int>(std::round(color[0] * 255.0f)), 0, 255);
            const int g = std::clamp(static_cast<int>(std::round(color[1] * 255.0f)), 0, 255);
            const int b = std::clamp(static_cast<int>(std::round(color[2] * 255.0f)), 0, 255);
            const int a = std::clamp(static_cast<int>(std::round(color[3] * 255.0f)), 0, 255);
            std::snprintf(buffer, size, "#%02X%02X%02X%02X", r, g, b, a);
        };
        static std::map<ImGuiID, std::array<char, 10>> hex_buffers;
        std::array<char, 10>& hex_buffer = hex_buffers[button_id];
        if (ImGui::IsWindowAppearing()) {
            write_hex_color(hex_buffer.data(), hex_buffer.size());
        }

        ImGui::TextUnformatted("Color");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        const bool picker_changed =
            ImGui::ColorPicker4("##picker", color, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                                                   ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_AlphaBar);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextUnformatted("HEX");
        ImGui::PushItemWidth(220.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, phobia_inner_panel_vec4());
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, phobia_inner_panel_vec4());
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, phobia_inner_panel_vec4());
        ImGui::PushStyleColor(ImGuiCol_Border, phobia_inner_border_vec4());
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        const bool hex_changed = ImGui::InputText("##hex_input", hex_buffer.data(), hex_buffer.size(),
                                                  ImGuiInputTextFlags_CharsUppercase);
        const bool hex_active = ImGui::IsItemActive();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        ImGui::PopItemWidth();
        if (hex_changed) {
            parse_hex_color(hex_buffer.data());
        }
        if (picker_changed && !hex_active) {
            write_hex_color(hex_buffer.data(), hex_buffer.size());
        }
        if (!hex_active) {
            write_hex_color(hex_buffer.data(), hex_buffer.size());
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

bool phobia_button(const char* label, const ImVec2& size) {
    const ImGuiID button_id = ImGui::GetID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 resolved_size =
        ImGui::CalcItemSize(size, label_size.x + style.FramePadding.x * 2.0f, label_size.y + style.FramePadding.y * 2.0f);
    ImGui::InvisibleButton(label, resolved_size);

    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();
    const float delta_time = ImGui::GetIO().DeltaTime;
    const float alpha_scale = scaled_anim_alpha();

    static std::map<ImGuiID, float> hover_animation;
    float& hover_value = hover_animation[button_id];
    hover_value = std::clamp(hover_value + (0.2f * delta_time * (hovered ? 1.0f : -1.0f)), 0.0f, 1.0f);
    hover_value *= alpha_scale;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 fill_color = held ? phobia_active_u32() : (hovered ? phobia_button_hover_u32() : phobia_button_default_u32());
    draw->AddRectFilled(pos, offset_vec2(pos, resolved_size.x, resolved_size.y), fill_color, 3.0f);
    draw->AddRect(pos, offset_vec2(pos, resolved_size.x, resolved_size.y), phobia_soft_border_u32(hover_value * 0.75f + 0.2f), 3.0f);

    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 text_pos(pos.x + (resolved_size.x - text_size.x) * 0.5f, pos.y + (resolved_size.y - text_size.y) * 0.5f);
    draw->AddText(text_pos, IM_COL32(255, 255, 255, 255), label);
    return clicked;
}

bool phobia_slider_float(const char* label, float* value, float min_value, float max_value, const char* format) {
    if (value == nullptr || max_value <= min_value) {
        return false;
    }

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) {
        return false;
    }

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const float w = ImGui::CalcItemWidth();
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const ImRect frame_bb(offset_vec2(window->DC.CursorPos, 20.0f, 18.0f),
                          offset_vec2(window->DC.CursorPos, w + 90.0f, label_size.y + style.FramePadding.y * 2.0f + 15.0f));
    const ImRect total_bb(frame_bb.Min, offset_vec2(frame_bb.Max, 0.0f, 24.0f));

    const bool temp_input_allowed = true;
    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, id, &frame_bb, temp_input_allowed ? ImGuiItemFlags_Inputable : 0)) {
        return false;
    }

    const char* fmt = format != nullptr ? format : "%.0f";
    const bool hovered = ImGui::ItemHoverable(frame_bb, id, g.LastItemData.ItemFlags);
    bool temp_input_is_active = temp_input_allowed && ImGui::TempInputIsActive(id);
    if (!temp_input_is_active) {
        const bool clicked = hovered && g.IO.MouseClicked[0];
        const bool make_active = clicked || g.NavActivateId == id;
        if (make_active && temp_input_allowed) {
            if (clicked && g.IO.KeyCtrl) {
                temp_input_is_active = true;
            }
        }
        if (make_active && !temp_input_is_active) {
            ImGui::SetActiveID(id, window);
            ImGui::SetFocusID(id, window);
            ImGui::FocusWindow(window);
            g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
        }
    }

    if (temp_input_is_active) {
        return ImGui::TempInputScalar(frame_bb, id, label, ImGuiDataType_Float, value, fmt, &min_value, &max_value);
    }

    static std::map<ImGuiID, float> hover_animation;
    static std::map<ImGuiID, float> filled_animation;
    float& hover_value = hover_animation[id];
    float& filled_value = filled_animation[id];

    hover_value = std::clamp(hover_value + (0.2f * g.IO.DeltaTime * ((hovered || ImGui::IsItemActive()) ? 1.0f : -1.0f)), 0.0f, 0.15f);
    hover_value *= scaled_anim_alpha();
    filled_value = std::clamp(filled_value + (2.55f * g.IO.DeltaTime * (hovered ? 1.0f : -1.0f)), hover_value, 1.0f);
    filled_value *= scaled_anim_alpha();

    ImGui::RenderNavHighlight(frame_bb, id);
    ImGui::RenderFrame(frame_bb.Min, frame_bb.Max, phobia_inner_panel_u32(ImGui::GetStyle().Alpha),
                       true, g.Style.FrameRounding);

    ImRect grab_bb;
    bool value_changed = ImGui::SliderBehavior(frame_bb, id, ImGuiDataType_Float, value, &min_value, &max_value, fmt, ImGuiSliderFlags_None, &grab_bb);
    if (value_changed) {
        ImGui::MarkItemEdited(id);
    }

    if (grab_bb.Max.x > grab_bb.Min.x) {
        window->DrawList->AddRectFilled(offset_vec2(frame_bb.Min, 0.0f, 1.0f), offset_vec2(grab_bb.Max, 2.0f, 1.0f),
                                        phobia_accent_u32(ImGui::GetStyle().Alpha), 3.0f);
    }
    window->DrawList->AddRect(offset_vec2(frame_bb.Min, -5.0f, -5.0f), offset_vec2(frame_bb.Min, 284.0f, 18.0f),
                              phobia_inner_border_u32(ImGui::GetStyle().Alpha), 3.0f);
    window->DrawList->AddRect(offset_vec2(frame_bb.Min, -5.0f, -5.0f), offset_vec2(frame_bb.Min, 284.0f, 18.0f),
                              phobia_soft_border_u32(filled_value * 0.8f), 3.0f);

    char value_buffer[64]{};
    const char* value_buffer_end = value_buffer + ImGui::DataTypeFormatString(value_buffer, IM_ARRAYSIZE(value_buffer), ImGuiDataType_Float, value, fmt);
    const ImVec2 value_size = ImGui::CalcTextSize(value_buffer, value_buffer_end, true);
    ImGui::RenderText(ImVec2(frame_bb.Max.x - value_size.x, frame_bb.Min.y - g.Font->FontSize - 7.0f), value_buffer, value_buffer_end);
    if (label_size.x > 0.0f) {
        ImGui::RenderText(ImVec2(frame_bb.Min.x, frame_bb.Min.y - style.FramePadding.y - 25.0f), label);
    }

    return value_changed;
}

bool phobia_combo(const char* label, int* current_item, const char* const items[], int items_count) {
    if (current_item == nullptr || items == nullptr || items_count <= 0) {
        return false;
    }

    *current_item = std::clamp(*current_item, 0, items_count - 1);

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) {
        return false;
    }

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const float w = ImGui::CalcItemWidth() + 95.0f;
    const ImRect bb(offset_vec2(window->DC.CursorPos, 15.0f, 10.0f),
                    offset_vec2(window->DC.CursorPos, w, label_size.y + style.FramePadding.y * 2.0f + 25.0f));
    const ImRect total_bb(bb.Min, offset_vec2(bb.Max, 0.0f, 8.0f));

    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, id, &bb)) {
        return false;
    }

    bool hovered = false;
    bool held = false;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

    static std::map<ImGuiID, float> hover_animation;
    float& hover_value = hover_animation[id];
    hover_value = std::clamp(hover_value + (0.2f * g.IO.DeltaTime * (hovered ? 1.0f : -1.0f)), 0.0f, 1.0f);
    hover_value *= scaled_anim_alpha();

    const std::string popup_id = "##phobia_combo_" + std::to_string(static_cast<unsigned int>(id));
    bool popup_open = ImGui::IsPopupOpen(popup_id.c_str(), ImGuiPopupFlags_None);
    if (pressed && !popup_open) {
        ImGui::OpenPopup(popup_id.c_str());
        popup_open = true;
    }

    ImGui::RenderNavHighlight(bb, id);
    window->DrawList->AddRectFilled(bb.Min, bb.Max, phobia_inner_panel_u32(), 5.0f);
    window->DrawList->AddRect(bb.Min, bb.Max, phobia_inner_border_u32(ImGui::GetStyle().Alpha), 5.0f);
    window->DrawList->AddRect(bb.Min, bb.Max, phobia_soft_border_u32(hover_value * 0.75f), 5.0f);
    window->DrawList->AddText(offset_vec2(bb.Min, 10.0f, 6.0f), IM_COL32(255, 255, 255, 255), items[*current_item]);

    const ImVec2 arrow_center(bb.Max.x - 26.0f, bb.Min.y + 8.0f);
    if (popup_open) {
        window->DrawList->AddTriangleFilled(offset_vec2(arrow_center, -4.0f, 4.0f), offset_vec2(arrow_center, 4.0f, 4.0f),
                                            offset_vec2(arrow_center, 0.0f, -1.0f), phobia_active_u32());
    } else {
        window->DrawList->AddTriangleFilled(offset_vec2(arrow_center, -4.0f, -1.0f), offset_vec2(arrow_center, 4.0f, -1.0f),
                                            offset_vec2(arrow_center, 0.0f, 4.0f), phobia_muted_text_u32());
    }

    if (label != nullptr && label_size.x > 0.0f && std::strcmp(label, " ") != 0) {
        ImGui::RenderText(ImVec2(bb.Min.x + 5.0f, bb.Min.y - style.FramePadding.y - 20.0f), label);
    }

    bool value_changed = false;
    ImGui::SetNextWindowPos(ImVec2(bb.Min.x, bb.Max.y));
    ImGui::SetNextWindowSizeConstraints(ImVec2(bb.GetWidth(), 0.0f), ImVec2(FLT_MAX, 260.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, phobia_panel_alt_vec4());
    ImGui::PushStyleColor(ImGuiCol_Border, phobia_inner_border_vec4());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.FramePadding.x, style.WindowPadding.y));
    if (ImGui::BeginPopup(popup_id.c_str(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoSavedSettings)) {
        for (int i = 0; i < items_count; ++i) {
            if (ImGui::Selectable(items[i], *current_item == i)) {
                *current_item = i;
                value_changed = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    return value_changed;
}

bool phobia_input_text(const char* label, char* buffer, size_t buffer_size) {
    const float width = std::max(140.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddText(offset_vec2(pos, 20.0f, 0.0f), IM_COL32(255, 255, 255, 255), label);

    std::string hidden_label = std::string("##") + label;
    ImGui::SetCursorScreenPos(offset_vec2(pos, 15.0f, 18.0f));
    ImGui::PushItemWidth(width - 30.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, phobia_inner_panel_vec4());
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, phobia_inner_panel_vec4());
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, phobia_inner_panel_vec4());
    ImGui::PushStyleColor(ImGuiCol_Border, phobia_inner_border_vec4());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    const bool changed = ImGui::InputText(hidden_label.c_str(), buffer, buffer_size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    ImGui::PopItemWidth();
    ImGui::Dummy(ImVec2(width, 8.0f));
    return changed;
}

bool draw_close_button(float width, float height) {
    ImGui::InvisibleButton("##phobia_close", ImVec2(width, height));
    const bool clicked = ImGui::IsItemClicked();
    const ImVec2 min_pos = ImGui::GetItemRectMin();
    const ImVec2 max_pos = ImGui::GetItemRectMax();
    const float pad = 5.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImVec4(1, 1, 1, 1) : ImVec4(0.72f, 0.72f, 0.72f, 1.0f));
    draw->AddLine(ImVec2(min_pos.x + pad, min_pos.y + pad), ImVec2(max_pos.x - pad, max_pos.y - pad), col, 1.7f);
    draw->AddLine(ImVec2(max_pos.x - pad, min_pos.y + pad), ImVec2(min_pos.x + pad, max_pos.y - pad), col, 1.7f);
    return clicked;
}

void begin_card(const char* id, const char* title, const ImVec2& pos, const ImVec2& size,
                ImGuiWindowFlags child_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) {
    constexpr float kCardContentTopInset = 31.0f;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 root_pos = ImGui::GetWindowPos();
    const ImVec2 root_size = ImGui::GetWindowSize();
    const ImVec2 card_pos(root_pos.x + pos.x, root_pos.y + pos.y);
    const float draw_height = std::max(1.0f, std::min(size.y + 16.0f, root_size.y - pos.y - 1.0f));
    draw->AddRectFilled(card_pos, offset_vec2(card_pos, size.x, draw_height), phobia_panel_u32(), 5.0f);
    draw->AddRect(card_pos, offset_vec2(card_pos, size.x, draw_height), phobia_border_u32(), 5.0f);
    draw->AddText(choose_font(g_menu_body_font), choose_font_size(g_menu_body_font, 17.0f), offset_vec2(card_pos, 10.0f, 8.0f),
                  phobia_muted_text_u32(), title);

    ImGui::SetCursorPos(offset_vec2(pos, 0.0f, kCardContentTopInset));
    ImGui::BeginChild(id, ImVec2(size.x, std::max(1.0f, draw_height - kCardContentTopInset)), 0,
                      ImGuiWindowFlags_NoBackground | child_flags);
}

void end_card() {
    ImGui::EndChild();
}

void add_control_gap(float height = 6.0f) {
    ImGui::Dummy(ImVec2(0.0f, height));
}

void draw_particles() {
    PreviewState& preview = preview_state();
    if (!preview.particles) {
        return;
    }

    const ImVec2 screen_size = ImGui::GetIO().DisplaySize;
    static ImVec2 particle_pos[100]{};
    static ImVec2 particle_target_pos[100]{};
    static float particle_speed[100]{};
    static float particle_radius[100]{};

    for (int i = 1; i < 50; ++i) {
        if (particle_pos[i].x == 0.0f || particle_pos[i].y == 0.0f) {
            particle_pos[i].x = static_cast<float>(std::rand() % std::max(1, static_cast<int>(screen_size.x)));
            particle_pos[i].y = 15.0f;
            particle_speed[i] = 1.0f + static_cast<float>(std::rand() % 25);
            particle_radius[i] = static_cast<float>(std::rand() % 4);

            particle_target_pos[i].x = static_cast<float>(std::rand() % std::max(1, static_cast<int>(screen_size.x)));
            particle_target_pos[i].y = screen_size.y * 2.0f;
        }

        particle_pos[i] = lerp_vec2(particle_pos[i], particle_target_pos[i],
                                    ImGui::GetIO().DeltaTime * (particle_speed[i] / 60.0f));
        if (particle_pos[i].y > screen_size.y) {
            particle_pos[i] = ImVec2(0.0f, 0.0f);
        }

        ImGui::GetWindowDrawList()->AddCircleFilled(particle_pos[i], particle_radius[i], phobia_accent_u32(0.5f));
    }
}

void draw_main_tab(PreviewState& preview, State& state) {
    static const char* hitbox_priority_types[] = {
        "Head",
        "Body",
        "All",
    };
    static const char* triggerbot_hitgroup_types[] = {
        "Head",
        "Chest",
        "Stomach",
        "Body",
        "Head + Body",
    };
    begin_card("##general_card", "General", ImVec2(169.0f, 38.0f), ImVec2(320.0f, 244.0f));
    ImGui::Spacing();
    phobia_checkbox("Aim", &state.aimbot_enabled);
    add_control_gap(4.0f);
    phobia_slider_float("Fov", &state.aimbot_fov, 15.0f, 320.0f, "%.0f");
    add_control_gap(10.0f);
    phobia_combo("Hitbox priority", &state.aimbot_hitbox_priority, hitbox_priority_types, IM_ARRAYSIZE(hitbox_priority_types));
    add_control_gap(6.0f);
    phobia_checkbox("Spread control", &state.spread_control_enabled);
    add_control_gap(4.0f);
    phobia_checkbox("AutoShift", &state.auto_shift_enabled);
    end_card();

    begin_card("##triggerbot_card", "Triggerbot", ImVec2(505.0f, 38.0f), ImVec2(320.0f, 286.0f));
    ImGui::Spacing();
    phobia_checkbox("Enable", &state.triggerbot_enabled);
    add_control_gap(4.0f);
    phobia_checkbox("Wall", &state.triggerbot_wall_check);
    add_control_gap(6.0f);
    phobia_combo("Hitgroup filter", &state.triggerbot_hitgroup, triggerbot_hitgroup_types, IM_ARRAYSIZE(triggerbot_hitgroup_types));
    add_control_gap(6.0f);
    phobia_slider_float("Delay before shot", &state.triggerbot_delay_ms, 0.0f, 250.0f, "%.0f ms");
    end_card();
}

void draw_players_tab(PreviewState& preview, State& state) {
    preview.player_box = state.esp_box;
    preview.player_hp = state.esp_hp_bar;
    preview.player_armor = state.esp_armor;
    preview.player_weapon = state.esp_weapon;
    preview.player_skeleton = state.esp_skeleton;

    begin_card("##players_visuals", "Players", ImVec2(169.0f, 38.0f), ImVec2(320.0f, 218.0f), 0);
    ImGui::Spacing();
    phobia_checkbox("Bounding box", &state.esp_box);
    phobia_color_circle_button("##players_box_color", state.esp_box_color);
    add_control_gap(4.0f);
    phobia_checkbox("Hp", &state.esp_hp_bar);
    phobia_color_circle_button("##players_hp_color", state.esp_hp_color);
    add_control_gap(4.0f);
    phobia_checkbox("Armor", &state.esp_armor);
    phobia_color_circle_button("##players_armor_color", state.esp_armor_color);
    add_control_gap(4.0f);
    phobia_checkbox("Weapons under box", &state.esp_weapon);
    add_control_gap(4.0f);
    phobia_checkbox("Bomb", &state.esp_bomb);
    add_control_gap(4.0f);
    phobia_checkbox("Skeleton", &state.esp_skeleton);
    phobia_color_circle_button("##players_skeleton_color", state.esp_skeleton_color);
    end_card();

    preview.player_box = state.esp_box;
    preview.player_hp = state.esp_hp_bar;
    preview.player_armor = state.esp_armor;
    preview.player_weapon = state.esp_weapon;
    preview.player_skeleton = state.esp_skeleton;
    state.esp_enabled =
        state.esp_box || state.esp_hp_bar || state.esp_armor || state.esp_weapon || state.esp_bomb || state.esp_head ||
        state.esp_skeleton || state.bomb_info_enabled;

    begin_card("##players_preview", "Preview actions", ImVec2(505.0f, 38.0f), ImVec2(320.0f, 464.0f));
#ifdef _WIN32
    if (g_menu_players_preview.shader_resource != nullptr) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 window_pos = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();
        const ImVec2 preview_min = offset_vec2(window_pos, 18.0f, 38.0f);
        const ImVec2 preview_max = offset_vec2(window_pos, window_size.x - 18.0f, window_size.y - 14.0f);
        ImRect image_rect = fit_texture_rect(preview_min, preview_max, g_menu_players_preview.width, g_menu_players_preview.height);
        image_rect.Min.y -= 18.0f;
        image_rect.Max.y -= 18.0f;
        draw->AddImage((ImTextureID)g_menu_players_preview.shader_resource, image_rect.Min, image_rect.Max);
        draw_players_preview_overlay(draw, image_rect, state);
    }
#endif
    end_card();
}

void draw_world_tab(PreviewState& preview, State& state) {
    preview.world_bomb = state.bomb_info_enabled;
    begin_card("##world_card", "World", ImVec2(169.0f, 38.0f), ImVec2(320.0f, 220.0f));
    ImGui::Spacing();
    phobia_checkbox("Bomb info", &state.bomb_info_enabled);
    add_control_gap(4.0f);
    phobia_checkbox("Grenades", &preview.world_grenades);
    add_control_gap(4.0f);
    phobia_checkbox("Dropped weapons", &preview.player_weapon);
    end_card();

    preview.world_bomb = state.bomb_info_enabled;
    state.esp_enabled = state.esp_box || state.esp_hp_bar || state.esp_armor || state.esp_weapon || state.esp_bomb ||
                        state.esp_head || state.esp_skeleton || state.bomb_info_enabled;

    begin_card("##view_card", "View", ImVec2(169.0f, 276.0f), ImVec2(320.0f, 226.0f));
    ImGui::Spacing();
    phobia_checkbox("Viewmodel FOV", &preview.view_fov);
    add_control_gap(6.0f);
    phobia_slider_float("Camera FOV", &preview.viewmodel, 50.0f, 120.0f, "%.0f");
    end_card();

    begin_card("##actions_card", "Actions", ImVec2(505.0f, 38.0f), ImVec2(320.0f, 464.0f));
    phobia_button("Save theme", ImVec2(-1.0f, 30.0f));
    add_control_gap(6.0f);
    phobia_button("Load theme", ImVec2(-1.0f, 30.0f));
    add_control_gap(6.0f);
    phobia_button("Reset theme", ImVec2(-1.0f, 30.0f));
    end_card();
}

void draw_misc_tab(PreviewState& preview) {
    static const char* config_slots[] = {"Default", "Legit", "Rage", "HVH"};

    begin_card("##misc_main", "Main", ImVec2(169.0f, 38.0f), ImVec2(320.0f, 220.0f));
    ImGui::Spacing();
    phobia_checkbox("Bunny hop", &preview.misc_bhop);
    add_control_gap(4.0f);
    phobia_checkbox("Hit sound", &preview.misc_hit_sound);
    add_control_gap(4.0f);
    phobia_checkbox("Menu particles", &preview.particles);
    end_card();

    begin_card("##misc_cfg", "Configs", ImVec2(169.0f, 276.0f), ImVec2(320.0f, 226.0f));
    phobia_combo("Slot", &preview.config_slot, config_slots, IM_ARRAYSIZE(config_slots));
    add_control_gap(4.0f);
    phobia_input_text("Name", preview.search, sizeof(preview.search));
    phobia_button("Save config", ImVec2(-1.0f, 28.0f));
    add_control_gap(6.0f);
    phobia_button("Load config", ImVec2(-1.0f, 28.0f));
    end_card();

    begin_card("##misc_about", "About", ImVec2(505.0f, 38.0f), ImVec2(320.0f, 464.0f));
    ImGui::TextWrapped("Phobia interface preview is active. This menu now uses the original bytearray fonts, tab icons and images from the menu phobia folder.");
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    phobia_button("Check updates", ImVec2(-1.0f, 30.0f));
    add_control_gap(6.0f);
    phobia_button("Open changelog", ImVec2(-1.0f, 30.0f));
    end_card();
}

} // namespace

bool try_add_font(ImGuiIO& io, float) {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,
        0x0400, 0x052F,
        0x2DE0, 0x2DFF,
        0xA640, 0xA69F,
        0xE000, 0xE226,
        0,
    };

    ImFontConfig base_config{};
    base_config.PixelSnapH = false;
    base_config.OversampleH = 5;
    base_config.OversampleV = 5;
    base_config.RasterizerMultiply = 1.2f;
    base_config.FontDataOwnedByAtlas = false;

    ImFontConfig title_config = base_config;
    ImFontConfig icon_config = base_config;

    g_menu_body_font = io.Fonts->AddFontFromMemoryTTF(poppin_font, sizeof(poppin_font), 16.0f, &base_config, ranges);
    g_menu_label_font = io.Fonts->AddFontFromMemoryTTF(poppin_font, sizeof(poppin_font), 19.0f, &title_config, ranges);
    g_menu_title_font = io.Fonts->AddFontFromMemoryTTF(poppin_font, sizeof(poppin_font), 25.0f, &title_config, ranges);
    g_menu_icon_font = io.Fonts->AddFontFromMemoryTTF(icon_font, sizeof(icon_font), 25.0f, &icon_config, ranges);

    return g_menu_body_font != nullptr;
}

void apply_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.FramePadding = ImVec2(1.0f, 0.0f);
    style.FrameRounding = 3.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.ScrollbarRounding = 3.0f;
    style.ScrollbarSize = 5.0f;
    style.WindowPadding = ImVec2(8.0f, 8.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(230.0f / 255.0f, 234.0f / 255.0f, 242.0f / 255.0f, 1.0f);
    colors[ImGuiCol_TextDisabled] = phobia_idle_text_vec4();
    colors[ImGuiCol_WindowBg] = phobia_frame_vec4();
    colors[ImGuiCol_ChildBg] = phobia_panel_vec4();
    colors[ImGuiCol_PopupBg] = colors[ImGuiCol_ChildBg];
    colors[ImGuiCol_Border] = phobia_border_vec4();
    colors[ImGuiCol_FrameBg] = phobia_inner_panel_vec4();
    colors[ImGuiCol_FrameBgHovered] = phobia_panel_alt_vec4();
    colors[ImGuiCol_FrameBgActive] = phobia_panel_active_vec4();
    colors[ImGuiCol_Button] = phobia_button_default_vec4();
    colors[ImGuiCol_ButtonHovered] = phobia_button_hover_vec4();
    colors[ImGuiCol_ButtonActive] = phobia_active_vec4();
    colors[ImGuiCol_CheckMark] = phobia_accent_vec4();
    colors[ImGuiCol_SliderGrab] = ImVec4(167.0f / 255.0f, 196.0f / 255.0f, 1.0f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

bool initialize_visual_assets(ID3D11Device* device) {
#ifdef _WIN32
    if (device == nullptr) {
        return false;
    }

    if (g_menu_device != device) {
        shutdown_visual_assets();
        g_menu_device = device;
        g_menu_device->AddRef();
    }

    if (g_menu_logo.shader_resource == nullptr) {
        g_menu_custom_logo_loaded = load_texture_from_file(find_custom_menu_logo_path(), g_menu_logo);
        if (!g_menu_custom_logo_loaded) {
            load_texture_from_memory(logo_one, sizeof(logo_one), g_menu_logo);
        }
    }
    if (!g_menu_custom_logo_loaded && g_menu_logo_alt.shader_resource == nullptr) {
        load_texture_from_memory(logo_two, sizeof(logo_two), g_menu_logo_alt);
    }
    if (g_menu_user.shader_resource == nullptr) {
        if (!load_animated_texture_from_file(find_custom_user_avatar_path(), g_menu_user)) {
            load_texture_from_memory(user, sizeof(user), g_menu_user);
        }
    }
    if (g_menu_players_preview.shader_resource == nullptr) {
        load_texture_from_file(find_players_preview_image_path(), g_menu_players_preview);
    }

    return g_menu_logo.shader_resource != nullptr || g_menu_logo_alt.shader_resource != nullptr ||
           g_menu_user.shader_resource != nullptr || g_menu_players_preview.shader_resource != nullptr;
#else
    (void)device;
    return false;
#endif
}

void shutdown_visual_assets() {
#ifdef _WIN32
    release_texture(g_menu_background);
    release_texture(g_menu_logo);
    release_texture(g_menu_logo_alt);
    release_texture(g_menu_user);
    release_texture(g_menu_players_preview);

    if (g_menu_wic_factory != nullptr) {
        g_menu_wic_factory->Release();
        g_menu_wic_factory = nullptr;
    }
    if (g_menu_device != nullptr) {
        g_menu_device->Release();
        g_menu_device = nullptr;
    }
    if (g_menu_com_initialized) {
        CoUninitialize();
        g_menu_com_initialized = false;
    }
    g_menu_custom_logo_loaded = false;
#endif
}

bool draw_menu(State& state) {
    PreviewState& preview = preview_state();
    preview.particles = true;

    ImGui::SetNextWindowPos(ImVec2(state.menu_x, state.menu_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kWindowWidth, kWindowHeight), ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##phobia_root", nullptr, flags);
    bool close_requested = false;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 right_min(pos.x + 160.0f, pos.y);
    const ImVec2 right_max(pos.x + size.x, pos.y + size.y);
    const ImVec2 right_grad_min(right_min.x + 1.0f, right_min.y + 1.0f);
    const ImVec2 right_grad_max(right_max.x - 1.0f, right_max.y - 1.0f);

    draw->AddRectFilled(pos, ImVec2(pos.x + 161.0f, pos.y + size.y), phobia_sidebar_bg_u32(), 10.0f, ImDrawFlags_RoundCornersLeft);
    draw->AddRectFilledMultiColor(ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImVec2(pos.x + 160.0f, pos.y + size.y - 1.0f),
                                  phobia_sidebar_glow_u32(0.34f), phobia_sidebar_glow_u32(0.0f),
                                  phobia_sidebar_glow_u32(0.0f), phobia_sidebar_glow_u32(0.18f));
    draw->AddRect(pos, ImVec2(pos.x + 161.0f, pos.y + size.y), phobia_border_u32(), 10.0f, ImDrawFlags_RoundCornersLeft);
    draw->AddRectFilled(right_min, right_max, phobia_frame_u32(), 10.0f, ImDrawFlags_RoundCornersRight);
    draw->AddRectFilledMultiColor(right_grad_min, right_grad_max, phobia_gradient_u32(0.42f), phobia_gradient_u32(0.28f),
                                  phobia_frame_u32(0.06f), phobia_frame_u32(0.02f));
    draw->AddRect(right_min, right_max, phobia_border_u32(), 10.0f,
                  ImDrawFlags_RoundCornersRight);

#ifdef _WIN32
    if (g_menu_custom_logo_loaded && g_menu_logo.shader_resource != nullptr) {
        draw_texture_fit(draw, (ImTextureID)g_menu_logo.shader_resource, offset_vec2(pos, 18.0f, 12.0f), offset_vec2(pos, 140.0f, 88.0f),
                         g_menu_logo.width, g_menu_logo.height);
    } else if (g_menu_logo.shader_resource != nullptr) {
        draw->AddImage((ImTextureID)g_menu_logo.shader_resource, offset_vec2(pos, 23.0f, 30.0f), offset_vec2(pos, 143.0f, 55.1f));
    }
    if (!g_menu_custom_logo_loaded && g_menu_logo_alt.shader_resource != nullptr) {
        draw->AddImage((ImTextureID)g_menu_logo_alt.shader_resource, offset_vec2(pos, 36.0f, -7.0f), offset_vec2(pos, 121.0f, 91.0f));
    }
#endif
    if (g_menu_logo.shader_resource == nullptr && g_menu_logo_alt.shader_resource == nullptr) {
        draw->AddText(choose_font(g_menu_title_font), choose_font_size(g_menu_title_font, 25.0f), offset_vec2(pos, 22.0f, 24.0f),
                      IM_COL32(255, 255, 255, 255), "PHOBIA");
    }

    ImGui::SetCursorPos(ImVec2(size.x - 28.0f, 10.0f));
    close_requested = draw_close_button(18.0f, 18.0f);

    draw->AddText(choose_font(g_menu_body_font), choose_font_size(g_menu_body_font, 17.0f), offset_vec2(pos, 13.0f, 94.0f),
                  phobia_muted_text_u32(), "Aimbot");
    draw->AddText(choose_font(g_menu_body_font), choose_font_size(g_menu_body_font, 17.0f), offset_vec2(pos, 13.0f, 149.0f),
                  phobia_muted_text_u32(), "Visuals");
    draw->AddText(choose_font(g_menu_body_font), choose_font_size(g_menu_body_font, 17.0f), offset_vec2(pos, 13.0f, 287.0f),
                  phobia_muted_text_u32(), "Miscellaneous");

    ImGui::SetCursorPos(ImVec2(13.0f, 112.0f));
    if (phobia_tab_button("##tab_legit", "r", "Legit Bot", preview.active_tab == 0)) preview.active_tab = 0;
    ImGui::SetCursorPos(ImVec2(13.0f, 167.0f));
    if (phobia_tab_button("##tab_players", "x", "Players", preview.active_tab == 3)) preview.active_tab = 3;
    ImGui::SetCursorPos(ImVec2(13.0f, 205.0f));
    if (phobia_tab_button("##tab_world", "w", "World", preview.active_tab == 4)) preview.active_tab = 4;
    ImGui::SetCursorPos(ImVec2(13.0f, 243.0f));
    if (phobia_tab_button("##tab_view", "v", "View", preview.active_tab == 5)) preview.active_tab = 5;
    ImGui::SetCursorPos(ImVec2(13.0f, 308.0f));
    if (phobia_tab_button("##tab_main", "z", "Main", preview.active_tab == 6)) preview.active_tab = 6;
    ImGui::SetCursorPos(ImVec2(13.0f, 346.0f));
    if (phobia_tab_button("##tab_inventory", "s", "Inventory", preview.active_tab == 7)) preview.active_tab = 7;
    ImGui::SetCursorPos(ImVec2(13.0f, 384.0f));
    if (phobia_tab_button("##tab_configs", "c", "Configs", preview.active_tab == 8)) preview.active_tab = 8;

    draw->AddRectFilled(offset_vec2(pos, 9.0f, 486.0f), offset_vec2(pos, 152.0f, 523.0f), phobia_sidebar_item_u32(), 5.0f);
    draw->AddRect(offset_vec2(pos, 9.0f, 486.0f), offset_vec2(pos, 152.0f, 523.0f), phobia_soft_border_u32(0.85f), 5.0f);
#ifdef _WIN32
    ID3D11ShaderResourceView* user_avatar = current_texture_view(g_menu_user);
    if (user_avatar != nullptr) {
        const ImVec2 avatar_min = offset_vec2(pos, 15.0f, 490.0f);
        const ImVec2 avatar_max = offset_vec2(pos, 43.0f, 518.0f);
        draw->AddImageRounded((ImTextureID)user_avatar, avatar_min, avatar_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                              IM_COL32_WHITE, 14.0f, ImDrawFlags_RoundCornersAll);
        draw->AddCircle(ImVec2((avatar_min.x + avatar_max.x) * 0.5f, (avatar_min.y + avatar_max.y) * 0.5f), 14.5f,
                        phobia_soft_border_u32(0.95f), 0, 1.0f);
    }
#endif
    add_colored_text(offset_vec2(pos, 49.0f, 488.0f), phobia_muted_text_u32(), IM_COL32(255, 255, 255, 255), "MikDo");
    add_colored_text(offset_vec2(pos, 49.0f, 503.0f), phobia_muted_text_u32(), IM_COL32(255, 255, 255, 255),
                     "Till: (19.03.2026)");

    switch (preview.active_tab) {
    case 0:
    case 1:
    case 2:
        draw_main_tab(preview, state);
        break;
    case 3:
        draw_players_tab(preview, state);
        break;
    case 4:
    case 5:
        draw_world_tab(preview, state);
        break;
    case 6:
    case 7:
    case 8:
        draw_misc_tab(preview);
        break;
    default:
        draw_main_tab(preview, state);
        break;
    }

    draw_particles();

    ImGui::End();
    ImGui::PopStyleVar();
    return close_requested;
}

} // namespace timmy::ui
