---
id: performance
title: Mesurer les performances
description: Mesurer les performances — CyImgView.
section: Développeurs
order: 230
sourceRevision: 1
---

## Mesurer les performances {#performance}

```bat
build\Release\bin\cyimg_bench.exe D:\Photos\MonDossier
build\Release\bin\cyimg_bench.exe D:\Photos\MonDossier 160 best
```

Cette commande affiche les temps de décodage en pleine résolution et en vignette, plus les EXIF.
Avec `best`, elle mesure aussi le développement RAW complet, même quand l'aperçu suffit.

Conversion en ligne de commande (le format de sortie est déduit de l'extension) :

```bat
build\Release\bin\cyimg_bench.exe convert photo.CR3 photo.jpg 90
```

Si la variable d'environnement `CYIMG_TRACE=1` est définie, le démarrage de la visionneuse est
chronométré dans `%TEMP%\cyimg_trace.txt`.
