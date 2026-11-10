#include "vehicle_check.hpp"
#include "native_image.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <map>
#include <tuple>

namespace qs {
namespace {
bool road_vehicle(const VehicleArchiveEntry &row) {
    return row.kind == VehicleArchiveKind::motorbike || row.kind == VehicleArchiveKind::truck ||
           row.kind == VehicleArchiveKind::roadster;
}
bool same(const LiveVehicle &live, const VehicleArchiveEntry &saved) {
    return road_vehicle(saved) && live.world == saved.world && live.actor == saved.actor_id &&
           live.persistent_id == saved.persistent_id;
}
template <class A, class B> double distance(const A &a, const B &b) {
    return std::hypot(double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]);
}
double rotation_delta(const std::array<float, 4> &a, const std::array<float, 4> &b) {
    // q and -q encode the same rotation. Normalize the dot for small float errors.
    double dot = 0, aa = 0, bb = 0;
    for (unsigned i = 0; i < 4; ++i) {
        dot += double(a[i]) * b[i];
        aa += double(a[i]) * a[i];
        bb += double(b[i]) * b[i];
    }
    if (aa < 0.95 || aa > 1.05 || bb < 0.95 || bb > 1.05)
        return 3.14159265;
    return 2 * std::acos(std::clamp(std::abs(dot) / std::sqrt(aa * bb), 0., 1.));
}
} // namespace
std::optional<NearbyCargo> summarize_vehicle_cargo(const VehicleFrame &live) {
    if (!live.complete || live.vehicles.empty())
        return {};
    // Sorting by world and persistent identity makes per-vehicle item ownership
    // part of the checksum, independent of engine enumeration order.
    std::vector<const LiveVehicle *> sorted;
    for (const auto &v : live.vehicles) {
        if (!v.cargo)
            return {};
        sorted.push_back(&v);
    }
    auto key = [](const LiveVehicle *v) { return std::tie(v->world, v->actor, v->persistent_id); };
    std::sort(sorted.begin(), sorted.end(), [&](auto a, auto b) { return key(a) < key(b); });
    Bytes bytes{1};
    auto add = [&](const auto &value) {
        auto p = reinterpret_cast<const uint8_t *>(&value);
        bytes.insert(bytes.end(), p, p + sizeof value);
    };
    NearbyCargo summary;
    const LiveVehicle *previous = nullptr;
    for (auto v : sorted) {
        if (previous && key(previous) == key(v))
            return {};
        previous = v;
        add(v->world);
        add(v->actor);
        add(v->persistent_id);
        add(v->cargo->count);
        add(v->cargo->digest);
        ++summary.vehicles;
        summary.items += v->cargo->count;
    }
    summary.digest = sha256(bytes);
    return summary;
}

VehicleCaptureReport align_private_vehicle_poses(Bytes &image, const VehicleFrame &live) {
    VehicleCaptureReport report;
    if (live.vehicles.empty())
        return report;
    const auto archive = inspect_vehicle_archive(image);
    using Key = std::tuple<uint16_t, uint32_t, std::array<uint8_t, 16>>;
    std::map<Key, unsigned> observations;
    for (const auto &v : live.vehicles)
        ++observations[{v.world, v.actor, v.persistent_id}];
    for (const auto &actor : live.vehicles) {
        double norm = 0;
        for (float q : actor.orientation)
            norm += double(q) * q;
        if (observations.at({actor.world, actor.actor, actor.persistent_id}) != 1 || !std::isfinite(norm) ||
            std::abs(norm - 1) > 0.02 ||
            !std::all_of(actor.position.begin(), actor.position.end(),
                         [](float x) { return std::isfinite(x); })) {
            ++report.unavailable;
            continue;
        }
        auto matches = [&](const auto &row) { return same(actor, row); };
        auto it = std::find_if(archive.vehicles.begin(), archive.vehicles.end(), matches);
        if (it == archive.vehicles.end() ||
            std::find_if(it + 1, archive.vehicles.end(), matches) != archive.vehicles.end()) {
            ++report.unavailable;
            continue;
        }
        // Native bit 0x400 selects an authored return transform on certain bikes.
        // The physical position at this moment does not supersede that game state.
        if (it->packed_fields[0x43 - 0x30] & 4) {
            ++report.authored;
            continue;
        }
        ++report.checked;
        if (distance(it->position, actor.position) <= 0.001 &&
            rotation_delta(it->orientation, actor.orientation) <= 0.0005)
            continue;
        auto *row = image.data() + it->image_offset;
        std::memcpy(row + 0x10, actor.position.data(), 12);
        std::memcpy(row + 0x1c, actor.orientation.data(), 16);
        ++report.adjusted;
    }
    if (report.adjusted)
        update_native_payload_digest(image);
    return report;
}

std::string compare_loaded_vehicles(const VehicleArchive &saved, const VehicleFrame &live,
                                    const std::optional<Position> &saved_sam,
                                    std::optional<uint32_t> saved_seat, bool *waiting_for_vehicles) {
    if (waiting_for_vehicles)
        *waiting_for_vehicles = false;
    if (saved.status != VehicleArchiveStatus::readable || !saved_sam)
        return "vehicle observation unavailable";
    if (!live.complete) {
        if (waiting_for_vehicles)
            *waiting_for_vehicles = true;
        return "vehicle observation unavailable";
    }
    unsigned expected = 0, found = 0, life_changed = 0, battery_changed = 0;
    unsigned life_unreadable = 0, battery_unreadable = 0;
    double max_distance = 0, max_angle = 0;
    for (const auto &row : saved.vehicles) {
        if (!road_vehicle(row) || row.world != live.world ||
            (saved_seat != row.actor_id && distance(row.position, *saved_sam) > vehicle_radius))
            continue;
        ++expected;
        auto it = std::find_if(live.vehicles.begin(), live.vehicles.end(),
                               [&](const auto &actor) { return same(actor, row); });
        if (it == live.vehicles.end())
            continue;
        ++found;
        max_distance = std::max(max_distance, distance(it->position, row.position));
        max_angle = std::max(max_angle, rotation_delta(it->orientation, row.orientation));
        uint16_t life{};
        std::memcpy(&life, row.packed_fields.data() + 4, sizeof life);
        if (!it->life_known)
            ++life_unreadable;
        else if (it->life != life)
            ++life_changed;
        std::array<uint16_t, 2> battery{};
        std::memcpy(battery.data(), row.packed_fields.data() + 8, sizeof battery);
        if (!it->battery_known)
            ++battery_unreadable;
        else if (it->battery != battery[0] || it->capacity != battery[1])
            ++battery_changed;
    }
    // Bike/truck reattachment is a separate native update after the world reader.
    // A roadster can restore parked even when its rider ID was saved.
    const bool rider_expected =
        saved_seat && (((*saved_seat >> 13) & 255) == 15 || ((*saved_seat >> 13) & 255) == 16);
    if (waiting_for_vehicles)
        *waiting_for_vehicles = expected != found || (rider_expected && saved_seat != live.seat);
    std::ostringstream out;
    out << "matched=" << found << "/" << expected << " max_distance=" << max_distance
        << " max_angle_deg=" << max_angle * 57.29578 << " life_changed=" << life_changed
        << " battery_changed=" << battery_changed << " life_unreadable=" << life_unreadable
        << " battery_unreadable=" << battery_unreadable << " seat=";
    if (!saved_seat || !live.seat)
        out << "unknown";
    else
        out << (*saved_seat == *live.seat ? "match" : "different");
    return out.str();
}
} // namespace qs
