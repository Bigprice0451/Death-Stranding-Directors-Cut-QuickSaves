#include "vehicle_live.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace qs {
namespace {
// Steam 13300582 only. The host has verified the complete executable before calling here.
// See VEHICLES.md for the matching game functions and the lifetime of each pointer.
constexpr uintptr_t manager_rva = 0x7bfecc0, world_rva = 0x7be6318, players_rva = 0x7bc7568;
constexpr uintptr_t vehicle_types[] = {0x3e26070, 0x3e26a48, 0x3e27790};
using Address = uintptr_t;
bool read_bytes(Address address, void *out, size_t size) {
    SIZE_T got{};
    return address &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), out, size, &got) &&
           got == size;
}
template <class T> bool read(Address address, T &value) { return read_bytes(address, &value, sizeof(value)); }
std::optional<uint32_t> rider(Address game) {
    Address players{}, player{};
    if (!read(game + players_rva, players) || !players)
        return {};
    using FindPlayer = Address (*)(Address, int);
    player = reinterpret_cast<FindPlayer>(game + 0x23c60b0)(players, 0);
    uint32_t id{};
    if (!player || !read(player + 0x2b0 + 0x10c, id))
        return {};
    return id;
}
bool read_vehicle(Address game, Address actor, const Position &sam, VehicleFrame &frame,
                  vehicle_detail::QuaternionFromMatrix quaternion) {
    Address type{};
    if (!read(actor, type))
        return false;
    if (std::find(std::begin(vehicle_types), std::end(vehicle_types), type - game) == std::end(vehicle_types))
        return true;
    LiveVehicle v;
    uint8_t active{};
    uint32_t state{};
    Address link{};
    if (!read(actor + 0x20, active) || !read(actor + 0x24, state) || !read(actor + 0x1a4, v.world))
        return false;
    if (!active || state != 2 || v.world != frame.world)
        return true;
    if (!read(actor + 8, v.actor) || !read(actor + 0x68, link) || link < 0x20)
        return false;
    const Address entity = link - 0x20;
    struct Transform {
        Position xyz;
        std::array<float, 9> axes;
    };
    static_assert(offsetof(Transform, axes) == 24);
    Transform pose{};
    if (!read_bytes(entity + 0xc8, &pose, 60))
        return false;
    for (double coord : pose.xyz)
        if (!std::isfinite(coord))
            return false;
    if (frame.seat != v.actor &&
        std::hypot(pose.xyz[0] - sam[0], pose.xyz[1] - sam[1], pose.xyz[2] - sam[2]) > vehicle_radius)
        return true;
    for (int i = 0; i < 3; ++i) {
        v.position[i] = static_cast<float>(pose.xyz[i]);
        if (!std::isfinite(v.position[i]))
            return false;
    }
    Address locator{};
    if (!read(actor + 0x18, locator) || !read(locator ? locator + 8 : actor + 0x188, v.persistent_id))
        return false;
    if (std::all_of(v.persistent_id.begin(), v.persistent_id.end(), [](uint8_t b) { return b == 0; }))
        return false;
    // Build the same padded row matrix and call the same pure quaternion conversion
    // as the native vehicle writer. No game memory is written.
    alignas(16) float matrix[16]{}, quat[4]{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            const float component = pose.axes[row * 3 + col];
            if (!std::isfinite(component))
                return false;
            matrix[row * 4 + col] = component;
        }
    matrix[15] = 1;
    quaternion(quat, matrix);
    float norm = 0;
    for (int i = 0; i < 4; ++i) {
        v.orientation[i] = quat[i];
        norm += quat[i] * quat[i];
    }
    if (!std::isfinite(norm) || std::abs(norm - 1) > 0.02f)
        return false;
    float life{}, battery{}, capacity{};
    // Missing condition data should never discard a valid physical position.
    const auto in_range = [](float value) { return value >= 0 && value <= 65535; };
    if (read(entity + 0x6e8, life) && in_range(life)) {
        v.life = static_cast<uint16_t>(life);
        v.life_known = true;
    }
    if (read(actor + 0xb8, battery) && read(actor + 0xbc, capacity) && in_range(battery) && in_range(capacity)) {
        v.battery = static_cast<uint16_t>(battery);
        v.capacity = static_cast<uint16_t>(capacity);
        v.battery_known = true;
    }
    Address inventory{};
    if (read(actor + 0x140, inventory) && inventory)
        v.cargo = sample_inventory_signature(inventory);
    frame.vehicles.push_back(v);
    return true;
}
} // namespace

VehicleFrame sample_vehicles(Address game, const std::optional<Position> &sam) {
    VehicleFrame out;
    if (!game || !sam)
        return out;
    Address world{}, manager{};
    if (!read(game + world_rva, world) || !world || !read(game + manager_rva, manager) || !manager)
        return out;
    using WorldBucket = uint16_t (*)(Address);
    const auto bucket = reinterpret_cast<WorldBucket>(game + 0x262a860)(world);
    return vehicle_detail::collect(game, manager, bucket, rider(game), *sam,
                                   reinterpret_cast<vehicle_detail::QuaternionFromMatrix>(game + 0x19c44d0));
}

VehicleFrame vehicle_detail::collect(Address game, Address manager, uint16_t world,
                                     std::optional<uint32_t> seat, const Position &sam,
                                     QuaternionFromMatrix quaternion) {
    VehicleFrame out;
    out.world = world;
    out.seat = seat;
    std::array<Address, 4> units{};
    if (!manager || !read(manager + 0x28, units))
        return out;
    out.complete = true;
    for (Address unit : units) {
        if (!unit)
            continue;
        int count{};
        Address data{};
        // The low 13 bits of an actor ID index the unit. Reject an invalid array
        // before allocating or walking it during a world transition.
        if (!read(unit + 0x40, count) || count < 0 || count > 8192 || !read(unit + 0x48, data)) {
            out.complete = false;
            continue;
        }
        if (!count)
            continue;
        std::vector<Address> actors(static_cast<size_t>(count));
        if (!read_bytes(data, actors.data(), actors.size() * sizeof(Address))) {
            out.complete = false;
            continue;
        }
        for (Address actor : actors)
            if (actor && !read_vehicle(game, actor, sam, out, quaternion))
                out.complete = false;
    }
    return out;
}
} // namespace qs
