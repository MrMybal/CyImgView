---
id: editor-api
title: Editing plugin API
description: Add an effect, tool, mode or action using the public editing ABI.
section: Guides
order: 190
revision: 2
parent: plugins
---


## Editing plugin API {#editor-api}

The C API is in `include/cyimg_edit_plugin.h`.
An editing plugin is a DLL in the same `plugins\` folder exporting `CyImg_GetEditPlugin`.
It can also export `CyImg_GetPlugin` to decode formats. It declares arrays of four extension
types; the host handles interface, preview, selection, undo and translation.

**Effect**, in the Retouch Effects menu, using the same settings panel and preview as built-in effects:

```cpp
#include "cyimg_edit_plugin.h"

static const CyImgParam kParams[] = {
    CYIMG_SLIDER("plugin.myplugin.level", L"Level", 1, 254, 128, 1, nullptr),
    CYIMG_CHECK("plugin.myplugin.invert", L"Invert", 0),
};

// Worker thread: modify img->data within img->sel (premultiplied 8-bit BGRA)
static int Process(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* v) {
    for (uint32_t y = 0; y < img->sel.h; ++y) {
        if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
        uint8_t* p = img->data + size_t(img->sel.y + y) * img->stride + size_t(img->sel.x) * 4;
        for (uint32_t x = 0; x < img->sel.w; ++x, p += 4) {
            const bool white = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2] >= v[0] * p[3] / 255;
            p[0] = p[1] = p[2] = (white != (v[1] != 0)) ? p[3] : 0;  // premultiplied white = alpha
        }
    }
    return CYIMG_OK;
}

static const CyImgEffect kEffect = {sizeof(CyImgEffect), "threshold", "plugin.myplugin.threshold", L"Threshold",
                                    nullptr, nullptr, kParams, 2, Process};
static const CyImgEffect* const kEffects[] = {&kEffect};
static const CyImgEditPlugin kPlugin = {CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "myplugin", nullptr,
                                        L"My plugin", L"1.0", kEffects, 1, nullptr, 0, nullptr, 0, nullptr, 0};

extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) { return &kPlugin; }
```

While adjusting settings, `process` receives a screen-sized copy (`img->scale` < 1: multiply
pixel distances by `scale`), then full resolution shortly after the last adjustment and on Apply.
`img->source` supplies original pixels for effects reading neighbors.

**Tool**, after built-in tools in the tools column, with parameters in the options bar:

```cpp
static int Down(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent* ev) {
    const double r = h->param(c, 0) / 2;  // parameter 0: size
    h->draw_ellipse(c, ev->x, ev->y, r, r, 0, 0xFF000000u | h->primary_color(c));  // active layer
    return 0;  // 1: follow dragging (mouse_move then mouse_up)
}
static const CyImgParam kToolParams[] = {CYIMG_SLIDER("plugin.myplugin.size", L"Size", 4, 200, 30, 1, L"px")};
static const CyImgTool kTool = {sizeof(CyImgTool), "dot", "plugin.myplugin.dot", L"Dot", nullptr, nullptr,
                                0xE7C8 /* Segoe MDL2 icon */, nullptr, 0, 'J', kToolParams, 1,
                                Down, nullptr, nullptr, nullptr, nullptr, nullptr};
```

Host primitives (`draw_line`, `draw_polyline`, `draw_rect`, `draw_ellipse`, `fill_polygon`,
`draw_text`, `measure_text`, `begin_group` / `end_group` with opacity and multiply mode)
draw on the active layer through Direct2D, clipped to selection. From `draw_overlay` they draw
an on-screen preview without modifying anything. `lock_pixels` / `unlock_pixels` access a pixel
rectangle for blur, pixelation and similar effects. Each gesture creates an undo entry through
`begin_edit`, called automatically on first drawing. `input_text` opens a small input dialog,
`set_param` updates a displayed parameter (counter), and `screen_pixel` gives one screen pixel's size.

**Editing mode**, a tab beside Retouch and Develop: `controls` describes the right panel
(collapsible `CYIMG_SECTION` sections, sliders, checkboxes, lists, colors, `CYIMG_BUTTON` buttons,
text and separators; `visible_if` shows a control based on another value). `process` computes
the result from `img->source`. Like Develop, it is nondestructive: reduced preview during
adjustment, full render at rest, then integration into the layer as one history entry when
applying the mode. Plugin modes also support cancellation; see [Black & white](doc:black-white#cancel). Develop applies on leaving the mode. `on_button` receives and can modify values for presets.

**“Open with” action**:

```cpp
static int Run(const CyImgEditHost*, CyImgEditCtx*, const wchar_t* path, void* hwnd) {
    return INT_PTR(ShellExecuteW(HWND(hwnd), L"print", path, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
static const CyImgAction kPrint = {sizeof(CyImgAction), "print", "plugin.myplugin.print", L"Print…",
                                   nullptr, 0xE749, nullptr /* all extensions */, Run, nullptr, nullptr};
// Or an external application without code: program = L"%ProgramFiles%\\Tool\\tool.exe", args = L"\"%1\""
```

Actions appear in the plugin group of “Open with”: using the displayed file in viewer mode,
or a temporary PNG of the modified image in editing mode.

Rules:

- **Threads**: `process` runs on workers, sometimes two concurrently (preview/full render).
  No mutable global state; only host `translate` and `is_cancelled` are allowed, checking
  cancellation regularly. Other callbacks (tools, buttons, actions) run one at a time on the UI thread.
- **Pixels**: premultiplied-alpha 8-bit BGRA (color ≤ alpha).
- **Protection**: each plugin call is protected; a crashing function is disabled for the
  session with a message, without closing the application or losing the document.
- **Translation**: a key (`plugin.<name>.<text>`) and default text; choice lists use the key
  plus `.choices` (`One|Two|Three`). Bundled plugins have text in `src/lang/editplugins.<code>.lang`.
- **Versions**: each structure starts with `struct_size`; `CYIMG_EDIT_HOST_HAS(host, field)`
  checks for a host function added later.
- **Loading**: on first entering editing, opening “Open with” or the installed-plugin list, with no startup effect.
  Right-click in the viewer and choose **Installed plugins** for the live list. F1 opens the local manual.

## Translate labels {#translation}

The host resolves `plugin.<id>.<text>` keys; supply a default label as well. Choice lists use the key followed by `.choices`, with items separated by `|`. [Effect contract](doc:api-cyimgeffect-process-process) · [Pixel access](doc:api-cyimgedithost-lock-pixels-lock-pixels).
