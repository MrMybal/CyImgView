/*
 * CyImgView — API des plugins d'édition (version 1)
 *
 * Un plugin d'édition est une DLL placée dans le dossier "plugins" à côté de CyImgView.exe (le même
 * dossier que les plugins de décodage ; une même DLL peut exporter les deux points d'entrée). Elle
 * exporte :
 *
 *     extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost* host);
 *
 * et décrit des tableaux de quatre sortes d'extensions :
 *
 *  - Effets (menu Effets du mode Retouche) : une fonction process() qui modifie des pixels. L'hôte
 *    affiche le même panneau de réglages que pour ses effets intégrés : aperçu calculé sur une copie
 *    réduite à la taille de l'écran pendant les réglages, puis rendu complet en arrière-plan et à
 *    « Appliquer », limité à la sélection, annulable (Ctrl+Z).
 *  - Outils (colonne d'outils de la Retouche, après les outils intégrés) : rappels souris en
 *    coordonnées de l'image et dessin d'une surcouche d'aperçu. L'hôte fournit des primitives de
 *    dessin (traits, rectangles, ellipses, polygones, texte : rendus par Direct2D, antialiasés) sur
 *    le calque actif, l'accès direct à ses pixels, l'annulation, les couleurs, la sélection.
 *  - Modes d'édition (onglet à côté de Retouche et Développement) : un panneau de contrôles décrit
 *    par le plugin et rendu par l'hôte (sections repliables, curseurs, cases, listes, boutons,
 *    textes), et une fonction process() source -> résultat. Non destructif : l'aperçu est recalculé
 *    à chaque réglage (réduit pendant le réglage, complet à l'arrêt) et le résultat n'est intégré au
 *    calque (une entrée d'historique) qu'au retour en Retouche, à « Terminer » ou à l'enregistrement.
 *  - Actions « Ouvrir avec » (groupe des plugins du menu Ouvrir avec) : une fonction run(), ou un
 *    programme externe à lancer. En visualisation l'action reçoit le fichier affiché ; en édition, un
 *    PNG temporaire de l'image modifiée.
 *
 * Règles :
 *  - API C pure, versionnée : chaque structure commence par struct_size (= sizeof de la structure
 *    vue par le plugin). Les champs ajoutés plus tard le seront à la fin ; l'hôte ne lit que ce que
 *    struct_size couvre. Côté plugin, tester les fonctions de l'hôte avec CYIMG_EDIT_HOST_HAS.
 *  - Pixels : BGRA 8 bits, alpha PRÉMULTIPLIÉ (chaque couleur <= alpha). Un plugin qui travaille sur
 *    des couleurs « droites » doit dé-prémultiplier puis re-prémultiplier (voir les exemples).
 *  - Threads : process() (effets et modes) est appelée sur des threads de travail, parfois deux à la
 *    fois (aperçu réduit et rendu complet) : pas d'état global modifiable. Elle ne doit utiliser que
 *    translate() et is_cancelled() de l'hôte, et abandonner (CYIMG_E_CANCELLED) quand is_cancelled()
 *    devient non nul (tester toutes les quelques lignes). Tous les autres rappels (outils, boutons,
 *    actions) sont appelés sur le thread de l'interface, un seul à la fois : un outil peut garder son
 *    état dans des variables statiques.
 *  - Robustesse : chaque appel vers le plugin est protégé. Un plugin qui plante (accès mémoire
 *    invalide, exception) voit la fonction concernée (effet, outil, mode, action) désactivée pour la
 *    session, avec un message ; le document n'est pas perdu et l'application reste ouverte.
 *  - Traduction : les textes affichés sont donnés par une clé ASCII (convention
 *    « plugin.<nom>.<texte> », cherchée dans les fichiers de langue de CyImgView) et un texte par
 *    défaut utilisé si la clé est inconnue. Une liste de choix se traduit avec la clé du contrôle
 *    suivie de « .choices » (valeur « Un|Deux|Trois »).
 *  - Les chaînes et tableaux décrits par le plugin doivent rester valides jusqu'à la fin du
 *    programme (données statiques) : la DLL n'est jamais déchargée.
 *  - Couleurs : 0xRRGGBB (paramètres, couleurs de l'outil) ; 0xAARRGGBB, alpha non prémultiplié,
 *    pour les primitives de dessin.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYIMG_EDIT_API_VERSION 1

#define CYIMG_OK             0
#define CYIMG_E_FAIL        -1
#define CYIMG_E_CANCELLED   -3
#define CYIMG_E_OUTOFMEMORY -4

typedef struct CyImgEditCtx CyImgEditCtx;  /* opaque : contexte de l'appel en cours */

