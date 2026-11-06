---
id: licenses
title: Licences et crédits
description: Consultez la licence de l’application et les crédits des composants fournis.
section: À propos
order: 240
sourceRevision: 2
---


## Licences et crédits {#licenses}

CyImgView est distribué sous licence **GNU GPL v3.0** : voir `CyImgView-LICENSE.txt` (texte complet fourni dans `Documentation/DocData`) .

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

## CyDoc {#cydoc}

Le lecteur local utilise CyDoc 0.1.0 sous licence MIT. Sa licence est fournie dans `CyDoc-LICENSE.txt`, dans `Documentation/DocData`. Seuls les lecteurs image et galerie sont inclus ; Three.js n’est pas distribué.
