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
using GeneratorTransfer = void (*)(void *, void *, float, void *);
using GeneratorUnavailable = bool (*)(void *);
UpdateBattery original_update{};
GeneratorTransfer original_transfer{};
uintptr_t image{};
std::mutex installation;
std::atomic<bool> ready{}, enabled{};
bool activation_failed{};
std::atomic<uint64_t> charging_updates{}, repair_updates{}, excluded_updates{};
std::array<std::atomic<uint64_t>, static_cast<size_t>(ChargingRepairReason::count)> reasons{};
SRWLOCK observation_lock = SRWLOCK_INIT;
ChargingRepairObservation latest;
uint64_t observed_at{};
std::atomic<uint64_t> repaired_at{};

template<class T> T field(uintptr_t address) {
    T result{};
    std::memcpy(&result, reinterpret_cast<const void *>(address), sizeof result);
    return result;
}
struct Observation {
    uintptr_t actor{}, world{};
    uint32_t id{};
    ChargingRepairObservation sample;
};
// Only the native battery call owns these pointers. The nested generator hook
// reports the engine-selected charger; it never searches by distance or keeps
// a vehicle/charger pointer after the update returns.
struct ChargerContext {
    uintptr_t battery{}, entity{};
    bool generator_seen{}, working_generator{};
};
thread_local ChargerContext *current_charger{};

