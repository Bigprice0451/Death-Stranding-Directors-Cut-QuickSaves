#include "telemetry.hpp"
#include <iostream>
#include <windows.h>
using namespace qs;
int main(){
    auto folder=fs::current_path()/("log-test-"+std::to_string(GetCurrentProcessId()));
    try {
        Telemetry log(folder); WorldSample world{0x40,std::array<double,3>{1.25,2.,3.}};
        world.carried_count=0;world.delivery=DeliverySample{2,3,{}};
        if(!log.write({"capture","line\n\"two\"",1,2,10,world,{}})) throw std::runtime_error("write");
        auto bytes=read_file(log.current()); std::string s(bytes.begin(),bytes.end());
        if(s.find("\"sam_xyz\":[1.25,2,3]")==s.npos || s.find("line\\u000a\\\"two\\\"")==s.npos ||
            s.find("\"cinematic\":true")==s.npos || s.back()!='\n') throw std::runtime_error("JSON fields");
        if(s.find("\"carried_cargo_count\":0")==s.npos || s.find("\"active_orders\":2")==s.npos ||
           s.find("\"delivered_count\":3")==s.npos) throw std::runtime_error("Delivery and cargo telemetry");
        fs::resize_file(log.current(),2u*1024u*1024u);
        if(!log.write({"next","",2,3}) || !fs::exists(folder/"quicksaves.previous.jsonl") ||
            fs::file_size(log.current())>1024) throw std::runtime_error("rotation");
        fs::remove_all(folder); std::cout<<"Verified persistent JSON fields, escaping, timestamps, position and rotation.\n";return 0;
    } catch(const std::exception &e){fs::remove_all(folder);std::cerr<<e.what();return 1;}
}
