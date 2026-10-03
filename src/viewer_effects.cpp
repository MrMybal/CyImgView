// Mode édition : effets (luminosité, teinte, flou...) appliqués au calque actif, limités à la
// sélection, avec aperçu en direct ; et valeurs des curseurs (outils, calque, effets).
//
// Les effets sont des graphes d'effets Direct2D intégrés à Windows 10. Les réglages de couleur
// (luminosité, contraste, teinte, saturation, niveaux de gris, sépia, négatif) sont combinés en une
// seule matrice de couleur. L'aperçu est rendu dans une texture à chaque changement de réglage ;
// « Appliquer » la substitue au calque (l'ancienne part dans l'historique, sans copie).

#include "viewer.h"
#include "viewer_util.h"
#include "i18n.h"

#include <d2d1effects_2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ui;

namespace {

enum ValFmt { kInt, kSigned, kPx, kDeg, kPct, kEv };
struct FxParam {
    const char* key;
    float min, max, def;
    int fmt;
    bool sq;  // curseur quadratique (précision dans les petites valeurs)
};
struct FxDef {
    const char* key;
    int count;
    FxParam p[3];
};
enum FxId { kBrightContrast, kHueSat, kExposure, kGray, kSepia, kInvert, kBlur, kSharpen, kPixelate, kPosterize,
            kVignette, kEmboss, kTemperature };
const FxDef kFx[] = {
    {"edit.fx.brightness_contrast", 2, {{"edit.fx.p.brightness", -100, 100, 0, kSigned, false}, {"edit.fx.p.contrast", -100, 100, 0, kSigned, false}}},
    {"edit.fx.hue_saturation", 3, {{"edit.fx.p.hue", -180, 180, 0, kDeg, false}, {"edit.fx.p.saturation", -100, 100, 0, kSigned, false},
                                   {"edit.fx.p.lightness", -100, 100, 0, kSigned, false}}},
    {"edit.fx.exposure", 1, {{"edit.fx.p.exposure", -200, 200, 0, kEv, false}}},
    {"edit.fx.grayscale", 1, {{"edit.fx.p.intensity", 0, 100, 100, kPct, false}}},
    {"edit.fx.sepia", 1, {{"edit.fx.p.intensity", 0, 100, 100, kPct, false}}},
    {"edit.fx.invert", 1, {{"edit.fx.p.intensity", 0, 100, 100, kPct, false}}},
    {"edit.fx.blur", 1, {{"edit.fx.p.radius", 0, 250, 8, kPx, true}}},
    {"edit.fx.sharpen", 2, {{"edit.fx.p.amount", 0, 300, 100, kPct, false}, {"edit.fx.p.radius", 0.5f, 50, 2, kPx, true}}},
    {"edit.fx.pixelate", 1, {{"edit.fx.p.block", 2, 200, 16, kPx, true}}},
    {"edit.fx.posterize", 1, {{"edit.fx.p.levels", 2, 16, 4, kInt, false}}},
    {"edit.fx.vignette", 2, {{"edit.fx.p.strength", 0, 100, 60, kPct, false}, {"edit.fx.p.size", 0, 100, 50, kPct, false}}},
    {"edit.fx.emboss", 2, {{"edit.fx.p.height", 1, 100, 10, kInt, false}, {"edit.fx.p.direction", 0, 360, 135, kDeg, false}}},
    {"edit.fx.temperature", 2, {{"edit.fx.p.temperature", -100, 100, 0, kSigned, false}, {"edit.fx.p.tint", -100, 100, 0, kSigned, false}}},
};

// Transformation affine des couleurs : sortie = m · entrée + o (RVB, alpha inchangé)
struct ColorAff {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float o[3] = {0, 0, 0};
};
ColorAff Then(const ColorAff& a, const ColorAff& b) {  // a puis b
    ColorAff r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r.m[i][j] = b.m[i][0] * a.m[0][j] + b.m[i][1] * a.m[1][j] + b.m[i][2] * a.m[2][j];
        r.o[i] = b.m[i][0] * a.o[0] + b.m[i][1] * a.o[1] + b.m[i][2] * a.o[2] + b.o[i];
    }
    return r;
}
ColorAff Mix(const ColorAff& fx, float k) {  // identité -> fx selon k (intensité)
    ColorAff id, r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) r.m[i][j] = id.m[i][j] + (fx.m[i][j] - id.m[i][j]) * k;
        r.o[i] = fx.o[i] * k;
    }
    return r;
}
ColorAff FromRows(const float rows[3][3]) {
    ColorAff a;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) a.m[i][j] = rows[i][j];
    return a;
}
// Matrice Direct2D : ligne = canal d'entrée, colonne = canal de sortie
D2D1_MATRIX_5X4_F ToD2D(const ColorAff& a) {
    D2D1_MATRIX_5X4_F m{};
    float* f = &m._11;
    for (int in = 0; in < 3; ++in)
        for (int out = 0; out < 3; ++out) f[in * 4 + out] = a.m[out][in];
    m._44 = 1;
    m._51 = a.o[0];
    m._52 = a.o[1];
    m._53 = a.o[2];
    return m;
}

}  // namespace

