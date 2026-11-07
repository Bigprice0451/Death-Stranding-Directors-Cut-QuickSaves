#include "restore.hpp"
#include <windows.h>
#include <cstdio>
#include <iostream>
#include <stdexcept>

using namespace qs;
static void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static Bytes checkpoint(char state) {
    std::string info="[Slot]\r\nTitle=Bridge test\r\n";
    Bytes b(24);
    auto section=[&](int i,int n){auto s=std::to_string(n);std::copy(s.begin(),s.end(),b.begin()+8*i);};
    section(0,static_cast<int>(info.size())); section(1,0); section(2,1);
    b.insert(b.end(),info.begin(),info.end()); b.push_back(static_cast<uint8_t>(state)); return b;
}
static void write(const fs::path& path,const Bytes& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); require(out.good(),"fixture write");
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Give the test an empty scratch parent on D:");
        fs::path parent=fs::absolute(argv[1]);
        require(parent.root_name()==L"D:"||parent.root_name()==L"d:","test only runs on D:");
        fs::create_directories(parent);
        auto root=parent/("restore-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        require(fs::create_directory(root),"test needs fresh directory");
        auto native=root/"native"/"12345", recovery=root/"recovery";
        for(unsigned n=0;n<23;++n) fs::create_directories(native/("manualsave"+std::to_string(n)));
        auto slot=[&](int n){return native/("manualsave"+std::to_string(n))/"checkpoint.dat";};
        RestoreBridge bridge(native,recovery);
        auto a=checkpoint('A'),b=checkpoint('B'),c=checkpoint('C');
        write(slot(9),a); auto stamp=fs::last_write_time(slot(9));
        require(bridge.stage(a)==9 && !bridge.pending() && !bridge.recover(),"duplicate should use native slot untouched");
        require(fs::last_write_time(slot(9))==stamp,"existing match timestamp changed");
        write(slot(0).parent_path()/"slotinfo.ini",{'x'});
        write(slot(1).parent_path()/"checkpoint.dattemp",{'x'});
        int chosen=bridge.stage(b);require(chosen==2 && bridge.pending(),"sidecar directories must be skipped");
        require(bridge.stage(b)==chosen,"retry must reuse same stage");
        require(read_file(slot(chosen))==b,"native copy differs");
        RestoreBridge restarted(native,recovery);
        require(restarted.recover() && !bridge.pending() && !fs::exists(slot(chosen)),"empty slot recovery");
        require(!bridge.recover(),"second recovery should have no effect");
        fs::remove(slot(0).parent_path()/"slotinfo.ini"); fs::remove(slot(1).parent_path()/"checkpoint.dattemp");
        for(int i=0;i<23;++i) write(slot(i),a);
        std::vector<fs::file_time_type> stamps;
        for(int i=0;i<23;++i) stamps.push_back(fs::last_write_time(slot(i)));
        chosen=bridge.stage(b);require(read_file(slot(chosen))==b,"full slot stage");
        require(bridge.recover() && read_file(slot(chosen))==a,"full slot original bytes");
        require(fs::last_write_time(slot(chosen))==stamps.at(chosen),"original timestamp not restored");
        chosen=bridge.stage(b);write(slot(chosen),c);
        require(bridge.recover() && read_file(slot(chosen))==c,"newer game save must survive");
        bool preserved=false;
        for(auto& e:fs::recursive_directory_iterator(recovery))
            if(e.is_regular_file() && e.path().extension()==".dat") preserved|=read_file(e.path())==a;
        require(preserved,"displaced original not retained after external write");
        for(int i=0;i<23;++i) write(slot(i).parent_path()/"checkpoint.dattemp",{'x'});
        bool blocked=false;try{bridge.stage(b);}catch(const std::exception&){blocked=true;}
        require(blocked && !bridge.pending(),"sidecar-only ring must fail safely");
        for(int i=0;i<23;++i) fs::remove(slot(i).parent_path()/"checkpoint.dattemp");
        chosen=bridge.stage(b); write(slot(chosen).parent_path()/"slotinfo.ini",{'x'});
        blocked=false;try{bridge.recover();}catch(const std::exception&){blocked=true;}
        require(blocked && bridge.pending() && read_file(slot(chosen))==b,"appearing native sidecar must stop recovery");
        fs::remove(slot(chosen).parent_path()/"slotinfo.ini");require(bridge.recover(),"recovery did not resume");
        // A power cut can leave a completed native rollback with a journal that still needs cleanup.
        chosen=bridge.stage(b);write(slot(chosen),a);require(bridge.recover(),"interrupted cleanup was not recognized");
        require(read_file(slot(chosen))==a,"interrupted cleanup changed player's data");
        chosen=bridge.stage(b);
        auto journal=recovery/"12345"/"pending.dsqj";
        const auto valid_journal=read_file(journal);
        auto damaged=valid_journal;damaged[16]^=1;write(journal,damaged);
        blocked=false;try{bridge.recover();}catch(const std::exception&){blocked=true;}
        require(blocked && bridge.pending() && read_file(slot(chosen))==b,"damaged recovery record must fail closed");
        write(journal,valid_journal);
        auto other=root/"other"/"12345";fs::create_directories(other/"manualsave0");
        RestoreBridge wrongProfile(other,recovery);
        blocked=false;try{wrongProfile.recover();}catch(const std::exception&){blocked=true;}
        require(blocked && bridge.pending() && !fs::exists(other/"manualsave0"/"checkpoint.dat"),
                "journal must be bound to the actual source directory");
        HANDLE busy=CreateFileW(slot(chosen).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        require(busy!=INVALID_HANDLE_VALUE,"could not start sharing conflict");
        blocked=false;try{bridge.recover();}catch(const std::exception&){blocked=true;}
        CloseHandle(busy);require(blocked && bridge.pending(),"writer must block native cleanup");
        require(bridge.recover() && !bridge.pending(),"did not recover after writer released slot");
        bool rejected=false;try{RestoreBridge invalid(native,fs::path(L"C:\\forbidden"));}catch(const std::exception&){rejected=true;}
        require(rejected,"recovery location on C was permitted");
        fs::path orphan=journal;orphan+=L".writing";write(orphan,{'x'});
        require(bridge.pending() && !bridge.recover() && !fs::exists(orphan),"orphan journal scratch should be cleaned");
        fs::remove_all(root);
        std::cout<<"Restore round trips, recovery, changed files, sidecars and rotation passed.\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
