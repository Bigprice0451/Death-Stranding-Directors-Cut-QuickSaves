#include "delivery_sample.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using Address = std::uintptr_t;
using qs::Bytes;
struct PointerList {
    std::int32_t count;
    std::uint32_t capacity;
    const Address *data;
};
static_assert(sizeof(PointerList) == 16);
template <class T> void put(Bytes &object, std::size_t offset, T value) {
    std::memcpy(object.data() + offset, &value, sizeof(value));
}
Address address(const Bytes &object) { return reinterpret_cast<Address>(object.data()); }
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        // Build the same pointer chain used by the game's vehicle cargo inventory.
        Bytes inventory(0xc8), left_group(0xa0), right_group(0xa0);
        Bytes left_stack(0x20), right_stack(0x20), bag_a(0xd8), bag_b(0xd8);
        Address groups[]{address(left_group), address(right_group)};
        Address left_stacks[]{address(left_stack)}, right_stacks[]{address(right_stack)};
        Address left_bags[]{address(bag_b), address(bag_a)}, right_bags[]{address(bag_a)};
        put(inventory, 0x10, PointerList{2, 2, groups});
        put(left_group, 0xc, 1);
        put(right_group, 0xc, 1);
        put(left_group, 0x18, PointerList{1, 1, left_stacks});
        put(right_group, 0x18, PointerList{1, 1, right_stacks});
        put(left_stack, 0x10, PointerList{2, 2, left_bags});
        put(right_stack, 0x10, PointerList{1, 1, right_bags});
        put(bag_a, 0xd4, 0x12001u);
        put(bag_b, 0xd4, 0x102u);
        const auto sample = [&] { return qs::sample_inventory_signature(address(inventory)); };
        auto first = sample();
        check(first && first->count == 2 && first->digest == qs::sha256(Bytes{1, 0x01, 0x60, 0x02, 0x61}),
              "Wrong cargo ID normalization, byte order or duplicate handling");
        std::swap(groups[0], groups[1]);
        std::swap(left_bags[0], left_bags[1]);
        auto again = sample();
        check(again && again->digest == first->digest, "Group or stack order changed cargo identity");
        put(bag_b, 0xd4, 0x103u);
        again = sample();
        check(again && again->count == 2 && again->digest != first->digest,
              "A replacement package of the same count was missed");
        put(left_group, 0xc, 0);
        again = sample();
        check(again && again->count == 1, "An inactive group was counted");
        put(right_group, 0x88, Address{1}); // OwningThread of the group's x64 critical section
        check(!sample(), "A locked group was reported as an observation");
        put(right_group, 0x88, Address{0});
        put(right_stack, 0x10, PointerList{513, 513, nullptr});
        check(!sample(), "An oversized inventory was reported as valid");
        put(right_stack, 0x10, PointerList{1, 1, nullptr});
        check(!sample(), "An unreadable list was counted as empty");
        put(inventory, 0x10, PointerList{});
        again = sample();
        check(again && !again->count && again->digest == qs::sha256(Bytes{1}),
              "A readable empty inventory needs its own valid signature");
        check(!qs::sample_inventory_signature(0), "A missing inventory was counted as empty");
        std::cout << "Vehicle cargo inventory checks passed.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
