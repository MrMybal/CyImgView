---
id: effects
title: Apply image effects
description: Preview an effect, choose its target, then apply or cancel it.
section: Guides
order: 60
revision: 2
parent: edit
---


## Choose an effect {#gallery}

Open **Effects** in Retouch. The gallery uses a common test image, with the original in the upper corner. These previews are illustrative examples; opening an effect still uses its own defaults. Arrows or the wheel change pages.

## Select the processing target {#target}

Choose **Active layer** or **Entire image** before the effect. An entire-image effect creates a result layer and keeps the original layers hidden and recoverable through Undo or Layers. A selection limits the processing area.

## Apply or cancel {#apply}

Change the sliders and use Preview to compare. **Apply** or Enter commits the effect. **Cancel** or Escape restores the prior document. Ctrl+Z can undo an applied effect. Finish the effect before saving, copying or changing tools: some commands abandon an unfinished preview.

## Add more effects {#extensions}

**Add tools / effects** opens the plugins directory. DLLs can add effects and tools; HLSL files go in `plugins/effects/`. Restart to discover new files. [HLSL guide](doc:hlsl#contract) · [Plugins](doc:plugins#overview). AI processing is not bundled.

```image
{"id": "effect-gallery", "src": "assets/screenshots/effects-en.png", "alt": "Effect gallery", "title": "Effect gallery", "controls": "minimal"}
```

```image
{"id": "effect-adjustments", "src": "assets/screenshots/effect-preview-en.png", "alt": "Live vignette settings", "title": "Live vignette settings", "controls": "minimal"}
```

```diagram
{"type": "flow", "title": "Preview before applying", "nodes": [{"id": "0", "label": "Choose target", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Adjust + preview", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Apply or cancel", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```
