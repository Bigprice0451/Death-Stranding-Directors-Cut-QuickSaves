#pragma once
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace qs {
// Each number is an independent Windows virtual key. Spaces keep the list
// a single ReShade config value. "none" preserves an intentionally empty set.
inline bool bindable_key(unsigned key) {
    return key>=VK_BACK && key<=0xfe && key!=VK_ESCAPE && key!=VK_LWIN && key!=VK_RWIN &&
           key!=VK_PROCESSKEY && key!=VK_PACKET && !(key>=VK_GAMEPAD_A && key<=VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT);
}
struct KeyBindings {
    std::vector<unsigned> keys{VK_F5};
    bool assign(std::optional<size_t> index,unsigned key) {
        if(!bindable_key(key) || (index && *index>=keys.size())) return false;
        auto old=std::find(keys.begin(),keys.end(),key);
        if(old!=keys.end() && (!index || size_t(old-keys.begin())!=*index)) return false;
        if(index) keys[*index]=key; else keys.push_back(key);
        return true;
    }
    std::string encode() const {
        if(keys.empty()) return "none";
        std::string result;
        for(auto key:keys) {if(!result.empty()) result+=' ';result+=std::to_string(key);}
        return result;
    }
    static KeyBindings decode(std::string_view text) {
        KeyBindings result;
        if(text.empty()) return result;
        result.keys.clear();
        if(text=="none") return result;
        while(!text.empty()) {
            size_t end=text.find(' ');
            auto part=text.substr(0,end);unsigned key{};
            auto n=std::from_chars(part.data(),part.data()+part.size(),key);
            if(part.empty() || n.ec!=std::errc{} || n.ptr!=part.data()+part.size() || !result.assign({},key))
                return KeyBindings{};
            if(end==text.npos) break;
            text.remove_prefix(end+1);
        }
        return result;
    }
    template<class Pressed> bool pressed(Pressed test) const {
        return std::any_of(keys.begin(),keys.end(),test);
    }
};
}
