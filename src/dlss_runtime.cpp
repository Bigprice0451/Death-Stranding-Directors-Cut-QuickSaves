#include "dlss_runtime.hpp"
#include <vector>

namespace qs::dlss {
namespace {
Version parse_info(const VS_FIXEDFILEINFO *info, unsigned length) {
    if(!info || length<sizeof(*info) || info->dwSignature!=0xfeef04bd) return {};
    return {HIWORD(info->dwFileVersionMS),LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS),LOWORD(info->dwFileVersionLS)};
}
}
std::filesystem::path module_path(HMODULE module) {
    std::wstring path(32768,L'\0');
    const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!length || length>=path.size()) return {};
    path.resize(length); return path;
}
Version file_version(const std::filesystem::path &file) {
    DWORD unused{},length=GetFileVersionInfoSizeW(file.c_str(),&unused);
    if(!length || length>4*1024*1024) return {};
    std::vector<uint8_t> buf(length);
    if(!GetFileVersionInfoW(file.c_str(),0,length,buf.data())) return {};
    VS_FIXEDFILEINFO *info=nullptr;UINT n{};
    if(!VerQueryValueW(buf.data(),L"\\",reinterpret_cast<void**>(&info),&n)) return {};
    return parse_info(info,n);
}
Version loaded_version() {
    HMODULE dll=nullptr;
    if(!GetModuleHandleExW(0,L"nvngx_dlss.dll",&dll)) return {};
    auto res=FindResourceW(dll,MAKEINTRESOURCEW(VS_VERSION_INFO),MAKEINTRESOURCEW(16));
    auto data=res?LoadResource(dll,res):nullptr;
    auto blob=data?LockResource(data):nullptr;
    VS_FIXEDFILEINFO *info=nullptr;UINT n{};Version version{};
    if(blob && VerQueryValueW(blob,L"\\",reinterpret_cast<void**>(&info),&n)) version=parse_info(info,n);
    FreeLibrary(dll);return version;
}
std::string version_text(const Version &v) {
    if(v==Version{}) return "unknown";
    return std::to_string(v[0])+"."+std::to_string(v[1])+"."+std::to_string(v[2]);
}
}
