#include "restore.hpp"
#include <windows.h>
#include <ktmw32.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <limits>
#include <stdexcept>
#ifdef _MSC_VER
#pragma comment(lib, "KtmW32.lib")
#endif

namespace qs {
namespace {
constexpr unsigned slot_count = 23;
constexpr size_t max_checkpoint = 128u * 1024u * 1024u;
constexpr char journal_magic[8] = "DSQJR01";
constexpr wchar_t journal_name[] = L"pending.dsqj";
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle &) = delete;
    ~Handle() { close(); }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
    void close() { if (valid()) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
};
struct Lock {
    Handle mutex;
    explicit Lock(const std::string &account) :
        mutex(CreateMutexA(nullptr,FALSE,("Local\\DeathStrandingRestore-"+account).c_str())) {
        auto result = mutex.valid() ? WaitForSingleObject(mutex.value,3000) : WAIT_FAILED;
        require(result == WAIT_OBJECT_0 || result == WAIT_ABANDONED,"Another quickload is using the native slot.");
    }
    ~Lock() { ReleaseMutex(mutex.value); }
};

// Keep the old file visible to other readers until one complete, checked change
// commits. Windows rolls back an uncommitted transaction on crash. A volume
// without transactional NTFS therefore rejects staging before a native write.
struct Transaction {
    Handle tx{CreateTransaction(nullptr,nullptr,0,0,0,0,nullptr)};
    Transaction() { require(tx.valid(),"Windows could not start a save-file transaction."); }
    void commit() { require(CommitTransaction(tx.value)!=FALSE,"The game changed or locked the slot. The temporary write was cancelled."); }
};
uint64_t ticks(FILETIME time) { return (uint64_t(time.dwHighDateTime)<<32) | time.dwLowDateTime; }
FILETIME filetime(uint64_t value) { return {DWORD(value),DWORD(value>>32)}; }
bool missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }

