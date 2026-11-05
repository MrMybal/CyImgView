---
id: export
title: Save a project or export an image
description: Save a project or export an image — CyImgView.
section: Guides
order: 90
revision: 1
---

## Keep an editable project {#project}

Use **Save as → CyImgView project (.cyimg)**. It preserves pixel layers, editable objects, masks, opacity and blending. Reopen it with Ctrl+O. Keep this working master before exporting a flattened image.

## Choose the export format {#export}

| Format | Keeps |
|---|---|
| `.cyimg` | Editable layers, objects and masks. |
| PNG | Flattened image with transparency. |
| JPEG | Flattened opaque image, adjustable quality. |
| PSD | Rasterized layers with masks applied to their pixels. |

## Avoid surprises {#details}

Apply or cancel the effect being adjusted before saving. Displayed rotation is applied to exported pixels. EXIF metadata is not copied to saved images. Opaque formats composite transparent areas onto white. Viewing support and saving support differ: saving uses available WIC encoders, and HEIF saving may need the Windows HEVC extension.

```diagram
{"type": "flow", "title": "Keep the master, share an export", "nodes": [{"id": "0", "label": "Save project .cyimg", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Save as another name", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Share PNG / JPEG", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```
