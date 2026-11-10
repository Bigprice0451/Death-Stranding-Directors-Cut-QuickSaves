#pragma once
#include "delivery_sample.hpp"
#include <optional>

namespace qs {
using Position = std::array<double, 3>;
constexpr double vehicle_radius = 100.0; // world units from Sam
constexpr uint32_t no_ride = 0x1fffff;

// These identifiers are scoped to a game world. Never persist an entity pointer.
struct LiveVehicle {
    uint16_t world{};
    uint32_t actor{};
    std::array<uint8_t, 16> persistent_id{};
    std::array<float, 3> position{};
    std::array<float, 4> orientation{};
    uint16_t life{}, battery{}, capacity{};
    bool life_known{}, battery_known{};
    std::optional<InventorySignature> cargo;
};
struct VehicleFrame {
    bool complete{};
    uint16_t world{};
    std::optional<uint32_t> seat;
    std::vector<LiveVehicle> vehicles;
};
namespace vehicle_detail {
using QuaternionFromMatrix = void (*)(float *, const float *);
VehicleFrame collect(uintptr_t game, uintptr_t manager, uint16_t world, std::optional<uint32_t> seat,
                     const Position &sam, QuaternionFromMatrix quaternion);
} // namespace vehicle_detail
// Only called on a regular game update, before capture or after streaming.
VehicleFrame sample_vehicles(uintptr_t game, const std::optional<Position> &sam);
} // namespace qs
