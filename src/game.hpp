#pragma once
#include "telemetry.hpp"

namespace qs {
// Construction verifies the entire executable. Only then may callers access
// particular engine functions, on the regular game update thread.
class Game {
public:
    Game();
    std::uintptr_t base() const { return base_; }
    WorldSample sample() const;
    std::optional<std::uint64_t> state_flags() const;
    std::uintptr_t active_timer_world(bool &paused) const;
private:
    std::uintptr_t base_{};
};
}
