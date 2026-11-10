#include "vehicle_check.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace qs;
template <size_t N, class T> void put(std::array<uint8_t, N> &bytes, size_t offset, T value) {
    if (offset + sizeof value > N)
        throw std::runtime_error("Invalid test layout");
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}
template <class T> uintptr_t address(T &data) { return reinterpret_cast<uintptr_t>(data.data()); }
void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void quaternion(float *result, const float *matrix) {
    for (int i = 0; i < 16; ++i)
        check(matrix[i] == (i % 5 == 0 ? 1.f : 0.f), "Bad native matrix order");
    result[3] = 1;
}
struct Bike {
    std::array<uint8_t, 0x1d0> actor{};
    std::array<uint8_t, 0x700> entity{};
    Bike(uintptr_t base, uint32_t id, double x) {
        put(actor, 0, base + 0x3e26070);
        put(actor, 8, id);
        put(actor, 0x20, uint8_t{1});
        put(actor, 0x24, uint32_t{2});
        put(actor, 0x68, address(entity) + 0x20);
        put(actor, 0x1a4, uint16_t{100});
        put(actor, 0x188, id);
        put(actor, 0xb8, 500.f);
        put(actor, 0xbc, 600.f);
        put(entity, 0xc8, Position{x, 0, 0});
        put(entity, 0xc8 + 24, std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1});
        put(entity, 0x6e8, 980.75f);
    }
};
} // namespace
int main() {
    try {
        constexpr uintptr_t base = 0x140000000;
        Bike near(base, 0x1e000, 99), far(base, 0x1e001, 102), rider(base, 0x1e002, 150);
        std::array<uintptr_t, 3> members{address(near.actor), address(far.actor), address(rider.actor)};
        std::array<uint8_t, 0x60> unit{}, manager{};
        put(unit, 0x40, int{3});
        put(unit, 0x48, address(members));
        put(manager, 0x28 + 3 * 8, address(unit));
        auto sample = [&] {
            return qs::vehicle_detail::collect(base, address(manager), 100, 0x1e002, {0, 0, 0}, quaternion);
        };
        auto report = sample();
        check(report.complete && report.vehicles.size() == 2 && report.seat == 0x1e002,
              "Rider or proximity selection failed");
        const auto &parked = report.vehicles.front();
        check(parked.actor == 0x1e000 && parked.position[0] == 99 && parked.life == 980 &&
                  parked.battery == 500 && parked.capacity == 600 && parked.world == 100 &&
                  parked.orientation[3] == 1,
              "Vehicle state changed");
        check(report.vehicles.back().actor == 0x1e002, "Mounted vehicle must always be included");
        InventorySignature package{1, {}}, empty{0, {}};
        package.digest.back() = 71;
        report.vehicles.front().cargo = package;
        report.vehicles.back().cargo = empty;
        const auto loaded = summarize_vehicle_cargo(report);
        check(loaded && loaded->vehicles == 2 && loaded->items == 1, "Cargo observation missing");
        std::reverse(report.vehicles.begin(), report.vehicles.end());
        check(summarize_vehicle_cargo(report)->digest == loaded->digest, "Enumeration order changed cargo");
        std::swap(report.vehicles[0].cargo, report.vehicles[1].cargo);
        check(summarize_vehicle_cargo(report)->digest != loaded->digest,
              "Moving cargo between vehicles went undetected");
        report.vehicles[0].cargo.reset();
        check(!summarize_vehicle_cargo(report), "Unavailable inventory treated as empty");
        VehicleArchive previous{VehicleArchiveStatus::readable, {}};
        const auto &current = report.vehicles[0];
        VehicleArchiveEntry saved;
        saved.actor_id = current.actor;
        saved.world = current.world;
        saved.persistent_id = current.persistent_id;
        saved.kind = VehicleArchiveKind::motorbike;
        saved.position = current.position;
        saved.orientation = current.orientation;
        previous.vehicles.push_back(saved);
        bool waiting{};
        auto result = compare_loaded_vehicles(previous, report, Position{}, current.actor, &waiting);
        check(!waiting && result.find("matched=1/1") != result.npos && result.find("seat=match") != result.npos,
              "Ridden vehicle not matched after streaming");
        report.seat = no_ride;
        compare_loaded_vehicles(previous, report, Position{}, current.actor, &waiting);
        check(waiting, "Mount verification did not wait for native attachment");
        report.complete = false;
        compare_loaded_vehicles(previous, report, Position{}, current.actor, &waiting);
        check(waiting, "Unavailable world should allow time for streaming");
        put(near.actor, 0xb8, -1.f);
        put(near.entity, 0x6e8, -1.f);
        for (uintptr_t vehicle_type : {uintptr_t{0x3e26a48}, uintptr_t{0x3e27790}}) {
            put(near.actor, 0, base + vehicle_type);
            const auto alternate = sample();
            check(alternate.complete && alternate.vehicles.size() == 2 &&
                      !alternate.vehicles[0].battery_known && !alternate.vehicles[0].life_known &&
                      alternate.vehicles[0].position[0] == 99,
                  "Truck or roadster position lost when condition was unavailable");
        }
        put(near.actor, 0x1a4, uint16_t{99});
        check(sample().vehicles.size() == 1, "Different world included");
        put(unit, 0x40, int{8200});
        check(!sample().complete, "Damaged unit accepted");
        std::cout << "Verified ridden, nearby, distant, other-world and invalid vehicle lists.\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
