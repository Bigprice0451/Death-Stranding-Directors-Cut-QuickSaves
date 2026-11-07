#include "private_load.hpp"
#include "private_load_simulation.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
template<class T> void put(qs::Bytes& image, std::size_t offset, T value) {
    std::memcpy(image.data() + offset, &value, sizeof value);
}

qs::Bytes fixture() {
    qs::Bytes image(qs::native_header_size + 3);
    image[0x24] = 0x43;
    put<std::uint16_t>(image, 0x26, 511);
    put<std::uint32_t>(image, 0x2c, 0x4c);
    put<std::uint32_t>(image, 0x1fc, 3);
    // Published MD5 test vector for the ASCII bytes "abc".
    const unsigned char md5[] = {0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,
                                 0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
    std::copy(std::begin(md5), std::end(md5), image.begin() + 0x1ec);
    std::memcpy(image.data() + qs::native_header_size, "abc", 3);
    return image;
}

template<class Modify> void expect_rejected(Modify modify) {
    auto image = fixture();
    modify(image);
    try { qs::validate_native_image(image); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Malformed native quicksave passed validation");
}
}

int main() {
    try {
        const auto valid = fixture();
        qs::validate_native_image(valid);
        if (qs::native_world_id(valid) != 511) throw std::runtime_error("Wrong world ID");
        expect_rejected([](auto& b) { b.resize(0x1ff); });
        expect_rejected([](auto& b) { b.resize(qs::native_bank_size + 1); });
        expect_rejected([](auto& b) { b[0x24]--; });
        expect_rejected([](auto& b) { put<std::uint16_t>(b, 0x26, 0); });
        expect_rejected([](auto& b) { put<std::uint32_t>(b, 0x2c, 0xffff); });
        expect_rejected([](auto& b) { put<float>(b, 0x1e0, std::numeric_limits<float>::infinity()); });
        expect_rejected([](auto& b) { put<std::uint32_t>(b, 0x1fc, 0xffffffffu); });
        expect_rejected([](auto& b) { b.pop_back(); });
        expect_rejected([](auto& b) { b[0x1ec] ^= 1; });
        expect_rejected([](auto& b) { b.back() ^= 1; });
        qs::PrivateLoad loader;
        bool rejected = false;
        try { loader.begin(0, valid); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected || loader.poll().state != qs::LoadState::idle)
            throw std::runtime_error("An unverified host changed load state");
        load_test::exercise(valid);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
