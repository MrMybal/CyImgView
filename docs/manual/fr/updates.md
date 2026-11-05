---
id: updates
title: Mises à jour et versions
description: Mises à jour et versions — CyImgView.
section: Développeurs
order: 220
sourceRevision: 1
---

## Mises à jour et versions {#updates}

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
2. `git tag v1.10.0 && git push origin v1.10.0`. Le workflow `.github/workflows/release.yml` compile,
   empaquette et crée la release avec le zip.

Sans GitHub Actions, `tools\package.ps1` produit `dist\CyImgView-<version>-win64.zip`, à joindre à
une release étiquetée `v<version>`.

Oodle (`oo2core_*.dll`) n'est jamais inclus dans l'archive : il appartient à Epic et reste chargé
depuis l'installation d'Unreal de l'utilisateur.
