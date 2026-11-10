#include "private_record.hpp"
#include "native_image.hpp"
#include "thumbnail_image.hpp"
#include <charconv>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <locale>
#include <algorithm>

namespace qs {
namespace {
// This format is little-endian, for the Windows x64 release only. Reserved
// bytes stay zero so a future reader can distinguish new formats reliably.
struct Header {
    char magic[8];
    uint64_t flags;
    double xyz[3];
    uint32_t has_position, scene_count, image_size, reserved;
    uint32_t has_delivery,active_orders,delivered_count,delivery_reserved;
    Digest delivery_digest;
    uint32_t has_carried_count,carried_count;
};
static_assert(sizeof(Header)==112 && sizeof(SceneStamp)==24);
constexpr char magic[8]={'D','S','W','O','R','L','D','1'};
constexpr unsigned max_scenes=128;
void append(Bytes &out,const void* data,size_t n) {
    if(n) { auto p=static_cast<const uint8_t*>(data); out.insert(out.end(),p,p+n); }
}
void put_length(Bytes &out,size_t offset,size_t value) {
    char* start=reinterpret_cast<char*>(out.data()+offset);
    if(std::to_chars(start,start+8,value).ec!=std::errc{}) throw std::runtime_error("Private record is too large.");
}
size_t get_length(const Bytes &data,size_t pos) {
    size_t value=0; auto p=reinterpret_cast<const char*>(data.data()+pos);
    auto result=std::from_chars(p,p+8,value);
    if(result.ec!=std::errc{} || (result.ptr!=p+8 && *result.ptr!='\0'))
        throw std::runtime_error("Unreadable quicksave section length.");
    return value;
}
}
Bytes pack_record(const PrivateRecord &record,const Bytes &png) {
    validate_native_image(record.image);
    if(record.scenes.size()>max_scenes || png.size()>max_preview_png_bytes)
        throw std::runtime_error("Quicksave preview or scene list exceeds its limit.");
    Header h{}; std::memcpy(h.magic,magic,8); h.flags=record.world.flags;
    if(record.world.position) {
        std::copy_n(record.world.position->data(),3,h.xyz);
        h.has_position=1;
    }
    if(record.world.delivery) {
        const auto &d=*record.world.delivery;
        h.has_delivery=1; h.active_orders=d.active_orders; h.delivered_count=d.delivered_count; h.delivery_digest=d.digest;
    }
    if(record.world.carried_count) {h.has_carried_count=1;h.carried_count=*record.world.carried_count;}
    h.scene_count=static_cast<uint32_t>(record.scenes.size()); h.image_size=static_cast<uint32_t>(record.image.size());
    std::ostringstream text; text.imbue(std::locale::classic());
    text<<"[Slot]\r\nTitle = "<<(record.world.cinematic()||!record.scenes.empty()?"Cutscene":"Delivery")<<" quicksave\r\nDetail = World "<<native_world_id(record.image);
    if(h.has_position) text<<std::fixed<<std::setprecision(1)<<"   Sam ("<<h.xyz[0]<<", "<<h.xyz[1]<<", "<<h.xyz[2]<<")";
    if(h.has_delivery) text<<"   Active orders: "<<h.active_orders;
    if(h.has_carried_count) text<<"   Carried cargo: "<<h.carried_count;
    text<<"\r\n";
    if(record.vehicle_seat) text<<"VehicleSeat = "<<*record.vehicle_seat<<"\r\n";
    if(record.vehicle_cargo) {
        const auto &cargo=*record.vehicle_cargo;
        text<<"VehicleCargo = "<<cargo.vehicles<<' '<<cargo.items<<' '<<std::hex<<std::setfill('0');
        for(auto b:cargo.digest) text<<std::setw(2)<<unsigned(b);
        text<<"\r\n";
    }
    auto ini=text.str();
    Bytes result(24);
    put_length(result,0,ini.size()); put_length(result,8,png.size());
    put_length(result,16,sizeof h + sizeof(SceneStamp)*record.scenes.size()+record.image.size());
    result.reserve(24+ini.size()+png.size()+sizeof h+sizeof(SceneStamp)*h.scene_count+h.image_size);
    append(result,ini.data(),ini.size()); append(result,png.data(),png.size()); append(result,&h,sizeof h);
    append(result,record.scenes.data(),h.scene_count*sizeof(SceneStamp)); append(result,record.image.data(),h.image_size);
    return result;
}
PrivateRecord unpack_record(const Bytes &archive) {
    describe(archive);  // Checks the total length matches all three bounded sections.
    auto offset=24+get_length(archive,0)+get_length(archive,8);
    auto state=get_length(archive,16);
    if(state<sizeof(Header) || std::memcmp(archive.data()+offset,magic,8)!=0)
        throw std::runtime_error("This entry uses the old checkpoint format. It cannot be loaded as a private world.");
    Header h{}; std::memcpy(&h,archive.data()+offset,sizeof h); offset+=sizeof h;
    if(h.scene_count>max_scenes || h.has_position>1 || h.reserved || h.has_delivery>1 || h.delivery_reserved || h.has_carried_count>1 ||
       sizeof h + size_t(h.scene_count)*sizeof(SceneStamp)+h.image_size!=state)
        throw std::runtime_error("The private world record is damaged.");
    PrivateRecord result; result.world.flags=h.flags;
    if(h.has_carried_count) result.world.carried_count=h.carried_count;
    if(h.has_delivery) result.world.delivery=DeliverySample{h.active_orders,h.delivered_count,h.delivery_digest};
    if(h.has_position) {
        for(auto xyz:h.xyz) if(!std::isfinite(xyz)) throw std::runtime_error("Invalid saved position.");
        result.world.position=std::array<double,3>{h.xyz[0],h.xyz[1],h.xyz[2]};
    }
    result.scenes.resize(h.scene_count); auto bytes=h.scene_count*sizeof(SceneStamp);
    if(bytes) std::memcpy(result.scenes.data(),archive.data()+offset,bytes);
    result.image.assign(archive.begin()+offset+bytes,archive.end());
    validate_native_image(result.image);
    result.vehicles=inspect_vehicle_archive(result.image);
    // This optional line keeps version 1 archives readable in both directions.
    std::string_view info(reinterpret_cast<const char*>(archive.data()+24),get_length(archive,0));
    constexpr std::string_view label="\r\nVehicleSeat = ";
    const auto start=info.find(label);
    if(start!=info.npos) {
        const auto text=info.substr(start+label.size());
        const auto eol=text.find("\r\n");
        uint32_t id{};
        if(eol!=text.npos) {
            const auto parsed=std::from_chars(text.data(),text.data()+eol,id);
            if(parsed.ec==std::errc{} && parsed.ptr==text.data()+eol) result.vehicle_seat=id;
        }
    }
    constexpr std::string_view cargo_label="\r\nVehicleCargo = ";
    auto cargo_at=info.find(cargo_label);
    if(cargo_at!=info.npos) {
        auto fields=info.substr(cargo_at+cargo_label.size());
        const auto eol=fields.find("\r\n");
        if(eol==fields.npos || eol>100) throw std::runtime_error("Invalid vehicle cargo signature.");
        std::istringstream input(std::string{fields.substr(0,eol)});
        input.imbue(std::locale::classic());
        NearbyCargo cargo; std::string hex,extra;
        if(!(input>>cargo.vehicles>>cargo.items>>hex) || (input>>extra) || hex.size()!=cargo.digest.size()*2)
            throw std::runtime_error("Invalid vehicle cargo signature.");
        for(size_t i=0;i<cargo.digest.size();++i) {
            auto begin=hex.data()+2*i;
            auto decoded=std::from_chars(begin,begin+2,cargo.digest[i],16);
            if(decoded.ec!=std::errc{} || decoded.ptr!=begin+2) throw std::runtime_error("Invalid cargo checksum.");
        }
        result.vehicle_cargo=cargo;
    }
    return result;
}
}
