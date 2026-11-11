#include "ultrawide.hpp"
#include "graphics_snapshot.hpp"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace qs {
namespace {
using namespace ultrawide_detail;
// The native caller treats AL as "settings changed" and resets temporal
// history when true. Preserve this result through all observation/RAII work.
using Poll = bool (*)(void *, void *);
using Apply = bool (*)(void *, void *, const Options *);
using ReadOptions = void (*)(void *, Options *);
constexpr uintptr_t poll_rva=0x2642ec0, apply_rva=0x2642010, read_options_rva=0x26447c0;
constexpr uintptr_t device_rva=0x4f6d430, renderer_rva=0x578c148;
uintptr_t image{};
Poll original_poll{};
Apply original_apply{};
std::atomic<bool> ready{}, wanted{}, owns_size{};
std::atomic<uint64_t> selected{};
std::atomic<HWND> window{};
std::atomic<uint64_t> focus_generation{};
HHOOK focus_hook{};
DWORD focus_thread{};
std::atomic_flag processing=ATOMIC_FLAG_INIT;
std::mutex install_mutex, report_mutex;
UltrawideStatus report;
bool installed{}, activation_failed{};
// Only the intercepted normal settings poll owns these values. Nested native
// apply calls use the same thread; other calls pass through without touching it.
thread_local bool in_poll{};
thread_local bool force_comparison{};
RepairBudget budget;
uint64_t repair_count{};

LRESULT CALLBACK focus_messages(int code, WPARAM sent, LPARAM data) {
    if (code >= 0) {
        const auto *message=reinterpret_cast<const CWPSTRUCT *>(data);
        if (message && message->hwnd==window.load() &&
            ((message->message==WM_ACTIVATEAPP && !message->wParam) ||
             (message->message==WM_ACTIVATE && LOWORD(message->wParam)==WA_INACTIVE) ||
             (message->message==WM_SIZE && message->wParam==SIZE_MINIMIZED) ||
              message->message==WM_KILLFOCUS))
            focus_generation.fetch_add(1,std::memory_order_relaxed);
    }
    return CallNextHookEx(nullptr,code,sent,data);
}
void watch_focus(HWND hwnd) {
    DWORD pid{};
    const auto thread=GetWindowThreadProcessId(hwnd,&pid);
    if (!thread || pid!=GetCurrentProcessId()) throw std::runtime_error("Could not identify the game display window.");
    if (focus_hook && focus_thread==thread) return;
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(focus_messages),&module))
        throw std::runtime_error("Could not keep display focus tracking loaded.");
    const auto replacement=SetWindowsHookExW(WH_CALLWNDPROC,focus_messages,module,thread);
    if (!replacement) throw std::runtime_error("Could not track Alt+Tab focus changes. Restart the game.");
    if (focus_hook) UnhookWindowsHookEx(focus_hook);
    focus_hook=replacement; focus_thread=thread;
}

