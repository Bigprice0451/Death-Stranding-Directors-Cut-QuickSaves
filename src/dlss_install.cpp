#include "dlss_runtime.hpp"
#include "library.hpp"
#include "telemetry.hpp"
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <string>
#include <cwchar>
#include <iomanip>
#include <sstream>

namespace {
using namespace qs;
using namespace qs::dlss;
struct Handle { HANDLE h{}; ~Handle(){if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h);} };
struct Internet { HINTERNET h{}; ~Internet(){if(h) WinHttpCloseHandle(h);} };
struct Temporary { fs::path file; ~Temporary(){if(!file.empty()) DeleteFileW(file.c_str());} };

// Verify the Authenticode chain and the leaf signer. A valid signature from an
// unrelated vendor is not permission to replace their DLL with NVIDIA's.
bool nvidia_signed(const fs::path &file) {
    WINTRUST_FILE_INFO in{sizeof(in)};in.pcwszFilePath=file.c_str();
    WINTRUST_DATA trust{sizeof(trust)};trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE;trust.dwUnionChoice=WTD_CHOICE_FILE;
    trust.pFile=&in;trust.dwStateAction=WTD_STATEACTION_VERIFY;
    trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID purpose=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status=WinVerifyTrust(nullptr,&purpose,&trust);
    wchar_t name[256]{};
    if(status==ERROR_SUCCESS) {
        auto chain=WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto signer=chain?WTHelperGetProvSignerFromChain(chain,0,FALSE,0):nullptr;
        if(signer && signer->csCertChain && signer->pasCertChain[0].pCert)
            CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,nullptr,name,256);
    }
    trust.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&purpose,&trust);
    return status==ERROR_SUCCESS && _wcsicmp(name,L"NVIDIA Corporation")==0;
}
bool cancelled(HANDLE cancel) {return cancel && WaitForSingleObject(cancel,0)==WAIT_OBJECT_0;}

