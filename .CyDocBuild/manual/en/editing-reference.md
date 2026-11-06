---
id: editing-reference
title: Document and layer reference
section: Guides
order: 112
revision: 2
description: Review how documents, objects, layers, masks and effects interact.
parent: edit
---


## Reference {#reference}

- **Ctrl+N** opens the new-document window with 18 screen, photo and print presets,
  custom pixel dimensions, orientation swapping and transparent, white or black
  backgrounds. The document opens directly in Retouch.
- Lines, arrows, rectangles, ellipses and text are retained **objects** on separate
  layers. Use **V** to select and move an object. Right-click its layer to edit its
  text and styling, or choose **Rasterize object** before painting directly on it.
- **Hold the right mouse button on the canvas to pan the view**, regardless of the
  active editing tool. This does not move layers or paint pixels. Right-clicking
  the Layers panel still opens its context menu. **X** swaps the painting colors.
- The **Layers** context menu provides creation, duplication, renaming, merging,
  blend modes and layer masks. Masks start from the selection or reveal everything.
  When painting a mask, black hides, white reveals, and gray partially reveals.
  Masks can be disabled, inverted or removed. These operations support undo.
- Plugin modes, including Black & White, provide **Apply** and **Cancel mode**.
  Escape cancels the mode; after applying, Ctrl+Z in Retouch restores the previous
  result. Original remains a temporary comparison.
- The **effect gallery** shows real previews on a common test image, including
  native effects and HLSL plugins, with the original image as a reference. Choose
  **Active layer** or **Entire image** before opening an effect. Whole-image effects
  create a result layer and keep the original layers hidden and recoverable.
- **Save as > CyImgView project (.cyimg)** preserves objects, masks and layers.
  Reopen with Ctrl+O. PNG/JPEG export a flattened image; PSD keeps rasterized layers
  with their masks applied.
- **Add tools / effects** opens the plugins directory. Native extensions use
  `include/cyimg_edit_plugin.h`. Place HLSL effects in `plugins/effects/`; see the
  [shader contract and examples](doc:hlsl). Extension points for
  future AI tools are available; no AI engine is included.
- Initial image fitting reserves space for the visible toolbar and filmstrip.
