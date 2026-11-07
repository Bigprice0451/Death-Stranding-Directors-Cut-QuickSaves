#pragma once
#include "game.hpp"
#include "private_load.hpp"
#include "private_record.hpp"
#include "post_load_check.hpp"
#include <future>
#include <memory>
#include <mutex>

namespace qs {
enum class Action { start, refresh, save, timed_save, load, resize };
struct View {
    std::string message="Starting quicksaves...";
    std::shared_ptr<const std::vector<Entry>> entries=std::make_shared<const std::vector<Entry>>();
    std::shared_ptr<const std::vector<Entry>> auto_entries=std::make_shared<const std::vector<Entry>>();
    bool busy=true;
    unsigned damaged{};
    unsigned auto_damaged{};
    unsigned manual_capacity=100,auto_capacity=15;
    std::string manual_retention_error,auto_retention_error;
    bool auto_enabled{};
    int auto_minutes=10,auto_remaining_seconds=-1;
    uint64_t changed{};
};

// The overlay submits one intent. Only tick() touches game state. Archive I/O
// runs on one worker and the UI receives immutable copies of the entry list.
class Session {
public:
    void attach();
    bool supported() const;
    bool submit(Action action, const Entry &entry={}, Bytes preview={}, uint64_t pressed_at=0, std::uintptr_t timed_world=0);
    bool allow_input(Action action);
    bool can_take_preview(Action action) const;
    std::uintptr_t take_timed_capture();
    void configure_auto(bool enabled,int minutes);
    void resize_shelf(Shelf shelf,unsigned capacity,const std::shared_ptr<const std::vector<Entry>>& reviewed);
    void tick();
    void runtime_closed();
    void runtime_reopened();
    void expire_unhandled();
    bool needs_tick() const;
    void error(const std::string &message);
    View view() const;
    Bytes thumbnail(const Entry &entry);
private:
    enum class Phase { idle, writing, reading, loading, verify, scene };
    struct Request {
        Action action; Entry entry; uint64_t id,submitted,wall_time; Bytes preview;
        std::uintptr_t timed_world{}; unsigned capacity{};
        std::shared_ptr<const std::vector<Entry>> expected_rows;
    };
    mutable std::mutex mutex_;
    View view_;
    std::optional<Request> request_;
    uint64_t next_request_{};
    std::unique_ptr<Game> game_;
    std::shared_ptr<Library> library_,auto_library_;
    AutoClock timer_;
    uint64_t next_probe_{};
    std::string account_;
    Phase phase_=Phase::idle;
    std::future<std::string> writing_;
    std::future<PrivateRecord> reading_;
    PrivateLoad loader_;
    PrivateRecord selected_;
    uint64_t active_{},sequence_{},began_{},last_notice_{};
    Shelf source_=Shelf::manual;
    bool timed_capture_{};
    PostLoadCheck delivery_check_;
    int last_native_phase_=-1;
    std::mutex tracking_mutex_;
    bool tracker_started_{},tracking_attempted_{},listed_{};
    void ensure_tracking();
    std::string world_message_;
    std::string last_auto_write_error_;
    void status(std::string message);
    void finish(std::string message);
    void bind_profile();
    void reload_entries();
    void probe_timer();
    SaveHold hold() const;
    void require_gameplay() const;
    void handle(Request request);
    void poll();
    void event(std::string name,std::string detail={},std::optional<WorldSample> world={},std::optional<double> distance={});
};
}
