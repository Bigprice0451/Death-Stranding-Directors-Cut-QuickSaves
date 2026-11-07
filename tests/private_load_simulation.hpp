#pragma once
#include "private_load.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace load_test {
using Byte = unsigned char;
template<class T> T& at(Byte* object, std::size_t offset) {
    return *reinterpret_cast<T*>(object + offset);
}
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Queue { int count, capacity; Byte** items; };

// This engine deliberately runs only the handoff and completion protocol.
// It records which bytes each caller can see. No game code or files are used.
struct Engine {
    alignas(16) std::array<Byte, 0x1000> world{};
    alignas(16) std::array<Byte, 0x100> startup{}, runtime{}, service{}, table{}, request{}, manager{}, player{};
    alignas(16) std::array<Byte, 0x100> entity{};
    alignas(16) std::array<Byte, 0x200> first{}, second{}, retry{};
    Byte* world_ptr = world.data();
    Byte* startup_ptr = startup.data();
    Byte* runtime_ptr = runtime.data();
    Byte* service_ptr = service.data();
    Byte* manager_ptr = manager.data();
    Byte ready = 1;
    bool stall_before_read{}, reject_admission{};
    bool saw_private_source{}, released_temp_queue{}, rolled_back{};
    Byte* borrowed{};
    std::uint16_t map = 511;
    inline static Engine* active{};

    Byte* controller() { return world.data() + 0x2a0; }
    Queue& queue() { return at<Queue>(controller(), 0x88); }
    Engine() {
        active = this;
        auto* c = controller();
        at<Byte*>(world.data(), 0x400) = service.data();
        at<Byte*>(world.data(), 0x420) = runtime.data();
        at<Byte*>(c, 0xb8) = first.data();
        at<Byte*>(c, 0xc0) = second.data();
        at<Byte*>(c, 0xc8) = retry.data();
        retry.fill(Byte{0x72});
        second.fill(Byte{0x53});
        at<Byte*>(service.data(), 0) = table.data();
        at<std::uintptr_t>(table.data(), 0x40) = address(&noop);
        at<Byte*>(manager.data(), 0x48) = player.data();
        at<Byte*>(player.data(), 0x70) = entity.data();
        at<double>(entity.data(), 0xc8) = 3;
        at<double>(entity.data(), 0xd0) = 4;
    }
    ~Engine() { active = nullptr; }
    template<class T> static std::uintptr_t address(T value) { return reinterpret_cast<std::uintptr_t>(value); }
    static bool yes() { return true; }
    static bool no() { return false; }
    static void noop() {}
    static unsigned char flags(bool) { return 0; }
    static std::uint16_t map_id(void*) { return active->map; }
    static void submit(void* input, Byte kind, Byte slot, Byte mode, Byte reason, int extra, bool reset) {
        auto& e = *active;
        auto* context = static_cast<Byte*>(input);
        require(kind == 15 && slot == 0xfe && mode == 2 && reason == 1 && extra == -1 && reset,
                "Wrong native memory-load parameters");
        require(context != e.controller() && !at<Byte*>(context, 0xc8), "Admission could clear the real retry bank");
        if (e.reject_admission) return;
        auto& q = at<Queue>(context, 0x88);
        q = {1, 1, new Byte*[1]{e.request.data()}};
        at<int>(e.request.data(), 0xc) = 6;
        at<Byte>(e.request.data(), 0x11) = slot;
        at<Byte>(e.request.data(), 0x48) = reason;
        at<Byte*>(e.request.data(), 0x78) = e.world.data();
        at<std::uint16_t>(context, 0x12c) = 0;
    }
    static int move(Queue* q, Byte** item) {
        require(!q->count && *item == active->request.data(), "Unexpected ownership transfer");
        *q = {1, 1, new Byte*[1]{*item}};
        *item = nullptr;
        return 0;
    }
    static void remove(Queue* q, int index, bool keep_capacity) {
        require(q->count == 1 && index == 0 && !keep_capacity, "Wrong queue removal");
        if (q != &active->queue()) active->released_temp_queue = (q->items[0] == nullptr);
        delete[] q->items;
        *q = {};
    }
    static void step(void* context) {
        auto& e = *active;
        auto* c = static_cast<Byte*>(context);
        require(c == e.controller() && e.queue().count == 1, "Game cannot see the active load");
        auto* source = at<Byte*>(c, 0xc0);
        require(source != e.second.data() && at<Byte*>(c, 0xc8) == e.retry.data(), "Source or retry bank is wrong");
        require(at<std::uint16_t>(source, 0x26) == e.map, "Wrong private world data");
        auto& phase = at<int>(e.request.data(), 0xc);
        if (phase == 6) {
            if (!e.stall_before_read) { at<int>(e.request.data(), 0x2c) = 1; phase = 9; }
            return;
        }
        require(phase == 9, "Loader advanced an unrelated phase");
        e.borrowed = source + 0x200;
        e.saw_private_source = true;
        // Deliberately differ from the rounded preview in the fixture, so the
        // measured distance proves that the decoded native position was used.
        at<double>(c, 0x18) = 6;
        at<double>(c, 0x20) = 8;
        at<double>(e.entity.data(), 0xc8) = 9;
        at<double>(e.entity.data(), 0xd0) = 12;
        at<Byte>(e.world.data(), 0x481) = 1;
        at<void*>(c, 8) = e.borrowed;
        at<void*>(c, 0x10) = e.borrowed;
        at<unsigned>(e.world.data(), 0x448) = 2;
        phase = 14;
    }
    static void rollback(void* world, int reason, bool arg) {
        require(world == active->world.data() && reason == 3 && !arg, "Wrong rollback");
        active->rolled_back = true;
    }
    static std::uintptr_t lookup(std::uintptr_t, std::uintptr_t rva) {
        auto& e = *active;
        switch (rva) {
        case 0x7be6318: return address(&e.world_ptr);
        case 0x7be6a48: return address(&e.startup_ptr);
        case 0x7be6a40: return address(&e.runtime_ptr);
        case 0x577d3f0: case 0x577b670: return address(&e.service_ptr);
        case 0x577b681: return address(&e.ready);
        case 0x7bc7568: return address(&e.manager_ptr);
        case 0x18e9b10: return address(&noop);
        case 0x3668c30: case 0x1a00210: return address(&yes);
        case 0x3668db0: case 0x3668c50: case 0x19fdb60: return address(&no);
        case 0x1ddda50: return address(&map_id);
        case 0x36355a0: return address(&flags);
        case 0x362fc50: return address(&remove);
        case 0x362d750: return address(&submit);
        case 0x1906570: return address(&move);
        case 0x3633640: return address(&step);
        case 0x2635b50: return address(&rollback);
        default: throw std::runtime_error("Unexpected engine dependency in offline load test");
        }
    }
};

