#include "vehicle_archive.hpp"
#include "native_image.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace qs {
namespace {
using Status = VehicleArchiveStatus;
using Kind = VehicleArchiveKind;
constexpr std::size_t row_size = 0x74;
// Writer RVA 0x2FA65E0 emits tag 0x55005509 as a full-width compact integer.
constexpr std::array<std::uint8_t, 5> manager_tag{0x80, 0x09, 0x55, 0x00, 0x55};

template <class T> T read(const Bytes &image, std::size_t at) {
    T value{};
    std::memcpy(&value, image.data() + at, sizeof value);
    return value;
}

// The game uses a signed byte for short values. The two most negative byte
// values introduce a signed 16- or 32-bit number (writer RVA 0x1D42760).
bool read_compact(const Bytes &image, std::size_t &at, std::size_t end, std::int32_t &value) {
    if (at >= end)
        return false;
    const auto marker = image[at++];
    if (marker == 0x80) {
        if (end - at < 4)
            return false;
        value = read<std::int32_t>(image, at);
        at += 4;
    } else if (marker == 0x81) {
        if (end - at < 2)
            return false;
        value = read<std::int16_t>(image, at);
        at += 2;
    } else {
        value = static_cast<std::int8_t>(marker);
    }
    return true;
}

struct SectionMark {
    // Directory entries store the payload-relative position of a four-byte
    // checksum of the tag name. It is independent of the vehicle data.
    std::array<std::uint8_t, 5> tag;
    std::uint32_t checksum;
};
constexpr SectionMark vehicle_start{{0x80, 0x93, 0x08, 0xf6, 0x2b}, 0x15b4e7a9};
constexpr SectionMark vehicle_end{{0x80, 0x70, 0xf1, 0x06, 0x46}, 0xa77598a7};

Status locate_mark(const Bytes &image, std::size_t tables, std::size_t strings, const SectionMark &mark,
                   std::size_t &at) {
    // RVA 0x1D3F3A5 writes (tag, position, flags) in the final index table.
    // The footer gives bounds for all index tables together. Require a single
    // candidate in those bounds and verify it against the physical checksum.
    // Unrelated payload bytes and strings never select a vehicle record.
    const auto end = image.begin() + strings;
    const auto entry = std::search(image.begin() + tables, end, mark.tag.begin(), mark.tag.end());
    if (entry == end)
        return Status::missing;
    const auto next = entry + mark.tag.size();
    if (std::search(next, end, mark.tag.begin(), mark.tag.end()) != end)
        return Status::ambiguous;
    auto cursor = static_cast<std::size_t>(next - image.begin());
    std::int32_t offset{}, flags{};
    if (!read_compact(image, cursor, strings, offset) || offset < 0 ||
        !read_compact(image, cursor, strings, flags))
        return Status::malformed;
    if (flags != 0)
        return Status::unsupported;
    at = native_header_size + static_cast<std::size_t>(offset);
    if (at > tables || tables - at < 4 || read<std::uint32_t>(image, at) != mark.checksum)
        return Status::malformed;
    return Status::readable;
}

Kind kind_of(std::uint32_t id, std::uint8_t flags) {
    // A removal row has only a persistent ID and flag. All-zero coordinates
    // in such a row are not a vehicle parked at the map origin.
    if (flags & 4)
        return Kind::removal;
    switch (id & 0x1fe000) {
    case 0x1e000:
        return Kind::motorbike;
    case 0x20000:
        return Kind::truck;
    case 0x40000:
        return Kind::roadster;
    default:
        return Kind::unclassified;
    }
}

bool read_row(const Bytes &image, std::size_t at, std::uint16_t world, std::uint16_t group,
              std::vector<VehicleArchiveEntry> &rows) {
    VehicleArchiveEntry entry;
    entry.image_offset = at;
    entry.world = world;
    entry.group = group;
    entry.actor_id = read<std::uint32_t>(image, at + 0x2c);
    entry.kind = kind_of(entry.actor_id, image[at + 0x41]);
    std::copy_n(image.begin() + at, entry.persistent_id.size(), entry.persistent_id.begin());
    std::memcpy(entry.position.data(), image.data() + at + 0x10, sizeof entry.position);
    std::memcpy(entry.orientation.data(), image.data() + at + 0x1c, sizeof entry.orientation);
    std::copy_n(image.begin() + at + 0x30, entry.packed_fields.size(), entry.packed_fields.begin());
    for (float v : entry.position)
        if (!std::isfinite(v))
            return false;
    for (float v : entry.orientation)
        if (!std::isfinite(v))
            return false;
    rows.push_back(entry);
    return true;
}

// World writer RVA 0x2FA6BC0 emits a group-offset table followed by the
// contiguous group arrays. Every declared length and offset is checked before
// reading the next level. A new grouping format should produce "unsupported".
Status read_world(const Bytes &image, std::size_t start, std::size_t length,
                  std::vector<VehicleArchiveEntry> &rows) {
    if (length < 8 || read<std::uint32_t>(image, start + 4) != length)
        return Status::malformed;
    const auto groups = read<std::uint16_t>(image, start);
    const auto world = read<std::uint16_t>(image, start + 2);
    if (groups > 4)
        return Status::unsupported;
    std::size_t next = 8 + 4 * groups;
    if (next > length)
        return Status::malformed;
    std::uint32_t used_groups = 0;
    for (unsigned index = 0; index < groups; ++index) {
        if (read<std::uint32_t>(image, start + 8 + 4 * index) != next || length - next < 4)
            return Status::malformed;
        const auto group = read<std::uint16_t>(image, start + next);
        const auto count = read<std::uint16_t>(image, start + next + 2);
        if (group >= 4)
            return Status::unsupported;
        if (used_groups & (1u << group))
            return Status::malformed;
        used_groups |= 1u << group;
        next += 4;
        if (count > (length - next) / row_size)
            return Status::malformed;
        for (unsigned row = 0; row < count; ++row) {
            if (!read_row(image, start + next, world, group, rows))
                return Status::malformed;
            next += row_size;
        }
    }
    return next == length ? Status::readable : Status::malformed;
}
} // namespace

