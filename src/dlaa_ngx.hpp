#pragma once
#include <cstdint>

namespace qs::dlaa_detail {
using U32 = uint32_t;
constexpr int quality = 2, dlaa = 5;
constexpr U32 preset_k = 11, super_sampling = 1;
constexpr const char *quality_key = "PerfQualityValue", *preset_key = "DLSS.Hint.Render.Preset.DLAA";
constexpr bool success(U32 result) { return (result & 0xfff00000u) != 0xbad00000u; }
struct Size { U32 w{}, h{}; };
inline bool same(Size a, Size b) { return a.w && a.h && a.w == b.w && a.h == b.h; }
struct Params {
    void (*set_i)(void *, const char *, int);
    void (*set_u)(void *, const char *, U32);
    U32 (*get_i)(void *, const char *, int *);
    U32 (*get_u)(void *, const char *, U32 *);
};
using Optimal = bool (*)(void *, U32, U32, int, bool, U32 *, U32 *, float *);
using Create = U32 (*)(void *, U32, void *, void **);

// Decima's Quality slot must remain 2. Ask NGX whether DLAA actually works at
// this output size before Decima allocates textures. Re-run the normal query on
// failure, including the normal sharpness output if the caller requested it.
inline bool optimal(Optimal original, void *manager, Size output, int mode, bool flag,
                    U32 *w, U32 *h, float *sharpness, bool requested, bool &native) {
    native = false;
    if (requested && mode == quality && output.w && output.h && w && h) {
        Size answer;
        if (original(manager, output.w, output.h, dlaa, flag, &answer.w, &answer.h, sharpness) && same(answer, output)) {
            *w = answer.w;
            *h = answer.h;
            native = true;
            return true;
        }
    }
    return original(manager, output.w, output.h, mode, flag, w, h, sharpness);
}

inline bool get_size(const Params &api, void *params, const char *width, const char *height, Size &s) {
    return success(api.get_u(params, width, &s.w)) && success(api.get_u(params, height, &s.h));
}
struct Created { Size input{}, output{}; bool preset_requested{}; };

// NVIDIA's parameter block belongs to the game and is reused. Set the DLAA-
// specific hint only for this create call, then restore the previous mode/hint.
// Unset preset has NVIDIA's documented default value 0.
inline U32 create(const Params &api, Create original, void *cmd, U32 feature, void *params,
                  void **handle, bool requested, Size sized_output, Created &result) {
    int previous_mode{};
    if (feature != super_sampling || !requested || !params || !handle ||
        !success(api.get_i(params, quality_key, &previous_mode)) || previous_mode != quality ||
        !get_size(api, params, "Width", "Height", result.input) ||
        !get_size(api, params, "OutWidth", "OutHeight", result.output) ||
        !same(result.input, sized_output) || !same(result.input, result.output))
        return original(cmd, feature, params, handle);

    U32 previous_preset = 0;
    if (!success(api.get_u(params, preset_key, &previous_preset))) previous_preset = 0;
    api.set_i(params, quality_key, dlaa);
    api.set_u(params, preset_key, preset_k);
    int actual_mode = -1;
    U32 actual_preset = 0;
    result.preset_requested = success(api.get_i(params, quality_key, &actual_mode)) && actual_mode == dlaa &&
        success(api.get_u(params, preset_key, &actual_preset)) && actual_preset == preset_k;
    if (!result.preset_requested) {
        api.set_i(params, quality_key, previous_mode);
        api.set_u(params, preset_key, previous_preset);
        return original(cmd, feature, params, handle);
    }
    const U32 status = original(cmd, feature, params, handle);
    api.set_i(params, quality_key, previous_mode);
    api.set_u(params, preset_key, previous_preset);
    return status;
}

// The active input rectangle can occupy part of a padded D3D12 texture. The
// game disables NGX output subrects, so the actual output texture must match.
inline bool native_frame(Size input_rect, Size color_texture, Size output_texture, Size created_output) {
    return same(input_rect, created_output) && same(output_texture, created_output) &&
           color_texture.w >= input_rect.w && color_texture.h >= input_rect.h;
}
}
