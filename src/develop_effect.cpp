// Mode Développement : réglages, effet Direct2D personnalisé (pixel shader) et calculs annexes
// (balance des blancs, réglage automatique, recadrage, histogramme, enregistrement des réglages).

#include "develop_effect.h"

#include <d2d1effectauthor.h>
#include <d2d1effects.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace dev {

namespace {

constexpr float kPi = 3.14159265f;

// {6B1C4F52-2D7A-4E39-9C1B-5A0E7F3D2C61}
const GUID kClsidDevelop = {0x6b1c4f52, 0x2d7a, 0x4e39, {0x9c, 0x1b, 0x5a, 0x0e, 0x7f, 0x3d, 0x2c, 0x61}};
// {A3E5D1B7-48C2-4F0A-B6E9-1D7C3A92F804}
const GUID kShaderId = {0xa3e5d1b7, 0x48c2, 0x4f0a, {0xb6, 0xe9, 0x1d, 0x7c, 0x3a, 0x92, 0xf8, 0x04}};

constexpr UINT32 kInputs = 5;     // image, flou de netteté, flou fin, flou de clarté, grand flou
constexpr UINT32 kLutSize = 1024; // LUT 1D : RVB = courbes par points, A = tonalité globale sur [0, 2]

const ParamDef kDefs[kParamCount] = {
    {"temp", "develop.p.temp", -100, 100, 0, kFmtSigned, 1},
    {"tint", "develop.p.tint", -100, 100, 0, kFmtSigned, 1},
    {"exposure", "develop.p.exposure", -5, 5, 0, kFmtEv, 0.05f},
    {"contrast", "develop.p.contrast", -100, 100, 0, kFmtSigned, 1},
    {"highlights", "develop.p.highlights", -100, 100, 0, kFmtSigned, 1},
    {"shadows", "develop.p.shadows", -100, 100, 0, kFmtSigned, 1},
    {"whites", "develop.p.whites", -100, 100, 0, kFmtSigned, 1},
    {"blacks", "develop.p.blacks", -100, 100, 0, kFmtSigned, 1},
    {"texture", "develop.p.texture", -100, 100, 0, kFmtSigned, 1},
    {"clarity", "develop.p.clarity", -100, 100, 0, kFmtSigned, 1},
    {"dehaze", "develop.p.dehaze", -100, 100, 0, kFmtSigned, 1},
    {"vibrance", "develop.p.vibrance", -100, 100, 0, kFmtSigned, 1},
    {"saturation", "develop.p.saturation", -100, 100, 0, kFmtSigned, 1},
    {"pc_highlights", "develop.p.pc_highlights", -100, 100, 0, kFmtSigned, 1},
    {"pc_lights", "develop.p.pc_lights", -100, 100, 0, kFmtSigned, 1},
    {"pc_darks", "develop.p.pc_darks", -100, 100, 0, kFmtSigned, 1},
    {"pc_shadows", "develop.p.pc_shadows", -100, 100, 0, kFmtSigned, 1},
    {"hue_red", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"hue_orange", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"hue_yellow", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"hue_green", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"hue_aqua", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"hue_blue", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"hue_purple", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"hue_magenta", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"sat_red", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"sat_orange", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"sat_yellow", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"sat_green", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"sat_aqua", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"sat_blue", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"sat_purple", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"sat_magenta", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"lum_red", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"lum_orange", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"lum_yellow", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"lum_green", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"lum_aqua", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"lum_blue", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"lum_purple", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"lum_magenta", nullptr, -100, 100, 0, kFmtSigned, 1},
    {"cg_shadow_hue", "develop.p.hue", 0, 360, 0, kFmtDeg, 1},
    {"cg_shadow_sat", "develop.p.sat", 0, 100, 0, kFmtInt, 1},
    {"cg_mid_hue", "develop.p.hue", 0, 360, 0, kFmtDeg, 1},
    {"cg_mid_sat", "develop.p.sat", 0, 100, 0, kFmtInt, 1},
    {"cg_high_hue", "develop.p.hue", 0, 360, 0, kFmtDeg, 1},
    {"cg_high_sat", "develop.p.sat", 0, 100, 0, kFmtInt, 1},
    {"cg_blend", "develop.p.blend", 0, 100, 50, kFmtInt, 1},
    {"cg_balance", "develop.p.balance", -100, 100, 0, kFmtSigned, 1},
    {"sharp_amount", "develop.p.amount", 0, 150, 0, kFmtInt, 1},
    {"sharp_radius", "develop.p.radius", 0.5f, 3, 1, kFmtPx, 0.1f},
    {"sharp_detail", "develop.p.detail", 0, 100, 25, kFmtInt, 1},
    {"sharp_masking", "develop.p.masking", 0, 100, 0, kFmtInt, 1},
    {"nr_luma", "develop.p.nr_luma", 0, 100, 0, kFmtInt, 1},
    {"nr_color", "develop.p.nr_color", 0, 100, 0, kFmtInt, 1},
    {"vig_amount", "develop.p.amount", -100, 100, 0, kFmtSigned, 1},
    {"vig_midpoint", "develop.p.midpoint", 0, 100, 50, kFmtInt, 1},
    {"vig_roundness", "develop.p.roundness", -100, 100, 0, kFmtSigned, 1},
    {"vig_feather", "develop.p.feather", 0, 100, 50, kFmtInt, 1},
    {"grain_amount", "develop.p.grain_amount", 0, 100, 0, kFmtInt, 1},
    {"grain_size", "develop.p.size", 0, 100, 25, kFmtInt, 1},
    {"grain_roughness", "develop.p.roughness", 0, 100, 50, kFmtInt, 1},
    {"angle", "develop.p.angle", -45, 45, 0, kFmtAngle, 0.1f},
};

const char* const kCurveIni[4] = {"curve_rgb", "curve_red", "curve_green", "curve_blue"};

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float Smooth(float a, float b, float x) {
    const float t = Clamp01((x - a) / (b - a));
    return t * t * (3 - 2 * t);
}
float Enc(float y) { return y <= 0.0031308f ? y * 12.92f : 1.055f * std::pow(y, 1 / 2.4f) - 0.055f; }
float Dec(float p) { return p <= 0.04045f ? p / 12.92f : std::pow((p + 0.055f) / 1.055f, 2.4f); }

bool CurveIdentity(const Curve& c) {
    return c.size() < 2 || (c.size() == 2 && c[0] == Pt{0, 0} && c[1] == Pt{1, 1});
}

// Interpolation cubique monotone (Fritsch-Carlson)
struct Spline {
    std::vector<float> x, y, m;
    explicit Spline(const Curve& c) {
        for (const Pt& p : c) {
            x.push_back(p.x);
            y.push_back(p.y);
        }
        const size_t n = x.size();
        if (n < 2) return;
        std::vector<float> d(n - 1);
        for (size_t i = 0; i + 1 < n; ++i) d[i] = (y[i + 1] - y[i]) / std::max(1e-6f, x[i + 1] - x[i]);
        m.assign(n, 0);
        m[0] = d[0];
        m[n - 1] = d[n - 2];
        for (size_t i = 1; i + 1 < n; ++i) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
        for (size_t i = 0; i + 1 < n; ++i) {
            if (d[i] == 0) {
                m[i] = m[i + 1] = 0;
                continue;
            }
            const float a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
            if (s > 9) {
                const float t = 3 / std::sqrt(s);
                m[i] = t * a * d[i];
                m[i + 1] = t * b * d[i];
            }
        }
    }
    float operator()(float v) const {
        const size_t n = x.size();
        if (n < 2) return v;
        if (v <= x[0]) return y[0];
        if (v >= x[n - 1]) return y[n - 1];
        size_t k = 0;
        while (k + 2 < n && v >= x[k + 1]) ++k;
        const float h = std::max(1e-6f, x[k + 1] - x[k]), t = (v - x[k]) / h;
        const float t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * y[k] + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * y[k + 1] +
               (t3 - t2) * h * m[k + 1];
    }
};

// Tonalité globale (luminance perceptuelle, domaine [0, 2]) : noirs, blancs, contraste, courbe paramétrique
float ToneCurve(const Params& p, float x) {
    const float b = p.v[kBlacks] / 100, w = p.v[kWhites] / 100, c = p.v[kContrast] / 100;
    float y = x;
    const float lo = x < 0.6f ? (1 - x / 0.6f) * (1 - x / 0.6f) : 0;
    y += b * 0.22f * lo + w * 0.22f * Smooth(0.4f, 1.0f, x);
    if (c != 0) {  // courbe en S autour du gris moyen (≈ 18 %)
        const float pv = 0.46f, g = std::exp(c * 0.85f);
        if (y < pv) y = pv * std::pow(std::max(y, 0.0f) / pv, g);
        else if (y < 1) y = 1 - (1 - pv) * std::pow((1 - y) / (1 - pv), g);
    }
    auto bump = [&](float v, float a, float e) {
        if (v <= a || v >= e) return 0.0f;
        const float s = std::sin(kPi * (v - a) / (e - a));
        return s * s;
    };
    const float yy = y;
    y += 0.10f * (p.v[kPcShadows] * bump(yy, 0, 0.4f) + p.v[kPcDarks] * bump(yy, 0.12f, 0.62f) +
                  p.v[kPcLights] * bump(yy, 0.38f, 0.88f) + p.v[kPcHighlights] * bump(yy, 0.6f, 1.12f)) / 100;
    return std::max(y, 0.0f);
}

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

uint64_t LutKey(const Params& p) {
    uint64_t h = 1469598103934665603ull;
    for (int id : {kContrast, kWhites, kBlacks, kPcHighlights, kPcLights, kPcDarks, kPcShadows}) h = Fnv(h, &p.v[id], 4);
    for (const Curve& c : p.curve) {
        h = Fnv(h, "|", 1);
        if (!c.empty()) h = Fnv(h, c.data(), c.size() * sizeof(Pt));
    }
    return h;
}

void BuildLut(const Params& p, std::vector<float>& out) {
    out.resize(kLutSize * 4);
    const Spline master(p.curve[0]), r(p.curve[1]), g(p.curve[2]), b(p.curve[3]);
    for (UINT32 i = 0; i < kLutSize; ++i) {
        const float x = float(i) / (kLutSize - 1);
        const float m = Clamp01(master(x));
        out[i * 4 + 0] = Clamp01(r(m));
        out[i * 4 + 1] = Clamp01(g(m));
        out[i * 4 + 2] = Clamp01(b(m));
        out[i * 4 + 3] = ToneCurve(p, x * 2);
    }
}

void HsvToRgb(float h, float s, float v, float rgb[3]) {
    const float hh = std::fmod(std::fmod(h, 1.0f) + 1.0f, 1.0f) * 6;
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(hh, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    switch (int(hh)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}

// Constantes du pixel shader (même disposition que le cbuffer HLSL)
struct Constants {
    float gain[4];   // RVB : balance des blancs × exposition ; A : voile
    float tone[4];   // ombres, hautes lumières, texture, clarté
    float col[4];    // vibrance, saturation, netteté (gain), netteté (détail)
    float det[4];    // masquage, bruit (luminance), bruit (couleur), écrêtage affiché
    float hsl[8][4]; // teinte, saturation, luminance par couleur
    float cgS[4], cgM[4], cgH[4];  // étalonnage : décalage RVB des ombres, tons moyens, hautes lumières
    float cg[4];     // pivot, largeur
    float vig0[4];   // centre, demi-taille de la zone recadrée (espace de l'entrée)
    float vig1[4];   // gain, milieu, arrondi, contour progressif
    float grain[4];  // quantité, échelle (1 / taille en pixels de l'entrée), rugosité
};

Constants MakeConstants(const Params& p, const View& v) {
    Constants k{};
    float g[3];
    WhiteBalanceGains(p.v[kTemp], p.v[kTint], g);
    const float ev = std::exp2(p.v[kExposure]);
    for (int i = 0; i < 3; ++i) k.gain[i] = g[i] * ev;
    k.gain[3] = p.v[kDehaze] / 100;
    k.tone[0] = p.v[kShadows] / 100;
    k.tone[1] = p.v[kHighlights] / 100;
    k.tone[2] = p.v[kTexture] / 100 * 1.2f;
    k.tone[3] = p.v[kClarity] / 100 * 1.3f;
    k.col[0] = p.v[kVibrance] / 100;
    k.col[1] = p.v[kSaturation] / 100;
    k.col[2] = p.v[kSharpAmount] / 100 * 1.6f;
    k.col[3] = p.v[kSharpDetail] / 100;
    k.det[0] = p.v[kSharpMasking] / 100;
    k.det[1] = p.v[kNrLuma] / 100;
    k.det[2] = p.v[kNrColor] / 100;
    k.det[3] = v.clipping ? 1.0f : 0.0f;
    for (int j = 0; j < 8; ++j) {
        k.hsl[j][0] = p.v[kHslHue0 + j] / 100;
        k.hsl[j][1] = p.v[kHslSat0 + j] / 100;
        k.hsl[j][2] = p.v[kHslLum0 + j] / 100;
    }
    float* tints[3] = {k.cgS, k.cgM, k.cgH};
    for (int z = 0; z < 3; ++z) {
        float rgb[3];
        HsvToRgb(p.v[kCgShadowHue + 2 * z] / 360, 1, 1, rgb);
        const float y = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
        const float s = p.v[kCgShadowSat + 2 * z] / 100 * 0.3f;
        for (int i = 0; i < 3; ++i) tints[z][i] = (rgb[i] - y) * s;
    }
    k.cg[0] = 0.5f - 0.3f * p.v[kCgBalance] / 100;
    k.cg[1] = p.v[kCgBlend] / 100;
    const D2D1_RECT_F f = v.frame;
    k.vig0[0] = (f.left + f.right) / 2;
    k.vig0[1] = (f.top + f.bottom) / 2;
    k.vig0[2] = std::max(1.0f, (f.right - f.left) / 2);
    k.vig0[3] = std::max(1.0f, (f.bottom - f.top) / 2);
    k.vig1[0] = p.v[kVigAmount] / 100;
    k.vig1[1] = p.v[kVigMidpoint] / 100;
    k.vig1[2] = p.v[kVigRoundness] / 100;
    k.vig1[3] = p.v[kVigFeather] / 100;
    const float grainPx = (0.8f + p.v[kGrainSize] / 100 * 2.7f) * std::max(1.0f, v.longSide / 3000);
    k.grain[0] = p.v[kGrainAmount] / 100;
    k.grain[1] = 1 / std::max(1e-3f, grainPx * v.scale);
    k.grain[2] = p.v[kGrainRoughness] / 100;
    return k;
}

// ---------------------------------------------------------------------------------------------
// Pixel shader

const char kShader[] = R"(
Texture2D T0 : register(t0);
Texture2D T1 : register(t1);
Texture2D T2 : register(t2);
Texture2D T3 : register(t3);
Texture2D T4 : register(t4);
Texture2D Lut : register(t5);
SamplerState S0 : register(s0);
SamplerState S1 : register(s1);
SamplerState S2 : register(s2);
SamplerState S3 : register(s3);
SamplerState S4 : register(s4);
SamplerState S5 : register(s5);
cbuffer C : register(b0) {
    float4 gain; float4 tone; float4 col; float4 det;
    float4 hsl[8];
    float4 cgS; float4 cgM; float4 cgH; float4 cg;
    float4 vig0; float4 vig1; float4 grain;
};
struct PSIn {
    float4 pos : SV_POSITION;
    float4 scene : SCENE_POSITION;
    float4 uv0 : TEXCOORD0;
    float4 uv1 : TEXCOORD1;
    float4 uv2 : TEXCOORD2;
    float4 uv3 : TEXCOORD3;
    float4 uv4 : TEXCOORD4;
};
static const float3 W = float3(0.2126, 0.7152, 0.0722);
static const float HC[9] = {0, 0.5, 1, 2, 3, 4, 4.5, 5, 6};  // centres des 8 teintes (sextants)

float3 Un(float4 c) { return c.a > 1e-4 ? c.rgb / c.a : float3(0, 0, 0); }
float3 ToLin(float3 c) { c = max(c, 0); return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 ToGam(float3 c) { c = max(c, 0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }
float Enc(float y) { y = max(y, 0); return y <= 0.0031308 ? y * 12.92 : 1.055 * pow(y, 1 / 2.4) - 0.055; }
float Dec(float p) { p = max(p, 0); return p <= 0.04045 ? p / 12.92 : pow((p + 0.055) / 1.055, 2.4); }
float4 L(float x) { return Lut.SampleLevel(S5, float2(saturate(x) * (1023.0 / 1024.0) + 0.5 / 1024.0, 0.5), 0); }
float3 RgbToHsv(float3 c) {
    float4 K = float4(0, -1.0 / 3, 2.0 / 3, -1);
    float4 p = c.g < c.b ? float4(c.bg, K.wz) : float4(c.gb, K.xy);
    float4 q = c.r < p.x ? float4(p.xyw, c.r) : float4(c.r, p.yzx);
    float d = q.x - min(q.w, q.y);
    return float3(abs(q.z + (q.w - q.y) / (6 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
float3 HsvToRgb(float3 c) {
    float3 p = abs(frac(c.xxx + float3(1, 2.0 / 3, 1.0 / 3)) * 6 - 3);
    return c.z * lerp(float3(1, 1, 1), saturate(p - 1), c.y);
}
float Hash(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
float Noise(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), f.x), f.y);
}

float4 main(PSIn i) : SV_Target {
    float4 s = T0.Sample(S0, i.uv0.xy);
    float a = s.a;
    float3 c = Un(s);
    float3 b1 = Un(T1.Sample(S1, i.uv1.xy));
    float3 b2 = Un(T2.Sample(S2, i.uv2.xy));
    float3 b3 = Un(T3.Sample(S3, i.uv3.xy));
    float3 b4 = Un(T4.Sample(S4, i.uv4.xy));
    float y = dot(c, W), y1 = dot(b1, W), y2 = dot(b2, W), y3 = dot(b3, W);

    // Réduction du bruit : luminance rapprochée du petit flou là où l'écart est faible (pas sur les contours),
    // couleur remplacée par celle du flou fin
    float d = y1 - y, th = 0.012 + 0.05 * det.y;
    c += d * det.y * exp(-d * d / (th * th));
    y = dot(c, W);
    c = lerp(c, y + (b2 - y2), det.z);

    // Netteté (masque flou sur la luminance), texture (détails moyens), clarté (contraste local des tons moyens)
    float lim = lerp(0.03, 0.3, col.w);
    float edge = abs(y1 - y2) * 8;
    float mask = det.x > 0 ? smoothstep(det.x * 0.6, det.x * 0.6 + 0.15, edge) : 1;
    float dl = col.z * clamp(y - y1, -lim, lim) * mask;
    float dt = y - y2;
    dl += tone.z * dt / (1 + abs(dt) * 6);
    float dc = y - y3;  // écarts forts (contours) atténués : moins de halos
    dl += tone.w * dc / (1 + abs(dc) * 5) * saturate(1 - (2 * y - 1) * (2 * y - 1));
    c += dl;

    // Espace linéaire. Correction du voile : lumière ambiante blanche, transmission estimée par le canal sombre
    float3 lin = ToLin(c);
    float3 l4 = ToLin(b4);
    if (gain.a > 0) {
        float dark = lerp(min(lin.r, min(lin.g, lin.b)), min(l4.r, min(l4.g, l4.b)), 0.7);
        float t = max(1 - 0.9 * gain.a * saturate(dark), 0.15);
        lin = max(1 - (1 - lin) / t, 0) * (1 + 0.25 * gain.a);
        l4 = max(1 - (1 - l4) / t, 0) * (1 + 0.25 * gain.a);
    } else if (gain.a < 0) {
        float t = 1 + 0.7 * gain.a;
        lin = lin * t + (1 - t) * 0.8;
        l4 = l4 * t + (1 - t) * 0.8;
    }
    // Balance des blancs, exposition
    lin *= gain.rgb;
    l4 *= gain.rgb;

    // Ombres / hautes lumières : masque adapté localement (grand flou), qui suit le pixel sur les contours
    float Y = dot(lin, W);
    float P = Enc(Y), Pb = Enc(dot(l4, W));
    float Pm = lerp(Pb, P, saturate(abs(P - Pb) * 2) * 0.6 + 0.2);
    float ws = pow(saturate(1 - Pm / 0.55), 2);
    float wh = pow(saturate((Pm - 0.3) / 0.7), 1.5);
    lin *= exp2(tone.x * 1.6 * ws + tone.y * 1.5 * wh);

    // Tonalité globale (contraste, blancs, noirs, courbe paramétrique) sur la luminance, teinte conservée
    Y = dot(lin, W);
    if (Y > 1e-6) lin *= Dec(L(Enc(Y) * 0.5).a) / Y;
    // Hors gamut : désaturation vers la luminance plutôt qu'écrêtage canal par canal
    float mx = max(lin.r, max(lin.g, lin.b));
    if (mx > 1) {
        float yc = min(dot(lin, W), 1);
        lin = yc + (lin - yc) * saturate((1 - yc) / max(mx - yc, 1e-5));
    }

    // Espace perceptuel : courbes par points (RVB puis canal)
    float3 p = saturate(ToGam(lin));
    p = float3(L(p.r).r, L(p.g).g, L(p.b).b);

    // Mélangeur TSL : réglages interpolés entre les deux teintes voisines, gris épargnés
    float3 hsv = RgbToHsv(p);
    float h6 = hsv.x * 6;
    float3 adj = hsl[0].xyz;
    [unroll] for (int j = 0; j < 8; ++j) {
        if (h6 >= HC[j] && h6 < HC[j + 1])
            adj = lerp(hsl[j].xyz, hsl[(j + 1) % 8].xyz, smoothstep(0, 1, (h6 - HC[j]) / (HC[j + 1] - HC[j])));
    }
    float sat0 = hsv.y, wsat = saturate(sat0 * 3);
    hsv.x = frac(hsv.x + adj.x * (30.0 / 360) * wsat + 1);
    hsv.y = saturate(hsv.y * (1 + adj.y));
    p = HsvToRgb(hsv);
    p = saturate(p * (1 + adj.z * 0.5 * sat0));

    // Vibrance (couleurs peu saturées d'abord, tons chair protégés) et saturation
    float yp = dot(p, W);
    float skin = exp(-pow((hsv.x - 0.07) / 0.06, 2));
    float vib = col.x;
    float f = (1 + col.y) * (1 + (vib > 0 ? vib * (1 - sat0) * (1 - 0.6 * skin) : vib * (1 - 0.3 * sat0)));
    p = saturate(yp + (p - yp) * f);

    // Étalonnage des couleurs : teinte ajoutée par zone de luminance
    float lp = dot(p, W), wd = 0.5 + cg.y;
    float wS = exp(-pow(lp / (cg.x * wd), 2));
    float wH = exp(-pow((1 - lp) / ((1 - cg.x) * wd), 2));
    float wM = exp(-pow((lp - cg.x) / (0.25 * wd), 2));
    p = saturate(p + wS * cgS.rgb + wM * cgM.rgb + wH * cgH.rgb);

    // Vignetage (zone recadrée) : arrondi = ellipse -> cercle, ou -> rectangle
    float2 dv = i.scene.xy - vig0.xy;
    float2 q = lerp(dv / vig0.zw, dv / sqrt(vig0.z * vig0.w), saturate(vig1.z));
    float n = 2 + max(-vig1.z, 0) * 6;
    float dist = pow(pow(abs(q.x), n) + pow(abs(q.y), n), 1 / n);
    float r0 = lerp(0.25, 1.45, vig1.y), fw = 0.05 + vig1.w * 1.1;
    float v = smoothstep(r0 - fw * 0.5, r0 + fw * 0.5, dist);
    p = vig1.x < 0 ? p * (1 + vig1.x * v) : lerp(p, float3(1, 1, 1), vig1.x * v);

    // Grain, attaché aux pixels de l'image
    if (grain.x > 0) {
        float2 gp = i.scene.xy * grain.y;
        float nz = lerp(Noise(gp) - 0.5, Noise(gp * 2.3 + 17) - 0.5, grain.z * 0.6);
        float lg = dot(p, W);
        p = saturate(p + grain.x * 0.35 * nz * (1 - (2 * lg - 1) * (2 * lg - 1) * 0.6));
    }

    // Zones écrêtées : hautes lumières en rouge, ombres en bleu
    if (det.w > 0) {
        if (max(p.r, max(p.g, p.b)) >= 0.998) p = float3(1, 0.15, 0.15);
        else if (max(p.r, max(p.g, p.b)) <= 0.002) p = float3(0.15, 0.4, 1);
    }
    return float4(p * a, a);
}
)";

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

// Bytecode du shader, compilé une fois (thread de l'interface)
const std::vector<uint8_t>& ShaderBytecode() {
    static std::vector<uint8_t> code;
    static bool tried = false;
    if (tried) return code;
    tried = true;
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<D3DCompileFn>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    if (!compile) return code;
    ComPtr<ID3DBlob> blob, err;
    if (SUCCEEDED(compile(kShader, sizeof(kShader) - 1, "develop", nullptr, nullptr, "main", "ps_4_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
        const uint8_t* b = static_cast<const uint8_t*>(blob->GetBufferPointer());
        code.assign(b, b + blob->GetBufferSize());
    } else if (err) {
        OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
    }
    return code;
}

// ---------------------------------------------------------------------------------------------
// Effet Direct2D : une seule transformation de dessin, 5 entrées, 1 texture de LUT

class DevelopEffect final : public ID2D1EffectImpl, public ID2D1DrawTransform {
public:
    static HRESULT CALLBACK Create(IUnknown** out) {
        *out = static_cast<ID2D1EffectImpl*>(new (std::nothrow) DevelopEffect());
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    static DevelopEffect* From(IUnknown* u) { return static_cast<DevelopEffect*>(static_cast<ID2D1EffectImpl*>(u)); }
    static const DevelopEffect* From(const IUnknown* u) {
        return static_cast<const DevelopEffect*>(static_cast<const ID2D1EffectImpl*>(u));
    }

    static HRESULT CALLBACK SetConst(IUnknown* e, const BYTE* data, UINT32 size) {
        DevelopEffect* self = From(e);
        if (size != sizeof(Constants)) return E_INVALIDARG;
        memcpy(&self->m_const, data, size);
        return S_OK;
    }
    static HRESULT CALLBACK GetConst(const IUnknown* e, BYTE* data, UINT32 size, UINT32* actual) {
        if (actual) *actual = sizeof(Constants);
        if (data && size >= sizeof(Constants)) memcpy(data, &From(e)->m_const, sizeof(Constants));
        return S_OK;
    }
    static HRESULT CALLBACK SetLut(IUnknown* e, const BYTE* data, UINT32 size) {
        DevelopEffect* self = From(e);
        if (size != kLutSize * 16) return E_INVALIDARG;
        self->m_lut.assign(reinterpret_cast<const float*>(data), reinterpret_cast<const float*>(data) + kLutSize * 4);
        self->m_lutDirty = true;
        return S_OK;
    }
    static HRESULT CALLBACK GetLut(const IUnknown* e, BYTE* data, UINT32 size, UINT32* actual) {
        const DevelopEffect* self = From(e);
        const UINT32 n = UINT32(self->m_lut.size() * 4);
        if (actual) *actual = n;
        if (data && size >= n && n) memcpy(data, self->m_lut.data(), n);
        return S_OK;
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1EffectImpl)) *out = static_cast<ID2D1EffectImpl*>(this);
        else if (riid == __uuidof(ID2D1DrawTransform) || riid == __uuidof(ID2D1Transform) || riid == __uuidof(ID2D1TransformNode))
            *out = static_cast<ID2D1DrawTransform*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ULONG(InterlockedIncrement(&m_ref)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return ULONG(r);
    }

    // ID2D1EffectImpl
    IFACEMETHODIMP Initialize(ID2D1EffectContext* ctx, ID2D1TransformGraph* graph) override {
        const std::vector<uint8_t>& code = ShaderBytecode();
        if (code.empty()) return E_FAIL;
        HRESULT hr = ctx->LoadPixelShader(kShaderId, code.data(), UINT32(code.size()));
        if (FAILED(hr)) return hr;
        m_ctx = ctx;
        return graph->SetSingleTransformNode(this);
    }
    IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE) override {
        if (!m_draw) return E_FAIL;
        HRESULT hr = m_draw->SetPixelShaderConstantBuffer(reinterpret_cast<const BYTE*>(&m_const), sizeof(m_const));
        if (FAILED(hr)) return hr;
        if (m_lutDirty || !m_lutTex) {
            if (m_lut.size() != kLutSize * 4) {  // identité tant que rien n'a été transmis
                m_lut.resize(kLutSize * 4);
                for (UINT32 i = 0; i < kLutSize; ++i) {
                    const float x = float(i) / (kLutSize - 1);
                    m_lut[i * 4] = m_lut[i * 4 + 1] = m_lut[i * 4 + 2] = x;
                    m_lut[i * 4 + 3] = x * 2;
                }
            }
            const UINT32 ext[2] = {kLutSize, 1};
            const D2D1_EXTEND_MODE modes[2] = {D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP};
            D2D1_RESOURCE_TEXTURE_PROPERTIES rp{};
            rp.extents = ext;
            rp.dimensions = 2;
            rp.bufferPrecision = D2D1_BUFFER_PRECISION_32BPC_FLOAT;
            rp.channelDepth = D2D1_CHANNEL_DEPTH_4;
            rp.filter = D2D1_FILTER_MIN_MAG_MIP_LINEAR;
            rp.extendModes = modes;
            const UINT32 strides[1] = {kLutSize * 16};
            ComPtr<ID2D1ResourceTexture> tex;
            hr = m_ctx->CreateResourceTexture(nullptr, &rp, reinterpret_cast<const BYTE*>(m_lut.data()), strides,
                                              kLutSize * 16, &tex);
            if (FAILED(hr)) return hr;
            hr = m_draw->SetResourceTexture(kInputs, tex.Get());
            if (FAILED(hr)) return hr;
            m_lutTex = tex;
            m_lutDirty = false;
        }
        return S_OK;
    }
    IFACEMETHODIMP SetGraph(ID2D1TransformGraph*) override { return E_NOTIMPL; }

    // ID2D1DrawTransform
    IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* info) override {
        m_draw = info;
        return info->SetPixelShader(kShaderId, D2D1_PIXEL_OPTIONS_NONE);
    }
    // ID2D1Transform : correspondance pixel à pixel entre les entrées et la sortie
    IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L* outRect, D2D1_RECT_L* inRects, UINT32 count) const override {
        for (UINT32 k = 0; k < count; ++k) inRects[k] = *outRect;
        return S_OK;
    }
    IFACEMETHODIMP MapInputRectsToOutputRect(const D2D1_RECT_L* inRects, const D2D1_RECT_L*, UINT32 count, D2D1_RECT_L* outRect,
                                             D2D1_RECT_L* opaque) override {
        if (!count) return E_INVALIDARG;
        *outRect = inRects[0];  // les flous (bords prolongés) sont infinis : l'image donne l'étendue
        m_inRect = inRects[0];
        *opaque = D2D1::RectL(0, 0, 0, 0);
        return S_OK;
    }
    IFACEMETHODIMP MapInvalidRect(UINT32, D2D1_RECT_L, D2D1_RECT_L* out) const override {
        *out = m_inRect;
        return S_OK;
    }
    // ID2D1TransformNode
    IFACEMETHODIMP_(UINT32) GetInputCount() const override { return kInputs; }

private:
    DevelopEffect() = default;
    ~DevelopEffect() = default;
    LONG m_ref = 1;
    ComPtr<ID2D1EffectContext> m_ctx;
    ComPtr<ID2D1DrawInfo> m_draw;
    ComPtr<ID2D1ResourceTexture> m_lutTex;
    Constants m_const{};
    std::vector<float> m_lut;
    bool m_lutDirty = true;
    D2D1_RECT_L m_inRect{};
};

const wchar_t kEffectXml[] = LR"(<?xml version='1.0'?>
<Effect>
  <Property name='DisplayName' type='string' value='CyImgView Develop'/>
  <Property name='Author' type='string' value='CyImgView'/>
  <Property name='Category' type='string' value='Color'/>
  <Property name='Description' type='string' value='Develop'/>
  <Inputs>
    <Input name='Source'/>
    <Input name='Sharpen'/>
    <Input name='Fine'/>
    <Input name='Clarity'/>
    <Input name='Large'/>
  </Inputs>
  <Property name='Constants' type='blob'>
    <Property name='DisplayName' type='string' value='Constants'/>
  </Property>
  <Property name='Lut' type='blob'>
    <Property name='DisplayName' type='string' value='Lut'/>
  </Property>
</Effect>)";

}  // namespace

// =============================================================================================
// Réglages

const ParamDef& Def(int id) { return kDefs[std::clamp(id, 0, int(kParamCount) - 1)]; }

float AspectRatio(int aspect, float w, float h) {
    static const float r[kAspectCount] = {0, 0, 1, 4.0f / 3, 3.0f / 2, 16.0f / 9, 5.0f / 4, 7.0f / 5};
    if (aspect <= 0 || aspect >= kAspectCount) return 0;
    if (aspect == 1) return h > 0 ? w / h : 1;
    return w >= h ? r[aspect] : 1 / r[aspect];  // suit l'orientation de l'image
}

const wchar_t* AspectLabel(int aspect) {
    static const wchar_t* const l[kAspectCount] = {nullptr, nullptr, L"1:1", L"4:3", L"3:2", L"16:9", L"5:4", L"7:5"};
    return aspect >= 0 && aspect < kAspectCount ? l[aspect] : nullptr;
}

Params::Params() {
    for (int i = 0; i < kParamCount; ++i) v[i] = kDefs[i].def;
}

bool Params::ColorIdentity() const {
    for (int i = 0; i < kParamCount; ++i)
        if (i != kAngle && v[i] != kDefs[i].def) return false;
    for (const Curve& c : curve)
        if (!CurveIdentity(c)) return false;
    return true;
}

bool Params::GeoIdentity() const { return v[kAngle] == 0 && aspect == 0 && !cropSet; }

void Params::ResetGeometry() {
    v[kAngle] = 0;
    aspect = 0;
    aspectPortrait = false;
    cropSet = false;
    crop = D2D1::RectF(0, 0, 1, 1);
}

bool Params::SameColor(const Params& o) const {
    for (int i = 0; i < kParamCount; ++i)
        if (i != kAngle && v[i] != o.v[i]) return false;
    for (int k = 0; k < 4; ++k)
        if (!(curve[k] == o.curve[k]) && !(CurveIdentity(curve[k]) && CurveIdentity(o.curve[k]))) return false;
    return true;
}

bool Params::operator==(const Params& o) const {
    return SameColor(o) && v[kAngle] == o.v[kAngle] && aspect == o.aspect && aspectPortrait == o.aspectPortrait &&
           cropSet == o.cropSet && (!cropSet || memcmp(&crop, &o.crop, sizeof(crop)) == 0);
}

float EvalCurve(const Curve& c, float x) { return Spline(c)(x); }

void WhiteBalanceGains(float temp, float tint, float g[3]) {
    const float t = temp / 100, n = tint / 100;
    g[0] = std::exp2(0.8f * t);
    g[1] = std::exp2(-0.45f * n);
    g[2] = std::exp2(-0.8f * t);
    const float y = 0.2126f * g[0] + 0.7152f * g[1] + 0.0722f * g[2];
    for (int i = 0; i < 3; ++i) g[i] /= y;
}

void SolveWhiteBalance(float r, float g, float b, float& temp, float& tint) {
    r = std::max(r, 1e-4f);
    g = std::max(g, 1e-4f);
    b = std::max(b, 1e-4f);
    const float t = std::log2(b / r) / 1.6f;
    const float n = -(std::log2(r / g) + 0.8f * t) / 0.45f;
    temp = std::clamp(std::round(t * 100), -100.0f, 100.0f);
    tint = std::clamp(std::round(n * 100), -100.0f, 100.0f);
}

void AutoTone(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride, Params& p) {
    float g[3];
    WhiteBalanceGains(p.v[kTemp], p.v[kTint], g);
    std::vector<uint32_t> hist(1024, 0);  // luminance perceptuelle sur [0, 2]
    uint64_t n = 0;
    float lin[256];
    for (int i = 0; i < 256; ++i) lin[i] = Dec(i / 255.0f);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* px = bgra + size_t(y) * stride;
        for (uint32_t x = 0; x < w; ++x, px += 4) {
            if (px[3] < 128) continue;
            const float Y = 0.2126f * lin[px[2]] * g[0] + 0.7152f * lin[px[1]] * g[1] + 0.0722f * lin[px[0]] * g[2];
            hist[std::min<size_t>(1023, size_t(Enc(Y) * 512))]++;
            ++n;
        }
    }
    if (!n) return;
    auto pct = [&](double q, float shift) {  // percentile après une exposition de shift IL
        uint64_t acc = 0;
        const uint64_t target = uint64_t(q * double(n));
        for (int i = 0; i < 1024; ++i) {
            acc += hist[size_t(i)];
            if (acc > target) return Enc(Dec(i / 512.0f) * std::exp2(shift));
        }
        return Enc(Dec(2.0f) * std::exp2(shift));
    };
    auto above = [&](float lvl, float shift) {  // fraction des pixels au-dessus / au-dessous d'un niveau
        uint64_t acc = 0;
        for (int i = 0; i < 1024; ++i)
            if (Enc(Dec(i / 512.0f) * std::exp2(shift)) > lvl) acc += hist[size_t(i)];
        return float(double(acc) / double(n));
    };
    const float median = pct(0.5, 0);
    float ev = std::log2(Dec(0.46f) / std::max(1e-4f, Dec(median))) * 0.75f;
    ev = std::clamp(std::round(ev * 20) / 20, -2.5f, 2.5f);
    const float lo = pct(0.005, ev), hi = pct(0.995, ev);
    const float q1 = pct(0.25, ev), q3 = pct(0.75, ev);
    const float bright = above(0.9f, ev), dark = 1 - above(0.12f, ev);
    p.v[kExposure] = ev;
    p.v[kWhites] = hi < 0.97f ? std::min(60.0f, (0.97f - hi) * 250) : -std::min(60.0f, (hi - 0.97f) * 150);
    p.v[kBlacks] = lo > 0.03f ? -std::min(60.0f, (lo - 0.03f) * 300) : std::min(15.0f, (0.01f - std::min(lo, 0.01f)) * 800);
    p.v[kHighlights] = -std::min(70.0f, bright * 400);
    p.v[kShadows] = std::min(60.0f, dark * 300);
    p.v[kContrast] = std::clamp((0.3f - (q3 - q1)) * 150, -20.0f, 40.0f);
    for (int id : {kWhites, kBlacks, kHighlights, kShadows, kContrast}) p.v[id] = std::round(p.v[id]);
}

D2D1_RECT_F MaxCrop(float angleDeg, float ratio, float w, float h) {
    const float a = std::fabs(angleDeg) * kPi / 180, c = std::cos(a), s = std::sin(a);
    if (ratio <= 0) ratio = w / h;
    const float ch = std::min(w / (ratio * c + s), h / (ratio * s + c)), cw = ch * ratio;
    return D2D1::RectF((w - cw) / 2, (h - ch) / 2, (w + cw) / 2, (h + ch) / 2);
}

namespace {
bool CornersInside(float cx, float cy, float hw, float hh, float angleDeg, float w, float h) {
    const float a = angleDeg * kPi / 180, c = std::cos(a), s = std::sin(a);
    for (int k = 0; k < 4; ++k) {
        const float x = cx + ((k & 1) ? hw : -hw) - w / 2, y = cy + ((k & 2) ? hh : -hh) - h / 2;
        const float u = x * c + y * s, v = -x * s + y * c;
        if (std::fabs(u) > w / 2 + 0.01f || std::fabs(v) > h / 2 + 0.01f) return false;
    }
    return true;
}
}  // namespace

D2D1_RECT_F FitCrop(D2D1_RECT_F r, float angleDeg, float w, float h) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float hw = (r.right - r.left) / 2, hh = (r.bottom - r.top) / 2;
    if (!CornersInside(cx, cy, 0, 0, angleDeg, w, h)) {  // centre hors de l'image : ramené vers le centre
        float lo = 0, hi = 1;
        for (int i = 0; i < 24; ++i) {
            const float t = (lo + hi) / 2;
            if (CornersInside(cx + (w / 2 - cx) * t, cy + (h / 2 - cy) * t, 0, 0, angleDeg, w, h)) hi = t;
            else lo = t;
        }
        cx += (w / 2 - cx) * hi;
        cy += (h / 2 - cy) * hi;
    }
    if (!CornersInside(cx, cy, hw, hh, angleDeg, w, h)) {
        float lo = 0, hi = 1;
        for (int i = 0; i < 24; ++i) {
            const float k = (lo + hi) / 2;
            if (CornersInside(cx, cy, hw * k, hh * k, angleDeg, w, h)) lo = k;
            else hi = k;
        }
        hw *= lo;
        hh *= lo;
    }
    return D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
}

D2D1_RECT_F CropRect(const Params& p, float w, float h) {
    if (!p.cropSet) {
        float ratio = AspectRatio(p.aspect, w, h);
        if (ratio > 0 && p.aspectPortrait) ratio = 1 / ratio;
        return MaxCrop(p.v[kAngle], ratio, w, h);
    }
    return D2D1::RectF(p.crop.left * w, p.crop.top * h, p.crop.right * w, p.crop.bottom * h);
}

void ComputeHistogram(const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride, Histogram& out) {
    out = Histogram();
    uint64_t lowN = 0, highN = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* px = bgra + size_t(y) * stride;
        for (uint32_t x = 0; x < w; ++x, px += 4) {
            const uint32_t a = px[3];
            if (a < 128) continue;
            uint32_t c[3] = {px[2], px[1], px[0]};
            if (a < 255)
                for (uint32_t& v : c) v = std::min<uint32_t>(255, (v * 255 + a / 2) / a);
            for (int k = 0; k < 3; ++k) out.bins[k][c[k]]++;
            if (c[0] >= 254 || c[1] >= 254 || c[2] >= 254) ++highN;
            if (c[0] <= 1 && c[1] <= 1 && c[2] <= 1) ++lowN;
            ++out.total;
        }
    }
    if (out.total) {
        out.clipLow = float(double(lowN) / double(out.total));
        out.clipHigh = float(double(highN) / double(out.total));
        out.valid = true;
    }
}

