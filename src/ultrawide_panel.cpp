#include "ultrawide_panel.hpp"
#include "ultrawide.hpp"
#include "telemetry.hpp"
#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

namespace qs {
namespace {
bool preference{};
uint64_t next_sample{};
uint32_t output_width{},output_height{};
std::string last_report;
}
void ultrawide_panel_start(uintptr_t base, void *window) {
    preference=false;
    reshade::get_config_value(nullptr,"DirectorCutQuickSaves","ForceUltrawide3440x1440",preference);
    start_ultrawide(base,window,preference);
    next_sample=0;
    const auto status=ultrawide_status();
    telemetry().write({status.ready?"ultrawide_ready":"ultrawide_error",status.ready?(preference?"on":"off"):status.error});
}
void ultrawide_panel_tick(reshade::api::effect_runtime *rt) {
    if (GetTickCount64()<next_sample) return;
    next_sample=GetTickCount64()+1000;
    rt->get_screenshot_width_and_height(&output_width,&output_height);
    const auto status=ultrawide_status();
    if (!status.requested && !status.restoring) return;
    const std::string sample="image="+std::to_string(status.render.width)+"x"+std::to_string(status.render.height)+
        "; viewport="+std::to_string(status.viewport.width)+"x"+std::to_string(status.viewport.height)+
        "; output="+std::to_string(output_width)+"x"+std::to_string(output_height)+
        "; aspect="+std::to_string(status.aspect)+"; focused="+std::to_string(status.focused)+
        "; monitor="+std::to_string(status.monitor_matches)+"; repairs="+std::to_string(status.repairs)+
        "; restoring="+std::to_string(status.restoring);
    if (sample!=last_report) {last_report=sample;telemetry().write({"ultrawide_sample",sample});}
}
void draw_ultrawide_panel() {
    const auto status=ultrawide_status();
    ImGui::BeginDisabled(!status.ready);
    if (ImGui::Checkbox("Force 3440 x 1440 Ultrawide",&preference)) {
        reshade::set_config_value(nullptr,"DirectorCutQuickSaves","ForceUltrawide3440x1440",preference);
        set_ultrawide_enabled(preference);
        next_sample=0;last_report.clear();
        telemetry().write({"ultrawide_toggle",preference?"on":"off"});
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Use the full 3440 x 1440 image area regardless of the game's resolution choice.\nReapplies after Alt+Tab returns; does not take focus while you are away.\nRequires a monitor currently running at 3440 x 1440.\nSwitch off to restore the game's selected resolution. DLAA remains a separate option.");
    if (!status.error.empty()) {ImGui::TextWrapped("%s",status.error.c_str());return;}
    if (!preference && !status.restoring) return;
    if (!status.focused) ImGui::TextDisabled("Waiting for the game to regain focus.");
    else if (status.restoring) ImGui::TextDisabled("Restoring the game's selected resolution...");
    else if (status.last_sample && !status.monitor_matches) ImGui::TextWrapped("Set this monitor to 3440 x 1440 in Windows to use this option.");
    else if (status.measured && output_width==3440 && output_height==1440)
        ImGui::TextDisabled("Full 3440 x 1440 game image and output measured.");
    else if (status.attempts>=3)
        ImGui::TextWrapped("The game deferred the resize. Return from Alt+Tab or reapply display settings to retry.");
    else ImGui::TextDisabled("Applying 3440 x 1440 on the game's next display update...");
    if (status.render.valid()) ImGui::TextDisabled("Game image %u x %u  |  Output %u x %u",status.render.width,status.render.height,output_width,output_height);
}
}
