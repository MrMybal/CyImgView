<table width="100%">
  <tr>
    <td width="300" align="left" valign="top">
      <a href="https://mrmybal.github.io/CyImgView/?lang=fr">Site web</a><br>
      <a href="https://mrmybal.github.io/CyImgView/Documentation.html#/fr/overview">Manuel</a><br>
      <a href="https://github.com/MrMybal/CyImgView">GitHub</a><br><br>
      <sub>README</sub><br>
      <a href="README.md">English</a> · <a href="README.fr.md">Français</a>
    </td>
    <td width="400" align="center" valign="middle">
      <a href="https://mrmybal.github.io/CyImgView/?lang=fr"><img src="docs/logo.png" alt="Logo de CyImgView" width="128"></a>
    </td>
    <td width="300" align="right" valign="top">
      <p><sub>Licence de l’application</sub><br><a href="LICENSE"><strong>GNU GPL v3.0</strong></a></p>
      <p><sub>Version</sub><br><a href="docs/releases/1.11.0.md#français"><strong>1.11.0</strong></a><br><strong>Développement actif</strong></p>
      <p><a href="https://github.com/MrMybal/CyImgView/releases/download/v1.11.0/CyImgView-1.11.0-Setup.exe"><strong>Télécharger</strong></a><br><sub>Windows x64 · installeur</sub><br><a href="https://github.com/MrMybal/CyImgView/releases/download/v1.11.0/CyImgView-1.11.0-win64.zip">ZIP portable</a></p>
    </td>
  </tr>
</table>

<h1 align="center">CyImgView</h1>

Une visionneuse Windows native avec un éditeur pratique pour les photographes, les artistes et le travail quotidien sur les images. Parcourez un dossier, examinez les pixels, peignez, annotez, réglez une photo et enregistrez des projets modifiables.

