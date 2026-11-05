---
id: decoder-api
title: Decoder plugin API
description: Decoder plugin API — CyImgView.
section: Developers
order: 180
revision: 1
---

## Decoder plugin API {#decoder-api}

The C API is in [`include/cyimg_plugin.h`](doc:source-api-include-cyimg-plugin-h).
A plugin is a DLL exporting one function:

```cpp
#include "cyimg_plugin.h"

static int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    // Option 1: pixels decoded by the plugin
    uint32_t stride;
    uint8_t* px = host->alloc_bgra(ctx, width, height, /*has_alpha*/ 0, &stride);
    // ... fill px (8-bit BGRA, premultiplied alpha) ...

    // Option 2: pass an embedded JPEG/PNG/TIFF stream to the host
    // return host->decode_encoded(ctx, data, size);

    host->set_format(ctx, L"My format");
    return CYIMG_OK;
}

static const CyImgPlugin plugin = {
    CYIMG_API_VERSION, L"My plugin", L"1.0", L"ext1;ext2",
    nullptr,  // optional probe: header recognition
    Decode,
};

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &plugin; }
```

- `decode` runs concurrently on several threads; use no mutable global state.
- A nonzero `host->target_size(ctx)` requests a thumbnail; the plugin may return a smaller image.
- Plugins take precedence over WIC for declared extensions.
- **Progressive display**: a slow plugin may return a fast preview and call `host->set_refinable(ctx)`.
  The host shows it, then calls `decode` in the background with `host->quality(ctx) == 1` for
  the full version. These functions were added after the initial API: check with `CYIMG_HOST_HAS(host, quality)`.
- `host->set_error(ctx, message)` supplies the rejection message, such as “this .uasset is a Material”.
- **Folder filter**, optional export:
  `extern "C" __declspec(dllexport) int CyImg_Accept(const wchar_t* path)` returns 0 to hide a file.
  The `uasset` plugin uses it to retain only textures. It must be fast: it runs for every folder file.
- If a plugin returns an image larger than the requested thumbnail, the host downsizes it.
- **Animation**: after `alloc_bgra` for frame 0, `host->add_frame()` adds each already-composited
  subsequent frame, and `host->set_frame_delay()` sets duration.
- **Translation**: `host->translate()` (helper:
  [`include/cyimg_plugin_i18n.h`](doc:source-api-include-cyimg-plugin-i18n-h)) searches CyImgView language files,
  falling back to default text when the key is unknown.
- **Robustness**: a plugin crash on a file does not close the viewer; the file shows an error.

To build a new plugin, create `plugins/<name>/plugin.cmake` following `plugins/tga`
(`cyimg_add_plugin(<name> sources...)`); it is included automatically.


