#pragma once
#include "library.hpp"

namespace qs {
inline constexpr std::size_t native_header_size = 0x200;
inline constexpr std::size_t native_bank_size = 0xC00000;

// An image is exactly the current build's 512-byte header and its payload.
// Throws before a malformed length or version reaches the game reader.
void update_native_payload_digest(Bytes& image);
void validate_native_image(const Bytes& image);
std::uint16_t native_world_id(const Bytes& validated_image);
}
