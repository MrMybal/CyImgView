# CyImg document icon

`src/cyimg-document.svg` is the editable vector source. The paper silhouette, folded corner,
stacked image layers and CYIMG label distinguish native `.cyimg` editing projects from the
round CyImgView application icon. It has no linked fonts or images.

`src/cyimg-document.ico` contains transparent 32-bit PNG frames at 16, 20, 24, 32, 40, 48,
64, 96, 128 and 256 pixels. `docs/cyimg-document.png` is a 512-pixel transparent preview.
These generated files are committed; normal builds require no graphics dependency.
To regenerate, with the optional `sharp` maintainer dependency available:

```powershell
node tools/build_document_icon.mjs
# Or pass an existing sharp installation without adding a build dependency:
node tools/build_document_icon.mjs C:\path\to\node_modules\sharp
```

The application keeps icon resource 1; document icon resource 101 is registered as
`"CyImgView.exe",-101` under `CyImgView.Project`. The installer adds the native format to
Open With and Default Apps. The installed application repairs this registration at startup
after an automatic update. Portable copies do not change installed registrations.
Existing defaults belonging to other applications and Explorer's `UserChoice` remain intact.
Uninstall removes only CyImgView's native-format association.

For isolated Windows registry and embedded-resource checks, configure with
`-DCYIMG_SHELL_TESTS=ON`, build `CyImgView`, `CyImgViewSetup` and `cyimg_shell_tests`, then run:

```powershell
.\build\Release\bin\cyimg_shell_tests.exe .\build\Release\bin\CyImgView.exe
```

The test redirects HKCU to a disposable per-process registry hive, preserving real associations.
