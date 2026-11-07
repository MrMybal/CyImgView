# Metadata engine maintenance

`include/cyimg_metadata_plugin.h` is the public C ABI. `src/metadata.*` loads provider DLLs,
collects typed fields and commits metadata on a private same-volume copy. The native workspace
is in `src/metadata_dialog.cpp`; I / Ctrl+I opens it through `viewer_metadata.cpp`.

The bundled provider in `plugins/metadata` combines Exiv2 with a PNG text-chunk reader/writer.
PNG text editing copies IDAT/fdAT and unrelated chunks verbatim. Exiv2 is statically linked,
with Expat and zlib. Every dependency has its source URL/archive hash in `CYIMG-SOURCE.txt`.
The sole upstream source patch preserves explicit empty XMP array items; its regression test
also covers escaped newlines, commas, Unicode and backslashes. The Windows I/O adapter avoids
Exiv2's ANSI pathname limitation without modifying its file-I/O implementation.

```powershell
.\build.ps1 -Clean -MetadataTests
.\build\Release\bin\cyimg_metadata_tests.exe
# Also exercise the native dialog and retain EN/FR screenshots in metadata-fixtures-<pid>:
.\build\Release\bin\cyimg_metadata_tests.exe --keep
node .CyDocBuild/build.mjs --check
node --test .CyDocBuild/test.mjs .CyDocBuild/ui.test.mjs
.\tools\package.ps1 -NoBuild
python .CyDocBuild/check_package.py
```

The independent example provider is `tools/metadata_test_plugin.cpp`. It is built only in
`bin/testplugins`, never shipped. Tests load its DLL through the real provider ABI and verify
successful writes, a failing writer that already modified its private copy, and a native
access violation. Source bytes must remain intact for either failure. JPEG/PNG/TIFF fixtures,
ICC profiles, repeated fields, source-change detection, backup bytes and PNG/JPEG compressed
image payloads are checked with the real engine. Release CI runs the console suite before packaging.

The human guides and reviewed API import live under `.CyDocBuild/manual/{en,fr}/metadata*.md`
and `.CyDocBuild/api/catalog.json`. Keep both translations and their stable anchors aligned.
Only image/gallery CyDoc modules are used; diagrams remain static SVG. Reading the installed
manual needs no Node runtime, server or Internet. Metadata writes are distinct from pixel/layer
edits; structural fields and formats without a suitable writer stay read-only.
