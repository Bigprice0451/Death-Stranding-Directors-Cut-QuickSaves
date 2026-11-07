#pragma once
#include "library.hpp"

namespace qs {
// Allows the game's Load menu to read one archived checkpoint through a known
// manual slot. Call recover() only after the game has finished reading it.
class RestoreBridge {
public:
    RestoreBridge(fs::path nativeAccountFolder, fs::path recoveryFolderOnD);
    int stage(const Bytes &native);
    bool recover();
    bool pending() const;
private:
    fs::path native_, recovery_;
    std::string account_;
};
}