template<class T> T field(uintptr_t p) {
    T value{}; std::memcpy(&value,reinterpret_cast<const void *>(p),sizeof value); return value;
}
struct Observation {Size render{},viewport{};float aspect{};bool valid{};};
Observation observe(void *renderer) {
    Observation result;
    __try {
        if (reinterpret_cast<uintptr_t>(renderer) != field<uintptr_t>(image+renderer_rva)) return result;
        const auto device=field<uintptr_t>(image+device_rva);
        if (!device || !renderer) return result;
        result.render=field<Size>(reinterpret_cast<uintptr_t>(renderer)+0x3f8);
        const auto surface=field<uintptr_t>(device+0x5ba1d0);
        if (surface) {
            const auto dimensions=field<uint32_t>(surface+0x34);
            result.viewport={dimensions & 0x3fff,(dimensions>>15) & 0x3fff};
        } else result.viewport=field<Size>(device+0x5ba188);
        result.aspect=field<float>(device+0x5ba1f8);
        result.valid=result.render.valid() && result.viewport.valid() && std::isfinite(result.aspect);
    } __except(EXCEPTION_EXECUTE_HANDLER) { result.valid=false; }
    return result;
}
struct Display {bool focused{},monitor_matches{};Size client{};};
Display display() {
    Display result;
    const HWND hwnd=window.load();
    if (!hwnd || !IsWindow(hwnd)) return result;
    result.focused=!IsIconic(hwnd) && GetForegroundWindow()==hwnd;
    RECT client{};
    if (GetClientRect(hwnd,&client)) result.client={uint32_t(client.right-client.left),uint32_t(client.bottom-client.top)};
    MONITORINFO monitor{sizeof monitor};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))
        result.monitor_matches=monitor.rcMonitor.right-monitor.rcMonitor.left==int(target.width) &&
                               monitor.rcMonitor.bottom-monitor.rcMonitor.top==int(target.height);
    return result;
}
void remember(Size value) {
    // Requests assembled from current forced state often carry 3440x1440 while
    // changing an unrelated option. Do not replace the underlying menu choice.
    if (value.valid() && (!selected.load() || value!=target)) selected.store(pack(value));
}
bool apply_hook(void *settings, void *renderer, const Options *options) {
    if (!ready.load() || !in_poll || (!wanted.load() && !owns_size.load()) || !options || !observe(renderer).valid)
        return original_apply(settings,renderer,options);
    remember(size(*options));
    budget.reset();
    const auto screen=display();
    if (wanted.load() && screen.focused && screen.monitor_matches && size(*options).valid()) {
        const auto effective=with_size(*options,target);
        owns_size.store(true);
        return original_apply(settings,renderer,&effective);
    }
    return original_apply(settings,renderer,options);
}
void publish(const Observation &sample, const Display &screen) {
    std::lock_guard lock(report_mutex);
    report.focused=screen.focused; report.monitor_matches=screen.monitor_matches;
    report.render=sample.render; report.viewport=sample.viewport; report.aspect=sample.aspect;
    report.measured=sample.valid && full_image(sample.render,sample.viewport,sample.aspect) && screen.client==target;
    report.selected=unpack(selected.load()); report.attempts=budget.attempts();
    report.repairs=repair_count; report.last_sample=GetTickCount64();
}
bool poll_hook(void *renderer, void *settings) {
    if (!ready.load() || processing.test_and_set(std::memory_order_acquire)) {
        return original_poll(renderer,settings);
    }
    struct Scope {
        Scope(){in_poll=true;}
        ~Scope(){in_poll=false;processing.clear(std::memory_order_release);}
    } scope;
    if (!wanted.load() && !owns_size.load()) {
        selected.store(0); budget.reset();
        return original_poll(renderer,settings);
    }
    auto before=observe(renderer);
    if (wanted.load() && !selected.load() && before.valid) remember(before.render);
    bool changed=original_poll(renderer,settings);
    auto sample=observe(renderer);
    const auto screen=display();
    const bool request=wanted.load();
    budget.observe(request,screen.focused,focus_generation.load(std::memory_order_relaxed));
    const auto normal=unpack(selected.load());
    const bool complete=sample.valid && full_image(sample.render,sample.viewport,sample.aspect) && screen.client==target;
    const bool restoring=!request && owns_size.load();
    if ((request && complete) || (restoring && sample.valid && sample.render==normal && sample.viewport==normal)) {
        if (restoring) owns_size.store(false);
        budget.reset();
    } else if (settings && sample.valid && normal.valid() && screen.focused &&
               ((request && screen.monitor_matches) || restoring) && budget.take(GetTickCount64())) {
        // This is the same graphics-application path called by original_poll.
        // It fences/recreates native targets and window state in native order.
        // Never resize a DXGI surface independently of the game's render targets.
        Options current; current.fill(-1); // Native constructor's unspecified-field sentinel.
        reinterpret_cast<ReadOptions>(image+read_options_rva)(renderer,&current);
        const auto effective=with_size(current,request?target:normal);
        owns_size.store(request);
        ++repair_count;
        {
            // Even if the renderer cached 3440x1440, the window/viewport may
            // have changed independently on focus return. Invalidate only the
            // native apply routine's temporary previous-settings comparison.
            struct ComparisonScope {
                bool previous=force_comparison;
                ComparisonScope(){force_comparison=true;}
                ~ComparisonScope(){force_comparison=previous;}
            } comparison;
            const bool applied=original_apply(settings,renderer,&effective);
            changed |= applied;
        }
        sample=observe(renderer);
        if (!request && (!sample.valid || sample.render!=normal || sample.viewport!=normal))
            owns_size.store(true); // Restoration was deferred; retain it for the next opportunity.
    }
    publish(sample,screen);
    return changed;
}
template<size_t N> void signature(uintptr_t rva,const uint8_t (&bytes)[N]) {
    if (std::memcmp(reinterpret_cast<const void *>(image+rva),bytes,N))
        throw std::runtime_error("Display code differs from this build or another mod changed it.");
}
}
void ultrawide_settings_snapshot(void *copy, uintptr_t caller) {
    if (!owns_size.load() && !force_comparison) return;
    adjust_snapshot(copy,caller-image,owns_size.load(),unpack(selected.load()),force_comparison);
}
void start_ultrawide(uintptr_t base, void *hwnd, bool enable) {
    std::lock_guard lock(install_mutex);
    window.store(static_cast<HWND>(hwnd));
    wanted.store(enable);
    try {
        if (!base || base!=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))
            throw std::runtime_error("Invalid display executable identity.");
        ensure_graphics_snapshot(base);
        watch_focus(static_cast<HWND>(hwnd));
        if (activation_failed) throw std::runtime_error("Display hooks could not start. Restart the game.");
        if (!installed) {
            image=base;
            signature(poll_rva,{0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57});
            signature(apply_rva,{0x40,0x55,0x53,0x56,0x48,0x8d,0xac,0x24,0x70,0xff,0xff,0xff});
            signature(read_options_rva,{0x48,0x89,0x5c,0x24,0x18,0x57,0x48,0x83,0xec,0x60});
            auto *poll=reinterpret_cast<void *>(base+poll_rva);
            auto *apply=reinterpret_cast<void *>(base+apply_rva);
            if (MH_CreateHook(poll,reinterpret_cast<void *>(poll_hook),reinterpret_cast<void **>(&original_poll))!=MH_OK)
                throw std::runtime_error("Could not prepare display refresh tracking.");
            if (MH_CreateHook(apply,reinterpret_cast<void *>(apply_hook),reinterpret_cast<void **>(&original_apply))!=MH_OK) {
                MH_RemoveHook(poll); throw std::runtime_error("Could not prepare the resolution override.");
            }
            // The shared snapshot detour already pinned this module. Retain all
            // live trampolines on partial activation failure until process exit.
            if (MH_EnableHook(apply)!=MH_OK || MH_EnableHook(poll)!=MH_OK) {
                activation_failed=true;
                throw std::runtime_error("Display hooks could not both start. Restart the game.");
            }
            installed=true;
        }
        {
            std::lock_guard report_lock(report_mutex); report.error.clear();
        }
        ready.store(true);
    } catch (const std::exception &e) {
        ready.store(false);
        std::lock_guard report_lock(report_mutex); report.error=e.what();
    }
}
void set_ultrawide_enabled(bool enable) { wanted.store(enable); }
UltrawideStatus ultrawide_status() {
    std::lock_guard lock(report_mutex);
    auto result=report;
    result.ready=ready.load(); result.requested=wanted.load();
    result.restoring=!result.requested && owns_size.load();
    if (!result.last_sample || GetTickCount64()-result.last_sample>1800) result.measured=false;
    return result;
}
}
