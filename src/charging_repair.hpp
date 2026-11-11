#pragma once
#include <cstdint>

namespace qs {
struct ChargingRepairStatus {
    bool ready{}, enabled{};
    uint64_t charging_updates{}, repair_updates{}, excluded_updates{};
};
// The host must first verify the complete supported executable. Callbacks stay
// pinned across ReShade runtime recreation; stopping only suspends the effect.
void start_charging_repair(uintptr_t verified_game, bool enabled);
void set_charging_repair_enabled(bool enabled);
ChargingRepairStatus charging_repair_status();
namespace charging_repair_detail {
// Used by the isolated fixture after it verifies its own original executable.
void install(uintptr_t verified_image);
}
}
