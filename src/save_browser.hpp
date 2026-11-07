#pragma once
#include "library.hpp"
#include <functional>
#include <memory>

namespace reshade::api { struct effect_runtime; }
namespace qs {
class Session;
class ThumbnailCache;
using SetSaveCapacity = std::function<void(Shelf,unsigned,const std::shared_ptr<const std::vector<Entry>> &)>;

// Draw on the overlay thread. The session supplies an immutable snapshot and
// owns every load/write. Changing a widget alone never removes an archive.
void draw_save_browser(reshade::api::effect_runtime *runtime, Session &session, ThumbnailCache &thumbnails,
                       bool can_save, const std::function<void()> &capture_manual,
                       const std::function<void(bool,int)> &configure_auto,
                       const SetSaveCapacity &configure_capacity);
}
