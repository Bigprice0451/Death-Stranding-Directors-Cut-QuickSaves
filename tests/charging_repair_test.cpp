#include "charging_repair_policy.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value, const char *why) { if (!value) throw std::runtime_error(why); }
qs::charging_repair_detail::Sample sample() { return {true, true, 0.1f, 100.f, 101.f, 500.f, 1000.f, 0.f}; }
}
int main() {
    using qs::charging_repair_detail::repaired_condition;
    try {
        // One second split into different simulation steps must give the same rate.
        for (unsigned hz : {20u, 30u, 60u, 144u, 240u}) {
            auto s = sample(); s.dt = 1.f / hz;
            for (unsigned i=0; i<5*hz; ++i) s.condition = repaired_condition(s);
            check(std::abs(s.condition - 510.f) < 0.03f, "Repair is not 1% maximum per five seconds");
        }
        auto s = sample();
        check(repaired_condition(s) > s.condition, "Real charging did not repair");
        s.enabled = false; check(repaired_condition(s)==s.condition, "Disabled feature repaired");
        s = sample(); s.active_charge = false;
        check(repaired_condition(s)==s.condition, "Passive battery recharge repaired");
        for (float after : {99.f, 100.f}) {
            s=sample(); s.battery_after=after;
            check(repaired_condition(s)==s.condition, "No actual transfer still repaired");
        }
        // No stored time: leaving the charger, a full battery, and toggling off
        // cannot bank an interval that is awarded on the next charging frame.
        s=sample(); const auto once=repaired_condition(s); s.condition=once;
        for (unsigned i=0; i<10000; ++i) { s.active_charge=false; s.condition=repaired_condition(s); }
        check(s.condition==once, "Noncharging time accumulated");
        s.active_charge=true; s.condition=repaired_condition(s);
        check(std::abs(s.condition-500.4f)<0.0001f, "Charging restart granted catch-up repair");
        for (float current : {0.f,-1.f,1000.f,1100.f}) {
            s=sample(); s.condition=current;
            check(repaired_condition(s)==current, "Dead/full/overfull vehicle was changed");
        }
        s=sample(); s.condition=999.99f;
        check(repaired_condition(s)==1000.f, "Repair exceeded maximum condition");
        s=sample(); s.pending_damage=500.f;
        check(repaired_condition(s)==s.condition, "Pending fatal damage was defeated");
        s.pending_damage=50.f;
        check(repaired_condition(s)>s.condition && s.pending_damage==50.f, "Ordinary pending damage was modified");
        for (float dt : {0.f,-0.1f,2.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            s=sample(); s.dt=dt;
            check(repaired_condition(s)==s.condition, "Invalid/discontinuous time was accepted");
        }
        for (int field=0; field<5; ++field) {
            s=sample();
            float *values[]{&s.battery_before,&s.battery_after,&s.maximum,&s.pending_damage,&s.condition};
            *values[field]=std::numeric_limits<float>::quiet_NaN();
            const auto result=repaired_condition(s);
            check(field==4 ? std::isnan(result) : result==s.condition, "Invalid game state was accepted");
        }
        s=sample(); s.maximum=0; check(repaired_condition(s)==s.condition, "Zero maximum was accepted");
        std::cout << "Charging gates, 20/30/60/144/240 Hz rate, stop/restart, caps and fatal damage passed.\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
