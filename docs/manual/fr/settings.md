---
id: settings
title: Paramètres et langues
section: Références
order: 115
sourceRevision: 1
---

## Paramètres {#settings}

- **Ouvrir les fichiers dans la fenêtre existante** (désactivé par défaut) : une image ouverte depuis
  l'Explorateur remplace celle de la fenêtre déjà ouverte au lieu d'en créer une nouvelle. Ça
  fonctionne aussi quand plusieurs fichiers sont ouverts d'un coup.
- **Afficher la pellicule.**
- **Rechercher les mises à jour automatiquement**, plus un bouton pour rechercher tout de suite.
- **Langue de l'interface** : Automatique (langue de Windows) ou l'une des 12 langues.
- **Diaporama** : ordre aléatoire, durée par image.
- **Filtrage de l'image** : Automatique (pixel exact à 100 %, lissé en réduction, net au-delà de
  300 %), Pixels bruts (aucun lissage, pour le pixel art), Linéaire, Cubique, Cubique HQ, Anisotrope.
  Aussi dans le menu du clic droit ; la touche N bascule en pixels bruts.

Les réglages, ainsi que la position de la fenêtre, sont enregistrés dans
`%APPDATA%\CyImgView\settings.ini`.

## Langue du manuel {#languages}

F1 ouvre le manuel dans la langue de l’application. Les guides détaillés sont disponibles en français et en anglais. L’aide d’origine sur les raccourcis est conservée dans les douze langues de l’interface. Les autres pages se replient sur l’anglais. Le sélecteur de langue conserve la page et sa section. Les commandes du lecteur CyDoc sont actuellement traduites en français et en anglais.

## Ajouter ou corriger une traduction de l’application {#translation-files}

Déposez un fichier UTF-8 `.lang` dans un dossier `lang` à côté de l’exécutable,
puis redémarrez. Il complète les textes intégrés et peut ajouter une langue.
Utilisez les clés existantes ; exemple de correction :

```text
# custom.fr.lang
@code = fr
@name = Français
viewer.btn.help = Aide
```

Les valeurs reconnaissent `\n`, `\t` et `\\`. Gardez les spécificateurs printf
dans le même ordre que la source et les listes de choix séparées par `|`.
Une clé manquante utilise la langue choisie, puis l’anglais, puis le français,
puis son identifiant. Ce repli de l’interface est distinct de celui du manuel.
Les contributeurs vérifient les fichiers de `src/lang` avec
`python tools/check_lang.py`.
