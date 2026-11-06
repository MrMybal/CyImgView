# CyDoc update — internal integration record

This directory is outside the public content and asset roots. It is not copied to `Documentation/` or application packages.

## Migration decisions (2026-11-06)

- Updated CyDoc from `774f2a7eabf7dd181a98ffc5add115b580b67f42` to `6ddf9a582defb2f11fbb3f992afef6f98dcadc93`, fetched from the authenticated private repository. The upstream package is still named version 0.1.0; the archive filename, lock integrity and provenance pin the actual commit.
- Reviewed the application and the five requested CyDoc AI guides. No application AGENTS.md was present. CyDoc's own AGENTS.md was not applied to application changes.
- Moved authoring, images and generation tools under `.CyDocBuild/`. Removed the previous custom viewer shell; official CyDoc now supplies the reader, search, wider sidebar, tree, tooltips, themes and local media loader.
- Public manual: 25 authored guides, one generated native-shortcut reference and 18 API entries per language (88 pages total), English default plus complete French. The application's twelve UI languages are retained; native help uses French for French UI, English for other UI languages.
- Retained former build and benchmark chapters here, outside the human manual. Previous migration and tooling documents are archived here rather than indexed. Screenshot provenance is in `screenshots.md`.
- Replaced full SDK/example source publication with reviewed neutral-format API imports, checking explicit public C ABI declarations. The limited extractor does not provide semantic C++ analysis. Short authored code examples remain in the guides.
- Selected only image and gallery: screenshots, annotations, comparisons and workspace collection. Static diagrams are SVG; no mesh, Three.js, graph, video or GIF dependency is justified.
- Retained old page aliases, common EN/FR anchors and media IDs. Old full-source page IDs lead to the relevant public guide; former source-line anchors cannot represent implementation lines after removal. Such obsolete section links fall back to the guide.
- F1 and contextual help now open `Documentation/Documentation.html`; the gradient tool has its own section. Build and package scripts ship the complete new folder. Public Pages staging includes only the export plus an authored redirect retaining old root URLs and fragments. Nothing was pushed or published during this update.

## Verification

- CyDoc `check` exports successfully, validates links, sections and all EN/FR translations. Additional application checks enforce parity of parents, headings, anchors and media IDs, local resources, modules, native help routes and the absence of internal/source pages.
- Twelve Node tests pass using actual CyDoc components in DOM fixtures: global search/Ctrl+K, tree, descriptions/tooltips, two languages, theme selection, reading positions, image keyboard zoom, in-window expansion/Escape, pointer/keyboard comparison and gallery reader lifecycle. These tests are not a browser rendering or visual layout check.
- Native Release build succeeds. Offscreen native integration suite: 108 assertions pass, including contextual F1, special-character file paths, unsupported manual-language fallback and preservation of edit state/history.
- `python tools/check_lang.py`: fourteen groups, 158 files, no issues.
- Local ZIP and installer verification compares every exported resource and native executable, validates the export layout and minimal modules, rejects tool/source trees in the manual, and compares installer payload bytes with the ZIP.
- The supported browser tool rejected `file://` navigation: its policy allows only HTTP(S) and explicitly forbids workarounds. No server, alternate browser or indirect launch was used. Actual file-URL rendering, narrow-layout overlap, touch input and F1 launching the default browser remain for a manual Windows check. The generated HTML path is provided to the user to open directly.
- No remote Actions execution or deployment was performed: the user explicitly requested no push until asked.

Build logs and temporary migration scripts are private ignored working files. Regeneration commands and package provenance are maintained in `../README.md` and `../vendor/PROVENANCE.md`.
