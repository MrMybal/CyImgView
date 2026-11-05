---
id: troubleshooting
title: Troubleshooting
description: Troubleshooting — CyImgView.
section: Guides
order: 125
revision: 1
---

## Find a solution {#problems}

| Problem | Try this |
|---|---|
| Dragging moves a layer | Hold the **right** button to move the view. V with a left drag moves content. |
| The image stays black and white | [Cancel the mode or undo its application](doc:black-white#cancel). |
| I cannot paint on text or shapes | Create a pixel layer or rasterize the object through its layer menu. |
| Painting seems invisible | Check the active layer, eye, opacity, mask-editing state and selection. |
| A shader is missing | Read its `.hlsl.errors.txt`, correct the contract, then restart. |
| An image does not open | Check [formats and limits](doc:formats#formats), then try an original export. |
| Create is disabled | Check dimensions and total pixel count. |
| Editable objects vanished after export | Reopen the `.cyimg` project; PNG/JPEG are flattened. |

## Missing local help {#help}

Keep the complete **help** directory next to `CyImgView.exe`. Copy the whole portable distribution, not just the executable or `index.html`. Reinstall or extract the complete distribution if F1 reports missing help. Reading needs a modern browser and JavaScript, but no server or Internet connection.

## Notes and reading positions {#storage}

Some browsers restrict storage for file URLs or private windows. CyDoc reports unavailable persistence; export notes as JSON. Keep the help path stable. Page positions persist during a reader session; explicit section and media links take priority.
