#include "delivery_sample.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>

namespace qs {
namespace {
using Address = std::uintptr_t;
using ActiveOrder = std::tuple<std::uint8_t, std::uint64_t, std::uint8_t>;
constexpr Address mission_manager_rva = 0x7c0aff8;
constexpr Address delivery_manager_rva = 0x7c0aff0;
constexpr std::size_t mission_stride = 0xa0;
constexpr std::size_t delivery_stride = 0x28;

struct GameArray {
    std::int32_t count;
    std::uint32_t allocation;
    Address data;
};
static_assert(sizeof(GameArray) == 16);

// A world transition can retire a manager. This read only touches our process
// and reports an inaccessible page without dereferencing an engine pointer.
bool read_bytes(Address address, void* output, std::size_t size) {
    SIZE_T copied{};
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                         output, size, &copied) && copied == size;
}
template<class T> bool read_field(Address address, T& value) {
    return read_bytes(address, &value, sizeof(value));
}
template<class T> T scalar(const std::uint8_t* address) {
    T value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

bool read_rows(Address address, std::size_t stride, Bytes& rows) {
    GameArray array{};
    // These limits bound diagnostic work and prevent a bad header from
    // requesting a large allocation. Exceeding them means "sample unavailable."
    if (!read_field(address, array) || array.count < 0 || array.count > 8192)
        return false;
    rows.resize(static_cast<std::size_t>(array.count) * stride);
    return rows.empty() || read_bytes(array.data, rows.data(), rows.size());
}

bool collect(Address image, std::vector<ActiveOrder>& orders) {
    Address missions{}, deliveries{};
    if (!read_field(image + mission_manager_rva, missions) || !missions ||
        !read_field(image + delivery_manager_rva, deliveries) || !deliveries)
        return false;

    // DSMissionInfo.GetDeliveryCount looks for the first matching 64-bit mission
    // ID in this table. A missing ID has the native delivery count zero.
    Bytes rows;
    if (!read_rows(deliveries + 0x816f8, delivery_stride, rows)) return false;
    std::map<std::uint64_t, std::uint8_t> delivered;
    for (std::size_t i = 0; i < rows.size(); i += delivery_stride)
        delivered.try_emplace(scalar<std::uint64_t>(&rows[i + 0x10]), rows[i + 0x24]);

    std::array<Address, 3> lists{};
    if (!read_field(missions + 0x108, lists)) return false;
    for (std::uint8_t group = 0; group < lists.size(); ++group) {
        if (!lists[group] || !read_rows(lists[group], mission_stride, rows)) return false;
        for (std::size_t i = 0; i < rows.size(); i += mission_stride) {
            const auto id = scalar<std::uint64_t>(&rows[i + 0x38]);
            if (!id || scalar<std::uint16_t>(&rows[i + 0x2c]) != 4) continue;
            const auto entry = delivered.find(id);
            orders.emplace_back(group, id, entry == delivered.end() ? std::uint8_t{0} : entry->second);
        }
    }
    std::sort(orders.begin(), orders.end());
    return true;
}

bool read_pointers(Address header, std::size_t maximum, std::vector<Address>& pointers) {
    GameArray array{};
    if (!read_field(header, array) || array.count < 0 || static_cast<std::size_t>(array.count) > maximum)
        return false;
    pointers.resize(static_cast<std::size_t>(array.count));
    return pointers.empty() || read_bytes(array.data, pointers.data(), pointers.size() * sizeof(Address));
}

bool unowned(Address section) {
    // Do not acquire the game's inventory locks or interrupt an inventory
    // rebuild to make a diagnostic observation. Two complete matching passes
    // below also reject changes between these point-in-time ownership checks.
    Address owner{};
    return read_field(section + offsetof(CRITICAL_SECTION, OwningThread), owner) && !owner;
}

bool collect_carried_ids(Address image, std::vector<std::uint16_t>& ids) {
    Address manager{}, inventory{};
    if (!read_field(image + 0x7be7980, manager) || !manager ||
        !read_field(manager + 0x20220, inventory) || !inventory || !unowned(inventory + 0xa0))
        return false;
    std::vector<Address> groups, stacks, items;
    if (!read_pointers(inventory + 0x10, 128, groups)) return false;
    std::size_t stacks_seen = 0;
    for (Address group : groups) {
        std::int32_t enabled{};
        if (!group || !unowned(group + 0x78) || !read_field(group + 0xc, enabled)) return false;
        if (!enabled) continue;
        if (!read_pointers(group + 0x18, 512 - stacks_seen, stacks)) return false;
        stacks_seen += stacks.size();
        for (Address stack : stacks) {
            if (!stack || !read_pointers(stack + 0x10, 512 - ids.size(), items)) return false;
            for (Address item : items) {
                std::uint32_t id{};
                if (!item || !read_field(item + 0xd4, id)) return false;
                ids.push_back(static_cast<std::uint16_t>((id & 0x1fff) | 0x6000));
            }
        }
        if (!unowned(group + 0x78)) return false;
    }
    if (!unowned(inventory + 0xa0)) return false;
    // The script tests membership by actor ID. Count a bag once even if it
    // appears under two attachment points during a change of equipment.
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return true;
}
}

std::optional<DeliverySample> sample_delivery_state(std::uintptr_t validated_base) noexcept {
    try {
        if (!validated_base) return std::nullopt;
        // Two matching observations also tolerate arrays being reordered during
        // map reconstruction. Sampling never affects saving or loading.
        std::vector<ActiveOrder> orders, confirmation;
        if (!collect(validated_base, orders) || !collect(validated_base, confirmation) ||
            orders != confirmation) return std::nullopt;
        DeliverySample result;
        result.active_orders = static_cast<std::uint32_t>(orders.size());
        // Format version 1: sorted (group byte, mission ID in 8 little-endian
        // bytes, delivered-count byte). No addresses or padding enter the digest.
        Bytes encoded{1};
        encoded.reserve(1 + 10 * orders.size());
        for (auto [group, id, delivered] : orders) {
            encoded.push_back(group);
            for (unsigned byte = 0; byte < 8; ++byte)
                encoded.push_back(static_cast<std::uint8_t>(id >> (byte * 8)));
            encoded.push_back(delivered);
            result.delivered_count += delivered;
        }
        result.digest = sha256(encoded);
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint32_t> sample_carried_cargo_count(std::uintptr_t validated_base) noexcept {
    try {
        if (!validated_base) return std::nullopt;
        std::vector<std::uint16_t> ids, confirmation;
        if (!collect_carried_ids(validated_base, ids) || !collect_carried_ids(validated_base, confirmation) ||
            ids != confirmation) return std::nullopt;
        return static_cast<std::uint32_t>(ids.size());
    } catch (...) {
        return std::nullopt;
    }
}
}
