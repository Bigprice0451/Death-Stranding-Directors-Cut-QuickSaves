#include "quick_keys.hpp"
#include "key_bindings.hpp"
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <array>
#include <cstring>

namespace qs {
namespace {
KeyBindings bindings;
enum class Edit {idle,adding,replacing};
Edit edit=Edit::idle;
size_t selected{};
int began=-1,consumed=-1,last_draw=-1,triggered=-1;
unsigned overlay_key=VK_HOME;
std::string feedback;

std::string key_name(unsigned k) {
    if(k>='0' && k<='9' || k>='A' && k<='Z') return std::string(1,static_cast<char>(k));
    if(k>=VK_F1 && k<=VK_F24) return "F"+std::to_string(k-VK_F1+1);
    auto scan=MapVirtualKeyExW(k,MAPVK_VK_TO_VSC_EX,GetKeyboardLayout(0));
    LONG packed=static_cast<LONG>((scan&255)<<16);
    if((scan&0xff00)==0xe000 || (k>=VK_PRIOR && k<=VK_DOWN) || k==VK_INSERT || k==VK_DELETE || k==VK_DIVIDE)
        packed|=1<<24;
    wchar_t word[64]{};
    const int count=GetKeyNameTextW(packed,word,64);
    if(!count) return "Key "+std::to_string(k);
    char name[256]{};
    const auto n=WideCharToMultiByte(CP_UTF8,0,word,count,name,sizeof(name),nullptr,nullptr);
    return n>0?std::string(name,n):"Key "+std::to_string(k);
}
void save_keys() {
    auto encoded=bindings.encode();
    reshade::set_config_value(nullptr,"DirectorCutQuickSaves","QuickSaveKeys",encoded.c_str());
}
void start_edit(Edit state,size_t index=0) {
    edit=state;selected=index;began=ImGui::GetFrameCount();feedback.clear();
}
}
void keys_start() {
    char config[1024]{};size_t size=sizeof(config);
    bindings=reshade::get_config_value(nullptr,"DirectorCutQuickSaves","QuickSaveKeys",config,&size)
        ?KeyBindings::decode(config):KeyBindings{};
    // First integer in ReShade's shortcut is the virtual key. Preserve the
    // menu shortcut regardless of keyboard layout or the user's remapping.
    char shortcut[128]{};size=sizeof(shortcut);overlay_key=VK_HOME;
    if(reshade::get_config_value(nullptr,"INPUT","KeyOverlay",shortcut,&size)) {
        unsigned k{};auto end=shortcut+strnlen_s(shortcut,sizeof shortcut);
        if(std::from_chars(shortcut,end,k).ec==std::errc{} && k<=255) overlay_key=k;
    }
    edit=Edit::idle;feedback.clear();triggered=consumed=last_draw=began=-1;
}
void keys_stop() {edit=Edit::idle;feedback.clear();}
std::string quick_keys_label() {
    std::string result;
    for(auto k:bindings.keys){if(!result.empty())result+=" / ";result+=key_name(k);}
    return result.empty()?"Unbound":result;
}
bool quick_key_pressed(reshade::api::effect_runtime *rt) {
    int frame=ImGui::GetFrameCount();
    if(edit!=Edit::idle && frame-last_draw>1) edit=Edit::idle;
    if(edit!=Edit::idle || frame==consumed || frame==triggered || ImGui::GetIO().WantTextInput ||
       ImGui::GetIO().WantCaptureKeyboard || ImGui::IsAnyItemActive()) return false;
    if(!bindings.pressed([&](unsigned k){return k!=overlay_key && rt->is_key_pressed(k);})) return false;
    triggered=frame;return true;
}
void draw_quick_keys(reshade::api::effect_runtime *rt) {
    int frame=ImGui::GetFrameCount();last_draw=frame;
    if(!ImGui::TreeNode("Quick Save keys")) {edit=Edit::idle;return;}
    if(edit!=Edit::idle) {
        ImGui::TextWrapped("Press one key. Esc cancels.");
        if(frame!=began) {
            auto k=rt->last_key_pressed();
            if(k==VK_ESCAPE) {edit=Edit::idle;consumed=frame;}
            else if(k && k!=VK_LBUTTON && k!=VK_RBUTTON && k!=VK_MBUTTON) {
                consumed=frame;
                if(k==overlay_key || !bindable_key(k)) feedback="That key is reserved. Choose another.";
                else if(!bindings.assign(edit==Edit::replacing?std::optional<size_t>(selected):std::nullopt,k))
                    feedback="Already assigned. Choose another key.";
                else {save_keys();feedback.clear();edit=Edit::idle;}
            }
        }
        if(edit!=Edit::idle && ImGui::SmallButton("Cancel assignment")) {edit=Edit::idle;consumed=frame;}
    } else {
        ImGui::TextDisabled("Each key saves on its own.");
        for(size_t i=0;i<bindings.keys.size();++i) {
            ImGui::PushID(static_cast<int>(i));auto label=key_name(bindings.keys[i]);
            if(ImGui::Button(label.c_str(),ImVec2(100,0))) start_edit(Edit::replacing,i);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Replace this key");
            ImGui::SameLine();
            if(ImGui::SmallButton("Remove")) {
                bindings.keys.erase(bindings.keys.begin()+i);save_keys();feedback.clear();ImGui::PopID();break;
            }
            ImGui::PopID();
        }
        if(ImGui::Button("Add key")) start_edit(Edit::adding);
        ImGui::SameLine();
        if(ImGui::SmallButton("Reset to F5")) {bindings=KeyBindings{};save_keys();feedback.clear();}
    }
    if(!feedback.empty()) ImGui::TextWrapped("%s",feedback.c_str());
    ImGui::TreePop();
}
}
