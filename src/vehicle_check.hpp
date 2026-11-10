#pragma once
#include "vehicle_archive.hpp"
#include "vehicle_live.hpp"

namespace qs {
struct NearbyCargo {
    uint32_t vehicles{}, items{};
    Digest digest{};
};
std::optional<NearbyCargo> summarize_vehicle_cargo(const VehicleFrame &live);

// Operates only on a new private image freshly produced by the game, before disk I/O.
struct VehicleCaptureReport {
    unsigned checked{}, adjusted{}, unavailable{}, authored{};
};
VehicleCaptureReport align_private_vehicle_poses(Bytes &image, const VehicleFrame &live);
// Compare matching vehicles once the engine's load and streaming phases finish.
std::string compare_loaded_vehicles(const VehicleArchive &saved, const VehicleFrame &live,
                                    const std::optional<Position> &saved_sam,
                                    std::optional<uint32_t> saved_seat, bool *waiting_for_vehicles = nullptr);
} // namespace qs
