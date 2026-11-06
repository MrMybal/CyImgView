---
id: layers
title: Calques, fusion et masques
description: Combinez les calques avec leur opacité, leurs modes de fusion et leurs masques.
section: Guides
order: 50
sourceRevision: 2
parent: edit
---


## Comprendre la pile de calques {#layers}

Les calques sont des feuilles transparentes sur le fond. Le calque visible supérieur se dessine au-dessus des autres. Sélectionnez un calque avant de peindre, de changer son opacité ou de lui appliquer un effet. L’œil règle la visibilité ; l’opacité et le mode de fusion règlent la combinaison avec les autres calques.

## Utiliser le menu des calques {#menu}

Le clic droit dans **Calques** permet de créer, dupliquer, renommer, réordonner, supprimer ou fusionner. **Ctrl+J** duplique, **Ctrl+E** fusionne vers le bas et **Ctrl+Maj+E** aplatit. Ces opérations sur le document sont annulables.

## Masquer des pixels avec un masque {#masks}

1. Faites un clic droit sur le calque, puis **Ajouter un masque**. Il révèle la sélection, ou tout le calque si aucune sélection n’existe.
2. Activez **Peindre le masque**. Le noir cache, le blanc révèle et le gris révèle partiellement. **X** inverse les couleurs de peinture.
3. Désactivez Peindre le masque pour revenir aux pixels de l’image. Le menu peut désactiver, inverser ou supprimer le masque.

Un masque change la visibilité sans supprimer les pixels cachés. Enregistrez en `.cyimg` pour le garder modifiable. Un PNG/JPEG exporte le résultat visible.

## Choisir un mode de fusion {#blending}

Normal dessine le calque selon son opacité. Produit assombrit ; Écran éclaircit. Incrustation, Obscurcir, Éclaircir, Différence et Addition sont également disponibles. Essayez un mode, comparez et annulez si nécessaire. [Projets et exports](doc:export#project).

```diagram
{"type": "flow", "title": "Composer des calques séparés", "nodes": [{"id": "0", "label": "Photo de fond", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Calques cadre + texte", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Résultat visible", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```

```diagram
{"type": "flow", "title": "Le masque règle la visibilité", "nodes": [{"id": "0", "label": "Noir → caché", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Gris → partiel", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Blanc → visible", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```
