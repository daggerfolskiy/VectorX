#pragma once

#include <string>

namespace timmy::ui {

struct State {
    bool esp_enabled = false;
    bool esp_box = false;
    bool esp_team = false;
    bool esp_hp_bar = false;
    bool esp_armor = false;
    bool esp_head = false;
    bool esp_skeleton = false;
    bool esp_weapon = false;
    bool esp_bomb = false;
    bool bomb_info_enabled = false;
    char esp_text[128] = "Text";
    float esp_box_color[4] = {255.0f / 255.0f, 110.0f / 255.0f, 105.0f / 255.0f, 1.0f};
    float esp_hp_color[4] = {90.0f / 255.0f, 220.0f / 255.0f, 120.0f / 255.0f, 1.0f};
    float esp_armor_color[4] = {80.0f / 255.0f, 165.0f / 255.0f, 255.0f / 255.0f, 1.0f};
    float esp_skeleton_color[4] = {245.0f / 255.0f, 245.0f / 255.0f, 245.0f / 255.0f, 1.0f};

    std::string active_tab = "main";
    float menu_x = 0.0f;
    float menu_y = 0.0f;

    bool aimbot_enabled = false;
    float aimbot_fov = 120.0f;
    int aimbot_hitbox_priority = 0;
    bool spread_control_enabled = false;
    bool auto_shift_enabled = true;
    bool triggerbot_enabled = false;
    int triggerbot_hitgroup = 4;
    bool triggerbot_wall_check = true;
    float triggerbot_delay_ms = 0.0f;

    bool glow_enabled = false;
    float glow_color_r = 0.22f;
    float glow_color_g = 0.86f;
    float glow_color_b = 0.27f;

    bool show_fps = false;
    bool watermark_enabled = false;
    float watermark_x = 14.0f;
    float watermark_y = 42.0f;
};

} // namespace timmy::ui
