#pragma once
#include <cstdint>
#include <vector>

namespace qs {
// A persistent identity for a root scene instance and its authored resource.
// The archive contains identifiers and frame numbers only, never process addresses.
struct SceneStamp { uint64_t low{},high{}; int32_t frame{}; uint32_t kind{}; };
using SceneStamps=std::vector<SceneStamp>;
struct SceneResult { unsigned matched{},total{}; bool finished{}; };

void start_cinematic_tracking(uintptr_t verified_game_base);
void stop_cinematic_tracking();
SceneStamps current_scenes();
bool has_live_scene();
void resume_scenes(const SceneStamps &saved);
SceneResult scene_result();
}
