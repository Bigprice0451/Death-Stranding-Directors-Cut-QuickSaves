#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace qs::ultrawide_detail {
struct Size {
    uint32_t width{}, height{};
    bool operator==(const Size &other) const { return width == other.width && height == other.height; }
    bool operator!=(const Size &other) const { return !(*this == other); }
    bool valid() const { return width >= 320 && height >= 200 && width <= 16384 && height <= 16384; }
};
inline constexpr Size target{3440, 1440};
inline constexpr float aspect = 3440.f / 1440.f;
// Native graphics request: 34 signed integers. Only the first two are size.
using Options = std::array<int32_t, 34>;
inline Size size(const Options &options) { return {uint32_t(options[0]), uint32_t(options[1])}; }
inline Options with_size(const Options &source, Size wanted) {
    auto copy = source;
    copy[0] = int32_t(wanted.width); copy[1] = int32_t(wanted.height);
    return copy;
}
inline bool full_image(Size render, Size viewport, float ratio) {
    return render == target && viewport == target && std::isfinite(ratio) && std::abs(ratio - aspect) < .0001f;
}
inline uint64_t pack(Size value) { return uint64_t(value.width) | (uint64_t(value.height) << 32); }
inline Size unpack(uint64_t value) { return {uint32_t(value), uint32_t(value >> 32)}; }

inline void adjust_snapshot(void *copy, uintptr_t caller_rva, bool owns, Size normal, bool &comparison) {
    if (comparison && caller_rva == 0x26447e6) {
        const uint32_t different=0;
        std::memcpy(copy,&different,sizeof different);
        comparison=false;
    } else if (owns && normal.valid() && caller_rva == 0x2645034) {
        std::memcpy(copy,&normal,sizeof normal);
    }
}

// A rejected resize must not flush the GPU every frame. Focus return, a new
// toggle or a normal game-settings request opens a new bounded opportunity.
class RepairBudget {
public:
    void observe(bool wanted, bool focused, uint64_t focus_generation = 0) {
        if (wanted != wanted_ || (focused && !focused_) || focus_generation != focus_generation_) reset();
        wanted_ = wanted; focused_ = focused;
        focus_generation_ = focus_generation;
    }
    void reset() { attempts_ = 0; next_ = 0; }
    bool take(uint64_t now) {
        if (!focused_ || attempts_ >= 3 || now < next_) return false;
        ++attempts_; next_ = now + 1000; return true;
    }
    unsigned attempts() const { return attempts_; }
private:
    bool wanted_{}, focused_{};
    unsigned attempts_{};
    uint64_t next_{};
    uint64_t focus_generation_{};
};
}
