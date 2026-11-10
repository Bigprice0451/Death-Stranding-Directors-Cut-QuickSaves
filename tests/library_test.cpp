#include "library.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <windows.h>

using namespace qs;
static void require(bool value, const char *reason) { if (!value) throw std::runtime_error(reason); }

static Bytes fixture(int value) {
    std::string ini = "[Slot]\r\nTitle = Order %22Test%22\r\nDetail = %0A%0ACapital Knot City\r\n";
    Bytes bytes(24, 0xAC);
    auto length = [&](size_t start, size_t n) {
        auto s = std::to_string(n);
        std::copy(s.begin(), s.end(), bytes.begin() + start);
        bytes[start + s.size()] = 0;
    };
    length(0,ini.size()); length(8,2); length(16,1);
    bytes.insert(bytes.end(),ini.begin(),ini.end());
    bytes.push_back(3); bytes.push_back(7); bytes.push_back(static_cast<uint8_t>(value));
    return bytes;
}

// Produce the on-disk result of the first, durable half of an approved trim.
// This models a process exit between publication of the decision and unlink.
static Bytes committed_trim(unsigned account,Shelf shelf,unsigned cap,const std::vector<Entry> &approved) {
    Bytes raw{'D','S','Q','C','A','P','1',0};
    auto number=[&](uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;++i){raw.push_back(static_cast<uint8_t>(n));n>>=8;}};
    number(account,4);number(static_cast<unsigned>(shelf),4);number(cap,4);number(approved.size(),4);
    for(auto &entry:approved) {
        number(entry.slot,4);number(0,4);number(entry.sequence,8);number(entry.saved_at,8);
        raw.insert(raw.end(),entry.digest.begin(),entry.digest.end());
    }
    const auto digest=sha256(raw);raw.insert(raw.end(),digest.begin(),digest.end());
    return raw;
}

// Run with --benchmark from a scratch folder with at least 550 MiB available.
// Use the normal store entry point to fill and rotate a separate synthetic shelf.
static void benchmark_full_shelf(const fs::path &work) {
    auto native=fixture(0);
    constexpr size_t state_size=5u*1024u*1024u;
    const auto length=std::to_string(state_size);
    std::memcpy(native.data()+16,length.c_str(),length.size()+1);
    native.resize(native.size()+state_size-1,0x5A);
    Library library(work,"104");
    for(unsigned i=0;i<limit;++i) {
        native.back()=static_cast<uint8_t>(i);
        library.store(native);
    }
    require(library.list().size()==limit,"synthetic shelf is incomplete");
    std::vector<double> samples;
    for(unsigned i=0;i<9;++i) {
        native.back()=static_cast<uint8_t>(limit+i);
        const auto start=std::chrono::steady_clock::now();
        auto entry=library.store(native);
        const auto elapsed=std::chrono::steady_clock::now()-start;
        require(entry.sequence==limit+i+1 && library.read(entry)==native,"benchmark rotation lost the record");
        samples.push_back(std::chrono::duration<double,std::milli>(elapsed).count());
    }
    std::cout<<"100-slot archive, "<<native.size()<<" bytes per checkpoint, rotation times (ms):";
    for(double ms:samples) std::cout<<" "<<ms;
    std::sort(samples.begin(),samples.end());
    std::cout<<"; median "<<samples[samples.size()/2]<<" ms\n";
}

