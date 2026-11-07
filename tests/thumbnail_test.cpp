#include "thumbnail_image.hpp"
#include <iostream>
#include <stdexcept>
#include <algorithm>
using namespace qs;

static void check(bool ok,const char *msg) { if(!ok) throw std::runtime_error(msg); }
int main() {
    try {
        ThumbnailPixels expected{2,2,{255,0,0,255, 0,255,0,255, 0,0,255,255, 25,40,70,255}};
        const auto png=encode_thumbnail(expected);
        check(png.size()>30,"PNG encoder failed");
        auto decoded=decode_thumbnail(png);
        check(decoded.width==2 && decoded.height==2 && decoded.rgba==expected.rgba,"PNG round trip or channel order");
        Bytes native(24,0);
        auto len=[&](int index,size_t n){auto text=std::to_string(n);std::copy(text.begin(),text.end(),native.begin()+index*8);};
        len(0,6);len(1,png.size());len(2,1);
        native.insert(native.end(),{'[','S','l','o','t',']'});native.insert(native.end(),png.begin(),png.end());native.push_back(1);
        check(native_thumbnail(native)==png,"embedded checkpoint thumbnail extraction");
        native.pop_back();check(native_thumbnail(native).empty(),"incomplete native layout");
        check(native_thumbnail({}).empty() && decode_thumbnail({1,2,3}).rgba.empty(),"invalid preview accepted");
        auto bomb=png;bomb[16]=0x7f;check(decode_thumbnail(bomb).rgba.empty(),"excessive PNG dimensions accepted");
        check(encode_thumbnail({512,512,{1,2,3}}).empty(),"invalid source buffer accepted");
        std::cout<<"Verified PNG colors, native extraction, absence and bounds.\n";
    } catch(const std::exception &e) {std::cerr<<e.what()<<"\n";return 1;}
}
