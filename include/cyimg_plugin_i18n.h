/*
 * CyImgView — aide à la traduction pour les plugins.
 *
 *     const wchar_t* msg = cyimg_tr(host, ctx, "plugin.webp.truncated", L"Fichier WebP tronqué");
 *
 * Le texte est cherché dans les fichiers de langue de CyImgView (src/lang/plugin.<nom>.<code>.lang ou
 * dossier « lang » à côté de l'exe). Avec un hôte ancien, ou si la clé est inconnue, le texte par
 * défaut (fallback) est renvoyé tel quel.
 *
 * Pour les fonctions internes qui n'ont pas accès à host/ctx : appeler cyimg_tr_begin(host, ctx) au
 * début de decode(), puis CYIMG_TR("clé", L"texte") n'importe où dans le même thread.
 */
#pragma once

#include "cyimg_plugin.h"

static inline const wchar_t* cyimg_tr(const CyImgHost* host, CyImgContext* ctx, const char* key, const wchar_t* fallback) {
    return (host && CYIMG_HOST_HAS(host, translate)) ? host->translate(ctx, key, fallback) : fallback;
}

#ifdef __cplusplus
struct CyImgTrState {
    const CyImgHost* host;
    CyImgContext* ctx;
};
inline CyImgTrState& cyimg_tr_state() {
    static thread_local CyImgTrState s = {nullptr, nullptr};
    return s;
}
inline void cyimg_tr_begin(const CyImgHost* host, CyImgContext* ctx) { cyimg_tr_state() = {host, ctx}; }
#define CYIMG_TR(key, fallback) cyimg_tr(cyimg_tr_state().host, cyimg_tr_state().ctx, key, fallback)
#endif
