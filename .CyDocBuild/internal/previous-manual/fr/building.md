---
id: building
title: Compiler et distribuer
description: Compiler et distribuer — CyImgView.
section: Développeurs
order: 210
sourceRevision: 1
---

## Compiler et distribuer {#build}

Prérequis : Node.js 22+ pour le manuel local, et Visual Studio 2022 avec la charge de travail « Développement Desktop en C++ ».
CMake et Ninja sont fournis par Visual Studio.

```bat
build.cmd            :: Release -> build\Release\bin\CyImgView.exe
build.cmd Debug
```

Contenu du dossier `build\Release\bin` :

```
CyImgView.exe
cyimg_bench.exe       banc d'essai du décodage (console)
plugins\raw.dll        plugin RAW (LibRaw intégré)
plugins\vcomp140.dll   runtime OpenMP de Microsoft (LibRaw multithread)
plugins\uasset.dll     plugin textures Unreal Engine
plugins\tga.dll        plugin TGA
plugins\webp.dll       WebP (libwebp)
plugins\heif.dll       HEIC / HEIF / AVIF (libheif, libde265, dav1d)
plugins\jxl.dll        JPEG XL (libjxl, highway)
plugins\jpeg2000.dll   JPEG 2000 (OpenJPEG)
plugins\exr.dll        OpenEXR (OpenEXRCore, libdeflate, zstd, OpenJPH)
plugins\vector.dll     SVG, PDF, AI, WMF, EMF, DXF (Direct2D, Windows.Data.Pdf, GDI+)
plugins\psd.dll        Photoshop PSD / PSB
plugins\docs.dll       XCF, KRA, ORA, CLIP, INDD, EPS, CDR, Affinity
plugins\dds.dll        DirectDraw Surface
plugins\dicom.dll      DICOM
plugins\classic.dll    Netpbm, PCX, XBM, XPM, IFF, HDR, FITS, ICNS, PICT
plugins\fx_extra.dll   plugin d'édition : effets supplémentaires (bruit, seuil, bichromie, aberration)
plugins\annotate.dll   plugin d'édition : outils d'annotation (pastille, surligneur, flou de masquage, bulle)
plugins\bw_mode.dll    plugin d'édition : mode Noir & blanc
plugins\share_actions.dll  plugin d'édition : Ouvrir avec → Imprimer…, Ouvrir dans le navigateur
plugins\hlsl.dll       effets HLSL
plugins\effects\       exemple de shader fourni
Documentation\Documentation.html        manuel CyDoc hors ligne (garder tout le dossier Documentation)
```

`-DCYIMG_TEST_PLUGINS=ON` compile en plus `bin\testplugins\crashtest.dll`, un plugin d'édition
volontairement défaillant pour tester la robustesse de l'hôte (jamais livré : `tools\package.ps1` ne
prend que `bin\plugins`). `python tools\check_lang.py` vérifie les fichiers de langue (clés,
spécificateurs printf, listes de choix).

Chaque plugin a son dossier `plugins\<nom>\` avec un `plugin.cmake`, inclus automatiquement. Pour
ne compiler que certains plugins : `-DCYIMG_ONLY_PLUGINS="webp;psd"`.

Pour déployer l'application, copiez `CyImgView.exe` et les dossiers `plugins` et `Documentation` complets.

Les sources de LibRaw 0.22.2 se trouvent dans `third_party/LibRaw` (double licence LGPL 2.1 /
CDDL 1.0). Elles sont utilisées sous LGPL 2.1, compatible avec la GPL-3.0 du projet.

L'installeur (`setup-stub.exe` + archive) est produit par `tools\package.ps1`.

## Générer le manuel local {#manual}

Node.js 22 ou plus est requis pour générer l’aide, pas pour la lire. Le paquet CyDoc est fixé localement. Exécutez :

```powershell
npm --prefix .CyDocBuild ci
node .CyDocBuild/build.mjs --check
npm --prefix .CyDocBuild test
```

Le build CMake génère et copie `Documentation/` à côté de l’exécutable ; le packaging copie le dossier complet.

## Mettre à jour les traductions de l’interface {#translations}

Les clés de l’interface sont dans `src/lang`. Lancez `python tools/check_lang.py`. Les traductions du manuel sont dans `.CyDocBuild/manual/en` et `.CyDocBuild/manual/fr` ; conservez les identifiants et les métadonnées revision/sourceRevision.
