#pragma once

#include <cstdint>

struct ImDrawList;
struct ImFont;
struct ID3D11Device;

namespace timmy::ui {
struct State;
}

namespace timmy::features {

void draw_esp_overlay(ImDrawList* draw_list, const ui::State& state, int screen_width, int screen_height,
                      bool overlay_mode);
bool query_game_avg_fps(float& out_fps);
void set_esp_weapon_font(ImFont* font);
void set_esp_weapon_icon_font(ImFont* font);
ImFont* get_esp_weapon_font();
ImFont* get_esp_weapon_icon_font();
const char* get_esp_preview_weapon_icon();
const char* get_esp_preview_bomb_icon();
bool initialize_esp_weapon_icons(ID3D11Device* device);
void shutdown_esp_weapon_icons();

} // namespace timmy::features
