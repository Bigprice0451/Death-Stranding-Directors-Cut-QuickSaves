#pragma once
#include <cstdint>

namespace qs {
struct TurningStatus {
    bool ready{}, enabled{};
    uint64_t eligible_calls{}, heading_changes{}, facing_changes{}, excluded_calls{};
};
// Call after Game has verified the full executable identity. Hooks remain
// pinned through swapchain recreation; disabling makes them pass through.
void start_turning(uintptr_t verified_game, bool enabled);
void set_turning_enabled(bool enabled);
TurningStatus turning_status();

namespace turning_detail {
// Also used by the isolated native-code fixture. Never accepts an unverified
// image from the overlay or configuration.
void install(uintptr_t verified_image);
}
}
