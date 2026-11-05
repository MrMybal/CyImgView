---
id: privacy
title: Privacy and local files
description: Privacy and local files — CyImgView.
section: Reference
order: 120
revision: 1
---

## Privacy and local files {#privacy}

- **No telemetry**, account or advertising.
- **Only network connection**: update checking against the public GitHub API
  (`api.github.com/repos/MrMybal/CyImgView/releases/latest`), at most every 20 hours, followed
  by the release archive download if you accept the update. No personal data is sent
  (`User-Agent: CyImgView/<version>`). Disable it in Settings.
- **Local files**: settings in `%APPDATA%\CyImgView\settings.ini` (and `develop_last.ini`
  for Develop settings), optional crash log in `%LOCALAPPDATA%\CyImgView\crash.txt`
  (never sent), and temporary “Open with” exports in `%TEMP%\CyImgView` (deleted after 24 hours).
- **DICOM**: patient-identifying data (name, ID, date of birth) is never displayed.
- “Open with” launches only applications you selected or that Windows associates with the format.



## Local manual {#manual}

The reader contacts no service. External links open only when you follow them. The browser may save the theme, language, bookmarks, notes and personal annotations in its local storage. These can be unavailable or lost if storage is blocked or the manual moves to a different path.
