#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <windows.h>

namespace qs::dlss {
using Version=std::array<unsigned,4>;
// Pinned together: the release tag, exact x64 production bytes, and digest.
inline constexpr Version required{310,9,16,0};
inline constexpr wchar_t url[]=L"https://raw.githubusercontent.com/NVIDIA/DLSS/9981c6350dc58c0ce21f024fd65e31e851c31689/lib/Windows_x86_64/rel/nvngx_dlss.dll";
inline constexpr char hash[]="d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e";
inline constexpr DWORD expected_bytes=58956912;

std::filesystem::path module_path(HMODULE module);
Version file_version(const std::filesystem::path &file);
Version loaded_version(); // Reads the mapped version resource even after a disk update.
std::string version_text(const Version &v);
enum Result : DWORD { installed=0, current=1, canceled=2, wrong_location=3, no_write_access=4,
    download_failed=5, verification_failed=6, other_dll=7, install_failed=8, already_running=9 };
}
