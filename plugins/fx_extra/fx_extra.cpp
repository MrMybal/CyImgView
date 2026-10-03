// Plugin CyImgView : effets supplémentaires (modèle d'un plugin d'édition qui ajoute des effets).
//
// Chaque effet décrit ses réglages (CyImgParam) et une fonction process() qui modifie img->data dans
// le rectangle img->sel. L'hôte s'occupe du reste : menu Effets, panneau de réglages, aperçu réduit
// pendant les réglages, rendu complet à « Appliquer », sélection, annulation, protection.
//
// Rappels : pixels BGRA 8 bits PRÉMULTIPLIÉS (couleur <= alpha) ; process() tourne sur un thread de
// travail (pas d'état global modifiable) et doit tester host->is_cancelled() régulièrement ; les
// distances en pixels se multiplient par img->scale (aperçu réduit).

#include "cyimg_edit_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

inline uint8_t Clamp8(double v) { return uint8_t(std::clamp(v, 0.0, 255.0) + 0.5); }

// Bruit pseudo-aléatoire stable : même résultat dans l'aperçu et le rendu complet
inline uint32_t Hash(uint32_t x, uint32_t y, uint32_t c) {
    uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ c * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return h;
}

// Luminance (0..255) d'un pixel prémultiplié, ramenée à l'alpha : comparable à 255 * a / 255
inline double Luma(const uint8_t* p) { return 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]; }

// Parcourt les lignes de la sélection ; false si le calcul est abandonné
template <class F>
int ForEachRow(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, F&& row) {
    const CyImgRect& s = img->sel;
    for (uint32_t y = 0; y < s.h; ++y) {
        if ((y & 15) == 0 && host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
        const uint32_t yy = uint32_t(s.y) + y;
        uint8_t* p = img->data + size_t(yy) * img->stride + size_t(s.x) * 4;
        row(p, uint32_t(s.x), yy);
    }
    return CYIMG_OK;
}

// --- Bruit -----------------------------------------------------------------------------------

const CyImgParam kNoiseParams[] = {
    CYIMG_SLIDER("plugin.fx_extra.amount", L"Quantité", 0, 100, 25, 1, L"%"),
    CYIMG_CHECK("plugin.fx_extra.mono", L"Monochrome", 1),
};

int Noise(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* v) {
    const double amount = v[0] / 100 * 80;  // écart maximal, en niveaux
    const bool mono = v[1] != 0;
    const double inv = 1 / std::max(img->scale, 1e-6);  // coordonnées de l'image réelle
    return ForEachRow(host, ctx, img, [&](uint8_t* p, uint32_t x0, uint32_t y) {
        const uint32_t fy = uint32_t(y * inv);
        for (uint32_t x = 0; x < img->sel.w; ++x, p += 4) {
            const uint32_t a = p[3];
            if (!a) continue;
            const uint32_t fx = uint32_t((x0 + x) * inv);
            // somme de deux uniformes (16 bits chacune) : répartition en triangle sur [-1, 1]
            auto tri = [](uint32_t h) { return (double(h & 0xFFFF) + double(h >> 16)) / 65535.0 - 1.0; };
            double n[3];
            if (mono) n[0] = n[1] = n[2] = tri(Hash(fx, fy, 7));
            else
                for (uint32_t c = 0; c < 3; ++c) n[c] = tri(Hash(fx, fy, c));
            for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::min<double>(a, Clamp8(p[c] + n[c] * amount * a / 255)));
        }
    });
}

// --- Seuil noir & blanc ----------------------------------------------------------------------

const CyImgParam kThresholdParams[] = {
    CYIMG_SLIDER("plugin.fx_extra.level", L"Niveau", 1, 254, 128, 1, nullptr),
};

int Threshold(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* v) {
    const double level = v[0];
    return ForEachRow(host, ctx, img, [&](uint8_t* p, uint32_t, uint32_t) {
        for (uint32_t x = 0; x < img->sel.w; ++x, p += 4) {
            const uint8_t a = p[3];
            const uint8_t out = Luma(p) >= level * a / 255 ? a : 0;  // blanc prémultiplié = alpha
            p[0] = p[1] = p[2] = out;
        }
    });
}

// --- Bichromie -------------------------------------------------------------------------------

const CyImgParam kDuotoneParams[] = {
    CYIMG_COLOR("plugin.fx_extra.shadows", L"Ombres", 0x1B2A6B),
    CYIMG_COLOR("plugin.fx_extra.highlights", L"Hautes lumières", 0xF4C95D),
    CYIMG_SLIDER("plugin.fx_extra.contrast", L"Contraste", -100, 100, 0, 1, nullptr),
};

