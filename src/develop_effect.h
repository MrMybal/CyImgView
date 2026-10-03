#pragma once

// Mode Développement : réglages (à la manière d'un développeur de fichiers RAW) et leur rendu GPU.
//
// Les réglages sont de simples paramètres (Params) ; Graph construit le graphe d'effets Direct2D qui
// les applique : quelques flous gaussiens (masques et bandes de détail) alimentent un effet
// personnalisé dont le pixel shader (HLSL compilé à l'exécution par d3dcompiler_47) fait tout le
// reste en une passe, en virgule flottante : linéarisation, voile, balance des blancs, exposition,
// ombres / hautes lumières, courbes (LUT 1D en texture), TSL, vibrance, étalonnage, vignetage, grain.
// Aucune dépendance au Viewer : ce fichier peut servir à d'autres modes ou à des plugins.

#include "common.h"

#include <d2d1_1.h>

#include <string>
#include <vector>

namespace dev {

enum Param : int {
    kTemp, kTint, kExposure, kContrast, kHighlights, kShadows, kWhites, kBlacks,
    kTexture, kClarity, kDehaze, kVibrance, kSaturation,
    kPcHighlights, kPcLights, kPcDarks, kPcShadows,
    kHslHue0, kHslSat0 = kHslHue0 + 8, kHslLum0 = kHslSat0 + 8,
    kCgShadowHue = kHslLum0 + 8, kCgShadowSat, kCgMidHue, kCgMidSat, kCgHighHue, kCgHighSat, kCgBlend, kCgBalance,
    kSharpAmount, kSharpRadius, kSharpDetail, kSharpMasking, kNrLuma, kNrColor,
    kVigAmount, kVigMidpoint, kVigRoundness, kVigFeather, kGrainAmount, kGrainSize, kGrainRoughness,
    kAngle,
    kParamCount
};

enum Fmt { kFmtInt, kFmtSigned, kFmtEv, kFmtDeg, kFmtPx, kFmtAngle };

struct ParamDef {
    const char* ini;    // nom dans develop_last.ini
    const char* label;  // clé de traduction (nullptr : libellé fourni par le panneau)
    float min, max, def;
    int fmt;
    float step;         // molette, flèches
};
const ParamDef& Def(int id);

// Proportions du recadrage : 0 libre, 1 original, puis des rapports fixes
constexpr int kAspectCount = 8;
float AspectRatio(int aspect, float w, float h);  // largeur / hauteur (0 : libre)
const wchar_t* AspectLabel(int aspect);           // « 4:3 »... (nullptr : libellé traduit)

struct Pt {
    float x, y;
    bool operator==(const Pt& o) const { return x == o.x && y == o.y; }
};
using Curve = std::vector<Pt>;  // points triés par x ; vide = identité

struct Params {
    float v[kParamCount];
    Curve curve[4];       // RVB, rouge, vert, bleu
    int aspect = 0;
    bool aspectPortrait = false;  // rapport fixe inversé (hauteur > largeur)
    bool cropSet = false;         // recadrage tracé à la main (sinon : le plus grand possible)
    D2D1_RECT_F crop{0, 0, 1, 1}; // normalisé (fraction de la largeur / hauteur de l'image)

    Params();
    bool ColorIdentity() const;  // aucun réglage de couleur / tonalité / détail
    bool GeoIdentity() const;    // ni redressement ni recadrage
    bool Identity() const { return ColorIdentity() && GeoIdentity(); }
    void ResetGeometry();
    bool SameColor(const Params& o) const;
    bool operator==(const Params& o) const;
    bool operator!=(const Params& o) const { return !(*this == o); }
};

// Courbe par points : interpolation cubique monotone (pas de dépassement)
float EvalCurve(const Curve& c, float x);

// Gains RVB linéaires de la balance des blancs (luminance conservée)
void WhiteBalanceGains(float temp, float tint, float g[3]);
// Température / teinte qui rendent neutre une couleur linéaire (pipette)
void SolveWhiteBalance(float r, float g, float b, float& temp, float& tint);
// Réglage automatique de la tonalité à partir des pixels (BGRA 8 bits prémultipliés, petite image)
void AutoTone(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride, Params& p);

// Rectangle de recadrage en pixels de l'image (repère de l'image, après redressement autour du centre)
D2D1_RECT_F CropRect(const Params& p, float w, float h);
// Le plus grand rectangle centré de proportions ratio (0 : celles de l'image) dans l'image tournée
D2D1_RECT_F MaxCrop(float angleDeg, float ratio, float w, float h);
// Ramène r (même proportions) à l'intérieur de l'image tournée
D2D1_RECT_F FitCrop(D2D1_RECT_F r, float angleDeg, float w, float h);

// Histogramme RVB d'une image BGRA 8 bits prémultipliée
struct Histogram {
    uint32_t bins[3][256]{};
    uint64_t total = 0;
    float clipLow = 0, clipHigh = 0;  // fraction des pixels écrêtés (au moins un canal)
    bool valid = false;
};
void ComputeHistogram(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride, Histogram& out);

// develop_last.ini : section « clé=valeur\0...\0\0 »
std::wstring SerializeSection(const Params& p);
bool ParseSection(const wchar_t* section, Params& p);  // false si vide ; la géométrie n'est jamais lue

// --- Rendu GPU ---

bool RegisterEffect(ID2D1Factory1* factory);  // une fois par fabrique

struct View {
    float scale = 1;          // pixels de l'entrée par pixel de l'image d'origine
    float longSide = 1;       // grand côté de l'image d'origine (rayons des flous relatifs à la définition)
    D2D1_RECT_F frame{};      // zone recadrée dans l'espace de l'entrée (vignetage)
    bool clipping = false;    // zones écrêtées en couleur (rouge / bleu)
};

class Graph {
public:
    bool Init(ID2D1DeviceContext* dc, bool cacheBlurs);
    bool Ready() const { return m_dev != nullptr; }
    // Image développée (même géométrie que input). Les effets sont réutilisés d'un appel à l'autre :
    // seuls les réglages modifiés sont transmis (les flous mis en cache ne sont pas recalculés).
    ComPtr<ID2D1Image> Build(ID2D1Image* input, const Params& p, const View& v);
    void Reset();

private:
    ComPtr<ID2D1Effect> m_border, m_blur[4], m_dev;
    ID2D1Image* m_input = nullptr;
    ID2D1Image* m_wired[5]{};
    float m_sigma[4]{};
    std::vector<uint8_t> m_lastConst;
    uint64_t m_lutKey = ~0ull;
};

}  // namespace dev
