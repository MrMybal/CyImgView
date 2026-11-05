---
id: plugins
title: Install and write extensions
description: Install and write extensions — CyImgView.
section: Guides
order: 170
revision: 1
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
- [Build the app and examples](doc:building#build).

## Modes and future AI tools {#modes}

The existing C API can host future image-enhancement tools. No AI model, cloud endpoint or automatic plugin marketplace is bundled. The source index includes only public SDK headers and selected shipped examples. It is lexical, not a C++ semantic index.

```diagram
{"type": "flow", "title": "Local extension points", "nodes": [{"id": "0", "label": "CyImgView", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "DLL / HLSL extensions", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Images, tools, effects", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```
