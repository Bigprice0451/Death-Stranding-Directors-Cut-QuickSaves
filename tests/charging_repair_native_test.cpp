// Optional offline native fixture. The supported ds.exe is mapped without its
// entry point, dependencies, Steam or graphics being started. Only the original
// battery update/charger arithmetic run; unrelated world services are fixtures.
#include "charging_repair.hpp"
#include "charging_repair_policy.hpp"
#include "library.hpp"
#include "autosave.hpp"
#include <windows.h>
#include <MinHook.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
uintptr_t base{}, charger{}, vehicle_resource{};
bool charger_present=true, charger_unavailable=false, generator_power_zero=false;
void (*during_native_update)(void *){};
void check(bool value,const char *why) { if (!value) throw std::runtime_error(why); }
template<class T> void put(void *p,size_t offset,T value) { std::memcpy(static_cast<uint8_t *>(p)+offset,&value,sizeof value); }
template<class T> T get(const void *p,size_t offset) { T value{}; std::memcpy(&value,static_cast<const uint8_t *>(p)+offset,sizeof value); return value; }
void writable_put(uintptr_t address,uintptr_t value) {
    DWORD old{}; check(VirtualProtect(reinterpret_cast<void *>(address),8,PAGE_READWRITE,&old),"Fixture protection failed");
    put(reinterpret_cast<void *>(address),0,value);
    DWORD ignored{}; VirtualProtect(reinterpret_cast<void *>(address),8,old,&ignored);
}
LONG WINAPI fault(EXCEPTION_POINTERS *p) {
    std::cerr << "Native charging fixture fault RVA=" << std::hex <<
        reinterpret_cast<uintptr_t>(p->ExceptionRecord->ExceptionAddress)-base <<
        " access=" << p->ExceptionRecord->ExceptionInformation[1] << std::endl;
    return EXCEPTION_EXECUTE_HANDLER;
}
int zero() { return 0; }
void nothing() {}
bool query_charger(void *,void *position,uintptr_t *out) {
    if (during_native_update) during_native_update(static_cast<uint8_t *>(position)-0xc8);
    *out=charger_present?charger:0; return charger_present;
}
void damage_during_update(void *entity) { put(entity,0x6e8,450.f); }
void change_actor_during_update(void *entity) {
    auto *actor=get<void *>(entity,0x8a8); put(actor,8,uint32_t{456});
}
bool unavailable() { return charger_unavailable; }
float wind(void *,void *,float *a,float *b) { *a=generator_power_zero?0.f:1.f; *b=*a; return 1; }
float phase() { return 1; }
void *resource() { return reinterpret_cast<void *>(vehicle_resource); }
void stub(uintptr_t rva,void *function) {
    void *unused{};
    check(MH_CreateHook(reinterpret_cast<void *>(base+rva),function,&unused)==MH_OK,"Could not prepare world fixture");
    check(MH_EnableHook(reinterpret_cast<void *>(base+rva))==MH_OK,"Could not activate world fixture");
}
void imports() {
    const auto *dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt=reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base+dos->e_lfanew);
    auto *descriptors=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    HMODULE kernel=GetModuleHandleW(L"kernel32.dll");
    unsigned count=0;
    for(;descriptors->Name;++descriptors) {
        auto *names=reinterpret_cast<IMAGE_THUNK_DATA64 *>(base+descriptors->OriginalFirstThunk);
        auto *slots=reinterpret_cast<IMAGE_THUNK_DATA64 *>(base+descriptors->FirstThunk);
        for(;names->u1.AddressOfData;++names,++slots) {
            if(IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto *name=reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base+names->u1.AddressOfData)->Name;
            if(std::strcmp(name,"TryAcquireSRWLockExclusive") && std::strcmp(name,"AcquireSRWLockExclusive") &&
               std::strcmp(name,"ReleaseSRWLockExclusive")) continue;
            auto function=GetProcAddress(kernel,name); check(function!=nullptr,"Missing Windows lock service");
            writable_put(reinterpret_cast<uintptr_t>(slots),reinterpret_cast<uintptr_t>(function)); ++count;
        }
    }
    check(count==3,"Did not resolve the three native lock imports");
}
using Entity=std::array<uint8_t,0xc00>;
using Actor=std::array<uint8_t,0x200>;
struct Result { Entity entity; Actor actor; };
struct Fixture {
    alignas(16) Entity entity{};
    alignas(16) Actor actor{};
    std::array<uint8_t,0x600> generator{},config{};
    std::array<uint8_t,0x200> manager{};
    std::array<uint8_t,0x200> resource_data{};
    std::array<uint8_t,0x40> world{};
    uintptr_t wind_service{};
    Fixture() {
        writable_put(base+0x7bf6e88,reinterpret_cast<uintptr_t>(world.data()));
        writable_put(base+0x7be6318,reinterpret_cast<uintptr_t>(world.data()));
        writable_put(base+0x7bfecc0,reinterpret_cast<uintptr_t>(manager.data()));
        writable_put(base+0x4c73850,reinterpret_cast<uintptr_t>(&wind_service));
        charger=reinterpret_cast<uintptr_t>(generator.data());
        vehicle_resource=reinterpret_cast<uintptr_t>(resource_data.data());
        put(resource_data.data(),0x138,10.f); // Ordinary passive recharge stays native.
    }
    void reset(uintptr_t type=0x3e26070) {
        entity.fill(0); actor.fill(0); generator.fill(0); config.fill(0); world.fill(0);
        charger_present=true; charger_unavailable=false; generator_power_zero=false; during_native_update=nullptr;
        put(actor.data(),0,base+type); put(actor.data(),8,uint32_t{123});
        put(actor.data(),0x20,uint8_t{1}); put(actor.data(),0x24,uint32_t{2});
        put(actor.data(),0x68,entity.data()+0x20);
        put(entity.data(),0x898,actor.data()+0xb0); put(entity.data(),0x8a8,actor.data());
        put(entity.data(),0x6e8,500.f); put(entity.data(),0x6ec,1000.f);
        put(entity.data(),0x8b0,uint32_t{0x1fffff});
        put(actor.data(),0xb8,100.f); put(actor.data(),0xbc,1000.f);
        put(actor.data(),0xf8,uint32_t{0x20}); // Native stationary/forced-stop state.
        put(generator.data(),0x84,uint32_t{1}); put(generator.data(),0x90,config.data());
        put(generator.data(),0x52c,100000.f); put(config.data(),0x4b4,100.f);
        // Distinct cargo/position/corrosion/velocity bytes must survive exactly.
        put(actor.data(),0x140,uintptr_t{0x123456789});
        put(entity.data(),0xc8,std::array<double,3>{1234,5678,9012});
        put(entity.data(),0x580,std::array<float,4>{11,22,33,44});
        put(entity.data(),0x6f4,.7f);
    }
    void update(float dt) {
        reinterpret_cast<void(*)(void *,float,void *,void *)>(base+qs::charging_repair_detail::battery_update_rva)
            (actor.data()+0xb0,dt,entity.data(),nullptr);
    }
    Result result() const { return {entity,actor}; }
};
void unchanged_except_condition(Result expected,Result actual) {
    const auto condition=get<float>(expected.entity.data(),0x6e8);
    put(actual.entity.data(),0x6e8,condition);
    check(expected.entity==actual.entity && expected.actor==actual.actor,
        "Repair changed native battery, cargo, physics, maximum, damage or corrosion bytes");
}
}
int wmain(int argc,wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX); SetUnhandledExceptionFilter(fault);
    try {
        check(argc==2,"Pass the supported ds.exe as the only argument");
        const qs::Digest expected{0x9d,0x53,0x06,0xd7,0xeb,0x2f,0x5d,0x31,0x35,0xc6,0x3a,0xcc,0xc3,0xe7,0x4b,0x8f,
            0x9b,0x02,0x9f,0x9d,0x76,0x11,0x2a,0x6a,0xae,0x4a,0x38,0xfd,0xbb,0x4e,0xdd,0x2b};
        check(qs::sha256(qs::read_file(argv[1]))==expected,"Unsupported executable hash");
        base=reinterpret_cast<uintptr_t>(LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES));
        check(base!=0,"Could not map original code"); imports();
        check(MH_Initialize()==MH_OK,"Could not initialize isolated hooks");
        stub(0x2f6be90,reinterpret_cast<void *>(zero));
        stub(0x2c63930,reinterpret_cast<void *>(query_charger));
        stub(0x19328c0,reinterpret_cast<void *>(nothing));
        stub(0x2c69110,reinterpret_cast<void *>(unavailable));
        stub(0x1a03f20,reinterpret_cast<void *>(nothing));
        stub(0x1943a30,reinterpret_cast<void *>(nothing));
        stub(0x2e0c890,reinterpret_cast<void *>(wind));
        stub(0x2d02310,reinterpret_cast<void *>(phase));
        stub(0x1dde990,reinterpret_cast<void *>(resource));
        Fixture fixture;
        fixture.reset(); fixture.update(.1f); const auto baseline=fixture.result();
        check(get<float>(baseline.actor.data(),0xb8)>100.f && baseline.actor[0xc4]==1,
            "Fixture did not execute the original native charger transfer");
        std::cout << "Original vehicle battery update and generator transfer exercised.\n" << std::flush;
        qs::charging_repair_detail::install(base);
        fixture.reset(); fixture.update(.1f);
        check(fixture.entity==baseline.entity && fixture.actor==baseline.actor,"Disabled hook changed native behavior");
        qs::set_charging_repair_enabled(true);
        for(uintptr_t type : {uintptr_t{0x3e26070},uintptr_t{0x3e26a48},uintptr_t{0x3e27790}}) {
            fixture.reset(type); fixture.update(.1f);
            check(std::abs(get<float>(fixture.entity.data(),0x6e8)-500.2f)<0.0001f,"Vehicle type did not repair at selected rate");
            auto expected_result=baseline; put(expected_result.actor.data(),0,base+type);
            unchanged_except_condition(expected_result,fixture.result());
        }
        for(unsigned hz : {30u,60u,144u}) {
            fixture.reset();
            for(unsigned i=0;i<5*hz;++i) fixture.update(1.f/hz);
            check(std::abs(get<float>(fixture.entity.data(),0x6e8)-510.f)<0.03f,"Native hook rate varies with frequency");
        }
        fixture.reset(); fixture.update(.1f);
        const auto repaired=get<float>(fixture.entity.data(),0x6e8);
        charger_present=false; fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==repaired,"Leaving charger still repaired");
        check(get<float>(fixture.actor.data(),0xb8)>110.f,"Passive recharge branch was not exercised");
        charger_present=true; fixture.update(.1f);
        check(std::abs(get<float>(fixture.entity.data(),0x6e8)-500.4f)<0.0001f,"Restart granted old time");
        fixture.reset(); charger_unavailable=true; fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Unavailable generator repaired");
        fixture.reset(); put(fixture.actor.data(),0xb8,1000.f); fixture.update(.1f);
        check(std::abs(get<float>(fixture.entity.data(),0x6e8)-500.2f)<0.0001f,
              "Full battery at a working generator did not repair");
        fixture.reset(); put(fixture.actor.data(),0xb8,999.f);
        for(unsigned i=0;i<300;++i) fixture.update(1.f/60.f);
        check(std::abs(get<float>(fixture.entity.data(),0x6e8)-510.f)<0.03f,
              "Repair stopped when the generator filled the battery");
        const auto no_power = qs::charging_repair_status().reasons[static_cast<size_t>(qs::ChargingRepairReason::charger_unavailable)];
        for (unsigned variant=0; variant<4; ++variant) {
            fixture.reset(); put(fixture.actor.data(),0xb8,1000.f);
            if (variant==0) charger_unavailable=true;
            if (variant==1) put(fixture.generator.data(),0xa8,uint8_t{0x80});
            if (variant==2) put(fixture.config.data(),0x4b4,0.f);
            if (variant==3) put(fixture.config.data(),0x4b4,std::numeric_limits<float>::quiet_NaN());
            fixture.update(.1f);
            check(get<float>(fixture.entity.data(),0x6e8)==500.f,
                  "Full battery at an unavailable/disabled/invalid-rate generator repaired");
        }
        check(qs::charging_repair_status().reasons[static_cast<size_t>(qs::ChargingRepairReason::charger_unavailable)]==no_power+4,
              "Generator rejection reasons were not recorded");
        fixture.reset(); put(fixture.actor.data(),0xb8,1000.f); charger_present=false; fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Leaving the generator with a full battery still repaired");
        fixture.reset(); put(fixture.actor.data(),0xe8,20.f); fixture.update(.1f);
        check(get<float>(fixture.actor.data(),0xb8)<100.f &&
              std::abs(get<float>(fixture.entity.data(),0x6e8)-500.2f)<0.0001f,
              "Native consumption hid the working generator connection");
        fixture.reset(); generator_power_zero=true; fixture.update(.1f);
        check(get<float>(fixture.actor.data(),0xb8)==100.f && get<float>(fixture.entity.data(),0x6e8)==500.f,
              "Unfilled battery at a generator transferring no power repaired");
        fixture.reset(); put(fixture.generator.data(),0x84,uint32_t{3});
        put(fixture.config.data(),0x4bc,100.f); put(fixture.config.data(),0x4dc,1.5f);
        put(fixture.actor.data(),0xb8,1000.f); generator_power_zero=true; fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Overcharge allowance was mistaken for full battery");
        put(fixture.actor.data(),0xb8,1500.f); fixture.update(.1f);
        check(std::abs(get<float>(fixture.entity.data(),0x6e8)-500.2f)<0.0001f,
              "Native overcharge-full battery at available generator did not repair");
        for(float health : {0.f,-1.f,1000.f,1100.f}) {
            fixture.reset(); put(fixture.entity.data(),0x6e8,health); fixture.update(.1f);
            check(get<float>(fixture.entity.data(),0x6e8)==health,"Dead/full/overfull vehicle changed");
        }
        fixture.reset(); put(fixture.entity.data(),0x6f0,500.f); fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Fatal pending damage was defeated");
        fixture.reset(); put(fixture.entity.data(),0x6e8,999.99f); fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==1000.f,"Native hook overhealed");
        fixture.reset(); during_native_update=damage_during_update; fixture.update(.1f);
        check(std::abs(get<float>(fixture.entity.data(),0x6e8)-450.2f)<0.0001f,"Repair overwrote intervening native damage");
        fixture.reset(); during_native_update=change_actor_during_update; fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Replaced actor inherited repair");
        for(uint64_t flags : {qs::scene_bits,qs::transition_bits,uint64_t{4},uint64_t{0x10000000}}) {
            fixture.reset(); put(fixture.world.data(),8,flags); fixture.update(.1f);
            check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Paused/transition world repaired");
        }
        fixture.reset(0x1234); fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Unknown actor repaired");
        fixture.reset(); put(fixture.actor.data(),0x24,uint32_t{1}); fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Inactive vehicle repaired");
        fixture.reset(); put(fixture.actor.data(),0x68,uintptr_t{1}); fixture.update(.1f);
        check(get<float>(fixture.entity.data(),0x6e8)==500.f,"Broken lifetime link repaired");
        qs::set_charging_repair_enabled(false); fixture.reset(); fixture.update(.1f);
        check(fixture.entity==baseline.entity && fixture.actor==baseline.actor,"Switch-off failed to restore native behavior");
        qs::charging_repair_detail::install(base);
        const auto status=qs::charging_repair_status();
        check(status.ready && !status.enabled && status.repair_updates>1000 && status.excluded_updates>=8,"Telemetry/lifecycle mismatch");
        std::cout << "Native charging hook ABI, 30/60/144 Hz rate, on/off, stop/full/unavailable, lifetime gates, caps and unchanged actor bytes passed.\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