typedef struct CyImgRect {   /* rectangle en pixels entiers */
    int32_t x, y;
    uint32_t w, h;
} CyImgRect;

/* --------------------------------------------------------------------------------------------
 * Paramètres (effets, outils) et contrôles (panneau d'un mode)
 * -------------------------------------------------------------------------------------------- */

enum {
    CYIMG_PARAM_SLIDER = 0,     /* curseur : min, max, def, step, unit */
    CYIMG_PARAM_CHECK = 1,      /* case à cocher : def = 0 ou 1 */
    CYIMG_PARAM_CHOICE = 2,     /* liste : choices = L"Un|Deux|Trois", def = index */
    CYIMG_PARAM_COLOR = 3,      /* couleur : def = 0xRRGGBB */
    /* Panneau d'un mode seulement : */
    CYIMG_PARAM_SECTION = 10,   /* en-tête de section repliable : def = 1 ouverte, 0 repliée */
    CYIMG_PARAM_BUTTON = 11,    /* bouton : appelle CyImgMode::on_button ; boutons consécutifs côte à côte */
    CYIMG_PARAM_LABEL = 12,     /* texte d'information */
    CYIMG_PARAM_SEPARATOR = 13  /* trait de séparation */
};

/* Drapeaux d'un paramètre */
#define CYIMG_PARAM_SQUARE  0x1u  /* curseur quadratique : plus de précision dans les petites valeurs */

typedef struct CyImgParam {
    uint32_t struct_size;      /* sizeof(CyImgParam) ; identique pour toutes les entrées d'un tableau */
    uint32_t type;             /* CYIMG_PARAM_* */
    const char* key;           /* clé de traduction du libellé (peut être NULL) */
    const wchar_t* name;       /* libellé par défaut */
    double min, max, def, step;  /* step : pas du curseur (0 = continu) */
    const wchar_t* unit;       /* curseur : unité affichée après la valeur (L"px", L"%", L"°") ou NULL */
    const wchar_t* choices;    /* liste : choix séparés par '|' (traduits par la clé key + ".choices") */
    uint32_t flags;            /* CYIMG_PARAM_SQUARE... */
    uint32_t visible_if;       /* 0 = toujours affiché ; sinon n : affiché si values[n - 1] == visible_value */
    double visible_value;
} CyImgParam;

/* Aides pour déclarer les paramètres (C++20 ou C99 : champs dans l'ordre de la structure) */
#define CYIMG_SLIDER(key, name, min, max, def, step, unit) \
    { sizeof(CyImgParam), CYIMG_PARAM_SLIDER, key, name, min, max, def, step, unit, NULL, 0, 0, 0 }
#define CYIMG_CHECK(key, name, def) \
    { sizeof(CyImgParam), CYIMG_PARAM_CHECK, key, name, 0, 1, def, 1, NULL, NULL, 0, 0, 0 }
#define CYIMG_CHOICE(key, name, choices, def) \
    { sizeof(CyImgParam), CYIMG_PARAM_CHOICE, key, name, 0, 0, def, 1, NULL, choices, 0, 0, 0 }
#define CYIMG_COLOR(key, name, rgb) \
    { sizeof(CyImgParam), CYIMG_PARAM_COLOR, key, name, 0, 0xFFFFFF, rgb, 0, NULL, NULL, 0, 0, 0 }
#define CYIMG_SECTION(key, name, open) \
    { sizeof(CyImgParam), CYIMG_PARAM_SECTION, key, name, 0, 1, open, 0, NULL, NULL, 0, 0, 0 }
#define CYIMG_BUTTON(key, name) \
    { sizeof(CyImgParam), CYIMG_PARAM_BUTTON, key, name, 0, 0, 0, 0, NULL, NULL, 0, 0, 0 }
#define CYIMG_LABEL(key, name) \
    { sizeof(CyImgParam), CYIMG_PARAM_LABEL, key, name, 0, 0, 0, 0, NULL, NULL, 0, 0, 0 }
#define CYIMG_SEPARATOR() \
    { sizeof(CyImgParam), CYIMG_PARAM_SEPARATOR, NULL, NULL, 0, 0, 0, 0, NULL, NULL, 0, 0, 0 }

/* --------------------------------------------------------------------------------------------
 * Pixels passés à process() (effets et modes)
 * -------------------------------------------------------------------------------------------- */

