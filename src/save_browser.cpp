#include "save_browser.hpp"
#include "session.hpp"
#include "thumbnails.hpp"
#include "telemetry.hpp"
#include "quick_keys.hpp"
#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <algorithm>
#include <cstdio>
#include <optional>

namespace qs {
namespace {
using Runtime = reshade::api::effect_runtime;

const char *shelf_name(Shelf shelf) { return shelf==Shelf::manual ? "Quick Saves" : "Auto Save"; }
const std::vector<Entry> &entries(const View &view,Shelf shelf) {
    return *(shelf==Shelf::manual ? view.entries : view.auto_entries);
}
unsigned capacity(const View &view,Shelf shelf) {
    return shelf==Shelf::manual ? view.manual_capacity : view.auto_capacity;
}
unsigned damaged(const View &view,Shelf shelf) {
    return shelf==Shelf::manual ? view.damaged : view.auto_damaged;
}
const std::string &retention_error(const View &view,Shelf shelf) {
    return shelf==Shelf::manual ? view.manual_retention_error : view.auto_retention_error;
}
struct Draft {
    int value{},last_setting{};
    void sync(int setting) {
        if(last_setting!=setting) value=last_setting=setting;
    }
};
struct CapacityChange { Shelf shelf; unsigned keep; };

std::string title(const Entry &entry) {
    // Older records use the same description for both libraries.
    std::string text=entry.info.title;
    if(text=="Delivery quicksave") text="Delivery";
    if(text=="Cutscene quicksave") text="Cutscene";
    return text+" #"+std::to_string(entry.sequence);
}
std::string card_detail(const Entry &entry) {
    // Current records place coordinates before the delivery counts. Give the
    // counts room in a narrow card. Hover and the load dialog show the full text.
    const auto &detail=entry.info.location;
    auto from=detail.find("Active orders: ");
    if(from==std::string::npos) from=detail.find("Carried cargo: ");
    return from==std::string::npos ? detail : detail.substr(from);
}
std::string local_time(uint64_t timestamp) {
    FILETIME utc{static_cast<DWORD>(timestamp),static_cast<DWORD>(timestamp>>32)};
    SYSTEMTIME system{},local{};
    if(!timestamp || !FileTimeToSystemTime(&utc,&system) || !SystemTimeToTzSpecificLocalTime(nullptr,&system,&local))
        return "Time unavailable";
    char out[32];
    std::snprintf(out,sizeof(out),"%04u-%02u-%02u  %02u:%02u",local.wYear,local.wMonth,local.wDay,local.wHour,local.wMinute);
    return out;
}
bool header(Shelf shelf,const View &view) {
    const bool open=ImGui::CollapsingHeader(shelf_name(shelf),ImGuiTreeNodeFlags_DefaultOpen);
    char count[32];
    std::snprintf(count,sizeof(count),"%zu / %u",entries(view,shelf).size(),capacity(view,shelf));
    const auto left=ImGui::GetItemRectMin(),right=ImGui::GetItemRectMax();
    const float x=right.x-ImGui::GetStyle().FramePadding.x-ImGui::CalcTextSize(count).x;
    if(x>left.x+ImGui::GetFontSize()*2+ImGui::CalcTextSize(shelf_name(shelf)).x)
        ImGui::GetWindowDrawList()->AddText({x,(left.y+right.y-ImGui::GetTextLineHeight())*.5f},
                                           ImGui::GetColorU32(ImGuiCol_Text),count);
    return open;
}
void text_line(ImVec2 pos,float width,const std::string &text,ImGuiCol color) {
    if(width<=0 || text.empty()) return;
    auto *draw=ImGui::GetWindowDrawList();
    const ImVec4 clip{pos.x,pos.y,pos.x+width,pos.y+ImGui::GetTextLineHeight()};
    const char *begin=text.data(),*end=begin+text.size();
    const ImU32 ink=ImGui::GetColorU32(color);
    if(ImGui::CalcTextSize(begin,end).x<=width) {
        draw->AddText(pos,ink,begin,end);
        return;
    }
    // Keep whole UTF-8 characters when shortening the line in a narrow dock.
    const float dots=ImGui::CalcTextSize("...").x;
    if(dots>=width) return;
    float used=0;
    const char *tail=begin;
    while(tail<end) {
        const char *next=tail+1;
        while(next<end && (static_cast<unsigned char>(*next)&0xC0)==0x80) ++next;
        const float glyph=ImGui::CalcTextSize(tail,next).x;
        if(used+glyph>width-dots) break;
        used+=glyph; tail=next;
    }
    std::string line(begin,tail);
    line+="...";
    draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),pos,ink,line.c_str(),nullptr,0,&clip);
}
void cards(Runtime *runtime,Session &session,ThumbnailCache &cache,const View &view,Shelf shelf,
           std::optional<Entry> &clicked) {
    const auto &items=entries(view,shelf);
    if(damaged(view,shelf))
        ImGui::TextWrapped("%u unreadable %s",damaged(view,shelf),damaged(view,shelf)==1?"save":"saves");
    if(items.empty()) {
        ImGui::TextDisabled(shelf==Shelf::manual ? "No Quick Saves yet." : "No Auto Saves yet.");
        return;
    }
    const auto &style=ImGui::GetStyle();
    const float line=ImGui::GetTextLineHeight(),gap=style.ItemSpacing.y;
    const float row_height=3*line+2*style.FramePadding.y;
    // Cap each list's height so opening a full manual library leaves the timer reachable.
    const float rows=std::min(2.5f,static_cast<float>(items.size()));
    const float height=rows*(row_height+gap)-gap;
    if(ImGui::BeginChild("rows",{0,height})) {
        for(const Entry &entry:items) {
            ImGui::PushID(static_cast<int>(entry.slot));
            const ImVec2 at=ImGui::GetCursorScreenPos();
            const float width=ImGui::GetContentRegionAvail().x;
            if(ImGui::Selectable("##load",false,view.busy ? ImGuiSelectableFlags_Disabled : 0,{width,row_height}))
                clicked=entry;
            if(ImGui::IsItemVisible()) {
                if(ImGui::BeginItemTooltip()) {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize()*25);
                    ImGui::TextUnformatted(title(entry).c_str());
                    ImGui::TextUnformatted(entry.info.location.c_str());
                    ImGui::TextUnformatted(local_time(entry.saved_at).c_str());
                    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
                }
                const ImVec2 next=ImGui::GetCursorScreenPos();
                const float preview=std::min(row_height*16/9,std::max(0.f,width*.30f));
                ImGui::SetCursorScreenPos({at.x,at.y+(row_height-preview*9/16)*.5f});
                cache.draw(runtime,entry,preview,[&]{return session.thumbnail(entry);});
                // Finish the card with a real layout item. Dear ImGui requires
                // this after moving the cursor back out of the preview.
                ImGui::SetCursorScreenPos({next.x,next.y-gap});
                ImGui::Dummy({0,0});
                const float x=at.x+preview+style.ItemSpacing.x;
                const float right=at.x+width-style.FramePadding.x;
                const float action=ImGui::CalcTextSize("Load").x;
                const float top=at.y+(row_height-3*line)*.5f;
                text_line({x,top},right-x-action-style.ItemSpacing.x,title(entry),ImGuiCol_Text);
                text_line({x,top+line},right-x,local_time(entry.saved_at),ImGuiCol_Text);
                text_line({x,top+2*line},right-x,card_detail(entry),ImGuiCol_TextDisabled);
                ImGui::GetWindowDrawList()->AddText({right-action,top},ImGui::GetColorU32(
                    view.busy?ImGuiCol_TextDisabled:ImGuiCol_Text),"Load");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}
void auto_settings(const View &view,Draft &interval,const std::function<void(bool,int)> &configure) {
    bool enabled=view.auto_enabled;
    if(ImGui::Checkbox("Enable Auto Save",&enabled)) configure(enabled,view.auto_minutes);
    interval.sync(view.auto_minutes);
    const auto &style=ImGui::GetStyle();
    const float field=ImGui::GetFontSize()*6;
    const float group_width=ImGui::CalcTextSize("Every").x+ImGui::CalcTextSize("min").x+2*style.ItemSpacing.x+field;
    if(ImGui::GetContentRegionAvail().x>ImGui::GetItemRectSize().x+group_width+2*style.ItemSpacing.x)
        ImGui::SameLine();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Every"); ImGui::SameLine();
    ImGui::SetNextItemWidth(field);
    ImGui::InputInt("##minutes",&interval.value,1,5);
    if(ImGui::IsItemDeactivatedAfterEdit()) {
        interval.value=std::clamp(interval.value,1,60);
        if(interval.value!=view.auto_minutes) configure(enabled,interval.value);
    }
    ImGui::SameLine(); ImGui::TextUnformatted("min");
    if(!enabled) return;
    if(view.auto_remaining_seconds<0) {
        ImGui::TextDisabled("Timer paused until a delivery is open.");
        return;
    }
    const int left=view.auto_remaining_seconds;
    if(left) ImGui::Text("Next save in %d:%02d",left/60,left%60);
    else ImGui::TextUnformatted("Next save is due");
    const float fraction=1.f-std::clamp(static_cast<float>(left)/(60*std::max(1,view.auto_minutes)),0.f,1.f);
    ImGui::ProgressBar(fraction,{-1,std::max(3.f,ImGui::GetFontSize()*.25f)},"");
}
void retention(const View &view,Shelf shelf,Draft &draft,std::optional<CapacityChange> &change) {
    draft.sync(static_cast<int>(capacity(view,shelf)));
    const auto &error=retention_error(view,shelf);
    ImGui::BeginDisabled(!error.empty());
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Keep last"); ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize()*6);
    ImGui::InputInt("##keep",&draft.value,1,10);
    if(ImGui::IsItemDeactivatedAfterEdit()) draft.value=std::clamp(draft.value,1,100);
    ImGui::SameLine();
    const int keep=std::clamp(draft.value,1,100);
    ImGui::BeginDisabled(view.busy || keep==static_cast<int>(capacity(view,shelf)));
    if(ImGui::Button("Apply")) {draft.value=keep; change=CapacityChange{shelf,static_cast<unsigned>(keep)};}
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if(!error.empty()) ImGui::TextWrapped("%s",error.c_str());
}
void dialog_width() {
    const float max_width=std::max(150.f,ImGui::GetIO().DisplaySize.x-32.f);
    ImGui::SetNextWindowSize({std::min(ImGui::GetFontSize()*27,max_width),0},ImGuiCond_Always);
}
bool still_current(const View &view,const Entry &selected) {
    const auto &shelf=entries(view,selected.shelf);
    return std::any_of(shelf.begin(),shelf.end(),[&](const Entry &e) {
        return e.slot==selected.slot && e.sequence==selected.sequence && e.account==selected.account && e.digest==selected.digest;
    });
}
}

void draw_save_browser(Runtime *runtime,Session &session,ThumbnailCache &thumbnails,bool can_save,
                       const std::function<void()> &capture_manual,const std::function<void(bool,int)> &configure_auto,
                       const SetSaveCapacity &configure_capacity) {
    // Only UI drafts and the identity selected for confirmation live across
    // frames. Session owns all saved settings, archive content and game state.
    static Draft manual_draft,auto_draft,interval;
    static Entry selected;
    static CapacityChange pending{};
    static std::shared_ptr<const std::vector<Entry>> reviewed;
    const View view=session.view();
    std::optional<Entry> clicked;
    std::optional<CapacityChange> change;
    ImGui::Spacing();
    for(Shelf shelf:{Shelf::manual,Shelf::timed}) {
        ImGui::PushID(static_cast<int>(shelf));
        if(header(shelf,view)) {
            if(shelf==Shelf::manual) {
                ImGui::BeginDisabled(!can_save || view.busy);
                if(ImGui::Button("Quick Save")) capture_manual();
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Quick Save keys: %s",quick_keys_label().c_str());
                ImGui::EndDisabled();
                ImGui::SameLine(); ImGui::BeginDisabled(view.busy);
                if(ImGui::Button("Refresh")) session.submit(Action::refresh);
                ImGui::EndDisabled();
                draw_quick_keys(runtime);
            } else auto_settings(view,interval,configure_auto);
            retention(view,shelf,shelf==Shelf::manual?manual_draft:auto_draft,change);
            cards(runtime,session,thumbnails,view,shelf,clicked);
        }
        ImGui::Spacing(); ImGui::PopID();
    }
    if(!view.message.empty()) ImGui::TextWrapped("%s",view.message.c_str());
    if(ImGui::TreeNode("Diagnostics")) {
        const auto path=telemetry().current().u8string();
        ImGui::TextWrapped("%s",path.empty() ? "Log unavailable" : path.c_str());
        ImGui::TreePop();
    }

    if(clicked) {selected=*clicked; ImGui::OpenPopup("Load this save?");}
    if(ImGui::IsPopupOpen("Load this save?")) dialog_width();
    if(ImGui::BeginPopupModal("Load this save?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool exists=still_current(view,selected);
        ImGui::TextUnformatted(shelf_name(selected.shelf));
        ImGui::TextWrapped("%s",title(selected).c_str());
        ImGui::TextUnformatted(local_time(selected.saved_at).c_str());
        ImGui::TextWrapped("%s",selected.info.location.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("%s",exists ? "Loading replaces your current unsaved progress." : "This save is no longer in the library.");
        ImGui::BeginDisabled(!exists || view.busy);
        if(ImGui::Button("Load") && session.submit(Action::load,selected)) {
            ImGui::CloseCurrentPopup();
            if(runtime) runtime->open_overlay(false,reshade::api::input_source::mouse);
        }
        ImGui::EndDisabled(); ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(change) {
        if(change->keep>=entries(view,change->shelf).size())
            configure_capacity(change->shelf,change->keep,change->shelf==Shelf::manual ? view.entries : view.auto_entries);
        else {
            pending=*change;
            reviewed=pending.shelf==Shelf::manual ? view.entries : view.auto_entries;
            ImGui::OpenPopup("Change save limit?");
        }
    }
    if(ImGui::IsPopupOpen("Change save limit?")) dialog_width();
    if(ImGui::BeginPopupModal("Change save limit?",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto &current=pending.shelf==Shelf::manual ? view.entries : view.auto_entries;
        const size_t count=current->size();
        const size_t remove=count>pending.keep ? count-pending.keep : 0;
        ImGui::TextUnformatted(shelf_name(pending.shelf));
        ImGui::TextWrapped("Keep the last %u. This removes %zu oldest %s.",pending.keep,remove,remove==1?"save":"saves");
        if(reviewed!=current)
            ImGui::TextWrapped("The library changed. Review the new count before applying.");
        ImGui::BeginDisabled(view.busy || !retention_error(view,pending.shelf).empty());
        if(reviewed!=current) {
            if(ImGui::Button("Review count")) reviewed=current;
        } else if(ImGui::Button("Change limit")) {
            configure_capacity(pending.shelf,pending.keep,reviewed);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled(); ImGui::SameLine();
        if(ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
}
