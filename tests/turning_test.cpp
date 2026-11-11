#include "turning_policy.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace qs::turning_detail;
void check(bool condition, const char *why) { if (!condition) throw std::runtime_error(why); }
using Mover = std::array<uint8_t, 0x800>;
void put(Mover &mover, size_t offset, float value) { std::memcpy(mover.data() + offset, &value, sizeof value); }
float get(const Mover &mover, size_t offset) { float f{}; std::memcpy(&f, mover.data() + offset, sizeof f); return f; }
}
int main() {
    try {
        Mover mover; mover.fill(0x56);
        put(mover, facing_factor, .2f); put(mover, heading_factor, .3f); put(mover, secondary_factor, .4f);
        const auto saved = mover;
        { ScopedResponse off(mover.data(), false, true); check(mover == saved, "Off changed memory"); }
        {
            ScopedResponse heading(mover.data(), true, true);
            check(heading.changed() == 6 && get(mover, facing_factor) == .2f && get(mover, heading_factor) == 1.f &&
                  get(mover, secondary_factor) == 1.f, "Heading scope affects the wrong fields");
            auto expected = saved;
            put(expected, heading_factor, 1); put(expected, secondary_factor, 1);
            check(mover == expected, "Heading changed unrelated movement data");
            {
                ScopedResponse facing(mover.data(), true, false);
                check(facing.changed() == 1 && get(mover, facing_factor) == 1, "Facing was not neutralized");
            }
            check(mover == expected, "Nested facing restoration failed");
        }
        check(mover == saved, "Original values were not restored");
        try { ScopedResponse response(mover.data(), true, false); throw 1; } catch(int) {}
        check(mover == saved, "Exception left modified game state");
        for (float factor : {0.f, -0.f, -1.f, 1.f, 2.f, std::numeric_limits<float>::infinity(),
                             std::numeric_limits<float>::quiet_NaN()}) {
            put(mover, facing_factor, factor);
            const auto before = mover;
            { ScopedResponse scope(mover.data(), true, false); check(!scope.changed() && mover == before, "Lock/boost/invalid value changed"); }
            check(mover == before, "Pass-through value was rewritten");
        }
        std::cout << "Angular scopes, exact restoration, locks, boosts and unrelated bytes passed.\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
