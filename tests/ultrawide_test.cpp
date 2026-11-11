#include "ultrawide_policy.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace qs::ultrawide_detail;
void check(bool condition, const char *why) { if (!condition) throw std::runtime_error(why); }
int main() {
    try {
        Options source{};
        for (size_t i=0;i<source.size();++i) source[i]=int32_t(i*17);
        for (const auto selected : {Size{1920,1080},Size{2520,1080},Size{3360,1440},Size{3840,2160}}) {
            source = with_size(source, selected);
            auto effective = with_size(source, target);
            check(size(source) == selected && size(effective) == target, "Resolution override changed its source");
            for (size_t i=2;i<source.size();++i) check(source[i] == effective[i], "An unrelated graphics option changed");
            check(with_size(effective,selected) == source, "Restoring selected dimensions lost settings");
            check(unpack(pack(selected)) == selected, "Stored selection changed dimensions");
        }
        check(full_image(target,target,aspect), "Exact native image rejected");
        check(!full_image({3360,1440},target,aspect), "Pillarboxed image falsely confirmed");
        check(!full_image(target,{1920,1080},aspect), "Wrong viewport falsely confirmed");
        check(!full_image(target,target,7.f/3.f), "21:9 crop falsely confirmed");
        check(!full_image(target,target,std::numeric_limits<float>::quiet_NaN()), "Invalid aspect accepted");
        std::array<uint8_t,40> snapshot; snapshot.fill(0x55);
        const auto original=snapshot;
        bool comparison=true;
        adjust_snapshot(snapshot.data(),0x2642f0d,true,{1920,1080},comparison);
        check(snapshot==original && comparison, "DLAA's poll snapshot was modified by ultrawide");
        adjust_snapshot(snapshot.data(),0x2645034,true,{1920,1080},comparison);
        auto expected=original; const Size normal{1920,1080}; std::memcpy(expected.data(),&normal,sizeof normal);
        check(snapshot==expected && comparison, "Config snapshot changed unrelated options");
        snapshot=original;
        adjust_snapshot(snapshot.data(),0x26447e6,true,normal,comparison);
        expected=original; const uint32_t zero=0; std::memcpy(expected.data(),&zero,4);
        check(snapshot==expected && !comparison, "Comparison invalidation is not single-use");
        snapshot=original;
        adjust_snapshot(snapshot.data(),0x26447e6,true,normal,comparison);
        check(snapshot==original, "A later ordinary read was invalidated");
        adjust_snapshot(snapshot.data(),0x2645034,false,normal,comparison);
        check(snapshot==original, "Disabled snapshot changed settings");
        RepairBudget budget;
        budget.observe(true,false); check(!budget.take(100), "Background resize allowed");
        budget.observe(true,true); check(budget.take(100), "Initial resize missing");
        check(!budget.take(101) && budget.take(1100) && budget.take(2100), "Retry spacing failed");
        check(!budget.take(3100), "GPU rebuild loop was not bounded");
        budget.observe(true,true,1);
        check(budget.take(3101), "Focus return was missed while native polling was suspended");
        budget.observe(true,false); budget.observe(true,true);
        check(budget.take(3200), "Alt+Tab return did not rearm");
        budget.observe(false,true); check(budget.take(3201), "Disabling cannot restore immediately");
        std::cout << "Resolution override, selected-size restoration, full-image checks and focus retry budget passed.\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
