---
id: hlsl
title: Créer un effet HLSL
description: Écrivez un effet de pixels avec le point d’entrée HLSL et les ressources de l’hôte.
section: Guides
order: 200
sourceRevision: 2
parent: plugins
---


## Contrat des shaders {#contract}


Placer les fichiers `.hlsl` dans `plugins/effects/` à côté de l'exécutable,
puis redémarrer CyImgView. La galerie charge jusqu'à 64 fichiers, par ordre
alphabétique. Chaque fichier devient un effet avec un curseur d'intensité.
Un fichier qui ne compile pas est ignoré ; ses diagnostics sont écrits dans
`nom.hlsl.errors.txt`. Les autres effets restent disponibles.

```hlsl
float4 CyEffect(float4 color, float2 uv) {
    return float4(1.0 - color.rgb, color.a);
}
```

`color` et le résultat sont RGBA non prémultipliés entre 0 et 1. `uv` désigne
le centre du pixel en coordonnées normalisées. L'hôte prend en charge
l'intensité, la conversion BGRA prémultipliée, la sélection, l'aperçu réduit,
le rendu complet et l'annulation. Le shader peut lire les pixels voisins :

```hlsl
float4 CyEffect(float4 color, float2 uv) {
    float2 pixel = float2(1.0 / Width, 1.0 / Height);
    return (Source.SampleLevel(LinearClamp, uv - pixel, 0) + color
          + Source.SampleLevel(LinearClamp, uv + pixel, 0)) / 3.0;
}
```

Ressources disponibles : `Texture2D<float4> Source`, `SamplerState LinearClamp`,
`uint Width`, `uint Height`, `float Amount` (0–1), `float Scale` (échelle de
l'aperçu). Les distances exprimées en pixels du document doivent être multipliées
par `Scale`. Ne pas déclarer de point d'entrée `main`, ni les ressources de
l'hôte. Les includes ne sont pas pris en charge. Compilation en `cs_5_0` ;
exécution Direct3D 11 avec repli logiciel WARP. L'annulation est vérifiée avant
et après l'exécution GPU, sans interrompre un dispatch déjà lancé.

Les extensions natives d'outils, d'effets, de modes ou d'amélioration IA utilisent
`include/cyimg_edit_plugin.h` et l'export `CyImg_GetEditPlugin`. Une DLL se dépose
dans `plugins/`. L'API existante conserve sa version et sa compatibilité. Aucun
modèle ni service IA n'est inclus ; les futurs plugins pourront proposer leurs
paramètres, leur traitement annulable et leurs outils via cette API.