int Duotone(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* v) {
    const uint32_t c1 = uint32_t(v[0]), c2 = uint32_t(v[1]);
    const double k = v[2] >= 0 ? 1 + v[2] / 50 : 1 + v[2] / 100;
    const double s[3] = {double(c1 & 0xFF), double((c1 >> 8) & 0xFF), double((c1 >> 16) & 0xFF)};  // B, V, R
    const double h[3] = {double(c2 & 0xFF), double((c2 >> 8) & 0xFF), double((c2 >> 16) & 0xFF)};
    return ForEachRow(host, ctx, img, [&](uint8_t* p, uint32_t, uint32_t) {
        for (uint32_t x = 0; x < img->sel.w; ++x, p += 4) {
            const uint32_t a = p[3];
            if (!a) continue;
            const double t = std::clamp((Luma(p) / a - 0.5) * k + 0.5, 0.0, 1.0);
            for (int c = 0; c < 3; ++c) p[c] = Clamp8((s[c] + (h[c] - s[c]) * t) * a / 255);
        }
    });
}

// --- Aberration chromatique ------------------------------------------------------------------

const CyImgParam kAberrationParams[] = {
    CYIMG_SLIDER("plugin.fx_extra.offset", L"Décalage", -30, 30, 6, 0.5, L"px"),
};

// Composante c de la source en (x, y) (interpolation bilinéaire, bords prolongés)
inline double Sample(const CyImgImage* img, double x, double y, int c) {
    x = std::clamp(x, 0.0, double(img->width) - 1.001);
    y = std::clamp(y, 0.0, double(img->height) - 1.001);
    const uint32_t x0 = uint32_t(x), y0 = uint32_t(y);
    const double fx = x - x0, fy = y - y0;
    const uint8_t* r0 = img->source + size_t(y0) * img->stride + size_t(x0) * 4 + c;
    const uint8_t* r1 = r0 + img->stride;
    return (r0[0] * (1 - fx) + r0[4] * fx) * (1 - fy) + (r1[0] * (1 - fx) + r1[4] * fx) * fy;
}

// Rouge agrandi, bleu réduit autour du centre : franges colorées vers les bords. Lit img->source
// (les pixels voisins d'origine), écrit img->data.
int Aberration(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* v) {
    const double cx = img->width / 2.0, cy = img->height / 2.0;
    const double k = v[0] * img->scale / std::max(1.0, std::hypot(cx, cy));  // décalage aux coins
    return ForEachRow(host, ctx, img, [&](uint8_t* p, uint32_t x0, uint32_t y) {
        const double dy = y + 0.5 - cy;
        for (uint32_t x = 0; x < img->sel.w; ++x, p += 4) {
            const double dx = x0 + x + 0.5 - cx;
            const uint8_t a = p[3];
            p[2] = uint8_t(std::min<double>(a, Clamp8(Sample(img, cx + dx * (1 + k) - 0.5, cy + dy * (1 + k) - 0.5, 2))));
            p[0] = uint8_t(std::min<double>(a, Clamp8(Sample(img, cx + dx * (1 - k) - 0.5, cy + dy * (1 - k) - 0.5, 0))));
        }
    });
}

// --- Description du plugin -------------------------------------------------------------------

#define FX(id, name, fn, params) \
    {sizeof(CyImgEffect), id, "plugin.fx_extra." id, name, "plugin.fx_extra.category", L"Effets supplémentaires", params, \
     uint32_t(sizeof(params) / sizeof(params[0])), fn}

const CyImgEffect kNoise = FX("noise", L"Bruit", Noise, kNoiseParams);
const CyImgEffect kThreshold = FX("threshold", L"Seuil noir et blanc", Threshold, kThresholdParams);
const CyImgEffect kDuotone = FX("duotone", L"Bichromie", Duotone, kDuotoneParams);
const CyImgEffect kAberration = FX("aberration", L"Aberration chromatique", Aberration, kAberrationParams);
const CyImgEffect* const kEffects[] = {&kNoise, &kThreshold, &kDuotone, &kAberration};

const CyImgEditPlugin kPlugin = {
    CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "fx_extra", "plugin.fx_extra.name", L"Effets supplémentaires", L"1.0",
    kEffects, uint32_t(sizeof(kEffects) / sizeof(kEffects[0])),
    nullptr, 0,  // outils
    nullptr, 0,  // modes
    nullptr, 0,  // actions
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) { return &kPlugin; }
