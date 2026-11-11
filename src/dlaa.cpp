#include "dlaa.hpp"
#include "dlaa_ngx.hpp"
#include "graphics_snapshot.hpp"
#include <windows.h>
#include <d3d12.h>
#include <intrin.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace qs {
namespace {
using namespace dlaa_detail;
using Configure = void (*)(void *, void *, int, bool);
using Evaluate = U32 (*)(void *, void *, void *, const void *);
using Release = U32 (*)(void *);
using SetDisplaySize = void (*)(void *, U32, U32);
constexpr uintptr_t manager_rva = 0x458ac50;
constexpr uintptr_t configure_rva = 0x19894c0, optimal_rva = 0x1980640;
constexpr uintptr_t create_rva = 0x38f38f0, evaluate_rva = 0x197af90, release_rva = 0x38f3ce0;
constexpr uintptr_t display_size_rva = 0x19a81a0;
constexpr uintptr_t gfx_device_rva = 0x4f6d430, renderer_uses_dlss_rva = 0x6cdeafb;
uintptr_t image{};
Configure configure_original{};
Optimal optimal_original{};
Create create_original{};
Evaluate evaluate_original{};
Release release_original{};
SetDisplaySize display_size_original{};
Params api{};
std::atomic<bool> active{}, preference{}, refresh{};
std::atomic<unsigned> nudges{};
std::atomic<uint64_t> next_nudge{};
std::mutex lifecycle, observations;
bool prepared{};
struct Live {
    void *handle{};
    Created creation{};
    Size input{}, output{};
    uint64_t last_sample{};
    bool native{};
    const char *problem{};
};
Live live;
struct Counters {
    uint64_t evaluations{}, failures{}, creations{}, releases{}, last_evaluation_at{};
    uint64_t history_resets{};
    bool last_frame_reset{};
    U32 last_result{};
};
// These are session totals, separate from Live: destroying a feature must not
// erase the evidence needed to diagnose repeated creation or failed frames.
Counters counters;
const char *startup_error{};

// A size answer may be used only by its matching creation on this thread. This
// scope also holds the preference steady if the UI toggles midway through it.
struct Rebuild {
    bool wanted{}, sized{}, retry{};
    Size output{};
    const char *problem{};
};
thread_local Rebuild *current{};

template <typename T> T addr(uintptr_t offset) { return reinterpret_cast<T>(image + offset); }
template <typename T> T field(const void *p, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const char *>(p) + offset, sizeof(value));
    return value;
}
void configure_hook(void *manager, void *context, int mode, bool force) {
    if (!active.load() || manager != addr<void *>(manager_rva) || current || !context) {
        configure_original(manager, context, mode, force);
        return;
    }
    Rebuild transaction;
    transaction.wanted = preference.load() && mode == quality;
    Rebuild *previous = current;
    current = &transaction;
    configure_original(manager, context, mode, force);
    if (transaction.retry) {
        // A driver can reject a valid creation hint. Rebuild through Decima a
        // second time with its original Quality sizing before returning control.
        transaction.wanted = false;
        transaction.sized = false;
        configure_original(manager, context, mode, true);
    }
    current = previous;
    if (transaction.problem) {
        std::lock_guard lock(observations);
        live.problem = transaction.problem;
    }
}
bool using_quality(void *device) {
    const void *manager = addr<const void *>(manager_rva);
    return device && field<void *>(device, 0xb40) && field<bool>(manager, 0) &&
        field<int>(manager, 0x14) == quality && *addr<const unsigned char *>(renderer_uses_dlss_rva) != 0;
}
void observe_snapshot(void *copy, uintptr_t caller) {
    if (active.load() && refresh.load() && caller == image + 0x2642f0d && copy) {
        void *device = *addr<void **>(gfx_device_rva);
        const auto now = GetTickCount64();
        if (using_quality(device) && nudges.load() < 2 && now >= next_nudge.load()) {
            // Some transitions postpone resource creation. Permit a second
            // wake-up, then let a normal resize finish it without stalling
            // the GPU in a repeated flush every frame.
            ++nudges;
            next_nudge.store(now + 3000);
            // Only this stack copy is a "previous settings" comparator. The
            // settings poll replaces this byte with the user's real option
            // after it flushes its graphics queues, before rebuilding targets.
            static_cast<unsigned char *>(copy)[0x20] = field<unsigned char>(device, 0x5ba1f4) ? 0 : 1;
        }
    }
}
void display_size_hook(void *device, U32 width, U32 height) {
    void *manager = addr<void *>(manager_rva);
    if (active.load() && refresh.load() && _ReturnAddress() == addr<void *>(0x1cd538a) &&
        device == *addr<void **>(gfx_device_rva) && width && height && using_quality(device)) {
        refresh.exchange(false);
        if (field<U32>(manager, 4) == width && field<U32>(manager, 8) == height) {
            // The game's resize helper skips unchanged display sizes. Only at
            // this stage of its own fenced rebuild can we safely force creation.
            configure_hook(manager, field<void *>(device, 0xb40), quality, true);
            return;
        }
    }
    display_size_original(device, width, height);
}
bool optimal_hook(void *manager, U32 width, U32 height, int mode, bool flag, U32 *w, U32 *h, float *sharpness) {
    if (!current || !current->wanted || manager != addr<void *>(manager_rva))
        return optimal_original(manager, width, height, mode, flag, w, h, sharpness);
    current->output = {width, height};
    const bool result = optimal(optimal_original, manager, current->output, mode, flag, w, h, sharpness, true, current->sized);
    if (!current->sized) current->problem = "NGX declined native DLAA sizing. The game is using its own settings.";
    return result;
}
U32 create_hook(void *cmd, U32 feature, void *params, void **handle) {
    if (!current || feature != super_sampling || handle != addr<void **>(manager_rva + 0x30))
        return create_original(cmd, feature, params, handle);
    Created info;
    const U32 result = create(api, create_original, cmd, feature, params, handle, current->sized, current->output, info);
    if (current->sized && (!info.preset_requested || !success(result) || !*handle)) {
        current->retry = true;
        current->problem = "NGX declined DLAA or its K hint. The game retried its normal Quality configuration.";
    }
    std::lock_guard lock(observations);
    ++counters.creations;
    live = {};
    if (success(result) && *handle && info.preset_requested) {
        live.handle = *handle;
        live.creation = info;
    }
    return result;
}
Size texture_size(ID3D12Resource *resource) {
    if (!resource) return {};
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width > UINT32_MAX) return {};
    return {static_cast<U32>(desc.Width), desc.Height};
}
U32 evaluate_hook(void *cmd, void *handle, void *params, const void *frame) {
    const U32 result = evaluate_original(cmd, handle, params, frame);
    if (!active.load() || !frame || !handle) return result;
    std::lock_guard lock(observations);
    ++counters.evaluations;
    // Build 13300582's older NGX frame structure: +0x38 is the Reset integer.
    // Count all native DLSS frames, including regular Quality with DLAA off.
    counters.last_frame_reset = field<int>(frame, 0x38) != 0;
    if (counters.last_frame_reset) ++counters.history_resets;
    counters.last_result = result;
    counters.last_evaluation_at = GetTickCount64();
    if (!success(result)) ++counters.failures;
    if (handle != live.handle) return result;
    if (!success(result)) {
        live.native = false;
        live.last_sample = 0;
        live.problem = "NGX reported an evaluation error.";
        return result;
    }
    const auto now = GetTickCount64();
    if (live.last_sample && now - live.last_sample < 250) return result;
    live.last_sample = now;
    live.input = field<Size>(frame, 0x30);
    const auto color = texture_size(field<ID3D12Resource *>(frame, 0));
    live.output = texture_size(field<ID3D12Resource *>(frame, 8));
    live.native = native_frame(live.input, color, live.output, live.creation.output);
    live.problem = live.native ? nullptr : "NGX ran, but the measured input rectangle and output texture do not confirm native resolution.";
    return result;
}
U32 release_hook(void *handle) {
    const U32 result = release_original(handle);
    if (success(result)) {
        std::lock_guard lock(observations);
        ++counters.releases;
        if (handle == live.handle) live = {};
    }
    return result;
}
struct Hook { uintptr_t rva; void *detour; void **original; std::array<unsigned char,8> signature; };
const Hook hooks[] = {
    {configure_rva, (void *)configure_hook, (void **)&configure_original, {0x40,0x53,0x57,0x48,0x81,0xec,0x88,0}},
    {optimal_rva, (void *)optimal_hook, (void **)&optimal_original, {0x40,0x55,0x53,0x56,0x41,0x54,0x41,0x55}},
    {create_rva, (void *)create_hook, (void **)&create_original, {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x6c}},
    {evaluate_rva, (void *)evaluate_hook, (void **)&evaluate_original, {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x6c}},
    {release_rva, (void *)release_hook, (void **)&release_original, {0x48,0x89,0x5c,0x24,8,0x57,0x48,0x83}},
    {display_size_rva, (void *)display_size_hook, (void **)&display_size_original, {0x40,0x53,0x48,0x83,0xec,0x20,0x80,0x3d}}
};
bool prepare(uintptr_t base) {
    if (!base || base != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) return false;
    image = base;
    for (const auto &hook : hooks) {
        if (std::memcmp(addr<const void *>(hook.rva), hook.signature.data(), hook.signature.size())) return false;
    }
    const auto mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) return false;
    size_t count = 0;
    for (const auto &hook : hooks) {
        if (MH_CreateHook(addr<void *>(hook.rva), hook.detour, hook.original) != MH_OK) break;
        ++count;
    }
    // An in-flight NGX call can still be returning through our code during a
    // runtime reset. Keep the module and its disabled trampolines until exit.
    HMODULE pinned{};
    if (count != std::size(hooks) || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(configure_hook), &pinned)) {
        while (count) MH_RemoveHook(addr<void *>(hooks[--count].rva));
        return false;
    }
    api = {addr<decltype(api.set_i)>(0x38f31d0), addr<decltype(api.set_u)>(0x38f3230),
           addr<decltype(api.get_i)>(0x38f2ed0), addr<decltype(api.get_u)>(0x38f2f30)};
    return true;
}
}
void dlaa_settings_snapshot(void *copy, uintptr_t caller) { observe_snapshot(copy, caller); }
void start_dlaa_tracking(uintptr_t base, bool enabled) {
    std::lock_guard life(lifecycle);
    if (active.load()) return;
    ensure_graphics_snapshot(base);
    preference.store(enabled);
    nudges.store(0);
    next_nudge.store(0);
    refresh.store(enabled || prepared);
    if (!prepared) prepared = prepare(base);
    bool ready = prepared && base == image;
    if (ready) for (const auto &hook : hooks) {
        const auto code = MH_EnableHook(addr<void *>(hook.rva));
        if (code != MH_OK && code != MH_ERROR_ENABLED) { ready = false; break; }
    }
    if (!ready && prepared) for (const auto &hook : hooks) MH_DisableHook(addr<void *>(hook.rva));
    {
        std::lock_guard lock(observations);
        live = {};
        startup_error = ready ? nullptr : "Graphics hooks are unavailable for this running executable.";
    }
    active.store(ready);
}
void stop_dlaa_tracking() {
    std::lock_guard life(lifecycle);
    active.store(false);
    if (prepared) for (const auto &hook : hooks) MH_DisableHook(addr<void *>(hook.rva));
    std::lock_guard lock(observations);
    live = {};
}
void set_dlaa_enabled(bool enabled) {
    if (preference.exchange(enabled) != enabled) {
        nudges.store(0);
        next_nudge.store(0);
        refresh.store(true);
    }
}
DlaaStatus dlaa_status() {
    DlaaStatus s;
    std::lock_guard lock(observations);
    s.requested = preference.load();
    s.hooks_ready = active.load();
    s.evaluations=counters.evaluations; s.evaluation_failures=counters.failures;
    s.history_resets=counters.history_resets; s.last_frame_reset=counters.last_frame_reset;
    s.creations=counters.creations; s.releases=counters.releases;
    s.last_evaluation_at=counters.last_evaluation_at; s.last_result=counters.last_result;
    s.refresh_pending=refresh.load(); s.refresh_nudges=nudges.load();
    const bool recent = live.last_sample && GetTickCount64() - live.last_sample < 1800;
    s.preset_requested = live.handle && live.creation.preset_requested;
    s.evaluated = s.preset_requested && live.native && recent;
    s.input_width = live.input.w;
    s.input_height = live.input.h;
    s.output_width = live.output.w;
    s.output_height = live.output.h;
    if (!s.hooks_ready) s.detail = startup_error ? startup_error : "Waiting for the graphics hooks.";
    else if (refresh.load() && nudges.load() >= 2 && GetTickCount64() >= next_nudge.load())
        s.detail = "The renderer deferred its rebuild. Reapply the game's display settings to finish the change.";
    else if (!s.requested && s.preset_requested) s.detail = "Restoring standard Quality on the next graphics update.";
    else if (!s.requested) s.detail = "Off. The game controls DLSS.";
    else if (live.problem) s.detail = live.problem;
    else if (s.evaluated) s.detail = "Native DLAA measured. NGX received the Preset K hint.";
    else if (s.preset_requested) s.detail = "NGX created DLAA with a K hint. Waiting for a native-resolution game frame.";
    else s.detail = "Waiting for DLSS Quality. Enable DLSS and select Quality in the game's graphics settings.";
    return s;
}
}
