#include "turning_panel.hpp"
#include "turning.hpp"
#include "telemetry.hpp"
#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

namespace qs {
namespace {
bool wanted{};
std::string error;
uint64_t next_report{}, last_calls{};
}
void turning_panel_start(uintptr_t base) {
    wanted = false; error.clear(); next_report = 0; last_calls = 0;
    reshade::get_config_value(nullptr, "DirectorCutQuickSaves", "SamTurnsOnADime", wanted);
    try {
        start_turning(base, wanted);
        telemetry().write({"turning_ready", wanted ? "on" : "off"});
    } catch (const std::exception &e) {
        error = e.what();
        set_turning_enabled(false);
        telemetry().write({"turning_error", error});
    }
}
void turning_panel_stop() { set_turning_enabled(false); }
void turning_panel_tick() {
    if (!wanted || !error.empty() || GetTickCount64() < next_report) return;
    next_report = GetTickCount64() + 15000;
    const auto state = turning_status();
    const auto calls = state.eligible_calls + state.excluded_calls;
    if (calls == last_calls) return;
    last_calls = calls;
    telemetry().write({"turning_sample", "eligible=" + std::to_string(state.eligible_calls) +
        "; heading=" + std::to_string(state.heading_changes) + "; facing=" + std::to_string(state.facing_changes) +
        "; excluded=" + std::to_string(state.excluded_calls)});
}
void draw_turning_panel() {
    if (!ImGui::CollapsingHeader("Gameplay", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::BeginDisabled(!error.empty());
    if (ImGui::Checkbox("Sam Turns on a Dime", &wanted)) {
        reshade::set_config_value(nullptr, "DirectorCutQuickSaves", "SamTurnsOnADime", wanted);
        set_turning_enabled(wanted);
        next_report = 0;
        telemetry().write({"turning_toggle", wanted ? "on" : "off"});
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Experimental turning assistance; gameplay validation is pending.\nReduces angular slowdown while carrying cargo.\nWalking/running speed and cargo weight stay controlled by the game.\nApplies on foot; paused during cutscenes and loading.");
    }
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    else if (wanted) {
        const auto state = turning_status();
        ImGui::TextDisabled("%s", state.heading_changes || state.facing_changes ?
            "Turning assistance has applied this session." : "Ready - waiting for a slowed turn on foot.");
    }
}
}
