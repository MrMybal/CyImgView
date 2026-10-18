<p align="center">
  <img src="docs/logo.png" alt="CyImgView logo" width="160">
</p>

<h1 align="center">CyImgView</h1>

**English** · [Français](README.fr.md)

A Windows image viewer built for speed: native C++, Direct2D GPU rendering, WIC and no
external dependencies for the main executable. The executable is about 600 KB and needs no runtime.

## Installation

Download `CyImgView-<version>-Setup.exe` from
[releases](https://github.com/MrMybal/CyImgView/releases/latest) and run it.

- **No administrator rights**: installs in `%LOCALAPPDATA%\Programs\CyImgView`.
- **Windows integration**: Start menu shortcut, optional desktop shortcut, “Open with” entries
  for images, RAW photos and Unreal textures, and CyImgView listed in *Settings > Default apps*.
- **Updates**: automatic through GitHub.
- **Uninstall**: *Settings > Apps*, or `uninstall.exe` in the installation folder.
- **Silent installation**: `CyImgView-<version>-Setup.exe /S`.

Portable version: `CyImgView-<version>-win64.zip` contains the executable and `plugins` folder.

<a id="fonctionnalités"></a>

## Features

- **Formats**: all support is bundled; no additional Windows codecs are needed for viewing.

  | Family | Formats | Plugin |
  |---|---|---|
  | Common (WIC) | JPEG, PNG, animated GIF, BMP, TIFF, ICO, JPEG XR | — |
  | Web / modern | Animated WebP, HEIC / HEIF, AVIF (including animated sequences), animated JPEG XL, JPEG 2000 (jp2, j2k, jpx…) | `webp`, `heif`, `jxl`, `jpeg2000` |
  | Vector | SVG / SVGZ, PDF and AI (first page), WMF / EMF, DXF (2D) | `vector` |
  | Working documents | PSD / PSB (merged image), XCF (GIMP, recomposited layers), KRA / ORA, CLIP, and embedded previews from INDD, EPS, CDR, Affinity (afphoto, afdesign, afpub) | `psd`, `docs` |
  | Specialized | EXR, HDR (Radiance), DDS (BC1–BC7, HDR, cubemaps), TGA, FITS, multiframe DICOM, ICNS | `exr`, `classic`, `dds`, `tga`, `dicom` |
  | Legacy | PCX, PICT, IFF / ILBM (HAM, EHB), Netpbm (pbm, pgm, ppm, pam, pfm), XBM / XPM | `classic` |

  HDR images (EXR, HDR, PFM, floating-point DDS) use simple tone mapping for display.
  Working documents show the merged image or preview saved by their originating application,
  without layer recomposition except for XCF.
- **Animation**: GIF, WebP, JPEG XL, HEIF/AVIF sequences and multiframe DICOM loop.
  Space: play/pause; `,` and `.`: step through frames.
- **Channels**, like Photoshop: the RGB button opens a panel with red, green, blue and alpha
  previews. Click a row to show that channel in grayscale; checkboxes combine channels in color.
  Uncheck alpha to show colors hidden beneath transparent areas. Shortcuts: C, Ctrl+2 through Ctrl+6.
- **Languages**: Français, English, Deutsch, Español, Italiano, Português, Nederlands, Polski,
  Русский, 日本語, 简体中文, 한국어. Defaults to the Windows language, with instant switching
  in Settings. Add or correct a language by placing a `.lang` file in a `lang` folder beside
  the executable; see [`src/i18n.h`](src/i18n.h) for the format and `src/lang/` for examples.
- **Slideshow** (D): fullscreen with hidden interface, crossfades, adjustable duration
  (1–60 seconds, ↑/↓ during playback) and optional random order.
- **Clipboard**: Ctrl+C copies the displayed image with transparency; Ctrl+Shift+C copies
  its path. Right-click opens common actions.
- **RAW**: all formats supported by [LibRaw](https://www.libraw.org) (about 1,000 cameras:
  CR3, CR2, CRW, NEF, ARW, RAF, RW2, ORF, DNG, PEF, SRW, IIQ, 3FR…) through `raw`.
  Display is progressive: the camera's embedded JPEG appears in 5–50 ms, at full resolution
  on most Canon CR2/CR3 and Nikon cameras. If smaller than the sensor (ARW, RAF, RW2…),
  LibRaw develops the RAW in the background (0.3–2 seconds) and replaces the preview,
  with tonality matched to the camera JPEG to avoid a brightness jump.
- **Unreal Engine textures** (uncooked `.uasset`, UE 4.x–5.8) through `uasset`:
  - In `Content` folders, only texture `.uasset` files (Texture2D, TextureCube…) are listed;
    materials, meshes and Blueprints are ignored.
  - Full-resolution source pixels are read in all known cases (PNG, JPEG, raw, zlib,
    UEDELTA, Oodle data) and formats (BGRA8, G8, G16, RGBA16, RGBA16F, RGBA32F, R16F,
    R32F, BGRE8). Cubemaps show all six faces side by side.
  - Oodle is loaded automatically from an Unreal installation through the Epic registry;
    otherwise copy `oo2core_9_win64.dll` into `plugins\`.
  - Thumbnails use the editor-saved preview. Information includes class, source format,
    compression setting, sRGB and engine version.
  - Cooked textures (packaged games, `Saved/Cooked`), UE 4.25–5.8:
    `.uasset` + `.uexp` + `.ubulk`/`.uptnl`. Engine version is inferred from file structure.
    BC1–BC7 GPU formats, including BC6H HDR, are decoded by Direct3D WARP, alongside
    uncompressed formats. BC5 normal maps reconstruct Z. Thumbnails directly read the
    smallest sufficient mip.
  - Limitations: mobile ASTC/ETC, virtual textures (VT), externally virtualized data and
    IoStore packages (`.utoc`/`.ucas`, which must first be extracted, for example with FModel).
- **Smooth mouse-centered zoom**, with stops at fit and 100%. Pixel-exact at 100%, high-quality
  bicubic downscaling and sharp pixels above 300%.
- **Folder navigation** with Explorer-style natural sorting and a thumbnail filmstrip below.
- **Speed**: multithreaded decoding, neighboring-image preloading in the browsing direction,
  early GPU upload, fast EXIF/JPEG-DCT thumbnails as immediate previews while decoding,
  a cache capped at 20% of RAM and no CPU use at idle.
- EXIF orientation, manual rotation, fullscreen, EXIF information (camera, lens, exposure,
  aperture, ISO, focal length, date) and a checkerboard under transparency.
- **Save as / convert** (Ctrl+S): JPEG (quality adjustable in the dialog), PNG, TIFF, BMP,
  GIF, JPEG XR and HEIF if the HEVC extension is installed. Displayed rotation is applied
  to pixels. Transparency is preserved where supported, otherwise composited on white.
  Writes use a temporary file to avoid corrupting the file being replaced.
  RAW images save their developed version.
- **Paint-style editing** (E): brush, eraser (transparent on formats with transparency),
  line, arrow, rectangle, ellipse (outline or filled), text, flood fill, eyedropper,
  crop with thirds grid, rotation, mirroring, resizing by percentage or long edge,
  color palette/custom color and undo/redo. Drawing uses GPU antialiasing.
- **Plugins**: DLLs in `plugins\` add formats, effects, tools, editing modes and “Open with”
  actions. Bundled: extra effects (noise, threshold, duotone, chromatic aberration), screenshot
  annotation (numbered marker, highlighter, masking blur, text bubble), Black & White mode,
  Print and Open in browser actions.

<a id="compiler"></a>

## Build

Requirement: Visual Studio 2022 with the “Desktop development with C++” workload.
CMake and Ninja are supplied by Visual Studio.

```bat
build.cmd            :: Release -> build\Release\bin\CyImgView.exe
build.cmd Debug
```

Contents of `build\Release\bin`:

```
CyImgView.exe
cyimg_bench.exe       decoding benchmark (console)
plugins\raw.dll        RAW (bundled LibRaw)
plugins\vcomp140.dll   Microsoft OpenMP runtime (multithreaded LibRaw)
plugins\uasset.dll     Unreal Engine textures
plugins\tga.dll        TGA
plugins\webp.dll       WebP (libwebp)
plugins\heif.dll       HEIC / HEIF / AVIF (libheif, libde265, dav1d)
plugins\jxl.dll        JPEG XL (libjxl, highway)
plugins\jpeg2000.dll   JPEG 2000 (OpenJPEG)
plugins\exr.dll        OpenEXR (OpenEXRCore, libdeflate, zstd, OpenJPH)
plugins\vector.dll     SVG, PDF, AI, WMF, EMF, DXF (Direct2D, Windows.Data.Pdf, GDI+)
plugins\psd.dll        Photoshop PSD / PSB
plugins\docs.dll       XCF, KRA, ORA, CLIP, INDD, EPS, CDR, Affinity
plugins\dds.dll        DirectDraw Surface
plugins\dicom.dll      DICOM
plugins\classic.dll    Netpbm, PCX, XBM, XPM, IFF, HDR, FITS, ICNS, PICT
plugins\fx_extra.dll   editing: noise, threshold, duotone, aberration
plugins\annotate.dll   editing: marker, highlighter, masking blur, bubble
plugins\bw_mode.dll    editing: Black & White mode
plugins\share_actions.dll  editing: Open with → Print…, Open in browser
```

`-DCYIMG_TEST_PLUGINS=ON` additionally builds `bin\testplugins\crashtest.dll`, a deliberately
faulty editing plugin for host robustness testing. It is never shipped:
`tools\package.ps1` takes only `bin\plugins`. `python tools\check_lang.py` checks language
files (keys, printf specifiers and choice lists).

Each plugin has `plugins\<name>\plugin.cmake`, included automatically. To build only selected
plugins, use `-DCYIMG_ONLY_PLUGINS="webp;psd"`. Deploy `CyImgView.exe` with its `plugins` folder.

LibRaw 0.22.2 sources are in `third_party/LibRaw`, dual-licensed LGPL 2.1 / CDDL 1.0.
They are used under LGPL 2.1, compatible with the project's GPL-3.0.
`tools\package.ps1` produces the installer (`setup-stub.exe` + archive).

<a id="mises-à-jour-github-releases"></a>

## Updates (GitHub Releases)

CyImgView checks for new versions a few seconds after startup, at most once every 20 hours,
or manually with **Ctrl+U**. An “Update X.Y.Z” badge appears in the top bar. Choose to install
and restart, view the release or skip that version. Installation downloads the release's
`*-win64.zip`, verifies SHA-256 when supplied by GitHub, replaces files including the running
executable, then restarts on the same image. On failure, everything is restored.
Disable with `auto=0` in `[update]` in `%APPDATA%\CyImgView\settings.ini`.

Configure the repository in `src/version.h`: `#define CYIMG_UPDATE_REPO L"owner/repository"`.
The feature is disabled while that field is empty.

To publish a version:

1. Increment `CYIMG_VERSION_MAJOR/MINOR/PATCH` in `src/version.h` and commit.
2. Run `git tag v1.3.0 && git push origin v1.3.0`. The `.github/workflows/release.yml`
   workflow builds, packages and creates the release with the ZIP.

Without GitHub Actions, `tools\package.ps1` produces `dist\CyImgView-<version>-win64.zip`
for attachment to a release tagged `v<version>`.

Oodle (`oo2core_*.dll`) is never included: it belongs to Epic and is loaded from the user's Unreal installation.

<a id="utilisation"></a>

## Usage

`CyImgView.exe [file|folder]`. You can also drag and drop, use Ctrl+O, or use “Open with” / file associations.

| Key | Action |
|---|---|
| ← → / Space / Backspace / Page Up / Page Down | Previous / next image |
| Home / End | First / last image |
| Wheel, + / -, ↑ / ↓ | Zoom (mouse-centered with wheel) |
| Drag | Pan the zoomed image |
| Double-click / Z | Toggle fit / actual size |
| 0 / 1 | Fit / 100% |
| R / Shift+R | Rotate |
| F / F11 / Enter / middle-click | Fullscreen |
| T | Filmstrip |
| I | Information |
| H / F1 | Help |
| F5 | Reload folder |
| Mouse side buttons | Previous / next image |
| Escape | Close panel, leave fullscreen, then exit |
| Ctrl+C / Ctrl+Shift+C | Copy image / file path |
| Right-click | Menu (copy, save, channels, slideshow…) |
| C / Ctrl+2 through Ctrl+6 | Channels: RGB, red, green, blue, alpha |
| Space / , / . | Animation: play-pause / previous / next frame |
| N | Raw pixels (no smoothing) / selected filtering |
| D | Slideshow (↑ ↓: image duration, Space: pause, Escape: exit) |
| Ctrl+S | Save as / convert |
| Ctrl+U | Check for updates |
| P | Settings |
| E | Editing mode |

<a id="mode-édition"></a>

### Editing mode

| Key | Tool / action |
|---|---|
| B / G | Brush / eraser (right-click: secondary color) |
| L / A / R / O | Line / arrow / rectangle / ellipse (Shift: 45°, square, circle) |
| T | Text (Enter: confirm, Shift+Enter: newline, Escape: cancel) |
| F / K | Fill / eyedropper |
| C | Crop (Enter or double-click to confirm) |
| M, held Space, middle-click | Pan |
| [ / ] | Stroke or font size |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| E / Escape | Leave editing (offers to save changes) |

Tool sizes use image pixels and adapt to its resolution.
EXIF metadata is not copied to the saved file.

<a id="paramètres-touche-p-ou-bouton-engrenage"></a>

### Settings (P or gear button)

- **Open files in the existing window** (off by default): an image opened from Explorer replaces
  the existing window's image instead of creating a new window, including when opening several files.
- **Show filmstrip**.
- **Check for updates automatically**, plus a button to check immediately.
- **Interface language**: Automatic (Windows language) or one of the 12 languages.
- **Slideshow**: random order and image duration.
- **Image filtering**: Automatic (pixel-exact at 100%, smoothed when downscaling, sharp above 300%),
  Raw pixels (no smoothing, for pixel art), Linear, Cubic, HQ Cubic, Anisotropic.
  Also in the context menu; N toggles raw pixels.

Settings and window position are saved in `%APPDATA%\CyImgView\settings.ini`.

<a id="écrire-un-plugin"></a>

## Write a plugin

The C API is in [`include/cyimg_plugin.h`](include/cyimg_plugin.h).
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
  [`include/cyimg_plugin_i18n.h`](include/cyimg_plugin_i18n.h)) searches CyImgView language files,
  falling back to default text when the key is unknown.
