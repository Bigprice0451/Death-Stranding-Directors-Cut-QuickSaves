// Real ImGui input and the shipping panel; native engine and ReShade storage
// are isolated here. Native hooks have their own original-code fixture.
#include <imgui.h>
#include <imgui_internal.h>
#include "charging_repair_panel.hpp"
#include "charging_repair.hpp"
#include "charging_repair_diagnostics.hpp"
#include "telemetry.hpp"
#include <windows.h>
#include <map>
#include <iostream>
#include <stdexcept>

namespace {
std::map<std::string, std::string> config;
std::map<ImGuiID, ImRect> bounds;
ImVec2 checkbox;
qs::ChargingRepairStatus state;
unsigned toggle_events{}, sample_events{};
unsigned diagnostic_errors{};
bool writes_work = true;
std::string sample_detail, visible_text;
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
void ReShadeLogMessage(void *, int level, const char *message) {
    if (level == static_cast<int>(reshade::log::level::error) &&
        std::string(message).find("could not write") != std::string::npos) ++diagnostic_errors;
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
bool Telemetry::write(const Event &event) const noexcept {
    if (event.name == "charging_repair_toggle") ++toggle_events;
    if (event.name == "charging_repair_sample") { ++sample_events; sample_detail=event.detail; }
    return writes_work;
}
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
    ImGui::LogToBuffer(); qs::draw_charging_repair_panel();
    visible_text=ImGui::GetCurrentContext()->LogBuffer.c_str(); ImGui::LogFinish();
    ImGui::End(); ImGui::Render();
}
void click() {
    auto &io=ImGui::GetIO(); io.AddMousePosEvent(checkbox.x,checkbox.y); frame();
    io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame(); frame();
}
int main() {
    try {
        using namespace qs::charging_repair_detail;
        DiagnosticHeartbeat clock;
        check(clock.due(1000) && !clock.due(15999) && clock.due(16000), "Heartbeat is not every 15 seconds");
        check(!clock.due(16001) && clock.due(31000), "Unchanged heartbeat did not repeat");
        clock.reset(); check(clock.due(31001), "Toggle did not request an immediate heartbeat");
        qs::ChargingRepairStatus observation;
        observation.ready=observation.enabled=true;
        observation.repair_updates=12; observation.observed_at_ms=10000; observation.repaired_at_ms=8000;
        observation.latest={123,qs::ChargingRepairReason::no_charger,3000,3000,703,7500,0,.02f,64,8,true};
        observation.reasons[static_cast<size_t>(qs::ChargingRepairReason::no_charger)]=17;
        check(std::string(status_text(observation,10000)).find("Waiting")!=std::string::npos,
              "Historical repairs hid a current no-charger state");
        observation.latest.reason=qs::ChargingRepairReason::repaired;
        check(std::string(status_text(observation,10000)).find("Repairing")!=std::string::npos,
              "Fresh repair was not shown");
        check(std::string(status_text(observation,14000))=="Waiting for live vehicle updates.",
              "Stale repair remained active in the UI");
        const auto sample=diagnostic_sample(observation,11000);
        for (const auto *field : {"repairs=12", "actor_id=123", "battery=3000", "capacity=3000",
             "condition=703", "maximum=7500", "game_flags=64", "battery_flags=8", "observed_age_ms=1000",
             "repair_age_ms=3000", "reason=repaired", "reason_no_charger=17", "reason_charger_unavailable=0"})
            check(sample.find(field)!=std::string::npos, "Diagnostic sample omitted an observed field or reason count");
        check(diagnostic_sample({},10000).find("observed_age_ms=-1")!=std::string::npos,
              "Unobserved state invented an observation age");
        ImGui::CreateContext()->TestEngineHookItems = true;
        auto &io=ImGui::GetIO(); io.DisplaySize={3440,1440}; io.IniFilename=nullptr; io.DeltaTime=1.f/60.f;
        unsigned char *pixels{}; int width{},height{}; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        config["SamTurnsOnADime"]="1";
        config["ActualUltrawide"]="0";
        qs::charging_repair_panel_start(1); frame(); frame();
        check(!state.enabled && config.size()==2 && checkbox.x>0, "Default-off toggle missing");
        qs::charging_repair_panel_tick(); check(sample_events==0, "Disabled repair emitted a heartbeat");
        click(); check(state.enabled && config.at("RepairVehiclesWhileCharging") == "1", "Click did not enable/save");
        qs::charging_repair_panel_tick();
        check(sample_events==1, "Enabled repair telemetry did not report a zero-update heartbeat");
        qs::charging_repair_panel_tick();
        check(sample_events==1, "Repair heartbeat logged every frame");
        check(sample_detail.find("charging=0")!=std::string::npos && sample_detail.find("observed_age_ms=-1")!=std::string::npos,
              "Quiet hook heartbeat omitted its empty-state evidence");
        state.repair_updates=12; state.observed_at_ms=GetTickCount64();
        state.latest.reason=qs::ChargingRepairReason::no_charger; frame();
        check(visible_text.find("Waiting for a vehicle at a working charger.")!=std::string::npos,
              "Shipping panel displayed historical success instead of current status");
        state.latest.reason=qs::ChargingRepairReason::repaired; frame();
        check(visible_text.find("Repairing at a working charger.")!=std::string::npos, "Shipping panel omitted current repair");
        state.observed_at_ms=GetTickCount64()-4000; frame();
        check(visible_text.find("Waiting for live vehicle updates.")!=std::string::npos, "Shipping panel kept stale success");
        writes_work=false; qs::charging_repair_panel_start(1); qs::charging_repair_panel_tick(); frame();
        check(diagnostic_errors==1 && visible_text.find("Diagnostics could not be written.")!=std::string::npos,
              "Diagnostic write failure was hidden or repeated in the ReShade log");
        writes_work=true; qs::charging_repair_panel_start(1); frame();
        check(visible_text.find("Diagnostics could not be written.")==std::string::npos,
              "Recovered diagnostics kept showing a write failure");
        qs::charging_repair_panel_stop(); check(!state.enabled, "Runtime close did not suspend");
        qs::charging_repair_panel_start(1); check(state.enabled, "Runtime restart lost preference");
        frame(); click(); check(!state.enabled && config.at("RepairVehiclesWhileCharging") == "0", "Click did not disable/save");
        qs::charging_repair_panel_start(0); frame(); click();
        check(!state.enabled && config.at("RepairVehiclesWhileCharging") == "0" && toggle_events == 2,
              "Unavailable toggle remained actionable or telemetry incorrect");
        check(config.at("SamTurnsOnADime")=="1" && config.at("ActualUltrawide")=="0",
              "Charging preference modified an independent feature");
        ImGui::DestroyContext();
        std::cout << "Charging repair checkbox, persistence, current/stale status, heartbeat evidence and visible log failures passed.\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}

