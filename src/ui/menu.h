#pragma once

struct ImGuiIO;
struct ID3D11Device;

namespace timmy::ui {

struct State;

void apply_style();
bool try_add_font(ImGuiIO& io, float size_px);
bool initialize_visual_assets(ID3D11Device* device);
void shutdown_visual_assets();
bool draw_menu(State& state);

} // namespace timmy::ui