int Viewer::EffectCount() { return int(ARRAYSIZE(kFx)); }
const char* Viewer::EffectKey(int fx) { return kFx[std::clamp(fx, 0, EffectCount() - 1)].key; }

// =============================================================================================
// Curseurs : position 0..1 <-> valeur

float Viewer::SliderT(int id) const {
    switch (id) {
    case kSlSize:
        return m_edit.tool == Tool::Text ? std::log(m_edit.fontSize / 8.0f) / std::log(50.0f) : std::log(m_edit.size) / std::log(200.0f);
    case kSlOpacity: return (m_edit.opacity - 1) / 99.0f;
    case kSlHardness: return m_edit.hardness / 100.0f;
    case kSlLayerOpacity: return m_edit.layers.empty() ? 1.0f : m_edit.layers[size_t(m_edit.layer)].opacity;
    default: break;
    }
    if (id >= kSlPlugTool0) return PluginSliderT(true, id - kSlPlugTool0);
    const int k = id - kSlFx0;
    if (m_edit.fx >= EffectCount()) return PluginSliderT(false, k);
    if (m_edit.fx < 0 || k < 0 || k >= kFx[m_edit.fx].count) return 0;
    const FxParam& p = kFx[m_edit.fx].p[k];
    const float t = (m_edit.fxVal[k] - p.min) / (p.max - p.min);
    return p.sq ? std::sqrt(std::max(0.0f, t)) : t;
}

void Viewer::SetSliderT(int id, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (id) {
    case kSlSize:
        if (m_edit.tool == Tool::Text) m_edit.fontSize = std::round(8.0f * std::pow(50.0f, t));  // 8..400
        else m_edit.size = std::max(1.0f, std::round(std::pow(200.0f, t)));                    // 1..200
        return;
    case kSlOpacity: m_edit.opacity = std::round(1 + 99 * t); return;
    case kSlHardness: m_edit.hardness = std::round(100 * t); return;
    case kSlLayerOpacity:
        if (!m_edit.layers.empty()) {
            CurLayer().opacity = std::round(100 * t) / 100;
            m_edit.dirty = true;
        }
        return;
    default: break;
    }
    if (id >= kSlPlugTool0) return SetPluginSliderT(true, id - kSlPlugTool0, t);
    const int k = id - kSlFx0;
    if (m_edit.fx >= EffectCount()) return SetPluginSliderT(false, k, t);
    if (m_edit.fx < 0 || k < 0 || k >= kFx[m_edit.fx].count) return;
    const FxParam& p = kFx[m_edit.fx].p[k];
    float v = p.min + (p.max - p.min) * (p.sq ? t * t : t);
    v = p.fmt == kPx && v < 10 ? std::round(v * 10) / 10 : std::round(v);
    if (v != m_edit.fxVal[k]) {
        m_edit.fxVal[k] = v;
        m_edit.fxDirty = true;
    }
}