DWORD attributes(const fs::path &path) {
    DWORD flags = GetFileAttributesW(path.c_str());
    if (flags==INVALID_FILE_ATTRIBUTES) require(missing(GetLastError()),"A save or recovery path is inaccessible.");
    else require(!(flags & FILE_ATTRIBUTE_REPARSE_POINT),"A linked save or recovery path was rejected.");
    return flags;
}
void inspect_path(const fs::path &p) {
    require(p.is_absolute() && p.root_name().wstring().size()==2,"A local absolute save path is required.");
    for (const auto &component:p.relative_path()) {
        const auto name=component.wstring();
        require(!name.empty() && name!=L".." && name!=L"." && name.find(L':')==std::wstring::npos,
                "A save path contains an unsafe component.");
    }
    fs::path current=p.root_path();
    for (const auto &component:p.relative_path()) { current/=component; attributes(current); }
}
uint64_t directory_id(const fs::path &p, uint32_t &volume) {
    inspect_path(p);
    Handle h(CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    require(h.valid() && GetFileInformationByHandle(h.value,&info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),"The original Steam account directory is unavailable.");
    volume=info.dwVolumeSerialNumber;
    return (uint64_t(info.nFileIndexHigh)<<32) | info.nFileIndexLow;
}
fs::path slot_dir(const fs::path &account,unsigned slot) {
    require(slot<slot_count,"Invalid manual slot number.");
    return account/("manualsave"+std::to_string(slot));
}
// The game prefers slotinfo.ini if present, and may use checkpoint.dattemp while
// saving. A different member means this directory cannot be borrowed safely.
bool eligible(const fs::path &dir) {
    DWORD flags=GetFileAttributesW(dir.c_str());
    if (flags==INVALID_FILE_ATTRIBUTES) { require(missing(GetLastError()),"Cannot inspect native manual slot."); return false; }
    if (!(flags & FILE_ATTRIBUTE_DIRECTORY) || flags & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    for (const auto &e:fs::directory_iterator(dir)) {
        if (_wcsicmp(e.path().filename().c_str(),L"checkpoint.dat")!=0) return false;
        DWORD f=GetFileAttributesW(e.path().c_str());
        if (f==INVALID_FILE_ATTRIBUTES || f & (FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) return false;
    }
    return true;
}
struct Snapshot { Bytes data; uint64_t stamp=0; };
Snapshot read_handle(HANDLE h,size_t max_size) {
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(h,&info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
            && info.nNumberOfLinks==1,"The save file is linked or unreadable.");
    const auto size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    require(size>0 && size<=max_size,"Unexpected save or recovery record size.");
    Snapshot result{Bytes(static_cast<size_t>(size)),ticks(info.ftLastWriteTime)};
    DWORD actual=0;
    require(ReadFile(h,result.data.data(),static_cast<DWORD>(size),&actual,nullptr) && actual==size,"Could not read the whole save file.");
    return result;
}
void write_handle(HANDLE h,const Bytes &bytes,uint64_t stamp=0) {
    LARGE_INTEGER start{}; DWORD written=0;
    require(SetFilePointerEx(h,start,nullptr,FILE_BEGIN) && WriteFile(h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)
            && written==bytes.size() && SetEndOfFile(h),"Unable to write the complete save file.");
    if(stamp) { auto time=filetime(stamp); require(SetFileTime(h,nullptr,nullptr,&time)!=FALSE,"Could not set the native save time."); }
}
fs::path scratch_name(const fs::path &published) {
    fs::path tmp=published;
    tmp+=L".writing";
    return tmp;
}
void clear_scratch(const fs::path &published) {
    const auto tmp=scratch_name(published);
    if (attributes(tmp)!=INVALID_FILE_ATTRIBUTES)
        require(DeleteFileW(tmp.c_str())!=FALSE,"Cannot clear an unfinished recovery write.");
}
void publish(const fs::path &to,const Bytes &data,uint64_t stamp=0) {
    // The named mutex owns this scratch filename. Its presence after a crash can
    // only precede publishing the record and cannot imply a native write.
    clear_scratch(to);
    const auto tmp=scratch_name(to);
    Handle h(CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    require(h.valid(),"Cannot create recovery data on D:.");
    write_handle(h.value,data,stamp);
    require(FlushFileBuffers(h.value)!=FALSE,"Could not flush recovery data to D:.");
    h.close();
    require(MoveFileExW(tmp.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"Could not publish recovery data on D:.");
}

// All numeric fields have fixed widths and the entire record carries a SHA-256.
// Only the slot number can select a destination. Paths are never read from disk.
struct Header {
    char magic[8]{};
    uint32_t account{},volume{},slot{},reserved{};
    uint64_t directory{},old_time{},staged_time{},old_size{};
    Digest staged_digest{};
};
static_assert(sizeof(Header)==88);
struct Record { Header h{}; Bytes original; };
Bytes encode(const Record &record) {
    Bytes bytes(sizeof(Header)+record.original.size());
    std::memcpy(bytes.data(),&record.h,sizeof(Header));
    if(!record.original.empty()) std::memcpy(bytes.data()+sizeof(Header),record.original.data(),record.original.size());
    auto sum=sha256(bytes);
    bytes.insert(bytes.end(),sum.begin(),sum.end());
    return bytes;
}
Record decode(const fs::path &file,const fs::path &account,const std::string &id) {
    attributes(file);
    Handle h(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(h.valid(),"Cannot open quickload recovery record.");
    auto bytes=read_handle(h.value,max_checkpoint+sizeof(Header)+sizeof(Digest)).data;
    require(bytes.size()>=sizeof(Header)+sizeof(Digest),"Quickload recovery record is incomplete.");
    Digest sum{};std::copy(bytes.end()-sum.size(),bytes.end(),sum.begin());
    bytes.resize(bytes.size()-sum.size());
    require(sha256(bytes)==sum,"Quickload recovery record failed its integrity check.");
    Record r; std::memcpy(&r.h,bytes.data(),sizeof(Header));uint32_t volume=0;
    const auto dir=directory_id(account,volume);
    require(std::memcmp(r.h.magic,journal_magic,8)==0 && r.h.account==std::stoul(id)
            && r.h.volume==volume && r.h.directory==dir && r.h.slot<slot_count && r.h.reserved==0 && r.h.staged_time
            && r.h.old_size==bytes.size()-sizeof(Header) && (r.h.old_time || !r.h.old_size),
            "Quickload journal belongs to a different profile, or has an invalid slot.");
    r.original.assign(bytes.begin()+sizeof(Header),bytes.end());
    return r;
}
HANDLE open_slot(const fs::path &file,DWORD access,DWORD creation,HANDLE tx) {
    return CreateFileTransactedW(file.c_str(),access,FILE_SHARE_READ,nullptr,creation,
                                 FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr,tx,nullptr,nullptr);
}
void preserve_displaced(const fs::path &folder,const Record &r) {
    if (r.original.empty()) return;
    // A later save owns the native slot. Keep one separately usable native
    // checkpoint on D:; refuse to discard a previous unresolved original.
    auto path=folder/L"displaced-checkpoint.dat";
    if(attributes(path)==INVALID_FILE_ATTRIBUTES) publish(path,r.original,r.h.old_time);
    else require(read_file(path)==r.original,"Move the earlier displaced-checkpoint.dat out of recovery before retrying.");
}
}

RestoreBridge::RestoreBridge(fs::path nativeAccountFolder,fs::path recoveryFolderOnD)
    : native_(std::move(nativeAccountFolder)), account_(native_.filename().string()) {
    require(valid_account(account_),"A numeric Steam profile is required for quickload.");
    inspect_path(native_);inspect_path(recoveryFolderOnD);
    require(_wcsicmp(recoveryFolderOnD.root_name().c_str(),L"D:")==0,"Quickload recovery must be on D:.");
    recovery_=recoveryFolderOnD/account_;
    inspect_path(recovery_);
}
bool RestoreBridge::pending() const {
    Lock lock(account_);
    inspect_path(recovery_);
    const auto journal=recovery_/journal_name;
    return attributes(journal)!=INVALID_FILE_ATTRIBUTES || attributes(scratch_name(journal))!=INVALID_FILE_ATTRIBUTES;
}
int RestoreBridge::stage(const Bytes &native) {
    require(!native.empty() && native.size()<=max_checkpoint,"Invalid native quicksave size.");
    describe(native);
    const auto digest=sha256(native);
    Lock lock(account_);
    inspect_path(recovery_);uint32_t volume=0;
    const auto id=directory_id(native_,volume);
    const auto journal=recovery_/journal_name;
    clear_scratch(journal);
    if(attributes(journal)!=INVALID_FILE_ATTRIBUTES) {
        auto r=decode(journal,native_,account_);
        const auto dir=slot_dir(native_,r.h.slot);
        if(r.h.staged_digest==digest && eligible(dir)) {
            const auto file=dir/L"checkpoint.dat";
            Handle h(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            if(h.valid()) { auto now=read_handle(h.value,max_checkpoint);
                if(now.stamp==r.h.staged_time && now.data==native) return static_cast<int>(r.h.slot); }
        }
        throw std::runtime_error("Finish the previous quickload recovery before staging a new selection.");
    }
    int free_slot=-1, oldest_slot=-1; uint64_t oldest_time=UINT64_MAX;
    for(unsigned i=0;i<slot_count;++i) {
        const auto dir=slot_dir(native_,i);
        if(!eligible(dir)) continue;
        const auto file=dir/L"checkpoint.dat";
        Handle h(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h.valid()) {
            require(missing(GetLastError()),"A manual slot is busy. Try quickload again.");
            if(free_slot<0) free_slot=static_cast<int>(i);
            continue;
        }
        const auto current=read_handle(h.value,max_checkpoint);
        if(current.data==native) return static_cast<int>(i);
        if(current.stamp<oldest_time) { oldest_time=current.stamp;oldest_slot=static_cast<int>(i); }
    }
    int slot=free_slot>=0?free_slot:oldest_slot;
    require(slot>=0,"No registered manual slot is available for quickload.");
    auto dir=slot_dir(native_,slot);
    require(eligible(dir),"The game is updating the chosen manual slot.");
    const auto file=dir/L"checkpoint.dat";
    Transaction t;
    Handle h(open_slot(file,GENERIC_READ|GENERIC_WRITE,free_slot>=0?CREATE_NEW:OPEN_EXISTING,t.tx.value));
    require(h.valid(),"Could not lock a native slot. Quickload requires local transactional NTFS.");
    Record r{};
    if(free_slot<0) {
        auto original=read_handle(h.value,max_checkpoint);
        require(original.stamp==oldest_time,"The game just changed the chosen slot. Try quickload again.");
        describe(original.data);
        r.original=std::move(original.data); r.h.old_time=original.stamp;
    }
    FILETIME now{};GetSystemTimeAsFileTime(&now);
    std::memcpy(r.h.magic,journal_magic,8);r.h.account=std::stoul(account_);
    r.h.slot=static_cast<unsigned>(slot);r.h.volume=volume;r.h.directory=id;
    r.h.staged_time=ticks(now);r.h.staged_digest=digest;r.h.old_size=r.original.size();
    fs::create_directories(recovery_);inspect_path(recovery_);
    publish(journal,encode(r)); // The original is durable before changing the native file.
    write_handle(h.value,native,r.h.staged_time);
    h.close();
    require(eligible(dir),"The game started writing this native slot. The slot change was cancelled.");
    t.commit();
    return slot;
}
bool RestoreBridge::recover() {
    Lock lock(account_);
    inspect_path(recovery_);
    auto journal=recovery_/journal_name;
    clear_scratch(journal);
    if(attributes(journal)==INVALID_FILE_ATTRIBUTES) return false;
    auto r=decode(journal,native_,account_);auto dir=slot_dir(native_,r.h.slot);auto file=dir/L"checkpoint.dat";
    require(eligible(dir),"The staged slot has additional game files. Cleanup will retry after they clear.");
    Transaction t;
    Handle h(open_slot(file,GENERIC_READ|GENERIC_WRITE|DELETE,OPEN_EXISTING,t.tx.value));
    if(h.valid()) {
        const auto current=read_handle(h.value,max_checkpoint);
        if(current.stamp==r.h.staged_time && sha256(current.data)==r.h.staged_digest) {
            if(r.original.empty()) {
                FILE_DISPOSITION_INFO remove{TRUE};
                require(SetFileInformationByHandle(h.value,FileDispositionInfo,&remove,sizeof(remove))!=FALSE,
                        "Cannot release the temporary native slot.");
            } else {
                write_handle(h.value,r.original,r.h.old_time);
            }
        } else if(current.data!=r.original) {
            preserve_displaced(recovery_,r);
        }
    } else {
        require(missing(GetLastError()),"The native save is busy. Quickload recovery will retry.");
        preserve_displaced(recovery_,r);
    }
    h.close();
    require(eligible(dir),"The game started writing this slot. Recovery will retry.");
    t.commit();
    require(DeleteFileW(journal.c_str())!=FALSE,"The native slot was released; its journal still needs cleanup.");
    return true;
}
}
