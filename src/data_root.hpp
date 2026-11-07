#pragma once
#include "library.hpp"

namespace qs {
// Kept separate from the Windows lookup so redirected folders, upgrades, and
// missing profile locations can be tested without touching anyone's saves.
fs::path choose_data_root(const fs::path &saved_games, const fs::path &local_appdata,
                          const fs::path &legacy_root);
const fs::path &data_root();
}
