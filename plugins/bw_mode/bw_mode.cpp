// Plugin CyImgView : mode d'édition « Noir & blanc » (modèle d'un plugin qui ajoute un mode).
//
// Le plugin décrit le panneau de droite (sections, curseurs, liste, boutons de préréglages) ; l'hôte
// le dessine, gère l'aperçu (calcul réduit pendant les réglages, complet à l'arrêt), l'annulation des
// réglages et l'intégration au calque au retour en Retouche, à « Terminer » ou à l'enregistrement.
//
// Conversion : mélangeur de couleurs à la manière de Lightroom (luminosité de chaque teinte : rouge,
// orange, jaune, vert, cyan, bleu, magenta) ou mélange des canaux R, V, B ; luminosité, contraste ;
// virage (teinte et saturation des ombres et des hautes lumières) ; grain.

#include "cyimg_edit_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

enum Ctl {
    kSecPresets, kNeutral, kHighContrast, kRedFilter, kSepia,
    kSecMixer, kMethod, kRed, kOrange, kYellow, kGreen, kCyan, kBlue, kMagenta, kChanR, kChanG, kChanB,
    kSecTone, kBrightness, kContrast,
    kSecToning, kShadowHue, kShadowSat, kHighHue, kHighSat, kBalance,
    kSecGrain, kGrainAmount, kGrainSize,
    kCount
};

// Contrôle visible seulement pour une méthode du mélangeur (visible_if = index + 1)
#define MIX_SLIDER(key, name, method) \
    {sizeof(CyImgParam), CYIMG_PARAM_SLIDER, key, name, -100, 100, 0, 1, nullptr, nullptr, 0, kMethod + 1, method}
#define CHAN_SLIDER(key, name, def) \
    {sizeof(CyImgParam), CYIMG_PARAM_SLIDER, key, name, -100, 200, def, 1, L"%", nullptr, 0, kMethod + 1, 1}

const CyImgParam kControls[kCount] = {
    CYIMG_SECTION("plugin.bw_mode.presets", L"Préréglages", 1),
    CYIMG_BUTTON("plugin.bw_mode.neutral", L"Neutre"),
    CYIMG_BUTTON("plugin.bw_mode.high_contrast", L"Contraste élevé"),
    CYIMG_BUTTON("plugin.bw_mode.red_filter", L"Filtre rouge"),
    CYIMG_BUTTON("plugin.bw_mode.sepia", L"Sépia"),
    CYIMG_SECTION("plugin.bw_mode.mixer", L"Mélangeur", 1),
    CYIMG_CHOICE("plugin.bw_mode.method", L"Méthode", L"Couleurs|Canaux R V B", 0),
    MIX_SLIDER("plugin.bw_mode.red", L"Rouge", 0),
    MIX_SLIDER("plugin.bw_mode.orange", L"Orange", 0),
    MIX_SLIDER("plugin.bw_mode.yellow", L"Jaune", 0),
    MIX_SLIDER("plugin.bw_mode.green", L"Vert", 0),
    MIX_SLIDER("plugin.bw_mode.cyan", L"Cyan", 0),
    MIX_SLIDER("plugin.bw_mode.blue", L"Bleu", 0),
    MIX_SLIDER("plugin.bw_mode.magenta", L"Magenta", 0),
    CHAN_SLIDER("plugin.bw_mode.chan_r", L"Canal rouge", 30),
    CHAN_SLIDER("plugin.bw_mode.chan_g", L"Canal vert", 59),
    CHAN_SLIDER("plugin.bw_mode.chan_b", L"Canal bleu", 11),
    CYIMG_SECTION("plugin.bw_mode.tone", L"Tonalité", 1),
    CYIMG_SLIDER("plugin.bw_mode.brightness", L"Luminosité", -100, 100, 0, 1, nullptr),
    CYIMG_SLIDER("plugin.bw_mode.contrast", L"Contraste", -100, 100, 0, 1, nullptr),
    CYIMG_SECTION("plugin.bw_mode.toning", L"Virage", 0),
    CYIMG_SLIDER("plugin.bw_mode.shadow_hue", L"Teinte des ombres", 0, 360, 30, 1, L"°"),
    CYIMG_SLIDER("plugin.bw_mode.shadow_sat", L"Saturation des ombres", 0, 100, 0, 1, nullptr),
    CYIMG_SLIDER("plugin.bw_mode.high_hue", L"Teinte des lumières", 0, 360, 45, 1, L"°"),
    CYIMG_SLIDER("plugin.bw_mode.high_sat", L"Saturation des lumières", 0, 100, 0, 1, nullptr),
    CYIMG_SLIDER("plugin.bw_mode.balance", L"Équilibre", -100, 100, 0, 1, nullptr),
    CYIMG_SECTION("plugin.bw_mode.grain", L"Grain", 0),
    CYIMG_SLIDER("plugin.bw_mode.grain_amount", L"Quantité", 0, 100, 0, 1, nullptr),
    CYIMG_SLIDER("plugin.bw_mode.grain_size", L"Taille", 1, 8, 1.5, 0.1, L"px"),
};

