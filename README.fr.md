[English](README.md) · **Français**

<p align="center">
  <img src="docs/logo.png" alt="Logo CyImgView" width="160">
</p>

<h1 align="center">CyImgView</h1>

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

- **Formats** : tout est intégré, aucun codec Windows supplémentaire n'est nécessaire.

  | Famille | Formats | Plugin |
  |---|---|---|
  | Courants (WIC) | JPEG, PNG, GIF (animé), BMP, TIFF, ICO, JPEG XR | — |
  | Web / modernes | WebP (animé), HEIC / HEIF, AVIF (et leurs séquences animées), JPEG XL (animé), JPEG 2000 (jp2, j2k, jpx…) | `webp`, `heif`, `jxl`, `jpeg2000` |
  | Vectoriels | SVG / SVGZ, PDF et AI (1re page), WMF / EMF, DXF (2D) | `vector` |
  | Fichiers de travail | PSD / PSB (image fusionnée), XCF (GIMP, calques recomposés), KRA / ORA, CLIP, et l'aperçu intégré des INDD, EPS, CDR, Affinity (afphoto, afdesign, afpub) | `psd`, `docs` |
  | Spéciaux | EXR, HDR (Radiance), DDS (BC1 à BC7, HDR, cubemaps), TGA, FITS, DICOM (multi-images), ICNS | `exr`, `classic`, `dds`, `tga`, `dicom` |
  | Anciens | PCX, PICT, IFF / ILBM (HAM, EHB), Netpbm (pbm, pgm, ppm, pam, pfm), XBM / XPM | `classic` |

  Les images HDR (EXR, HDR, PFM, DDS float) sont ramenées à l'écran par un tone mapping simple.
  Pour les fichiers de travail, c'est l'image fusionnée ou l'aperçu enregistré par le logiciel qui est
  affiché (pas de recomposition des calques, sauf XCF).
- **Animations** : GIF, WebP, JPEG XL, séquences HEIF/AVIF et DICOM multi-images sont lus en boucle.
  Espace : lecture / pause ; `,` et `.` : image par image.
- **Couches** (comme Photoshop) : le bouton « RVB » ouvre un panneau avec l'aperçu de chaque canal
  (rouge, vert, bleu, alpha). Un clic sur une ligne affiche ce canal seul en niveaux de gris ; les
  cases à cocher combinent plusieurs canaux (en couleur). Décocher l'alpha montre les couleurs
  cachées sous les zones transparentes. Raccourcis : C, Ctrl+2 à Ctrl+6.
- **Multilingue** : français, English, Deutsch, Español, Italiano, Português, Nederlands, Polski,
  Русский, 日本語, 简体中文, 한국어. Langue de Windows par défaut, changement instantané dans les
  Paramètres. Pour ajouter ou corriger une langue, déposez un fichier `.lang` dans un dossier `lang`
  à côté de l'exe (format décrit dans [`src/i18n.h`](src/i18n.h), modèles dans `src/lang/`).
- **Diaporama** (D) : plein écran, interface masquée, fondu enchaîné entre les images, durée par
  image réglable (1 à 60 s, ↑ / ↓ pendant le diaporama), ordre aléatoire en option.
- **Presse-papier** : Ctrl+C copie l'image affichée (transparence conservée), Ctrl+Maj+C le chemin
  du fichier. Le clic droit ouvre un menu avec les actions courantes.
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
- **Plugins** : des DLL déposées dans `plugins\` ajoutent de nouveaux formats, et des effets, outils,
  modes d'édition et actions « Ouvrir avec » (plugins d'édition). Fournis : effets supplémentaires
  (bruit, seuil, bichromie, aberration chromatique), annotation de captures d'écran (pastille
  numérotée, surligneur, flou de masquage, bulle de texte), mode Noir & blanc, actions Imprimer et
  Ouvrir dans le navigateur.

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
```

`-DCYIMG_TEST_PLUGINS=ON` compile en plus `bin\testplugins\crashtest.dll`, un plugin d'édition
volontairement défaillant pour tester la robustesse de l'hôte (jamais livré : `tools\package.ps1` ne
prend que `bin\plugins`). `python tools\check_lang.py` vérifie les fichiers de langue (clés,
spécificateurs printf, listes de choix).