VehicleArchive inspect_vehicle_archive(const Bytes &image) {
    if (image.size() < native_header_size + 8 || image.size() > native_bank_size)
        return {Status::malformed, {}};
    // Footer offsets count from the start of the payload, excluding the game's
    // 512-byte header (reader RVA 0x1D3F589). The two tags bracket this manager.
    const auto footer = image.size() - 8;
    const auto tables = native_header_size + read<std::uint32_t>(image, footer);
    const auto strings = native_header_size + read<std::uint32_t>(image, footer + 4);
    if (tables > strings || strings > footer)
        return {Status::malformed, {}};
    std::size_t start{}, stop{};
    const auto first = locate_mark(image, tables, strings, vehicle_start, start);
    const auto last = locate_mark(image, tables, strings, vehicle_end, stop);
    if (first == Status::missing && last == Status::missing)
        return {Status::missing, {}};
    if (first == Status::ambiguous || last == Status::ambiguous)
        return {Status::ambiguous, {}};
    if (first == Status::unsupported || last == Status::unsupported)
        return {Status::unsupported, {}};
    if (first != Status::readable || last != Status::readable || stop < start + 4)
        return {Status::malformed, {}};
    auto cursor = start + 4;
    if (stop - cursor < manager_tag.size())
        return {Status::malformed, {}};
    if (!std::equal(manager_tag.begin(), manager_tag.end(), image.begin() + cursor))
        return {Status::unsupported, {}};
    cursor += manager_tag.size();
    std::int32_t size{};
    if (!read_compact(image, cursor, stop, size) || size < 0 || static_cast<std::size_t>(size) > stop - cursor)
        return {Status::malformed, {}};
    // Separate native state can follow the world blocks before the end tag.
    const auto end = cursor + static_cast<std::size_t>(size);
    VehicleArchive result{Status::readable, {}};
    while (cursor < end) {
        if (end - cursor < 4)
            return {Status::malformed, {}};
        const auto world_length = read<std::uint32_t>(image, cursor);
        cursor += 4;
        if (world_length > end - cursor)
            return {Status::malformed, {}};
        const auto status = read_world(image, cursor, world_length, result.vehicles);
        if (status != Status::readable)
            return {status, {}};
        cursor += world_length;
    }
    return result;
}
} // namespace qs
