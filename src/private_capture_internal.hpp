#pragma once
#include "library.hpp"

// The public entry point in private_capture.hpp supplies the real game bindings.
// These lower-level operations can also be exercised with a simulated scheduler,
// without starting the game, using any save slot, or reading the player's files.
namespace qs::capture_detail {
using JobStep = void (*)(void*);
struct CaptureBindings {
    JobStep prepare, start, join;
    const void *request_type, *empty_string, *default_player_reference;
};
bool has_regular_callers(std::uintptr_t image, void* const* stack, std::size_t depth);
void require_idle_controller(const void* native_controller);
Bytes run_capture_job(const CaptureBindings& engine, const void* world_header);
}