std::wstring SerializeSection(const Params& p) {
    std::wstring s;
    wchar_t b[96];
    for (int i = 0; i < kParamCount; ++i) {
        if (i == kAngle) continue;
        swprintf_s(b, L"%hs=%g", kDefs[i].ini, double(p.v[i]));
        s += b;
        s.push_back(L'\0');
    }
    for (int k = 0; k < 4; ++k) {
        swprintf_s(b, L"%hs=", kCurveIni[k]);
        s += b;
        for (size_t i = 0; i < p.curve[k].size(); ++i) {
            swprintf_s(b, L"%ls%.4f,%.4f", i ? L";" : L"", double(p.curve[k][i].x), double(p.curve[k][i].y));
            s += b;
        }
        s.push_back(L'\0');
    }
    s.push_back(L'\0');
    return s;
}

bool ParseSection(const wchar_t* section, Params& p) {
    if (!section || !*section) return false;
    Params out;
    bool any = false;
    for (const wchar_t* line = section; *line; line += wcslen(line) + 1) {
        const wchar_t* eq = wcschr(line, L'=');
        if (!eq) continue;
        std::string key;
        for (const wchar_t* c = line; c < eq; ++c) key.push_back(char(*c));
        const wchar_t* val = eq + 1;
        for (int i = 0; i < kParamCount; ++i) {
            if (i == kAngle || key != kDefs[i].ini) continue;
            out.v[i] = std::clamp(float(_wtof(val)), kDefs[i].min, kDefs[i].max);
            any = true;
        }
        for (int k = 0; k < 4; ++k) {
            if (key != kCurveIni[k]) continue;
            Curve c;
            for (const wchar_t* q = val; *q;) {
                wchar_t* end = nullptr;
                const float x = float(wcstod(q, &end));
                if (end == q || *end != L',') break;
                q = end + 1;
                const float y = float(wcstod(q, &end));
                if (end == q) break;
                c.push_back({Clamp01(x), Clamp01(y)});
                q = *end == L';' ? end + 1 : end;
                if (*end != L';') break;
            }
            std::sort(c.begin(), c.end(), [](const Pt& a, const Pt& b) { return a.x < b.x; });
            if (c.size() >= 2) out.curve[k] = std::move(c);
            any = true;
        }
    }
    if (!any) return false;
    out.ResetGeometry();
    out.v[kAngle] = p.v[kAngle];  // géométrie propre à chaque image : conservée
    out.aspect = p.aspect;
    out.aspectPortrait = p.aspectPortrait;
    out.cropSet = p.cropSet;
    out.crop = p.crop;
    p = std::move(out);
    return true;
}

