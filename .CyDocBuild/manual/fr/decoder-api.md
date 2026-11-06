---
id: decoder-api
title: API des plugins de décodage
description: Renvoyez des pixels d’image avec l’interface publique C de décodage.
section: Guides
order: 180
sourceRevision: 2
parent: plugins
---


## API des plugins de décodage {#decoder-api}

L'API C est décrite dans `include/cyimg_plugin.h`. Un plugin est une DLL
qui exporte une seule fonction :

```cpp
#include "cyimg_plugin.h"

static int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    // Option 1 : pixels décodés par le plugin
    uint32_t stride;
    uint8_t* px = host->alloc_bgra(ctx, width, height, /*has_alpha*/ 0, &stride);
    // ... remplir px (BGRA 8 bits, alpha prémultiplié) ...

    // Option 2 : confier à l'hôte un flux JPEG/PNG/TIFF embarqué
    // return host->decode_encoded(ctx, data, size);

    host->set_format(ctx, L"Mon format");
    return CYIMG_OK;
}

static const CyImgPlugin plugin = {
    CYIMG_API_VERSION, L"Mon plugin", L"1.0", L"ext1;ext2",
    nullptr,  // probe (optionnel) : reconnaissance par l'en-tête
    Decode,
};

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &plugin; }
```

- `decode` est appelée en parallèle depuis plusieurs threads, donc sans état global modifiable.
- Si `host->target_size(ctx)` est différent de 0, l'hôte demande une vignette : le plugin peut alors
  renvoyer une image plus petite.
- Les plugins passent avant WIC pour les extensions qu'ils déclarent.
- **Affichage progressif** : un plugin lent peut d'abord rendre un aperçu rapide, puis appeler
  `host->set_refinable(ctx)`. L'hôte affiche l'aperçu, puis rappelle `decode` en arrière-plan avec
  `host->quality(ctx) == 1` pour obtenir la version complète. Ces deux fonctions ont été ajoutées
  après la première version de l'API : testez leur présence avec `CYIMG_HOST_HAS(host, quality)`.
- `host->set_error(ctx, message)` : message affiché quand le fichier est refusé (ex. « ce .uasset
  est un Material »).
- **Filtre de dossier** (export optionnel) :
  `extern "C" __declspec(dllexport) int CyImg_Accept(const wchar_t* path)`. Il retourne 0 pour
  masquer un fichier de la liste du dossier ; c'est ainsi que le plugin `uasset` ne garde que les
  textures. Il doit être rapide, car il est appelé pour chaque fichier du dossier.
- Si un plugin rend une image plus grande que la vignette demandée, l'hôte la réduit lui-même.

- **Animation** : après `alloc_bgra` (image 0), `host->add_frame()` ajoute chaque image suivante
  déjà composée et `host->set_frame_delay()` fixe sa durée.
- **Traduction** : `host->translate()` (aide : `include/cyimg_plugin_i18n.h`)
  cherche un texte dans les fichiers de langue de CyImgView, avec un texte par défaut si la clé
  est inconnue.
- **Robustesse** : un plugin qui plante sur un fichier ne ferme pas la visionneuse ; le fichier
  affiche un message d'erreur.


## Fonctions publiques {#functions}

[Allouer les pixels](doc:api-cyimghost-alloc-bgra-alloc-bgra), [décoder un flux](doc:api-cyimghost-decode-encoded-decode-encoded) et [rendre une image](doc:api-cyimgplugin-decode-decode). Les signatures et paramètres sont décrits séparément.
