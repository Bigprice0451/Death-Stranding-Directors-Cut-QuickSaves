#include "native_image.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace qs {
namespace {
template<class T> T value_at(const Bytes& image, std::size_t offset) {
    T result{};
    std::memcpy(&result, image.data() + offset, sizeof result);
    return result;
}
}

std::uint16_t native_world_id(const Bytes& image) {
    if (image.size() < native_header_size) throw std::runtime_error("The quicksave header is incomplete.");
    return value_at<std::uint16_t>(image, 0x26);
}

void validate_native_image(const Bytes& image) {
    if (image.size() <= native_header_size || image.size() > native_bank_size)
        throw std::runtime_error("The quicksave is outside the supported world snapshot size.");
    if (image[0x24] != 0x43 || !native_world_id(image))
        throw std::runtime_error("The quicksave has an unsupported version or missing world ID.");
    // The engine copies this many bytes into a local structure before restoring.
    // Build 13300582's writer returns 0x4C at RVA 0x358A60F.
    if (value_at<std::uint32_t>(image, 0x2c) != 0x4c)
        throw std::runtime_error("The quicksave's activity header is invalid.");
    for (std::size_t offset = 0x1e0; offset <= 0x1e8; offset += sizeof(float))
        if (!std::isfinite(value_at<float>(image, offset)))
            throw std::runtime_error("The quicksave's recorded position is invalid.");
    const auto length = value_at<std::uint32_t>(image, 0x1fc);
    if (length != image.size() - native_header_size)
        throw std::runtime_error("The quicksave's payload length differs from its file size.");

    // The engine verifies MD5 at 0x3633F1E for disk files. Its memory route
    // trusts game-owned buffers, so perform that same check before submitting.
    // The library also checks a separate SHA-256 that covers the entire image.
    std::array<unsigned char, 16> digest{};
    auto* payload = const_cast<unsigned char*>(image.data() + native_header_size);
    if (BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, payload, length, digest.data(),
                   static_cast<ULONG>(digest.size())) < 0)
        throw std::runtime_error("Windows could not verify the native quicksave digest.");
    if (!std::equal(digest.begin(), digest.end(), image.begin() + 0x1ec))
        throw std::runtime_error("The quicksave's native payload digest does not match.");
}
}
