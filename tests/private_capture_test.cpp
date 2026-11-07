#include "private_capture.hpp"
#include "private_capture_internal.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace qs;
using namespace qs::capture_detail;
enum class Outcome { success, aborted, bad_length, bad_digest };
Outcome outcome{};
int step{};
std::array<std::uint8_t, 0x24> header{};
std::array<std::uint8_t, 16> player_reference{};

void expect(bool condition) {
    if (!condition) throw std::logic_error("Private capture contract failed.");
}
template<class T> T get(const void* p, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(p) + offset, sizeof(value));
    return value;
}
template<class T> void put(void* p, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(p) + offset, &value, sizeof(value));
}
void* queued_request(void* context) {
    return *get<void**>(context, 0x90);
}
void prepare(void* context) {
    expect(step++ == 0);
    expect(get<int>(context, 0x88) == 1 && get<int>(context, 0x8c) == 1);
    expect(std::memcmp(static_cast<std::uint8_t*>(context) + 0x108, header.data(), header.size()) == 0);
    expect(get<void*>(context, 0xb8) && !get<void*>(context, 0xc0) && !get<void*>(context, 0xc8));
    auto request = queued_request(context);
    expect(get<int>(request, 8) == 1 && get<int>(request, 0xc) == 1);
    expect(get<std::uint8_t>(request, 0x10) == 1 && get<std::uint8_t>(request, 0x11) == 0xfe);
    expect(!get<std::uint8_t>(request, 0x19) && !get<std::uint8_t>(request, 0x70));
    expect(std::memcmp(static_cast<std::uint8_t*>(request) + 0x1a, player_reference.data(), 16) == 0);
    put(request, 0xc, 2);
    put(request, 8, 2);
    put(context, 0x98, request);
    put(context, 0xa0, request);
}
void start(void* context) {
    expect(step++ == 1 && get<int>(context, 0xa8) == 0);
    put(context, 0xa8, 1);
}
void join(void* context) {
    expect(step++ == 2 && get<int>(context, 0xa8) == 1);
    auto bank = get<std::uint8_t*>(context, 0xb8);
    auto request = queued_request(context);
    put(context, 0x98, static_cast<void*>(nullptr));
    put(context, 0xa0, static_cast<void*>(nullptr));
    put(context, 0xa8, 0);
    put(request, 8, 1);
    put(request, 0xc, outcome == Outcome::aborted ? 5 : 3);
    bank[0x24] = 0x43;
    put(bank, 0x26, std::uint16_t{1});
    put(bank, 0x2c, 0x4c);
    put(bank, 0x1fc, outcome == Outcome::bad_length ? 0x7fffffff : 3);
    constexpr std::uint8_t abc_md5[]{0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,
                                     0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
    std::memcpy(bank + 0x1ec, abc_md5, 16);
    std::memcpy(bank + 0x200, "abc", 3);
    if (outcome == Outcome::bad_digest) bank[0x1ec] ^= 1;
}
}

int main() {
    try {
        constexpr std::uintptr_t base = 0x140000000;
        void* regular[]{reinterpret_cast<void*>(base + 0x263d2ae), reinterpret_cast<void*>(base + 0x263a869)};
        expect(has_regular_callers(base, regular, 2));
        expect(!has_regular_callers(base, regular, 1));
        expect(!has_regular_callers(base, regular + 1, 1));
        expect(!regular_capture_frame(base));

        std::array<std::uint8_t, 0x150> vanilla{};
        require_idle_controller(vanilla.data());
        for (std::size_t offset : {0x88, 0x98, 0xa0, 0xa8}) {
            vanilla[offset] = 1;
            bool rejected = false;
            try { require_idle_controller(vanilla.data()); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected && vanilla[offset] == 1 && step == 0);
            vanilla[offset] = 0;
        }

        header.fill(0x42);
        player_reference.fill(0xf2);
        char type{}, empty{};
        CaptureBindings bindings{prepare, start, join, &type, &empty, player_reference.data()};
        for (auto result : {Outcome::success, Outcome::aborted, Outcome::bad_length, Outcome::bad_digest}) {
            outcome = result;
            step = 0;
            bool rejected = false;
            try {
                auto image = run_capture_job(bindings, header.data());
                expect(image.size() == 0x203 && image.back() == 'c');
            } catch (const std::runtime_error&) { rejected = true; }
            expect(step == 3 && rejected == (result != Outcome::success));
        }
        expect(header[0] == 0x42 && player_reference[0] == 0xf2);
        std::cout << "Private capture dispatch, isolation, refusal and integrity checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
