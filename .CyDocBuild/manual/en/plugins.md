---
id: plugins
title: Install and write extensions
description: Install local extensions for image formats, tools, effects and actions.
section: Extensions
order: 170
revision: 2
---


## Plugin families {#overview}

Plugins are DLLs in the app’s `plugins` directory. Decoder plugins open additional formats; editing plugins add effects, tools, modes and Open with actions. HLSL files add image effects. Restart after adding an extension. Native DLLs execute code inside the app: use trusted extensions.

## Install an extension {#install}

1. Choose **Add tools / effects** in the effect gallery to open the plugins directory beside the running executable.
2. Copy DLLs into `plugins`, shaders into `plugins/effects`, and keep any files required by the extension.
3. Restart and check the appropriate tool, mode, effect or file-type list.

## Author an extension {#api}

- [Decoder ABI and example](doc:decoder-api#decoder-api).
- [Editing ABI, effects, tools and modes](doc:editor-api#editor-api).
- [HLSL shader contract](doc:hlsl#contract).

## Modes and future AI tools {#modes}

The existing C API can host future image-enhancement tools. No AI model, cloud endpoint or automatic plugin marketplace is bundled. The API reference describes public signatures and short examples. It does not publish implementation files.

```diagram
{"type": "flow", "title": "Local extension points", "nodes": [{"id": "0", "label": "CyImgView", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "DLL / HLSL extensions", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Images, tools, effects", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```

## Open with actions {#actions}

The **Open with** button offers associated applications and plugin actions, including Print and Open in browser. During editing, the action receives a temporary PNG copy of the visible result.