void outcome(ChargingRepairReason reason, ChargingRepairObservation sample) {
    reasons[static_cast<size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    if (reason != ChargingRepairReason::repaired) excluded_updates.fetch_add(1, std::memory_order_relaxed);
    const auto now = GetTickCount64();
    if (reason == ChargingRepairReason::repaired) repaired_at.store(now, std::memory_order_relaxed);
    // Native updates never wait for the overlay, allocate, or write to disk.
    // Keep a coherent small sample at most four times per second.
    if (TryAcquireSRWLockExclusive(&observation_lock)) {
        if (!observed_at || now - observed_at >= 250) {
            sample.reason = reason; latest = sample; observed_at = now;
        }
        ReleaseSRWLockExclusive(&observation_lock);
    }
}
ChargingRepairReason observe(uintptr_t battery, uintptr_t entity, float dt, Observation &out) {
    out.sample.dt = dt;
    __try {
        const auto flags_owner = field<uintptr_t>(image + 0x7bf6e88);
        const auto world = field<uintptr_t>(image + 0x7be6318);
        if (!flags_owner || !world) return ChargingRepairReason::unavailable_world;
        out.sample.game_flags = field<uint64_t>(flags_owner + 8);
        if (out.sample.game_flags & (scene_bits | transition_bits | 0x2000 | 4 | 0x10000000))
            return ChargingRepairReason::blocked_scene;
        if (!battery || !entity) return ChargingRepairReason::invalid_vehicle;
        const auto actor = field<uintptr_t>(entity + 0x8a8);
        if (!actor || field<uintptr_t>(entity + 0x898) != battery || battery != actor + 0xb0 ||
            field<uintptr_t>(actor + 0x68) != entity + 0x20 ||
            !field<uint8_t>(actor + 0x20) || field<uint32_t>(actor + 0x24) != 2)
            return ChargingRepairReason::invalid_vehicle;
        const auto type = field<uintptr_t>(actor) - image;
        if (type != 0x3e26070 && type != 0x3e26a48 && type != 0x3e27790)
            return ChargingRepairReason::invalid_vehicle;
        out.actor = actor; out.world = world; out.id = field<uint32_t>(actor + 8);
        auto &s = out.sample;
        s.actor_id = out.id; s.battery = field<float>(battery + 8); s.capacity = field<float>(battery + 0xc);
        s.condition = field<float>(entity + 0x6e8); s.maximum = field<float>(entity + 0x6ec);
        s.pending_damage = field<float>(entity + 0x6f0); s.battery_flags = field<uint32_t>(battery + 0x48);
        s.valid = true;
        if (!std::isfinite(s.battery) || s.battery < 0 || !std::isfinite(s.capacity) || s.capacity <= 0)
            return ChargingRepairReason::invalid_vehicle;
        return ChargingRepairReason::count; // Valid; outcome is determined after the native update.
    } __except(EXCEPTION_EXECUTE_HANDLER) { return ChargingRepairReason::invalid_vehicle; }
}
bool working_generator(uintptr_t generator, uintptr_t battery, float before) {
    __try {
        if (field<int8_t>(generator + 0xa8) < 0) return false;
        const auto resource = field<uintptr_t>(generator + 0x90);
        if (!resource) return false;
        const auto level = field<uint32_t>(generator + 0x84);
        const float rate = field<float>(resource + 0x4b4 + std::min(level - 1u, 4u) * sizeof(float));
        if (!std::isfinite(rate) || rate <= 0 || field<float>(battery + 0xc) <= 0) return false;
        const float after = field<float>(battery + 8);
        const float multiplier = level > 2 ? field<float>(resource + 0x4dc) : 1.f;
        const float full = field<float>(battery + 0xc) * multiplier;
        if (!std::isfinite(before) || !std::isfinite(after) || !std::isfinite(full) || full <= 0 ||
            !(after > before || after >= full)) return false;
        // This exact-build leaf is a pure availability predicate (resource/level
        // thresholds and generator condition). Reuse it at full battery because
        // the game's short-circuit skips it when no more charge can be accepted.
        return !reinterpret_cast<GeneratorUnavailable>(image + charging_repair_detail::generator_unavailable_rva)
            (reinterpret_cast<void *>(generator));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void generator_hook(void *generator, void *battery, float dt, void *entity) {
    auto *context = current_charger;
    const bool matching = context && context->battery == reinterpret_cast<uintptr_t>(battery) &&
        context->entity == reinterpret_cast<uintptr_t>(entity);
    // Read exactly around the transfer, not around the entire battery update:
    // vehicle consumption may already have reduced it earlier in this frame.
    float before{};
    if (matching) before = field<float>(reinterpret_cast<uintptr_t>(battery) + 8);
    original_transfer(generator, battery, dt, entity);
    if (matching) {
        context->generator_seen = true;
        context->working_generator = working_generator(reinterpret_cast<uintptr_t>(generator),
                                                        reinterpret_cast<uintptr_t>(battery), before);
    }
}
void repair(uintptr_t battery, uintptr_t entity, float dt, const Observation &before, const ChargerContext &charger) {
    __try {
        Observation after;
        auto reason = observe(battery, entity, dt, after);
        if (!enabled.load(std::memory_order_acquire)) reason = ChargingRepairReason::disabled;
        else if (reason == ChargingRepairReason::count &&
                 (before.actor != after.actor || before.world != after.world || before.id != after.id))
            reason = ChargingRepairReason::changed_vehicle;
        if (reason != ChargingRepairReason::count) { outcome(reason, after.sample); return; }
        auto &s = after.sample;
        // A working generator remains a repair source at full battery and when
        // earlier vehicle consumption offsets its charge. Other charge volumes
        // retain the original positive-transfer gate; passive recovery never qualifies.
        const bool other_transfer = !charger.generator_seen && (s.battery_flags & 8) &&
            field<uint8_t>(battery + 0x14) == 1 && s.battery > before.sample.battery;
        const bool connected = charger.working_generator || other_transfer;
        if (!connected) {
            reason = charger.generator_seen ? ChargingRepairReason::charger_unavailable :
                     (s.battery_flags & 8) ? ChargingRepairReason::no_transfer : ChargingRepairReason::no_charger;
            outcome(reason, s); return;
        }
        charging_updates.fetch_add(1, std::memory_order_relaxed);
        if (!std::isfinite(s.condition) || !std::isfinite(s.maximum) || s.maximum <= 0 ||
            !std::isfinite(s.pending_damage) || s.pending_damage < 0) reason = ChargingRepairReason::invalid_condition;
        else if (s.condition <= 0) reason = ChargingRepairReason::destroyed;
        else if (s.condition >= s.maximum) reason = ChargingRepairReason::condition_full;
        else if (s.pending_damage >= s.condition) reason = ChargingRepairReason::pending_fatal_damage;
        else {
            const charging_repair_detail::Sample sample{true, connected, dt, before.sample.battery, s.battery,
                s.condition, s.maximum, s.pending_damage};
            const float next = charging_repair_detail::repaired_condition(sample);
            if (next > s.condition) {
                std::memcpy(reinterpret_cast<void *>(entity + 0x6e8), &next, sizeof next);
                s.condition = next; repair_updates.fetch_add(1, std::memory_order_relaxed);
                reason = ChargingRepairReason::repaired;
            } else reason = ChargingRepairReason::invalid_condition;
        }
        outcome(reason, s);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        outcome(ChargingRepairReason::invalid_vehicle, before.sample);
    }
}
void battery_hook(void *battery, float dt, void *entity, void *driver) {
    const bool wanted = enabled.load(std::memory_order_acquire);
    Observation before;
    const auto b = reinterpret_cast<uintptr_t>(battery), e = reinterpret_cast<uintptr_t>(entity);
    auto reason = ChargingRepairReason::disabled;
    if (wanted) {
        before.sample.dt = dt;
        reason = std::isfinite(dt) && dt > 0 && dt <= 1 ? observe(b, e, dt, before) : ChargingRepairReason::invalid_time;
    }
    ChargerContext charger{b, e};
    struct ContextScope {
        ChargerContext *previous;
        explicit ContextScope(ChargerContext *value) : previous(current_charger) { current_charger = value; }
        ~ContextScope() { current_charger = previous; }
    } scope(reason == ChargingRepairReason::count ? &charger : nullptr);
    original_update(battery, dt, entity, driver);
    if (reason == ChargingRepairReason::count) repair(b, e, dt, before, charger);
    else if (wanted) outcome(reason, before.sample);
}
}

void charging_repair_detail::install(uintptr_t base) {
    std::lock_guard lock(installation);
    if (activation_failed) throw std::runtime_error("Charging repair could not start. Restart the game before retrying.");
    if (ready.load(std::memory_order_acquire)) {
        if (image != base) throw std::runtime_error("Charging repair belongs to another executable.");
        return;
    }
    constexpr uint8_t battery_entry[]{0x48,0x8b,0xc4,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
        0x48,0x81,0xec,0x80,0x01,0x00,0x00,0xc5,0xf8,0x29,0x70,0xb8};
    constexpr uint8_t generator_entry[]{0x48,0x8b,0xc4,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x68,0x01,0x00,0x00};
    if (std::memcmp(reinterpret_cast<const void *>(base + battery_update_rva), battery_entry, sizeof battery_entry) ||
        std::memcmp(reinterpret_cast<const void *>(base + generator_transfer_rva), generator_entry, sizeof generator_entry))
        throw std::runtime_error("Charging code differs from the supported game build or another mod changed it.");
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
        throw std::runtime_error("Could not initialize charging repair.");
    auto *target = reinterpret_cast<void *>(base + battery_update_rva);
    auto *generator = reinterpret_cast<void *>(base + generator_transfer_rva);
    if (MH_CreateHook(target, reinterpret_cast<void *>(battery_hook), reinterpret_cast<void **>(&original_update)) != MH_OK)
        throw std::runtime_error("Could not prepare charging repair.");
    if (MH_CreateHook(generator, reinterpret_cast<void *>(generator_hook), reinterpret_cast<void **>(&original_transfer)) != MH_OK) {
        MH_RemoveHook(target);
        throw std::runtime_error("Could not prepare generator repair.");
    }
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&battery_hook), &pinned)) {
        MH_RemoveHook(target); MH_RemoveHook(generator);
        throw std::runtime_error("Could not keep charging repair loaded.");
    }
    image = base;
    // Enable only our two hooks, never every hook owned by other features.
    if (MH_EnableHook(generator) != MH_OK || MH_EnableHook(target) != MH_OK) {
        activation_failed = true; // A thread may have entered a trampoline; keep it alive and disabled.
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
    ChargingRepairStatus result{ready.load(), enabled.load(), charging_updates.load(std::memory_order_relaxed),
        repair_updates.load(std::memory_order_relaxed), excluded_updates.load(std::memory_order_relaxed)};
    for (size_t i=0; i<result.reasons.size(); ++i) result.reasons[i] = reasons[i].load(std::memory_order_relaxed);
    AcquireSRWLockShared(&observation_lock);
    result.latest = latest; result.observed_at_ms = observed_at;
    ReleaseSRWLockShared(&observation_lock);
    result.repaired_at_ms = repaired_at.load(std::memory_order_relaxed);
    return result;
}
}
