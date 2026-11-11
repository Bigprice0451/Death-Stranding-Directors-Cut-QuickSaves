#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace qs::turning_detail {
inline constexpr uintptr_t heading_rva = 0x26d5bc0, facing_rva = 0x268b0e0;
inline constexpr size_t facing_factor = 0x314, heading_factor = 0x318, secondary_factor = 0x31c;

// One is the mover's constructor/reset value. Preserve explicit turn locks,
// non-finite values and authored boosts. This does not change cargo mass.
inline float normal_response(float factor) {
    return std::isfinite(factor) && factor > 0.f && factor < 1.f ? 1.f : factor;
}

// Only the angular calculation sees these values. Restore the exact bytes
// before returning to the movement update, including C++ exception unwinding.
class ScopedResponse {
public:
    ScopedResponse(void *mover, bool apply, bool heading) : mover_(static_cast<uint8_t *>(mover)) {
        if (!apply) return;
        for (size_t i = heading ? 1 : 0; i < (heading ? 3u : 1u); ++i) {
            auto *location = mover_ + facing_factor + i * sizeof(float);
            float original{};
            std::memcpy(&original, location, sizeof original);
            const float replacement = normal_response(original);
            if (!(replacement == 1.f && original > 0.f && original < 1.f)) continue;
            std::memcpy(&saved_[i], location, sizeof(float));
            std::memcpy(location, &replacement, sizeof replacement);
            changed_ |= 1u << i;
        }
    }
    ~ScopedResponse() {
        for (size_t i = 0; i < saved_.size(); ++i)
            if (changed_ & (1u << i))
                std::memcpy(mover_ + facing_factor + i * sizeof(float), &saved_[i], sizeof(float));
    }
    ScopedResponse(const ScopedResponse &) = delete;
    ScopedResponse &operator=(const ScopedResponse &) = delete;
    unsigned changed() const { return changed_; }
private:
    uint8_t *mover_;
    std::array<uint32_t, 3> saved_{};
    unsigned changed_{};
};
}
