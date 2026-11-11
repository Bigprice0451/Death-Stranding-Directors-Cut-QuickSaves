#pragma once
#include "ultrawide_policy.hpp"
#include <string>

namespace qs {
struct UltrawideStatus {
    bool ready{}, requested{}, restoring{}, focused{}, monitor_matches{}, measured{};
    ultrawide_detail::Size selected{}, render{}, viewport{};
    float aspect{};
    unsigned attempts{};
    uint64_t repairs{}, last_sample{};
    std::string error;
};
void start_ultrawide(uintptr_t verified_game, void *window, bool wanted);
void set_ultrawide_enabled(bool wanted);
UltrawideStatus ultrawide_status();
}