// Teinte -> couleur pleine (r, v, b dans 0..1)
void HueRgb(double deg, double out[3]) {
    const double h = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0) / 60, x = 1 - std::fabs(std::fmod(h, 2.0) - 1);
    const int i = int(h);
    const double r[6] = {1, x, 0, 0, x, 1}, g[6] = {x, 1, 1, x, 0, 0}, b[6] = {0, 0, x, 1, 1, x};
    out[0] = r[i % 6];
    out[1] = g[i % 6];
    out[2] = b[i % 6];
}

inline uint32_t Hash(uint32_t x, uint32_t y) {
    uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return h;
}
inline double Noise(uint32_t x, uint32_t y) { return double(Hash(x, y) & 0xFFFF) / 32767.5 - 1; }

int Process(const CyImgEditHost* host, CyImgEditCtx* ctx, CyImgImage* img, const double* v) {
    // Centres des bandes de teinte du mélangeur (degrés), dans l'ordre des curseurs
    static const double kCenters[7] = {0, 30, 60, 120, 180, 240, 300};
    const bool channels = v[kMethod] != 0;
    const double mix[7] = {v[kRed] / 100, v[kOrange] / 100, v[kYellow] / 100, v[kGreen] / 100, v[kCyan] / 100, v[kBlue] / 100, v[kMagenta] / 100};
    const double cr = v[kChanR] / 100, cg = v[kChanG] / 100, cb = v[kChanB] / 100;
    const double bright = v[kBrightness] / 200, contrast = v[kContrast] / 100;
    double shTint[3], hiTint[3];
    HueRgb(v[kShadowHue], shTint);
    HueRgb(v[kHighHue], hiTint);
    auto centered = [](double t[3]) {  // décalage de couleur de luminance nulle (le gris garde sa valeur)
        const double l = 0.299 * t[0] + 0.587 * t[1] + 0.114 * t[2];
        for (int c = 0; c < 3; ++c) t[c] -= l;
    };
    centered(shTint);
    centered(hiTint);
    const double shSat = v[kShadowSat] / 100 * 0.5, hiSat = v[kHighSat] / 100 * 0.5;
    const double bal = v[kBalance] / 100;
    const double grain = v[kGrainAmount] / 100 * 0.22;
    const double cell = std::max(1.0, v[kGrainSize]) * img->scale;  // taille du grain en pixels de ce tampon
    const CyImgRect& s = img->sel;
    for (uint32_t y = 0; y < s.h; ++y) {
        if ((y & 15) == 0 && host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
        const uint32_t yy = uint32_t(s.y) + y;
        uint8_t* p = img->data + size_t(yy) * img->stride + size_t(s.x) * 4;
        const double gy = yy / cell;
        const uint32_t gy0 = uint32_t(gy);
        const double fy = gy - gy0;
        for (uint32_t x = 0; x < s.w; ++x, p += 4) {
            const uint32_t a = p[3];
            if (!a) continue;
            const double inv = 1.0 / a;
            const double r = p[2] * inv, g = p[1] * inv, b = p[0] * inv;  // couleur non prémultipliée, 0..1
            double l;
            if (channels) {
                l = r * cr + g * cg + b * cb;
            } else {
                l = 0.299 * r + 0.587 * g + 0.114 * b;
                const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), chroma = mx - mn;
                if (chroma > 1e-4) {
                    double h;  // teinte (degrés)
                    if (mx == r) h = 60 * std::fmod((g - b) / chroma + 6, 6.0);
                    else if (mx == g) h = 60 * ((b - r) / chroma + 2);
                    else h = 60 * ((r - g) / chroma + 4);
                    int i = 6;
                    while (i > 0 && h < kCenters[i]) --i;
                    const double c0 = kCenters[i], c1 = i == 6 ? 360 : kCenters[i + 1];
                    const double t = (h - c0) / (c1 - c0);
                    const double adj = mix[i] * (1 - t) + mix[(i + 1) % 7] * t;
                    l *= 1 + 1.6 * adj * chroma;
                }
            }
            l = std::clamp(l + bright, 0.0, 1.0);
            if (contrast > 0) l += (l * l * (3 - 2 * l) - l) * contrast * 1.5;  // courbe en S
            else l = 0.5 + (l - 0.5) * (1 + contrast);
            l = std::clamp(l, 0.0, 1.0);
            if (grain > 0) {  // grain interpolé entre cellules (coordonnées de l'image réelle)
                const double gx = (s.x + x) / cell;
                const uint32_t gx0 = uint32_t(gx);
                const double fx = gx - gx0;
                const double n = (Noise(gx0, gy0) * (1 - fx) + Noise(gx0 + 1, gy0) * fx) * (1 - fy) +
                                 (Noise(gx0, gy0 + 1) * (1 - fx) + Noise(gx0 + 1, gy0 + 1) * fx) * fy;
                l = std::clamp(l + n * grain * (0.35 + 2.6 * l * (1 - l)), 0.0, 1.0);
            }
            double out[3] = {l, l, l};
            const double ws = (1 - l) * (1 - l) * std::clamp(1 - bal, 0.0, 2.0) * shSat;
            const double wh = l * l * std::clamp(1 + bal, 0.0, 2.0) * hiSat;
            for (int c = 0; c < 3; ++c) out[c] = std::clamp(out[c] + shTint[c] * ws + hiTint[c] * wh, 0.0, 1.0);
            p[2] = uint8_t(out[0] * a + 0.5);
            p[1] = uint8_t(out[1] * a + 0.5);
            p[0] = uint8_t(out[2] * a + 0.5);
        }
    }
    return CYIMG_OK;
}

