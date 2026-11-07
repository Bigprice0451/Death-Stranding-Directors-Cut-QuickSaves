#pragma once
#include "thumbnail_image.hpp"
#include <functional>
#include <memory>

namespace reshade::api { struct effect_runtime; }
namespace qs {
// Register after ReShade accepts the add-on, to identify the active HDR transfer function.
void register_thumbnail_output();
// Call on the ReShade render thread. Unsupported back buffers or image capture
// failures yield an empty preview; they must never cancel the underlying save.
Bytes capture_thumbnail(reshade::api::effect_runtime *runtime) noexcept;

class ThumbnailCache {
public:
    ThumbnailCache();
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache &)=delete;
    ThumbnailCache& operator=(const ThumbnailCache &)=delete;
    // The loader supplies PNG bytes only on first display of this slot version.
    void draw(reshade::api::effect_runtime *runtime,const Entry &entry,float width,
              const std::function<Bytes()> &load_png);
    // Call before the owning runtime is destroyed; a replacement builds fresh GPU resources.
    void end_runtime(reshade::api::effect_runtime *runtime);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
