---
id: settings
title: Settings and languages
section: Reference
order: 115
revision: 1
---

## Settings {#settings}

- **Open files in the existing window** (off by default): an image opened from Explorer replaces
  the existing window's image instead of creating a new window, including when opening several files.
- **Show filmstrip**.
- **Check for updates automatically**, plus a button to check immediately.
- **Interface language**: Automatic (Windows language) or one of the 12 languages.
- **Slideshow**: random order and image duration.
- **Image filtering**: Automatic (pixel-exact at 100%, smoothed when downscaling, sharp above 300%),
  Raw pixels (no smoothing, for pixel art), Linear, Cubic, HQ Cubic, Anisotropic.
  Also in the context menu; N toggles raw pixels.

Settings and window position are saved in `%APPDATA%\CyImgView\settings.ini`.



## Manual language {#languages}

F1 opens the manual in the application language. Detailed guides are available in English and French. The original shortcut help is retained in all twelve interface languages. Other pages fall back to English. The language selector keeps the page and section when switching languages. CyDoc’s reader controls are currently translated into English and French.

## Add or correct an application translation {#translation-files}

Place a UTF-8 `.lang` file in a `lang` folder beside the executable, then restart.
It supplements embedded texts and can add a language. Use existing keys; for
example, a French correction:

```text
# custom.fr.lang
@code = fr
@name = Français
viewer.btn.help = Aide
```

Values support `\n`, `\t` and `\\`. Preserve printf specifiers in the source
order and keep choice lists separated by `|`. Missing keys fall back from the
selected language to English, then French, then the identifier. This interface
fallback differs from the manual fallback. Contributors validate `src/lang`
with `python tools/check_lang.py`.
