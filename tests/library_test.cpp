#include "library.hpp"
#include <iostream>
#include <algorithm>
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

int main() {
    const auto work = fs::current_path() / ("test-library-" + std::to_string(GetCurrentProcessId()));
    fs::create_directory(work);
    try {
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
