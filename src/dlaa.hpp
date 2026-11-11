#pragma once
#include <cstdint>
#include <string>

namespace qs {
struct DlaaStatus {
    bool requested{}, hooks_ready{}, preset_requested{}, evaluated{};
    unsigned input_width{}, input_height{}, output_width{}, output_height{};
    std::string detail;
    uint64_t evaluations{}, evaluation_failures{}, creations{}, releases{}, last_evaluation_at{};
    uint64_t history_resets{};
    bool last_frame_reset{};
    uint32_t last_result{};
    bool refresh_pending{};
    unsigned refresh_nudges{};
};
// Caller already verified ds.exe's complete hash via Game. Startup and shutdown
// must run outside DllMain. Only the user's preference is changed from the UI.
void start_dlaa_tracking(uintptr_t verified_game_base, bool enabled);
void stop_dlaa_tracking();
void set_dlaa_enabled(bool enabled);
DlaaStatus dlaa_status();
}