typedef struct CyImgImage {
    uint32_t struct_size;
    uint8_t* data;             /* pixels à modifier (BGRA prémultiplié), initialisés avec la source */
    const uint8_t* source;     /* pixels d'origine, même taille et même pas : lecture seule */
    uint32_t width, height, stride;
    CyImgRect sel;             /* zone à modifier (sélection, ou image entière) : le reste est ignoré */
    double scale;              /* échelle par rapport à l'image réelle : 1 = pleine résolution, < 1 = aperçu
                                  réduit (multiplier les distances en pixels : rayon, décalage, grain) */
    uint32_t full_width, full_height;  /* taille de l'image réelle */
} CyImgImage;

/* --------------------------------------------------------------------------------------------
 * Fonctions de l'hôte
 * -------------------------------------------------------------------------------------------- */

/* Boutons et modificateurs d'un événement souris */
#define CYIMG_BTN_LEFT   0x1u
#define CYIMG_BTN_RIGHT  0x2u
#define CYIMG_MOD_SHIFT  0x1u
#define CYIMG_MOD_CTRL   0x2u
#define CYIMG_MOD_ALT    0x4u

/* Fusion d'un groupe de primitives (begin_group) */
#define CYIMG_BLEND_NORMAL    0
#define CYIMG_BLEND_MULTIPLY  1
#define CYIMG_BLEND_SCREEN    2

/* Texte (draw_text, measure_text) */
#define CYIMG_TEXT_BOLD     0x1u
#define CYIMG_TEXT_CENTER   0x2u   /* centré horizontalement dans la largeur donnée (ou autour de x si largeur 0) */
#define CYIMG_TEXT_VCENTER  0x4u   /* centré verticalement autour de y */

typedef struct CyImgEditHost {
    uint32_t api_version;      /* CYIMG_EDIT_API_VERSION */
    uint32_t struct_size;

    /* --- Partout (y compris process() sur un thread de travail) --- */

    /* Traduction (voir l'en-tête) ; le pointeur reste valable jusqu'à la fin du programme. */
    const wchar_t* (*translate)(CyImgEditCtx* ctx, const char* key, const wchar_t* fallback);
    /* process() : non nul si le calcul n'est plus utile (réglage changé, annulation). */
    int (*is_cancelled)(CyImgEditCtx* ctx);

    /* --- Thread de l'interface (outils, boutons, actions) --- */

    void (*doc_size)(CyImgEditCtx* ctx, uint32_t* width, uint32_t* height);
    uint32_t (*primary_color)(CyImgEditCtx* ctx);    /* 0xRRGGBB */
    uint32_t (*secondary_color)(CyImgEditCtx* ctx);
    int (*selection)(CyImgEditCtx* ctx, CyImgRect* rect);  /* 1 et rect rempli s'il y a une sélection */
    double (*param)(CyImgEditCtx* ctx, uint32_t index);    /* valeur d'un paramètre de l'outil */
    void (*set_param)(CyImgEditCtx* ctx, uint32_t index, double value);  /* ex. compteur qui s'incrémente */
    double (*screen_pixel)(CyImgEditCtx* ctx);  /* taille d'un pixel de l'écran en pixels de l'image */
    void (*redraw)(CyImgEditCtx* ctx);           /* demande de redessiner (surcouche changée) */
    void (*toast)(CyImgEditCtx* ctx, const wchar_t* text);
    void* (*window)(CyImgEditCtx* ctx);          /* HWND de la fenêtre (parent des boîtes de dialogue) */
    /* Petite boîte de dialogue de saisie (multiligne). Retourne 1 si validée ; out reçoit le texte. */
    int (*input_text)(CyImgEditCtx* ctx, const wchar_t* title, const wchar_t* initial, wchar_t* out, uint32_t out_len);

    /* Modification du calque actif. begin_edit crée une entrée d'annulation (copie du calque) : les
       modifications suivantes en font partie jusqu'au prochain appui de souris. Appelée
       automatiquement par la première modification si le plugin ne l'a pas fait. Toutes les
       modifications sont limitées à la sélection. Retourne 0 si impossible (mémoire). */
    int (*begin_edit)(CyImgEditCtx* ctx);
    /* Accès aux pixels du calque actif (BGRA prémultiplié) : un seul verrou à la fois. Le rectangle
       est ramené au document (rect est mis à jour) ; NULL si vide. unlock(modified = 1) renvoie la
       zone sur le GPU. */
    uint8_t* (*lock_pixels)(CyImgEditCtx* ctx, CyImgRect* rect, uint32_t* stride);
    void (*unlock_pixels)(CyImgEditCtx* ctx, int modified);
    /* Nouveau calque vide au-dessus du calque actif, qui devient actif. 0 si impossible. */
    int (*new_layer)(CyImgEditCtx* ctx, const wchar_t* name);

    /* Primitives de dessin, coordonnées en pixels de l'image (antialiasées). Hors de draw_overlay :
       dessin sur le calque actif. Pendant draw_overlay : aperçu à l'écran, rien n'est modifié.
       width <= 0 : forme pleine. */
    void (*draw_line)(CyImgEditCtx* ctx, double x1, double y1, double x2, double y2, double width, uint32_t argb);
    void (*draw_polyline)(CyImgEditCtx* ctx, const double* xy, uint32_t points, double width, uint32_t argb);
    void (*draw_rect)(CyImgEditCtx* ctx, double x, double y, double w, double h, double radius, double width, uint32_t argb);
    void (*draw_ellipse)(CyImgEditCtx* ctx, double cx, double cy, double rx, double ry, double width, uint32_t argb);
    void (*fill_polygon)(CyImgEditCtx* ctx, const double* xy, uint32_t points, uint32_t argb);
    /* Texte (Segoe UI) : size = hauteur de police en pixels de l'image ; box_w > 0 : retour à la ligne
       automatique dans cette largeur. */
    void (*draw_text)(CyImgEditCtx* ctx, double x, double y, double box_w, const wchar_t* text, double size,
                      uint32_t argb, uint32_t flags);
    void (*measure_text)(CyImgEditCtx* ctx, const wchar_t* text, double size, double box_w, uint32_t flags,
                         double* w, double* h);
    /* Groupe : les primitives jusqu'à end_group sont composées ensemble, avec une opacité et un mode
       de fusion (CYIMG_BLEND_*) : les chevauchements d'un trait semi-transparent ne se cumulent pas. */
    void (*begin_group)(CyImgEditCtx* ctx, double opacity, uint32_t blend);
    void (*end_group)(CyImgEditCtx* ctx);
} CyImgEditHost;

