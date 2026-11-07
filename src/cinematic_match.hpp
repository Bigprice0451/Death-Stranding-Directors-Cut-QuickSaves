#pragma once
#include "cinematic.hpp"
#include <algorithm>
#include <optional>

namespace qs::cinematic_detail {
constexpr uint32_t engine_sequence=1, bink_movie=2;
constexpr unsigned scene_limit=128;
constexpr uint64_t wait_ms=6000;
inline bool same_scene(const SceneStamp &a,const SceneStamp &b) {
    return a.kind==b.kind && a.low==b.low && a.high==b.high;
}
// plan() is called by one actual engine tick. A matching UUID alone never
// completes a requested seek: wait for a later simulation update to observe it.
class Match {
    enum class Phase { waiting, requested, verifying, matched, unavailable };
    struct Target { SceneStamp saved; Phase phase; };
    std::vector<Target> targets_;
    uint64_t began_;
public:
    explicit Match(const SceneStamps &saved={},uint64_t began=0):began_(began) {
        targets_.reserve(std::min<size_t>(saved.size(),scene_limit));
        for(const auto &entry:saved) {
            if(targets_.size()==scene_limit) break;
            const bool unique=std::count_if(saved.begin(),saved.end(),[&](const auto &s){return same_scene(s,entry);})==1;
            const bool valid=unique && (entry.kind==engine_sequence || entry.kind==bink_movie) && (entry.low||entry.high) && entry.frame>=(entry.kind==bink_movie?1:0);
            targets_.push_back({entry,valid?Phase::waiting:Phase::unavailable});
        }
    }
    std::optional<int32_t> plan(const SceneStamp &now,int32_t last_frame,uint64_t tick) {
        if(tick-began_>=wait_ms || now.frame<0 || now.frame>last_frame) return {};
        for(auto &t:targets_) {
            if(!same_scene(t.saved,now) || t.saved.frame>last_frame) continue;
            if(t.phase==Phase::verifying) {
                if(now.frame==t.saved.frame || int64_t(now.frame)==int64_t(t.saved.frame)+1)
                    t.phase=Phase::matched;
            } else if(t.phase==Phase::waiting) {
                if(t.saved.frame==now.frame) t.phase=Phase::matched;
                else if(t.saved.kind!=bink_movie || t.saved.frame>1) {t.phase=Phase::requested; return t.saved.frame;}
            }
            return {};
        }
        return {};
    }
    void issued(const SceneStamp &key,bool both_cursors_at_target) {
        for(auto &t:targets_) if(same_scene(t.saved,key) && t.phase==Phase::requested) {
            t.phase=both_cursors_at_target?Phase::verifying:Phase::unavailable;return;
        }
    }
    SceneResult result(uint64_t tick) const {
        SceneResult r{};r.total=static_cast<unsigned>(targets_.size());
        for(auto &t:targets_) if(t.phase==Phase::matched) ++r.matched;
        // Keep unmatched targets available for the first few frames while the world streams.
        r.finished=r.matched==r.total || tick-began_>=wait_ms;
        return r;
    }
};
}
