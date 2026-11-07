#pragma once
#include <cstdint>
#include <string>

namespace qs {
struct DlaaStatus {
    bool requested{}, hooks_ready{}, preset_requested{}, evaluated{};
    unsigned input_width{}, input_height{}, output_width{}, output_height{};
    std::string detail;
};
// Caller already verified ds.exe's complete hash via Game. Startup and shutdown
// must run outside DllMain. Only the user's preference is changed from the UI.
void start_dlaa_tracking(uintptr_t verified_game_base, bool enabled);
void stop_dlaa_tracking();
void set_dlaa_enabled(bool enabled);
DlaaStatus dlaa_status();
}