#define CYIMG_EDIT_HOST_HAS(host, field) \
    ((host)->struct_size >= offsetof(CyImgEditHost, field) + sizeof((host)->field) && (host)->field != NULL)

/* --------------------------------------------------------------------------------------------
 * Effets
 * -------------------------------------------------------------------------------------------- */

typedef struct CyImgEffect {
    uint32_t struct_size;
    const char* id;            /* identifiant ASCII unique dans le plugin */
    const char* key;           /* clé de traduction du nom */
    const wchar_t* name;
    const char* category_key;  /* catégorie (section du menu Effets) ; NULL = nom du plugin */
    const wchar_t* category;
    const CyImgParam* params;  /* SLIDER, CHECK, CHOICE, COLOR */
    uint32_t param_count;
    /* Modifie img->data dans img->sel (thread de travail). values[i] = valeur du paramètre i. */
    int (*process)(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* values);
} CyImgEffect;

/* --------------------------------------------------------------------------------------------
 * Outils
 * -------------------------------------------------------------------------------------------- */

typedef struct CyImgToolEvent {
    uint32_t struct_size;
    double x, y;               /* position en pixels de l'image (peut sortir du document) */
    uint32_t buttons;          /* CYIMG_BTN_* (0 pendant un survol) */
    uint32_t mods;             /* CYIMG_MOD_* */
    uint32_t clicks;           /* 2 : double-clic */
} CyImgToolEvent;

typedef struct CyImgTool {
    uint32_t struct_size;
    const char* id;
    const char* key;           /* nom (info-bulle) */
    const wchar_t* name;
    const char* hint_key;      /* aide affichée dans la barre d'options (peut être NULL) */
    const wchar_t* hint;
    uint32_t glyph;            /* icône : code de caractère Segoe MDL2 Assets / Segoe Fluent Icons (0 = aucun) */
    const uint8_t* png;        /* ou icône PNG intégrée (prioritaire), idéalement blanche sur transparent */
    uint32_t png_size;
    uint32_t shortcut;         /* touche (lettre majuscule ASCII) ; ignorée si déjà prise par la Retouche */
    const CyImgParam* params;  /* barre d'options : SLIDER, CHECK, CHOICE, COLOR */
    uint32_t param_count;
    /* Rappels (tous facultatifs, thread de l'interface). mouse_down retourne 1 pour suivre le
       glissement (mouse_move avec boutons, puis mouse_up). mouse_move est aussi appelé au survol. */
    int (*mouse_down)(const CyImgEditHost* host, CyImgEditCtx* ctx, const CyImgToolEvent* ev);
    void (*mouse_move)(const CyImgEditHost* host, CyImgEditCtx* ctx, const CyImgToolEvent* ev);
    void (*mouse_up)(const CyImgEditHost* host, CyImgEditCtx* ctx, const CyImgToolEvent* ev);
    void (*draw_overlay)(const CyImgEditHost* host, CyImgEditCtx* ctx);  /* aperçu (primitives à l'écran) */
    void (*activate)(const CyImgEditHost* host, CyImgEditCtx* ctx, int active);  /* outil choisi / quitté */
    void (*cancel)(const CyImgEditHost* host, CyImgEditCtx* ctx);  /* Échap pendant un glissement */
} CyImgTool;