Result fetch(HANDLE out,HANDLE cancel) {
    // Exact revision and path. The hash, size and NVIDIA signature are checked
    // independently before publication. Never execute downloaded setup code.
    Internet session{WinHttpOpen(L"DirectorCutQuickSaves/0.2",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.h) return download_failed;
    WinHttpSetTimeouts(session.h,15000,15000,30000,30000);
    URL_COMPONENTS parts{sizeof(parts)};parts.dwHostNameLength=parts.dwUrlPathLength=DWORD(-1);
    if(!WinHttpCrackUrl(url,0,0,&parts) || parts.nScheme!=INTERNET_SCHEME_HTTPS) return download_failed;
    std::wstring host(parts.lpszHostName,parts.dwHostNameLength),path(parts.lpszUrlPath,parts.dwUrlPathLength);
    Internet conn{WinHttpConnect(session.h,host.c_str(),parts.nPort,0)};
    if(!conn.h) return download_failed;
    Internet req{WinHttpOpenRequest(conn.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    if(!req.h || !WinHttpSendRequest(req.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,nullptr,0,0,0)
              || !WinHttpReceiveResponse(req.h,nullptr)) return download_failed;
    DWORD status{},n=sizeof(status);
    if(!WinHttpQueryHeaders(req.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&n,nullptr) ||
       status!=200) return download_failed;
    std::array<uint8_t,128*1024> buffer{}; DWORD total=0;
    for(;;) {
        if(cancelled(cancel)) return canceled;
        DWORD count=0;
        if(!WinHttpReadData(req.h,buffer.data(),DWORD(buffer.size()),&count)) return download_failed;
        if(!count) break;
        if(count>expected_bytes-total) return verification_failed;
        DWORD written{};if(!WriteFile(out,buffer.data(),count,&written,nullptr) || written!=count) return no_write_access;
        total+=count;
    }
    if(total!=expected_bytes) return verification_failed;
    return FlushFileBuffers(out)?installed:no_write_access;
}

Result inspect_target(const fs::path &file) {
    DWORD attrs=GetFileAttributesW(file.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES)
        return GetLastError()==ERROR_FILE_NOT_FOUND?installed:no_write_access;
    if((attrs & (FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) || !nvidia_signed(file)) return other_dll;
    return file_version(file)>=required?current:installed;
}
Result update(DWORD pid) {
    const auto own=module_path(nullptr);
    if(own.empty()) return wrong_location;
    const auto folder=own.parent_path().parent_path();
    const auto game=folder/L"ds.exe", addon=folder/L"DirectorCutQuickSaves.addon64", target=folder/L"nvngx_dlss.dll";
    if(!fs::is_regular_file(game) || !fs::is_regular_file(addon)) return wrong_location;
    Handle process, cancel,ready;
    if(pid) {
        process.h=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(!process.h) return wrong_location;
        wchar_t path[32768]; DWORD count=32768;
        if(!QueryFullProcessImageNameW(process.h,0,path,&count) || !fs::equivalent(fs::path(path),game))
            return wrong_location;
        const auto name=L"Local\\DSDC_DLSS_CANCEL_"+std::to_wstring(pid);
        cancel.h=OpenEventW(SYNCHRONIZE,FALSE,name.c_str());
        ready.h=OpenEventW(EVENT_MODIFY_STATE,FALSE,(L"Local\\DSDC_DLSS_READY_"+std::to_wstring(pid)).c_str());
        if(!cancel.h || !ready.h) return wrong_location;
    }
    Handle mutex{CreateMutexW(nullptr,FALSE,L"Local\\DSDC_Official_DLSS_Upgrade")};
    if(!mutex.h) return install_failed;
    DWORD lock=WaitForSingleObject(mutex.h,0);if(lock!=WAIT_OBJECT_0 && lock!=WAIT_ABANDONED) return already_running;
    struct Unlock {HANDLE h; ~Unlock(){ReleaseMutex(h);}} unlock{mutex.h};
    if(auto result=inspect_target(target);result!=installed) return result;
    fs::path stage=folder/(L".DSQuickSaves-DLSS-"+std::to_wstring(GetCurrentProcessId())+L".part");
    Temporary temp{};
    {
        Handle out{CreateFileW(stage.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(out.h==INVALID_HANDLE_VALUE) return no_write_access;
        temp.file=stage;
        auto result=fetch(out.h,cancel.h);if(result!=installed) return result;
    }
    const auto digest=sha256(read_file(stage)); std::ostringstream hex;hex<<std::hex<<std::setfill('0');
    for(auto b:digest) hex<<std::setw(2)<<unsigned(b);
    if(hex.str()!=hash || file_version(stage)!=required || !nvidia_signed(stage)) return verification_failed;
    // Let this game's current NGX session close before changing any live DLL.
    // Toggling off while it waits signals cancellation and removes only staging.
    if(process.h) {
        SetEvent(ready.h);
        HANDLE handles[]={cancel.h,process.h};
        auto wait=WaitForMultipleObjects(2,handles,FALSE,INFINITE);
        if(wait!=WAIT_OBJECT_0+1) return canceled;
    }
    if(cancelled(cancel.h) || !fs::exists(addon)) return canceled;
    if(auto final=inspect_target(target);final!=installed) return final;
    return MoveFileExW(stage.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)
        ?installed:install_failed;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR args,int) {
    // With no argument the same helper can be run while the game is closed.
    // The in-game switch passes only its own numeric PID, never a target path.
    try {
        wchar_t *end=nullptr;auto pid=wcstoul(args,&end,10);
        if(!end || *end || (*args && !pid)) return qs::dlss::wrong_location;
        const auto result=update(pid);
        if(pid) qs::telemetry().write({"dlss_runtime_update","Installer result "+std::to_string(static_cast<DWORD>(result))});
        return result;
    } catch(...) {return qs::dlss::install_failed;}
}