Chaque plugin a son dossier `plugins\<nom>\` avec un `plugin.cmake`, inclus automatiquement. Pour
ne compiler que certains plugins : `-DCYIMG_ONLY_PLUGINS="webp;psd"`.

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
| Ctrl+C / Ctrl+Maj+C | Copier l'image / le chemin du fichier |
| Clic droit | Menu (copier, enregistrer, couches, diaporama…) |
| C / Ctrl+2 à Ctrl+6 | Couches : RVB, rouge, vert, bleu, alpha |
| Espace / , / . | Animation : lecture-pause / image précédente / suivante |
| N | Pixels bruts (sans lissage) / filtrage choisi |
| D | Diaporama (↑ ↓ : durée par image, Espace : pause, Échap : quitter) |
| Ctrl+S | Enregistrer sous / convertir |
| Ctrl+U | Rechercher une mise à jour |
| P | Paramètres |
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

### Paramètres (touche P ou bouton engrenage)

- **Ouvrir les fichiers dans la fenêtre existante** (désactivé par défaut) : une image ouverte depuis
  l'Explorateur remplace celle de la fenêtre déjà ouverte au lieu d'en créer une nouvelle. Ça
  fonctionne aussi quand plusieurs fichiers sont ouverts d'un coup.
- **Afficher la pellicule.**
- **Rechercher les mises à jour automatiquement**, plus un bouton pour rechercher tout de suite.
- **Langue de l'interface** : Automatique (langue de Windows) ou l'une des 12 langues.
- **Diaporama** : ordre aléatoire, durée par image.
- **Filtrage de l'image** : Automatique (pixel exact à 100 %, lissé en réduction, net au-delà de
  300 %), Pixels bruts (aucun lissage, pour le pixel art), Linéaire, Cubique, Cubique HQ, Anisotrope.
  Aussi dans le menu du clic droit ; la touche N bascule en pixels bruts.

Les réglages, ainsi que la position de la fenêtre, sont enregistrés dans
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

- **Animation** : après `alloc_bgra` (image 0), `host->add_frame()` ajoute chaque image suivante
  déjà composée et `host->set_frame_delay()` fixe sa durée.
- **Traduction** : `host->translate()` (aide : [`include/cyimg_plugin_i18n.h`](include/cyimg_plugin_i18n.h))
  cherche un texte dans les fichiers de langue de CyImgView, avec un texte par défaut si la clé
  est inconnue.
- **Robustesse** : un plugin qui plante sur un fichier ne ferme pas la visionneuse ; le fichier
  affiche un message d'erreur.

Pour compiler un nouveau plugin, créez `plugins/<nom>/plugin.cmake` sur le modèle de `plugins/tga`
(`cyimg_add_plugin(<nom> sources...)`) : il est inclus automatiquement.

## Écrire un plugin d'édition

L'API C est décrite dans [`include/cyimg_edit_plugin.h`](include/cyimg_edit_plugin.h). Un plugin
d'édition est une DLL du même dossier `plugins\` qui exporte `CyImg_GetEditPlugin` (une DLL peut
exporter aussi `CyImg_GetPlugin` et décoder des formats). Il déclare des tableaux de quatre sortes
d'extensions ; l'hôte s'occupe de l'interface, de l'aperçu, de la sélection, de l'annulation et des
traductions.

**Effet** (menu Effets de la Retouche, même panneau de réglages et même aperçu que les effets
intégrés) :

```cpp
#include "cyimg_edit_plugin.h"

static const CyImgParam kParams[] = {
    CYIMG_SLIDER("plugin.monplugin.level", L"Niveau", 1, 254, 128, 1, nullptr),
    CYIMG_CHECK("plugin.monplugin.invert", L"Inverser", 0),
};

// Thread de travail : modifier img->data dans img->sel (BGRA 8 bits prémultiplié)
static int Process(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* v) {
    for (uint32_t y = 0; y < img->sel.h; ++y) {
        if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
        uint8_t* p = img->data + size_t(img->sel.y + y) * img->stride + size_t(img->sel.x) * 4;
        for (uint32_t x = 0; x < img->sel.w; ++x, p += 4) {
            const bool white = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2] >= v[0] * p[3] / 255;
            p[0] = p[1] = p[2] = (white != (v[1] != 0)) ? p[3] : 0;  // blanc prémultiplié = alpha
        }
    }
    return CYIMG_OK;
}

