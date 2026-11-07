#pragma once
#include <cstdint>
namespace reshade::api {struct effect_runtime;}
namespace qs {
// Call for the owning ReShade runtime of a verified game. Graphics preferences
// and the official runtime download are independent of the save queue.
void graphics_start(reshade::api::effect_runtime *rt,uintptr_t verified_base);
void graphics_tick();
void graphics_stop();
void draw_graphics_panel(reshade::api::effect_runtime *rt);
}
