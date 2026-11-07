---
id: installation
title: Installer CyImgView
section: Guides
order: 5
sourceRevision: 3
description: Installez l’application, gardez une copie portable ou désinstallez-la.
parent: overview
---


## Installation {#installation}

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


Gardez le dossier `Documentation` complet à côté de l’exécutable pour l’aide hors ligne.

## Reconnaître les projets CyImg {#documents}

Les projets modifiables `.cyimg` utilisent une icône de document avec un coin replié, des calques et la mention CYIMG. Le logo rond de CyImgView identifie l’application elle-même.

L’installeur enregistre ce format natif. Si une autre application est déjà définie par défaut, choisissez CyImgView pour `.cyimg` dans **Paramètres > Applications > Applications par défaut** lorsque vous souhaitez la remplacer. Windows conserve votre choix explicite d’application. Les copies portables ne s’enregistrent pas automatiquement.
