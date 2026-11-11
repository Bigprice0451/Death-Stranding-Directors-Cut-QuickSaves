// Optional, isolated verification against the user's own supported ds.exe.
// Loads code without running the game's entry point or attaching to a game.
#include "turning.hpp"
#include "turning_policy.hpp"
#include "library.hpp"
#include "autosave.hpp"
#include "vehicle_live.hpp"
#include <windows.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char *why) { if (!condition) throw std::runtime_error(why); }
uintptr_t base{};
LONG WINAPI fault(EXCEPTION_POINTERS *info) {
    std::cerr << "Native fixture fault at RVA 0x" << std::hex <<
        reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) - base <<
        " code=" << info->ExceptionRecord->ExceptionCode << std::endl;
    std::cerr << "image=" << base << " ip=" << info->ContextRecord->Rip << " access=" <<
        info->ExceptionRecord->ExceptionInformation[1] << " rax=" << info->ContextRecord->Rax << " rcx=" <<
        info->ContextRecord->Rcx << " rdx=" << info->ContextRecord->Rdx << std::endl;
    return EXCEPTION_EXECUTE_HANDLER;
}
template<class T> void put(void *memory, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t *>(memory) + offset, &value, sizeof value);
}
void writable_put(uintptr_t location, uintptr_t value) {
    DWORD old{};
    check(VirtualProtect(reinterpret_cast<void *>(location), sizeof value, PAGE_READWRITE, &old), "IAT protection failed");
    put(reinterpret_cast<void *>(location), 0, value);
    DWORD ignored{}; VirtualProtect(reinterpret_cast<void *>(location), sizeof value, old, &ignored);
}
void resolve_math_imports() {
    const auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    auto *imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    auto runtime = LoadLibraryW(L"ucrtbase.dll");
    check(runtime != nullptr, "Windows math runtime missing");
    unsigned resolved = 0;
    auto resolve = [&](uint32_t names_rva, uint32_t slots_rva, uint32_t dll_rva) {
        if (std::strcmp(reinterpret_cast<const char *>(base + dll_rva), "api-ms-win-crt-math-l1-1-0.dll")) return;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + names_rva);
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + slots_rva);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto *name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name;
            // Do not initialize Steam, graphics, game dependencies, or the game.
            auto proc = GetProcAddress(runtime, name);
            check(proc != nullptr, "Unresolved native math function");
            writable_put(reinterpret_cast<uintptr_t>(slots), reinterpret_cast<uintptr_t>(proc));
            ++resolved;
        }
    };
    for (; imports->Name; ++imports) resolve(imports->OriginalFirstThunk, imports->FirstThunk, imports->Name);
    struct DelayImport { uint32_t attributes, name, module, slots, names, bound, unload, timestamp; };
    auto *delayed = reinterpret_cast<DelayImport *>(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress);
    for (; delayed->name; ++delayed) {
        check(delayed->attributes == 1, "Unsupported delay import layout");
        resolve(delayed->names, delayed->slots, delayed->name);
    }
    check(resolved >= 2, "Math imports were not found");
}
using Mover = std::array<uint8_t, 0x900>;
struct Result { Mover mover; std::array<float, 4> facing; };
struct Fixture {
    alignas(16) Mover mover{};
    alignas(16) std::array<uint8_t, 0x6000> entity{};
    alignas(16) std::array<uint8_t, 0x400> player{};
    std::array<uint8_t, 0x80> manager{};
    std::array<uint8_t, 0x20> world{};
    Fixture() {
        writable_put(base + 0x7bf6e88, reinterpret_cast<uintptr_t>(world.data()));
        writable_put(base + 0x7be6318, reinterpret_cast<uintptr_t>(world.data()));
        writable_put(base + 0x7bc7568, reinterpret_cast<uintptr_t>(manager.data()));
        put(manager.data(), 0x48, player.data());
        put(player.data(), 0x70, entity.data());
    }
    Result run(float factor, float dt, uint64_t flags = 0, bool ride = false, bool other = false) {
        mover.fill(0); entity.fill(0);
        put(world.data(), 8, flags);
        put(player.data(), 0x3bc, ride ? uint32_t{0x12345} : qs::no_ride);
        put(entity.data(), 0x54b0, other ? nullptr : mover.data());
        put(mover.data(), 0x48, entity.data()); put(mover.data(), 0x2c8, entity.data());
        put(entity.data(), 0xe0, std::array<float, 9>{1,0,0,0,1,0,0,0,1});
        // Distinct movement/cargo state, untouched by the selected functions.
        put(entity.data(), 0x45e4, 0.47f);
        put(entity.data(), 0xc8, std::array<double, 3>{1234.5, 678.9, -321.});
        for (size_t offset : {0x314, 0x318, 0x31c}) put(mover.data(), offset, factor);
        put(mover.data(), 0x326, uint8_t{1});
        put(mover.data(), 0x330, std::array<float, 4>{1,0,0,0});
        put(mover.data(), 0x350, std::array<float, 4>{1,0,0,0});
        put(mover.data(), 0x360, std::array<float, 4>{0,1,0,0});
        put(mover.data(), 0x370, std::array<float, 4>{0,1,0,0});
        put(mover.data(), 0x4f8, 1.f);
        const auto actor_before = entity;
        alignas(16) std::array<float, 4> out{};
        reinterpret_cast<void(*)(void *, float)>(base + qs::turning_detail::heading_rva)(mover.data(), dt);
        reinterpret_cast<void(*)(void *, void *, float)>(base + qs::turning_detail::facing_rva)(mover.data(), out.data(), dt);
        check(entity == actor_before, "Turning changed entity position/speed/cargo bytes");
        for (size_t offset : {0x314, 0x318, 0x31c}) {
            float restored{}; std::memcpy(&restored, mover.data() + offset, 4);
            check(restored == factor, "A hook left its temporary multiplier in game state");
            put(mover.data(), offset, 1.f); // Normalize inputs for complete result comparison.
        }
        for (float value : out) check(std::isfinite(value), "Native facing produced invalid output");
        return {mover, out};
    }
};
bool same(const Result &a, const Result &b) { return a.mover == b.mover && a.facing == b.facing; }
double time_turns(Fixture &fixture) {
    constexpr unsigned count = 50000;
    alignas(16) std::array<float,4> out{};
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i=0; i<count; ++i) {
        put(fixture.mover.data(), 0x326, uint8_t{1});
        put(fixture.mover.data(), 0x330, std::array<float,4>{1,0,0,0});
        reinterpret_cast<void(*)(void *,float)>(base + qs::turning_detail::heading_rva)(fixture.mover.data(), 1.f/60.f);
        reinterpret_cast<void(*)(void *,void *,float)>(base + qs::turning_detail::facing_rva)(fixture.mover.data(), out.data(), 1.f/60.f);
    }
    return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/count;
}
}
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(fault);
    try {
        check(argc == 2, "Pass the supported ds.exe as the only argument");
        const qs::Digest expected{0x9d,0x53,0x06,0xd7,0xeb,0x2f,0x5d,0x31,0x35,0xc6,0x3a,0xcc,0xc3,0xe7,0x4b,0x8f,
            0x9b,0x02,0x9f,0x9d,0x76,0x11,0x2a,0x6a,0xae,0x4a,0x38,0xfd,0xbb,0x4e,0xdd,0x2b};
        check(qs::sha256(qs::read_file(argv[1])) == expected, "Unsupported executable hash");
        base = reinterpret_cast<uintptr_t>(LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES));
        check(base != 0, "Could not map game code");
        resolve_math_imports();
        Fixture fixture;
        std::array<Result, 3> baseline, slowed;
        const std::array<float, 3> dt{1.f/30.f, 1.f/60.f, 1.f/144.f};
        for (size_t i = 0; i < dt.size(); ++i) {
            baseline[i] = fixture.run(1.f, dt[i]);
            slowed[i] = fixture.run(.15f, dt[i]);
            check(!same(baseline[i], slowed[i]), "Fixture did not exercise angular slowdown");
        }
        std::cout << "Original native turning code exercised at 30/60/144 Hz.\n" << std::flush;
        const auto original_us = time_turns(fixture);
        qs::turning_detail::install(base);
        const auto off_us = time_turns(fixture);
        for (size_t i = 0; i < dt.size(); ++i) {
            check(same(fixture.run(.15f, dt[i]), slowed[i]), "Disabled detour changed native behavior");
            qs::set_turning_enabled(true);
            check(same(fixture.run(.15f, dt[i]), baseline[i]), "Enabled detour differs from neutral response");
            check(same(fixture.run(1.f, dt[i]), baseline[i]), "Already neutral response changed");
            qs::set_turning_enabled(false);
            check(same(fixture.run(.15f, dt[i]), slowed[i]), "Disabling did not restore native behavior");
        }
        qs::set_turning_enabled(true);
        for (uint64_t flags : {uint64_t{0x40}, uint64_t{0x40000}, uint64_t{0x80000}, uint64_t{0x100000},
                               uint64_t{0x600000} << 32, uint64_t{0x400000}, uint64_t{0x4000000},
                               uint64_t{0x20000000}, uint64_t{4}, uint64_t{0x10000000}})
            check(same(fixture.run(.15f, dt[1], flags), slowed[1]), "Scene/transition gate failed");
        check(same(fixture.run(.15f, dt[1], 0, true), slowed[1]), "Vehicle gate failed");
        auto other = fixture.run(.15f, dt[1], 0, false, true);
        check(same(other, slowed[1]), "Other mover gate failed");
        writable_put(base + 0x7bf6e88, 0);
        check(same(fixture.run(.15f, dt[1]), slowed[1]), "Missing state gate failed");
        writable_put(base + 0x7bf6e88, 1);
        check(same(fixture.run(.15f, dt[1]), slowed[1]), "Unreadable state gate failed");
        writable_put(base + 0x7bf6e88, reinterpret_cast<uintptr_t>(fixture.world.data()));
        writable_put(base + 0x7be6318, 0);
        check(same(fixture.run(.15f, dt[1]), slowed[1]), "Missing world gate failed");
        writable_put(base + 0x7be6318, reinterpret_cast<uintptr_t>(fixture.world.data()));
        writable_put(base + 0x7bc7568, 0);
        check(same(fixture.run(.15f, dt[1]), slowed[1]), "Missing player manager gate failed");
        writable_put(base + 0x7bc7568, reinterpret_cast<uintptr_t>(fixture.manager.data()));
        const auto status = qs::turning_status();
        check(status.ready && status.heading_changes == 3 && status.facing_changes == 3 && status.excluded_calls >= 24,
              "Telemetry counters do not describe actual hook behavior");
        qs::turning_detail::install(base); // Runtime recreation must reuse the hooks.
        const auto on_us = time_turns(fixture);
        qs::set_turning_enabled(false);
        std::cout << "Two native calls, average microseconds: original=" << original_us <<
            " disabled=" << off_us << " enabled=" << on_us << " (isolated fixture, not game FPS).\n";
        std::cout << "Native hook ABI, neutral response, on/off restoration, gates and actor bytes passed.\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