// =============================================================================================
// Effets

void Viewer::OpenEffect(int fx) {
    if (m_edit.textActive) CommitText();
    if (m_edit.fx >= 0) CloseEffect(false);
    m_edit.menu = EditMenu::None;
    if (fx >= EffectCount()) return OpenPluginEffect(fx - EffectCount());  // effet d'un plugin
    if (fx < 0 || m_edit.layers.empty()) return;
    m_edit.fx = fx;
    for (int k = 0; k < 3; ++k) m_edit.fxVal[k] = kFx[fx].p[k].def;
    // flou : rayon par défaut proportionnel à la définition (8 px est invisible sur 24 Mpx)
    if (fx == kBlur) m_edit.fxVal[0] = std::clamp(std::round(std::max(m_edit.w, m_edit.h) / 600.0f), 2.0f, 50.0f);
    m_edit.fxPreview = true;
    m_edit.fxDirty = true;
    Invalidate();
}

void Viewer::CloseEffect(bool apply) {
    if (m_edit.fx < 0) return;
    if (m_edit.fx >= EffectCount()) return ClosePluginEffect(apply);
    if (apply) {
        if (m_edit.fxDirty || !m_edit.fxOut) UpdateFxPreview();
        if (m_edit.fxOut) PushLayerSwap(std::move(m_edit.fxOut));
    }
    m_edit.fx = -1;
    m_edit.fxOut.Reset();
    m_edit.fxDirty = false;
    Invalidate();
}

