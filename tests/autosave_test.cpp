#include "autosave.hpp"
#include "post_load_check.hpp"
#include <iostream>
#include <stdexcept>
namespace {
void require(bool state,const char* message){if(!state)throw std::runtime_error(message);}
}
int main() {
    using namespace qs;
    try {
        AutoClock clock;
        require(!clock.enabled() && clock.minutes()==10,"wrong defaults");
        clock.observe(10,1);
        require(!clock.take(600010),"disabled clock fired");
        clock.configure(true,0);
        require(clock.minutes()==1,"minimum interval");
        for(unsigned t=0;t<=60000;t+=500) clock.observe(t,1);
        require(clock.take(60000)==1 && !clock.take(60001),"one due observation");
        clock.observe(60500,0);
        require(clock.remaining_seconds(60500)==-1,"clock stayed armed in a cutscene");
        for(unsigned t=70000;t<130000;t+=500) { clock.observe(t,2); require(!clock.take(t),"fired early after transition"); }
        clock.observe(130000,2); require(clock.take(130000)==2,"never resumed in new world");
        clock.observe(250000,2); require(!clock.take(250000),"catch-up after suspension");
        clock.configure(true,100); require(clock.minutes()==60 && !clock.take(999999),"maximum interval/settings reset");
        clock.configure(true,1);
        for(unsigned t=300000;t<=360000;t+=500) clock.observe(t,2);
        require(!clock.take(362000),"expired observation accepted");
        clock.reset(); clock.observe(400000,1);
        for(unsigned t=400500;t<=410000;t+=500) clock.observe(t,1);
        for(unsigned t=410500;t<=475000;t+=500) clock.observe(t,1,true);
        require(!clock.take(475000) && clock.remaining_seconds(475000)==50,"ordinary pause changed the time left");
        clock.observe(475500,1);
        require(clock.remaining_seconds(475500)==50,"unpause lost the previous timer");
        require(save_hold(0,false)==SaveHold::none && save_hold(0,true)==SaveHold::cutscene,"live cinematic clock");
        for(auto flag:{0x40ull,0x40000ull,0x80000ull,0x100000ull,0x400000ull<<32})
            require(save_hold(flag,false)==SaveHold::cutscene,"native cinematic flag");
        require(save_hold(0x10400000,false)==SaveHold::transition && save_hold({},false)==SaveHold::transition,"transition/unknown");
        PostLoadCheck wait;
        using S=PostLoadCheck::State;
        wait.start(1000);
        require(wait.observe(4000,false)==S::waiting && wait.observe(4050,true)==S::waiting,"compared during load");
        require(wait.observe(4100,false)==S::waiting && wait.observe(4150,true)==S::waiting,"did not reset settle interval");
        require(wait.observe(4399,true)==S::waiting && wait.observe(4400,true)==S::sample,"settled observation timing");
        require(wait.observe(4500,true)==S::waiting && wait.observe(9000,false)==S::timed_out,"timeout or retry");
        std::cout<<"Timed saving, cinematic holds and deferred delivery checks passed.\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
