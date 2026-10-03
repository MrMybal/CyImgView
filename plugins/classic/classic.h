// Plugin CyImgView « classic » : formats historiques (Netpbm, PCX, XBM/XPM, IFF, HDR, FITS, ICNS, PICT).
// Déclarations communes aux décodeurs.
#pragma once

#include "cyimg_plugin_i18n.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace classic {

// Fichier projeté en mémoire + accès à l'hôte. Une instance par appel de decode().
struct Dec {
    const CyImgHost* host = nullptr;
    CyImgContext* ctx = nullptr;
    const uint8_t* d = nullptr;  // contenu du fichier
    size_t n = 0;                // taille
    std::wstring ext;            // extension en minuscules, sans point

    int Fail(const wchar_t* msg) const;
    void Format(const wchar_t* name) const { host->set_format(ctx, name); }
    void Info(const wchar_t* key, const wchar_t* value) const { host->set_info(ctx, key, value); }
    void InfoA(const wchar_t* key, const std::string& value) const;  // valeur ASCII/Latin-1
    uint32_t Target() const { return host->target_size(ctx); }
    bool Cancelled() const { return host->is_cancelled(ctx) != 0; }
    // Pas de sous-échantillonnage pour une vignette (>= 1) : l'image reste plus grande que la cible.
    uint32_t ThumbStep(uint32_t w, uint32_t h) const;
};

// Dimensions acceptées par l'hôte (65535 max) et raisonnables en mémoire.
inline bool SizeOk(uint64_t w, uint64_t h) { return w && h && w <= 65535 && h <= 65535 && w * h <= (1ull << 28); }

// Image RGBA 8 bits non prémultipliée, convertie en BGRA prémultiplié par Emit.
struct Rgba {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;  // w*h*4, ordre R G B A
    void Init(uint32_t W, uint32_t H, uint8_t fill = 0) {
        w = W, h = H;
        px.assign(size_t(W) * H * 4, fill);
    }
    uint8_t* Row(uint32_t y) { return px.data() + size_t(y) * w * 4; }
};

// Envoie l'image à l'hôte (canal alpha détecté si alpha=true et qu'il n'est pas uniformément 255).
int Emit(const Dec& d, const Rgba& img, bool alpha);

// Tone mapping HDR (RVB linéaire flottant -> sRGB 8 bits). Reinhard avec exposition automatique
// (moyenne géométrique de la luminance ramenée à 0,18). Retourne l'exposition appliquée (EV).
double ToneMap(const float* rgb, uint32_t w, uint32_t h, Rgba& out, bool autoExposure = true);
uint8_t LinearToSrgb8(float v);

// Lecture big/little endian
inline uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint16_t LE16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t LE32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

std::wstring Widen(const std::string& s);  // Latin-1 -> UTF-16

// Décodeurs (retournent CYIMG_OK ou un code d'erreur, après Fail()).
int DecodeNetpbm(Dec& d);
int DecodePcx(Dec& d);
int DecodeXbm(Dec& d);
int DecodeXpm(Dec& d);
int DecodeIff(Dec& d);
int DecodeHdr(Dec& d);
int DecodeFits(Dec& d);
int DecodeIcns(Dec& d);
int DecodePict(Dec& d);

// Détection par le contenu (formats de type texte : examen des premiers octets).
bool IsXbm(const uint8_t* p, size_t n);
bool IsXpm(const uint8_t* p, size_t n);
bool IsPict(const uint8_t* p, size_t n);

}  // namespace classic