- **Robustness**: a plugin crash on a file does not close the viewer; the file shows an error.

To build a new plugin, create `plugins/<name>/plugin.cmake` following `plugins/tga`
(`cyimg_add_plugin(<name> sources...)`); it is included automatically.

<a id="écrire-un-plugin-dédition"></a>

## Write an editing plugin

The C API is in [`include/cyimg_edit_plugin.h`](include/cyimg_edit_plugin.h).
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
returning to Retouch, finishing or saving. `on_button` receives and can modify values for presets.

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
- **Loading**: on first entering editing, opening “Open with” or Help, with no startup effect.
  The list is in Help (H), under Editing plugins.

Bundled plugins are commented examples: [`plugins/fx_extra`](plugins/fx_extra) (effects),
[`plugins/annotate`](plugins/annotate) (tools), [`plugins/bw_mode`](plugins/bw_mode) (mode),
[`plugins/share_actions`](plugins/share_actions) (actions). They build like other plugins,
with `plugin.cmake` (`cyimg_add_plugin(<name> sources...)`).

<a id="mesurer-les-performances"></a>

## Measure performance

```bat
build\Release\bin\cyimg_bench.exe D:\Photos\MyFolder
build\Release\bin\cyimg_bench.exe D:\Photos\MyFolder 160 best
```

