#pragma once

struct ImDrawList;

namespace timmy::ui {
struct State;
}

namespace timmy::features {

void draw_aim_overlay(ImDrawList* draw_list, const ui::State& state, int screen_width, int screen_height,
                      bool overlay_mode);

} // namespace timmy::features