static const CyImgEffect kEffect = {sizeof(CyImgEffect), "threshold", "plugin.monplugin.threshold", L"Seuil",
                                    nullptr, nullptr, kParams, 2, Process};
static const CyImgEffect* const kEffects[] = {&kEffect};
static const CyImgEditPlugin kPlugin = {CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "monplugin", nullptr,
                                        L"Mon plugin", L"1.0", kEffects, 1, nullptr, 0, nullptr, 0, nullptr, 0};

extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) { return &kPlugin; }
```

Pendant les réglages, `process` reçoit une copie réduite à la taille de l'écran (`img->scale` < 1 :
multiplier les distances en pixels par `scale`) ; peu après le dernier réglage, puis à « Appliquer »,
la pleine résolution. `img->source` donne les pixels d'origine (effets qui lisent les voisins).

**Outil** (colonne d'outils, après les outils intégrés ; paramètres dans la barre d'options) :

```cpp
static int Down(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent* ev) {
    const double r = h->param(c, 0) / 2;  // paramètre 0 : taille
    h->draw_ellipse(c, ev->x, ev->y, r, r, 0, 0xFF000000u | h->primary_color(c));  // sur le calque actif
    return 0;  // 1 : suivre le glissement (mouse_move puis mouse_up)
}
static const CyImgParam kToolParams[] = {CYIMG_SLIDER("plugin.monplugin.size", L"Taille", 4, 200, 30, 1, L"px")};
static const CyImgTool kTool = {sizeof(CyImgTool), "dot", "plugin.monplugin.dot", L"Point", nullptr, nullptr,
                                0xE7C8 /* icône Segoe MDL2 */, nullptr, 0, 'J', kToolParams, 1,
                                Down, nullptr, nullptr, nullptr, nullptr, nullptr};
