#pragma once
#include <cstdint>
namespace qs {
// Native load completion provides the arrival position immediately. Other
// systems can still be populating, so check order/cargo after a settled window.
class PostLoadCheck {
public:
    enum class State { waiting, sample, timed_out };
    void start(std::uint64_t now) { until_=now+8000; clear_since_=0; }
    State observe(std::uint64_t now,bool world_ready) {
        if(now>=until_) return State::timed_out;
        if(!world_ready) clear_since_=0;
        else if(!clear_since_) clear_since_=now;
        if(clear_since_ && now-clear_since_>=250) {
            clear_since_=now; return State::sample;
        }
        return State::waiting;
    }
private:
    std::uint64_t until_{},clear_since_{};
};
}
