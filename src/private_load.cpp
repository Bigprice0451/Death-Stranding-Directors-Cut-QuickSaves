#include "private_load.hpp"
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace qs {
namespace {
using Byte = unsigned char;
template<class T> T& field(Byte* p, std::size_t offset) {
    return *reinterpret_cast<T*>(p + offset);
}
struct Host { std::uintptr_t base; PrivateLoad::AddressLookup lookup; };
template<class T> T entry(Host host, std::uintptr_t rva) {
    const auto address = host.lookup ? host.lookup(host.base, rva) : host.base + rva;
    return reinterpret_cast<T>(address);
}
Byte* singleton(Host base, std::uintptr_t rva) {
    return *entry<Byte**>(base, rva);
}

// Build 13300582 only. These queue offsets describe fields in the real
// SaveGameSystem. Its request vector contains intrusive engine references.
constexpr auto world_address = 0x7be6318;
constexpr auto controller_offset = 0x2a0;
constexpr auto queue_offset = 0x88;
struct Queue { int count, capacity; Byte** items; };
static_assert(sizeof(Queue) == 16);
using Remove = void (*)(Queue*, int, bool);
using MoveIntoQueue = int (*)(Queue*, Byte**);
using Submit = void (*)(void*, unsigned char, unsigned char, unsigned char, unsigned char, int, bool);
using Step = void (*)(void*);
using Check = bool (*)();

bool contains(Queue& queue, const void* request) {
    for (int i = 0; i < queue.count; ++i)
        if (queue.items[i] == request) return true;
    return false;
}

// The admission function also clears a retry header if the controller has
// one. Give admission an empty, private queue and no retry bank. On the
// observed non-retry, empty-queue path it accesses only the queue, optional
// retry pointer and the two reader-initialization flags. The resulting request
// is constructed by the game and holds the game's own reference to the world.
struct PreparedRequest {
    alignas(16) std::array<Byte, 0x148> context{};
    Remove remove;
    Queue& queue() { return field<Queue>(context.data(), queue_offset); }
    ~PreparedRequest() {
        if (queue().count == 1) remove(&queue(), 0, false);
    }
};

// Scope the source to two synchronous native steps on the regular update
// thread. The native retry bank at +C8 and both native buffers' contents
// remain available. The game later reads the separately owned payload through
// the stream created in phase 9, after this pointer has been restored.
struct ScopedSource {
    Byte*& slot;
    Byte* previous;
    ScopedSource(Byte*& slot, Byte* source) : slot(slot), previous(slot) { slot = source; }
    ~ScopedSource() { slot = previous; }
};

void ensure_idle(Host base, Byte* world, Byte* controller) {
    auto* startup = singleton(base, 0x7be6a48);
    auto* runtime = singleton(base, 0x7be6a40);
    auto* platform = singleton(base, 0x577d3f0);
    if (!startup || !runtime || !platform || !field<Byte*>(world, 0x420) ||
        !field<Byte*>(world, 0x400) || !entry<Check>(base, 0x3668c30)())
        throw std::runtime_error("Load a delivery before restoring a private quicksave.");
    // On Windows the savegame.dat diagnostic publisher does nothing. If a
    // different service retained named data, we could not free it on finish.
    auto* platform_table = field<Byte*>(platform, 0);
    if (!platform_table || field<void*>(platform_table, 0x40) != entry<void*>(base, 0x18e9b10))
        throw std::runtime_error("The game's snapshot service differs from the supported Windows build.");
    const int stage = field<int>(startup, 0x30);
    const int index = field<int>(controller, 0xd0);
    if (stage == 1 || stage == 2 || (index != 0 && index != 1) ||
        !singleton(base, 0x577b670) || !*entry<Byte*>(base, 0x577b681) ||
        !entry<Check>(base, 0x1a00210)() || entry<Check>(base, 0x3668db0)() ||
        entry<Check>(base, 0x3668c50)() || entry<Check>(base, 0x19fdb60)() ||
        field<Queue>(controller, queue_offset).count || field<int>(controller, 0xa8) ||
        field<void*>(controller, 0xb0) || field<void*>(controller, 0x98) ||
        field<void*>(controller, 0xa0) || field<void*>(controller, 8) ||
        field<void*>(controller, 0x10) || (field<unsigned>(world, 0x448) & 2))
        throw std::runtime_error("The game is busy saving, loading, or streaming. Try Load again in a moment.");
}

double position_difference(Host base, const std::array<double, 3>& position) {
    auto* manager = singleton(base, 0x7bc7568);
    auto* player = manager ? field<Byte*>(manager, 0x48) : nullptr;
    auto* entity = player ? field<Byte*>(player, 0x70) : nullptr;
    if (!entity) return -1;
    double sum = 0;
    for (int i = 0; i < 3; ++i) {
        const double delta = field<double>(entity, 0xc8 + i * sizeof(double)) -
                             position[i];
        sum += delta * delta;
    }
    return std::isfinite(sum) ? std::sqrt(sum) : -1;
}
}

void PrivateLoad::free_image() {
    if (image_) VirtualFree(image_, 0, MEM_RELEASE);
    image_ = nullptr;
}

PrivateLoad::~PrivateLoad() {
    // During forced add-on unload Windows must keep any borrowed bytes alive
    // for the game. VirtualAlloc pages survive this DLL and die with ds.exe.
    if (status_.state != LoadState::loading && status_.state != LoadState::interrupted)
        free_image();
}

void PrivateLoad::begin(std::uintptr_t verified_base, const Bytes& image) {
    if (status_.state == LoadState::loading || status_.state == LoadState::interrupted)
        throw std::runtime_error("The previous world restore has not released its data.");
    validate_native_image(image);
    if (!verified_base) throw std::runtime_error("The game build has not been verified.");
    const Host base{verified_base, lookup_};
    auto* world = singleton(base, world_address);
    if (!world) throw std::runtime_error("Load an ordinary delivery in the saved world, then select this quicksave again.");
    auto* controller = world + controller_offset;
    ensure_idle(base, world, controller);
    const auto map = entry<std::uint16_t (*)(void*)>(base, 0x1ddda50)(field<void*>(world, 0x420));
    if (map != native_world_id(image)) {
        char message[160]{};
        std::snprintf(message, sizeof message,
            "This quicksave uses world %04X. The current world is %04X. Open a delivery in the saved world first.",
            static_cast<unsigned>(native_world_id(image)), static_cast<unsigned>(map));
        throw std::runtime_error(message);
    }
    const auto flags = entry<unsigned char (*)(bool)>(base, 0x36355a0)(false);
    if ((flags ^ image[0x25]) & 0x80)
        throw std::runtime_error("This quicksave is incompatible with the current game mode.");

    const int index = 1 - field<int>(controller, 0xd0);
    auto& source = field<Byte*>(controller, 0xb8 + index * sizeof(void*));
    if (!source) throw std::runtime_error("The game's memory reader is not ready.");
    free_image();
    image_ = static_cast<Byte*>(VirtualAlloc(nullptr, native_bank_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!image_) throw std::runtime_error("Cannot reserve memory for the private quicksave.");
    std::memcpy(image_, image.data(), image.size());

    const auto remove = entry<Remove>(base, 0x362fc50);
    PreparedRequest prepared{{}, remove};
    auto& prepare_flags = field<std::uint16_t>(prepared.context.data(), 0x12c);
    prepare_flags = field<std::uint16_t>(controller, 0x12c);
    // Exact same-map memory-load parameters from the engine at 0x263F967.
    entry<Submit>(base, 0x362d750)(prepared.context.data(), 15, 0xfe, 2, 1, -1, true);
    auto& queue = field<Queue>(controller, queue_offset);
    if (prepared.queue().count != 1 || !prepared.queue().items || !prepared.queue().items[0] ||
        field<int>(prepared.queue().items[0], 0xc) != 6 || queue.count ||
        singleton(base, world_address) != world) {
        free_image();
        throw std::runtime_error("The game declined the private quickload request.");
    }
    auto* request = prepared.queue().items[0];
    if (field<Byte*>(request, 0x78) != world || field<Byte>(request, 0x11) != 0xfe ||
        field<Byte>(request, 0x48) != 1) {
        free_image();
        throw std::runtime_error("The native request did not identify the selected world.");
    }
    // This is the game's move-insertion function. It nulls the old owning
    // reference. Removing the emptied temporary vector will not drop the job.
    entry<MoveIntoQueue>(base, 0x1906570)(&queue, prepared.queue().items);
    field<std::uint16_t>(controller, 0x12c) = prepare_flags;
    base_ = verified_base;
    world_ = world;
    request_ = request;
    position_deadline_ = 0;
    status_ = {LoadState::loading, 6, -1};

    auto step = entry<Step>(base, 0x3633640);
    {
        ScopedSource selected(source, image_);
        step(controller);
        if (queue.count == 1 && queue.items[0] == request && field<int>(request, 0xc) == 9 &&
            field<int>(request, 0x2c) == index) {
            status_.native_phase = 9;
            step(controller);
        }
    }
    if (contains(queue, request)) {
        const int phase = field<int>(request, 0xc);
        if (phase == 11 || phase == 14 || phase == 15) {
            // RVA 0x36282C0 decodes the three full-precision saved coordinates
            // here. The preview at header +1E0 rounds each to a float.
            std::memcpy(position_.data(), controller + 0x18, sizeof position_);
            status_.world_position_override = field<Byte>(world, 0x481) != 0;
            status_.native_phase = phase;
            return;
        }
        // An unstarted request must not remain queued for a later frame when
        // the source binding has already reverted to the game's own scratch.
        if ((phase == 6 || phase == 9) && !field<void*>(controller, 8) &&
            !field<void*>(controller, 0x10)) {
            for (int i = 0; i < queue.count; ++i) {
                if (queue.items[i] == request) { remove(&queue, i, false); break; }
            }
            entry<void (*)(void*, int, bool)>(base, 0x2635b50)(world, 3, false);
        } else {
            status_.state = LoadState::interrupted;
            throw std::runtime_error("The game entered an unexpected load phase. The source is being retained.");
        }
    }
    if (field<void*>(controller, 8) || field<void*>(controller, 0x10)) {
        status_.state = LoadState::interrupted;
        throw std::runtime_error("The game retained a reader after declining the quickload. Restart before retrying.");
    }
    free_image();
    status_ = {};
    throw std::runtime_error("The game declined to start reading this quicksave.");
}

PrivateLoadStatus PrivateLoad::poll() {
    if (status_.state != LoadState::loading) return status_;
    const Host base{base_, lookup_};
    if (singleton(base, world_address) != world_) {
        status_.state = image_ ? LoadState::interrupted : LoadState::completed;
        return status_;
    }
    auto* controller = world_ + controller_offset;
    auto& queue = field<Queue>(controller, queue_offset);
    if (image_ && contains(queue, request_)) {
        status_.native_phase = field<int>(static_cast<Byte*>(request_), 0xc);
        return status_;
    }
    // The successful phase-15 path releases the stream and reader, erases the
    // request and clears this flag. A missing request alone is insufficient.
    if (image_) {
        if (field<void*>(controller, 8) || field<void*>(controller, 0x10) ||
            (field<unsigned>(world_, 0x448) & 2)) return status_;
        free_image();
        position_deadline_ = GetTickCount64() + 500;
    }
    status_.position_difference = position_difference(base, position_);
    // A movie may intentionally have no controllable Sam. The native release
    // already finished. Give one ordinary entity spawn a chance to appear.
    if (status_.position_difference < 0 && GetTickCount64() < position_deadline_) return status_;
    status_.state = LoadState::completed;
    return status_;
}
}
