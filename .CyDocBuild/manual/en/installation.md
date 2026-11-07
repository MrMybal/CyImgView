---
id: installation
title: Install CyImgView
section: Guides
order: 5
revision: 3
description: Install the app, keep a portable copy and remove it when needed.
parent: overview
---


## Installation {#installation}

Download `CyImgView-<version>-Setup.exe` from
[releases](https://github.com/MrMybal/CyImgView/releases/latest) and run it.

- **No administrator rights**: installs in `%LOCALAPPDATA%\Programs\CyImgView`.
- **Windows integration**: Start menu shortcut, optional desktop shortcut, “Open with” entries
  for images, RAW photos and Unreal textures, and CyImgView listed in *Settings > Default apps*.
- **Updates**: automatic through GitHub.
- **Uninstall**: *Settings > Apps*, or `uninstall.exe` in the installation folder.
- **Silent installation**: `CyImgView-<version>-Setup.exe /S`.

Portable version: `CyImgView-<version>-win64.zip` contains the executable and `plugins` folder.

Keep the entire `Documentation` directory beside the executable for offline help.

## Recognize CyImg projects {#documents}

Editable `.cyimg` projects use a document icon with a folded corner, stacked image layers and a CYIMG label. The round CyImgView logo identifies the application itself.

The installer registers this native format. If another application is already your default, select CyImgView for `.cyimg` in **Settings > Apps > Default apps** when you want to change it. Windows keeps your explicit default-app choice. Portable copies do not register themselves automatically.
