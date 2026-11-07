#include "cinematic.hpp"
#include "cinematic_match.hpp"
#include "telemetry.hpp"
#include "library.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace qs {
namespace {
using namespace cinematic_detail;
using Address=uintptr_t;
using EntityTick=void(*)(void*,void*);
using MovieTick=bool(*)(void*,void*,void*);
using Jump=void(*)(void*,int32_t,float,uint32_t);
using MovieSeek=void(*)(void*,uint32_t,bool);
using Resolve=void*(*)(void*);

constexpr Address sequence_tick_rva=0x24abba0, jump_rva=0x24a7e20, resolve_rva=0x1dde990;
constexpr Address movie_tick_rva=0x19c9e00, movie_seek_rva=0x19cb270, world_flags_rva=0x2dd36c0;
// The global address is read by the game's own world-state getter. ReadProcessMemory
// lets an observation on a movie/render thread fail safely during world teardown.
constexpr Address world_owner_rva=0x7bf6e88;
constexpr uint64_t fresh_ms=150;

EntityTick sequence_original=nullptr;
MovieTick movie_original=nullptr;
Address module_base{};
DWORD game_thread{};
std::atomic<bool> enabled{};
std::atomic<bool> awaiting{};
bool installed{};
// Serialize lifecycle changes and live object use. Render reads only the copied clocks.
std::recursive_mutex callback_gate;
thread_local bool observing=false;
std::mutex mutex;
Match matcher;
struct Recent {SceneStamp clock{};Address owner{};uint64_t time{};bool ambiguous{};};
std::array<Recent,scene_limit> recent{};

template<typename T> T field(const void *object,size_t offset){
    T value{};std::memcpy(&value,static_cast<const uint8_t*>(object)+offset,sizeof value);return value;
}
template<typename T> T proc(Address rva){return reinterpret_cast<T>(module_base+rva);}
bool in_cutscene() {
    WorldSample sample{};
    if(GetCurrentThreadId()==game_thread) sample.flags=proc<uint64_t(*)()>(world_flags_rva)();
    else {
        Address owner=0;SIZE_T bytes=0;
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(module_base+world_owner_rva),&owner,sizeof owner,&bytes)
            || bytes!=sizeof owner || !owner || !ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(owner+8),
                &sample.flags,sizeof sample.flags,&bytes) || bytes!=sizeof sample.flags) return false;
    }
    return sample.cinematic();
}
struct Id {uint64_t low{},high{};};
struct SequenceKey {Id entity,resource;};
Id fingerprint(const uint8_t *data,size_t n) {
    const auto hash=sha256(Bytes(data,data+n));
    Id key;std::memcpy(&key,hash.data(),sizeof key);return key;
}
SceneStamp sequence_clock(void *s,void *resource,int32_t frame) {
    const SequenceKey key{field<Id>(s,8),field<Id>(resource,8)};
    if(!(key.entity.low||key.entity.high) || !(key.resource.low||key.resource.high)) return {};
    // One entity can play different authored resources. Include both UUIDs and
    // never depend on the next process assigning either the same memory address.
    thread_local SequenceKey previous{};
    thread_local Id cached{};
    if(std::memcmp(&previous,&key,sizeof key)!=0) {
        cached=fingerprint(reinterpret_cast<const uint8_t*>(&key),sizeof key);
        previous=key;
    }
    return {cached.low,cached.high,frame,engine_sequence};
}
struct MovieKey {uint32_t width,height,frames,last;};
SceneStamp movie_clock(const char *filename,const MovieKey &info,int32_t frame) {
    // Filename is the same UTF-8 string passed to BinkOpen. A different language
    // file, shorter edit or different resolution must not receive this seek.
    constexpr size_t max_path=4096;
    const size_t length=strnlen_s(filename,max_path);
    if(!length || length==max_path) return {};
    thread_local std::string previous;
    thread_local MovieKey previous_info{};
    thread_local Id cached{};
    if(previous.size()!=length || std::memcmp(filename,previous.data(),length)!=0 ||
       std::memcmp(&previous_info,&info,sizeof info)!=0) {
        Bytes input(length+sizeof info);std::memcpy(input.data(),filename,length);
        std::memcpy(input.data()+length,&info,sizeof info);
        cached=fingerprint(input.data(),input.size());
        previous.assign(filename,length);previous_info=info;
    }
    return {cached.low,cached.high,frame,bink_movie};
}
std::optional<int32_t> observe(const SceneStamp &s,Address owner,int32_t end,bool can_seek) {
    if(!s.kind || s.frame<0 || s.frame>end) return {};
    const auto now=GetTickCount64();std::lock_guard lock(mutex);
    Recent *entry=nullptr;
    for(auto &r:recent) {
        if(r.clock.kind && same_scene(r.clock,s) && now-r.time<=fresh_ms){entry=&r;break;}
        if(!entry && (!r.clock.kind||now-r.time>fresh_ms)) entry=&r;
    }
    if(!entry) return {};
    const bool prior=entry->clock.kind && now-entry->time<=fresh_ms && same_scene(entry->clock,s);
    const bool ambiguous=prior && (entry->ambiguous || entry->owner!=owner);
    *entry={s,owner,now,ambiguous};
    // Require a second sample from this same owner after arming. That also
    // gives any simultaneously active copies of one movie time to identify themselves.
    if(!can_seek || !prior || ambiguous || !awaiting.load(std::memory_order_relaxed)) return {};
    return matcher.plan(s,end,now);
}
void confirm_request(const SceneStamp &s,bool hit) {
    std::lock_guard lock(mutex);matcher.issued(s,hit);
}
void observe_sequence(void *s) {
    if(!s || !field<uint8_t>(s,0x341) || field<void*>(s,0x450) || !field<void*>(s,0x58) || !in_cutscene()) return;
    void *resource=proc<Resolve>(resolve_rva)(static_cast<uint8_t*>(s)+0x58);
    if(!resource || !field<uint8_t>(resource,0x1f5)) return;
    const auto frame=field<int32_t>(s,0x378),end=field<int32_t>(resource,0x158);
    const auto clock=sequence_clock(s,resource,frame);
    const auto target=observe(clock,reinterpret_cast<Address>(s),end,GetCurrentThreadId()==game_thread);
    if(!target) return;
    // Mode zero is the authored JumpEvent operation. Its implementation detects
    // backwards jumps itself. Mode one is the distinct return-label operation.
    proc<Jump>(jump_rva)(s,*target,0.f,0);
    confirm_request(clock,field<int32_t>(s,0x378)==*target && field<int32_t>(s,0x37c)==*target);
}
void observe_movie(void *player) {
    if(!player || !field<uint8_t>(player,0x2ab) || field<uint8_t>(player,0x25c) ||
       field<uint8_t>(player,0x2ac) || !in_cutscene()) return;
    auto *application=*proc<void* const*>(0x4f6d430);
    if(!application || field<uint32_t>(application,0x5b8908)<=field<uint32_t>(player,0x258)) return;
    auto *decoder=field<void*>(player,0x248);
    const auto *name=field<const char*>(player,0x268);
    if(!decoder || !name) return;
    const MovieKey info{field<uint32_t>(decoder,0),field<uint32_t>(decoder,4),field<uint32_t>(decoder,8),field<uint32_t>(player,0x2a0)};
    const int32_t frame=field<int32_t>(decoder,0xc);
    if(!info.width || !info.height || info.width>32768 || info.height>32768 || !info.frames ||
       !info.last || info.last>info.frames || info.last>INT32_MAX || frame<=0) return;
    const auto clock=movie_clock(name,info,frame);
    const auto target=observe(clock,reinterpret_cast<Address>(player),static_cast<int32_t>(info.last),true);
    if(!target) return;
    // This wrapper waits for active async decode, seeks and restarts its work.
    // It explicitly ignores frame 1: only an observed natural frame 1 can match.
    if(*target<=1){confirm_request(clock,false);return;}
    proc<MovieSeek>(movie_seek_rva)(player,static_cast<uint32_t>(*target),false);
    // A seek request can move to an earlier keyframe first. The match tracker
    // still requires a later native update to see the requested decoder frame.
    confirm_request(clock,field<void*>(player,0x248)==decoder &&
                           field<uint32_t>(player,0x2b0)==static_cast<uint32_t>(*target));
}
void sequence_hook(void *seq,void *msg) {
    sequence_original(seq,msg);
    if(!enabled.load(std::memory_order_acquire) || observing) return;
    std::unique_lock gate(callback_gate,std::try_to_lock);
    if(!gate || !enabled.load()) return;
    observing=true;
    try{observe_sequence(seq);}catch(const std::exception&){} // Recording a clock is optional to gameplay.
    observing=false;
}
bool movie_hook(void *player,void *arg2,void *arg3) {
    // Inspect the player before its original tick can end playback and release
    // the decoder. Only inspect the same new simulation ticks the game accepts.
    if(enabled.load(std::memory_order_acquire) && !observing) {
        std::unique_lock gate(callback_gate,std::try_to_lock);
        if(gate && enabled.load()) {
            observing=true;
            try{observe_movie(player);}catch(const std::exception&){}
            observing=false;
        }
    }
    return movie_original(player,arg2,arg3);
}
template<size_t N> void signature(Address base,Address rva,const unsigned char (&bytes)[N]) {
    if(std::memcmp(reinterpret_cast<const void*>(base+rva),bytes,N)!=0)
        throw std::runtime_error("The cinematic callback does not match this game build.");
}
}
void start_cinematic_tracking(uintptr_t base) {
    std::lock_guard gate(callback_gate);std::lock_guard lock(mutex);
    if(enabled.load()) return;
    if(!base||base!=reinterpret_cast<Address>(GetModuleHandleW(nullptr))) throw std::runtime_error("Invalid cinematic executable identity.");
    module_base=base; game_thread=GetCurrentThreadId();
    if(!installed){
        // The caller already validated the SHA-256. Also reject entry points
        // changed in memory by another live mod, before creating either hook.
        signature(base,sequence_tick_rva,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9});
        signature(base,jump_rva,{0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55});
        signature(base,resolve_rva,{0x48,0x89,0x5c,0x24,0x20,0x57,0x48,0x83,0xec,0x20});
        signature(base,movie_tick_rva,{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x6c,0x24,0x20});
        signature(base,movie_seek_rva,{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10});
        auto status=MH_Initialize();
        if(status!=MH_OK && status!=MH_ERROR_ALREADY_INITIALIZED) throw std::runtime_error("Could not initialize cinematic tracking.");
        const auto a=proc<void*>(sequence_tick_rva),b=proc<void*>(movie_tick_rva);
        if(MH_CreateHook(a,reinterpret_cast<void*>(sequence_hook),reinterpret_cast<void**>(&sequence_original))!=MH_OK)
            throw std::runtime_error("Could not subscribe to cinematic frames.");
        if(MH_CreateHook(b,reinterpret_cast<void*>(movie_hook),reinterpret_cast<void**>(&movie_original))!=MH_OK) {
            MH_RemoveHook(a);throw std::runtime_error("Could not subscribe to the movie clock.");
        }
        // Threads may still be returning from a callback during ReShade shutdown.
        // Retain the small, inert trampolines until process exit, even if a runtime resets.
        HMODULE pinned{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(sequence_hook),&pinned)) {
            MH_RemoveHook(a);MH_RemoveHook(b);throw std::runtime_error("Could not protect the cinematic callback lifetime.");
        }
        installed=true;
    }
    auto a=proc<void*>(sequence_tick_rva),b=proc<void*>(movie_tick_rva);
    const auto sequence_status=MH_EnableHook(a);
    if(sequence_status!=MH_OK && sequence_status!=MH_ERROR_ENABLED)
        throw std::runtime_error("Could not activate the sequence clock.");
    const auto movie_status=MH_EnableHook(b);
    if(movie_status!=MH_OK && movie_status!=MH_ERROR_ENABLED) {
        MH_DisableHook(a);throw std::runtime_error("Could not activate the movie clock.");
    }
    recent={};matcher=Match{};awaiting.store(false);
    enabled.store(true,std::memory_order_release);
}
void stop_cinematic_tracking() {
    std::lock_guard gate(callback_gate);
    enabled.store(false,std::memory_order_release);
    if(installed) {
        // This is a runtime callback outside DllMain. Disable the two detours;
        // keep trampoline memory because an earlier caller can still be
        // returning through it on another thread.
        MH_DisableHook(proc<void*>(sequence_tick_rva));
        MH_DisableHook(proc<void*>(movie_tick_rva));
    }
    std::lock_guard lock(mutex);recent={};matcher=Match{};awaiting.store(false);
}
bool has_live_scene() {
    if(!enabled.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex); const auto now=GetTickCount64();
    // Duplicate clocks cannot be used for seeking, but still signal playback.
    for(const auto &r:recent) if(r.clock.kind && now-r.time<=fresh_ms) return true;
    return false;
}
SceneStamps current_scenes() {
    // May run on the render thread at F5. Return only copied observations; do
    // not touch engine objects, native functions or the world-state singleton.
    if(!enabled.load(std::memory_order_acquire)) return {};
    std::lock_guard lock(mutex);SceneStamps result;const auto now=GetTickCount64();
    for(const auto &r:recent) if(r.clock.kind && !r.ambiguous && now-r.time<=fresh_ms) result.push_back(r.clock);
    return result;
}
void resume_scenes(const SceneStamps &saved) {
    std::lock_guard lock(mutex);recent={};matcher=Match(saved,GetTickCount64());awaiting.store(!saved.empty());
}
SceneResult scene_result() {
    std::lock_guard lock(mutex);const auto r=matcher.result(GetTickCount64());if(r.finished) awaiting.store(false);return r;
}
}
