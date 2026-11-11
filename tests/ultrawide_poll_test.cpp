// Exercise the production detour, including its return type. No game is loaded,
// hooks installed, or real windows inspected. The display answers below only
// describe the standalone fixture's imaginary 3440x1440 foreground window.
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace test_display {
HWND hwnd() { return reinterpret_cast<HWND>(1); }
BOOL WINAPI is_window(HWND) { return TRUE; }
BOOL WINAPI is_iconic(HWND) { return FALSE; }
HWND WINAPI foreground() { return hwnd(); }
BOOL WINAPI client(HWND, LPRECT value) { *value={0,0,3440,1440}; return TRUE; }
HMONITOR WINAPI monitor(HWND, DWORD) { return reinterpret_cast<HMONITOR>(1); }
BOOL WINAPI info(HMONITOR, LPMONITORINFO value) { value->rcMonitor={0,0,3440,1440}; return TRUE; }
}
#define IsWindow test_display::is_window
#define IsIconic test_display::is_iconic
#define GetForegroundWindow test_display::foreground
#define GetClientRect test_display::client
#define MonitorFromWindow test_display::monitor
#define GetMonitorInfoW test_display::info
#include "../src/ultrawide.cpp"
#undef IsWindow
#undef IsIconic
#undef GetForegroundWindow
#undef GetClientRect
#undef MonitorFromWindow
#undef GetMonitorInfoW

namespace qs {
void ensure_graphics_snapshot(uintptr_t) {}
}

using NativePoll = bool (*)(void *, void *);
static_assert(std::is_same_v<qs::Poll, NativePoll>,
    "The native graphics poll returns a Boolean change flag, not void");
static_assert(std::is_same_v<decltype(&qs::poll_hook), NativePoll>,
    "The detour must return the native Boolean through its cleanup paths");

namespace {
using namespace qs::ultrawide_detail;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool native_changed{}, repair_changed{};
unsigned poll_calls{}, repair_calls{}, read_calls{};
Options applied{};
bool native_poll(void *, void *) { ++poll_calls; return native_changed; }
bool native_apply(void *, void *, const Options *options) {
    ++repair_calls; applied=*options; return repair_changed;
}
void read_options(void *, Options *options) {
    ++read_calls;
    options->fill(-1);
    *options=with_size(*options,{1920,1080});
}

// Production reads verified RVAs directly. Reserve their address span without
// committing it; only two global-pointer pages and one function-thunk page are
// backed. This is private test memory, never the installed game's executable.
struct NativeFixture {
    unsigned char *mapping=static_cast<unsigned char *>(VirtualAlloc(nullptr,0x5790000,MEM_RESERVE,PAGE_NOACCESS));
    std::vector<unsigned char> renderer=std::vector<unsigned char>(0x410);
    std::vector<unsigned char> device=std::vector<unsigned char>(0x5ba200);
    std::array<unsigned char,0x40> surface{};

    unsigned char *commit(uintptr_t offset) {
        auto *page=mapping+(offset & ~uintptr_t(0xfff));
        check(VirtualAlloc(page,0x1000,MEM_COMMIT,PAGE_READWRITE)==page,"Could not commit fixture page");
        return mapping+offset;
    }
    template<class T> static void put(void *base,size_t offset,T value) {
        std::memcpy(static_cast<unsigned char *>(base)+offset,&value,sizeof value);
    }
    NativeFixture() {
        check(mapping!=nullptr,"Could not reserve fixture address span");
        qs::image=reinterpret_cast<uintptr_t>(mapping);
        put(commit(qs::renderer_rva),0,renderer.data());
        put(commit(qs::device_rva),0,device.data());
        put(device.data(),0x5ba1d0,surface.data());
        auto *code=commit(qs::read_options_rva);
        // x64 absolute jump to the local read-options stub, preserving the
        // caller's original return address and normal Windows calling ABI.
        code[0]=0x48; code[1]=0xb8;
        put(code,2,reinterpret_cast<uintptr_t>(&read_options));
        code[10]=0xff; code[11]=0xe0;
        DWORD previous{};
        check(VirtualProtect(code,12,PAGE_EXECUTE_READ,&previous)!=FALSE,"Could not seal fixture thunk");
        check(FlushInstructionCache(GetCurrentProcess(),code,12)!=FALSE,"Could not publish fixture thunk");
    }
    ~NativeFixture() { if(mapping) VirtualFree(mapping,0,MEM_RELEASE); }
    void dimensions(Size value) {
        put(renderer.data(),0x3f8,value);
        put(surface.data(),0x34,value.width | (value.height<<15));
        put(device.data(),0x5ba1f8,float(value.width)/float(value.height));
    }
    bool call() { return qs::poll_hook(renderer.data(),reinterpret_cast<void *>(1)); }
};

void reset(bool changed) {
    native_changed=changed; repair_changed=false;
    poll_calls=repair_calls=read_calls=0; applied.fill(0);
    qs::original_poll=&native_poll; qs::original_apply=&native_apply;
    qs::ready.store(true); qs::wanted.store(false); qs::owns_size.store(false);
    qs::selected.store(0); qs::window.store(test_display::hwnd());
    qs::focus_generation.store(0); qs::processing.clear();
    qs::in_poll=false; qs::force_comparison=false;
    qs::budget={}; qs::repair_count=0; qs::report={};
}
void cleaned() {
    check(!qs::in_poll,"Poll scope leaked after returning");
    const bool busy=qs::processing.test_and_set(); qs::processing.clear();
    check(!busy,"Poll ownership leaked after returning");
    check(poll_calls==1,"Native poll must run exactly once");
}
}

int main() {
    try {
        NativeFixture fixture;
        for (bool changed : {false,true}) {
            reset(changed); qs::ready.store(false);
            check(fixture.call()==changed,"Not-ready path lost native change flag"); cleaned();

            reset(changed); qs::processing.test_and_set(); qs::in_poll=true;
            check(fixture.call()==changed,"Reentrant path lost native change flag");
            check(qs::in_poll && qs::processing.test_and_set(),"Reentrant call released its outer scope");
            check(poll_calls==1,"Reentrant native poll count");

            reset(changed);
            check(fixture.call()==changed,"Disabled path lost native change flag through cleanup"); cleaned();

            reset(changed); qs::wanted.store(true); fixture.dimensions(target);
            check(fixture.call()==changed,"Active observation lost native change flag"); cleaned();
            check(!repair_calls && qs::report.measured,"Already-correct display unexpectedly repaired");

            for (bool repaired : {false,true}) {
                reset(changed); repair_changed=repaired; qs::wanted.store(true);
                fixture.dimensions({1920,1080});
                check(fixture.call()==(changed || repaired),"Repair did not preserve both change results"); cleaned();
                check(repair_calls==1 && read_calls==1,"Native true short-circuited a needed repair");
                check(size(applied)==target,"Repair did not request the intended resolution");
                check(!qs::force_comparison,"Temporary repair comparison leaked");
            }
        }
        std::cout<<"Production poll: native Boolean preserved through all paths; repair results combined without skipped calls.\n";
        return 0;
    } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