This displays full-resolution/thumbnail decode times and EXIF. With `best`, it also measures
full RAW development even when the preview is sufficient.

Command-line conversion, with output format inferred from the extension:

```bat
build\Release\bin\cyimg_bench.exe convert photo.CR3 photo.jpg 90
```

With `CYIMG_TRACE=1` in the environment, viewer startup is timed in `%TEMP%\cyimg_trace.txt`.

<a id="confidentialité"></a>

## Privacy

- **No telemetry**, account or advertising.
- **Only network connection**: update checking against the public GitHub API
  (`api.github.com/repos/MrMybal/CyImgView/releases/latest`), at most every 20 hours, followed
  by the release archive download if you accept the update. No personal data is sent
  (`User-Agent: CyImgView/<version>`). Disable it in Settings.
- **Local files**: settings in `%APPDATA%\CyImgView\settings.ini` (and `develop_last.ini`
  for Develop settings), optional crash log in `%LOCALAPPDATA%\CyImgView\crash.txt`
  (never sent), and temporary “Open with” exports in `%TEMP%\CyImgView` (deleted after 24 hours).
- **DICOM**: patient-identifying data (name, ID, date of birth) is never displayed.
- “Open with” launches only applications you selected or that Windows associates with the format.

<a id="licence"></a>

## License

CyImgView is distributed under **GNU GPL v3.0**: see [LICENSE](LICENSE).

Third-party components:

- **LibRaw** (`third_party/LibRaw`): LGPL 2.1 / CDDL 1.0.
- **libwebp**: BSD 3-Clause (+ Google patent grant).
- **libheif**, **libde265**: LGPL 3.0. **dav1d**: BSD 2-Clause.
- **libjxl**: BSD 3-Clause (+ patents). **highway**: Apache 2.0.
- **OpenJPEG**: BSD 2-Clause.
- **OpenEXR** (OpenEXRCore) and **Imath**: BSD 3-Clause. **libdeflate**: MIT.
  **zstd**: BSD. **OpenJPH**: BSD 2-Clause.

Their sources and licenses are in `third_party/`. Other formats (PSD, XCF, DDS, DICOM,
FITS, PCX…) use code written for this project.

- **Oodle** (RAD Game Tools / Epic Games) is **not** distributed. It is loaded at runtime
  from the user's Unreal Engine installation.
- **Unreal Engine**: `.uasset` is read by code written for this project; no Epic code is included.
