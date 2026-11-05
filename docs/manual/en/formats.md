---
id: formats
title: Formats and capabilities
description: Formats and capabilities — CyImgView.
section: Reference
order: 100
revision: 1
---

## Formats and capabilities {#formats}

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
  the executable; see [`src/i18n.h`](doc:building#translations) for the format and `src/lang/` for examples.
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


