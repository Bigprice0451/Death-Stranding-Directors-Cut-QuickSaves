// Run the shipping panel against real ImGui input on a simulated 3440 x 1440 display.
// The small session and thumbnail doubles keep it separate from a running game.
#include <imgui.h>
#include <imgui_internal.h>
#include "save_browser.hpp"
#include "thumbnails.hpp"
#include "session.hpp"
#include "quick_keys.hpp"
#include <map>
#include <cstdio>
#include <stdexcept>
namespace {
qs::View view;
std::map<unsigned,ImRect> bounds;
std::map<std::string,std::vector<ImVec2>> locations;
int saves=0,refreshes=0,loads=0,edits=0,last_minutes=0; bool last_auto=false;
qs::Shelf loaded{},edited{};
unsigned kept=0;
qs::Session session;
qs::ThumbnailCache cache;
}
namespace qs {
View Session::view() const {return ::view;}
bool Session::submit(Action a,const Entry &e,Bytes,uint64_t,uintptr_t) {
    if(a==Action::load) { ++loads;loaded=e.shelf; }
    if(a==Action::refresh) ++refreshes;
    return true;
}
Bytes Session::thumbnail(const Entry &) {return {};}
PrivateLoad::~PrivateLoad()=default;
struct ThumbnailCache::State {};
ThumbnailCache::ThumbnailCache()=default;ThumbnailCache::~ThumbnailCache()=default;
void ThumbnailCache::draw(reshade::api::effect_runtime*,const Entry&,float width,const std::function<Bytes()>&) {ImGui::Dummy({width,width*9/16});}
void draw_quick_keys(reshade::api::effect_runtime *) {if(ImGui::TreeNode("Quick Save keys"))ImGui::TreePop();}
std::string quick_keys_label(){return "F5";}
Telemetry::Telemetry(fs::path folder) :folder_(std::move(folder)) {}
Telemetry &telemetry(){static Telemetry instance({});return instance;}
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*,ImGuiID id,const ImRect &rect,const ImGuiLastItemData*) {bounds[id]=rect;}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*,ImGuiID id,const char*label,ImGuiItemStatusFlags) {
    if(bounds.count(id)) locations[label].push_back(bounds[id].GetCenter());
}
void ImGuiTestEngineHook_Log(ImGuiContext*,const char*,...){}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*,ImGuiID){return "";}
static void ensure(bool condition,const char *msg){if(!condition)throw std::runtime_error(msg);}
static float panel_width=1610;
static void frame() {
    bounds.clear();locations.clear();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({10,10});ImGui::SetNextWindowSize({panel_width,1290});
    ImGui::Begin("QuickSaves",nullptr,ImGuiWindowFlags_NoSavedSettings);
    qs::draw_save_browser(nullptr,session,cache,true,[]{++saves;},
        [](bool enabled,int m){last_auto=enabled;last_minutes=m;view.auto_enabled=enabled;view.auto_minutes=m;},
        [](qs::Shelf shelf,unsigned keep,const auto &snapshot){
            ensure(snapshot==(shelf==qs::Shelf::manual?view.entries:view.auto_entries),"Wrong approved identity");
            ++edits;edited=shelf;kept=keep;
            (shelf==qs::Shelf::manual?view.manual_capacity:view.auto_capacity)=keep;
        });
    ImGui::End();ImGui::Render();
}
static void click(const char *label,unsigned n=0) {
    if(locations[label].size()<=n) {
        for(auto &l:locations) std::printf("[%s %zu]",l.first.c_str(),l.second.size());std::puts("");
        throw std::runtime_error(std::string("Cannot find widget ")+label);
    }
    auto p=locations.at(label)[n];
    auto &io=ImGui::GetIO();
    io.AddMousePosEvent(p.x,p.y);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();frame();
}
int main() {
    ImGui::CreateContext()->TestEngineHookItems=true;
    auto &io=ImGui::GetIO(); io.DisplaySize={3440,1440};io.IniFilename=nullptr;io.DeltaTime=.017f;
    unsigned char *pix;int w,h;io.Fonts->GetTexDataAsRGBA32(&pix,&w,&h);
    view.manual_capacity=3;view.auto_capacity=3; view.busy=false;view.auto_enabled=false;view.auto_minutes=10;
    for(auto shelf:{qs::Shelf::manual,qs::Shelf::timed}) {
        auto rows=std::make_shared<std::vector<qs::Entry>>();
        for(unsigned i=1;i<=2;i++) {
            qs::Entry e{};e.shelf=shelf;e.account=123;e.slot=i;e.sequence=i;e.info={"Delivery quicksave","World 1"};rows->push_back(e);
        }
        (shelf==qs::Shelf::manual?view.entries:view.auto_entries)=rows;
    }
    try {
        frame();frame();frame();
        auto grips=locations.at("##resize-height");
        ensure(grips.size()==2 && grips[1].x-grips[0].x>550,"Wide layout failed");
        auto *table=ImGui::GetCurrentContext()->Tables.GetByIndex(0);
        auto divider=ImVec2(table->Columns[0].MaxX,table->OuterRect.Min.y+5);
        auto &input=ImGui::GetIO();
        input.AddMousePosEvent(divider.x,divider.y);frame();frame();
        input.AddMouseButtonEvent(0,true);frame();input.AddMousePosEvent(divider.x+100,divider.y);frame();frame();
        input.AddMouseButtonEvent(0,false);frame();frame();
        ensure(table->Columns[0].MaxX-divider.x>90,"Center divider did not resize");
        grips=locations.at("##resize-height");
        auto drag=[&](unsigned index,float dy) {
            auto p=locations.at("##resize-height")[index];auto &io=ImGui::GetIO();
            io.AddMousePosEvent(p.x,p.y);frame();io.AddMouseButtonEvent(0,true);frame();
            io.AddMousePosEvent(p.x,p.y+dy);frame();frame();io.AddMouseButtonEvent(0,false);frame();frame();
        };
        drag(0,90);
        auto after=locations.at("##resize-height");
        ensure(after[0].y-grips[0].y>85 && std::abs(after[1].y-grips[1].y)<2,"Manual resize moved wrong shelf");
        drag(1,-72);
        auto next_grips=locations.at("##resize-height");
        ensure(after[1].y-next_grips[1].y>68 && std::abs(after[0].y-next_grips[0].y)<2,"Auto resize moved wrong shelf");
        panel_width=485; frame();frame();
        auto quick=locations.at("Quick Saves").at(0), timed=locations.at("Auto Save").at(0);
        ensure(std::abs(quick.x-timed.x)<3 && timed.y>quick.y+100,"Dock failed to stack");
        panel_width=1610;frame();frame();
        std::puts("PASS 3440x1440 wide layout, independent manual and auto drags, narrow stacked layout.");
        click("Quick Save");click("Refresh");ensure(saves==1&&refreshes==1,"Save and refresh failed");
        click("Enable Auto Save");ensure(last_auto && last_minutes==10,"Enable timer failed");
        click("+",1);ensure(last_minutes==11,"Interval plus did not persist");
        click("##load",2);ensure(locations.count("Cancel"),"Timed row did not open confirmation");
        click("Load");ensure(loads==1 && loaded==qs::Shelf::timed,"Wrong shelf or load failed");
        click("##load",0);auto next=std::make_shared<std::vector<qs::Entry>>(*view.entries);(*next)[0].sequence=999;view.entries=next;
        frame();click("Load");ensure(loads==1,"Stale selection loaded");click("Cancel");
        click("-",0);click("-",0);click("Apply",0);
        ensure(edits==0 && locations.count("Change limit"),"Shrinking approved without confirmation");
        view.entries=std::make_shared<const std::vector<qs::Entry>>(*view.entries);frame();frame();frame();
        ensure(locations.count("Review count") && !locations.count("Change limit"),"Replacement failed to trigger review");
        click("Review count");click("Change limit");
        ensure(edits==1 && kept==1 && edited==qs::Shelf::manual,"Confirmed retention callback incorrect");
        view.auto_retention_error="Unreadable retention settings";frame();
        click("-",2);click("Apply",1);ensure(edits==1,"Damaged retention settings remained editable");
        std::puts("Passed: manual capture, refresh, timed toggle, interval +1, same-slot timed load, stale manual load, count reduction with renewed approval, disabled damaged retention.");
    } catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
    ImGui::DestroyContext();
}
