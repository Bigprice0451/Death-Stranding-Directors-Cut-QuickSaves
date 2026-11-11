#pragma once
#include <cstdint>
namespace qs {
void charging_repair_panel_start(uintptr_t verified_game);
void charging_repair_panel_stop();
void charging_repair_panel_tick();
void draw_charging_repair_panel();
}