ComPtr<ID2D1Image> Viewer::BuildEffect(ID2D1Image* input, std::vector<ComPtr<ID2D1Effect>>& keep) {
    ComPtr<ID2D1Image> cur = input;
    auto add = [&](REFCLSID id) -> ID2D1Effect* {
        ComPtr<ID2D1Effect> e;
        if (FAILED(m_dc->CreateEffect(id, &e))) return nullptr;
        keep.push_back(e);
        return e.Get();
    };
    auto chain = [&](ID2D1Effect* e) {
        if (!e) return;
        e->SetInput(0, cur.Get());
        cur.Reset();
        e->GetOutput(&cur);
    };
    auto matrix = [&](const ColorAff& a) {
        ID2D1Effect* e = add(CLSID_D2D1ColorMatrix);
        if (!e) return;
        e->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, ToD2D(a));
        e->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
        chain(e);
    };
    auto border = [&] {  // bords prolongés : pas de halo transparent sur les côtés (flou, relief)
        if (ID2D1Effect* e = add(CLSID_D2D1Border)) {
            e->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_CLAMP);
            e->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_CLAMP);
            chain(e);
        }
    };
    const float* v = m_edit.fxVal;
    switch (m_edit.fx) {
    case kBrightContrast: {
        const float c = v[1] / 100, k = c >= 0 ? 1 + 2 * c : 1 + c;
        const float o = 0.5f * (1 - k) + v[0] / 100 * 0.5f;
        ColorAff a;
        for (int i = 0; i < 3; ++i) {
            a.m[i][i] = k;
            a.o[i] = o;
        }
        matrix(a);
        break;
    }
    case kHueSat: {
        const float ang = v[0] * 3.14159265f / 180, cs = std::cos(ang), sn = std::sin(ang);
        const float hue[3][3] = {{0.213f + cs * 0.787f - sn * 0.213f, 0.715f - cs * 0.715f - sn * 0.715f, 0.072f - cs * 0.072f + sn * 0.928f},
                                 {0.213f - cs * 0.213f + sn * 0.143f, 0.715f + cs * 0.285f + sn * 0.140f, 0.072f - cs * 0.072f - sn * 0.283f},
                                 {0.213f - cs * 0.213f - sn * 0.787f, 0.715f - cs * 0.715f + sn * 0.715f, 0.072f + cs * 0.928f + sn * 0.072f}};
        const float s = 1 + v[1] / 100;
        const float sat[3][3] = {{0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s},
                                 {0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s},
                                 {0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s}};
        ColorAff light;
        const float l = v[2] / 100;
        for (int i = 0; i < 3; ++i) {
            light.m[i][i] = l >= 0 ? 1 - l : 1 + l;
            light.o[i] = l >= 0 ? l : 0;
        }
        matrix(Then(Then(FromRows(hue), FromRows(sat)), light));
        break;
    }
    case kExposure:
        if (ID2D1Effect* e = add(CLSID_D2D1Exposure)) {
            e->SetValue(D2D1_EXPOSURE_PROP_EXPOSURE_VALUE, v[0] / 100);
            chain(e);
        }
        break;
    case kGray: {
        const float g[3][3] = {{0.299f, 0.587f, 0.114f}, {0.299f, 0.587f, 0.114f}, {0.299f, 0.587f, 0.114f}};
        matrix(Mix(FromRows(g), v[0] / 100));
        break;
    }
    case kSepia: {
        const float sp[3][3] = {{0.393f, 0.769f, 0.189f}, {0.349f, 0.686f, 0.168f}, {0.272f, 0.534f, 0.131f}};
        matrix(Mix(FromRows(sp), v[0] / 100));
        break;
    }
    case kInvert: {
        ColorAff a;
        for (int i = 0; i < 3; ++i) {
            a.m[i][i] = -1;
            a.o[i] = 1;
        }
        matrix(Mix(a, v[0] / 100));
        break;
    }
    case kBlur:
        if (v[0] <= 0) break;
        border();
        if (ID2D1Effect* e = add(CLSID_D2D1GaussianBlur)) {
            e->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, v[0]);
            e->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            chain(e);
        }
        break;
    case kSharpen: {  // masque flou : (1 + k) · original - k · flou
        ComPtr<ID2D1Image> orig = cur;
        border();
        ID2D1Effect* blur = add(CLSID_D2D1GaussianBlur);
        ID2D1Effect* ar = add(CLSID_D2D1ArithmeticComposite);
        if (!blur || !ar) break;
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, v[1]);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        blur->SetInput(0, cur.Get());
        ComPtr<ID2D1Image> blurred;
        blur->GetOutput(&blurred);
        const float k = v[0] / 100;
        ar->SetValue(D2D1_ARITHMETICCOMPOSITE_PROP_COEFFICIENTS, D2D1::Vector4F(0, 1 + k, -k, 0));
        ar->SetValue(D2D1_ARITHMETICCOMPOSITE_PROP_CLAMP_OUTPUT, TRUE);
        ar->SetInput(0, orig.Get());     // coefficient C2
        ar->SetInput(1, blurred.Get());  // coefficient C3
        cur.Reset();
        ar->GetOutput(&cur);
        break;
    }
    case kPixelate: {  // réduction (moyenne) puis agrandissement au plus proche voisin
        const float b = std::max(2.0f, std::round(v[0]));
        ID2D1Effect* down = add(CLSID_D2D1Scale);
        ID2D1Effect* up = add(CLSID_D2D1Scale);
        if (!down || !up) break;
        down->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(1 / b, 1 / b));
        down->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        down->SetValue(D2D1_SCALE_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        chain(down);
        up->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(b, b));
        up->SetValue(D2D1_SCALE_PROP_INTERPOLATION_MODE, D2D1_SCALE_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        up->SetValue(D2D1_SCALE_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        chain(up);
        break;
    }
    case kPosterize:
        if (ID2D1Effect* e = add(CLSID_D2D1Posterize)) {
            const UINT32 n = UINT32(std::clamp(v[0], 2.0f, 16.0f));
            e->SetValue(D2D1_POSTERIZE_PROP_RED_VALUE_COUNT, n);
            e->SetValue(D2D1_POSTERIZE_PROP_GREEN_VALUE_COUNT, n);
            e->SetValue(D2D1_POSTERIZE_PROP_BLUE_VALUE_COUNT, n);
            chain(e);
        }
        break;
    case kVignette:
        if (ID2D1Effect* e = add(CLSID_D2D1Vignette)) {
            e->SetValue(D2D1_VIGNETTE_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 1));
            e->SetValue(D2D1_VIGNETTE_PROP_STRENGTH, v[0] / 100);
            e->SetValue(D2D1_VIGNETTE_PROP_TRANSITION_SIZE, v[1] / 100);
            chain(e);
        }
        break;
    case kEmboss:
        border();
        if (ID2D1Effect* e = add(CLSID_D2D1Emboss)) {
            e->SetValue(D2D1_EMBOSS_PROP_HEIGHT, v[0] / 10);
            e->SetValue(D2D1_EMBOSS_PROP_DIRECTION, v[1]);
            chain(e);
        }
        break;
    case kTemperature:
        if (ID2D1Effect* e = add(CLSID_D2D1TemperatureTint)) {
            e->SetValue(D2D1_TEMPERATUREANDTINT_PROP_TEMPERATURE, v[0] / 100);
            e->SetValue(D2D1_TEMPERATUREANDTINT_PROP_TINT, v[1] / 100);
            chain(e);
        }
        break;
    default: break;
    }
    return cur;
}

