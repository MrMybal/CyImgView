---
id: updates
title: Updates and releases
description: Updates and releases — CyImgView.
section: Developers
order: 220
revision: 1
---

## Updates and releases {#updates}

CyImgView checks for new versions a few seconds after startup, at most once every 20 hours,
or manually with **Ctrl+U**. An “Update X.Y.Z” badge appears in the top bar. Choose to install
and restart, view the release or skip that version. Installation downloads the release's
`*-win64.zip`, verifies SHA-256 when supplied by GitHub, replaces files including the running
executable, then restarts on the same image. On failure, everything is restored.
Disable with `auto=0` in `[update]` in `%APPDATA%\CyImgView\settings.ini`.

Configure the repository in `src/version.h`: `#define CYIMG_UPDATE_REPO L"owner/repository"`.
The feature is disabled while that field is empty.

To publish a version:

1. Increment `CYIMG_VERSION_MAJOR/MINOR/PATCH` in `src/version.h` and commit.
2. Run `git tag v1.10.0 && git push origin v1.10.0`. The `.github/workflows/release.yml`
   workflow builds, packages and creates the release with the ZIP.

Without GitHub Actions, `tools\package.ps1` produces `dist\CyImgView-<version>-win64.zip`
for attachment to a release tagged `v<version>`.

Oodle (`oo2core_*.dll`) is never included: it belongs to Epic and is loaded from the user's Unreal installation.


