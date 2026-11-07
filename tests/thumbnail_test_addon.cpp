#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "thumbnails.hpp"
#include "quick_keys.hpp"
using Runtime=reshade::api::effect_runtime;
static qs::ThumbnailCache cache;
static qs::Entry version{};
static qs::Bytes png;
static int displayed, reads,key_events;
static bool key_failure;
static void log(const char *msg){reshade::log::message(reshade::log::level::info,msg);}
static void start(Runtime *) {
    reshade::set_config_value(nullptr,"DirectorCutQuickSaves","QuickSaveKeys","116 67 66");
    qs::keys_start();key_events=0;key_failure=false;
    displayed=reads=0;version.slot=1;version.sequence=1;
    qs::ThumbnailPixels source{16,8,qs::Bytes(16*8*4)};
    for(size_t n=0;n<source.rgba.size();n+=4){source.rgba[n]=255;source.rgba[n+1]=20;source.rgba[n+2]=215;source.rgba[n+3]=255;}
    png=qs::encode_thumbnail(source);
}
static void draw(Runtime *rt) {
    ++displayed;
    const bool expected=rt->is_key_pressed(VK_F5)||rt->is_key_pressed('C')||rt->is_key_pressed('B');
    const bool pressed=qs::quick_key_pressed(rt);
    if(pressed) ++key_events;
    if(expected!=pressed || qs::quick_key_pressed(rt)) key_failure=true;
    if(displayed==8) ++version.sequence;
    ImGui::SetNextWindowPos({0,0},ImGuiCond_Always);
    ImGui::SetNextWindowSize({140,100},ImGuiCond_Always);
    if(ImGui::Begin("##Preview test",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoInputs)){
        cache.draw(rt,version,112.f,[]{++reads;return png;});
    }
    ImGui::End();
}
static void after(Runtime *rt) {
    if(rt->is_key_pressed(VK_F5)) log("THUMB_F5_DETECTED");
    if(displayed==6 || displayed==12) {
        auto shot=qs::decode_thumbnail(qs::capture_thumbnail(rt));
        unsigned seen=0;
        for(size_t i=0;i+3<shot.rgba.size();i+=4)
            if(shot.rgba[i]>180 && shot.rgba[i+1]<90 && shot.rgba[i+2]>160) ++seen;
        char message[150]{};
        std::snprintf(message,sizeof(message),"THUMB_GPU_TEST frame=%d magenta_pixels=%u loaders=%d dimensions=%ux%u",
                           displayed,seen,reads,shot.width,shot.height);
        log(message);
    }
}
static void end(Runtime *rt) {
    if(key_events==4 && !key_failure) log("MULTIBIND_F5_C_B_AND_SIMULTANEOUS");
    else {char msg[80]; std::snprintf(msg,sizeof msg,"MULTIBIND_FAILED events=%d bad=%d",key_events,key_failure);log(msg);}
    qs::keys_stop();cache.end_runtime(rt);log("THUMB_RUNTIME_CLEAN");
}
extern "C" __declspec(dllexport) const char *NAME="DSDC graphics preview smoke";
BOOL WINAPI DllMain(HMODULE mod,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){
        DisableThreadLibraryCalls(mod);
        if(!reshade::register_addon(mod))return FALSE;
        qs::register_thumbnail_output();
        reshade::register_event<reshade::addon_event::init_effect_runtime>(start);
        reshade::register_event<reshade::addon_event::reshade_overlay>(draw);
        reshade::register_event<reshade::addon_event::reshade_present>(after);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(end);
    } else if(reason==DLL_PROCESS_DETACH) reshade::unregister_addon(mod);
    return TRUE;
}
