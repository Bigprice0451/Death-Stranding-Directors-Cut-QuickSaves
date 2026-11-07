#include "private_capture.hpp"
#include "private_capture_internal.hpp"
#include "native_image.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace qs {
namespace {

using Byte = std::uint8_t;
// Game::Game verifies the SHA-256 before passing us this image base. Addresses
// here belong to Steam build 13300582. See NATIVE_CAPTURE.md for the evidence.
enum Address : std::uintptr_t {
    World = 0x7be6318, PlayerManager = 0x7bc7568,
    RequestVirtualTable = 0x3f199e0, EmptyString = 0x4562cb0,
    DefaultPlayerReference = 0x4f6be68,
    PrepareJob = 0x3633640, StartJob = 0x362db80, JoinJob = 0x362dcf0,
    IsLoading = 0x3668c50, IsSaving = 0x3668db0
};

// Offsets can be unaligned in the game's request (including +1A). Copying the
// field also avoids aliasing a byte buffer as an unrelated C++ object.
template<class T> T field(const void* object, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const Byte*>(object) + offset, sizeof(value));
    return value;
}
template<class T> void set(void* object, std::size_t offset, T value) {
    std::memcpy(static_cast<Byte*>(object) + offset, &value, sizeof(value));
}
Byte* singleton(std::uintptr_t image, std::uintptr_t rva) {
    return field<Byte*>(reinterpret_cast<const void*>(image + rva), 0);
}
void check(bool allowed, const char* message) {
    if (!allowed) throw std::runtime_error(message);
}

} // namespace

bool capture_detail::has_regular_callers(std::uintptr_t image, void* const* stack, std::size_t depth) {
    // The game also pumps Windows messages during streaming and shutdown.
    // These two callers identify the ordinary frame, after its preceding job
    // barrier. The private worker will use its own separate completion fence.
    bool regular_pump = false, outer_tick = false;
    for (std::size_t i = 0; i < depth; ++i) {
        const auto pc = reinterpret_cast<std::uintptr_t>(stack[i]);
        regular_pump |= pc == image + 0x263d2ae;
        outer_tick |= pc == image + 0x263a869;
    }
    return regular_pump && outer_tick;
}

bool regular_capture_frame(std::uintptr_t image) {
    void* stack[62]{};
    const auto depth = RtlCaptureStackBackTrace(0, 62, stack, nullptr);
    return capture_detail::has_regular_callers(image, stack, depth);
}

void capture_detail::require_idle_controller(const void* live) {
    check(!field<int>(live, 0x88) && !field<void*>(live, 0x98) &&
          !field<void*>(live, 0xa0) && !field<int>(live, 0xa8),
          "A game save or load is still running. Wait for it to finish before pressing F5.");
}

namespace {
const Byte* ready_world_controller(std::uintptr_t image) {
    using Query = bool (*)();
    Byte* world = singleton(image, World);
    check(world && !reinterpret_cast<Query>(image + IsLoading)() &&
          !reinterpret_cast<Query>(image + IsSaving)(),
          "The world is loading or saving. Try F5 again when it finishes.");
    auto live = world + 0x2a0;
    capture_detail::require_idle_controller(live);
    // The worker directly dereferences these services. Some are temporarily
    // absent between teardown of one map and construction of the next one.
    for (auto rva : {0x7be6a40u, 0x7be6c18u, 0x7ae90d8u, 0x7bc9218u, 0x7bf83c0u, 0x4f6cf40u})
        check(singleton(image, rva) != nullptr, "The world systems needed to capture a checkpoint are still loading.");
    check(field<void*>(world, 0x420) != nullptr, "World save data has not finished initializing.");
    auto players = singleton(image, PlayerManager);
    // The writer requests local player category 3 (selector at 0x4B1ACD0).
    // Its position is recorded by the same worker as the world and mission.
    auto owner = players ? field<void*>(players, 0x48 + 3 * sizeof(void*)) : nullptr;
    auto entity = owner ? field<void*>(owner, 0x70) : nullptr;
    auto handle = entity ? field<void*>(entity, 0x58) : nullptr;
    check(handle && field<void*>(handle, 0), "The game has not created the player entity needed for a checkpoint.");
    return live;
}

} // namespace

Bytes capture_detail::run_capture_job(const CaptureBindings& engine, const void* world_header) {
    // State 1 of the native save dispatcher only prepares the serializer job.
    // This controller owns its entire queue, its completion counter, and the
    // only bank that job can address. Vanilla's rotating and retry banks never
    // enter this queue. Keep these objects alive until the native join returns.
    alignas(16) std::array<Byte, 0x150> controller{};
    alignas(16) std::array<Byte, 0x80> request{};
    Bytes image_bytes(native_bank_size);
    void* only_request = request.data();
    auto ctx = controller.data();
    auto req = request.data();

    set(ctx, 0x88, 1); // request count
    set(ctx, 0x8c, 1); // request capacity
    set(ctx, 0x90, &only_request);
    set(ctx, 0xb8, image_bytes.data()); // private bank zero
    std::memcpy(ctx + 0x108, world_header, 0x24);
    set(ctx, 0x13c, (std::numeric_limits<int>::max)());

    set(req, 0x00, engine.request_type);
    set(req, 0x08, 1); // our reference survives the scheduler's temporary reference
    set(req, 0x0c, 1); // native state: prepare capture
    set(req, 0x10, Byte{1}); // Manual records the actual player and world
    set(req, 0x11, Byte{0xfe}); // memory-only dispatch, using this private controller
    set(req, 0x14, 1);
    std::memcpy(req + 0x1a, engine.default_player_reference, 16);
    set(req, 0x30, engine.empty_string);
    set(req, 0x38, engine.empty_string);
    set(req, 0x48, Byte{1}); // normal request, not the game's retry point

    engine.prepare(ctx);
    // One request on a fresh private controller deterministically enters state
    // 2 with bank zero. There is no second dispatcher tick. Later states would
    // normally rotate the native bank, write files and raise UI notifications.
    engine.start(ctx);
    engine.join(ctx);

    check(field<int>(req, 0x0c) == 3 && field<int>(req, 0x08) == 1 &&
          !field<void*>(ctx, 0x98) && !field<void*>(ctx, 0xa0) && !field<int>(ctx, 0xa8),
          "The game did not finish the private memory capture.");
    const auto length = field<std::int32_t>(image_bytes.data(), 0x1fc);
    check(length > 0 && static_cast<std::size_t>(length) <= native_bank_size - native_header_size,
          "The game returned an invalid private checkpoint size.");
    image_bytes.resize(native_header_size + static_cast<std::size_t>(length));
    validate_native_image(image_bytes);
    return image_bytes;
}

Bytes capture_private_world(std::uintptr_t image) {
    check(image != 0 && reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) == image,
          "The game image used for capture is unavailable.");
    check(regular_capture_frame(image), "Capture needs the regular game update. Try F5 again once the world has settled.");
    const auto live = ready_world_controller(image);
    using capture_detail::JobStep;
    const capture_detail::CaptureBindings engine{
        reinterpret_cast<JobStep>(image + PrepareJob), reinterpret_cast<JobStep>(image + StartJob),
        reinterpret_cast<JobStep>(image + JoinJob), reinterpret_cast<const void*>(image + RequestVirtualTable),
        reinterpret_cast<const void*>(image + EmptyString), reinterpret_cast<const void*>(image + DefaultPlayerReference)
    };
    return capture_detail::run_capture_job(engine, live + 0x108);
}
}
