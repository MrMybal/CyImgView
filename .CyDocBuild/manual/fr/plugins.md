---
id: plugins
title: Installer et créer des extensions
description: Installez des extensions locales de formats, d’outils, d’effets et d’actions.
section: Extensions
order: 170
sourceRevision: 2
---


## Familles de plugins {#overview}

Les plugins sont des DLL dans le dossier `plugins`. Les plugins de décodage ouvrent des formats ; ceux d’édition ajoutent effets, outils, modes et actions Ouvrir avec. Les HLSL ajoutent des effets d’image. Redémarrez après un ajout. Les DLL exécutent du code dans l’application : utilisez des extensions de confiance.

## Installer une extension {#install}

1. **Ajouter des outils / effets**, dans la galerie, ouvre le dossier des plugins à côté de l’exécutable.
2. Copiez les DLL dans `plugins`, les shaders dans `plugins/effects` et gardez les fichiers requis par l’extension.
3. Redémarrez et vérifiez la liste des outils, modes, effets ou formats concernés.

## Créer une extension {#api}

- [ABI de décodage et exemple](doc:decoder-api#decoder-api).
- [ABI d’édition, effets, outils et modes](doc:editor-api#editor-api).
- [Contrat des shaders HLSL](doc:hlsl#contract).

## Modes et futurs outils IA {#modes}

L’API C existante peut accueillir de futurs outils d’amélioration. Aucun modèle IA, service distant ni magasin automatique de plugins n’est fourni. La référence API présente les signatures publiques et des exemples courts, sans publier les fichiers d’implémentation.

```diagram
{"type": "flow", "title": "Points d’extension locaux", "nodes": [{"id": "0", "label": "CyImgView", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Extensions DLL / HLSL", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Images, outils, effets", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```

## Actions Ouvrir avec {#actions}

Le bouton **Ouvrir avec** permet de lancer les applications associées et les actions ajoutées par des plugins, dont Imprimer et Ouvrir dans le navigateur. En édition, l’action reçoit une copie PNG temporaire du résultat visible.
