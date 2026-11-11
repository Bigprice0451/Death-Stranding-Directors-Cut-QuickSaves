// Real ImGui input and the shipping panel; native engine and ReShade storage
// are isolated here. Native hooks have their own original-code fixture.
#include <imgui.h>
#include <imgui_internal.h>
#include "charging_repair_panel.hpp"
#include "charging_repair.hpp"
#include "telemetry.hpp"
#include <map>
#include <iostream>
#include <stdexcept>

namespace {
std::map<std::string, std::string> config;
std::map<ImGuiID, ImRect> bounds;
ImVec2 checkbox;
qs::ChargingRepairStatus state;
unsigned toggle_events{};
void check(bool condition, const char *why) { if (!condition) throw std::runtime_error(why); }
}
bool ReShadeGetConfigValue(void *, reshade::api::effect_runtime *runtime, const char *section, const char *key,
                          char *value, size_t *size) {
    check(!runtime && std::string(section) == "DirectorCutQuickSaves", "Wrong preference scope");
    const auto found = config.find(key);
    if (found == config.end()) return false;
    check(*size > found->second.size(), "Preference buffer too short");
    std::memcpy(value, found->second.c_str(), found->second.size() + 1);
    *size = found->second.size(); return true;
}
void ReShadeSetConfigValue(void *, reshade::api::effect_runtime *runtime, const char *section, const char *key, const char *value) {
    check(!runtime && std::string(section) == "DirectorCutQuickSaves", "Wrong preference scope");
    config[key] = value;
}
namespace qs {
void start_charging_repair(uintptr_t base, bool enabled) {
    if (!base) throw std::runtime_error("Fixture unsupported game");
    state.ready = true; state.enabled = enabled;
}
void set_charging_repair_enabled(bool enabled) { state.enabled = enabled; }
ChargingRepairStatus charging_repair_status() { return state; }
Telemetry::Telemetry(fs::path folder) : folder_(std::move(folder)) {}
Telemetry &telemetry() { static Telemetry instance({}); return instance; }
bool Telemetry::write(const Event &event) const noexcept { if (event.name == "charging_repair_toggle") ++toggle_events; return true; }
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *,ImGuiID id,const ImRect &rect,const ImGuiLastItemData *) { bounds[id]=rect; }
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *,ImGuiID id,const char *label,ImGuiItemStatusFlags) {
    if (std::string(label) == "Repair Vehicles While Charging") checkbox = bounds.at(id).GetCenter();
}
void ImGuiTestEngineHook_Log(ImGuiContext *,const char *,...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *,ImGuiID) { return ""; }
void frame() {
    ImGui::NewFrame(); ImGui::SetNextWindowPos({10,10}); ImGui::SetNextWindowSize({1100,300});
    ImGui::Begin("QuickSaves", nullptr, ImGuiWindowFlags_NoSavedSettings);
    qs::draw_charging_repair_panel(); ImGui::End(); ImGui::Render();
}
void click() {
    auto &io=ImGui::GetIO(); io.AddMousePosEvent(checkbox.x,checkbox.y); frame();
    io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame(); frame();
}
int main() {
    try {
        ImGui::CreateContext()->TestEngineHookItems = true;
        auto &io=ImGui::GetIO(); io.DisplaySize={3440,1440}; io.IniFilename=nullptr; io.DeltaTime=1.f/60.f;
        unsigned char *pixels{}; int width{},height{}; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        config["SamTurnsOnADime"]="1";
        config["ActualUltrawide"]="0";
        qs::charging_repair_panel_start(1); frame(); frame();
        check(!state.enabled && config.size()==2 && checkbox.x>0, "Default-off toggle missing");
        click(); check(state.enabled && config.at("RepairVehiclesWhileCharging") == "1", "Click did not enable/save");
        qs::charging_repair_panel_stop(); check(!state.enabled, "Runtime close did not suspend");
        qs::charging_repair_panel_start(1); check(state.enabled, "Runtime restart lost preference");
        frame(); click(); check(!state.enabled && config.at("RepairVehiclesWhileCharging") == "0", "Click did not disable/save");
        qs::charging_repair_panel_start(0); frame(); click();
        check(!state.enabled && config.at("RepairVehiclesWhileCharging") == "0" && toggle_events == 2,
              "Unavailable toggle remained actionable or telemetry incorrect");
        check(config.at("SamTurnsOnADime")=="1" && config.at("ActualUltrawide")=="0",
              "Charging preference modified an independent feature");
        ImGui::DestroyContext();
        std::cout << "Charging repair checkbox, persistence, runtime lifecycle, failure UI and telemetry passed.\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

