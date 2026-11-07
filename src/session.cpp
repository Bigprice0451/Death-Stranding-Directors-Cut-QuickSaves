#include "session.hpp"
#include "private_capture.hpp"
#include "thumbnail_image.hpp"
#include <windows.h>
#include <reshade.hpp>
#include <chrono>
#include <utility>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace qs {
namespace {
// Use the same Steam interface version requested by this ds.exe. Steam's running
// session identifies the player even when older profiles exist on the same PC.
template<class T> T steam_fn(HMODULE steam, const char *name) {
    auto p=GetProcAddress(steam,name);
    if(!p) throw std::runtime_error("Steam's account interface is unavailable.");
    return reinterpret_cast<T>(p);
}
std::string current_account() {
    auto steam=GetModuleHandleW(L"steam_api64.dll");
    if(!steam) throw std::runtime_error("Steam has not connected yet.");
    auto client=steam_fn<void*(*)()>(steam,"SteamClient")();
    int user=steam_fn<int(*)()>(steam,"SteamAPI_GetHSteamUser")();
    int pipe=steam_fn<int(*)()>(steam,"SteamAPI_GetHSteamPipe")();
    if(!client || !user || !pipe) throw std::runtime_error("Steam has not connected yet.");
    auto iface=steam_fn<void*(*)(void*,int,int,const char*)>(steam,"SteamAPI_ISteamClient_GetISteamUser")(client,user,pipe,"SteamUser021");
    if(!iface) throw std::runtime_error("Steam could not identify this game's signed-in player.");
    auto id=steam_fn<uint64_t(*)(void*)>(steam,"SteamAPI_ISteamUser_GetSteamID")(iface);
    // Individual account type, nonzero universe, nonzero 32-bit account ID.
    if(!((id>>56)&255) || ((id>>52)&15)!=1 || !uint32_t(id)) throw std::runtime_error("Invalid Steam player.");
    return std::to_string(uint32_t(id));
}

uint64_t file_time() {
    FILETIME ft{}; GetSystemTimePreciseAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
}
std::string digest_text(const Digest &digest) {
    std::ostringstream s; s<<std::hex<<std::setfill('0');
    for(auto c:digest) s<<std::setw(2)<<unsigned(c);
    return s.str();
}
std::string action_name(Action a) {
    switch(a) {
        case Action::save: return "Quick Save"; case Action::timed_save: return "Auto Save"; case Action::load: return "load";
        case Action::resize: return "change retention";
        case Action::refresh: return "refresh"; default: return "startup";
    }
}
void log_event(const Event &e) {
    if(!telemetry().write(e)) reshade::log::message(reshade::log::level::error,"QuickSaves could not write its local diagnostic log.");
}
std::string describe_scenes(const SceneStamps &scenes) {
    std::ostringstream s; s<<"clocks="<<scenes.size();
    for(const auto &clock:scenes)
        s<<" ["<<clock.kind<<' '<<std::hex<<clock.high<<'-'<<clock.low<<std::dec<<" frame "<<clock.frame<<']';
    return s.str();
}
}
View Session::view() const {
    std::lock_guard lock(mutex_); auto result=view_;
    result.auto_enabled=timer_.enabled(); result.auto_minutes=timer_.minutes();
    result.auto_remaining_seconds=timer_.remaining_seconds(GetTickCount64());
    return result;
}
bool Session::needs_tick() const {
    std::lock_guard lock(mutex_);
    return view_.busy || (timer_.enabled() && GetTickCount64()>=next_probe_);
}
SaveHold Session::hold() const {
    return save_hold(game_->state_flags(),has_live_scene());
}
namespace {
const char* hold_message(SaveHold reason) {
    return reason==SaveHold::cutscene ? "Saving and loading are unavailable during a cutscene." :
        "Saving and loading are unavailable while the world is changing.";
}
}
void Session::require_gameplay() const {
    auto reason=hold();
    if(reason!=SaveHold::none) throw std::runtime_error(hold_message(reason));
}
bool Session::allow_input(Action action) {
    if (!game_) return false;
    const auto reason=hold();
    if (reason==SaveHold::none) return true;
    { std::lock_guard lock(mutex_); timer_.reset(); }
    if (action!=Action::timed_save) {
        log_event({"input_held",action_name(action)+" "+hold_message(reason)});
        status(hold_message(reason));
    }
    return false;
}
bool Session::can_take_preview(Action action) const {
    std::lock_guard lock(mutex_);
    return !view_.busy || (action==Action::save && request_ && request_->action==Action::timed_save);
}
void Session::configure_auto(bool enabled,int minutes) {
    std::lock_guard lock(mutex_);
    timer_.configure(enabled,minutes); next_probe_=0;
    // A queued timer belongs to the configuration that scheduled it. The native
    // writer, once started, is allowed to finish its atomic archive write.
    if(request_ && request_->action==Action::timed_save) {
        request_.reset(); view_.busy=false;
    }
}
std::uintptr_t Session::take_timed_capture() {
    std::lock_guard lock(mutex_);
    const auto due=timer_.take(GetTickCount64());
    return view_.busy ? 0 : due;
}
void Session::resize_shelf(Shelf shelf,unsigned capacity,const std::shared_ptr<const std::vector<Entry>>& reviewed) {
    std::lock_guard lock(mutex_);
    if(!game_ || capacity<1 || capacity>limit) return;
    if(view_.busy || !reviewed || reviewed!=(shelf==Shelf::timed?view_.auto_entries:view_.entries)) {
        view_.message="The save list changed or is busy. Review the limit again.";
        view_.changed=GetTickCount64(); return;
    }
    Entry selection; selection.shelf=shelf;
    request_=Request{Action::resize,selection,++next_request_,GetTickCount64(),file_time(),{},0,capacity,
                     reviewed};
    view_.busy=true;
}
void Session::probe_timer() {
    const auto now=GetTickCount64();
    {
        std::lock_guard lock(mutex_);
        if(!timer_.enabled() || now<next_probe_) return;
        next_probe_=now+500;
    }
    bool paused{};
    const auto world=(phase_==Phase::loading || phase_==Phase::reading || phase_==Phase::verify || phase_==Phase::scene ||
                      has_live_scene()) ? 0 : game_->active_timer_world(paused);
    std::lock_guard lock(mutex_); timer_.observe(now,world,paused);
}
bool Session::supported() const { return bool(game_); }
void Session::status(std::string message) {
    reshade::log::message(reshade::log::level::info,message.c_str());
    std::lock_guard lock(mutex_); view_.message=std::move(message); view_.changed=GetTickCount64();
}
void Session::finish(std::string message) {
    phase_=Phase::idle;
    status(std::move(message)); std::lock_guard lock(mutex_); view_.busy=false;
}
void Session::error(const std::string &message) {
    log_event({"integration_error",message}); finish(message);
}
void Session::event(std::string name,std::string detail,std::optional<WorldSample> world,std::optional<double> distance) {
    if(source_==Shelf::timed) detail="Auto Save "+detail;
    log_event({std::move(name),std::move(detail),active_,sequence_,GetTickCount64()-began_,world,distance});
}
void Session::attach() {
    try { game_=std::make_unique<Game>(); }
    catch(const std::exception &e) {
        // A compatibility smoke host should report to its local ReShade log.
        // Only a ds.exe run belongs in the player's persistent diagnostic log.
        wchar_t path[MAX_PATH]{};
        if(GetModuleFileNameW(nullptr,path,MAX_PATH) && _wcsicmp(fs::path(path).filename().c_str(),L"ds.exe")==0) error(e.what());
        else finish(e.what());
        return;
    }
    log_event({"attach","Add-on 0.2.1, private world format 1, Steam build 13300582"});
    std::lock_guard lock(mutex_);
    request_=Request{Action::start,{},++next_request_,GetTickCount64(),file_time(),{}};
}
bool Session::submit(Action action,const Entry &entry,Bytes preview,uint64_t pressed_at,std::uintptr_t timed_world) {
    if ((action==Action::save || action==Action::load || action==Action::timed_save) && !allow_input(action)) return false;
    uint64_t now=GetTickCount64(), id{}; bool busy{};
    {
        std::lock_guard lock(mutex_);
        if(!game_) return false;
        if(action==Action::timed_save && (!timer_.enabled() || !timed_world)) return false;
        if(action!=Action::timed_save && request_ && request_->action==Action::timed_save) {
            request_.reset(); view_.busy=false;
        }
        id=++next_request_; busy=view_.busy;
        if(!busy) {
            request_=Request{action,entry,id,pressed_at?pressed_at:now,file_time(),std::move(preview),timed_world};
            view_.busy=true;
            if(action==Action::load) timer_.reset();
            else if(action==Action::save) timer_.take(now); // manual wins a simultaneous deadline
        }
    }
    if(!busy || action!=Action::timed_save)
        log_event({busy?"busy_input":"requested",entry.shelf==Shelf::timed?"Auto Save "+action_name(action):action_name(action),id,entry.sequence,pressed_at?now-pressed_at:0});
    if(busy && (action==Action::save || action==Action::load)) status("Another save or load is still running. Try again when it finishes.");
    return !busy;
}
void Session::expire_unhandled() {
    std::optional<Request> expired;
    {
        std::lock_guard lock(mutex_);
        if(!request_ || request_->action==Action::start) return;
        const bool timed=request_->action==Action::timed_save;
        if(GetTickCount64()-request_->submitted<(timed?1000u:4000u)) return;
        expired=std::move(request_); request_.reset(); view_.busy=false;
        if(!timed) {
            view_.changed=GetTickCount64();
            view_.message="The game did not reach a stable update for this request. Try again.";
        }
    }
    log_event({"request_expired",action_name(expired->action),expired->id,expired->entry.sequence,GetTickCount64()-expired->submitted});
}
void Session::bind_profile() {
    auto active=current_account();
    std::lock_guard lock(mutex_);
    if(library_ && active!=account_) throw std::runtime_error("Steam account changed. Restart the game to switch quicksave libraries.");
    if(data_root().empty()) throw std::runtime_error("Windows could not locate the Saved Games or local app data folder.");
    if(!library_) {
        auto manual=std::make_shared<Library>(data_root()/L"QuickSaves",active);
        auto automatic=std::make_shared<Library>(data_root()/L"AutoSaves",active,Shelf::timed);
        account_=active; library_=std::move(manual); auto_library_=std::move(automatic);
    }
}
Bytes Session::thumbnail(const Entry &entry) {
    std::shared_ptr<Library> library;
    { std::lock_guard lock(mutex_); library=entry.shelf==Shelf::timed?auto_library_:library_; }
    if(!library) return {};
    try { return native_thumbnail(library->read(entry)); }
    catch(const std::exception &e){ log_event({"thumbnail_read_error",e.what(),0,entry.sequence});return {}; }
}
void Session::reload_entries() {
    unsigned damaged{}; auto list=std::make_shared<const std::vector<Entry>>(library_->list(&damaged));
    unsigned auto_damaged{}; auto automatic=std::make_shared<const std::vector<Entry>>(auto_library_->list(&auto_damaged));
    std::optional<unsigned> manual_cap,timed_cap;
    std::string manual_error,timed_error;
    try { manual_cap=library_->capacity(); } catch(const std::exception& e) { manual_error=e.what(); }
    try { timed_cap=auto_library_->capacity(); } catch(const std::exception& e) { timed_error=e.what(); }
    std::lock_guard lock(mutex_); view_.entries=std::move(list); view_.damaged=damaged;
    view_.auto_entries=std::move(automatic); view_.auto_damaged=auto_damaged; listed_=true;
    if(manual_cap) view_.manual_capacity=*manual_cap;
    if(timed_cap) view_.auto_capacity=*timed_cap;
    view_.manual_retention_error=std::move(manual_error); view_.auto_retention_error=std::move(timed_error);
}
void Session::handle(Request req) {
    active_=req.id; began_=req.submitted; sequence_=req.entry.sequence; last_notice_=began_;
    timed_capture_=req.action==Action::timed_save;
    source_=timed_capture_ ? Shelf::timed : req.entry.shelf;
    ensure_tracking();
    bind_profile();
    if(req.action==Action::start || req.action==Action::refresh) {
        writing_=std::async(std::launch::async,[this]{reload_entries();return std::string("Save lists ready.");});
        phase_=Phase::writing;return;
    }
    auto library=source_==Shelf::timed?auto_library_:library_;
    if(req.action==Action::resize) {
        writing_=std::async(std::launch::async,[this,library,cap=req.capacity,expected=req.expected_rows]{
            try {
                const auto removed=library->set_capacity(cap,*expected);
                event("retention_changed","limit="+std::to_string(cap)+" removed="+std::to_string(removed));
            } catch(...) {reload_entries();throw;}
            reload_entries(); return std::string("Save limit updated.");
        });
        phase_=Phase::writing;return;
    }
    require_gameplay();
    if(req.action==Action::save || timed_capture_) {
        bool paused{};
        if(timed_capture_ && (GetTickCount64()-req.submitted>1000 || req.timed_world!=game_->active_timer_world(paused) || paused))
            throw std::runtime_error("The Auto Save observation expired or its world changed.");
        PrivateRecord record; record.world=game_->sample();
        if(record.world.cinematic()) throw std::runtime_error(hold_message(SaveHold::cutscene));
        event("capture_begin","thumbnail_bytes="+std::to_string(req.preview.size()),record.world);
        require_gameplay(); // Last gate before native code sees a save request.
        record.image=capture_private_world(game_->base());
        event("capture_complete","world="+std::to_string(native_world_id(record.image))+" bytes="+std::to_string(record.image.size()),record.world);
        writing_=std::async(std::launch::async,[this,library,rec=std::move(record),png=std::move(req.preview),when=req.wall_time]() {
            auto entry=library->store(pack_record(rec,png),when);
            const bool timed=entry.shelf==Shelf::timed;
            log_event({"saved",std::string(timed?"Auto Save ":"Quick Save ")+"slot="+std::to_string(entry.slot)+" sha256="+digest_text(entry.digest)+" preview_bytes="+std::to_string(png.size()),
                       active_,entry.sequence,GetTickCount64()-began_,rec.world});
            if(!listed_) reload_entries();
            // store already scanned the ring under its process lock. Update just
            // the published row; Refresh explicitly rechecks every file on disk.
            {
                std::lock_guard lock(mutex_);
                auto &rows=timed?view_.auto_entries:view_.entries;
                auto next=std::make_shared<std::vector<Entry>>(*rows);
                next->erase(std::remove_if(next->begin(),next->end(),[&](const Entry &e){return e.slot==entry.slot;}),next->end());
                next->insert(next->begin(),entry); rows=next;
            }
            return std::string(timed?"Auto Save #":"Quicksaved #")+std::to_string(entry.sequence)+(png.empty()?" (no preview).":".");
        });
        phase_=Phase::writing; if(!timed_capture_) status("Writing private quicksave...");return;
    }
    event("read_selected","slot="+std::to_string(req.entry.slot)+" sha256="+digest_text(req.entry.digest));
    reading_=std::async(std::launch::async,[library,entry=req.entry]{return unpack_record(library->read(entry));});
    phase_=Phase::reading;status("Opening quicksave #"+std::to_string(sequence_)+"...");
}
void Session::poll() {
    using namespace std::chrono_literals;
    if(phase_==Phase::writing && writing_.wait_for(0ms)==std::future_status::ready) {
        auto result=writing_.get();
        if(timed_capture_) last_auto_write_error_.clear();
        finish(std::move(result));return;
    }
    if(phase_==Phase::reading && reading_.wait_for(0ms)==std::future_status::ready) {
        selected_=reading_.get();
        event("load_begin","world="+std::to_string(native_world_id(selected_.image))+" "+describe_scenes(selected_.scenes),selected_.world);
        require_gameplay();
        loader_.begin(game_->base(),selected_.image);
        Bytes().swap(selected_.image); last_native_phase_=-1; phase_=Phase::loading;
        status("Loading private quicksave #"+std::to_string(sequence_)+"...");return;
    }
    if(phase_==Phase::loading) {
        auto s=loader_.poll();
        if(s.native_phase!=last_native_phase_) {
            last_native_phase_=s.native_phase;event("load_phase",std::to_string(s.native_phase));
        }
        if(s.state==LoadState::interrupted) throw std::runtime_error("The world changed during quickload. Restart before loading another quicksave.");
        if(s.state!=LoadState::completed) {
            if(GetTickCount64()-last_notice_>30000) {last_notice_=GetTickCount64();event("waiting_for_world",std::to_string(s.native_phase));status("Waiting for the game to finish loading the world...");}
            return;
        }
        const auto actual=game_->sample();
        std::optional<double> distance;
        if(actual.position && selected_.world.position) {
            auto a=*actual.position,b=*selected_.world.position;
            distance=std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);
        }
        event("world_loaded","native_position_difference="+std::to_string(s.position_difference)+" world_position_override="+(s.world_position_override?"yes":"no"),actual,distance);
        world_message_="Loaded #"+std::to_string(sequence_)+".";
        if(distance && *distance>0.25) {std::ostringstream m;m<<std::fixed<<std::setprecision(2)<<" Sam is "<<*distance<<" world units from the saved position.";world_message_+=m.str();}
        // Keep the arrival position above. Inventory can still be rebuilding
        // for several frames after the native load request finishes.
        if(!selected_.scenes.empty()) resume_scenes(selected_.scenes);
        delivery_check_.start(GetTickCount64()); phase_=Phase::verify;
        event("delivery_check_pending","Waiting for the loaded world to settle.");
        return;
    }
    if(phase_==Phase::verify) {
        const auto now=GetTickCount64();
        const auto flags=game_->state_flags();
        const auto stage=delivery_check_.observe(now,flags && !(*flags & transition_bits));
        if(stage==PostLoadCheck::State::waiting) return;
        const bool ready=stage==PostLoadCheck::State::sample;
        const auto actual=ready ? game_->sample() : WorldSample{};
        if(ready && ((selected_.world.delivery && !actual.delivery) ||
                    (selected_.world.carried_count && !actual.carried_count))) return;
        event("delivery_world_sample",ready?"settled":"timed out",ready?std::optional<WorldSample>(actual):std::nullopt);
        if(actual.delivery && selected_.world.delivery) {
            const auto &saved=*selected_.world.delivery;
            const auto &restored=*actual.delivery;
            const bool same=saved.active_orders==restored.active_orders && saved.delivered_count==restored.delivered_count && saved.digest==restored.digest;
            event("order_comparison",same?"match":"mismatch",actual);
            if(!same) world_message_+=" Active orders differ from the save.";
        }
        else event("order_comparison","unavailable");
        if(actual.carried_count && selected_.world.carried_count) {
            const bool same=*actual.carried_count==*selected_.world.carried_count;
            event("cargo_count_comparison",same?"match":"mismatch",actual);
            if(!same) world_message_+=" Sam's carried cargo count differs.";
        }
        else event("cargo_count_comparison","unavailable");
        if(selected_.scenes.empty()) {
            if(selected_.world.cinematic()) {event("scene_unconfirmed","No identifiable live timeline at capture.");world_message_+=" Cutscene timing is unconfirmed.";}
            finish(world_message_);return;
        }
        phase_=Phase::scene;status(world_message_+" Checking cutscene timing...");return;
    }
    if(phase_==Phase::scene) {
        const auto r=scene_result();
        if(!r.finished) return;
        event("scene_result",std::to_string(r.matched)+"/"+std::to_string(r.total)+" clocks matched",game_->sample());
        finish(world_message_+(r.total && r.matched==r.total?" Scene frame matched.":" Cutscene timing is unconfirmed."));
    }
}
void Session::ensure_tracking() {
    std::lock_guard lock(tracking_mutex_);
    if(!tracking_attempted_) {
        tracking_attempted_=true;
        event("tracking_start","Enabling scene and movie frame observation");
        try { start_cinematic_tracking(game_->base()); tracker_started_=true; event("tracking_ready"); }
        catch(const std::exception &e){ event("scene_tracker_error",e.what()); }
    }
}
void Session::runtime_reopened() {
    // A new swap chain supplies a new update hook. A pending load is polled
    // there, while an idle session schedules a refresh to resume observing F5.
    if(!view().busy) submit(Action::refresh);
}
void Session::runtime_closed() {
    { std::lock_guard lock(mutex_); timer_.reset(); next_probe_=0; }
    std::lock_guard lock(tracking_mutex_);
    if(tracker_started_) { stop_cinematic_tracking(); tracker_started_=false; }
    tracking_attempted_=false;
}
void Session::tick() {
    try {
        probe_timer();
        std::optional<Request> next;
        { std::lock_guard lock(mutex_); next=std::exchange(request_,std::nullopt); }
        if(next) handle(std::move(*next));
        else { ensure_tracking(); poll(); }
    } catch(const std::exception &e) {
        event(timed_capture_?"auto_save_skipped":"request_failed",e.what()); selected_=PrivateRecord{};
        if(timed_capture_) {
            if(phase_==Phase::writing && last_auto_write_error_!=e.what()) {
                last_auto_write_error_=e.what();
                status("Auto Save could not write the file. "+last_auto_write_error_);
            }
            phase_=Phase::idle;
            std::lock_guard lock(mutex_); view_.busy=false; timer_.reset();
        } else finish(e.what());
    }
}
}
