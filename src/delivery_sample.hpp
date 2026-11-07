#pragma once
#include "library.hpp"
#include <optional>

namespace qs {
struct DeliverySample {
    std::uint32_t active_orders{};
    std::uint32_t delivered_count{};
    Digest digest{};
};

// Called on an ordinary game frame, using the build-verified image base.
// A missing sample is distinct from an empty order list. See NATIVE_CAPTURE.md.
std::optional<DeliverySample> sample_delivery_state(std::uintptr_t validated_base) noexcept;
std::optional<std::uint32_t> sample_carried_cargo_count(std::uintptr_t validated_base) noexcept;
}
