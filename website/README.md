# CyImgView showcase / Vitrine

Local, dependency-free static website. Its presentation takes inspiration from the CyDAW showcase, with CyImgView's own identity and verified features. English is the default; `?lang=fr` selects the complete French version. No analytics, runtime fetches, third-party fonts, CDN or framework are required.

## Build and preview

From the repository root, with Node.js 22+ and the existing documentation dependencies installed:

```powershell
node .CyDocBuild/build.mjs --check
node .CyDocBuild/stage_pages.mjs
node website/check.mjs
node website/preview.mjs --port 4174
```

Open `http://127.0.0.1:4174/` (English) or `http://127.0.0.1:4174/?lang=fr` (French). Ctrl+C stops the loopback-only preview. The generated `build/pages/index.html` also works by opening the file directly in a normal browser; no runtime server is required. The automated browser currently supports HTTP(S) previews only.

## Public layout, when publication is requested

`build/pages/` contains the showcase `index.html`, content-fingerprinted resources in `site-assets/`, and the original CyDoc `Documentation.html`, `DocAI.md`, `DocData/` export. The existing GitHub Pages workflow builds and verifies this directory. Nothing in this directory is published until an authorized push triggers that workflow.

The application still ships the unmodified `Documentation/` folder, without the website. Old root documentation fragments such as `/#/fr/layers/masks` redirect to the real HTML reader; language-aware links now use its direct address.

## Content and captures

- `content.mjs`: complete English/French copy. Features and limitations come from the application and manual, not the CyDAW reference.
- `render.mjs`: semantic markup and static English fallback. Language switching and media interactions are in `app.js`.
- `style.css`: graphite surfaces, turquoise accents, compact introduction, full uncropped screenshots, mobile navigation and reduced-motion support.
- `assets/` and `asset-provenance.json`: optimized real screenshots from the documentation. Explicit crops isolate actual controls; no interface element has been added or retouched. The demonstration landscape is not private user data.
- `prepare_assets.py`: reproduces the optimized images using Pillow. Run only when captures change. No Python dependency is needed for a regular site build.
- `check.mjs`: checks locale coverage, image resources/dimensions, cache fingerprints, section links and contextual documentation routes, and compares the copied CyDoc export byte for byte.

The version and download URLs are derived from `src/version.h`. Version 1.10.1 was checked against the published GitHub release: Windows x64 installer and portable ZIP; it is not marked as a prerelease or labelled alpha/beta. No macOS/Linux package or bundled AI service is advertised.
