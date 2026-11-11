#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace qs::charging_repair_detail {
constexpr uintptr_t battery_update_rva = 0x2f5c190;
constexpr uintptr_t generator_transfer_rva = 0x2cf6f00;
constexpr uintptr_t generator_unavailable_rva = 0x2c69110;
constexpr float fraction_per_second = 0.01f / 5.f;
struct Sample {
    bool enabled{}, active_charge{};
    float dt{}, battery_before{}, battery_after{}, condition{}, maximum{}, pending_damage{};
};
// active_charge means a verified working external charger connection, including
// a full battery. The native bridge owns source selection and availability; net
// battery change cannot establish it because consumption occurs before charging.
// Each call describes one completed native battery update. There is deliberately
// no timer or accumulated repair credit that can outlive this charging interval.
inline float repaired_condition(const Sample &s) {
    if (!s.enabled || !s.active_charge || !std::isfinite(s.dt) || s.dt <= 0 || s.dt > 1 ||
        !std::isfinite(s.battery_before) || !std::isfinite(s.battery_after) || s.battery_before < 0 ||
        s.battery_after < 0 || !std::isfinite(s.condition) ||
        !std::isfinite(s.maximum) || s.maximum <= 0 || s.condition <= 0 || s.condition >= s.maximum ||
        !std::isfinite(s.pending_damage) || s.pending_damage < 0 || s.pending_damage >= s.condition)
        return s.condition;
    return static_cast<float>(std::min(static_cast<double>(s.maximum),
        static_cast<double>(s.condition) + static_cast<double>(s.maximum) * fraction_per_second * s.dt));
}
}
