# Documentation tooling / Outillage documentaire

This directory contains authoring inputs and build tools. It is never included in the application's manual export. Human-readable documentation is generated into `../Documentation/Documentation.html`.

Ce dossier contient les sources et outils de génération. Il n’est pas distribué avec le manuel utilisateur. Les notes internes restent dans `internal/`.

## Generate and validate / Générer et vérifier

From the application root, with Node.js 22 or later:

```powershell
npm --prefix .CyDocBuild ci --no-audit --no-fund
node .CyDocBuild/build.mjs --check
npm --prefix .CyDocBuild test
```

`--check` also writes the complete export. `--out build/Release/bin/Documentation` generates the installed copy; CMake and `tools/package.ps1` do this automatically. No Node runtime is required to read it.

The export is exactly `Documentation.html`, `DocAI.md` and `DocData/`. Open the HTML directly through `file://`; do not start a web server. `DocAI.md` is generated separately by CyDoc and never indexed by the human reader.

## Authoring / Rédaction

- `manual/en/` is the original and default language; `manual/fr/` is mandatory and complete. Share stable page IDs, explicit anchors and media IDs. Update `revision` and `sourceRevision` together after reviewing both translations.
- Use `parent` and `section` for the navigation tree. Every page needs a helpful localized `description` for tooltips and search.
- Keep images in `assets/`; use reviewed language-specific screenshots. Static diagrams compile to SVG.
- Only `image` and `gallery` are selected: annotated screenshots, zoom, image comparison and workspace collections. No video, GIF, graph, mesh or Three.js is needed by the current manual.
- API references come from `api/catalog.json` plus explicit C ABI declarations in two SDK headers, imported with CyDoc's documented neutral-format API adapter. `generate_api.mjs` checks signatures and translated parameter descriptions; it is a narrow declaration extractor, not a C++ semantic parser. Never enable full-source export or copy implementation trees.
- `internal/` holds developer notes and previous migration records, outside the content and asset roots.

## Distribution and public hosting

The pinned vendor archive and provenance in `vendor/` allow builds without private GitHub access. Installing build-time npm dependencies can require a connection; reading the generated manual never does.

`stage_pages.mjs` copies only the verified `Documentation/` export into `build/pages/` and adds a compatibility entry for historical `/#/en/...` and `/#/fr/...` URLs. The official generated reader remains untouched. The Pages workflow deploys that staging directory when changes are later pushed; local generation does not publish anything.

After a native build and local packaging:

```powershell
./tools/package.ps1 -NoBuild
python .CyDocBuild/check_package.py
```

The package check compares every documentation resource, the native executable and the installer's ZIP payload byte for byte. It also rejects private tools and full source listings in the manual.

Developer references retained outside the reader: [build (EN)](internal/previous-manual/en/building.md), [compilation (FR)](internal/previous-manual/fr/building.md), [benchmarks (EN)](internal/previous-manual/en/performance.md), [mesures (FR)](internal/previous-manual/fr/performance.md).
