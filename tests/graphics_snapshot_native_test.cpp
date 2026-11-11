// Maps the user's own verified game image without running its entry point.
// Executes only its leaf snapshot function plus our actual MinHook detour.
#include "graphics_snapshot.hpp"
#include "ultrawide_policy.hpp"
#include "library.hpp"
#include <windows.h>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
uintptr_t image{}, observed_caller{};
unsigned dlaa_calls{}, ultrawide_calls{};
bool owns{}, compare_once{}, dlaa_refresh{};
using Snapshot = void *(*)(void *,void *);
void check(bool value,const char *why){if(!value)throw std::runtime_error(why);}
template<class T> void put(void *memory,size_t offset,T value){std::memcpy(static_cast<uint8_t *>(memory)+offset,&value,sizeof value);}
void *caller(uintptr_t return_rva) {
    // 4-byte shadow-space prologue, 5-byte call, epilogue. These three private
    // test thunks occupy the real callers' addresses in this isolated mapping;
    // no original graphics/window function or running process is invoked.
    const auto begin=image+return_rva-9;
    std::array<uint8_t,14> code{0x48,0x83,0xec,0x28,0xe8,0,0,0,0,0x48,0x83,0xc4,0x28,0xc3};
    const int32_t relative=int32_t((image+0x1cd4bb0)-(image+return_rva));
    put(code.data(),5,relative);
    DWORD old{};
    check(VirtualProtect(reinterpret_cast<void *>(begin),code.size(),PAGE_EXECUTE_READWRITE,&old),"Cannot prepare fixture caller");
    std::memcpy(reinterpret_cast<void *>(begin),code.data(),code.size());
    FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void *>(begin),code.size());
    DWORD ignored{}; VirtualProtect(reinterpret_cast<void *>(begin),code.size(),old,&ignored);
    return reinterpret_cast<void *>(begin);
}
}
namespace qs {
void dlaa_settings_snapshot(void *copy,uintptr_t from) {
    ++dlaa_calls; observed_caller=from;
    if(dlaa_refresh && from==image+0x2642f0d) static_cast<uint8_t *>(copy)[0x20]=1;
}
void ultrawide_settings_snapshot(void *copy,uintptr_t from) {
    ++ultrawide_calls;
    ultrawide_detail::adjust_snapshot(copy,from-image,owns,{1920,1080},compare_once);
}
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        check(argc==2,"Pass the supported ds.exe as the only argument");
        const qs::Digest expected{0x9d,0x53,0x06,0xd7,0xeb,0x2f,0x5d,0x31,0x35,0xc6,0x3a,0xcc,0xc3,0xe7,0x4b,0x8f,
            0x9b,0x02,0x9f,0x9d,0x76,0x11,0x2a,0x6a,0xae,0x4a,0x38,0xfd,0xbb,0x4e,0xdd,0x2b};
        check(qs::sha256(qs::read_file(argv[1]))==expected,"Unsupported executable hash");
        image=reinterpret_cast<uintptr_t>(LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES));
        check(image!=0,"Could not map native snapshot code");
        std::array<uint8_t,0x500> renderer{};
        put(renderer.data(),0x3f8,qs::ultrawide_detail::target);
        put(renderer.data(),0x3f0,uint32_t{8});
        put(renderer.data(),0x400,uint32_t{99});put(renderer.data(),0x404,uint32_t{101});
        const auto unchanged=renderer;
        std::array<uint8_t,40> snapshot; snapshot.fill(0x55);
        const auto leaf=reinterpret_cast<Snapshot>(image+0x1cd4bb0);
        check(leaf(renderer.data(),snapshot.data())==snapshot.data(),"Native return ABI differs");
        const auto native=snapshot;
        qs::graphics_snapshot_detail::install(image);
        auto config=reinterpret_cast<Snapshot>(caller(0x2645034));
        auto options=reinterpret_cast<Snapshot>(caller(0x26447e6));
        auto poll=reinterpret_cast<Snapshot>(caller(0x2642f0d));
        auto run=[&](Snapshot target,uintptr_t from) {
            snapshot.fill(0x55);
            check(target(renderer.data(),snapshot.data())==snapshot.data(),"Detour changed return pointer");
            check(observed_caller==image+from,"Original call-site identity was lost");
            check(renderer==unchanged,"Snapshot changed actual renderer fields");
        };
        run(config,0x2645034);check(snapshot==native,"Disabled hook changed native snapshot");
        owns=true;
        run(config,0x2645034);
        auto wanted=native;put(wanted.data(),0,qs::ultrawide_detail::Size{1920,1080});
        check(snapshot==wanted,"Only the stored choice should reach configuration output");
        compare_once=true;
        run(options,0x26447e6);wanted=native;put(wanted.data(),0,uint32_t{0});
        check(snapshot==wanted && !compare_once,"Native comparison override failed");
        run(options,0x26447e6);check(snapshot==native,"Comparison flag leaked into another read");
        dlaa_refresh=true;
        run(poll,0x2642f0d);wanted=native;wanted[0x20]=1;
        check(snapshot==wanted,"DLAA refresh and ultrawide preservation overlap");
        qs::graphics_snapshot_detail::install(image);
        check(dlaa_calls==5 && ultrawide_calls==5,"Callbacks were lost or duplicated");
        std::cout<<"Original snapshot ABI, exact native callers, shared callback dispatch and private-copy preservation passed.\n";
        return 0;
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
