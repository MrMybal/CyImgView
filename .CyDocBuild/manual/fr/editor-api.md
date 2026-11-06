---
id: editor-api
title: API des plugins d’édition
description: Ajoutez un effet, un outil, un mode ou une action avec l’ABI publique d’édition.
section: Guides
order: 190
sourceRevision: 2
parent: plugins
---


## API des plugins d’édition {#editor-api}

L'API C est décrite dans `include/cyimg_edit_plugin.h`. Un plugin
d'édition est une DLL du même dossier `plugins\` qui exporte `CyImg_GetEditPlugin` (une DLL peut
exporter aussi `CyImg_GetPlugin` et décoder des formats). Il déclare des tableaux de quatre sortes
d'extensions ; l'hôte s'occupe de l'interface, de l'aperçu, de la sélection, de l'annulation et des
traductions.

**Effet** (menu Effets de la Retouche, même panneau de réglages et même aperçu que les effets
intégrés) :

```cpp
#include "cyimg_edit_plugin.h"

static const CyImgParam kParams[] = {
    CYIMG_SLIDER("plugin.monplugin.level", L"Niveau", 1, 254, 128, 1, nullptr),
    CYIMG_CHECK("plugin.monplugin.invert", L"Inverser", 0),
};

// Thread de travail : modifier img->data dans img->sel (BGRA 8 bits prémultiplié)
static int Process(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* v) {
    for (uint32_t y = 0; y < img->sel.h; ++y) {
        if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
        uint8_t* p = img->data + size_t(img->sel.y + y) * img->stride + size_t(img->sel.x) * 4;
        for (uint32_t x = 0; x < img->sel.w; ++x, p += 4) {
            const bool white = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2] >= v[0] * p[3] / 255;
            p[0] = p[1] = p[2] = (white != (v[1] != 0)) ? p[3] : 0;  // blanc prémultiplié = alpha
        }
    }
    return CYIMG_OK;
}

static const CyImgEffect kEffect = {sizeof(CyImgEffect), "threshold", "plugin.monplugin.threshold", L"Seuil",
                                    nullptr, nullptr, kParams, 2, Process};
static const CyImgEffect* const kEffects[] = {&kEffect};
static const CyImgEditPlugin kPlugin = {CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "monplugin", nullptr,
                                        L"Mon plugin", L"1.0", kEffects, 1, nullptr, 0, nullptr, 0, nullptr, 0};

extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) { return &kPlugin; }
```

Pendant les réglages, `process` reçoit une copie réduite à la taille de l'écran (`img->scale` < 1 :
multiplier les distances en pixels par `scale`) ; peu après le dernier réglage, puis à « Appliquer »,
la pleine résolution. `img->source` donne les pixels d'origine (effets qui lisent les voisins).

**Outil** (colonne d'outils, après les outils intégrés ; paramètres dans la barre d'options) :

```cpp
static int Down(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent* ev) {
    const double r = h->param(c, 0) / 2;  // paramètre 0 : taille
    h->draw_ellipse(c, ev->x, ev->y, r, r, 0, 0xFF000000u | h->primary_color(c));  // sur le calque actif
    return 0;  // 1 : suivre le glissement (mouse_move puis mouse_up)
}
static const CyImgParam kToolParams[] = {CYIMG_SLIDER("plugin.monplugin.size", L"Taille", 4, 200, 30, 1, L"px")};
static const CyImgTool kTool = {sizeof(CyImgTool), "dot", "plugin.monplugin.dot", L"Point", nullptr, nullptr,
                                0xE7C8 /* icône Segoe MDL2 */, nullptr, 0, 'J', kToolParams, 1,
                                Down, nullptr, nullptr, nullptr, nullptr, nullptr};
```

