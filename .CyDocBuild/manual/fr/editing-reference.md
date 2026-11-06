---
id: editing-reference
title: Référence des documents et calques
section: Guides
order: 112
sourceRevision: 2
description: Retrouvez les règles des documents, objets, calques, masques et effets.
parent: edit
---


## Référence {#reference}

- **Ctrl+N** ou le bouton Nouvelle image : choisir parmi 18 modèles ou saisir des dimensions personnalisées,
  puis choisir un fond transparent, blanc ou noir. Le document s'ouvre directement en Retouche.
- Les lignes, flèches, rectangles, ellipses et textes sont des **objets** sur des
  calques séparés. Avec **V**, cliquer/glisser un objet pour le déplacer, ou
  sélectionner son calque puis le déplacer. Clic droit sur le calque pour
  modifier son texte et reprendre la couleur, la taille ou le remplissage des
  options de l'outil. **Convertir l'objet en pixels** permet ensuite de peindre
  directement dessus. Ces opérations sont annulables avec Ctrl+Z.
- Clic droit dans **Calques** : nouveau calque, duplication, renommage, fusion,
  modes de fusion et masques. Un masque part de la sélection ou révèle tout.
  En mode **Peindre le masque**, noir cache, blanc révèle et gris rend
  partiellement transparent. Le menu permet aussi de désactiver, inverser ou
  supprimer le masque. Désactiver « Peindre le masque » pour revenir aux pixels.
- Les modes de plugins, dont Noir et blanc, proposent **Annuler le mode** et
  **Appliquer**. Échap abandonne le mode ; après application, Ctrl+Z en Retouche
  restaure le résultat précédent. « Original » reste une comparaison temporaire.
- La **galerie d'effets** affiche des aperçus sur une image de test commune pour les effets intégrés et les plugins (dont HLSL), avec pagination. Choisir **Calque actif** ou **Image entière**
  avant l'effet. Un effet sur l'image entière crée un calque de résultat et
  conserve les sources masquées, récupérables par annulation ou dans Calques.
- **Enregistrer sous > Projet CyImgView (.cyimg)** conserve les objets, les
  masques et la pile de calques. Le rouvrir avec Ctrl+O. PNG/JPEG exportent une
  image aplatie ; PSD conserve des calques pixellisés avec le masque appliqué.
- **Ajouter des outils / effets** ouvre le dossier des plugins. Les DLL utilisent
  l'API `include/cyimg_edit_plugin.h`. Les effets HLSL se déposent dans
  `plugins/effects/` : voir [leur contrat et exemple](doc:hlsl).
  L'emplacement des futures extensions IA est prêt, sans moteur IA intégré.


La fenêtre **Nouvelle image** propose 18 modèles (écran, photo, impression), des dimensions personnalisées en pixels, une inversion largeur/hauteur et un fond transparent, blanc ou noir. Un aperçu affiche les proportions avant la création.

En édition, maintenez le bouton droit sur l’image pour déplacer la vue sans modifier le document, quel que soit l’outil. Le clic droit sur les calques conserve son menu contextuel.
