#pragma once
#include "native_image.hpp"

namespace qs {
enum class LoadState { idle, loading, completed, interrupted };
struct PrivateLoadStatus {
    LoadState state = LoadState::idle;
    int native_phase = 0;
    // World units between the 3 saved doubles decoded by the native reader
    // and the live player entity after streaming.
    // Negative means the player entity was unavailable for measurement.
    double position_difference = -1;
    bool world_position_override = false;
};

class PrivateLoad {
public:
    // The installed add-on uses normal module-relative addresses. The offline
    // handoff test supplies a simulated engine with the same request layout.
    using AddressLookup = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t);
    explicit PrivateLoad(AddressLookup lookup = nullptr) : lookup_(lookup) {}
    PrivateLoad(const PrivateLoad&) = delete;
    PrivateLoad& operator=(const PrivateLoad&) = delete;
    ~PrivateLoad();

    // Both calls belong to the game's main update thread. verified_base must
    // come from Game after checking the executable's complete hash and size.
    void begin(std::uintptr_t verified_base, const Bytes& image);
    PrivateLoadStatus poll();

private:
    AddressLookup lookup_{};
    std::uintptr_t base_{};
    unsigned char* world_{};
    void* request_{};
    unsigned char* image_{};
    std::array<double, 3> position_{};
    std::uint64_t position_deadline_{};
    PrivateLoadStatus status_{};
    void free_image();
};
}