// =============================================================================================
// Rendu GPU

bool RegisterEffect(ID2D1Factory1* factory) {
    static ID2D1Factory1* registered = nullptr;
    if (!factory) return false;
    if (registered == factory) return true;
    const D2D1_PROPERTY_BINDING bindings[] = {
        {L"Constants", &DevelopEffect::SetConst, &DevelopEffect::GetConst},
        {L"Lut", &DevelopEffect::SetLut, &DevelopEffect::GetLut},
    };
    if (FAILED(factory->RegisterEffectFromString(kClsidDevelop, kEffectXml, bindings, ARRAYSIZE(bindings),
                                                 &DevelopEffect::Create)))
        return false;
    registered = factory;
    return true;
}

void Graph::Reset() {
    m_border.Reset();
    for (auto& b : m_blur) b.Reset();
    m_dev.Reset();
    m_input = nullptr;
    for (auto& w : m_wired) w = nullptr;
    for (float& s : m_sigma) s = 0;
    m_lastConst.clear();
    m_lutKey = ~0ull;
}

bool Graph::Init(ID2D1DeviceContext* dc, bool cacheBlurs) {
    Reset();
    if (!dc || FAILED(dc->CreateEffect(kClsidDevelop, &m_dev)) || FAILED(dc->CreateEffect(CLSID_D2D1Border, &m_border))) {
        Reset();
        return false;
    }
    m_dev->SetValue(D2D1_PROPERTY_PRECISION, D2D1_BUFFER_PRECISION_16BPC_FLOAT);
    m_border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_CLAMP);
    m_border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_CLAMP);
    ComPtr<ID2D1Image> bordered;
    m_border->GetOutput(&bordered);
    for (int k = 0; k < 4; ++k) {
        if (FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &m_blur[k]))) {
            Reset();
            return false;
        }
        m_blur[k]->SetInput(0, bordered.Get());
        m_blur[k]->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        m_blur[k]->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                            k >= 2 ? D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED : D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
        m_blur[k]->SetValue(D2D1_PROPERTY_PRECISION, D2D1_BUFFER_PRECISION_16BPC_FLOAT);
        if (cacheBlurs) m_blur[k]->SetValue(D2D1_PROPERTY_CACHED, TRUE);
    }
    return true;
}

