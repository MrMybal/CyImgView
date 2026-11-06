---
id: privacy
title: Confidentialité et fichiers locaux
description: Comprenez les réglages locaux, les fichiers temporaires et les connexions de mise à jour.
section: Guides
order: 120
sourceRevision: 2
parent: settings
---


## Confidentialité et fichiers locaux {#privacy}

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

## Manuel local {#manual}

Le lecteur ne contacte aucun service. Les liens externes ne s’ouvrent que si vous les suivez. Le navigateur peut enregistrer localement le thème, la langue, les signets, les notes et les annotations personnelles ; ces données restent dans son stockage local. Elles peuvent être indisponibles ou perdues si le stockage est bloqué ou si le manuel change de chemin.
