#pragma once
#include "dlaa.hpp"
#include <sstream>

namespace qs {
// Kept across swapchain recreation so a resize loop cannot flood the log.
// No graphics work or disk writes happen in the NGX callbacks; the overlay
// collects these counters at this bounded interval, even with DLAA switched off.
class GraphicsDiagnosticClock {
public:
    bool take(uint64_t now) {
        if (started_ && now < next_) return false;
        started_=true; next_=now+15000; return true;
    }
private:
    bool started_{};
    uint64_t next_{};
};
inline std::string graphics_diagnostic_text(const DlaaStatus &s, uint64_t now) {
    std::ostringstream out;
    out << "requested=" << s.requested << "; ready=" << s.hooks_ready
        << "; evaluated=" << s.evaluated << "; K=" << s.preset_requested
        << "; input=" << s.input_width << 'x' << s.input_height
        << "; output=" << s.output_width << 'x' << s.output_height
        << "; evaluations=" << s.evaluations << "; failures=" << s.evaluation_failures
        << "; history_resets=" << s.history_resets << "; last_frame_reset=" << s.last_frame_reset
        << "; creations=" << s.creations << "; releases=" << s.releases
        << "; result=0x" << std::hex << s.last_result << std::dec
        << "; refresh_pending=" << s.refresh_pending << "; nudges=" << s.refresh_nudges
        << "; evaluation_age_ms=";
    if (s.last_evaluation_at && now>=s.last_evaluation_at) out << now-s.last_evaluation_at;
    else out << "unavailable";
    out << "; status=" << s.detail;
    return out.str();
}
}
