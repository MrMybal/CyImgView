---
id: changelog
title: What changed?
description: Discover recent changes that affect everyday image work.
section: About
order: 250
revision: 6
---


## 1.11.1 — Complete RAW runtime distribution {#v1-11-1}

- The installer and portable ZIP include the native OpenMP runtime needed by the RAW plugin, including when built with newer Visual Studio versions on GitHub.
- Build and package validation now stop if that dependency is missing. Metadata features and the manual from 1.11.0 are included.

## 1.11.0 — Metadata workspace and provider plugins {#v1-11-0}

- Press **I** or **Ctrl+I** to search and inspect typed metadata, add or remove fields, and save changes or a separate copy. Export the complete displayed metadata to JSON.
- The bundled native provider reads EXIF, IPTC, XMP, GPS, ICC profiles and comments; PNG text chunks also expose generation parameters and other arbitrary text. Write support depends on the container and metadata group.
- Metadata saves run on a private file copy, check for source changes, verify the written fields and keep a backup when replacing the source. Structural fields and formats without a writer remain read-only.
- Add independent metadata readers and writers through the public provider DLL API. Read the [metadata guide](metadata.md) and [provider reference](metadata-api.md).
- These operations edit the image file on disk independently of unsaved pixel/layer edits. Export native `.cyimg` projects to an image before using the metadata workspace.

## 1.10.2 — Dedicated CyImg document icon {#v1-10-2}

- Recognize editable `.cyimg` projects by their document icon with stacked image layers and a CYIMG label, distinct from the round application logo.
- The installer registers the native project format with Windows. The installed app refreshes its project handler after an automatic update.
- Existing defaults for other applications and ordinary image formats remain unchanged. Portable copies do not replace the installed file association.

## 1.10.1 — Updated documentation reader {#v1-10-1}

- Search from the top bar or Ctrl+K and browse organized guide and API sections with descriptive tooltips.
- Read the entire manual in English or French, including screenshots, diagrams and public API references.
- F1 opens `Documentation/Documentation.html` at the current topic; the gradient tool has its own guide. Other interface languages open the complete English manual.
- Keep the entire `Documentation/` folder beside the executable. The manual continues to work without an Internet connection.

## 1.10.0 — Illustrated local manual {#v1-10-0}

- Read the complete English/French manual with screenshots, diagrams and image comparisons, without an Internet connection.
- Use F1 and the help buttons for the current viewer, tool, effect or editing mode. The new-document dialog opens its template guide.
- Find the installed-plugin list in the viewer context menu.
- The portable distribution and installer include the complete `Documentation/` folder. Keep it beside the executable.

## 1.9.0 — Documents, objects and effects {#v1-9-0}

- Create blank images from 18 screen, photo and print presets, with custom dimensions and transparent, white or black backgrounds.
- Keep shapes, arrows and text editable on separate layers; move them or rasterize them for painting.
- Use layer context menus, blend modes and editable layer masks. Save objects, masks and layers in `.cyimg` projects.
- Browse real effect previews on a shared test image, apply effects to one layer or the entire image, and add HLSL shaders through `plugins/effects/`.
- Cancel Black & White mode or undo its application. Hold the right mouse button on the canvas to pan without moving objects or painting.
- Fit images between the visible toolbars. New controls are available in English and French.

Extension points for future AI tools are included; no AI model or service is bundled.

The original 1.9.0 release notes are preserved in the repository under `docs/releases/1.9.0.md`.