// Aperçu : copie du calque, effet dessiné par-dessus dans la sélection (ou partout)
void Viewer::UpdateFxPreview() {
    if (m_edit.fx >= EffectCount()) return UpdatePluginFxPreview();
    m_edit.fxDirty = false;
    if (m_edit.fx < 0 || m_edit.layers.empty()) return;
    if (!m_edit.fxOut || m_edit.fxOut->GetPixelSize().width != m_edit.w || m_edit.fxOut->GetPixelSize().height != m_edit.h)
        m_edit.fxOut = NewCanvas(m_edit.w, m_edit.h);
    if (!m_edit.fxOut) {
        ShowToast(T("edit.err.vram"));
        m_edit.fx = -1;
        return;
    }
    std::vector<ComPtr<ID2D1Effect>> keep;
    ID2D1Bitmap1* src = CurLayer().bmp.Get();
    ComPtr<ID2D1Image> img = BuildEffect(src, keep);
    const D2D1_RECT_F doc = D2D1::RectF(0, 0, float(m_edit.w), float(m_edit.h));
    DrawOn(m_edit.fxOut.Get(), [&] {
        m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (m_edit.selValid) {
            m_dc->DrawBitmap(src, doc, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
            m_dc->PushAxisAlignedClip(m_edit.sel, D2D1_ANTIALIAS_MODE_ALIASED);
            m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        }
        if (img) m_dc->DrawImage(img.Get(), D2D1::Point2F(0, 0), doc, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
        if (m_edit.selValid) m_dc->PopAxisAlignedClip();
    });
    m_edit.fxVer = ++m_edit.verSeq;
}

// Panneau de réglages (à la place du panneau Calques)
void Viewer::DrawEffectPanel(float cw, float ch) {
    if (m_edit.fx < 0) return;
    if (m_edit.fx >= EffectCount()) return DrawPluginEffectPanel(cw, ch);
    const float s = m_scale, pad = 12 * s;
    const FxDef& d = kFx[m_edit.fx];
    const D2D1_RECT_F full = LayersPanelRect(cw, ch);
    float height = 46 * s + (m_edit.selValid ? 22 * s : 0) + d.count * 54 * s + 34 * s + 54 * s;
    const D2D1_RECT_F p = D2D1::RectF(full.left, full.top, full.right, std::min(full.bottom, full.top + height));
    FillRound(p, 8 * s, kPanel, 0.97f);
    SetColor(kAccent, 0.35f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(p, 8 * s, 8 * s), m_brush.Get(), 1);
    EditBtn(EditCmd::Panel, 0, p, nullptr);
    Text(T(d.key), m_tfTitle.Get(), D2D1::RectF(p.left + pad, p.top + 8 * s, p.right - pad, p.top + 34 * s), kAccent, 1);
    float y = p.top + 42 * s;
    if (m_edit.selValid) {
        Text(T("edit.fx.selection_note"), m_tfSmall.Get(), D2D1::RectF(p.left + pad, y, p.right - pad, y + 20 * s), kGrey, 0.9f);
        y += 22 * s;
    }
    for (int k = 0; k < d.count; ++k) {
        const FxParam& prm = d.p[k];
        const float v = m_edit.fxVal[k];
        wchar_t vb[32];
        switch (prm.fmt) {
        case kSigned: swprintf_s(vb, L"%+.0f", v); break;
        case kPx: swprintf_s(vb, v < 10 ? L"%.1f px" : L"%.0f px", v); break;
        case kDeg: swprintf_s(vb, L"%.0f°", v); break;
        case kPct: swprintf_s(vb, L"%.0f %%", v); break;
        case kEv: swprintf_s(vb, L"%+.2f EV", v / 100); break;
        default: swprintf_s(vb, L"%.0f", v); break;
        }
        Text(T(prm.key), m_tfSmall.Get(), D2D1::RectF(p.left + pad, y, p.right - pad - 70 * s, y + 22 * s), kWhite, 0.95f);
        Text(vb, m_tfSmall.Get(), D2D1::RectF(p.right - pad - 70 * s, y, p.right - pad, y + 22 * s), kAccent, 1);
        DrawSlider(kSlFx0 + k, D2D1::RectF(p.left + pad, y + 24 * s, p.right - pad, y + 42 * s), T(prm.key));
        y += 54 * s;
    }
    // Aperçu (case à cocher) et Réinitialiser
    const D2D1_RECT_F pv = D2D1::RectF(p.left + pad - 4 * s, y, p.left + pad + 110 * s, y + 26 * s);
    EditBtn(EditCmd::FxPreview, 0, pv, T("edit.tip.fx_preview"));
    const D2D1_RECT_F box = D2D1::RectF(pv.left + 4 * s, y + 5 * s, pv.left + 20 * s, y + 21 * s);
    if (m_edit.fxPreview) FillRound(box, 3 * s, kAccent, 1);
    SetColor(m_edit.fxPreview ? kAccent : kWhite, m_edit.fxPreview ? 1.0f : 0.6f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3 * s, 3 * s), m_brush.Get(), 1.5f * s);
    if (m_edit.fxPreview) Text(L"", m_tfIcon.Get(), box, 0x0B1A2A, 1);
    Text(T("edit.fx.preview"), m_tfSmall.Get(), D2D1::RectF(box.right + 8 * s, y, pv.right, y + 26 * s), kWhite, 0.95f);
    const D2D1_RECT_F rs = D2D1::RectF(p.right - pad - 110 * s, y, p.right - pad + 4 * s, y + 26 * s);
    EditBtn(EditCmd::FxReset, 0, rs, T("edit.tip.fx_reset"));
    Text(T("edit.fx.reset"), m_tfSmall.Get(), D2D1::RectF(rs.left, rs.top, rs.right - 6 * s, rs.bottom), kGrey, 1);
    y += 34 * s;
    // Appliquer / Annuler
    const float bw = (p.right - p.left - 2 * pad - 8 * s) / 2;
    const D2D1_RECT_F ok = D2D1::RectF(p.left + pad, y, p.left + pad + bw, y + 34 * s);
    const D2D1_RECT_F no = D2D1::RectF(ok.right + 8 * s, y, p.right - pad, y + 34 * s);
    const bool hOk = EditBtn(EditCmd::FxApply, 0, ok, T("edit.tip.fx_apply"));
    FillRound(ok, 6 * s, kAccent, hOk ? 1.0f : 0.85f);
    Text(T("edit.fx.apply"), m_tfCenter.Get(), ok, 0x0B1A2A, 1);
    EditBtn(EditCmd::FxCancel, 0, no, T("edit.tip.fx_cancel"));
    SetColor(kWhite, 0.25f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(no, 6 * s, 6 * s), m_brush.Get(), 1);
    Text(T("edit.fx.cancel"), m_tfCenter.Get(), no, kWhite, 0.95f);
}
