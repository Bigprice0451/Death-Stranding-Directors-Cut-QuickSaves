#pragma once
#include "library.hpp"
#include <cstddef>

namespace qs {
enum class VehicleArchiveStatus { readable, missing, malformed, ambiguous, unsupported };
enum class VehicleArchiveKind { motorbike, truck, roadster, unclassified, removal };

struct VehicleArchiveEntry {
    std::uint16_t world{}, group{};
    std::uint32_t actor_id{};
    VehicleArchiveKind kind = VehicleArchiveKind::unclassified;
    std::array<std::uint8_t, 16> persistent_id{};
    std::array<float, 3> position{};
    std::array<float, 4> orientation{}; // Game quaternion order is x, y, z, w.
    // Everything after actor_id. This includes the auxiliary tag and packed state.
    // Keep these bytes intact without assigning meanings to unverified subfields.
    std::array<std::uint8_t, 0x44> packed_fields{};
    std::size_t image_offset{}; // Byte zero is the start of the native header.
};

struct VehicleArchive {
    VehicleArchiveStatus status = VehicleArchiveStatus::missing;
    std::vector<VehicleArchiveEntry> vehicles;
};

// Read-only diagnostic for build 13300582. Pass the native image after
// validate_native_image() has checked its header, payload length and MD5.
// An empty readable result means the manager saved no rows. Other statuses
// have no rows and must never be interpreted as proof that a vehicle vanished.
VehicleArchive inspect_vehicle_archive(const Bytes &native_image);
} // namespace qs
