#include "private_record.hpp"
#include "native_image.hpp"
#include "thumbnail_image.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <iostream>
using namespace qs;
namespace {
template<class T> void set(Bytes &bytes,size_t offset,T value) {
    std::memcpy(bytes.data()+offset,&value,sizeof value);
}
void expect(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    try {f();} catch(const std::runtime_error &) {return;}
    throw std::runtime_error("Damaged record accepted");
}
}
int main() {
    try {
        PrivateRecord record; record.image.resize(native_header_size+3);
        record.image[0x24]=0x43; set<uint16_t>(record.image,0x26,111);
        set<uint32_t>(record.image,0x2c,0x4c);set<uint32_t>(record.image,0x1fc,3);
        uint8_t md5[]={0x90,1,0x50,0x98,0x3c,0xd2,0x4f,0xb0,0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
        std::copy_n(md5,16,record.image.data()+0x1ec);
        std::memcpy(record.image.data()+native_header_size,"abc",3);
        record.world={0x40,std::array<double,3>{23.75,-45.1,28.5}};
        record.world.carried_count=7;
        record.world.delivery=DeliverySample{2,3,{}};
        record.world.delivery->digest[31]=77;
        record.scenes={{12,23,456,1},{34,55,678,2}};
        auto png=encode_thumbnail({2,2,Bytes(16,255)});
        expect(!png.empty(),"Cannot encode test image");
        auto wrapped=pack_record(record,png);
        expect(native_thumbnail(wrapped)==png,"Screenshot mismatch");
        expect(describe(wrapped).title=="Cutscene quicksave","Incorrect title");
        auto read=unpack_record(wrapped);
        expect(!read.vehicle_seat,"Invented a seat for a legacy record");
        record.vehicle_cargo=NearbyCargo{2,11,{}};
        record.vehicle_cargo->digest[12]=0xab;
        const auto cargo_read=unpack_record(pack_record(record,{})).vehicle_cargo;
        expect(cargo_read && cargo_read->vehicles==2 && cargo_read->items==11 &&
               cargo_read->digest==record.vehicle_cargo->digest,"Lost vehicle cargo signature");
        record.vehicle_seat=0x1e000;
        expect(unpack_record(pack_record(record,{})).vehicle_seat==record.vehicle_seat,"Lost mounted vehicle identity");
        expect(read.world.position==record.world.position && read.world.flags==record.world.flags && read.image==record.image,"Lost world state");
        expect(read.world.carried_count && *read.world.carried_count==7,"Lost cargo count");
        expect(read.world.delivery && read.world.delivery->active_orders==2 && read.world.delivery->delivered_count==3 && read.world.delivery->digest[31]==77,"Lost order diagnostic");
        expect(read.scenes.size()==2 && read.scenes[1].high==55 && read.scenes[0].frame==456 && read.scenes[1].kind==2,"Lost timecodes");
        auto original=wrapped;wrapped.back()^=1;rejects([&]{unpack_record(wrapped);});
        wrapped=original;auto m=std::search(wrapped.begin(),wrapped.end(),"DSWORLD1","DSWORLD1"+8);
        expect(m!=wrapped.end(),"Missing format marker");*m='X';rejects([&]{unpack_record(wrapped);});
        wrapped=original;wrapped[16]='0';rejects([&]{unpack_record(wrapped);});
        record.scenes.clear();record.world={};auto road=unpack_record(pack_record(record,{}));
        expect(!road.world.position && road.scenes.empty(),"Missing sample replaced with invented state");
        std::cout<<"Verified world, PNG and scene clock round trip and rejection of damaged records.\n";
    } catch(const std::exception &e) {std::cerr<<e.what();return 1;}
}
