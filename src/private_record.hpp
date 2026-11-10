#pragma once
#include "cinematic.hpp"
#include "vehicle_check.hpp"
#include "telemetry.hpp"

namespace qs {
// The archive keeps the screen, world image and matching scene clocks together.
// The game itself only receives image, never the mod's outer record or PNG.
struct PrivateRecord {
    WorldSample world;
    SceneStamps scenes;
    Bytes image;
    std::optional<uint32_t> vehicle_seat; // Empty on older captures or unavailable player.
    std::optional<NearbyCargo> vehicle_cargo;
    VehicleArchive vehicles; // Parsed off the game thread for post-load verification.
};
Bytes pack_record(const PrivateRecord &record, const Bytes &png);
PrivateRecord unpack_record(const Bytes &archive);
}