inline void exercise(const qs::Bytes& image) {
    Engine engine;
    qs::PrivateLoad loader(Engine::lookup);
    engine.map = 512;
    bool denied = false;
    try { loader.begin(1, image); } catch (const std::runtime_error&) { denied = true; }
    require(denied && !engine.queue().count && !engine.saw_private_source, "Wrong-world load was accepted");
    engine.map = 511;
    engine.stall_before_read = true;
    denied = false;
    try { loader.begin(1, image); } catch (const std::runtime_error&) { denied = true; }
    require(denied && engine.rolled_back && !engine.queue().count && engine.released_temp_queue,
            "A stalled load left a request or a temporary queue behind");
    engine.stall_before_read = false;
    for (bool missing_player : {false, true}) {
        loader.begin(1, image);
        auto* c = engine.controller();
        require(engine.saw_private_source && engine.released_temp_queue &&
                at<Byte*>(c, 0xc0) == engine.second.data() && at<Byte*>(c, 0xc8) == engine.retry.data(),
                "A native source was replaced beyond the request handoff");
        require(std::all_of(engine.retry.begin(), engine.retry.end(), [](Byte b){ return b == 0x72; }) &&
                std::all_of(engine.second.begin(), engine.second.end(), [](Byte b){ return b == 0x53; }),
                "Vanilla bytes changed during private load");
        require(loader.poll().native_phase == 14 && std::memcmp(engine.borrowed, "abc", 3) == 0,
                "Source did not survive the asynchronous native reader");
        at<int>(engine.request.data(), 0xc) = 15;
        require(loader.poll().native_phase == 15, "Final streaming phase was lost");
        Engine::remove(&engine.queue(), 0, false);
        require(loader.poll().state == qs::LoadState::loading && std::memcmp(engine.borrowed, "abc", 3) == 0,
                "Missing request freed a still-borrowed source");
        at<void*>(c, 8) = nullptr;
        at<void*>(c, 0x10) = nullptr;
        at<unsigned>(engine.world.data(), 0x448) = 0;
        if (missing_player) {
            engine.manager_ptr = nullptr;
            require(loader.poll().state == qs::LoadState::loading, "Player observation was skipped");
            Sleep(550);
        }
        const auto result = loader.poll();
        require(result.state == qs::LoadState::completed && result.world_position_override &&
                result.position_difference == (missing_player ? -1 : 5),
                "Completion or position observation was incorrect");
    }
}
}