int main(int argc,char **argv) {
    const auto work = fs::current_path() / ("test-library-" + std::to_string(GetCurrentProcessId()));
    require(fs::create_directory(work),"synthetic test folder already exists");
    try {
        if(argc==2 && std::string(argv[1])=="--benchmark") {
            benchmark_full_shelf(work);
            fs::remove_all(work);return 0;
        }
        auto input = fixture(0);
        auto info = describe(input);
        require(info.title=="Order \"Test\"" && info.location=="Capital Knot City","native fields");
        auto corrupt=input; corrupt.pop_back();
        bool failed=false; try { describe(corrupt); } catch (...) { failed=true; }
        require(failed,"truncated checkpoint accepted");
        Library lib(work, "101");
        for (int n=0; n<103; ++n) lib.store(fixture(n));
        auto rows=lib.list();
        require(rows.size()==100 && rows.front().sequence==103 && rows.back().sequence==4,"retention order");
        require(lib.read(rows.front())==fixture(102),"payload round trip");
        require(Library(work,"102").list().empty(),"account separation");
        auto stale=rows.back();
        lib.store(fixture(103));
        failed=false; try { lib.read(stale); } catch (...) { failed=true; }
        require(failed,"rotated selection silently changed");
        auto file=lib.path(rows.front().slot);
        auto broken=read_file(file); broken.back()^=1;
        { std::ofstream out(file, std::ios::binary|std::ios::trunc); out.write((char*)broken.data(),broken.size()); }
        failed=false; try { lib.read(rows.front()); } catch (...) { failed=true; }
        require(failed,"payload damage accepted");
        Library damaged(work,"103");
        auto bad=damaged.store(fixture(1)), good=damaged.store(fixture(2));
        auto invalidHeader=read_file(damaged.path(bad.slot));
        invalidHeader[0]^=0x40;
        const auto put=[&](const fs::path &target,const Bytes &data) {
            std::ofstream out(target,std::ios::binary|std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()),data.size());
            require(out.good(),"failed to create damaged archive fixture");
        };
        // File size and modification time alone can both survive an outside edit.
        // Corrupt the oldest file after a verified scan. The next store must use
        // a free slot, and a subsequent external repair must rejoin the rotation.
        Library edited(work/"Edited","121",Shelf::manual,2);
        const auto first=edited.store(fixture(10));
        const auto second=edited.store(fixture(11));
        require(edited.list().size()==2,"could not warm the archive inventory");
        const auto first_file=edited.path(first.slot);
        const auto old_time=fs::last_write_time(first_file);
        const auto original=read_file(first_file);
        auto changed_bytes=original;changed_bytes.back()^=1;
        put(first_file,changed_bytes);fs::last_write_time(first_file,old_time);
        const auto third=edited.store(fixture(12));
        require(third.slot==3 && read_file(first_file)==changed_bytes && edited.read(second)==fixture(11),
                "same-length damage with restored timestamp was overwritten");
        fs::remove(edited.path(second.slot));
        fs::remove(first_file);put(first_file,original);fs::last_write_time(first_file,old_time);
        const auto fourth=edited.store(fixture(13));
        require(fourth.slot==first.slot && fourth.sequence==third.sequence+1 && edited.read(third)==fixture(12),
                "external repair or removal escaped the retained inventory");
        require(edited.list().size()==2,"could not refresh the repaired archive");
        // Another instance publishes a healthy replacement. An existing instance
        // must notice its newer sequence even when the timestamp is restored.
        const auto previous_time=fs::last_write_time(edited.path(third.slot));
        Library importer(work/"Edited","121");
        auto imported=importer.store(fixture(14));
        require(imported.slot==third.slot,"fixture replaced unexpected slot");
        fs::last_write_time(edited.path(imported.slot),previous_time);
        auto next=edited.store(fixture(15));
        require(next.sequence==imported.sequence+1 && edited.read(imported)==fixture(14),
                "existing inventory missed a newer external occupant");
        require(edited.list().size()==2,"could not warm the busy-file fixture");
        const auto busy_file=edited.path(imported.slot);
        HANDLE writer=CreateFileW(busy_file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        require(writer!=INVALID_HANDLE_VALUE,"could not open synthetic save for editing");
        Entry beside_busy;
        try { beside_busy=edited.store(fixture(16)); }
        catch(...) { CloseHandle(writer);throw; }
        CloseHandle(writer);
        require(beside_busy.slot!=imported.slot && edited.read(imported)==fixture(14),
                "cached slot stayed eligible while an outside writer held it");
        put(damaged.path(bad.slot),invalidHeader);
        unsigned hidden=0; auto survivors=damaged.list(&hidden);
        require(hidden==1,"wrong damaged slot count");
        require(survivors.size()==1 && survivors[0].sequence==good.sequence &&
                damaged.read(survivors[0])==fixture(2),"damaged header hid healthy save");
        auto added=damaged.store(fixture(3));
        require(added.slot!=bad.slot && added.sequence==good.sequence+1,"F5 failed next to damaged save");
        require(read_file(damaged.path(bad.slot))==invalidHeader,"F5 overwrote damaged save");
        // Fill all remaining numbered slots with damaged records. Rotation may
        // replace the oldest healthy entry, including when only one survives.
        for(unsigned n=4;n<=limit;++n) put(damaged.path(n),invalidHeader);
        auto rotates=damaged.store(fixture(4));
        require(rotates.slot==good.slot && rotates.sequence==added.sequence+1,"rotation picked damaged occupied slot");
        auto payloadBreak=read_file(damaged.path(added.slot));payloadBreak.back()^=1;
        put(damaged.path(added.slot),payloadBreak);
        auto lastHealthy=damaged.store(fixture(5));
        require(lastHealthy.slot==rotates.slot && lastHealthy.sequence==rotates.sequence+1,
                "silently rotated save with damaged payload");
        put(damaged.path(lastHealthy.slot),invalidHeader);
        failed=false;try{damaged.store(fixture(6));}catch(const std::exception&){failed=true;}
        require(failed && read_file(damaged.path(added.slot))==payloadBreak,"all damaged files must be preserved");
        Library timed(work/"AutoSaves","111",Shelf::timed), manual(work/"QuickSaves","111");
        require(timed.capacity()==15 && manual.capacity()==100,"wrong independent default limits");
        for(int i=0;i<17;++i) timed.store(fixture(i));
        auto shown=timed.list();
        require(shown.size()==15 && shown.front().sequence==17 && shown.back().sequence==3,"timed default rotation");
        require(timed.set_capacity(100,shown)==0 && timed.capacity()==100,"could not increase timed capacity");
        const auto sixteenth=timed.store(fixture(17));
        require(sixteenth.slot==16 && timed.list().size()==16,"timed archive did not grow above 15");
        Library other(work/"AutoSaves","111",Shelf::timed);
        require(other.capacity()==100,"preference did not survive new instance");
        bool rejected=false; try{timed.set_capacity(1,shown);}catch(...){rejected=true;}
        require(rejected && timed.list().size()==16 && timed.capacity()==100,"stale approval deleted current saves");
        auto before=other.list();
        const auto oldest=before.back(), newest=before.front();
        fs::rename(timed.path(newest.slot),timed.path(99));
        const auto broken_bytes=read_file(timed.path(oldest.slot));
        auto bad_header=broken_bytes;bad_header[0]^=0x40;put(timed.path(oldest.slot),bad_header);
        before=other.list(&hidden);
        require(before.size()==15 && hidden==1 && before.front().slot==99,"high-numbered or damaged timed slot was miscounted");
        // A share lock on even one victim prevents every deletion before commit.
        HANDLE held=CreateFileW(timed.path(before.back().slot).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        require(held!=INVALID_HANDLE_VALUE,"could not lock synthetic test record");
        rejected=false;try{timed.set_capacity(1,before);}catch(...){rejected=true;}
        CloseHandle(held);
        require(rejected && timed.capacity()==100 && timed.list().size()==15,"partial trim from a locked record");
        auto reversed=before;std::reverse(reversed.begin(),reversed.end());
        require(timed.set_capacity(1,reversed)==14 && other.capacity()==1 && other.list().size()==1,"confirmed trim failed");
        require(other.read(other.list().front())==fixture(17) && other.list().front().slot==99 &&
            read_file(other.path(oldest.slot))==bad_header && manual.capacity()==100,"newest, damaged file or other shelf was changed");
        const auto old_single=other.list();
        auto after=other.store(fixture(18));
        require(after.slot==99 && other.list().size()==1 && timed.capacity()==1,"another instance failed to use the persisted limit");
        rejected=false;try{timed.set_capacity(2,old_single);}catch(...){rejected=true;}
        require(rejected && timed.capacity()==1 && other.read(after)==fixture(18),"stale equal-size approval accepted");
        for(unsigned invalid:{0u,101u}) {
            rejected=false;try{other.set_capacity(invalid,other.list());}catch(...){rejected=true;}
            require(rejected && other.capacity()==1,"accepted out of range retention");
        }
        Library seed(work/"Initial","112",Shelf::timed,3);
        require(Library(work/"Initial","112",Shelf::timed,99).capacity()==3,"explicit fresh capacity was not persistent");
        for(int i=0;i<3;++i) seed.store(fixture(i));
        auto victims=seed.list();const auto a=victims.back(), b=victims[1], survivor=victims.front();
        put(seed.path(1).parent_path()/L"retention.dsqp",committed_trim(112,Shelf::timed,1,{a,b}));
        // Another tool replaces an approved file after a crash. Recovery can
        // retire the other old file, while preserving the changed occupant.
        const auto newer=read_file(seed.path(survivor.slot));put(seed.path(b.slot),newer);
        auto resumed=seed.list();
        require(resumed.size()==2 && !fs::exists(seed.path(a.slot)) && read_file(seed.path(b.slot))==newer &&
                seed.read(survivor)==fixture(2),"crash recovery removed a newer occupant or left the original");
        require(seed.set_capacity(1,resumed)==1 && seed.list().size()==1,"could not reconcile an imported over-limit save");
        auto corrupt_setting=committed_trim(112,Shelf::timed,1,{});corrupt_setting.back()^=1;
        put(seed.path(1).parent_path()/L"retention.dsqp",corrupt_setting);
        require(seed.list().size()==1,"a damaged preference hid readable saves");
        rejected=false;try{seed.store(fixture(4));}catch(...){rejected=true;}
        require(rejected && seed.list().size()==1,"corrupted retention metadata permitted rotation");
        failed=false; try { Library(work,"../bad"); } catch (...) { failed=true; }
        require(failed,"invalid account accepted");
        fs::remove_all(work);
        std::cout << "Verified independent persistent 1-100 limits, stale approval, interrupted trims, damaged records and byte-for-byte saves.\n";
    } catch (const std::exception &e) {
        fs::remove_all(work); std::cerr << e.what() << '\n'; return 1;
    }
}
