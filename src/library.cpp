#include "library.hpp"
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>

namespace qs {
namespace {
constexpr size_t max_file = 128u * 1024u * 1024u;
// Fixed-width, little-endian Windows format. Records wrap an entire native checkpoint.
// A sequence identifies what occupied a slot when the player selected it.
struct Header {
    char magic[8];
    uint64_t sequence, saved_at, account, size;
    Digest digest;
};
static_assert(sizeof(Header)==72);
constexpr char magic[] = "DSQS001";

void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct Handle {
    HANDLE h;
    ~Handle() { if (h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); }
};
using OwnedFile = std::unique_ptr<Handle>;
bool missing(DWORD e) { return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND; }
std::string lock_name(Shelf shelf, const std::string &account) {
    return shelf == Shelf::timed ? "Auto-" + account : account;
}
struct ArchiveLock {
    Handle handle;
    explicit ArchiveLock(const std::string &id)
        : handle{CreateMutexA(nullptr,FALSE,("Local\\DSDCQuickSave-" + id).c_str())} {
        const auto result = handle.h ? WaitForSingleObject(handle.h, 2500) : WAIT_FAILED;
        check(result==WAIT_OBJECT_0 || result==WAIT_ABANDONED,"Quicksave library is busy.");
    }
    ~ArchiveLock() { ReleaseMutex(handle.h); }
};

size_t part_size(const uint8_t *s) {
    size_t n=0;
    const char *start=reinterpret_cast<const char*>(s);
    const auto result=std::from_chars(start,start+8,n);
    check(result.ec==std::errc{} && (result.ptr==start+8 || *result.ptr=='\0'),"Invalid native checkpoint length.");
    return n;
}
std::string clean_field(std::string_view input) {
    std::string out;
    for(size_t i=0;i<input.size();++i) {
        unsigned char c=static_cast<unsigned char>(input[i]);
        if(c=='%' && i+2<input.size()) {
            unsigned value=0;
            auto p=input.data()+i+1; auto result=std::from_chars(p,p+2,value,16);
            if(result.ec==std::errc{} && result.ptr==p+2){c=static_cast<unsigned char>(value); i+=2;}
        }
        if(c>=32 && c!=127) out.push_back(static_cast<char>(c));
        else if(!out.empty() && out.back()!=' ') out+=' ';
    }
    while(!out.empty() && out.back()==' ') out.pop_back();
    return out;
}
Info native_info(const uint8_t *data, size_t total) {
    check(total>=24, "Checkpoint is incomplete.");
    const size_t meta=part_size(data), thumbnail=part_size(data+8), state=part_size(data+16);
    check(meta>=6 && meta<=65536 && total>=24+meta && state>0 &&
          24ull+meta+thumbnail+state==total, "Native checkpoint sections are incomplete.");
    std::string_view ini(reinterpret_cast<const char*>(data+24),meta);
    check(ini.substr(0,6)=="[Slot]", "Unrecognized native checkpoint.");
    Info info;
    while(!ini.empty()) {
        auto end=ini.find_first_of("\r\n");
        auto line=ini.substr(0,end);
        auto equals=line.find('=');
        if(equals!=line.npos) {
            auto key=line.substr(0,equals);
            key=key.substr(0,key.find_last_not_of(" \t")+1);
            auto value=line.substr(equals+1);
            auto begin=value.find_first_not_of(" \t");
            if(begin!=value.npos){
                if(key=="Title") info.title=clean_field(value.substr(begin));
                if(key=="Detail") info.location=clean_field(value.substr(begin));
            }
        }
        if(end==ini.npos) break;
        ini.remove_prefix(end+1);
    }
    check(!info.title.empty(),"Checkpoint title is missing.");
    return info;
}
Digest hash_bytes(const uint8_t *data,size_t size) {
    BCRYPT_ALG_HANDLE alg=nullptr; Digest digest{};
    check(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA-256 is unavailable.");
    const auto status=BCryptHash(alg,nullptr,0,const_cast<PUCHAR>(data),static_cast<ULONG>(size),digest.data(),static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(alg,0);
    check(status>=0,"Could not check quicksave integrity.");
    return digest;
}
Entry inspect_record(const Bytes &record,unsigned slot,uint64_t account,Shelf shelf) {
    Header h{};
    check(record.size()>=sizeof h,"Incomplete quicksave.");
    std::memcpy(&h,record.data(),sizeof h);
    check(std::memcmp(h.magic,magic,sizeof magic)==0 && h.account==account && h.sequence &&
          h.size>=24 && h.size<=max_file && h.size==record.size()-sizeof h,
          "Invalid quicksave file or Steam account.");
    const auto *data=record.data()+sizeof h;
    check(hash_bytes(data,static_cast<size_t>(h.size))==h.digest,"Quicksave failed its integrity check.");
    return {slot,h.sequence,h.saved_at,native_info(data,static_cast<size_t>(h.size)),h.digest,shelf,static_cast<uint32_t>(account)};
}
Bytes read_handle(HANDLE file, size_t maximum) {
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file,&size) && size.QuadPart>0 && static_cast<uint64_t>(size.QuadPart)<=maximum,"Unexpected save file size.");
    Bytes data(static_cast<size_t>(size.QuadPart));
    DWORD count=0;
    check(ReadFile(file,data.data(),static_cast<DWORD>(data.size()),&count,nullptr) && count==data.size(),"Incomplete save file read.");
    return data;
}
BY_HANDLE_FILE_INFORMATION regular_slot_info(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(file,&info) && !(info.dwFileAttributes &
          (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"Not a regular quicksave file.");
    return info;
}
Bytes read_slot_file(const fs::path &path) {
    Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    check(file.h!=INVALID_HANDLE_VALUE,"Quicksave is busy or unavailable.");
    regular_slot_info(file.h);
    return read_handle(file.h,max_file+sizeof(Header));
}
uint64_t combined(DWORD high, DWORD low) { return (uint64_t(high)<<32)|low; }

// The change journal can distinguish same-size edits even when a tool restores
// the file's timestamp. A drive without a usable journal gets full checksums.
uint64_t file_change_number(HANDLE file, uint64_t file_id) {
    BYTE buffer[sizeof(USN_RECORD_V2)+MAX_PATH*sizeof(wchar_t)];
    DWORD returned=0;
    if(!DeviceIoControl(file,FSCTL_READ_FILE_USN_DATA,nullptr,0,buffer,sizeof buffer,&returned,nullptr) ||
       returned<offsetof(USN_RECORD_V2,FileName)) return 0;
    USN_RECORD_V2 record{};
    std::memcpy(&record,buffer,offsetof(USN_RECORD_V2,FileName));
    return record.MajorVersion==2 && record.FileReferenceNumber==file_id && record.Usn>0 ? record.Usn : 0;
}

// The same atomic file contains the new preference and the exact records the
// player approved for removal. A restart can finish an approved trim. A new
// occupant, damaged data or a link at an old slot is never removed by recovery.
struct PolicyHeader {
    char magic[8];
    uint32_t account, shelf, capacity, count;
};
struct Retired {
    uint32_t slot, reserved;
    uint64_t sequence, saved_at;
    Digest digest;
};
static_assert(sizeof(PolicyHeader)==24 && sizeof(Retired)==56);
struct Policy {
    unsigned capacity;
    std::vector<Retired> pending;
    bool persisted{};
};
constexpr char policy_magic[8] = "DSQCAP1";
constexpr auto policy_file = L"retention.dsqp";
bool valid_limit(unsigned n) { return n>=1 && n<=limit; }
Retired identity(const Entry &entry) {
    return {entry.slot,0,entry.sequence,entry.saved_at,entry.digest};
}
bool matches(const Retired &r, const Entry &e) {
    return r.slot==e.slot && r.sequence==e.sequence && r.saved_at==e.saved_at && r.digest==e.digest;
}
Policy load_policy(const fs::path &folder, uint32_t account, Shelf shelf, unsigned initial) {
    Handle h{CreateFileW((folder/policy_file).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    if(h.h==INVALID_HANDLE_VALUE && missing(GetLastError())) return {initial,{},false};
    check(h.h!=INVALID_HANDLE_VALUE,"Cannot read this save list's retention setting.");
    const auto raw=read_handle(h.h,sizeof(PolicyHeader)+limit*sizeof(Retired)+sizeof(Digest));
    check(raw.size()>=sizeof(PolicyHeader)+sizeof(Digest),"Damaged save retention setting. Existing saves are preserved.");
    PolicyHeader header;
    std::memcpy(&header,raw.data(),sizeof header);
    const size_t n=raw.size()-sizeof(Digest);
    const auto digest=hash_bytes(raw.data(),n);
    check(std::memcmp(digest.data(),raw.data()+n,digest.size())==0 && std::memcmp(header.magic,policy_magic,8)==0 &&
          header.account==account && header.shelf==static_cast<unsigned>(shelf) && valid_limit(header.capacity) &&
          header.count<=limit && n==sizeof header+header.count*sizeof(Retired),"Invalid retention setting. Existing saves are preserved.");
    Policy p{header.capacity,{},true};
    p.pending.resize(header.count);
    if(header.count) std::memcpy(p.pending.data(),raw.data()+sizeof header,header.count*sizeof(Retired));
    std::array<bool,limit+1> used{};
    for(const auto &r:p.pending) {
        check(r.slot && r.slot<=limit && r.sequence && !r.reserved && !used[r.slot],"Invalid retention recovery entry.");
        used[r.slot]=true;
    }
    return p;
}
void write_policy(const fs::path &folder, uint32_t account, Shelf shelf, const Policy &p) {
    PolicyHeader header{{},account,static_cast<unsigned>(shelf),p.capacity,static_cast<unsigned>(p.pending.size())};
    std::memcpy(header.magic,policy_magic,8);
    Bytes raw(sizeof header+p.pending.size()*sizeof(Retired));
    std::memcpy(raw.data(),&header,sizeof header);
    if(!p.pending.empty()) std::memcpy(raw.data()+sizeof header,p.pending.data(),p.pending.size()*sizeof(Retired));
    const auto sum=sha256(raw);raw.insert(raw.end(),sum.begin(),sum.end());
    const auto stage=folder/L"retention.dsqp.tmp";
    // Remove our abandoned staging *name*, then create a new file exclusively.
    // CREATE_ALWAYS would follow a pre-existing link or truncate its target.
    check(DeleteFileW(stage.c_str()) || missing(GetLastError()),"Retention staging file is busy.");
    try {
        {
            Handle file{CreateFileW(stage.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
            check(file.h!=INVALID_HANDLE_VALUE,"Cannot stage the save retention setting.");
            DWORD written=0;
            check(WriteFile(file.h,raw.data(),static_cast<DWORD>(raw.size()),&written,nullptr) && written==raw.size() &&
                  FlushFileBuffers(file.h),"Cannot flush the save retention setting.");
        }
        check(MoveFileExW(stage.c_str(),(folder/policy_file).c_str(),MOVEFILE_WRITE_THROUGH|MOVEFILE_REPLACE_EXISTING)!=0,
              "Cannot publish the save retention setting.");
    } catch(...) { DeleteFileW(stage.c_str()); throw; }
}
// One handle both verifies and unlinks a file. Denying writes/deletes until it
// closes prevents an outside editor from swapping another record into the slot
// between checksum verification and deletion.
OwnedFile open_retired(const Library &library, const Retired &r, uint32_t account, bool recovering) {
    const auto path=library.path(r.slot);
    OwnedFile file(new Handle{CreateFileW(path.c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                                          FILE_FLAG_OPEN_REPARSE_POINT,nullptr)});
    if(file->h==INVALID_HANDLE_VALUE) {
        if(recovering && missing(GetLastError())) return {};
        throw std::runtime_error("A save selected for removal is busy or inaccessible. Refresh this list and try again.");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(file->h,&info)!=0,"Cannot verify a save selected for removal.");
    if(!(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))) {
        try {
            auto current=inspect_record(read_handle(file->h,max_file+sizeof(Header)),r.slot,account,library.shelf());
            if(matches(r,current)) {
                check(!(info.dwFileAttributes & FILE_ATTRIBUTE_READONLY),"A save selected for removal is read-only.");
                return file;
            }
        } catch(const std::runtime_error &) { if(!recovering) throw; }
    }
    if(recovering) return {};
    throw std::runtime_error("A save changed before removal. Refresh this list and confirm again.");
}
void unlink_files(std::vector<OwnedFile> &files) {
    FILE_DISPOSITION_INFO deletion{TRUE};
    for(auto &f:files) if(f) {
        check(SetFileInformationByHandle(f->h,FileDispositionInfo,&deletion,sizeof deletion)!=0,
              "Save removal was interrupted. The confirmed removal will resume on refresh.");
    }
    files.clear();
}
void recover_policy(const Library &library, const fs::path &folder, uint32_t account, Policy &p) {
    if(p.pending.empty()) return;
    std::vector<OwnedFile> files;
    for(const auto &r:p.pending) files.push_back(open_retired(library,r,account,true));
    unlink_files(files);
    p.pending.clear();
    write_policy(folder,account,library.shelf(),p);
}
}
struct Library::Inventory {
    std::vector<Entry> healthy;
    unsigned damaged=0, first_free=0;
};
Library::Inventory Library::scan(uint32_t account, Inspection inspection) const {
    Inventory result;
    for(unsigned slot=1;slot<=slot_count(shelf_);++slot) {
        auto &cached=verified_[slot-1];
        // Inspect the same open file for identity, change number and payload.
        // Writers and renames are denied while we check; links are never followed.
        const auto filename=path(slot);
        Handle file{CreateFileW(filename.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
        if(file.h==INVALID_HANDLE_VALUE) {
            const auto error=GetLastError();
            cached={};
            if(missing(error)) {
                if(!result.first_free) result.first_free=slot;
            } else {
                ++result.damaged;
            }
            continue;
        }
        try {
            const auto info=regular_slot_info(file.h);
            const auto file_id=combined(info.nFileIndexHigh,info.nFileIndexLow);
            const auto change=file_change_number(file.h,file_id);
            const auto size=combined(info.nFileSizeHigh,info.nFileSizeLow);
            const auto written=combined(info.ftLastWriteTime.dwHighDateTime,info.ftLastWriteTime.dwLowDateTime);
            const bool reliable=file_id && change && info.dwVolumeSerialNumber;
            if(inspection==Inspection::reuse_verified && reliable && cached.file_id==file_id &&
               cached.volume==info.dwVolumeSerialNumber && cached.change_number==change &&
               cached.size==size && cached.write_time==written) {
                result.healthy.push_back(cached.entry);
                continue;
            }
            auto entry=inspect_record(read_handle(file.h,max_file+sizeof(Header)),slot,account,shelf_);
            if(reliable) cached={file_id,change,size,written,info.dwVolumeSerialNumber,entry};
            else cached={};
            result.healthy.push_back(std::move(entry));
        } catch(const std::runtime_error &) {
            cached={};
            ++result.damaged;
        }
    }
    std::sort(result.healthy.begin(),result.healthy.end(),
              [](const Entry &a,const Entry &b){return a.sequence!=b.sequence ? a.sequence>b.sequence : a.slot<b.slot;});
    return result;
}
bool valid_account(const std::string &s) {
    if(s.empty() || s.size()>10 || s[0]=='0') return false;
    uint32_t value=0; auto result=std::from_chars(s.data(),s.data()+s.size(),value);
    return result.ec==std::errc{} && result.ptr==s.data()+s.size() && value!=0;
}
Bytes read_file(const fs::path &file) {
    // Deny writers for the duration of the read, so one file cannot span two saves.
    Handle f{CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    check(f.h!=INVALID_HANDLE_VALUE,"Checkpoint is busy or unavailable.");
    return read_handle(f.h,max_file+sizeof(Header));
}
Digest sha256(const Bytes &bytes) { return hash_bytes(bytes.data(),bytes.size()); }
Info describe(const Bytes &native) { return native_info(native.data(),native.size()); }
Library::Library(const fs::path &root, std::string account, Shelf shelf, unsigned requested)
    : folder_(root/account), account_(std::move(account)), shelf_(shelf),
      initial_capacity_(requested ? requested : default_capacity(shelf)) {
    check(valid_account(account_),"A valid active Steam account is required.");
    check(valid_limit(initial_capacity_),"Retention must be between 1 and 100.");
    if(requested) {
        ArchiveLock lock(lock_name(shelf_,account_));
        const auto id=static_cast<uint32_t>(std::stoul(account_));
        auto policy=load_policy(folder_,id,shelf_,initial_capacity_);
        if(!policy.persisted) {
            check(scan(id,Inspection::full).healthy.size()<=requested,"Confirm the removal of existing saves before lowering retention.");
            fs::create_directories(folder_);
            write_policy(folder_,id,shelf_,policy);
        }
    }
}
fs::path Library::path(unsigned slot) const {
    check(slot>0 && slot<=slot_count(shelf_),"Quicksave slot is out of range.");
    return folder_/("slot-"+std::to_string(slot)+".dsqs");
}
std::vector<Entry> Library::list(unsigned *damaged) const {
    ArchiveLock lock(lock_name(shelf_,account_));
    const auto id=static_cast<uint32_t>(std::stoul(account_));
    // A corrupted setting or a locked pending delete must not hide healthy
    // records. Mutating paths report the exact error instead of guessing.
    try {
        auto policy=load_policy(folder_,id,shelf_,initial_capacity_);
        recover_policy(*this,folder_,id,policy);
    } catch(const std::runtime_error &) {}
    auto result=scan(id,Inspection::full);
    if(damaged) *damaged=result.damaged;
    return std::move(result.healthy);
}
Entry Library::store(const Bytes &native,uint64_t captured_at) const {
    check(native.size()<=max_file,"Native checkpoint is too large.");
    const auto info=describe(native);
    ArchiveLock lock(lock_name(shelf_,account_));
    fs::create_directories(folder_);
    const auto id=static_cast<uint32_t>(std::stoul(account_));
    auto policy=load_policy(folder_,id,shelf_,initial_capacity_);
    recover_policy(*this,folder_,id,policy);
    const auto inventory=scan(id,Inspection::reuse_verified);
    const auto &entries=inventory.healthy;
    check(entries.size()<=policy.capacity,"More saves exist than the selected limit. Review this list's retention setting.");
    const uint64_t sequence=entries.empty()?1:entries.front().sequence+1;
    check(sequence!=0,"Quicksave sequence overflow.");
    check(inventory.first_free || !entries.empty(),"Every slot in this save list is unreadable. Free one damaged slot to save again.");
    const bool replace_oldest=entries.size()>=policy.capacity || !inventory.first_free;
    const unsigned slot=replace_oldest ? entries.back().slot : inventory.first_free;
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    Header h{}; std::memcpy(h.magic,magic,sizeof magic);
    h.sequence=sequence; h.saved_at=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
    if(captured_at) h.saved_at=captured_at;
    h.account=std::stoull(account_); h.size=native.size(); h.digest=sha256(native);
    // Only this library's fixed staging file can be replaced after an interrupted write.
    // MoveFileEx publishes a complete, flushed record before retiring the oldest entry.
    const fs::path staging=folder_/L"pending.dsqs.tmp";
    check(DeleteFileW(staging.c_str()) || missing(GetLastError()),"Quicksave staging file is busy.");
    try {
        {
            Handle f{CreateFileW(staging.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
            check(f.h!=INVALID_HANDLE_VALUE,"Cannot write quicksave. Check free space and write access in the quicksave folder.");
            DWORD n=0;
            check(WriteFile(f.h,&h,sizeof h,&n,nullptr) && n==sizeof h &&
                WriteFile(f.h,native.data(),static_cast<DWORD>(native.size()),&n,nullptr) &&
                n==native.size() && FlushFileBuffers(f.h),"Quicksave write failed. Check free space.");
        }
        DWORD move_flags=MOVEFILE_WRITE_THROUGH;
        if(replace_oldest) {
            // Another tool can edit a slot while our new file is being flushed.
            // Re-check the former oldest record before replacing it.
            auto current=inspect_record(read_slot_file(path(slot)),slot,h.account,shelf_);
            check(matches(identity(entries.back()),current),
                  "The oldest quicksave changed. Try saving again.");
            move_flags|=MOVEFILE_REPLACE_EXISTING;
        }
        check(MoveFileExW(staging.c_str(),path(slot).c_str(),move_flags)!=0,
              "Could not publish the quicksave.");
    } catch (...) { DeleteFileW(staging.c_str()); throw; }
    return {slot,sequence,h.saved_at,info,h.digest,shelf_,static_cast<uint32_t>(h.account)};
}
Bytes Library::read(const Entry &selection) const {
    check(selection.shelf==shelf_ && selection.account==std::stoull(account_),"That quicksave belongs to another list or Steam account.");
    auto all=read_slot_file(path(selection.slot));
    const auto current=inspect_record(all,selection.slot,std::stoull(account_),shelf_);
    check(matches(identity(selection),current),"That quicksave changed. Refresh the list.");
    Bytes native(all.begin()+sizeof(Header),all.end());
    return native;
}
unsigned Library::capacity() const {
    ArchiveLock lock(lock_name(shelf_,account_));
    return load_policy(folder_,static_cast<uint32_t>(std::stoul(account_)),shelf_,initial_capacity_).capacity;
}
unsigned Library::set_capacity(unsigned requested,const std::vector<Entry> &expected) const {
    check(valid_limit(requested),"Retention must be between 1 and 100.");
    ArchiveLock lock(lock_name(shelf_,account_));
    const auto id=static_cast<uint32_t>(std::stoul(account_));
    auto policy=load_policy(folder_,id,shelf_,initial_capacity_);
    recover_policy(*this,folder_,id,policy);
    const auto actual=scan(id,Inspection::full);
    const char *changed="This save list changed. Refresh it and confirm the new removal count.";
    check(actual.healthy.size()==expected.size(),changed);
    std::array<const Entry *,limit+1> by_slot{};
    for(const auto &e:expected) {
        check(e.shelf==shelf_ && e.account==id && e.slot && e.slot<=limit && !by_slot[e.slot],changed);
        by_slot[e.slot]=&e;
    }
    for(const auto &e:actual.healthy) {
        check(by_slot[e.slot] && matches(identity(e),*by_slot[e.slot]),changed);
    }
    std::vector<OwnedFile> files;
    for(size_t i=actual.healthy.size();i>requested;--i) {
        auto r=identity(actual.healthy[i-1]);
        files.push_back(open_retired(*this,r,id,false));
        policy.pending.push_back(r);
    }
    const auto removed=static_cast<unsigned>(files.size());
    if(!removed && policy.persisted && policy.capacity==requested) return 0;
    policy.capacity=requested;
    fs::create_directories(folder_);
    write_policy(folder_,id,shelf_,policy); // publishes the user's decision before unlinking
    if(removed) {
        unlink_files(files);
        policy.pending.clear();
        write_policy(folder_,id,shelf_,policy);
    }
    return removed;
}
}
