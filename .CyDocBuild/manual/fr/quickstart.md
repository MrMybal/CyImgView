---
id: quickstart
title: Premiers pas en cinq minutes
description: Ouvrez une photo, faites une première retouche et enregistrez une copie.
section: Guides
order: 10
sourceRevision: 2
parent: overview
---


## Ouvrir votre première image {#open}

1. Appuyez sur **Ctrl+O** et choisissez une image, ou glissez une image sur la fenêtre.
2. Utilisez la molette pour zoomer. **Z** ou un double-clic bascule entre ajustement et taille réelle. Gauche/Droite parcourt le dossier.
3. Appuyez sur **E** pour la Retouche. Ajoutez un objet ou peignez sur un nouveau calque.
4. **Enregistrer sous → Projet CyImgView (.cyimg)** garde les calques et objets modifiables.
5. Exportez un PNG ou JPEG sous un autre nom pour le partager.

## Déplacer la vue sans modifier le contenu {#navigation}

Dans l’éditeur, **maintenez le bouton droit et glissez** pour déplacer la vue. Le glissement gauche utilise l’outil actif : avec **V**, il déplace un objet ou un calque. Ce sont deux actions différentes.

## Partir d’une image vide {#blank}

**Ctrl+N** ouvre les [modèles de documents](doc:new-document#templates). Choisissez une taille et un fond, puis Créer. **Ctrl+Maj+N** ajoute un calque pour peindre.

```diagram
{"type": "flow", "title": "Conserver le projet et partager un export", "nodes": [{"id": "0", "label": "Ouvrir / créer", "x": 0, "y": 20, "width": 215}, {"id": "1", "label": "Retouche → projet .cyimg", "x": 245, "y": 20, "width": 215}, {"id": "2", "label": "Export → PNG / JPEG", "x": 490, "y": 20, "width": 215}], "edges": [{"from": "0", "to": "1"}, {"from": "1", "to": "2"}]}
```