Les primitives de l'hôte (`draw_line`, `draw_polyline`, `draw_rect`, `draw_ellipse`, `fill_polygon`,
`draw_text`, `measure_text`, `begin_group` / `end_group` avec opacité et mode produit) dessinent sur
le calque actif avec Direct2D, limitées à la sélection ; appelées dans `draw_overlay`, elles dessinent
un aperçu à l'écran sans rien modifier. `lock_pixels` / `unlock_pixels` donnent accès aux pixels d'un
rectangle (flou, pixellisation...). Chaque geste fait une entrée d'annulation (`begin_edit`, appelée
automatiquement au premier dessin). `input_text` ouvre une petite boîte de saisie, `set_param` met à
jour un paramètre affiché (compteur), `screen_pixel` donne la taille d'un pixel de l'écran.

**Mode d'édition** (onglet à côté de Retouche et Développement) : `controls` décrit le panneau de
droite (sections repliables `CYIMG_SECTION`, curseurs, cases, listes, couleurs, boutons
`CYIMG_BUTTON`, textes, séparateurs ; `visible_if` affiche un contrôle selon la valeur d'un autre) et
`process` calcule le résultat à partir de `img->source`. Le mode est non destructif comme le
Développement : aperçu réduit pendant les réglages, rendu complet à l'arrêt, résultat intégré au
calque (une entrée d’historique) à l’application du mode. Les modes de plugins permettent aussi d’annuler : voir [Noir et blanc](doc:black-white#cancel). Développement applique ses réglages en quittant le mode.
`on_button` reçoit les valeurs et peut les modifier (préréglages).

**Action « Ouvrir avec »** :

```cpp
static int Run(const CyImgEditHost*, CyImgEditCtx*, const wchar_t* path, void* hwnd) {
    return INT_PTR(ShellExecuteW(HWND(hwnd), L"print", path, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
static const CyImgAction kPrint = {sizeof(CyImgAction), "print", "plugin.monplugin.print", L"Imprimer…",
                                   nullptr, 0xE749, nullptr /* toutes les extensions */, Run, nullptr, nullptr};
// Ou un programme externe, sans code : program = L"%ProgramFiles%\\Outil\\outil.exe", args = L"\"%1\""
```

Les actions apparaissent dans le groupe des plugins du menu « Ouvrir avec » : en visualisation avec
le fichier affiché, en édition avec un PNG temporaire de l'image modifiée.

Règles :

- **Threads** : `process` tourne sur des threads de travail, parfois deux à la fois (aperçu et rendu
  complet) : pas d'état global modifiable, seulement `translate` et `is_cancelled` de l'hôte, à tester
  régulièrement. Les autres rappels (outils, boutons, actions) sont sur le thread de l'interface, un à
  la fois.
- **Pixels** : BGRA 8 bits à alpha prémultiplié (couleur ≤ alpha).
- **Protection** : chaque appel vers le plugin est protégé ; une fonction qui plante est désactivée
  pour la session avec un message, sans fermer l'application ni perdre le document.
- **Traduction** : textes donnés par une clé (`plugin.<nom>.<texte>`) et un texte par défaut ; les
  listes de choix se traduisent par la clé suivie de `.choices` (`Un|Deux|Trois`). Les plugins livrés
  ont leurs textes dans `src/lang/editplugins.<code>.lang`.
- **Versions** : chaque structure commence par `struct_size` ; `CYIMG_EDIT_HOST_HAS(host, champ)`
  teste la présence d'une fonction de l'hôte ajoutée plus tard.
- **Chargement** à la première entrée en édition, à l’ouverture du menu « Ouvrir avec » ou de la liste des plugins (aucun effet sur le démarrage).
  Le clic droit dans la visionneuse propose **Plugins installés** ; F1 ouvre le manuel local.

## Traduction des textes {#translation}

Les clés `plugin.<id>.<texte>` sont résolues par l’hôte ; fournissez aussi un libellé par défaut. Les listes de choix utilisent la clé suivie de `.choices`, avec les éléments séparés par `|`. [Contrat des effets](doc:api-cyimgeffect-process-process) · [Accès aux pixels](doc:api-cyimgedithost-lock-pixels-lock-pixels).
