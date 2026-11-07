#pragma once
#include "delivery_sample.hpp"
#include "autosave.hpp"
#include "data_root.hpp"
#include <optional>

namespace qs {
struct WorldSample {
    uint64_t flags{};
    std::optional<std::array<double,3>> position;
    std::optional<DeliverySample> delivery;
    std::optional<uint32_t> carried_count;
    bool cinematic() const { return (flags & scene_bits)!=0; }
};
struct Event {
    std::string name, detail;
    uint64_t request{}, sequence{}, elapsed_ms{};
    std::optional<WorldSample> world;
    std::optional<double> distance;
};
class Telemetry {
public:
    explicit Telemetry(fs::path folder);
    bool write(const Event &event) const noexcept;
    fs::path current() const { return folder_.empty() ? fs::path{} : folder_/L"quicksaves.jsonl"; }
private:
    fs::path folder_;
};
Telemetry &telemetry();
}
