#include "data_root.hpp"
#include <windows.h>
#include <shlobj.h>

namespace qs {
fs::path choose_data_root(const fs::path &saved_games,const fs::path &local_appdata,
                          const fs::path &legacy_root) {
    // The original local playtest stored its library here. Only recognize an
    // existing layout. A first-time install must never assume D: exists.
    std::error_code ec;
    if(legacy_root.is_absolute() && fs::is_directory(legacy_root/L"QuickSaves",ec)) return legacy_root;
    for(const auto *folder:{&saved_games,&local_appdata})
        if(folder->is_absolute()) return *folder/L"DirectorCutQuickSaves";
    return {};  // A missing Windows profile must never create relative save files.
}
namespace {
fs::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR path=nullptr;
    const auto status=SHGetKnownFolderPath(id,KF_FLAG_DONT_VERIFY,nullptr,&path);
    fs::path result;
    if(SUCCEEDED(status) && path) result=path;
    CoTaskMemFree(path);
    return result;
}
}
const fs::path &data_root() {
    static const fs::path root=choose_data_root(known_folder(FOLDERID_SavedGames),
        known_folder(FOLDERID_LocalAppData),LR"(D:\Game Saves\Death Stranding Director's Cut)");
    return root;
}
}
