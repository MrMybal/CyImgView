# CyImgView

Visionneuse d'images pour Windows, pensée pour la vitesse : C++ natif, Direct2D (rendu GPU), WIC,
aucune dépendance externe. L'exécutable fait environ 600 Ko et n'a besoin d'aucun runtime.

## Installation

Téléchargez `CyImgView-<version>-Setup.exe` depuis les
[releases](https://github.com/MrMybal/CyImgView/releases/latest) et lancez-le.

- **Sans droits administrateur** : installation dans `%LOCALAPPDATA%\Programs\CyImgView`.
- **Intégration Windows** :
  - raccourci dans le Menu Démarrer, et sur le Bureau en option ;
  - entrée « Ouvrir avec » pour les images, photos RAW et textures Unreal ;
  - CyImgView proposé dans *Paramètres > Applications par défaut*.
- **Mises à jour** : automatiques depuis GitHub.
- **Désinstallation** : *Paramètres > Applications*, ou `uninstall.exe` dans le dossier d'installation.
- **Installation silencieuse** : `CyImgView-<version>-Setup.exe /S`.

Version portable : l'archive `CyImgView-<version>-win64.zip` contient l'exe et le dossier
`plugins`.

## Fonctionnalités

- **Formats** : JPEG, PNG, GIF, BMP, TIFF, ICO, JPEG XR, DDS, ainsi que WebP, HEIF et AVIF si les
  codecs Windows correspondants sont installés.
- **RAW** : tous les formats gérés par [LibRaw](https://www.libraw.org) (~1000 boîtiers : CR3, CR2,
  CRW, NEF, ARW, RAF, RW2, ORF, DNG, PEF, SRW, IIQ, 3FR…) via le plugin `raw`. L'affichage est
  progressif :
  1. L'aperçu JPEG embarqué par le boîtier s'affiche en 5 à 50 ms. Il est en pleine résolution sur
     la plupart des Canon (CR2, CR3) et des Nikon.
  2. Si cet aperçu est plus petit que le capteur (ARW, RAF, RW2…), LibRaw développe le RAW en
     arrière-plan (0,3 à 2 s). La version développée remplace l'aperçu, avec une tonalité calée sur
     le JPEG de l'appareil pour éviter un saut de luminosité.
- **Textures Unreal Engine** (`.uasset` non cookés, UE 4.x à 5.8) via le plugin `uasset` :
  - **Filtrage** : dans un dossier `Content`, seuls les `.uasset` qui sont des textures sont listés
    (Texture2D, TextureCube…) ; matériaux, meshes et blueprints sont ignorés.
  - **Pleine résolution** : les pixels source sont lus dans tous les cas connus (PNG, JPEG, brut,
    zlib, UEDELTA, données Oodle) et dans tous les formats (BGRA8, G8, G16, RGBA16, RGBA16F,
    RGBA32F, R16F, R32F, BGRE8). Les cubemaps s'affichent avec leurs 6 faces côte à côte.
  - **Oodle** : le décompresseur est chargé automatiquement depuis une installation d'Unreal
    (registre Epic). À défaut, copiez `oo2core_9_win64.dll` dans `plugins\`.
  - **Vignettes** : la miniature enregistrée par l'éditeur.
  - **Infos** : classe, format source, réglage de compression, sRGB, version du moteur.
  - **Textures cookées** (jeux packagés, `Saved/Cooked`) : `.uasset` + `.uexp` + `.ubulk`/`.uptnl`,
    de UE 4.25 à 5.8.
    - La version du moteur est déduite de la structure du fichier.
    - Formats GPU BC1 à BC7 (dont BC6H HDR) décodés par Direct3D (WARP), ainsi que les formats non
      compressés. Les normal maps BC5 sont affichées avec le Z reconstruit.
    - Vignettes : la plus petite mip suffisante, lue directement.
  - **Limites** : textures mobiles (ASTC/ETC), textures virtuelles (VT), données virtualisées hors
    du fichier, packages IoStore (`.utoc`/`.ucas`, à extraire d'abord, avec FModel par exemple).
- **Zoom fluide** centré sur la souris, avec crans sur « ajusté » et 100 %. Rendu pixel-exact à
  100 %, filtrage bicubique haute qualité en réduction et pixels nets au-delà de 300 %.
- **Navigation dans le dossier** (tri naturel comme l'Explorateur), avec une pellicule de
  vignettes en bas de l'écran.
- **Vitesse** :
  - décodage multithread ;
  - préchargement des images voisines dans le sens de lecture ;
  - envoi anticipé au GPU ;
  - vignettes rapides (miniature EXIF, réduction DCT du JPEG) qui servent d'aperçu immédiat pendant
    le décodage ;
  - cache mémoire limité à 20 % de la RAM ;
  - aucune consommation CPU au repos.
- Orientation EXIF, rotation manuelle, plein écran, panneau d'infos EXIF (appareil, objectif,
  exposition, ouverture, ISO, focale, date) et damier sous les images transparentes.
- **Enregistrer sous / convertir** (Ctrl+S) : JPEG (qualité réglable dans la boîte de dialogue),
  PNG, TIFF, BMP, GIF, JPEG XR, ainsi que HEIF si l'extension HEVC est installée.
  - La rotation affichée est appliquée aux pixels.
  - La transparence est conservée si le format la gère, sinon l'image est composée sur fond blanc.
  - L'écriture passe par un fichier temporaire, donc le fichier remplacé ne peut pas être corrompu.
  - Un RAW s'enregistre dans sa version développée.
- **Mode édition** (E), façon Paint, avec les outils suivants :
  - pinceau, gomme (transparente sur les images qui gèrent la transparence) ;
  - ligne, flèche, rectangle, ellipse, en contour ou plein ;
  - texte, pot de peinture, pipette ;
  - recadrage avec grille des tiers ;
  - rotation, miroirs, redimensionnement (pourcentage ou grand côté) ;
  - palette de couleurs + couleur personnalisée, annuler / rétablir.

  Le dessin est fait par le GPU avec antialiasing.
- **Plugins** : des DLL déposées dans `plugins\` ajoutent de nouveaux formats.

## Compiler

Prérequis : Visual Studio 2022 avec la charge de travail « Développement Desktop en C++ ».
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
```

Pour déployer l'application, copiez `CyImgView.exe` et le dossier `plugins`.

Les sources de LibRaw 0.22.2 se trouvent dans `third_party/LibRaw` (double licence LGPL 2.1 /
CDDL 1.0). Elles sont utilisées sous LGPL 2.1, compatible avec la GPL-3.0 du projet.

L'installeur (`setup-stub.exe` + archive) est produit par `tools\package.ps1`.

## Mises à jour (GitHub Releases)

CyImgView vérifie lui-même s'il existe une nouvelle version :
- **Quand** : quelques secondes après le démarrage, au plus une fois toutes les 20 h, ou à la demande
  avec **Ctrl+U**. Une pastille « Mise à jour X.Y.Z » apparaît alors dans la barre du haut.
- **Choix proposés** : installer et redémarrer, voir la release, ignorer cette version.
- **Installation** :
  - téléchargement de l'archive `*-win64.zip` de la release, avec contrôle SHA-256 quand GitHub le
    fournit ;
  - remplacement des fichiers, y compris l'exe en cours d'exécution, puis redémarrage sur la même
    image ;
  - en cas d'échec, tout est remis en état.
- **Désactiver** : `auto=0` dans la section `[update]` de `%APPDATA%\CyImgView\settings.ini`.

**Configuration** : indiquer le dépôt dans `src/version.h` :
`#define CYIMG_UPDATE_REPO L"propriétaire/dépôt"`. Tant que ce champ est vide, la fonction est
désactivée.

**Publier une version :**
1. Augmenter `CYIMG_VERSION_MAJOR/MINOR/PATCH` dans `src/version.h` et committer.
2. `git tag v1.3.0 && git push origin v1.3.0`. Le workflow `.github/workflows/release.yml` compile,
   empaquette et crée la release avec le zip.

Sans GitHub Actions, `tools\package.ps1` produit `dist\CyImgView-<version>-win64.zip`, à joindre à
une release étiquetée `v<version>`.

Oodle (`oo2core_*.dll`) n'est jamais inclus dans l'archive : il appartient à Epic et reste chargé
depuis l'installation d'Unreal de l'utilisateur.

## Utilisation

`CyImgView.exe [fichier|dossier]`. Vous pouvez aussi glisser-déposer un fichier, utiliser Ctrl+O, ou
passer par « Ouvrir avec » / l'association de fichiers.

| Touche | Action |
|---|---|
| ← → / Espace / Retour arrière / Page préc. / Page suiv. | Image précédente / suivante |
| Début / Fin | Première / dernière image |
| Molette, + / -, ↑ / ↓ | Zoom (centré sur la souris pour la molette) |
| Glisser | Déplacer l'image zoomée |
| Double-clic / Z | Basculer entre « ajusté » et taille réelle |
| 0 / 1 | Ajuster / 100 % |
| R / Maj+R | Pivoter |
| F / F11 / Entrée / clic molette | Plein écran |
| T | Pellicule |
| I | Informations |
| H / F1 | Aide |
| F5 | Recharger le dossier |
| Boutons latéraux de la souris | Image précédente / suivante |
| Échap | Fermer le panneau, quitter le plein écran, puis quitter |
| Ctrl+S | Enregistrer sous / convertir |
| Ctrl+U | Rechercher une mise à jour |
| E | Mode édition |

### Mode édition

| Touche | Outil / action |
|---|---|
| B / G | Pinceau / gomme (clic droit : 2e couleur) |
| L / A / R / O | Ligne / flèche / rectangle / ellipse (Maj : 45°, carré, cercle) |
| T | Texte (Entrée : valider, Maj+Entrée : nouvelle ligne, Échap : annuler) |
| F / K | Remplissage / pipette |
| C | Recadrer (Entrée ou double-clic pour valider) |
| M, Espace maintenu, clic molette | Déplacer la vue |
| [ / ] | Taille du trait ou de la police |
| Ctrl+Z / Ctrl+Y | Annuler / rétablir |
| E / Échap | Quitter l'édition (l'enregistrement est proposé s'il y a des modifications) |

Les tailles d'outil sont en pixels de l'image et s'adaptent à sa définition.

Les métadonnées EXIF ne sont pas recopiées dans le fichier enregistré.

Les réglages (position de la fenêtre, pellicule) sont enregistrés dans
`%APPDATA%\CyImgView\settings.ini`.

## Écrire un plugin

L'API C est décrite dans [`include/cyimg_plugin.h`](include/cyimg_plugin.h). Un plugin est une DLL
qui exporte une seule fonction :

```cpp
#include "cyimg_plugin.h"

static int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    // Option 1 : pixels décodés par le plugin
    uint32_t stride;
    uint8_t* px = host->alloc_bgra(ctx, width, height, /*has_alpha*/ 0, &stride);
    // ... remplir px (BGRA 8 bits, alpha prémultiplié) ...

    // Option 2 : confier à l'hôte un flux JPEG/PNG/TIFF embarqué
    // return host->decode_encoded(ctx, data, size);

    host->set_format(ctx, L"Mon format");
    return CYIMG_OK;
}

static const CyImgPlugin plugin = {
    CYIMG_API_VERSION, L"Mon plugin", L"1.0", L"ext1;ext2",
    nullptr,  // probe (optionnel) : reconnaissance par l'en-tête
    Decode,
};

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &plugin; }
```

- `decode` est appelée en parallèle depuis plusieurs threads, donc sans état global modifiable.
- Si `host->target_size(ctx)` est différent de 0, l'hôte demande une vignette : le plugin peut alors
  renvoyer une image plus petite.
- Les plugins passent avant WIC pour les extensions qu'ils déclarent.
- **Affichage progressif** : un plugin lent peut d'abord rendre un aperçu rapide, puis appeler
  `host->set_refinable(ctx)`. L'hôte affiche l'aperçu, puis rappelle `decode` en arrière-plan avec
  `host->quality(ctx) == 1` pour obtenir la version complète. Ces deux fonctions ont été ajoutées
  après la première version de l'API : testez leur présence avec `CYIMG_HOST_HAS(host, quality)`.
- `host->set_error(ctx, message)` : message affiché quand le fichier est refusé (ex. « ce .uasset
  est un Material »).
- **Filtre de dossier** (export optionnel) :
  `extern "C" __declspec(dllexport) int CyImg_Accept(const wchar_t* path)`. Il retourne 0 pour
  masquer un fichier de la liste du dossier ; c'est ainsi que le plugin `uasset` ne garde que les
  textures. Il doit être rapide, car il est appelé pour chaque fichier du dossier.
- Si un plugin rend une image plus grande que la vignette demandée, l'hôte la réduit lui-même.

Pour compiler un nouveau plugin, ajoutez-le dans `CMakeLists.txt` sur le modèle de `raw`.

Pistes de plugins : PSD (image composite), EXR/HDR, JPEG XL.

## Mesurer les performances

```bat
build\Release\bin\cyimg_bench.exe D:\Photos\MonDossier
build\Release\bin\cyimg_bench.exe D:\Photos\MonDossier 160 best
```

Cette commande affiche les temps de décodage en pleine résolution et en vignette, plus les EXIF.
Avec `best`, elle mesure aussi le développement RAW complet, même quand l'aperçu suffit.

Conversion en ligne de commande (le format de sortie est déduit de l'extension) :

```bat
build\Release\bin\cyimg_bench.exe convert photo.CR3 photo.jpg 90
```

Si la variable d'environnement `CYIMG_TRACE=1` est définie, le démarrage de la visionneuse est
chronométré dans `%TEMP%\cyimg_trace.txt`.

## Licence

CyImgView est distribué sous licence **GNU GPL v3.0** : voir [LICENSE](LICENSE).

Composants tiers :
- **LibRaw** (`third_party/LibRaw`) : LGPL 2.1 / CDDL 1.0.
- **Oodle** (RAD Game Tools / Epic Games) : il n'est **pas** distribué. Il est chargé à l'exécution
  depuis une installation d'Unreal Engine de l'utilisateur.
- **Unreal Engine** : le format `.uasset` est lu par du code écrit pour ce projet ; aucun code d'Epic
  n'est inclus.
