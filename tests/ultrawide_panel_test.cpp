// Real ImGui input and the shipping panel; native engine and ReShade storage
// are isolated here. Native hooks have their own original-code fixture.
#include <imgui.h>
#include <imgui_internal.h>
#include "ultrawide_panel.hpp"
#include "ultrawide.hpp"
#include "telemetry.hpp"
#include <map>
#include <iostream>
#include <stdexcept>

namespace {
std::map<std::string, std::string> config;
std::map<ImGuiID, ImRect> bounds;
ImVec2 checkbox;
qs::UltrawideStatus state;
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
void start_ultrawide(uintptr_t base, void *, bool enabled) {
    state.ready = base != 0; state.requested = enabled;
    state.error = base ? "" : "Fixture unsupported game";
}
void set_ultrawide_enabled(bool enabled) { state.requested = enabled; }
UltrawideStatus ultrawide_status() { return state; }
Telemetry::Telemetry(fs::path folder) : folder_(std::move(folder)) {}
Telemetry &telemetry() { static Telemetry instance({}); return instance; }
bool Telemetry::write(const Event &event) const noexcept { if (event.name == "ultrawide_toggle") ++toggle_events; return true; }
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *,ImGuiID id,const ImRect &rect,const ImGuiLastItemData *) { bounds[id]=rect; }
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *,ImGuiID id,const char *label,ImGuiItemStatusFlags) {
    if (std::string(label) == "Force 3440 x 1440 Ultrawide") checkbox = bounds.at(id).GetCenter();
}
void ImGuiTestEngineHook_Log(ImGuiContext *,const char *,...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *,ImGuiID) { return ""; }
void frame() {
    ImGui::NewFrame(); ImGui::SetNextWindowPos({10,10}); ImGui::SetNextWindowSize({1100,300});
    ImGui::Begin("QuickSaves", nullptr, ImGuiWindowFlags_NoSavedSettings);
    qs::draw_ultrawide_panel(); ImGui::End(); ImGui::Render();
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
        qs::ultrawide_panel_start(1,nullptr); frame(); frame();
        check(!state.requested && config.empty() && checkbox.x>0, "Default-off toggle missing");
        click(); check(state.requested && config.at("ForceUltrawide3440x1440") == "1", "Click did not enable/save");
        qs::ultrawide_panel_start(1,nullptr); check(state.requested, "Runtime restart lost preference");
        frame(); click(); check(!state.requested && config.at("ForceUltrawide3440x1440") == "0", "Click did not disable/save");
        qs::ultrawide_panel_start(0,nullptr); frame(); click();
        check(!state.requested && config.at("ForceUltrawide3440x1440") == "0" && toggle_events == 2,
              "Unavailable toggle remained actionable or telemetry incorrect");
        ImGui::DestroyContext();
        std::cout << "Ultrawide checkbox, persistence, runtime restart, failure UI and telemetry passed.\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
