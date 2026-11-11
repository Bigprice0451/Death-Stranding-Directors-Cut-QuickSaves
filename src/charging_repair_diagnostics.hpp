#pragma once
#include "charging_repair.hpp"
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace qs::charging_repair_detail {
// Reports are an enabled-state heartbeat, not a change detector: a stalled
// native hook must still leave evidence. Time is explicit for deterministic tests.
class DiagnosticHeartbeat {
public:
    void reset() { next_ = 0; }
    bool due(uint64_t now) {
        if (now < next_) return false;
        next_ = now + 15000;
        return true;
    }
private:
    uint64_t next_{};
};
inline const char *reason_name(ChargingRepairReason reason) {
    switch (reason) {
    case ChargingRepairReason::disabled: return "disabled";
    case ChargingRepairReason::invalid_time: return "invalid_time";
    case ChargingRepairReason::unavailable_world: return "unavailable_world";
    case ChargingRepairReason::blocked_scene: return "blocked_scene";
    case ChargingRepairReason::invalid_vehicle: return "invalid_vehicle";
    case ChargingRepairReason::changed_vehicle: return "changed_vehicle";
    case ChargingRepairReason::no_charger: return "no_charger";
    case ChargingRepairReason::charger_unavailable: return "charger_unavailable";
    case ChargingRepairReason::no_transfer: return "no_transfer";
    case ChargingRepairReason::invalid_condition: return "invalid_condition";
    case ChargingRepairReason::destroyed: return "destroyed";
    case ChargingRepairReason::condition_full: return "condition_full";
    case ChargingRepairReason::pending_fatal_damage: return "pending_fatal_damage";
    case ChargingRepairReason::repaired: return "repaired";
    default: return "unknown";
    }
}
inline bool fresh_observation(const ChargingRepairStatus &state, uint64_t now) {
    return state.observed_at_ms && now >= state.observed_at_ms && now - state.observed_at_ms <= 3000;
}
inline const char *status_text(const ChargingRepairStatus &state, uint64_t now) {
    if (!state.ready) return "Vehicle repair is unavailable.";
    if (!state.enabled) return "Vehicle repair is off.";
    if (!fresh_observation(state, now)) return "Waiting for live vehicle updates.";
    switch (state.latest.reason) {
    case ChargingRepairReason::repaired: return "Repairing at a working charger.";
    case ChargingRepairReason::condition_full: return "Vehicle condition is already full.";
    case ChargingRepairReason::no_charger: return "Waiting for a vehicle at a working charger.";
    case ChargingRepairReason::charger_unavailable: return "The nearby charger is not supplying power.";
    case ChargingRepairReason::no_transfer: return "Waiting for a working charging connection.";
    case ChargingRepairReason::destroyed: return "Destroyed vehicles cannot be repaired.";
    case ChargingRepairReason::pending_fatal_damage: return "Repair paused: the vehicle has fatal damage pending.";
    case ChargingRepairReason::blocked_scene: return "Repair paused during a menu, cutscene or transition.";
    case ChargingRepairReason::unavailable_world: return "Waiting for the game world to be ready.";
    case ChargingRepairReason::invalid_time: return "Repair paused while simulation time is stopped or invalid.";
    case ChargingRepairReason::invalid_vehicle: return "Waiting for a supported vehicle.";
    case ChargingRepairReason::changed_vehicle: return "Waiting for the vehicle update to settle.";
    case ChargingRepairReason::invalid_condition: return "Vehicle condition is unavailable; see Diagnostics.";
    default: return "Waiting for the next vehicle update.";
    }
}
inline std::string diagnostic_sample(const ChargingRepairStatus &state, uint64_t now) {
    const auto &sample = state.latest;
    std::ostringstream out;
    out.imbue(std::locale::classic()); out << std::setprecision(9);
    out << "charging=" << state.charging_updates << "; repairs=" << state.repair_updates
        << "; excluded=" << state.excluded_updates << "; ready=" << state.ready << "; enabled=" << state.enabled
        << "; observed_at_ms=" << state.observed_at_ms << "; repaired_at_ms=" << state.repaired_at_ms;
    auto age = [&](const char *label, uint64_t at) {
        out << "; " << label << '=';
        if (at && now >= at) out << now - at; else out << -1;
    };
    age("observed_age_ms", state.observed_at_ms); age("repair_age_ms", state.repaired_at_ms);
    out << "; valid=" << sample.valid << "; actor_id=" << sample.actor_id << "; reason=" << reason_name(sample.reason)
        << "; battery=" << sample.battery << "; capacity=" << sample.capacity
        << "; condition=" << sample.condition << "; maximum=" << sample.maximum
        << "; pending_damage=" << sample.pending_damage << "; dt=" << sample.dt
        << "; game_flags=" << sample.game_flags << "; battery_flags=" << sample.battery_flags;
    for (size_t i = 0; i < state.reasons.size(); ++i)
        out << "; reason_" << reason_name(static_cast<ChargingRepairReason>(i)) << '=' << state.reasons[i];
    return out.str();
}
}
