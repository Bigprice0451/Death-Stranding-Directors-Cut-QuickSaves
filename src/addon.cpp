#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "session.hpp"
#include "thumbnails.hpp"
#include "private_capture.hpp"
#include "save_browser.hpp"
#include "graphics_panel.hpp"
#include "turning_panel.hpp"
#include "ultrawide_panel.hpp"
#include "charging_repair_panel.hpp"
#include "quick_keys.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace {
using Runtime = reshade::api::effect_runtime;
qs::Session session;
qs::ThumbnailCache thumbnails;
HMODULE module;
HWND game_window{};
HHOOK message_hook{};
Runtime *owner{};
UINT wake_message{};
bool attached{};
std::atomic<ULONGLONG> next_wake{};

LRESULT CALLBACK game_message(int code, WPARAM remove, LPARAM arg) {
    static thread_local bool dispatching=false;
    if(code>=0 && remove==PM_REMOVE && !dispatching && session.needs_tick()) {
        auto *msg=reinterpret_cast<MSG*>(arg);
        if(msg && msg->message==wake_message && msg->hwnd==game_window) {
            dispatching=true;
            if(qs::regular_capture_frame(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) session.tick();
            dispatching=false;
            msg->message=WM_NULL;
        }
    }
    return CallNextHookEx(nullptr,code,remove,arg);
}
void runtime_start(Runtime *rt) {
    if(owner || !rt->get_hwnd()) return;
    auto window=static_cast<HWND>(rt->get_hwnd());
    DWORD pid=0; auto thread=GetWindowThreadProcessId(window,&pid);
    if(!thread || pid!=GetCurrentProcessId()) return;
    owner=rt; game_window=window;
    qs::load_browser_layout(rt);
    if(!attached) { session.attach(); attached=true; }
    else if(session.supported()) session.runtime_reopened();
    if(!session.supported()) return;
    qs::ultrawide_panel_start(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),rt->get_hwnd());
    qs::graphics_start(rt,reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    qs::turning_panel_start(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    qs::charging_repair_panel_start(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    qs::keys_start();
    bool enabled=false; int minutes=10;
    reshade::get_config_value(rt,"DirectorCutQuickSaves","AutoSaveEnabled",enabled);
    reshade::get_config_value(rt,"DirectorCutQuickSaves","AutoSaveMinutes",minutes);
    session.configure_auto(enabled,minutes);
    wake_message=RegisterWindowMessageW(L"DeathStrandingQuickSave.RegularFrame.1");
    if(wake_message) message_hook=SetWindowsHookExW(WH_GETMESSAGE,game_message,module,thread);
    if(!message_hook) session.error("Could not reach the game's update thread. Restart Director's Cut.");
}
void runtime_end(Runtime *rt) {
    if(rt!=owner) return;
    if(message_hook) UnhookWindowsHookEx(message_hook);
    if(session.supported()) {qs::keys_stop();qs::graphics_stop();qs::turning_panel_stop();qs::charging_repair_panel_stop();}
    thumbnails.end_runtime(rt);
    session.runtime_closed();
    message_hook=nullptr; game_window=nullptr; owner=nullptr;
}
void present(Runtime *rt) {
    if(rt!=owner) return;
    if(session.supported()) {qs::graphics_tick();qs::turning_panel_tick();qs::ultrawide_panel_tick(rt);qs::charging_repair_panel_tick();}
    if(!message_hook) return;
    session.expire_unhandled();
    auto now=GetTickCount64();
    auto scheduled=next_wake.load();
    if(session.needs_tick() && now>=scheduled && next_wake.compare_exchange_strong(scheduled,now+16)) {
        PostMessageW(game_window,wake_message,0,0);
    }
}
void capture_now(Runtime *rt,qs::Action action=qs::Action::save,std::uintptr_t timed_world=0) {
    const auto when=GetTickCount64();
    if(!session.allow_input(action)) return;
    // Manual intent can displace a pending timer. If another job is already
    // running, submit reports the manual press without taking a GPU screenshot.
    auto preview=session.can_take_preview(action) ? qs::capture_thumbnail(rt) : qs::Bytes{};
    if(session.submit(action,{},std::move(preview),when,timed_world))
        PostMessageW(game_window,wake_message,0,0);
}
void panel(Runtime *rt) {
    if(rt!=owner) return;
    if(session.supported()) {qs::draw_ultrawide_panel();qs::draw_graphics_panel(rt);}
    if(session.supported()) qs::draw_turning_panel();
    if(session.supported()) qs::draw_charging_repair_panel();
    qs::draw_save_browser(rt,session,thumbnails,message_hook!=nullptr,[rt]{capture_now(rt);},[rt](bool enabled,int minutes) {
        minutes=std::clamp(minutes,1,60);
        session.configure_auto(enabled,minutes);
        reshade::set_config_value(rt,"DirectorCutQuickSaves","AutoSaveEnabled",enabled);
        reshade::set_config_value(rt,"DirectorCutQuickSaves","AutoSaveMinutes",minutes);
    },[](qs::Shelf shelf,unsigned capacity,const auto &reviewed){session.resize_shelf(shelf,capacity,reviewed);});
}
void notification(Runtime *rt) {
    if(rt!=owner) return;
    // ReShade invokes this callback before it submits the overlay draw list.
    if(message_hook) {
        if(qs::quick_key_pressed(rt)) capture_now(rt);
        else if(const auto world=session.take_timed_capture()) capture_now(rt,qs::Action::timed_save,world);
    }
    auto view=session.view();
    if(!view.changed || GetTickCount64()-view.changed>5500) return;
    ImGui::SetNextWindowPos(ImVec2(22,68),ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    if(ImGui::Begin("##DSDCQuickSaveNotice",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|
        ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing))
        ImGui::TextUnformatted(view.message.c_str());
    ImGui::End();
}
}

extern "C" __declspec(dllexport) const char *NAME="Director's Cut QuickSaves";
extern "C" __declspec(dllexport) const char *DESCRIPTION="Save and resume your delivery using Quick Save and optional Auto Save.";
BOOL WINAPI DllMain(HMODULE self,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        module=self; DisableThreadLibraryCalls(self);
        if(!reshade::register_addon(self)) return FALSE;
        qs::register_thumbnail_output();
        reshade::register_overlay("QuickSaves",panel);
        reshade::register_event<reshade::addon_event::init_effect_runtime>(runtime_start);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(runtime_end);
        reshade::register_event<reshade::addon_event::reshade_present>(present);
        reshade::register_event<reshade::addon_event::reshade_overlay>(notification);
    } else if(reason==DLL_PROCESS_DETACH) {
        if(message_hook) UnhookWindowsHookEx(message_hook);
        reshade::unregister_addon(self);
    }
    return TRUE;
}