```

Les primitives de l'hôte (`draw_line`, `draw_polyline`, `draw_rect`, `draw_ellipse`, `fill_polygon`,
`draw_text`, `measure_text`, `begin_group` / `end_group` avec opacité et mode produit) dessinent sur
le calque actif avec Direct2D, limitées à la sélection ; appelées dans `draw_overlay`, elles dessinent
un aperçu à l'écran sans rien modifier. `lock_pixels` / `unlock_pixels` donnent accès aux pixels d'un
rectangle (flou, pixellisation...). Chaque geste fait une entrée d'annulation (`begin_edit`, appelée
automatiquement au premier dessin). `input_text` ouvre une petite boîte de saisie, `set_param` met à
jour un paramètre affiché (compteur), `screen_pixel` donne la taille d'un pixel de l'écran.

**Mode d'édition** (onglet à côté de Retouche et Développement) : `controls` décrit le panneau de
droite (sections repliables `CYIMG_SECTION`, curseurs, cases, listes, couleurs, boutons
`CYIMG_BUTTON`, textes, séparateurs ; `visible_if` affiche un contrôle selon la valeur d'un autre) et
`process` calcule le résultat à partir de `img->source`. Le mode est non destructif comme le
Développement : aperçu réduit pendant les réglages, rendu complet à l'arrêt, résultat intégré au
calque (une entrée d'historique) au retour en Retouche, à « Terminer » ou à l'enregistrement.
`on_button` reçoit les valeurs et peut les modifier (préréglages).

**Action « Ouvrir avec »** :

```cpp
static int Run(const CyImgEditHost*, CyImgEditCtx*, const wchar_t* path, void* hwnd) {
    return INT_PTR(ShellExecuteW(HWND(hwnd), L"print", path, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
static const CyImgAction kPrint = {sizeof(CyImgAction), "print", "plugin.monplugin.print", L"Imprimer…",
                                   nullptr, 0xE749, nullptr /* toutes les extensions */, Run, nullptr, nullptr};
// Ou un programme externe, sans code : program = L"%ProgramFiles%\\Outil\\outil.exe", args = L"\"%1\""
```

Les actions apparaissent dans le groupe des plugins du menu « Ouvrir avec » : en visualisation avec
le fichier affiché, en édition avec un PNG temporaire de l'image modifiée.

Règles :

- **Threads** : `process` tourne sur des threads de travail, parfois deux à la fois (aperçu et rendu
  complet) : pas d'état global modifiable, seulement `translate` et `is_cancelled` de l'hôte, à tester
  régulièrement. Les autres rappels (outils, boutons, actions) sont sur le thread de l'interface, un à
  la fois.
- **Pixels** : BGRA 8 bits à alpha prémultiplié (couleur ≤ alpha).
- **Protection** : chaque appel vers le plugin est protégé ; une fonction qui plante est désactivée
  pour la session avec un message, sans fermer l'application ni perdre le document.
- **Traduction** : textes donnés par une clé (`plugin.<nom>.<texte>`) et un texte par défaut ; les
  listes de choix se traduisent par la clé suivie de `.choices` (`Un|Deux|Trois`). Les plugins livrés
  ont leurs textes dans `src/lang/editplugins.<code>.lang`.
- **Versions** : chaque structure commence par `struct_size` ; `CYIMG_EDIT_HOST_HAS(host, champ)`
  teste la présence d'une fonction de l'hôte ajoutée plus tard.
- **Chargement** à la première entrée en mode édition, à la première ouverture du menu « Ouvrir
  avec » ou de l'aide (aucun effet sur le démarrage) ; la liste est dans l'aide (H), section
  « Plugins d'édition ».

Les plugins livrés servent de modèles commentés : [`plugins/fx_extra`](plugins/fx_extra) (effets),
[`plugins/annotate`](plugins/annotate) (outils), [`plugins/bw_mode`](plugins/bw_mode) (mode),
[`plugins/share_actions`](plugins/share_actions) (actions). Ils se compilent comme les autres, avec un
`plugin.cmake` (`cyimg_add_plugin(<nom> sources...)`).

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

## Confidentialité

- **Aucune télémétrie**, aucun compte, aucune publicité.
- **Seule connexion réseau** : la recherche de mise à jour, qui interroge l'API publique de GitHub
  (`api.github.com/repos/MrMybal/CyImgView/releases/latest`) au plus une fois toutes les 20 h, puis
  télécharge l'archive de la release si vous acceptez la mise à jour. Aucune donnée personnelle
  n'est envoyée (en-tête `User-Agent: CyImgView/<version>`). Désactivable dans les Paramètres.
- **Fichiers locaux** : réglages dans `%APPDATA%\CyImgView\settings.ini` (et `develop_last.ini`
  pour les réglages du mode Développement), journal de plantage éventuel dans
  `%LOCALAPPDATA%\CyImgView\crash.txt` (jamais envoyé), exports temporaires de « Ouvrir avec »
  dans `%TEMP%\CyImgView` (supprimés après 24 h).
- **DICOM** : les données nominatives du patient (nom, identifiant, date de naissance) ne sont
  jamais affichées.
- « Ouvrir avec » ne lance que les applications que vous avez choisies ou que Windows associe au
  format.

## Licence

CyImgView est distribué sous licence **GNU GPL v3.0** : voir [LICENSE](LICENSE).

Composants tiers :
- **LibRaw** (`third_party/LibRaw`) : LGPL 2.1 / CDDL 1.0.
- **libwebp** : BSD 3 clauses (+ concession de brevets Google).
- **libheif**, **libde265** : LGPL 3.0. **dav1d** : BSD 2 clauses.
- **libjxl** : BSD 3 clauses (+ brevets). **highway** : Apache 2.0.
- **OpenJPEG** : BSD 2 clauses.
- **OpenEXR** (OpenEXRCore) et **Imath** : BSD 3 clauses. **libdeflate** : MIT. **zstd** : BSD.
  **OpenJPH** : BSD 2 clauses.

Les sources de ces bibliothèques, avec leurs licences, sont dans `third_party/`. Les autres formats
(PSD, XCF, DDS, DICOM, FITS, PCX…) sont lus par du code écrit pour ce projet.
- **Oodle** (RAD Game Tools / Epic Games) : il n'est **pas** distribué. Il est chargé à l'exécution
  depuis une installation d'Unreal Engine de l'utilisateur.
- **Unreal Engine** : le format `.uasset` est lu par du code écrit pour ce projet ; aucun code d'Epic
  n'est inclus.
