---
id: licenses
title: Licenses and credits
description: Read the application license and credits for bundled components.
section: About
order: 240
revision: 2
---


## Licenses and credits {#licenses}

CyImgView is distributed under **GNU GPL v3.0**: see `CyImgView-LICENSE.txt` (full text included in `Documentation/DocData`).

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

## CyDoc {#cydoc}

The local documentation reader uses CyDoc 0.1.0 under the MIT license. Its license is included as `CyDoc-LICENSE.txt` in `Documentation/DocData`. Only image and gallery readers are included; Three.js is not distributed.
