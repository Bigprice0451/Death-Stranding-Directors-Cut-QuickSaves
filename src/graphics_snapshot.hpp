#pragma once
#include <cstdint>

namespace qs {
// Single owner for the native settings-snapshot detour, shared by DLAA's
// fenced refresh and the ultrawide option's preservation of normal settings.
void ensure_graphics_snapshot(uintptr_t verified_image);
void dlaa_settings_snapshot(void *copy, uintptr_t caller);
void ultrawide_settings_snapshot(void *copy, uintptr_t caller);
namespace graphics_snapshot_detail {
// Shared by the production guard and isolated, hash-verified native fixture.
void install(uintptr_t verified_image);
}
}
