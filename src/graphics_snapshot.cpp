#include "graphics_snapshot.hpp"
#include <windows.h>
#include <intrin.h>
#include <MinHook.h>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace qs {
namespace {
using Snapshot = void *(*)(void *, void *);
Snapshot original{};
std::mutex lifecycle;
uintptr_t image{};
bool prepared{}, installed{};
void *snapshot(void *renderer, void *copy) {
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    auto *result = original(renderer,copy);
    if (result == copy && copy) {
        dlaa_settings_snapshot(copy,caller);
        ultrawide_settings_snapshot(copy,caller);
    }
    return result;
}
}
void ensure_graphics_snapshot(uintptr_t base) {
    if (!base || base != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))
        throw std::runtime_error("Invalid graphics executable identity.");
    graphics_snapshot_detail::install(base);
}
void graphics_snapshot_detail::install(uintptr_t base) {
    std::lock_guard lock(lifecycle);
    if (installed) {
        if (image!=base) throw std::runtime_error("Graphics callbacks belong to another executable.");
        return;
    }
    auto *target = reinterpret_cast<void *>(base + 0x1cd4bb0);
    if (!prepared) {
        const unsigned char signature[]{0x8b,0x81,0xf8,3,0,0,0x89,2,0x8b,0x81,0xfc,3,0,0};
        if (std::memcmp(target,signature,sizeof signature))
            throw std::runtime_error("Graphics settings code differs from this build or another mod changed it.");
        const auto status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
            throw std::runtime_error("Could not initialize graphics settings tracking.");
        if (MH_CreateHook(target,reinterpret_cast<void *>(snapshot),reinterpret_cast<void **>(&original)) != MH_OK)
            throw std::runtime_error("Could not prepare graphics settings tracking.");
        HMODULE pinned{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(snapshot),&pinned)) {
            MH_RemoveHook(target);
            throw std::runtime_error("Could not keep graphics settings callbacks loaded.");
        }
        image=base; prepared=true;
    }
    if (image != base || MH_EnableHook(target) != MH_OK)
        throw std::runtime_error("Could not activate graphics settings tracking. Restart the game.");
    installed=true;
}
}
