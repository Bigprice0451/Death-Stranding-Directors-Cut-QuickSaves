#include "charging_repair_panel.hpp"
#include "charging_repair.hpp"
#include "charging_repair_diagnostics.hpp"
#include "telemetry.hpp"
#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

namespace qs {
namespace {
bool wanted{};
std::string error;
bool diagnostic_write_failed{};
charging_repair_detail::DiagnosticHeartbeat heartbeat;
void record(const Event &event) {
    const bool failed = !telemetry().write(event);
    if (failed && !diagnostic_write_failed)
        reshade::log::message(reshade::log::level::error,
            "QuickSaves charging repair could not write its local diagnostic log. Check the Diagnostics folder and disk space.");
    diagnostic_write_failed = failed;
}
}
void charging_repair_panel_start(uintptr_t base) {
    wanted = false; error.clear(); heartbeat.reset();
    reshade::get_config_value(nullptr, "DirectorCutQuickSaves", "RepairVehiclesWhileCharging", wanted);
    try {
        start_charging_repair(base, wanted);
        record({"charging_repair_ready", wanted ? "on" : "off"});
    } catch (const std::exception &e) {
        error = e.what(); set_charging_repair_enabled(false);
        record({"charging_repair_error", error});
    }
}
void charging_repair_panel_stop() { set_charging_repair_enabled(false); }
void charging_repair_panel_tick() {
    const auto now = GetTickCount64();
    if (!wanted || !error.empty() || !heartbeat.due(now)) return;
    const auto state = charging_repair_status();
    record({"charging_repair_sample", charging_repair_detail::diagnostic_sample(state, now)});
}
void draw_charging_repair_panel() {
    if (!ImGui::CollapsingHeader("Vehicle repair", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::BeginDisabled(!error.empty());
    if (ImGui::Checkbox("Repair Vehicles While Charging", &wanted)) {
        reshade::set_config_value(nullptr, "DirectorCutQuickSaves", "RepairVehiclesWhileCharging", wanted);
        set_charging_repair_enabled(wanted); heartbeat.reset();
        record({"charging_repair_toggle", wanted ? "on" : "off"});
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Repairs 1%% of maximum vehicle condition per 5 seconds at a working charger.\nA full battery keeps repairing while connected to a working generator.\nStops after leaving or losing power. Destroyed vehicles cannot be repaired.");
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    else if (wanted) {
        const auto state = charging_repair_status();
        const auto now = GetTickCount64();
        ImGui::TextDisabled("%s", charging_repair_detail::status_text(state, now));
        if (state.latest.valid && charging_repair_detail::fresh_observation(state, now))
            ImGui::TextDisabled("Latest vehicle: condition %.1f / %.1f  |  Battery %.1f / %.1f",
                state.latest.condition, state.latest.maximum, state.latest.battery, state.latest.capacity);
    }
    if (diagnostic_write_failed)
        ImGui::TextWrapped("Diagnostics could not be written. Check the Diagnostics folder and available disk space.");
}
}
