#pragma once
#include <cstdint>
namespace reshade::api {struct effect_runtime;}
namespace qs {
void ultrawide_panel_start(uintptr_t verified_game, void *window);
void ultrawide_panel_tick(reshade::api::effect_runtime *runtime);
void draw_ultrawide_panel();
}
