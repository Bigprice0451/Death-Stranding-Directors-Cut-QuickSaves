#include "turning.hpp"
#include "turning_policy.hpp"
#include "autosave.hpp"
#include "vehicle_live.hpp"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace qs {
namespace {
using Heading = void (*)(void *, float);
using Facing = void (*)(void *, void *, float);
Heading original_heading{};
Facing original_facing{};
uintptr_t image{};
std::mutex install_mutex;
std::atomic<bool> ready{}, enabled{};
bool activation_failed{}; // Protected by install_mutex; callbacks never read this.
std::atomic<uint64_t> eligible_calls{}, heading_changes{}, facing_changes{}, excluded_calls{};

template<class T> T field(uintptr_t address) {
    T result{};
    std::memcpy(&result, reinterpret_cast<const void *>(address), sizeof result);
    return result;
}

// Called synchronously inside the mover update, never from the render thread.
// SEH handles stale world pointers without a kernel read for every field/frame.
// Do not extend this exception region around the original game's function.
bool local_on_foot(void *mover) {
    __try {
        const auto flags_owner = field<uintptr_t>(image + 0x7bf6e88);
        const auto manager = field<uintptr_t>(image + 0x7bc7568);
        if (!flags_owner || !manager || !field<uintptr_t>(image + 0x7be6318)) return false;
        const auto flags = field<uint64_t>(flags_owner + 8);
        if (flags & (scene_bits | transition_bits | 0x2000 | 0x4 | 0x10000000)) return false;
        const auto player = field<uintptr_t>(manager + 0x48);
        if (!player || field<int>(player + 0x88) != 0 || field<uint32_t>(player + 0x3bc) != no_ride)
            return false;
        const auto entity = field<uintptr_t>(player + 0x70);
        const auto address = reinterpret_cast<uintptr_t>(mover);
        return entity && address && field<uintptr_t>(entity + 0x54b0) == address &&
               field<uintptr_t>(address + 0x48) == entity && field<uintptr_t>(address + 0x2c8) == entity;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool applies(void *mover, float dt) {
    if (!enabled.load(std::memory_order_acquire)) return false;
    if (std::isfinite(dt) && dt > 0 && local_on_foot(mover)) {
        eligible_calls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    excluded_calls.fetch_add(1, std::memory_order_relaxed);
    return false;
}
void heading_hook(void *mover, float dt) {
    turning_detail::ScopedResponse response(mover, applies(mover, dt), true);
    if (response.changed()) heading_changes.fetch_add(1, std::memory_order_relaxed);
    original_heading(mover, dt);
}
void facing_hook(void *mover, void *out, float dt) {
    turning_detail::ScopedResponse response(mover, applies(mover, dt), false);
    if (response.changed()) facing_changes.fetch_add(1, std::memory_order_relaxed);
    original_facing(mover, out, dt);
}
template<size_t N> void signature(uintptr_t base, uintptr_t rva, const uint8_t (&bytes)[N]) {
    if (std::memcmp(reinterpret_cast<const void *>(base + rva), bytes, N))
        throw std::runtime_error("Turning code differs from the supported game build or another mod changed it.");
}
}

void turning_detail::install(uintptr_t base) {
    std::lock_guard lock(install_mutex);
    if (activation_failed)
        throw std::runtime_error("Turning hooks could not both start. Restart the game before retrying.");
    if (ready.load()) {
        if (image != base) throw std::runtime_error("Turning hooks belong to another executable.");
        return;
    }
    signature(base, heading_rva, {0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x78,0x20,0x55,0x48,0x8d,0x68,0xa8});
    signature(base, facing_rva, {0x48,0x8b,0xc4,0x55,0x53,0x41,0x56,0x48,0x8d,0x68,0x98,0x48,0x81,0xec,0x50,0x01});
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        throw std::runtime_error("Could not initialize turning hooks.");
    auto *a = reinterpret_cast<void *>(base + heading_rva);
    auto *b = reinterpret_cast<void *>(base + facing_rva);
    if (MH_CreateHook(a, reinterpret_cast<void *>(heading_hook), reinterpret_cast<void **>(&original_heading)) != MH_OK)
        throw std::runtime_error("Could not prepare heading control.");
    if (MH_CreateHook(b, reinterpret_cast<void *>(facing_hook), reinterpret_cast<void **>(&original_facing)) != MH_OK) {
        MH_RemoveHook(a);
        throw std::runtime_error("Could not prepare facing control.");
    }
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&heading_hook), &pinned)) {
        MH_RemoveHook(a); MH_RemoveHook(b);
        throw std::runtime_error("Could not keep turning callbacks loaded.");
    }
    image = base;
    // Effects stay disabled until both hooks are installed. On a partial
    // failure retain any live trampoline: never free code a game thread may use.
    if (MH_EnableHook(a) != MH_OK || MH_EnableHook(b) != MH_OK) {
        activation_failed = true;
        throw std::runtime_error("Turning hooks could not both start. Leave the switch off and restart the game.");
    }
    ready.store(true, std::memory_order_release);
}
void start_turning(uintptr_t base, bool wanted) {
    if (!base || base != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))
        throw std::runtime_error("Invalid executable for turning control.");
    turning_detail::install(base);
    set_turning_enabled(wanted);
}
void set_turning_enabled(bool wanted) {
    enabled.store(wanted && ready.load(std::memory_order_acquire), std::memory_order_release);
}
TurningStatus turning_status() {
    return {ready.load(), enabled.load(), eligible_calls.load(std::memory_order_relaxed),
            heading_changes.load(std::memory_order_relaxed), facing_changes.load(std::memory_order_relaxed),
            excluded_calls.load(std::memory_order_relaxed)};
}
}
