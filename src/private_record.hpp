#pragma once
#include "cinematic.hpp"
#include "telemetry.hpp"

namespace qs {
// The archive keeps the screen, world image and matching scene clocks together.
// The game itself only receives image, never the mod's outer record or PNG.
struct PrivateRecord {
    WorldSample world;
    SceneStamps scenes;
    Bytes image;
};
Bytes pack_record(const PrivateRecord &record, const Bytes &png);
PrivateRecord unpack_record(const Bytes &archive);
}
