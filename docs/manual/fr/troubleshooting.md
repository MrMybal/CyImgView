---
id: troubleshooting
title: Dépannage
description: Dépannage — CyImgView.
section: Guides
order: 125
sourceRevision: 1
---

## Trouver une solution {#problems}

| Problème | À essayer |
|---|---|
| Le glissement déplace un calque | Le bouton **droit** déplace la vue ; V et le glissement gauche déplacent le contenu. |
| L’image reste en noir et blanc | [Annuler le mode ou son application](doc:black-white#cancel). |
| Impossible de peindre un texte ou une forme | Créez un calque de pixels ou convertissez l’objet dans le menu de son calque. |
| La peinture semble invisible | Vérifiez calque actif, œil, opacité, édition du masque et sélection. |
| Un shader manque | Consultez son `.hlsl.errors.txt`, corrigez le contrat et redémarrez. |
| Une image ne s’ouvre pas | Vérifiez [formats et limites](doc:formats#formats), puis essayez un export original. |
| Créer est désactivé | Vérifiez dimensions et nombre total de pixels. |
| Les objets modifiables ont disparu après export | Rouvrez le `.cyimg` ; PNG/JPEG sont aplatis. |

## Aide locale manquante {#help}

Gardez le dossier **help** complet à côté de `CyImgView.exe`. Copiez toute la distribution portable, pas seulement l’exécutable ou `index.html`. Réinstallez ou extrayez la distribution complète si F1 indique une aide manquante. La lecture demande un navigateur moderne et JavaScript, mais aucun serveur ni connexion Internet.

## Notes et positions de lecture {#storage}

Certains navigateurs restreignent le stockage des URL locales ou fenêtres privées. CyDoc signale l’indisponibilité de la persistance ; exportez vos notes en JSON. Gardez un chemin d’aide stable. Les positions sont conservées pendant une session et les liens précis vers une section ou un média restent prioritaires.
