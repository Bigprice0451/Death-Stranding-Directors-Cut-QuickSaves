#include "graphics_panel.hpp"
#include "dlaa.hpp"
#include "graphics_diagnostics.hpp"
#include "dlss_runtime.hpp"
#include "telemetry.hpp"
#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

namespace qs {
namespace {
struct Graphics {
    bool active{}, nvidia_device{},hook_started{},wanted{},trying{},armed{},waiting_exit{};
    uint64_t next_poll{};
    HANDLE helper{},cancel{},ready{};
    dlss::Version disk{},mapped{};
    std::string error,hook_error,last_measurement;
};
Graphics gfx;
GraphicsDiagnosticClock diagnostic_clock;
void close_handle(HANDLE &h) {if(h) CloseHandle(h);h=nullptr;}
void log_graphics(const char *name,const std::string &message) {telemetry().write({name,message});}
void launch_upgrade() {
    if(gfx.helper || gfx.trying) return;
    gfx.trying=true;gfx.error.clear();gfx.waiting_exit=false;
    const auto dir=dlss::module_path(nullptr).parent_path();
    const auto exe=dir/L"DirectorCutQuickSaves"/L"DLSSRuntime.exe";
    const auto pid=std::to_wstring(GetCurrentProcessId());
    gfx.cancel=CreateEventW(nullptr,TRUE,FALSE,(L"Local\\DSDC_DLSS_CANCEL_"+pid).c_str());
    gfx.ready=CreateEventW(nullptr,TRUE,FALSE,(L"Local\\DSDC_DLSS_READY_"+pid).c_str());
    std::wstring args=L"\""+exe.wstring()+L"\" "+pid;
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if(gfx.cancel && gfx.ready && CreateProcessW(exe.c_str(),args.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                                                 nullptr,dir.c_str(),&si,&pi)) {
        gfx.helper=pi.hProcess;CloseHandle(pi.hThread);
        log_graphics("dlss_download","Requested official signed DLSS 310.9.16.");return;
    }
    close_handle(gfx.cancel);close_handle(gfx.ready);
    gfx.error="Could not start the DLSS updater. Extract the whole ZIP, including the DirectorCutQuickSaves folder.";
    log_graphics("dlss_download_error",gfx.error);
}
const char *error_for(DWORD result) {
    switch(result) {
        case dlss::no_write_access:return "No write access to the game folder. Download the official DLL using the link in the readme.";
        case dlss::download_failed:return "Could not download DLSS. Check your connection and toggle again.";
        case dlss::verification_failed:return "The download failed the NVIDIA signature, version, or hash check.";
        case dlss::other_dll:return "Another provider owns nvngx_dlss.dll. Install the official NVIDIA runtime before using this switch.";
        case dlss::already_running:return "Another game instance is already updating DLSS.";
        default:return "The DLSS update could not finish. See the readme for the official download.";
    }
}
void apply_preference() {
    const bool eligible=gfx.disk>=dlss::required && (gfx.mapped==dlss::Version{} || gfx.mapped>=dlss::required);
    const bool enable=gfx.wanted && eligible;
    if(enable!=gfx.armed) {set_dlaa_enabled(enable);gfx.armed=enable;}
    if(gfx.wanted && !eligible && gfx.disk<dlss::required) launch_upgrade();
}
}
void graphics_start(reshade::api::effect_runtime *rt,uintptr_t base) {
    gfx.active=true;gfx.hook_error.clear();gfx.wanted=false;
    unsigned vendor{};
    gfx.nvidia_device=rt->get_device()->get_property(reshade::api::device_properties::vendor_id,&vendor) && vendor==0x10de;
    if(!gfx.nvidia_device) return;
    reshade::get_config_value(nullptr,"DirectorCutQuickSaves","NativeDlaaPresetK",gfx.wanted);
    gfx.disk=dlss::file_version(dlss::module_path(nullptr).parent_path()/L"nvngx_dlss.dll");
    gfx.mapped=dlss::loaded_version();gfx.next_poll=0;
    const bool enable=gfx.wanted && gfx.disk>=dlss::required && (gfx.mapped==dlss::Version{} || gfx.mapped>=dlss::required);
    try {
        if(!gfx.hook_started) {
            start_dlaa_tracking(base,enable);
            auto state=dlaa_status();
            if(!state.hooks_ready) {gfx.hook_error=state.detail;log_graphics("dlss_hook_error",state.detail);return;}
            gfx.hook_started=true;gfx.armed=enable;
        }
    }
    catch(const std::exception &e){gfx.hook_error=e.what();log_graphics("dlss_hook_error",e.what());return;}
    apply_preference();
}
void graphics_stop() {
    gfx.active=false;
    // The pinned game hooks own no swapchain objects; keep them through a resize.
    // Re-arming them here would ask the game to rebuild the next swapchain again.
    // Runtime destruction also happens on resize. The helper deliberately
    // survives game exit so it can finish after Windows releases the old DLL.
}
void graphics_tick() {
    if(!gfx.active || !gfx.nvidia_device || GetTickCount64()<gfx.next_poll) return;
    gfx.next_poll=GetTickCount64()+1000;
    gfx.mapped=dlss::loaded_version();
    if(gfx.helper) {
        gfx.waiting_exit=WaitForSingleObject(gfx.ready,0)==WAIT_OBJECT_0;
        if(WaitForSingleObject(gfx.helper,0)==WAIT_OBJECT_0) {
            DWORD code=dlss::install_failed;GetExitCodeProcess(gfx.helper,&code);
            close_handle(gfx.helper);close_handle(gfx.cancel);close_handle(gfx.ready);gfx.waiting_exit=false;
            if(code==dlss::current || code==dlss::installed) {
                gfx.disk=dlss::file_version(dlss::module_path(nullptr).parent_path()/L"nvngx_dlss.dll");
            } else if(code==dlss::canceled) gfx.trying=false;
            else {gfx.error=error_for(code);log_graphics("dlss_download_error",gfx.error);}
        }
    }
    if(gfx.hook_error.empty()) apply_preference();
    if(gfx.hook_started && diagnostic_clock.take(GetTickCount64()))
        log_graphics("dlss_sample",graphics_diagnostic_text(dlaa_status(),GetTickCount64()));
    if(gfx.armed) {
        const auto state=dlaa_status();
        if(state.evaluated) {
            const std::string measurement=std::to_string(state.input_width)+"x"+std::to_string(state.input_height)+" -> "+
                std::to_string(state.output_width)+"x"+std::to_string(state.output_height)+"; K requested; DLSS "+dlss::version_text(gfx.mapped);
            if(measurement!=gfx.last_measurement) {log_graphics("dlss_evaluated",measurement);gfx.last_measurement=measurement;}
        }
    }
}
void draw_graphics_panel(reshade::api::effect_runtime *) {
    if(!gfx.active) return;
    if(!gfx.nvidia_device) {
        ImGui::TextDisabled("DLAA + Preset K requires an NVIDIA RTX GPU.");return;
    }
    if(ImGui::Checkbox("Force DLAA + Preset K",&gfx.wanted)) {
        reshade::set_config_value(nullptr,"DirectorCutQuickSaves","NativeDlaaPresetK",gfx.wanted);
        if(!gfx.wanted && gfx.cancel) SetEvent(gfx.cancel);
        gfx.trying=false;gfx.error.clear();gfx.last_measurement.clear();gfx.next_poll=0;
        log_graphics("dlss_toggle",gfx.wanted?"on":"off");
        if(gfx.hook_error.empty()) apply_preference();
    }
    if(!gfx.wanted) return;
    if(!gfx.hook_error.empty()) ImGui::TextWrapped("%s",gfx.hook_error.c_str());
    else if(!gfx.error.empty()) ImGui::TextWrapped("%s",gfx.error.c_str());
    else if(gfx.helper) ImGui::TextWrapped("%s",gfx.waiting_exit?
        "DLSS 310.9.16 is ready. Exit the game once to install it, then relaunch.":
        "Downloading and checking NVIDIA's DLSS 310.9.16 runtime...");
    else if(!gfx.armed) ImGui::TextWrapped("Restart the game to use the updated DLSS runtime.");
    else {
        const auto s=dlaa_status(); ImGui::TextWrapped("%s",s.detail.c_str());
        if(s.evaluated) ImGui::Text("DLSS input %u x %u  /  output %u x %u",s.input_width,s.input_height,s.output_width,s.output_height);
    }
    if(gfx.mapped!=dlss::Version{}) ImGui::TextDisabled("Loaded DLSS %s",dlss::version_text(gfx.mapped).c_str());
    ImGui::TextDisabled("Game graphics setting: NVIDIA DLSS Quality");
}
}
