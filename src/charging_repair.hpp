#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace qs {
enum class ChargingRepairReason : uint8_t {
    disabled, invalid_time, unavailable_world, blocked_scene, invalid_vehicle,
    changed_vehicle, no_charger, charger_unavailable, no_transfer,
    invalid_condition, destroyed, condition_full, pending_fatal_damage, repaired, count
};
struct ChargingRepairObservation {
    uint32_t actor_id{};
    ChargingRepairReason reason{ChargingRepairReason::no_charger};
    float battery{}, capacity{}, condition{}, maximum{}, pending_damage{}, dt{};
    uint64_t game_flags{};
    uint32_t battery_flags{};
    bool valid{};
};
struct ChargingRepairStatus {
    bool ready{}, enabled{};
    uint64_t charging_updates{}, repair_updates{}, excluded_updates{};
    std::array<uint64_t, static_cast<size_t>(ChargingRepairReason::count)> reasons{};
    ChargingRepairObservation latest;
    uint64_t observed_at_ms{}, repaired_at_ms{};
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
