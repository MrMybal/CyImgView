---
id: effects
title: Appliquer des effets
description: Appliquer des effets — CyImgView.
section: Guides
order: 60
sourceRevision: 1
---

## Choisir un effet {#gallery}

Ouvrez **Effets** en Retouche. La galerie utilise une même image de test ; l’original est dans le coin supérieur. Ces aperçus sont des exemples ; l’effet s’ouvre avec ses propres réglages par défaut. Les flèches ou la molette changent de page.

## Choisir la cible du traitement {#target}

Choisissez **Calque actif** ou **Image entière** avant l’effet. Un effet sur toute l’image crée un calque de résultat et garde les calques d’origine masqués, récupérables par Annuler ou dans Calques. Une sélection limite la zone traitée.

## Appliquer ou annuler {#apply}

Réglez les curseurs et utilisez Aperçu pour comparer. **Appliquer** ou Entrée valide l’effet. **Annuler** ou Échap restaure le document précédent. Ctrl+Z annule un effet appliqué. Terminez le réglage avant d’enregistrer, de copier ou de changer d’outil : certaines commandes abandonnent un aperçu non validé.

## Ajouter des effets {#extensions}

**Ajouter des outils / effets** ouvre le dossier des plugins. Les DLL ajoutent effets et outils ; les fichiers HLSL vont dans `plugins/effects/`. Redémarrez pour découvrir les nouveaux fichiers. [Guide HLSL](doc:hlsl#contract) · [Plugins](doc:plugins#overview). Aucun traitement IA n’est fourni.

```image
{"id": "effect-gallery", "src": "assets/screenshots/effects-fr.png", "alt": "Galerie d’effets", "title": "Galerie d’effets", "controls": "minimal"}
```

```image
{"id": "effect-adjustments", "src": "assets/screenshots/effect-preview-fr.png", "alt": "Réglage d’un vignettage", "title": "Réglage d’un vignettage", "controls": "minimal"}
```

```diagram
{"type": "flow", "title": "Prévisualiser avant de valider", "nodes": [{"id": "0", "label": "Choisir la cible", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Régler + prévisualiser", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Appliquer ou annuler", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```
