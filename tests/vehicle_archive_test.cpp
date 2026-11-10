#include "vehicle_archive.hpp"
#include "native_image.hpp"
#include "vehicle_check.hpp"
#include "private_load_simulation.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace qs;

void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class T> void append(Bytes &out, T value) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    out.insert(out.end(), bytes, bytes + sizeof value);
}

template <class T> void put(Bytes &out, std::size_t at, T value) {
    std::memcpy(out.data() + at, &value, sizeof value);
}

Bytes row(std::uint32_t actor, bool removed = false) {
    Bytes data(0x74);
    data[0] = 42;
    if (!removed) {
        put(data, 0x10, 3.5f);
        put(data, 0x28, 1.0f);
    }
    put(data, 0x2c, actor);
    data[0x41] = removed ? 4 : 1;
    data[0x73] = 99;
    return data;
}

using Group = std::pair<std::uint16_t, std::vector<Bytes>>;
Bytes world(std::uint16_t id, std::initializer_list<Group> groups) {
    Bytes block(8 + groups.size() * 4);
    put(block, 0, static_cast<std::uint16_t>(groups.size()));
    put(block, 2, id);
    std::size_t next_offset = 8;
    for (const auto &group : groups) {
        put(block, next_offset, static_cast<std::uint32_t>(block.size()));
        next_offset += 4;
        append(block, group.first);
        append(block, static_cast<std::uint16_t>(group.second.size()));
        for (const auto &record : group.second)
            block.insert(block.end(), record.begin(), record.end());
    }
    put(block, 4, static_cast<std::uint32_t>(block.size()));
    Bytes out;
    append(out, static_cast<std::uint32_t>(block.size()));
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

void compact(Bytes &out, std::uint32_t value) {
    if (value <= 127)
        append(out, static_cast<std::uint8_t>(value));
    else if (value <= 32767) {
        append(out, std::uint8_t{0x81});
        append(out, static_cast<std::uint16_t>(value));
    } else {
        append(out, std::uint8_t{0x80});
        append(out, value);
    }
}

Bytes image(std::initializer_list<Bytes> worlds, bool duplicate_mark = false) {
    Bytes payload;
    for (const auto &block : worlds)
        payload.insert(payload.end(), block.begin(), block.end());
    Bytes out(native_header_size);
    append(out, 0x15b4e7a9u); // Physical starting tag checksum.
    compact(out, 0x55005509);
    compact(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    append(out, std::uint8_t{0}); // Native manager tail outside the world blocks.
    const auto stop = static_cast<std::uint32_t>(out.size() - native_header_size);
    append(out, 0xa77598a7u);
    const auto tables = static_cast<std::uint32_t>(out.size() - native_header_size);
    compact(out, duplicate_mark ? 3 : 2);
    compact(out, 0x2bf60893);
    compact(out, 0);
    compact(out, 0);
    compact(out, 0x4606f170);
    compact(out, stop);
    compact(out, 0);
    if (duplicate_mark) {
        compact(out, 0x2bf60893);
        compact(out, 0);
        compact(out, 0);
    }
    const auto strings = static_cast<std::uint32_t>(out.size() - native_header_size);
    append(out, tables);
    append(out, strings);
    out[0x24] = 0x43;
    put(out, 0x26, std::uint16_t{511});
    put(out, 0x2c, 0x4cu);
    put(out, 0x1fc, static_cast<std::uint32_t>(out.size() - native_header_size));
    update_native_payload_digest(out);
    return out;
}

void rejected(const Bytes &image, VehicleArchiveStatus status) {
    const auto inspected = inspect_vehicle_archive(image);
    require(inspected.status == status && inspected.vehicles.empty(), "Unsafe or partial result");
}

void exercise() {
    const auto multi = image({world(100, {{0, {row(0x40001)}}, {3, {row(0x1e002), row(0, true)}}}),
                              world(200, {{2, {row(0x20008), row(0x42010)}}})});
    const auto report = inspect_vehicle_archive(multi);
    require(report.status == VehicleArchiveStatus::readable && report.vehicles.size() == 5,
            "Could not inspect multiple world and group buckets");
    const auto &bike = report.vehicles[1];
    require(bike.world == 100 && bike.group == 3 && bike.kind == VehicleArchiveKind::motorbike &&
                bike.actor_id == 0x1e002 && bike.position[0] == 3.5f && bike.orientation[3] == 1.0f &&
                bike.persistent_id[0] == 42 && multi[bike.image_offset] == 42 &&
                bike.packed_fields.back() == 99,
            "Wrong vehicle fields or source offset");
    require(report.vehicles[0].kind == VehicleArchiveKind::roadster &&
                report.vehicles[2].kind == VehicleArchiveKind::removal &&
                report.vehicles[3].kind == VehicleArchiveKind::truck &&
                report.vehicles[4].kind == VehicleArchiveKind::unclassified,
            "Wrong vehicle class");
    rejected(image({}), VehicleArchiveStatus::readable);
    rejected(image({world(100, {})}), VehicleArchiveStatus::readable);
    rejected(Bytes(native_header_size + 10), VehicleArchiveStatus::missing);
    rejected(image({world(100, {{4, {row(0x1e000)}}})}), VehicleArchiveStatus::unsupported);
    auto bad = multi;
    bad.pop_back();
    rejected(bad, VehicleArchiveStatus::malformed);
    bad = multi;
    put(bad, native_header_size + 12, 0xffffffffu);
    rejected(bad, VehicleArchiveStatus::malformed);
    bad = multi;
    put(bad, native_header_size + 12 + 4 + 8, 9u); // First group points into its own offset table.
    rejected(bad, VehicleArchiveStatus::malformed);
    bad = multi;
    put(bad, bike.image_offset + 0x10, std::numeric_limits<float>::quiet_NaN());
    rejected(bad, VehicleArchiveStatus::malformed);
    rejected(image({}, true), VehicleArchiveStatus::ambiguous);
    bad = image({});
    bad[native_header_size + 9] = 0xff; // Negative compact length.
    rejected(bad, VehicleArchiveStatus::malformed);
    bad = image({});
    bad[native_header_size] ^= 1; // Correct directory tag, wrong physical checksum.
    rejected(bad, VehicleArchiveStatus::malformed);
    bad = image({});
    bad[native_header_size + 5] ^= 1; // New manager format.
    rejected(bad, VehicleArchiveStatus::unsupported);
    bad = image({});
    put(bad, bad.size() - 8, 0xffffffffu);
    rejected(bad, VehicleArchiveStatus::malformed);
    std::vector<Bytes> many(300, row(0x1e000));
    const auto large = inspect_vehicle_archive(image({world(100, {{3, many}})}));
    require(large.status == VehicleArchiveStatus::readable && large.vehicles.size() == many.size(),
            "Full-width manager length was rejected");
}

void check_alignment() {
    const auto original = image({world(100, {{3, {row(0x1e002)}}})});
    const auto saved = inspect_vehicle_archive(original).vehicles.at(0);
    VehicleFrame live{};
    live.complete = true;
    live.world = saved.world;
    LiveVehicle actor{};
    actor.world = saved.world;
    actor.actor = saved.actor_id;
    actor.persistent_id = saved.persistent_id;
    actor.position = {-4, 5, 6};
    actor.orientation = {0, 1, 0, 0};
    live.vehicles = {actor};

    auto corrected = original;
    const auto report = align_private_vehicle_poses(corrected, live);
    require(report.checked == 1 && report.adjusted == 1, "Unique live vehicle was missed");
    validate_native_image(corrected);
    const auto adjusted = inspect_vehicle_archive(corrected).vehicles.at(0);
    require(adjusted.position == actor.position && adjusted.orientation == actor.orientation,
            "Capture did not receive both parts of the live pose");
    require(!std::equal(original.begin() + 0x1ec, original.begin() + 0x1fc, corrected.begin() + 0x1ec),
            "Native payload digest did not change");
    for (std::size_t i = 0; i < original.size(); ++i) {
        if ((i >= 0x1ec && i < 0x1fc) || (i >= saved.image_offset + 0x10 && i < saved.image_offset + 0x2c))
            continue;
        require(corrected[i] == original[i], "Alignment changed another field in the archived world");
    }
    const auto once = corrected;
    require(!align_private_vehicle_poses(corrected, live).adjusted && corrected == once,
            "Repeated capture changed a matching pose");
    live.vehicles[0].orientation = {0, -1, 0, 0};
    require(!align_private_vehicle_poses(corrected, live).adjusted && corrected == once,
            "Equivalent rotation changed saved data");

    auto unchanged = [&](Bytes before, const VehicleFrame &observation) {
        const auto copy = before;
        const auto skipped = align_private_vehicle_poses(before, observation);
        require(!skipped.adjusted && before == copy, "Unmatched, ambiguous or authored vehicle was changed");
        return skipped;
    };
    live.vehicles[0] = actor;
    ++live.vehicles[0].actor;
    require(unchanged(original, live).unavailable == 1, "Wrong actor ID matched");
    live.vehicles[0] = actor;
    ++live.vehicles[0].persistent_id[1];
    require(unchanged(original, live).unavailable == 1, "Wrong persistent ID matched");
    live.vehicles[0] = actor;
    ++live.vehicles[0].world;
    require(unchanged(original, live).unavailable == 1, "Wrong world matched");
    live.vehicles[0] = actor;
    VehicleFrame duplicated = live;
    auto other_observation = actor;
    other_observation.position[0] += 40;
    duplicated.vehicles.push_back(other_observation);
    unchanged(original, duplicated);
    VehicleFrame invalid = live;
    invalid.vehicles[0].position[2] = std::numeric_limits<float>::infinity();
    unchanged(original, invalid);
    invalid = live;
    invalid.vehicles[0].orientation[3] = std::numeric_limits<float>::quiet_NaN();
    unchanged(original, invalid);
    auto authored = row(actor.actor);
    authored[0x43] = 4; // Native 0x400 in the separate mode word at +0x42.
    require(unchanged(image({world(100, {{3, {authored}}})}), live).authored == 1,
            "Authored return pose was changed");
    unchanged(image({world(100, {{3, {row(actor.actor, true)}}})}), live);
    unchanged(image({world(100, {{3, {row(actor.actor), row(actor.actor)}}})}), live);

    // Existing archives enter the private loader exactly as saved. Exercise the
    // same admission entry point with a simulated engine and compare all bytes.
    load_test::Engine engine;
    PrivateLoad loader(load_test::Engine::lookup);
    loader.begin(1, original);
    require(engine.saw_private_source &&
                std::memcmp(engine.borrowed - native_header_size, original.data(), original.size()) == 0,
            "Loading a legacy archive changed its vehicle or another saved field");
    load_test::at<int>(engine.request.data(), 0xc) = 15;
    loader.poll();
    load_test::Engine::remove(&engine.queue(), 0, false);
    load_test::at<void *>(engine.controller(), 8) = nullptr;
    load_test::at<void *>(engine.controller(), 0x10) = nullptr;
    load_test::at<unsigned>(engine.world.data(), 0x448) = 0;
    require(loader.poll().state == LoadState::completed, "Legacy private load did not complete");
}
} // namespace

int main() {
    try {
        exercise();
        check_alignment();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