ComPtr<ID2D1Image> Graph::Build(ID2D1Image* input, const Params& p, const View& v) {
    ComPtr<ID2D1Image> out;
    if (!m_dev || !input) return out;
    if (input != m_input) {
        m_border->SetInput(0, input);
        m_input = input;
    }
    // Flous : rayons en pixels de l'image d'origine, ramenés à l'échelle de l'entrée
    const float L = v.longSide;
    const bool sharp = p.v[kSharpAmount] > 0;
    const float sigma[4] = {sharp ? p.v[kSharpRadius] : 1.0f, std::max(1.5f, L / 1500), std::max(4.0f, L / 120),
                            std::max(10.0f, L / 30)};
    const bool need[4] = {sharp || p.v[kNrLuma] > 0, p.v[kTexture] != 0 || p.v[kNrColor] > 0 || (sharp && p.v[kSharpMasking] > 0),
                          p.v[kClarity] != 0, p.v[kShadows] != 0 || p.v[kHighlights] != 0 || p.v[kDehaze] != 0};
    auto wire = [&](UINT32 idx, ID2D1Image* img) {
        if (m_wired[idx] == img) return;
        m_dev->SetInput(idx, img);
        m_wired[idx] = img;
    };
    wire(0, input);
    for (int k = 0; k < 4; ++k) {
        const float s = std::min(250.0f, sigma[k] * v.scale);
        if (!need[k] || s < 0.35f) {
            wire(UINT32(k + 1), input);
            continue;
        }
        if (s != m_sigma[k]) {
            m_blur[k]->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, s);
            m_sigma[k] = s;
        }
        ComPtr<ID2D1Image> b;
        m_blur[k]->GetOutput(&b);
        wire(UINT32(k + 1), b.Get());
    }
    const Constants k = MakeConstants(p, v);
    const uint8_t* kb = reinterpret_cast<const uint8_t*>(&k);
    if (m_lastConst.size() != sizeof(k) || memcmp(m_lastConst.data(), kb, sizeof(k)) != 0) {
        m_dev->SetValue(0, D2D1_PROPERTY_TYPE_BLOB, kb, sizeof(k));
        m_lastConst.assign(kb, kb + sizeof(k));
    }
    const uint64_t key = LutKey(p);
    if (key != m_lutKey) {
        std::vector<float> lut;
        BuildLut(p, lut);
        m_dev->SetValue(1, D2D1_PROPERTY_TYPE_BLOB, reinterpret_cast<const BYTE*>(lut.data()), UINT32(lut.size() * 4));
        m_lutKey = key;
    }
    m_dev->GetOutput(&out);
    return out;
}

}  // namespace dev
