#include "delivery_sample.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using qs::Bytes;
using Address = std::uintptr_t;
struct Table { std::int32_t count; std::uint32_t allocation; Address data; };
template<class T> void put(Bytes& bytes, std::size_t index, T value) {
    std::memcpy(bytes.data() + index, &value, sizeof(value));
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        // The two singleton pointers are neighbors in the target image. Point
        // their relative addresses into ordinary test memory; no game is opened.
        Bytes missions(0x120), deliveries(0x81710), mission_rows(4 * 0xa0), cargo_rows(3 * 0x28);
        std::array<Table, 3> lists{{{4, 4, reinterpret_cast<Address>(mission_rows.data())}, {}, {}}};
        std::array<Address, 2> managers{reinterpret_cast<Address>(deliveries.data()),
                                        reinterpret_cast<Address>(missions.data())};
        const Address base = reinterpret_cast<Address>(managers.data()) - 0x7c0aff0;
        for (std::size_t group = 0; group < lists.size(); ++group)
            put(missions, 0x108 + group * sizeof(Address), reinterpret_cast<Address>(&lists[group]));
        put(deliveries, 0x816f8, Table{3, 3, reinterpret_cast<Address>(cargo_rows.data())});
        auto set_order = [&](std::size_t index, std::uint64_t id, std::uint16_t state) {
            put(mission_rows, index * 0xa0 + 0x38, id);
            put(mission_rows, index * 0xa0 + 0x2c, state);
        };
        set_order(0, 1001, 4);
        set_order(1, 1002, 4);
        set_order(2, 1003, 16);  // already completed
        set_order(3, 0, 4);      // unoccupied row
        put(cargo_rows, 0x10, std::uint64_t{1001});
        cargo_rows[0x24] = 3;
        put(cargo_rows, 0x28 + 0x10, std::uint64_t{1001});
        cargo_rows[0x28 + 0x24] = 8; // native lookup takes the first duplicate
        put(cargo_rows, 0x50 + 0x10, std::uint64_t{1003});
        cargo_rows[0x50 + 0x24] = 10;

        const auto before = qs::sample_delivery_state(base);
        require(before && before->active_orders == 2 && before->delivered_count == 3,
                "progress-only counts or first-match delivery lookup wrong");
        std::swap_ranges(mission_rows.begin(), mission_rows.begin() + 0xa0, mission_rows.begin() + 0xa0);
        auto after = qs::sample_delivery_state(base);
        require(after && after->digest == before->digest, "row order changed fingerprint");
        set_order(0, 1004, 4);
        after = qs::sample_delivery_state(base);
        require(after && after->digest != before->digest, "different order with equal count undetected");
        set_order(0, 1002, 4);
        cargo_rows[0x24] = 2;
        after = qs::sample_delivery_state(base);
        require(after && after->delivered_count == 2 && after->digest != before->digest,
                "per-order delivery count change undetected");
        lists[0].count = 8193;
        require(!qs::sample_delivery_state(base), "oversized native list accepted");
        lists[0] = {1, 1, 1};
        require(!qs::sample_delivery_state(base), "inaccessible row pointer accepted");
        lists[0] = {};
        after = qs::sample_delivery_state(base);
        require(after && !after->active_orders && !after->delivered_count, "empty observation lost");
        managers[0] = 0;
        require(!qs::sample_delivery_state(base), "absent manager reported as an empty list");

        Bytes cargo_manager(0x20228), inventory(0xd0), group(0xa0), stack(0x20), bag(0xd8), bag2(0xd8);
        const auto address = [](const Bytes& memory) { return reinterpret_cast<Address>(memory.data()); };
        const Address cargo_global = address(cargo_manager);
        const Address cargo_base = reinterpret_cast<Address>(&cargo_global) - 0x7be7980;
        put(cargo_manager, 0x20220, address(inventory));
        Address group_pointer = address(group), stack_pointer = address(stack);
        std::array<Address, 3> bag_pointers{address(bag), address(bag2), address(bag)};
        put(inventory, 0x10, Table{1, 1, reinterpret_cast<Address>(&group_pointer)});
        put(group, 0xc, 1);
        put(group, 0x18, Table{1, 1, reinterpret_cast<Address>(&stack_pointer)});
        put(stack, 0x10, Table{3, 3, reinterpret_cast<Address>(bag_pointers.data())});
        put(bag, 0xd4, std::uint32_t{0x21});
        put(bag2, 0xd4, std::uint32_t{0x4024});
        require(qs::sample_carried_cargo_count(cargo_base) == 2u, "carried duplicates counted twice");
        put(bag2, 0xd4, std::uint32_t{0x8021});
        require(qs::sample_carried_cargo_count(cargo_base) == 1u, "wrong native carry ID mask");
        put(group, 0x78 + 0x10, Address{1}); // group critical section currently owned
        require(!qs::sample_carried_cargo_count(cargo_base), "busy inventory was sampled");
        put(group, 0x78 + 0x10, Address{0});
        put(group, 0xc, 0);
        require(qs::sample_carried_cargo_count(cargo_base) == 0u, "inactive group included");
        put(group, 0xc, 1);
        put(stack, 0x10, Table{513, 513, 1});
        require(!qs::sample_carried_cargo_count(cargo_base), "oversized cargo list accepted");
        std::cout << "Delivery sample tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
