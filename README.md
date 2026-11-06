<p align="center">
  <img src="docs/logo.png" alt="CyImgView logo" width="160">
</p>

<h1 align="center">CyImgView</h1>

**English** · [Français](README.fr.md) · [Documentation](https://mrmybal.github.io/CyImgView/#/en/overview)

A Windows image viewer built for speed: native C++, Direct2D GPU rendering, WIC and no
external runtime for the main executable. The distribution includes format plugins and a complete offline manual.

![CyImgView displaying a landscape with the image toolbar and folder filmstrip](.CyDocBuild/assets/screenshots/viewer-en.png)

## Editing modes and effects

Click a screenshot to open it at full size.

<table>
  <tr>
    <th>Retouch and layers</th>
    <th>Photo development</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/editor-en.png"><img src=".CyDocBuild/assets/screenshots/editor-en.png" alt="Retouch and layers" width="100%"></a>
      <p>Editable text, shapes and layer controls.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/develop-en.png"><img src=".CyDocBuild/assets/screenshots/develop-en.png" alt="Photo development" width="100%"></a>
      <p>Histogram, white balance, tone and color adjustments.</p>
    </td>
  </tr>
  <tr>
    <th>Black & white</th>
    <th>Effects gallery</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/black-white-en.png"><img src=".CyDocBuild/assets/screenshots/black-white-en.png" alt="Black & white" width="100%"></a>
      <p>Presets, color mixer and contrast controls.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/effects-en.png"><img src=".CyDocBuild/assets/screenshots/effects-en.png" alt="Effects gallery" width="100%"></a>
      <p>Compare previews and choose the layer or entire image.</p>
    </td>
  </tr>
  <tr>
    <th>Live effect adjustments</th>
    <th>New document templates</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/effect-preview-en.png"><img src=".CyDocBuild/assets/screenshots/effect-preview-en.png" alt="Live effect adjustments" width="100%"></a>
      <p>Adjust an effect, preview it, then apply or cancel.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/new-document-en.png"><img src=".CyDocBuild/assets/screenshots/new-document-en.png" alt="New document templates" width="100%"></a>
      <p>Screen, photo and print presets with custom dimensions.</p>
    </td>
  </tr>
</table>

## Installation

Download the installer or portable ZIP from [GitHub Releases](https://github.com/MrMybal/CyImgView/releases/latest).
The installer needs no administrator rights. Keep `plugins/` and **all of `Documentation/`** beside the executable in a portable installation.
[Installation, integration, updates and uninstall](https://mrmybal.github.io/CyImgView/#/en/installation).

## User manual

**[Documentation](https://mrmybal.github.io/CyImgView/#/en/overview)** — Read the complete visual manual online.

Press **F1** or click **?** in the application for contextual help. The manual opens locally from
`Documentation/Documentation.html`, with screenshots, diagrams, search and interactive image comparisons. It works offline without a server.
The complete manual is available in English and French. The application retains all 12 interface languages; help uses English for other interface languages.

- [Start here](https://mrmybal.github.io/CyImgView/#/en/quickstart) · [View and browse](https://mrmybal.github.io/CyImgView/#/en/viewer)
- [Create a blank document](https://mrmybal.github.io/CyImgView/#/en/new-document) · [Paint and edit objects](https://mrmybal.github.io/CyImgView/#/en/edit)
- [Layers, blending and masks](https://mrmybal.github.io/CyImgView/#/en/layers)
- [Effects gallery](https://mrmybal.github.io/CyImgView/#/en/effects) · [Develop](https://mrmybal.github.io/CyImgView/#/en/develop) · [Black & white](https://mrmybal.github.io/CyImgView/#/en/black-white)
- [Save projects and export](https://mrmybal.github.io/CyImgView/#/en/export) · [Keyboard reference](https://mrmybal.github.io/CyImgView/#/en/keyboard-reference)
- [Settings](https://mrmybal.github.io/CyImgView/#/en/settings) · [Troubleshooting](https://mrmybal.github.io/CyImgView/#/en/troubleshooting)

These links open the published HTML reader with navigation, search, images and diagrams. The same reader is also included offline with the application.

## Build

Visual Studio 2022 with Desktop C++, CMake/Ninja and **Node.js 22+** for manual generation:

```powershell
npm --prefix .CyDocBuild ci
./build.ps1
```

The application and complete help are generated in `build/Release/bin`.
[Build and distribution reference](.CyDocBuild/internal/previous-manual/en/building.md) · [Manual tooling](.CyDocBuild/README.md).

## Reference

User guides live in the manual; developer build and benchmark notes remain separate. Existing section addresses remain here as pointers.

<a id="features"></a><a id="fonctionnalités"></a>
- [Formats and features](https://mrmybal.github.io/CyImgView/#/en/formats)
<a id="compiler"></a>
- [Build and plugin selection](.CyDocBuild/internal/previous-manual/en/building.md)
<a id="updates-github-releases"></a><a id="mises-à-jour-github-releases"></a>
- [Updates and GitHub Releases](https://mrmybal.github.io/CyImgView/#/en/updates)
<a id="usage"></a><a id="utilisation"></a>
- [Viewer commands](https://mrmybal.github.io/CyImgView/#/en/keyboard-reference)
<a id="editing-mode"></a><a id="mode-édition"></a>
- [Editor commands](https://mrmybal.github.io/CyImgView/#/en/keyboard-reference) · [Document reference](https://mrmybal.github.io/CyImgView/#/en/editing-reference)
<a id="settings-p-or-gear-button"></a><a id="paramètres-touche-p-ou-bouton-engrenage"></a>
- [Settings reference](https://mrmybal.github.io/CyImgView/#/en/settings)
<a id="write-a-plugin"></a><a id="écrire-un-plugin"></a>
- [Decoder plugin API and code example](https://mrmybal.github.io/CyImgView/#/en/decoder-api)
<a id="write-an-editing-plugin"></a><a id="écrire-un-plugin-dédition"></a>
- [Editing plugin API and examples](https://mrmybal.github.io/CyImgView/#/en/editor-api) · [HLSL effects](https://mrmybal.github.io/CyImgView/#/en/hlsl)
<a id="benchmarking"></a><a id="mesurer-les-performances"></a>
- [Benchmarking](.CyDocBuild/internal/previous-manual/en/performance.md)
<a id="privacy"></a><a id="confidentialité"></a>
- [Privacy and local files](https://mrmybal.github.io/CyImgView/#/en/privacy)
<a id="license"></a><a id="licence"></a>
- [Licenses and credits](https://mrmybal.github.io/CyImgView/#/en/licenses) · [GNU GPL v3.0](LICENSE)
