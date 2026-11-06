---
id: layers
title: Layers, blending and masks
description: Combine layers with opacity, blend modes and editable masks.
section: Guides
order: 50
revision: 2
parent: edit
---


## Understand the layer stack {#layers}

Layers are transparent sheets over the background. The upper visible layer is drawn over the lower ones. Select a layer before painting, changing its opacity or applying an effect. The eye controls visibility; opacity and blend mode control how the layer combines with the rest.

## Use the layer menu {#menu}

Right-click **Layers** to create, duplicate, rename, reorder, delete or merge. **Ctrl+J** duplicates, **Ctrl+E** merges down and **Ctrl+Shift+E** flattens. These document operations support undo.

## Hide pixels with a mask {#masks}

1. Right-click the layer and choose **Add mask**. It reveals the selection, or the entire layer when nothing is selected.
2. Enable **Paint mask**. Black hides, white reveals and gray partially reveals. **X** swaps the painting colors.
3. Disable Paint mask to return to image pixels. The menu can disable, invert or remove the mask.

A mask changes visibility without deleting hidden pixels. Save as `.cyimg` to keep it editable. PNG/JPEG export the visible result.

## Choose a blend mode {#blending}

Normal draws the layer according to its opacity. Multiply darkens; Screen lightens. Overlay, Darken, Lighten, Difference and Add are also available. Try a mode, compare, then undo if needed. [Save projects and exports](doc:export#project).

```diagram
{"type": "flow", "title": "Composing separate layers", "nodes": [{"id": "0", "label": "Background photo", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Frame + text layers", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Visible result", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```

```diagram
{"type": "flow", "title": "Mask values control visibility", "nodes": [{"id": "0", "label": "Black → hidden", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Gray → partial", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "White → visible", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```