// Préréglages : valeurs par défaut, puis quelques réglages
void OnButton(const CyImgEditHost*, CyImgEditCtx*, uint32_t control, double* v) {
    if (control < kNeutral || control > kSepia) return;
    for (int i = 0; i < kCount; ++i)
        if (kControls[i].type != CYIMG_PARAM_SECTION) v[i] = kControls[i].def;
    switch (control) {
    case kHighContrast:
        v[kContrast] = 65;
        v[kYellow] = 15;
        v[kBlue] = -25;
        v[kRed] = 10;
        break;
    case kRedFilter:  // ciel sombre, peau claire (filtre rouge devant l'objectif)
        v[kRed] = 70;
        v[kOrange] = 50;
        v[kYellow] = 25;
        v[kGreen] = -30;
        v[kCyan] = -60;
        v[kBlue] = -85;
        v[kContrast] = 25;
        break;
    case kSepia:
        v[kShadowHue] = 28;
        v[kShadowSat] = 55;
        v[kHighHue] = 42;
        v[kHighSat] = 40;
        v[kContrast] = 10;
        v[kBrightness] = 4;
        break;
    default: break;
    }
}

const CyImgMode kMode = {
    sizeof(CyImgMode), "bw", "plugin.bw_mode.mode", L"Noir & blanc", "plugin.bw_mode.hint",
    L"Conversion en noir et blanc : mélangeur de couleurs, contraste, virage et grain.", 0xE793,
    kControls, kCount, Process, OnButton, nullptr, nullptr,
};
const CyImgMode* const kModes[] = {&kMode};

const CyImgEditPlugin kPlugin = {
    CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "bw_mode", "plugin.bw_mode.name", L"Noir & blanc", L"1.0",
    nullptr, 0, nullptr, 0, kModes, 1, nullptr, 0,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) { return &kPlugin; }
