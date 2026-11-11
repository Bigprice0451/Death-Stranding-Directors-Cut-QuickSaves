#include "charging_repair.hpp"
#include "charging_repair_policy.hpp"
#include "autosave.hpp"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace qs {
namespace {
using UpdateBattery = void (*)(void *, float, void *, void *);
UpdateBattery original_update{};
uintptr_t image{};
std::mutex installation;
std::atomic<bool> ready{}, enabled{};
bool activation_failed{};
std::atomic<uint64_t> charging_updates{}, repair_updates{}, excluded_updates{};

template<class T> T field(uintptr_t address) {
    T result{};
    std::memcpy(&result, reinterpret_cast<const void *>(address), sizeof result);
    return result;
}
struct Observation {
    uintptr_t actor{}, world{};
    uint32_t id{};
    float battery{};
};
// Called inside the native vehicle update, on its thread and within its object
// lifetime. No vehicle pointer survives the invocation. Both links are checked
// again after the native call in case it changed/removes the attached entity.
bool observe(uintptr_t battery, uintptr_t entity, Observation &out) {
    __try {
        const auto flags_owner = field<uintptr_t>(image + 0x7bf6e88);
        const auto world = field<uintptr_t>(image + 0x7be6318);
        if (!flags_owner || !world || !battery || !entity) return false;
        if (field<uint64_t>(flags_owner + 8) & (scene_bits | transition_bits | 0x2000 | 4 | 0x10000000))
            return false;
        const auto actor = field<uintptr_t>(entity + 0x8a8);
        if (!actor || field<uintptr_t>(entity + 0x898) != battery || battery != actor + 0xb0 ||
            field<uintptr_t>(actor + 0x68) != entity + 0x20 ||
            !field<uint8_t>(actor + 0x20) || field<uint32_t>(actor + 0x24) != 2)
            return false;
        const auto type = field<uintptr_t>(actor) - image;
        if (type != 0x3e26070 && type != 0x3e26a48 && type != 0x3e27790) return false;
        out = {actor, world, field<uint32_t>(actor + 8), field<float>(battery + 8)};
        return std::isfinite(out.battery) && out.battery >= 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void repair(uintptr_t battery, uintptr_t entity, float dt, const Observation &before) {
    // Keep SEH around only our field access. Never swallow an exception from
    // the original game update or roll back any of its battery/damage changes.
    __try {
        Observation after;
        if (!enabled.load(std::memory_order_acquire) || !observe(battery, entity, after) ||
            before.actor != after.actor || before.world != after.world || before.id != after.id) {
            excluded_updates.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const bool charging = (field<uint32_t>(battery + 0x48) & 8) &&
                              field<uint8_t>(battery + 0x14) == 1 && after.battery > before.battery;
        if (!charging) { excluded_updates.fetch_add(1, std::memory_order_relaxed); return; }
        charging_updates.fetch_add(1, std::memory_order_relaxed);
        const float current = field<float>(entity + 0x6e8);
        const charging_repair_detail::Sample sample{true, charging, dt, before.battery, after.battery,
            current, field<float>(entity + 0x6ec), field<float>(entity + 0x6f0)};
        const float next = charging_repair_detail::repaired_condition(sample);
        if (next > current) {
            // This is the current-condition member of the native vehicle damage
            // component. Its pending damage, maximum, corrosion and callbacks
            // retain their native ownership. The next regular damage update
            // consumes pending damage and refreshes the visible condition.
            std::memcpy(reinterpret_cast<void *>(entity + 0x6e8), &next, sizeof next);
            repair_updates.fetch_add(1, std::memory_order_relaxed);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        excluded_updates.fetch_add(1, std::memory_order_relaxed);
    }
}
void battery_hook(void *battery, float dt, void *entity, void *driver) {
    Observation before;
    const auto battery_address = reinterpret_cast<uintptr_t>(battery);
    const auto entity_address = reinterpret_cast<uintptr_t>(entity);
    const bool eligible = enabled.load(std::memory_order_acquire) && std::isfinite(dt) && dt > 0 && dt <= 1 &&
                          observe(battery_address, entity_address, before);
    original_update(battery, dt, entity, driver);
    if (eligible) repair(battery_address, entity_address, dt, before);
    else if (enabled.load(std::memory_order_relaxed)) excluded_updates.fetch_add(1, std::memory_order_relaxed);
}
}

void charging_repair_detail::install(uintptr_t base) {
    std::lock_guard lock(installation);
    if (activation_failed) throw std::runtime_error("Charging repair could not start. Restart the game before retrying.");
    if (ready.load(std::memory_order_acquire)) {
        if (image != base) throw std::runtime_error("Charging repair belongs to another executable.");
        return;
    }
    constexpr uint8_t entry[]{0x48,0x8b,0xc4,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
        0x48,0x81,0xec,0x80,0x01,0x00,0x00,0xc5,0xf8,0x29,0x70,0xb8};
    if (std::memcmp(reinterpret_cast<const void *>(base + battery_update_rva), entry, sizeof entry))
        throw std::runtime_error("Charging code differs from the supported game build or another mod changed it.");
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        throw std::runtime_error("Could not initialize charging repair.");
    auto *target = reinterpret_cast<void *>(base + battery_update_rva);
    if (MH_CreateHook(target, reinterpret_cast<void *>(battery_hook), reinterpret_cast<void **>(&original_update)) != MH_OK)
        throw std::runtime_error("Could not prepare charging repair.");
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&battery_hook), &pinned)) {
        MH_RemoveHook(target);
        throw std::runtime_error("Could not keep charging repair loaded.");
    }
    image = base;
    if (MH_EnableHook(target) != MH_OK) {
        activation_failed = true; // Keep any trampoline a game thread might have entered.
        throw std::runtime_error("Charging repair could not start. Leave it off and restart the game.");
    }
    ready.store(true, std::memory_order_release);
}
void start_charging_repair(uintptr_t base, bool wanted) {
    if (!base || base != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))
        throw std::runtime_error("Invalid executable for charging repair.");
    charging_repair_detail::install(base);
    set_charging_repair_enabled(wanted);
}
void set_charging_repair_enabled(bool wanted) {
    enabled.store(wanted && ready.load(std::memory_order_acquire), std::memory_order_release);
}
ChargingRepairStatus charging_repair_status() {
    return {ready.load(), enabled.load(), charging_updates.load(std::memory_order_relaxed),
        repair_updates.load(std::memory_order_relaxed), excluded_updates.load(std::memory_order_relaxed)};
}
}
