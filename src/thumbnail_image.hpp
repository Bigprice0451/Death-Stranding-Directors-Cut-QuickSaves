#pragma once
#include "library.hpp"

namespace qs {
// PNG is the on-disk form. RGBA is temporary, bounded working memory for WIC and the GPU.
struct ThumbnailPixels {
    unsigned width{},height{};
    Bytes rgba;
};
constexpr unsigned preview_width=320, preview_height=180;
constexpr unsigned max_preview_png_bytes=1024*1024;
Bytes native_thumbnail(const Bytes &checkpoint);
Bytes encode_thumbnail(const ThumbnailPixels &image);
ThumbnailPixels decode_thumbnail(const Bytes &png);
}
