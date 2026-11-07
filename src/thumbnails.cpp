#include <windows.h>
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "thumbnails.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace qs {
namespace {
namespace api=reshade::api;
using Runtime=api::effect_runtime;
using Format=api::format;
using Color=api::color_space;
struct Output { void *window{}; Color color=Color::unknown; };
thread_local Output output;
void on_swapchain_present(api::command_queue *,api::swapchain *swap,const api::rect *,const api::rect *,uint32_t,const api::rect *) {
    output={swap->get_hwnd(),swap->get_color_space()};
}
bool supported(Format f) {
    switch(f) {
    case Format::r8g8b8a8_unorm: case Format::r8g8b8x8_unorm:
    case Format::b8g8r8a8_unorm: case Format::b8g8r8x8_unorm:
    case Format::r10g10b10a2_unorm: case Format::b10g10r10a2_unorm:
    case Format::r16g16b16a16_float: return true;
    default:return false;
    }
}
float half_value(uint16_t half) {
    const int exp=(half>>10)&31;
    if(half&0x8000) return 0.f;
    if(exp==31) return 0.f;
    if(!exp) return std::ldexp(float(half&1023),-24);
    return std::ldexp(1.f+float(half&1023)/1024.f,exp-15);
}
float pq_to_linear(float signal) {
    // SMPTE ST 2084 outputs 0..10000 nits; use 203 nits as SDR reference white.
    constexpr float m1=2610.f/16384.f,m2=2523.f/32.f,c1=3424.f/4096.f,c2=2413.f/128.f,c3=2392.f/128.f;
    const float v=std::pow(std::max(signal,0.f),1.f/m2);
    return (10000.f/203.f)*std::pow(std::max(v-c1,0.f)/(c2-c3*v),1.f/m1);
}
uint8_t channel(float value,bool linear) {
    if(linear) value=value<=0.0031308f ? value*12.92f : 1.055f*std::pow(std::max(value,0.f),1.f/2.4f)-0.055f;
    return static_cast<uint8_t>(std::lround(std::clamp(value,0.f,1.f)*255.f));
}
void convert_pixel(const uint8_t *source,Format f,Color color,uint8_t *dest) {
    float r,g,b;
    if(f==Format::r16g16b16a16_float) {
        uint16_t v[3];std::memcpy(v,source,sizeof v);
        r=half_value(v[0]);g=half_value(v[1]);b=half_value(v[2]);
    } else if(f==Format::r10g10b10a2_unorm||f==Format::b10g10r10a2_unorm) {
        uint32_t v;std::memcpy(&v,source,sizeof v);
        r=(v&1023)/1023.f;g=((v>>10)&1023)/1023.f;b=((v>>20)&1023)/1023.f;
        if(f==Format::b10g10r10a2_unorm) std::swap(r,b);
    } else {
        const bool bgra=f==Format::b8g8r8a8_unorm||f==Format::b8g8r8x8_unorm;
        dest[0]=source[bgra?2:0];dest[1]=source[1];dest[2]=source[bgra?0:2];dest[3]=255;
        return;
    }
    const bool pq=color==Color::hdr10_pq;
    if(pq) {
        const float pr=pq_to_linear(r),pg=pq_to_linear(g),pb=pq_to_linear(b);
        // Convert the live HDR display primaries (Rec.2020) to the thumbnail's Rec.709.
        r=1.6605f*pr-0.5876f*pg-0.0728f*pb;
        g=-0.1246f*pr+1.1329f*pg-0.0083f*pb;
        b=-0.0182f*pr-0.1006f*pg+1.1187f*pb;
    }
    const bool linear=pq||color==Color::scrgb||f==Format::r16g16b16a16_float;
    dest[0]=channel(r,linear);dest[1]=channel(g,linear);dest[2]=channel(b,linear);dest[3]=255;
}
struct Texture {
    api::resource resource{};api::resource_view view{};
    unsigned width{},height{};
};
void destroy(api::device *device,Texture &texture) {
    if(texture.view.handle) device->destroy_resource_view(texture.view);
    if(texture.resource.handle) device->destroy_resource(texture.resource);
    texture={};
}
Texture upload(api::device *device,ThumbnailPixels pixels) {
    if(pixels.rgba.empty()) return {};
    constexpr auto format=Format::r8g8b8a8_unorm;
    constexpr auto usage=api::resource_usage::shader_resource;
    const api::resource_desc desc(pixels.width,pixels.height,1,1,format,1,api::memory_heap::default_,usage);
    const api::subresource_data data{pixels.rgba.data(),pixels.width*4,static_cast<uint32_t>(pixels.rgba.size())};
    Texture result{};result.width=pixels.width;result.height=pixels.height;
    if(!device->create_resource(desc,&data,usage,&result.resource)) return {};
    if(!device->create_resource_view(result.resource,usage,api::resource_view_desc(format),&result.view)) destroy(device,result);
    return result;
}
}
void register_thumbnail_output() { reshade::register_event<reshade::addon_event::present>(on_swapchain_present); }
Bytes capture_thumbnail(Runtime *rt) noexcept {
    try {
        if(!rt || !rt->get_back_buffer_count()) return {};
        unsigned w=0,h=0;rt->get_screenshot_width_and_height(&w,&h);
        const auto desc=rt->get_device()->get_resource_desc(rt->get_current_back_buffer());
        const auto f=api::format_to_default_typed(desc.texture.format,0);
        const auto color=output.window==rt->get_hwnd()?output.color:Color::unknown;
        if(!w||!h||desc.texture.width!=w||desc.texture.height!=h||!supported(f)||color==Color::hdr10_hlg) return {};
        const unsigned bytes_per_pixel=f==Format::r16g16b16a16_float?8:4;
        const uint64_t needed=uint64_t(w)*h*bytes_per_pixel;
        if(needed>256ull*1024*1024) return {};
        Bytes pixels(static_cast<size_t>(needed));
        if(!rt->capture_screenshot(pixels.data())) return {};
        const double scale=std::min({1.,double(preview_width)/w,double(preview_height)/h});
        const auto out_w=std::max(1u,static_cast<unsigned>(std::lround(w*scale)));
        const auto out_h=std::max(1u,static_cast<unsigned>(std::lround(h*scale)));
        ThumbnailPixels thumb{out_w,out_h,Bytes(size_t(out_w)*out_h*4)};
        for(unsigned y=0;y<out_h;++y) for(unsigned x=0;x<out_w;++x) {
            auto sx=static_cast<unsigned>((uint64_t(x)*2+1)*w/(2*out_w));
            auto sy=static_cast<unsigned>((uint64_t(y)*2+1)*h/(2*out_h));
            convert_pixel(pixels.data()+(size_t(sy)*w+sx)*bytes_per_pixel,f,color,thumb.rgba.data()+(size_t(y)*out_w+x)*4);
        }
        return encode_thumbnail(thumb);
    } catch(...) {return {};}
}
struct ThumbnailCache::State {
    struct Item { uint64_t sequence{}; Digest digest{}; int drawn_frame=-1; Texture texture{}; uint32_t account{}; };
    std::array<Item,2*limit> items{};
    // A retired view may still be in this frame's ImGui draw list. Wait until
    // a later frame before fencing and destroying it.
    std::vector<Texture> retired;
    int frame=-1;
    Runtime *owner=nullptr;
    State(){retired.reserve(limit);}
};
ThumbnailCache::ThumbnailCache()=default;
ThumbnailCache::~ThumbnailCache()=default;
void ThumbnailCache::draw(Runtime *rt,const Entry &entry,float width,const std::function<Bytes()> &load_png) {
    if(!rt||!entry.slot||entry.slot>limit||width<=0.f) return;
    const ImVec2 box{width,width*9.f/16.f};
    const bool visible=ImGui::IsRectVisible(box);
    ImGui::Dummy(box);
    if(!visible) return;
    if(!state_) state_=std::make_unique<State>();
    auto &cache=*state_;
    if(cache.owner && cache.owner!=rt) return;
    cache.owner=rt;
    const int frame=ImGui::GetFrameCount();
    auto *device=rt->get_device();
    if(frame!=cache.frame) {
        if(!cache.retired.empty()) {
            rt->get_command_queue()->wait_idle();
            for(auto &old:cache.retired) destroy(device,old);
            cache.retired.clear();
        }
        cache.frame=frame;
    }
    auto &item=cache.items[(entry.shelf==Shelf::timed?limit:0)+entry.slot-1];
    if(item.sequence!=entry.sequence || item.digest!=entry.digest || item.account!=entry.account) {
        // Different selections can refer to the same ring slot within one GUI
        // frame. Keep the first version until the next frame for safe lifetime.
        if(item.drawn_frame==frame) return;
        if(item.texture.resource.handle) cache.retired.push_back(item.texture);
        item={entry.sequence,entry.digest,frame,{},entry.account};
        try {item.texture=upload(device,decode_thumbnail(load_png()));}
        catch(const std::exception &) {} // A missing image never prevents loading the save.
    }
    item.drawn_frame=frame;
    const ImVec2 top=ImGui::GetItemRectMin();
    auto *draw=ImGui::GetWindowDrawList();
    if(!item.texture.view.handle) {
        draw->AddText(top,ImGui::GetColorU32(ImGuiCol_TextDisabled),"No preview");
        return;
    }
    const float scale=std::min(box.x/item.texture.width,box.y/item.texture.height);
    const ImVec2 size{scale*item.texture.width,scale*item.texture.height};
    const ImVec2 from{top.x+(box.x-size.x)/2,top.y+(box.y-size.y)/2};
    draw->AddImage(ImTextureRef(item.texture.view.handle),from,{from.x+size.x,from.y+size.y});
}
void ThumbnailCache::end_runtime(Runtime *rt) {
    if(!state_||state_->owner!=rt) return;
    rt->get_command_queue()->wait_idle();
    auto *device=rt->get_device();
    for(auto &item:state_->items) destroy(device,item.texture);
    for(auto &old:state_->retired) destroy(device,old);
    state_.reset();
}
}
