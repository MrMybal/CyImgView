---
id: edit
title: Paint, annotate and move objects
description: Paint on layers and keep text, frames and arrows editable.
section: Guides / Editing
order: 40
revision: 2
---


## Choose the right tool {#tools}

Press **E** for Retouch. Tools are on the left, options at the top and colors at the bottom. **B** paints, **G** erases, **S** selects, **D** draws a gradient and **C** crops. Sizes are measured in image pixels.

## Add a title or a frame {#objects}

1. Choose Text (**T**), Arrow (**A**), Rectangle (**R**) or Ellipse (**O**) and draw on the image. Enter confirms text; Shift+Enter adds a line.
2. Press **V**, select the object or its layer, then drag with the **left** button to move it.
3. Right-click its layer and choose **Edit object** to change the text and adopt the tool’s color, size and fill options.
4. Choose **Rasterize object** only when you need to paint directly on it. Undo can restore the object while history is available.

## Paint on a separate layer {#paint}

Create a layer with **Ctrl+Shift+N**, select it and paint. **X** swaps foreground/background colors. Retained text and shapes cannot be painted until rasterized. A transparent painting layer keeps the underlying photograph intact. **Ctrl+Z** undoes the previous operation.

## Move the view safely {#navigation}

Hold the **right button**, the middle button or Space with a drag to pan. With **V**, left dragging moves document content. Escape first cancels an active object move, shape, crop or selection before it eventually leaves editing. [Layer and mask guide](doc:layers#layers).

```image
{"id": "editor-overview", "src": "assets/screenshots/editor-en.png", "alt": "Retouch workspace", "title": "Retouch workspace", "controls": "minimal", "annotations": [{"id": "tools", "type": "rect", "x": 0.004, "y": 0.11, "toX": 0.04, "toY": 0.91, "label": "1 — Tools"}, {"id": "layers", "type": "rect", "x": 0.83, "y": 0.11, "toX": 0.993, "toY": 0.985, "label": "2 — Layers"}]}
```

## Draw a gradient {#gradient}

Select a pixel layer and choose the colors. Press **D**, then drag from the start to the end of the gradient. The options bar lets it end with the second color or with transparency. Use a separate layer and Ctrl+Z to try again.
