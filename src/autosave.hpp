#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace qs {
enum class SaveHold { none, cutscene, transition };
// The engine packs its two script flag words into one 64-bit state value.
// First-word Cutscene, Movie, InGameCutscene and InGameActing; second-word
// HeavyCutscene and EndingStaffRoll. Loading is a first-word transition.
constexpr std::uint64_t scene_bits = 0x40 | 0x40000 | 0x80000 | 0x100000 | (0x600000ull << 32);
constexpr std::uint64_t transition_bits = 0x400000 | 0x4000000 | 0x20000000;
inline SaveHold save_hold(std::optional<std::uint64_t> flags, bool live_scene) {
    if (live_scene || (flags && (*flags & scene_bits))) return SaveHold::cutscene;
    if (!flags || (*flags & transition_bits)) return SaveHold::transition;
    return SaveHold::none;
}

// The game thread observes a loaded world twice a second. The render thread
// can take one due observation for its thumbnail, while the same world is fresh.
// Session protects this object with its existing mutex.
class AutoClock {
public:
    bool enabled() const { return enabled_; }
    int minutes() const { return minutes_; }
    int remaining_seconds(std::uint64_t now) const {
        if (!world_ || !enabled_ || now-last_>3000) return -1;
        const auto at=paused_?last_:now;
        return due_<=at ? 0 : static_cast<int>((due_-at+999)/1000);
    }
    void configure(bool enabled, int minutes) {
        minutes=std::clamp(minutes,1,60);
        if (enabled_!=enabled || minutes_!=minutes) reset();
        enabled_=enabled; minutes_=minutes;
    }
    void reset() { world_=0; last_=due_=0; paused_=false; }
    void observe(std::uint64_t now,std::uintptr_t world,bool paused=false) {
        if (!enabled_ || !world) {reset();return;}
        // Lost focus, suspended computer, or an unobserved streaming interval:
        // a late frame must never trigger an old save in a different scene.
        if (world_!=world || now-last_>3000) due_=now+interval();
        else if(paused_ || paused) due_+=now-last_;
        last_=now; world_=world; paused_=paused;
    }
    std::uintptr_t take(std::uint64_t now) {
        if (!enabled_ || !world_ || paused_ || now-last_>1500 || now<due_) return 0;
        due_=now+interval();
        return world_;
    }
private:
    std::uint64_t interval() const { return static_cast<std::uint64_t>(minutes_)*60000; }
    bool enabled_{},paused_{};
    int minutes_=10;
    std::uintptr_t world_{};
    std::uint64_t last_{},due_{};
};
}
