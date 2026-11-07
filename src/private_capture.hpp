#pragma once
#include "library.hpp"
#include <cstdint>

namespace qs {
// The window procedure also runs while the game is loading. This identifies
// the ordinary frame call chain where save/load requests may be processed.
bool regular_capture_frame(std::uintptr_t supported_image_base);

// Call on the supported game's regular main-thread frame boundary, with the
// executable base already verified by Game. Joins the native job before return.
Bytes capture_private_world(std::uintptr_t supported_image_base);
}
