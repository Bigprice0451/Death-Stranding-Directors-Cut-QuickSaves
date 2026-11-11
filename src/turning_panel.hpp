#pragma once
#include <cstdint>
namespace qs {
void turning_panel_start(uintptr_t verified_game);
void turning_panel_stop();
void turning_panel_tick();
void draw_turning_panel();
}
