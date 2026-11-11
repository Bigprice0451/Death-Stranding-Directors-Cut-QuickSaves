#include "charging_repair_panel.hpp"
#include "charging_repair.hpp"
#include "telemetry.hpp"
#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

namespace qs {
namespace {
bool wanted{};
std::string error;
uint64_t next_report{}, last_updates{};
}
void charging_repair_panel_start(uintptr_t base) {
    wanted = false; error.clear(); next_report = last_updates = 0;
    reshade::get_config_value(nullptr, "DirectorCutQuickSaves", "RepairVehiclesWhileCharging", wanted);
    try {
        start_charging_repair(base, wanted);
        telemetry().write({"charging_repair_ready", wanted ? "on" : "off"});
    } catch (const std::exception &e) {
        error = e.what(); set_charging_repair_enabled(false);
        telemetry().write({"charging_repair_error", error});
    }
}
void charging_repair_panel_stop() { set_charging_repair_enabled(false); }
void charging_repair_panel_tick() {
    if (!wanted || !error.empty() || GetTickCount64() < next_report) return;
    next_report = GetTickCount64() + 15000;
    const auto state = charging_repair_status();
    const auto updates = state.charging_updates + state.excluded_updates;
    if (updates == last_updates) return;
    last_updates = updates;
    telemetry().write({"charging_repair_sample", "charging=" + std::to_string(state.charging_updates) +
        "; repairs=" + std::to_string(state.repair_updates) + "; excluded=" + std::to_string(state.excluded_updates)});
}
void draw_charging_repair_panel() {
    if (!ImGui::CollapsingHeader("Vehicle repair", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::BeginDisabled(!error.empty());
    if (ImGui::Checkbox("Repair Vehicles While Charging", &wanted)) {
        reshade::set_config_value(nullptr, "DirectorCutQuickSaves", "RepairVehiclesWhileCharging", wanted);
        set_charging_repair_enabled(wanted); next_report = 0;
        telemetry().write({"charging_repair_toggle", wanted ? "on" : "off"});
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Repairs 1%% of maximum vehicle condition per 5 seconds of actual charging.\nStops when charging stops or the battery is full.\nDestroyed vehicles cannot be repaired. Gameplay validation is pending.");
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    else if (wanted) {
        const auto state = charging_repair_status();
        ImGui::TextDisabled("%s", state.repair_updates ? "Vehicle repair has applied this session." :
            "Ready - waiting for a damaged vehicle to charge.");
    }
}
}
