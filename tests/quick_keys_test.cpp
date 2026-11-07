#include "key_bindings.hpp"
#include <iostream>
#include <stdexcept>
using namespace qs;
void check(bool yes){if(!yes)throw std::runtime_error("Hotkey regression");}
int main(){
    try {
        KeyBindings k;check(k.keys==std::vector<unsigned>{VK_F5});
        check(k.assign({},'C')&&k.assign({},'B')&&!k.assign({},'B'));
        for(auto key:{unsigned(VK_F5),unsigned('C'),unsigned('B')})
            check(k.pressed([&](unsigned value){return value==key;}));
        check(k.assign(0,VK_F8)&&!k.assign(2,'C')&&!k.assign({},VK_ESCAPE));
        check(KeyBindings::decode(k.encode()).keys==k.keys);
        check(!KeyBindings::decode("none").pressed([](unsigned){return true;}));
        check(KeyBindings::decode("116x 3").keys==KeyBindings{}.keys);
        check(KeyBindings::decode("9999").keys==KeyBindings{}.keys);
        check(KeyBindings::decode("67 67").keys==KeyBindings{}.keys);
        return 0;
    }catch(const std::exception&e){std::cerr<<e.what();return 1;}
}
