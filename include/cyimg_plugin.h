/*
 * CyImgView — API des plugins de décodage (version 1)
 *
 * Un plugin est une DLL placée dans le dossier "plugins" à côté de CyImgView.exe.
 * Elle doit exporter une seule fonction :
 *
 *     extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void);
 *
 * Règles :
 *  - API C pure : n'importe quel compilateur/langage capable de produire une DLL convient.
 *  - decode() est appelée simultanément depuis plusieurs threads : pas d'état global mutable.
 *  - Deux façons de rendre une image :
 *      1. host->alloc_bgra() puis remplir les pixels (BGRA 8 bits, alpha prémultiplié) ;
 *      2. host->decode_encoded() avec un flux JPEG/PNG/TIFF/... que l'hôte décode via WIC
 *         (pratique pour les RAW qui embarquent un aperçu JPEG).
 *  - host->target_size() > 0 signifie qu'on demande une vignette : le plugin peut alors
 *    rendre une image plus petite (aperçu embarqué), l'hôte redimensionne.
 *  - Affichage progressif : un plugin lent peut rendre un aperçu rapide et appeler
 *    host->set_refinable() ; l'hôte redemande ensuite la version complète (host->quality() == 1).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYIMG_API_VERSION 1

#define CYIMG_OK             0
#define CYIMG_E_FAIL        -1
#define CYIMG_E_UNSUPPORTED -2
#define CYIMG_E_CANCELLED   -3
#define CYIMG_E_OUTOFMEMORY -4

typedef struct CyImgContext CyImgContext; /* opaque, une instance par décodage */

typedef struct CyImgHost {
    uint32_t api_version;
    uint32_t struct_size;

    /* 0 = image pleine résolution demandée ; sinon taille max (px) de la vignette voulue. */
    uint32_t (*target_size)(CyImgContext* ctx);

    /* Non nul si l'hôte n'a plus besoin du résultat : le plugin peut abandonner. */
    int (*is_cancelled)(CyImgContext* ctx);

    /* Alloue l'image de sortie (BGRA 8 bits, alpha prémultiplié, A=255 si opaque).
       Retourne le début du buffer et écrit le pas (octets par ligne) dans *stride. */
    uint8_t* (*alloc_bgra)(CyImgContext* ctx, uint32_t width, uint32_t height, int has_alpha,
                           uint32_t* stride);

    /* Confie à l'hôte une image encodée (tout format lisible par Windows WIC).
       Les données n'ont besoin de rester valides que pendant l'appel. */
    int (*decode_encoded)(CyImgContext* ctx, const void* data, size_t size);

    /* Orientation EXIF (1..8), appliquée à l'affichage. */
    void (*set_orientation)(CyImgContext* ctx, uint32_t orientation);

    /* Nom du format affiché dans l'interface (ex. L"Canon CR2"). */
    void (*set_format)(CyImgContext* ctx, const wchar_t* name);

    /* Métadonnée affichée dans le panneau d'informations. */
    void (*set_info)(CyImgContext* ctx, const wchar_t* key, const wchar_t* value);

    /* --- Ajouts ultérieurs : vérifier leur présence avec CYIMG_HOST_HAS(host, champ) --- */

    /* 0 = rapide : un aperçu est acceptable ; 1 = meilleure qualité (suite à set_refinable). */
    uint32_t (*quality)(CyImgContext* ctx);

    /* L'image rendue n'est qu'un aperçu (ex. JPEG embarqué plus petit que le RAW) : l'hôte l'affiche
       tout de suite, puis rappelle decode() en arrière-plan avec quality() == 1. */
    void (*set_refinable)(CyImgContext* ctx);

    /* Message d'erreur affiché à l'utilisateur quand decode() échoue (ex. « ce .uasset est un Material »). */
    void (*set_error)(CyImgContext* ctx, const wchar_t* message);
} CyImgHost;

#define CYIMG_HOST_HAS(host, field) \
    ((host)->struct_size >= offsetof(CyImgHost, field) + sizeof((host)->field) && (host)->field != NULL)

typedef struct CyImgPlugin {
    uint32_t       api_version;  /* = CYIMG_API_VERSION */
    const wchar_t* name;
    const wchar_t* version;
    const wchar_t* extensions;   /* minuscules, sans point, séparées par ';' : L"cr2;nef" */

    /* Optionnel (peut être NULL) : non nul si l'en-tête du fichier est reconnu. */
    int (*probe)(const uint8_t* header, size_t size);

    /* Décode le fichier. Retourne CYIMG_OK ou un code d'erreur. */
    int (*decode)(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path);
} CyImgPlugin;

typedef const CyImgPlugin* (*CyImgGetPluginFn)(void);
#define CYIMG_PLUGIN_ENTRY "CyImg_GetPlugin"

/* Export optionnel : filtre rapide utilisé pour lister un dossier. Retourne 0 si le fichier (dont
   l'extension est déclarée par le plugin) ne contient pas d'image affichable — il est alors masqué.
   Exemple : parmi les .uasset d'Unreal, seules les textures sont gardées.
   extern "C" __declspec(dllexport) int CyImg_Accept(const wchar_t* path); */
typedef int (*CyImgAcceptFn)(const wchar_t* path);
#define CYIMG_ACCEPT_ENTRY "CyImg_Accept"

#ifdef __cplusplus
}
#endif
