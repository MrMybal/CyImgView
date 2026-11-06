---
id: building
title: Build and distribute
description: Build and distribute — CyImgView.
section: Developers
order: 210
revision: 1
---

## Build and distribute {#build}

Requirements: Node.js 22+ for the local manual, and Visual Studio 2022 with the “Desktop development with C++” workload.
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
plugins\hlsl.dll       HLSL effects
plugins\effects\       bundled shader example
Documentation\Documentation.html        offline CyDoc manual (keep the complete Documentation directory)
```

`-DCYIMG_TEST_PLUGINS=ON` additionally builds `bin\testplugins\crashtest.dll`, a deliberately
faulty editing plugin for host robustness testing. It is never shipped:
`tools\package.ps1` takes only `bin\plugins`. `python tools\check_lang.py` checks language
files (keys, printf specifiers and choice lists).

Each plugin has `plugins\<name>\plugin.cmake`, included automatically. To build only selected
plugins, use `-DCYIMG_ONLY_PLUGINS="webp;psd"`. Deploy `CyImgView.exe` with its complete `plugins` and `Documentation` folders.

LibRaw 0.22.2 sources are in `third_party/LibRaw`, dual-licensed LGPL 2.1 / CDDL 1.0.
They are used under LGPL 2.1, compatible with the project's GPL-3.0.
`tools\package.ps1` produces the installer (`setup-stub.exe` + archive).



## Build the local manual {#manual}

Node.js 22 or later is required to generate help, not to read it. The CyDoc package is pinned locally. Run:

```powershell
npm --prefix .CyDocBuild ci
node .CyDocBuild/build.mjs --check
npm --prefix .CyDocBuild test
```

Normal CMake builds generate and copy `Documentation/` beside the executable; packaging copies the complete directory.

## Update UI translations {#translations}

Interface keys live in `src/lang`. Run `python tools/check_lang.py`. Detailed manual translations live in `.CyDocBuild/manual/en` and `.CyDocBuild/manual/fr`; use stable IDs and revision/sourceRevision metadata.
