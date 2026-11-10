#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace qs {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using Digest = std::array<uint8_t, 32>;
constexpr unsigned limit = 100;
constexpr unsigned autosave_limit = 15;
enum class Shelf { manual, timed };
constexpr unsigned slot_count(Shelf) { return limit; } // physical slots, independent of the player's limit
constexpr unsigned default_capacity(Shelf shelf) { return shelf==Shelf::timed ? autosave_limit : limit; }
struct Info { std::string title, location; };
struct Entry {
    unsigned slot{};
    uint64_t sequence{}, saved_at{};
    Info info;
    Digest digest{};
    Shelf shelf=Shelf::manual;
    uint32_t account{};
};
Bytes read_file(const fs::path &file);
Digest sha256(const Bytes &bytes);
Info describe(const Bytes &native);
bool valid_account(const std::string &id);
class Library {
public:
    // root is the shelf's parent (QuickSaves or AutoSaves). Zero uses that
    // shelf's default; an existing folder's saved preference always wins.
    Library(const fs::path &root, std::string account, Shelf shelf=Shelf::manual, unsigned capacity=0);
    Shelf shelf() const { return shelf_; }
    fs::path path(unsigned slot) const;
    // Damaged or unreadable slots stay on disk and count toward the 100-slot limit.
    std::vector<Entry> list(unsigned *damaged = nullptr) const;
    Entry store(const Bytes &native, uint64_t captured_at = 0) const;
    Bytes read(const Entry &selection) const;
    unsigned capacity() const;
    // Full healthy list the player saw before confirming. Order is immaterial.
    // Returns the number retired. Stale selections never authorize removal.
    unsigned set_capacity(unsigned requested, const std::vector<Entry> &expected_healthy) const;
private:
    // Only verified entries are remembered, for this shelf and this process.
    // All reads and writes to this array run under the shelf's archive mutex.
    struct VerifiedSlot {
        uint64_t file_id{}, change_number{}, size{}, write_time{};
        uint32_t volume{};
        Entry entry;
    };
    struct Inventory;
    enum class Inspection { full, reuse_verified };
    Inventory scan(uint32_t account, Inspection inspection) const;
    mutable std::array<VerifiedSlot,limit> verified_{};
    fs::path folder_;
    std::string account_;
    Shelf shelf_;
    unsigned initial_capacity_;
};
}
