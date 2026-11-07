#include "telemetry.hpp"
#include <windows.h>
#include <iomanip>
#include <sstream>
#include <locale>
#include <cmath>
#include <cstdio>

namespace qs {
namespace {
struct Handle { HANDLE h{}; ~Handle(){ if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); } };
std::string quoted(const std::string &s) {
    std::ostringstream o; o<<'"';
    for(unsigned char c:s) {
        if(c=='"' || c=='\\') o<<'\\'<<c;
        else if(c<32) o<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<int(c);
        else o<<c;
    }
    o<<'"';return o.str();
}
std::string line(const Event &e) {
    SYSTEMTIME t{}; GetSystemTime(&t);
    char utc[48];
    std::snprintf(utc,sizeof utc,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    std::ostringstream o; o.imbue(std::locale::classic()); o<<std::setprecision(15);
    o<<"{\"utc\":"<<quoted(utc)<<",\"pid\":"<<GetCurrentProcessId()<<",\"event\":"<<quoted(e.name)
     <<",\"request\":"<<e.request<<",\"entry\":"<<e.sequence<<",\"elapsed_ms\":"<<e.elapsed_ms;
    if(!e.detail.empty()) o<<",\"detail\":"<<quoted(e.detail);
    if(e.world) {
        o<<",\"game_flags\":"<<e.world->flags<<",\"cinematic\":"<<(e.world->cinematic()?"true":"false");
        if(e.world->carried_count) o<<",\"carried_cargo_count\":"<<*e.world->carried_count;
        if(e.world->delivery) {
            const auto &delivery=*e.world->delivery;
            std::ostringstream hash; hash<<std::setfill('0')<<std::hex;
            for(uint8_t byte:delivery.digest) hash<<std::setw(2)<<unsigned(byte);
            o<<",\"active_orders\":"<<delivery.active_orders<<",\"delivered_count\":"<<delivery.delivered_count
             <<",\"order_digest\":"<<quoted(hash.str());
        }
        if(e.world->position) {
            const auto &p=*e.world->position;
            if(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))
                o<<",\"sam_xyz\":["<<p[0]<<','<<p[1]<<','<<p[2]<<']';
        }
    }
    if(e.distance && std::isfinite(*e.distance)) o<<",\"distance_from_save\":"<<*e.distance;
    o<<"}\n"; return o.str();
}
}
Telemetry::Telemetry(fs::path folder):folder_(std::move(folder)) {}
bool Telemetry::write(const Event &e) const noexcept {
    try {
        if(folder_.empty()) return false;
        // Serialize across game instances as well as the render, game, and archive threads.
        Handle mutex{CreateMutexW(nullptr,FALSE,L"Local\\DSDCQuickSavesTelemetry")};
        if(!mutex.h) return false;
        auto rc=WaitForSingleObject(mutex.h,500);
        if(rc!=WAIT_OBJECT_0 && rc!=WAIT_ABANDONED) return false;
        struct Unlock {HANDLE h; ~Unlock(){ReleaseMutex(h);}} unlock{mutex.h};
        fs::create_directories(folder_);
        const fs::path path=current();
        constexpr size_t limit_bytes=2u*1024u*1024u;
        if(fs::exists(path) && fs::file_size(path)>=limit_bytes &&
            !MoveFileExW(path.c_str(),(folder_/L"quicksaves.previous.jsonl").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
        auto text=line(e);
        Handle file{CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(file.h==INVALID_HANDLE_VALUE) return false;
        DWORD written=0;
        return WriteFile(file.h,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(file.h);
    } catch(...) { return false; }
}
Telemetry &telemetry(){ static Telemetry log(data_root().empty() ? fs::path{} : data_root()/L"Logs"); return log; }
}
