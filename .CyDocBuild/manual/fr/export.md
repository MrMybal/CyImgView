---
id: export
title: Enregistrer un projet ou exporter une image
description: Gardez un projet modifiable et choisissez un format pour partager le résultat.
section: Guides
order: 90
sourceRevision: 2
parent: edit
---


## Garder un projet modifiable {#project}

Utilisez **Enregistrer sous → Projet CyImgView (.cyimg)**. Il conserve calques de pixels, objets modifiables, masques, opacité et fusion. Rouvrez-le avec Ctrl+O. Gardez ce fichier de travail avant d’exporter une image aplatie.

## Choisir le format d’export {#export}

| Format | Conserve |
|---|---|
| `.cyimg` | Calques, objets et masques modifiables. |
| PNG | Image aplatie avec transparence. |
| JPEG | Image opaque aplatie, qualité réglable. |
| PSD | Calques pixellisés avec masques appliqués aux pixels. |

## Éviter les surprises {#details}

Appliquez ou annulez l’effet en cours avant d’enregistrer. La rotation affichée est appliquée aux pixels exportés. Les métadonnées EXIF ne sont pas recopiées. Les formats opaques composent la transparence sur du blanc. Lire un format et l’enregistrer sont deux capacités distinctes : l’enregistrement utilise les encodeurs WIC disponibles et l’export HEIF peut nécessiter l’extension HEVC de Windows.

```diagram
{"type": "flow", "title": "Garder le projet, partager un export", "nodes": [{"id": "0", "label": "Enregistrer .cyimg", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Choisir un autre nom", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Partager PNG / JPEG", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```
