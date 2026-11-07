#include "game.hpp"
#include "library.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cwchar>
#include <cmath>
#include <stdexcept>

namespace qs {
namespace {

static_assert(sizeof(void*) == 8, "Director's Cut uses the 64-bit Windows ABI.");

// Relative addresses from ds.exe, Steam build 13300582. These addresses are
// meaningful only for this exact executable. On an update, rediscover the
// capture, world restore and cinematic paths and retest
// a real delivery round trip before adding another build identity.
constexpr Digest supported_hash{
    0x9d,0x53,0x06,0xd7,0xeb,0x2f,0x5d,0x31,0x35,0xc6,0x3a,0xcc,0xc3,0xe7,0x4b,0x8f,
    0x9b,0x02,0x9f,0x9d,0x76,0x11,0x2a,0x6a,0xae,0x4a,0x38,0xfd,0xbb,0x4e,0xdd,0x2b
};
constexpr std::uintmax_t supported_size = 86479872;
}
Game::Game() {
    const auto host = GetModuleHandleW(nullptr);
    std::array<wchar_t, 32768> filename{};
    const DWORD size = GetModuleFileNameW(host, filename.data(), static_cast<DWORD>(filename.size()));
    if (!host || !size || size >= filename.size())
        throw std::runtime_error("Cannot identify the running game.");
    const fs::path path(filename.data());
    if (_wcsicmp(path.filename().c_str(), L"ds.exe") != 0 ||
        fs::file_size(path) != supported_size || sha256(read_file(path)) != supported_hash)
        throw std::runtime_error("Unsupported game executable. QuickSave requires Director's Cut Steam build 13300582.");
    base_ = reinterpret_cast<std::uintptr_t>(host);
}



namespace {
template<class T> bool sample_field(std::uintptr_t address,T &value) {
    SIZE_T count{};
    return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),
               &value,sizeof(value),&count) && count==sizeof(value);
}
}
std::optional<std::uint64_t> Game::state_flags() const {
    // The game's own GetGameState (2DD36C0) reads this singleton field. Copy it
    // through Windows so the render callback can fail closed during a teardown.
    std::uintptr_t state{}, confirm{};
    std::uint64_t flags{};
    if (!sample_field(base_+0x7bf6e88,state) || !state || !sample_field(state+8,flags) ||
        !sample_field(base_+0x7bf6e88,confirm) || confirm!=state) return {};
    return flags;
}
std::uintptr_t Game::active_timer_world(bool &paused) const {
    const auto flags=state_flags();
    paused=false;
    if (save_hold(flags,false)!=SaveHold::none || (*flags & 0x2000)) return 0;
    // Ordinary pauses keep the time left. Loads and world changes rearm it.
    paused=(*flags & (0x4|0x10000000))!=0;
    std::uintptr_t world{}, manager{}, player{}, entity{};
    if (!sample_field(base_+0x7be6318,world) || !world ||
        reinterpret_cast<bool(*)()>(base_+0x3668c50)() ||
        !sample_field(base_+0x7bc7568,manager) || !manager ||
        !sample_field(manager+0x60,player) || !player ||
        !sample_field(player+0x70,entity) || !entity) return 0;
    return world;
}
WorldSample Game::sample() const {
    WorldSample result;
    result.flags=state_flags().value_or(0);
    // These pointers may disappear during a transition. Coordinates are diagnostic
    // observations and a missing player must not become an invented (0,0,0).
    std::uintptr_t manager{},player{},entity{}; std::array<double,3> xyz{};
    if(sample_field(base_+0x7bc7568,manager) && manager &&
       sample_field(manager+0x48,player) && player && sample_field(player+0x70,entity) && entity &&
       sample_field(entity+0xc8,xyz) && std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2]))
        result.position=xyz;
    result.delivery=sample_delivery_state(base_);
    result.carried_count=sample_carried_cargo_count(base_);
    return result;
}
}