> **Développement actif · 1.11.0**
>
> L’installeur et le ZIP portable Windows x64 sont disponibles. Cette version ajoute un éditeur de métadonnées avec recherche, un moteur natif et une API de plugins de lecture/écriture. L’écriture dépend du format et du groupe de métadonnées. Consultez les [changements et limites documentées](docs/releases/1.11.0.md#français).

[![CyImgView affichant un paysage, avec la barre d’outils et la pellicule du dossier](.CyDocBuild/assets/screenshots/viewer-fr.png)](.CyDocBuild/assets/screenshots/viewer-fr.png)

## L’application

Ouvrez une image et parcourez son dossier avec la pellicule. Zoomez autour du pointeur, examinez les canaux RVB/alpha et les informations EXIF, ou avancez dans une animation image par image. Passez en retouche pour peindre, ajouter du texte ou un cadre, ou composer avec des calques.

Créez une image vierge à partir d’un modèle écran, photo ou impression, puis gardez un **projet .cyimg** pour conserver calques, objets et masques. Utilisez PNG/JPEG pour une image aplatie ou exportez les calques convertis en pixels vers PSD. Développement et Noir et blanc proposent des réglages photo ; la galerie d’effets permet de prévisualiser un rendu et de choisir sa cible avant de l’appliquer.

<p><img src="docs/cyimg-document.png" alt="Icône de document CyImg, avec un coin replié et des calques" width="48"> Les documents natifs .cyimg ont leur propre icône, distincte du logo rond de l’application.</p>

### Modes d’édition et effets

Cliquez sur une capture pour l’ouvrir en grand.

<table>
  <tr>
    <th>Retouche et calques</th>
    <th>Développement photo</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/editor-fr.png"><img src=".CyDocBuild/assets/screenshots/editor-fr.png" alt="Retouche et calques" width="100%"></a>
      <p>Textes et formes modifiables, avec les commandes des calques.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/develop-fr.png"><img src=".CyDocBuild/assets/screenshots/develop-fr.png" alt="Développement photo" width="100%"></a>
      <p>Histogramme, balance des blancs, tonalité et couleurs.</p>
    </td>
  </tr>
  <tr>
    <th>Noir et blanc</th>
    <th>Galerie d’effets</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/black-white-fr.png"><img src=".CyDocBuild/assets/screenshots/black-white-fr.png" alt="Noir et blanc" width="100%"></a>
      <p>Préréglages, mélangeur de couleurs et contraste.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/effects-fr.png"><img src=".CyDocBuild/assets/screenshots/effects-fr.png" alt="Galerie d’effets" width="100%"></a>
      <p>Comparez les aperçus et choisissez le calque ou l’image entière.</p>
    </td>
  </tr>
  <tr>
    <th>Réglage d’un effet en direct</th>
    <th>Modèles de nouveaux documents</th>
  </tr>
  <tr>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/effect-preview-fr.png"><img src=".CyDocBuild/assets/screenshots/effect-preview-fr.png" alt="Réglage d’un effet en direct" width="100%"></a>
      <p>Ajustez l’effet avec un aperçu, puis appliquez ou annulez.</p>
    </td>
    <td width="50%" valign="top">
      <a href=".CyDocBuild/assets/screenshots/new-document-fr.png"><img src=".CyDocBuild/assets/screenshots/new-document-fr.png" alt="Modèles de nouveaux documents" width="100%"></a>
      <p>Formats écran, photo et impression, avec dimensions personnalisées.</p>
    </td>
  </tr>
</table>

## Fonctionnalités

- **Parcourir et examiner** : pellicule du dossier, zoom centré sur la souris, lecture des animations, canaux, informations d’image et diaporamas.
- **Peindre et composer** : pinceau, gomme, sélections, recadrage, remplissage, dégradé et textes, flèches et formes modifiables ; calques avec opacité, fusion et masques.
- **Réglages photo** : exposition, balance des blancs, courbes, couleur et Noir et blanc, avec aperçu, application/annulation et historique.
- **Effets et extensions** : plugins natifs et effets HLSL personnalisés ; application au calque actif ou à l’image entière. Aucun modèle IA ni service d’amélioration d’image n’est fourni.
- **Métadonnées** : lecture et modification EXIF, IPTC, XMP, GPS, ICC et textes PNG ; ajout de DLL de lecture/écriture indépendantes des décodeurs. **I** / **Ctrl+I** ouvre cet espace ; l’enregistrement conserve une sauvegarde de l’original. [Guide des métadonnées](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/metadata).
- **Formats** : images courantes, photos RAW, formats web modernes, aperçus de documents et textures techniques. Consultez la [référence des formats et ses limites](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/formats).
- **Utilisation locale** : aucun compte ni télémétrie. L’interface propose 12 langues ; le manuel illustré complet existe en français et en anglais, en ligne et hors ligne.

<a id="manuel-utilisateur"></a>

Appuyez sur **F1** ou **?** pour l’aide contextuelle. Le [manuel illustré CyDoc](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/overview) comprend navigation, recherche, captures, schémas et comparaisons d’images. L’application ouvre `Documentation/Documentation.html` sans serveur ni connexion Internet ; les autres langues d’interface utilisent le manuel anglais.

---

## Installation

Les versions publiées sont destinées à **Windows x64**. L’exécutable principal utilise C++ natif et les API graphiques de Windows ; Node.js, Python et un environnement d’exécution distinct ne sont pas nécessaires pour utiliser l’application. Gardez les plugins fournis, dont la bibliothèque d’exécution OpenMP du plugin RAW.

1. [Téléchargez l’installeur 1.11.0](https://github.com/MrMybal/CyImgView/releases/download/v1.11.0/CyImgView-1.11.0-Setup.exe) ou le [ZIP portable](https://github.com/MrMybal/CyImgView/releases/download/v1.11.0/CyImgView-1.11.0-win64.zip).
2. Lancez l’installeur pour une installation par utilisateur sans droits administrateur, ou extrayez **toute** l’archive portable.
3. Lancez **CyImgView.exe**. En version portable, gardez `plugins/` et **tout le dossier `Documentation/`** à côté de l’exécutable.

L’installeur enregistre les projets natifs `.cyimg` avec leur icône de document. Les mises à jour intégrées passent par GitHub ; les copies portables ne remplacent pas l’association de la version installée. [Installation, mises à jour et désinstallation](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/installation).

<a id="compiler"></a>

## Sources, compilation et bibliothèques nécessaires

<details>
<summary>Compiler CyImgView depuis les sources</summary>

Prérequis : Windows, **Visual Studio 2022** avec la charge **Développement Desktop en C++** et le SDK Windows, ses outils **CMake/Ninja**, et **Node.js 22+ avec npm** pour générer le manuel.

```powershell
git clone https://github.com/MrMybal/CyImgView.git
cd CyImgView
npm --prefix .CyDocBuild ci --no-audit --no-fund
.\build.ps1
.\build\Release\bin\CyImgView.exe
```

Les sources des bibliothèques de décodage natives sont déjà dans `third_party/` ; le tableau ci-dessous indique leurs rôles. La dépendance CyDoc est figée dans `.CyDocBuild/vendor/` et npm installe ses outils de génération. Seuls les modules image et gallery du lecteur sont exportés, sans Three.js. Node.js sert à générer le manuel, pas à le consulter.

Les fichiers produits se trouvent dans `build/Release/bin/`. Pour créer un installeur et une archive portable, lancez `./tools/package.ps1` ; il effectue une compilation Release complète et inclut tout le manuel hors ligne. Voir le [script de compilation](build.ps1), la [configuration CMake](CMakeLists.txt) et l’[outillage documentaire](.CyDocBuild/README.md).

</details>

---

## Licence

**CyImgView utilise la [licence GNU GPL v3.0](LICENSE).** Les composants tiers conservent leurs propres licences ; leurs conditions ne remplacent pas celles de l’application.

### Bibliothèques natives et composants d’exécution

| Bibliothèque · sources | Licence · texte | Utilisation / périmètre |
| --- | --- | --- |
| [LibRaw](third_party/LibRaw) | [LGPL-2.1](third_party/LibRaw/LICENSE.LGPL) OR [CDDL-1.0](third_party/LibRaw/LICENSE.CDDL); [notices des composants](third_party/LibRaw/COPYRIGHT) | Décodage RAW dans le plugin `raw` fourni. |
| [libwebp](third_party/libwebp) | [BSD-3-Clause](third_party/libwebp/COPYING) + [concession de brevets](third_party/libwebp/PATENTS) | WebP fixe et animé dans le plugin `webp`. |
| [libheif](third_party/libheif) | [LGPL-3.0 licence de la bibliothèque](third_party/libheif/COPYING) | Conteneurs HEIF/HEIC/AVIF dans le plugin `heif`. |
| [libde265](third_party/libde265) | [LGPL-3.0 licence de la bibliothèque](third_party/libde265/COPYING) | Décodage HEVC utilisé par `heif`. |
| [dav1d](third_party/dav1d) | [BSD-2-Clause](third_party/dav1d/COPYING) | Décodage AV1 utilisé par `heif`. |
| [libjxl](third_party/libjxl) | [BSD-3-Clause](third_party/libjxl/LICENSE) + [concession de brevets](third_party/libjxl/PATENTS) | Décodage JPEG XL et animation dans `jxl`. |
| [Highway](third_party/highway) | [Apache-2.0 OR BSD-3-Clause; licences des composants](third_party/highway/LICENSE) | SIMD pour `jxl` ; la notice identifie aussi le composant distinct sous CC0. |
| [OpenJPEG](third_party/openjpeg) | [BSD-2-Clause](third_party/openjpeg/LICENSE) | JPEG 2000 dans `jpeg2000`. |
| [OpenEXRCore](third_party/openexr) / [Imath](third_party/Imath) | [BSD-3-Clause: OpenEXR](third_party/openexr/LICENSE.md), [Imath](third_party/Imath/LICENSE.md) | Décodage EXR et nombres flottants half dans `exr`. |
| [libdeflate](third_party/libdeflate) | [MIT](third_party/libdeflate/COPYING) | Prise en charge ZIP/ZIPS dans `exr`. |
| [Zstandard](third_party/openexr/external/zstd) | [BSD-3-Clause](third_party/openexr/external/zstd/LICENSE) | Prise en charge ZSTD dans `exr`. |
| [OpenJPH](third_party/openexr/external/OpenJPH) | [BSD-2-Clause](third_party/openexr/external/OpenJPH/LICENSE) | Prise en charge HTJ2K dans `exr`. |
| [Exiv2 0.28.9](third_party/exiv2) | [GPL-2.0-or-later](third_party/exiv2/LICENSE.txt) ; SDK XMP intégré [BSD-3-Clause](third_party/exiv2/doc/COPYING-XMPSDK) | Fournisseur natif de métadonnées : EXIF, IPTC, XMP, GPS, commentaires et ICC. |
| [Expat 2.10.0](third_party/expat/expat) | [MIT](third_party/expat/expat/COPYING) | Lecture XML locale des métadonnées XMP. |
| [zlib 1.3.2](third_party/zlib) | [Licence zlib](third_party/zlib/LICENSE) | Métadonnées PNG compressées. |
| [Microsoft OpenMP](plugins/raw/plugin.cmake) | [Conditions de redistribution Visual Studio](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution#visual-c-runtime-files) | `vcomp140.dll`, fourni pour LibRaw multithread. |

### Documentation, génération et outils de maintenance

| Bibliothèque · sources | Licence · texte | Utilisation / périmètre |
| --- | --- | --- |
| [CyDoc 0.1.0](https://github.com/MrMybal/CyDoc/tree/6ddf9a582defb2f11fbb3f992afef6f98dcadc93) | [MIT](https://github.com/MrMybal/CyDoc/blob/6ddf9a582defb2f11fbb3f992afef6f98dcadc93/LICENSE) | Paquet local figé pour la génération et lecteur hors ligne fourni ; image/gallery uniquement, sans Three.js. |
| [marked 17.0.6](https://github.com/markedjs/marked) | [MIT](https://github.com/markedjs/marked/blob/v17.0.6/LICENSE.md) | Compilation Markdown par CyDoc pendant la génération. |
| [PrismJS 1.30.0](https://github.com/PrismJS/prism) | [MIT](https://github.com/PrismJS/prism/blob/v1.30.0/LICENSE) | Coloration du code par CyDoc pendant la génération. |
| [esbuild 0.25.12](https://github.com/evanw/esbuild) | [MIT](https://github.com/evanw/esbuild/blob/v0.25.12/LICENSE.md) | Assemblage du lecteur par CyDoc pendant la génération. |
| [Happy DOM 21.0.4](https://github.com/capricorn86/happy-dom) | [MIT](https://github.com/capricorn86/happy-dom/blob/v21.0.4/LICENSE) | Vérifications du manuel et de la vitrine en développement, sans rôle à l’exécution de l’application. |
| [Pillow](https://github.com/python-pillow/Pillow) | [MIT-CMU et notices des composants](https://github.com/python-pillow/Pillow/blob/main/LICENSE) | Outil optionnel de préparation des images de la vitrine ; inutile pour une compilation normale. |
| [sharp](https://github.com/lovell/sharp) | [Apache-2.0](https://github.com/lovell/sharp/blob/main/LICENSE) + notices des composants natifs | Outil optionnel de régénération des icônes de document ; absent de l’application distribuée. |

Conservez les notices complètes fournies dans `licenses/` et `Documentation/DocData/`. Ce tableau est un index, pas un remplacement des notices des composants et dépendances transitives. Les API Windows suivent les conditions de Windows et du SDK. Oodle est optionnel, propriétaire et **non distribué** ; les textures Unreal prises en charge peuvent utiliser une bibliothèque compatible provenant de l’installation de l’utilisateur.

## Références et liens

<a id="références"></a>

<a id="mises-à-jour-github-releases"></a><a id="utilisation"></a><a id="mode-édition"></a><a id="paramètres-touche-p-ou-bouton-engrenage"></a><a id="écrire-un-plugin"></a><a id="écrire-un-plugin-dédition"></a><a id="mesurer-les-performances"></a><a id="confidentialité"></a><a id="features"></a>

- [Documentation](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/overview) · [Site web](https://mrmybal.github.io/CyImgView/?lang=fr) · [Dépôt des sources](https://github.com/MrMybal/CyImgView).
- [Notes de version et limites](docs/releases/1.11.0.md#français) · [Versions publiées](https://github.com/MrMybal/CyImgView/releases) · [Suivi des problèmes](https://github.com/MrMybal/CyImgView/issues).
- [Premiers pas](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/quickstart) · [Visionneuse](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/viewer) · [Retouche](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/edit).
- [Calques et masques](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/layers) · [Effets](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/effects) · [Projets et export](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/export).
- [Images vierges](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/new-document) · [Développement](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/develop) · [Noir et blanc](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/black-white) · [Raccourcis](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/keyboard-reference).
- [API des plugins](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/editor-api) · [API de décodage](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/decoder-api) · [Contrat HLSL](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/hlsl).
- [Paramètres et langues](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/settings) · [Confidentialité](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/privacy) · [Dépannage](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/troubleshooting).
- [Mises à jour](https://mrmybal.github.io/CyImgView/Documentation.html#/fr/updates) · [Notes de mesure des performances](.CyDocBuild/internal/previous-manual/fr/performance.md).