/* --------------------------------------------------------------------------------------------
 * Modes d'édition
 * -------------------------------------------------------------------------------------------- */

typedef struct CyImgMode {
    uint32_t struct_size;
    const char* id;
    const char* key;           /* nom de l'onglet */
    const wchar_t* name;
    const char* hint_key;      /* aide affichée dans la barre d'options */
    const wchar_t* hint;
    uint32_t glyph;            /* icône de l'onglet (Segoe MDL2 / Fluent) */
    const CyImgParam* controls;  /* panneau de droite, de haut en bas ; values[i] = valeur du contrôle i */
    uint32_t control_count;
    /* Calcule img->data à partir de img->source (thread de travail). */
    int (*process)(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* values);
    /* Facultatifs (thread de l'interface) : un bouton (préréglages...) peut modifier values. */
    void (*on_button)(const CyImgEditHost* host, CyImgEditCtx* ctx, uint32_t control, double* values);
    void (*on_enter)(const CyImgEditHost* host, CyImgEditCtx* ctx, double* values);
    void (*on_leave)(const CyImgEditHost* host, CyImgEditCtx* ctx, const double* values);
} CyImgMode;

/* --------------------------------------------------------------------------------------------
 * Actions « Ouvrir avec »
 * -------------------------------------------------------------------------------------------- */

typedef struct CyImgAction {
    uint32_t struct_size;
    const char* id;
    const char* key;           /* libellé du menu */
    const wchar_t* name;
    const wchar_t* icon;       /* icône : chemin d'un .exe, .dll ou .ico (variables %ENV% admises), ou NULL */
    uint32_t glyph;            /* sinon icône Segoe MDL2 / Fluent (0 = aucune) */
    const wchar_t* extensions; /* L"jpg;png" : fichiers acceptés ; NULL ou vide = tous */
    /* Soit une fonction (retourne 1 si réussi, 0 : l'hôte affiche un échec)... */
    int (*run)(const CyImgEditHost* host, CyImgEditCtx* ctx, const wchar_t* path, void* hwnd_parent);
    /* ... soit un programme externe : chemin (%ENV% admis) et arguments (%1 = chemin de l'image). */
    const wchar_t* program;
    const wchar_t* args;
} CyImgAction;

/* --------------------------------------------------------------------------------------------
 * Description du plugin
 * -------------------------------------------------------------------------------------------- */

typedef struct CyImgEditPlugin {
    uint32_t api_version;      /* CYIMG_EDIT_API_VERSION */
    uint32_t struct_size;      /* sizeof(CyImgEditPlugin) */
    const char* id;            /* nom court ASCII (préfixe des clés « plugin.<id>.* ») */
    const char* key;           /* nom affiché */
    const wchar_t* name;
    const wchar_t* version;
    const CyImgEffect* const* effects;  uint32_t effect_count;
    const CyImgTool* const* tools;      uint32_t tool_count;
    const CyImgMode* const* modes;      uint32_t mode_count;
    const CyImgAction* const* actions;  uint32_t action_count;
} CyImgEditPlugin;

typedef const CyImgEditPlugin* (*CyImgGetEditPluginFn)(const CyImgEditHost* host);
#define CYIMG_EDIT_PLUGIN_ENTRY "CyImg_GetEditPlugin"

/* Aide : texte traduit (repli sur fallback avec un hôte qui n'a pas translate). */
static inline const wchar_t* cyimg_edit_tr(const CyImgEditHost* host, CyImgEditCtx* ctx, const char* key,
                                           const wchar_t* fallback) {
    return (host && CYIMG_EDIT_HOST_HAS(host, translate)) ? host->translate(ctx, key, fallback) : fallback;
}

#ifdef __cplusplus
}
#endif
