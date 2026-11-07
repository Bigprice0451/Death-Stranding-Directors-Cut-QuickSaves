#include "data_root.hpp"
#include <windows.h>
#include <iostream>
using namespace qs;
int main() {
    const auto scratch=fs::current_path()/("profile-test-"+std::to_string(GetCurrentProcessId()));
    try {
        const auto saved=scratch/L"Redirected user"/L"Saved Games";
        const auto local=scratch/L"LocalAppData",old=scratch/L"Original campaign";
        if(choose_data_root(saved,local,old)!=saved/L"DirectorCutQuickSaves" ||
           choose_data_root({},local,old)!=local/L"DirectorCutQuickSaves" ||
           !choose_data_root({},{},old).empty() || fs::exists(old))
            throw std::runtime_error("Incorrect first-install folder selection.");
        fs::create_directories(old/L"QuickSaves");
        if(choose_data_root(saved,local,old)!=old)
            throw std::runtime_error("The playtest's existing library was lost.");
        fs::remove_all(scratch);
        return 0;
    } catch(const std::exception &error) {
        fs::remove_all(scratch);std::cerr<<error.what();return 1;
    }
}
