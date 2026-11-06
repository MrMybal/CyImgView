---
id: formats
title: Formats et possibilités
description: Vérifiez les formats reconnus et les limites des aperçus de documents.
section: Guides
order: 100
sourceRevision: 2
parent: viewer
---


## Formats et possibilités {#formats}

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
  à côté de l'exe ; consultez les [réglages de langue](doc:settings#languages) pour les personnaliser.
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
