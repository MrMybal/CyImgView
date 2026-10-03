// Mode Développement (onglet du mode édition) : réglages photo du calque actif, à la manière d'un
// développeur de fichiers RAW (balance des blancs, tonalité, courbes, TSL, étalonnage, détail, effets,
// redressement et recadrage).
//
// Non destructif tant qu'on reste dans le mode : les réglages sont des paramètres (dev::Params) et
// l'aperçu est recalculé sur le GPU à partir des pixels d'origine du calque (develop_effect.cpp).
// Pendant un réglage, l'aperçu est calculé à la résolution de l'écran (image réduite en 16 bits
// flottants, flous mis en cache) ; à l'arrêt, un rendu en pleine résolution le remplace. Le résultat est
// intégré au calque (une entrée d'historique, plus une pour la géométrie qui touche tous les calques)
// quand on revient en Retouche ou qu'on termine ; « Enregistrer sous » et « Copier » utilisent un
// document complété le temps de l'opération, sans quitter le mode. Annuler / rétablir agit ici sur les
// réglages.

#include "viewer_modes.h"
#include "develop_effect.h"
#include "viewer_util.h"
#include "i18n.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ui;
using dev::Params;

namespace {

constexpr UINT_PTR kTimerDevelop = 0x4456;  // rendu complet et histogramme, peu après le dernier réglage
constexpr double kFullDelay = 0.35;
constexpr float kPi = 3.14159265f;

enum DevTool { kToolNone, kToolWb, kToolStraighten, kToolCrop };
enum Section { kSecBasic, kSecCurve, kSecHsl, kSecGrading, kSecDetail, kSecEffects, kSecGeometry, kSecCount };
const char* const kSectionKeys[kSecCount] = {"develop.sec.basic",   "develop.sec.curve",  "develop.sec.hsl",
                                             "develop.sec.grading", "develop.sec.detail", "develop.sec.effects",
                                             "develop.sec.geometry"};
const char* const kColorKeys[8] = {"develop.color.red",  "develop.color.orange", "develop.color.yellow",
                                   "develop.color.green", "develop.color.aqua",  "develop.color.blue",
                                   "develop.color.purple", "develop.color.magenta"};
const float kHueDeg[8] = {0, 30, 60, 120, 180, 240, 270, 300};

// Contrôles du panneau (argument de EditCmd::ModeCtl) ; 0 .. kParamCount-1 : curseurs
enum Ctl {
    kCtlAuto = 1000, kCtlWbTool, kCtlStraighten, kCtlCropTool, kCtlResetAll, kCtlPrevious, kCtlCopy, kCtlPaste,
    kCtlSplit, kCtlOriginal, kCtlClip, kCtlUndo, kCtlRedo, kCtlCurveReset, kCtlGeoReset, kCtlAspectFlip, kCtlScrollbar,
    kCtlAspect0 = 1100, kCtlCurveCh0 = 1200, kCtlHslTab0 = 1300, kCtlSection0 = 1400, kCtlCurve = 1500,
};
enum Track { kTrackPlain, kTrackTemp, kTrackTint, kTrackRainbow, kTrackHslHue, kTrackHslSat, kTrackHslLum, kTrackSatOf };
enum Drag { kDragNone, kDragSlider, kDragCurve, kDragStraighten, kDragCropNew, kDragCropMove, kDragCropCorner, kDragScroll };

void CALLBACK OnDevelopTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    InvalidateRect(hwnd, nullptr, FALSE);
}

std::wstring LocalDecimal(std::wstring s) {
    const wchar_t* sep = T("viewer.decimal");
    if (sep[0] && !sep[1]) std::replace(s.begin(), s.end(), L'.', sep[0]);
    return s;
}

std::wstring FormatValue(int id, float v) {
    wchar_t b[32];
    if (std::fabs(v) < 1e-4f) v = 0;
    switch (dev::Def(id).fmt) {
    case dev::kFmtSigned: swprintf_s(b, v > 0 ? L"+%.0f" : L"%.0f", double(v)); break;
    case dev::kFmtEv: swprintf_s(b, v > 0 ? L"+%.2f" : L"%.2f", double(v)); break;
    case dev::kFmtDeg: swprintf_s(b, L"%.0f°", double(v)); break;
    case dev::kFmtPx: swprintf_s(b, L"%.1f", double(v)); break;
    case dev::kFmtAngle: swprintf_s(b, v > 0 ? L"+%.1f°" : L"%.1f°", double(v)); break;
    default: swprintf_s(b, L"%.0f", double(v)); break;
    }
    return LocalDecimal(b);
}

float Quantize(int id, float v) {
    const dev::ParamDef& d = dev::Def(id);
    const float q = d.fmt == dev::kFmtEv ? 0.01f : (d.fmt == dev::kFmtPx || d.fmt == dev::kFmtAngle) ? 0.1f : 1.0f;
    return std::clamp(std::round(v / q) * q, d.min, d.max);
}

uint32_t HueColor(float deg, float sat = 1, float val = 1) {
    const float h = std::fmod(std::fmod(deg, 360.0f) + 360.0f, 360.0f) / 60;
    const float c = val * sat, x = c * (1 - std::fabs(std::fmod(h, 2.0f) - 1)), m = val - c;
    float r = 0, g = 0, b = 0;
    switch (int(h)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    auto u = [](float f) { return uint32_t(std::lround(std::clamp(f, 0.0f, 1.0f) * 255)); };
    return u(r + m) << 16 | u(g + m) << 8 | u(b + m);
}

D2D1_RECT_F Intersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return D2D1::RectF(std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom));
}

std::wstring DevelopIniPath() {
    const std::wstring ini = SettingsIniPath();
    return ini.empty() ? std::wstring() : ParentOf(ini) + L"\\develop_last.ini";
}

void WriteParams(const wchar_t* section, const Params& p) {
    const std::wstring path = DevelopIniPath();
    if (path.empty()) return;
    const std::wstring data = dev::SerializeSection(p);
    WritePrivateProfileSectionW(section, data.c_str(), path.c_str());
}

bool ReadParams(const wchar_t* section, Params& p) {
    const std::wstring path = DevelopIniPath();
    if (path.empty()) return false;
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetPrivateProfileSectionW(section, buf.data(), DWORD(buf.size()), path.c_str());
    return n > 0 && dev::ParseSection(buf.data(), p);
}

}  // namespace

// =============================================================================================

class Viewer::DevelopMode final : public Viewer::EditMode {
public:
    using EditMode::EditMode;
    const char* NameKey() const override { return "develop.mode.develop"; }
    const wchar_t* Glyph() const override { return L""; }

    bool Enter() override;
    void Leave() override;
    void Commit() override;
    void Close() override;
    bool BeginExport() override;
    void EndExport() override;
    void DropGpu() override { Close(); }

    void Prepare() override;
    ID2D1Bitmap1* LayerBitmap(int layer) const override;
    bool DrawLayer(int layer, const D2D1_RECT_F& r, D2D1_INTERPOLATION_MODE mode, float opacity) override;
    bool DocTransform(const D2D1_RECT_F& r, D2D1_MATRIX_3X2_F& m) const override;
    void DrawOverlay(float left, float top, float z) override;
    float DrawTopButtons(float right, float y, float size) override;
    void DrawOptionsBar(float cw, float y, float h) override;
    void DrawSide(float cw, float ch) override { DrawPanel(cw, ch); }
    D2D1_RECT_F ViewMargins() const override {
        const float s = v.m_scale;
        return D2D1::RectF(12 * s, 100 * s, 324 * s, 12 * s);
    }

    bool MouseDown(POINT p, bool right, bool dbl) override;
    void MouseMove(POINT p) override;
    void MouseUp(POINT p) override;
    bool Key(WPARAM vk) override;
    bool Char(wchar_t c) override;
    bool Wheel(POINT p, double notches) override;
    const wchar_t* Cursor(POINT p) const override;

private:
    float W() const { return m_src ? float(m_src->GetPixelSize().width) : float(v.m_edit.w); }
    float H() const { return m_src ? float(m_src->GetPixelSize().height) : float(v.m_edit.h); }
    Layer* DevLayer() const {
        return m_layer >= 0 && m_layer < int(v.m_edit.layers.size()) ? &v.m_edit.layers[size_t(m_layer)] : nullptr;
    }
    bool SameRender(const Params& a, const Params& b) const;
    bool FullCurrent() const { return m_fullValid && m_full && SameRender(m_fullP, m_p); }
    bool BakedCurrent() const {
        const Layer* l = DevLayer();
        return m_baked && l && l->bmp == m_baked && SameRender(m_bakedP, m_p);
    }
    void Changed();
    void PushUndo(int id = -1);
    void SetValue(int id, float val);
    void InvalidateSource();
    bool EnsureProxy(float zoom);
    bool EnsureSmall();
    void UpdateHistogram();
    ComPtr<ID2D1Bitmap1> RenderFull(bool take);
    bool TransformLayers(std::vector<ComPtr<ID2D1Bitmap1>>& bmps, uint32_t& w, uint32_t& h);
    void ApplyGeometry();
    D2D1_POINT_2F ScreenToSource(POINT p) const;
    void PickWhiteBalance(POINT p);
    void CopySettings();
    void PasteSettings();
    void ApplyPrevious();
    void CopyImage();
    bool Control(int id, POINT p, bool right, bool dbl);
    void CurveDown(POINT p, bool right, bool dbl);
    void CurveMove(POINT p);
    void CropDown(POINT p, bool dbl);
    void CropMove(POINT p);
    float AspectNow() const;
    void SetCrop(D2D1_RECT_F r);

    // Dessin du panneau
    void DrawPanel(float cw, float ch);
    float DrawSection(int sec, float y);
    void DrawHistogram(const D2D1_RECT_F& r);
    void DrawCurve(const D2D1_RECT_F& r);
    void Slider(int id, float& y, const wchar_t* label = nullptr, int track = kTrackPlain, int extra = 0);
    void DrawTrack(int id, const D2D1_RECT_F& tr, int track, int extra);
    D2D1_RECT_F SubHeader(const wchar_t* label, float& y);
    bool Btn(int id, const D2D1_RECT_F& r, const wchar_t* tip, bool active = false, bool enabled = true);
    void TextButton(int id, const D2D1_RECT_F& r, const wchar_t* label, const wchar_t* tip, bool active = false, bool enabled = true);
    void IconButton(int id, const D2D1_RECT_F& r, const wchar_t* glyph, const wchar_t* tip, bool active = false, bool enabled = true);
    void Gradient(const D2D1_RECT_F& r, const uint32_t* cols, int n, float radius);
    bool Visible(const D2D1_RECT_F& r) const { return r.bottom > m_clip.top && r.top < m_clip.bottom; }

    // Session (calque développé)
    bool m_active = false;
    int m_layer = -1;
    ComPtr<ID2D1Bitmap1> m_src;    // pixels du calque avant développement
    ComPtr<ID2D1Bitmap1> m_baked;  // résultat intégré au calque au dernier Commit (reprise possible)
    Params m_p, m_bakedP;
    std::vector<Params> m_undo, m_redo;
    double m_lastPush = 0;
    int m_lastPushId = -1;
    bool m_hasClip = false;        // réglages copiés (Ctrl+Alt+C), gardés d'une image à l'autre
    Params m_clipboard;

    // GPU
    dev::Graph m_live, m_histGraph, m_fullGraph;
    ComPtr<ID2D1Bitmap1> m_proxy;  // image réduite (16 bits flottants) pour l'aperçu en mouvement
    float m_proxyScale = 0;
    ComPtr<ID2D1Bitmap1> m_small, m_smallOut;  // petite image : histogramme, réglage automatique
    PixelBuffer m_smallPx;
    uint32_t m_smallStride = 0;
    ComPtr<ID2D1Bitmap1> m_full;   // rendu en pleine résolution des réglages m_fullP
    Params m_fullP;
    bool m_fullValid = false;
    double m_changeTime = -10, m_fullTime = -10;
    dev::Histogram m_hist;
    bool m_histDirty = true;
    double m_histTime = -10;

    // Vue et outils
    int m_tool = kToolNone;
    bool m_split = false, m_original = false, m_clipView = false;

    // Panneau
    D2D1_RECT_F m_panel{}, m_scrollRc{}, m_clip{}, m_curveRc{}, m_thumbRc{};
    D2D1_RECT_F m_track[dev::kParamCount]{};
    float m_scroll = 0, m_contentH = 0;
    bool m_open[kSecCount] = {true, false, false, false, false, false, false};
    int m_curveCh = 0, m_hslTab = 0;

    // Glissement en cours
    int m_drag = kDragNone, m_dragId = -1, m_curvePt = -1, m_corner = 0;
    bool m_curveRemove = false;
    POINT m_dragStart{}, m_dragCur{};
    float m_dragVal = 0;
    D2D1_POINT_2F m_anchor{};
    D2D1_RECT_F m_cropStart{};

    // Enregistrer / copier : document d'origine mis de côté
    bool m_exporting = false;
    std::vector<Layer> m_savedLayers;
    uint32_t m_savedW = 0, m_savedH = 0;
};

std::shared_ptr<Viewer::EditMode> Viewer::NewDevelopMode() { return std::make_shared<DevelopMode>(*this); }

// =============================================================================================
// Cycle de vie

bool Viewer::DevelopMode::Enter() {
    auto& e = v.m_edit;
    if (e.layers.empty() || !v.m_dc) return false;
    if (e.textActive) v.CommitText();
    if (e.fx >= 0) v.CloseEffect(false);
    e.cropValid = false;
    e.menu = EditMenu::None;
    if (!dev::RegisterEffect(v.m_factory.Get()) || (!m_live.Ready() && !m_live.Init(v.m_dc.Get(), true)) ||
        (!m_histGraph.Ready() && !m_histGraph.Init(v.m_dc.Get(), false))) {
        v.ShowToast(T("develop.err.gpu"));
        return false;
    }
    // Retour sur le calque développé juste avant (et pas modifié depuis) : les réglages sont repris
    Layer& l = v.CurLayer();
    const bool resume = m_src && m_baked && l.bmp == m_baked && m_layer == e.layer &&
                        m_src->GetPixelSize().width == e.w && m_src->GetPixelSize().height == e.h;
    if (!resume) {
        m_src = l.bmp;
        m_baked.Reset();
        m_p = Params();
        m_bakedP = m_p;
        m_undo.clear();
        m_redo.clear();
        m_full.Reset();
        m_fullValid = false;
    }
    m_layer = e.layer;
    m_active = true;
    m_tool = kToolNone;
    m_split = m_original = false;
    m_drag = kDragNone;
    InvalidateSource();
    return true;
}

void Viewer::DevelopMode::Leave() {
    Commit();
    m_active = false;
    m_tool = kToolNone;
    m_proxy.Reset();
    m_small.Reset();
    m_smallOut.Reset();
    m_full.Reset();
    m_fullValid = false;
    KillTimer(v.m_hwnd, kTimerDevelop);
}

void Viewer::DevelopMode::Close() {
    m_active = false;
    m_src.Reset();
    m_baked.Reset();
    m_p = m_bakedP = Params();
    m_undo.clear();
    m_redo.clear();
    m_live.Reset();
    m_histGraph.Reset();
    m_fullGraph.Reset();
    m_proxy.Reset();
    m_small.Reset();
    m_smallOut.Reset();
    m_full.Reset();
    m_fullValid = false;
    m_savedLayers.clear();
    m_exporting = false;
    m_drag = kDragNone;
    if (v.m_hwnd) KillTimer(v.m_hwnd, kTimerDevelop);
}

// Intègre les réglages au document : pixels du calque (historique), puis géométrie (tous les calques)
void Viewer::DevelopMode::Commit() {
    Layer* l = DevLayer();
    if (!m_active || !m_src || !l || m_exporting) return;
    auto& e = v.m_edit;
    e.layer = m_layer;
    const bool wasClean = !e.dirty;  // déjà enregistré avec ces réglages : rien de neuf à enregistrer
    if (!m_p.ColorIdentity()) {
        if (!(l->bmp == m_baked && m_bakedP.SameColor(m_p) && SameRender(m_bakedP, m_p))) {
            ComPtr<ID2D1Bitmap1> res = RenderFull(true);
            if (!res) {
                v.ShowToast(T("edit.err.vram"));
                return;
            }
            v.PushLayerSwap(res);
            m_baked = res;
            m_bakedP = m_p;
            WriteParams(L"previous", m_p);  // « Réglages précédents » de la prochaine image
        }
    } else if (m_baked && l->bmp == m_baked) {  // réglages remis à zéro après une intégration
        v.PushLayerSwap(m_src);
        m_baked.Reset();
        m_bakedP = m_p;
    }
    if (!m_p.GeoIdentity()) ApplyGeometry();
    if (wasClean) e.dirty = false;
    v.Invalidate();
}

// Enregistrer / copier : le document est temporairement remplacé par le résultat (calque développé,
// géométrie appliquée à tous les calques) ; EndExport remet l'original.
bool Viewer::DevelopMode::BeginExport() {
    Layer* l = DevLayer();
    if (!m_active || !m_src || !l || m_exporting) return false;
    if (m_p.Identity() && !(m_baked && l->bmp == m_baked)) return false;
    auto& e = v.m_edit;
    std::vector<ComPtr<ID2D1Bitmap1>> bmps;
    for (auto& layer : e.layers) bmps.push_back(layer.bmp);
    if (!m_p.ColorIdentity()) {
        ComPtr<ID2D1Bitmap1> full = RenderFull(false);
        if (!full) {
            v.ShowToast(T("edit.err.vram"));
            return false;
        }
        bmps[size_t(m_layer)] = full;
    } else {
        bmps[size_t(m_layer)] = m_src;
    }
    uint32_t w = e.w, h = e.h;
    if (!m_p.GeoIdentity() && !TransformLayers(bmps, w, h)) {
        v.ShowToast(T("edit.err.vram"));
        return false;
    }
    m_savedLayers = e.layers;
    m_savedW = e.w;
    m_savedH = e.h;
    for (size_t i = 0; i < bmps.size(); ++i) e.layers[i].bmp = bmps[i];
    e.w = w;
    e.h = h;
    m_exporting = true;
    return true;
}

void Viewer::DevelopMode::EndExport() {
    if (!m_exporting) return;
    auto& e = v.m_edit;
    e.layers = std::move(m_savedLayers);
    m_savedLayers.clear();
    e.w = m_savedW;
    e.h = m_savedH;
    m_exporting = false;
    v.Invalidate();
}

// =============================================================================================
// Réglages, historique des réglages

bool Viewer::DevelopMode::SameRender(const Params& a, const Params& b) const {
    if (!a.SameColor(b)) return false;
    if (a.v[dev::kVigAmount] == 0) return true;  // le vignetage suit le recadrage
    const D2D1_RECT_F ca = dev::CropRect(a, W(), H()), cb = dev::CropRect(b, W(), H());
    return ca.left == cb.left && ca.top == cb.top && ca.right == cb.right && ca.bottom == cb.bottom;
}

void Viewer::DevelopMode::Changed() {
    m_changeTime = NowSeconds();
    m_histDirty = true;
    v.m_edit.dirty = true;
    SetTimer(v.m_hwnd, kTimerDevelop, UINT(kFullDelay * 1000) + 30, OnDevelopTimer);
    v.Invalidate();
}

// id >= 0 : molette sur ce curseur, les crans rapprochés ne font qu'une entrée
void Viewer::DevelopMode::PushUndo(int id) {
    const double now = NowSeconds();
    if (id >= 0 && id == m_lastPushId && now - m_lastPush < 0.8) {
        m_lastPush = now;
        return;
    }
    m_undo.push_back(m_p);
    if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_lastPushId = id;
    m_lastPush = now;
}

void Viewer::DevelopMode::SetValue(int id, float val) {
    val = Quantize(id, val);
    if (val == m_p.v[id]) return;
    m_p.v[id] = val;
    if (id == dev::kAngle) m_p.cropSet = false;  // recadrage automatique sur la nouvelle rotation
    Changed();
}

void Viewer::DevelopMode::InvalidateSource() {
    m_proxy.Reset();
    m_proxyScale = 0;
    m_small.Reset();
    m_smallOut.Reset();
    m_histDirty = true;
}

// =============================================================================================
// Rendu

// Image réduite à l'échelle de l'affichage (quart d'octave au-dessus), en 16 bits flottants
bool Viewer::DevelopMode::EnsureProxy(float zoom) {
    const float ps = std::min(1.0f, std::exp2(std::ceil(std::log2(std::max(zoom, 1e-3f)) * 4) / 4));
    if (m_proxy && m_proxyScale == ps) return true;
    const float w = W(), h = H();
    const UINT32 pw = std::max(1u, UINT32(std::ceil(w * ps))), ph = std::max(1u, UINT32(std::ceil(h * ps)));
    ComPtr<ID2D1Bitmap1> b;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(v.m_dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, bp, &b))) b = v.NewCanvas(pw, ph);
    if (!b) return false;
    ID2D1Bitmap1* src = m_src.Get();
    if (!v.DrawOn(b.Get(), [&] {
            v.m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            v.m_dc->DrawBitmap(src, D2D1::RectF(0, 0, float(pw), float(ph)), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                               nullptr, nullptr);
        }))
        return false;
    m_proxy = b;
    m_proxyScale = ps;
    return true;
}

bool Viewer::DevelopMode::EnsureSmall() {
    if (m_small) return true;
    const float w = W(), h = H(), k = std::min(1.0f, 400.0f / std::max(w, h));
    const UINT32 sw = std::max(1u, UINT32(std::lround(w * k))), sh = std::max(1u, UINT32(std::lround(h * k)));
    m_small = v.NewCanvas(sw, sh);
    m_smallOut = v.NewCanvas(sw, sh);
    ID2D1Bitmap1* src = m_src.Get();
    if (!m_small || !m_smallOut || !v.DrawOn(m_small.Get(), [&] {
            v.m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            v.m_dc->DrawBitmap(src, D2D1::RectF(0, 0, float(sw), float(sh)), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                               nullptr, nullptr);
        }) ||
        !v.ReadBitmap(m_small.Get(), m_smallPx, m_smallStride)) {
        m_small.Reset();
        m_smallOut.Reset();
        return false;
    }
    return true;
}

void Viewer::DevelopMode::UpdateHistogram() {
    m_histDirty = false;
    m_histTime = NowSeconds();
    static const bool tracing = GetEnvironmentVariableW(L"CYIMG_TRACE", nullptr, 0) > 0;
    struct Timer {
        double t0 = NowSeconds();
        ~Timer() {
            if (!tracing) return;
            wchar_t tr[64];
            swprintf_s(tr, L"développement : histogramme en %.1f ms", (NowSeconds() - t0) * 1000);
            Trace(tr);
        }
    } timer;
    if (!EnsureSmall()) return;
    const D2D1_SIZE_U ss = m_small->GetPixelSize();
    ID2D1Bitmap1* src = m_small.Get();
    if (!m_p.ColorIdentity() && m_histGraph.Ready()) {
        dev::View vw;
        vw.scale = float(ss.width) / W();
        vw.longSide = std::max(W(), H());
        const D2D1_RECT_F c = dev::CropRect(m_p, W(), H());
        vw.frame = D2D1::RectF(c.left * vw.scale, c.top * vw.scale, c.right * vw.scale, c.bottom * vw.scale);
        ComPtr<ID2D1Image> out = m_histGraph.Build(m_small.Get(), m_p, vw);
        if (out && v.DrawOn(m_smallOut.Get(), [&] {
                v.m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                v.m_dc->DrawImage(out.Get(), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            }))
            src = m_smallOut.Get();
    }
    PixelBuffer px;
    uint32_t stride = 0;
    if (v.ReadBitmap(src, px, stride)) dev::ComputeHistogram(px.Data(), ss.width, ss.height, stride, m_hist);
}

// Rendu en pleine résolution des réglages courants (take : la texture est cédée à l'appelant)
ComPtr<ID2D1Bitmap1> Viewer::DevelopMode::RenderFull(bool take) {
    if (!FullCurrent()) {
        m_fullValid = false;
        if (!m_fullGraph.Ready() && !m_fullGraph.Init(v.m_dc.Get(), false)) return {};
        const D2D1_SIZE_U sz = m_src->GetPixelSize();
        if (!m_full || m_full->GetPixelSize().width != sz.width || m_full->GetPixelSize().height != sz.height)
            m_full = v.NewCanvas(sz.width, sz.height);
        if (!m_full) return {};
        dev::View vw;
        vw.longSide = std::max(W(), H());
        vw.frame = dev::CropRect(m_p, W(), H());
        const double t0 = NowSeconds();
        ComPtr<ID2D1Image> out = m_fullGraph.Build(m_src.Get(), m_p, vw);
        if (!out || !v.DrawOn(m_full.Get(), [&] {
                v.m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                v.m_dc->DrawImage(out.Get(), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            })) {
            m_full.Reset();
            return {};
        }
        static const bool tracing = GetEnvironmentVariableW(L"CYIMG_TRACE", nullptr, 0) > 0;
        if (tracing) {  // mesure : lecture d'un pixel = attente de la fin du calcul GPU
            PixelBuffer one;
            uint32_t st = 0;
            const D2D1_RECT_U px = D2D1::RectU(0, 0, 1, 1);
            v.ReadBitmap(m_full.Get(), one, st, &px);
            wchar_t tr[96];
            swprintf_s(tr, L"développement : rendu complet %u × %u en %.0f ms", sz.width, sz.height, (NowSeconds() - t0) * 1000);
            Trace(tr);
        }
        m_fullP = m_p;
        m_fullValid = true;
        m_fullTime = NowSeconds();
        v.m_edit.fxVer = ++v.m_edit.verSeq;  // composition des calques à refaire
    }
    ComPtr<ID2D1Bitmap1> r = m_full;
    if (take) {
        m_full.Reset();
        m_fullValid = false;
    }
    return r;
}

// Redressement (rotation autour du centre) puis recadrage, pour chaque texture
bool Viewer::DevelopMode::TransformLayers(std::vector<ComPtr<ID2D1Bitmap1>>& bmps, uint32_t& w, uint32_t& h) {
    const float fw = float(w), fh = float(h), angle = m_p.v[dev::kAngle];
    D2D1_RECT_F c = dev::CropRect(m_p, fw, fh);
    if (angle != 0) c = D2D1::RectF(c.left + 1.5f, c.top + 1.5f, c.right - 1.5f, c.bottom - 1.5f);  // bords rééchantillonnés : pas de pixels à demi transparents
    const float left = std::round(c.left), top = std::round(c.top);
    const UINT32 cw = std::max(1u, UINT32(std::lround(c.right - c.left))), ch = std::max(1u, UINT32(std::lround(c.bottom - c.top)));
    const D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Rotation(angle, D2D1::Point2F(fw / 2, fh / 2)) * D2D1::Matrix3x2F::Translation(-left, -top);
    for (auto& b : bmps) {
        ComPtr<ID2D1Bitmap1> out = v.NewCanvas(cw, ch);
        ID2D1Bitmap1* src = b.Get();
        if (!out || !v.DrawOn(out.Get(), [&] {
                v.m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                v.m_dc->SetTransform(m);
                v.m_dc->DrawBitmap(src, D2D1::RectF(0, 0, fw, fh), 1.0f,
                                   angle == 0 ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                                   nullptr, nullptr);
                v.m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
            }))
            return false;
        b = out;
    }
    w = cw;
    h = ch;
    return true;
}

void Viewer::DevelopMode::ApplyGeometry() {
    auto& e = v.m_edit;
    std::vector<ComPtr<ID2D1Bitmap1>> bmps;
    for (auto& l : e.layers) bmps.push_back(l.bmp);
    const bool srcIsLayer = bmps[size_t(m_layer)] == m_src;
    const bool baked = m_baked && bmps[size_t(m_layer)] == m_baked;
    if (!srcIsLayer) bmps.push_back(m_src);  // les pixels d'origine suivent, pour continuer à développer
    uint32_t w = e.w, h = e.h;
    if (!TransformLayers(bmps, w, h)) {
        v.ShowToast(T("edit.err.vram"));
        return;
    }
    ComPtr<ID2D1Bitmap1> src = srcIsLayer ? bmps[size_t(m_layer)] : bmps.back();
    if (!srcIsLayer) bmps.pop_back();
    v.SetDocument(std::move(bmps));  // une entrée d'historique, comme les transformations de la Retouche
    m_src = src;
    m_baked = baked ? e.layers[size_t(m_layer)].bmp : nullptr;
    m_p.ResetGeometry();
    m_bakedP.ResetGeometry();
    for (auto& u : m_undo) u.ResetGeometry();
    for (auto& r : m_redo) r.ResetGeometry();
    m_full.Reset();
    m_fullValid = false;
    InvalidateSource();
}

void Viewer::DevelopMode::Prepare() {
    if (!m_active || !m_src || m_exporting || !v.m_dc) return;
    const double now = NowSeconds();
    const bool dragging = m_drag != kDragNone;
    if (float(v.m_zoom) < 0.75f) EnsureProxy(float(v.m_zoom));
    else m_proxy.Reset();
    // À l'arrêt (ou si la composition des calques l'exige) : rendu en pleine résolution
    if (!m_p.ColorIdentity() && !FullCurrent() && !BakedCurrent()) {
        const bool composite = v.NeedComposite() && now - m_fullTime > 0.15;
        if (composite || (!dragging && now - m_changeTime >= kFullDelay)) RenderFull(false);
    }
    if (m_histDirty && (!dragging || now - m_histTime > 0.12)) UpdateHistogram();
}

ID2D1Bitmap1* Viewer::DevelopMode::LayerBitmap(int layer) const {
    if (!m_active || m_exporting || layer != m_layer || !m_src) return nullptr;
    if (m_original || m_p.ColorIdentity()) return m_src.Get();
    if (FullCurrent()) return m_full.Get();
    if (BakedCurrent()) return DevLayer()->bmp.Get();
    return nullptr;
}

bool Viewer::DevelopMode::DocTransform(const D2D1_RECT_F& r, D2D1_MATRIX_3X2_F& m) const {
    if (!m_active || m_split || m_original || m_exporting || m_p.v[dev::kAngle] == 0) return false;
    m = D2D1::Matrix3x2F::Rotation(m_p.v[dev::kAngle], D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2));
    return true;
}

bool Viewer::DevelopMode::DrawLayer(int layer, const D2D1_RECT_F& r, D2D1_INTERPOLATION_MODE mode, float opacity) {
    if (!m_active || m_exporting || layer != m_layer || !m_src) return false;
    ID2D1DeviceContext* dc = v.m_dc.Get();
    auto drawBitmap = [&](ID2D1Bitmap1* b) { dc->DrawBitmap(b, r, 1.0f, mode, nullptr, nullptr); };
    auto drawAfter = [&] {
        if (m_p.ColorIdentity() && !m_clipView) return drawBitmap(m_src.Get());
        if (!m_clipView && FullCurrent()) return drawBitmap(m_full.Get());
        if (!m_clipView && BakedCurrent()) return drawBitmap(DevLayer()->bmp.Get());
        // Aperçu en direct : image réduite si l'image est affichée en petit, sinon pleine résolution
        // (Direct2D ne calcule que la partie visible)
        const float w = W(), h = H(), zoom = (r.right - r.left) / w;
        ID2D1Image* input = m_src.Get();
        float iw = w, ih = h;
        if (m_proxy && zoom < 0.75f) {
            input = m_proxy.Get();
            iw = float(m_proxy->GetPixelSize().width);
            ih = float(m_proxy->GetPixelSize().height);
        }
        dev::View vw;
        vw.scale = iw / w;
        vw.longSide = std::max(w, h);
        const D2D1_RECT_F c = dev::CropRect(m_p, w, h);
        vw.frame = D2D1::RectF(c.left * vw.scale, c.top * vw.scale, c.right * vw.scale, c.bottom * vw.scale);
        vw.clipping = m_clipView;
        ComPtr<ID2D1Image> out = m_live.Build(input, m_p, vw);
        if (!out) return drawBitmap(m_src.Get());
        D2D1_MATRIX_3X2_F cur;
        dc->GetTransform(&cur);
        dc->SetTransform(D2D1::Matrix3x2F::Scale((r.right - r.left) / iw, (r.bottom - r.top) / ih) *
                         D2D1::Matrix3x2F::Translation(r.left, r.top) * *D2D1::Matrix3x2F::ReinterpretBaseType(&cur));
        dc->DrawImage(out.Get(), D2D1::Point2F(0, 0), D2D1::RectF(0, 0, iw, ih),
                      input == m_src.Get() ? mode : D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
        dc->SetTransform(cur);
    };
    const bool fade = opacity < 0.999f;
    if (fade)
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), opacity),
                      nullptr);
    if (m_original) {
        drawBitmap(m_src.Get());
    } else if (m_split) {  // avant à gauche, après à droite
        const float mid = std::round((r.left + r.right) / 2);
        D2D1_MATRIX_3X2_F cur;
        dc->GetTransform(&cur);
        for (int half = 0; half < 2; ++half) {
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->PushAxisAlignedClip(half ? D2D1::RectF(mid, -1e6f, 1e6f, 1e6f) : D2D1::RectF(-1e6f, -1e6f, mid, 1e6f),
                                    D2D1_ANTIALIAS_MODE_ALIASED);
            dc->SetTransform(cur);
            if (half) drawAfter();
            else drawBitmap(m_src.Get());
            dc->PopAxisAlignedClip();
        }
    } else {
        drawAfter();
    }
    if (fade) dc->PopLayer();
    return true;
}

// =============================================================================================
// Surcouches : recadrage, redressement, avant / après, pipette

void Viewer::DevelopMode::DrawOverlay(float left, float top, float z) {
    if (!m_active) return;
    const float s = v.m_scale, w = W(), h = H();
    auto* rt = v.m_rt.Get();
    ID2D1Brush* br = v.m_brush.Get();
    RECT rc;
    GetClientRect(v.m_hwnd, &rc);
    const D2D1_RECT_F img = D2D1::RectF(left, top, left + w * z, top + h * z);
    auto label = [&](const wchar_t* text, float x, float y) {
        const D2D1_RECT_F lr = D2D1::RectF(x, y, x + 96 * s, y + 24 * s);
        v.FillRound(lr, 4 * s, 0x000000, 0.6f);
        v.Text(text, v.m_tfCenter.Get(), lr, kWhite, 0.95f);
    };

    // Recadrage : extérieur assombri (plus fort hors de l'outil), grille des tiers et poignées dans l'outil
    const bool cropTool = m_tool == kToolCrop;
    if (!m_split && !m_original && (cropTool || !m_p.GeoIdentity())) {
        const D2D1_RECT_F ic = dev::CropRect(m_p, w, h);
        const D2D1_RECT_F c = D2D1::RectF(left + ic.left * z, top + ic.top * z, left + ic.right * z, top + ic.bottom * z);
        const float cw = float(rc.right), ch = float(rc.bottom);
        v.SetColor(kBg, cropTool ? 0.6f : 0.88f);
        rt->FillRectangle(D2D1::RectF(0, 0, cw, c.top), br);
        rt->FillRectangle(D2D1::RectF(0, c.bottom, cw, ch), br);
        rt->FillRectangle(D2D1::RectF(0, c.top, c.left, c.bottom), br);
        rt->FillRectangle(D2D1::RectF(c.right, c.top, cw, c.bottom), br);
        if (cropTool) {
            v.SetColor(kWhite, 0.35f);
            for (int i = 1; i < 3; ++i) {
                const float x = c.left + (c.right - c.left) * i / 3, y = c.top + (c.bottom - c.top) * i / 3;
                rt->DrawLine(D2D1::Point2F(x, c.top), D2D1::Point2F(x, c.bottom), br, 1);
                rt->DrawLine(D2D1::Point2F(c.left, y), D2D1::Point2F(c.right, y), br, 1);
            }
            v.SetColor(kWhite, 0.95f);
            rt->DrawRectangle(c, br, 1.5f * s);
            const float k = 16 * s, t = 3 * s;
            for (int i = 0; i < 4; ++i) {
                const float x = (i & 1) ? c.right : c.left, y = (i & 2) ? c.bottom : c.top;
                const float dx = (i & 1) ? -1.0f : 1.0f, dy = (i & 2) ? -1.0f : 1.0f;
                rt->FillRectangle(D2D1::RectF(std::min(x, x + dx * k), std::min(y, y + dy * t), std::max(x, x + dx * k), std::max(y, y + dy * t)), br);
                rt->FillRectangle(D2D1::RectF(std::min(x, x + dx * t), std::min(y, y + dy * k), std::max(x, x + dx * t), std::max(y, y + dy * k)), br);
            }
            wchar_t b[48];
            swprintf_s(b, L"%.0f × %.0f", double(ic.right - ic.left), double(ic.bottom - ic.top));
            const D2D1_RECT_F lr = D2D1::RectF(c.left, c.top - 26 * s, c.left + 110 * s, c.top - 4 * s);
            v.FillRound(lr, 4 * s, 0x000000, 0.6f);
            v.Text(b, v.m_tfCenter.Get(), lr, kWhite, 1);
        } else {
            v.SetColor(kWhite, 0.5f);
            rt->DrawRectangle(c, br, 1);
        }
    }

    // Redressement : grille fine pendant le réglage de l'angle, ligne tracée par l'outil
    if (!m_split && !m_original && (m_tool == kToolStraighten || (m_drag == kDragSlider && m_dragId == dev::kAngle))) {
        v.SetColor(kWhite, 0.22f);
        const float step = std::max(24 * s, (img.right - img.left) / 12);
        for (float x = img.left + step; x < img.right; x += step) rt->DrawLine(D2D1::Point2F(x, img.top), D2D1::Point2F(x, img.bottom), br, 1);
        for (float y = img.top + step; y < img.bottom; y += step) rt->DrawLine(D2D1::Point2F(img.left, y), D2D1::Point2F(img.right, y), br, 1);
    }
    if (m_drag == kDragStraighten) {
        const D2D1_POINT_2F a = D2D1::Point2F(float(m_dragStart.x), float(m_dragStart.y));
        const D2D1_POINT_2F b = D2D1::Point2F(float(m_dragCur.x), float(m_dragCur.y));
        v.SetColor(0x000000, 0.7f);
        rt->DrawLine(a, b, br, 3 * s, v.m_roundStroke.Get());
        v.SetColor(kAccent, 1);
        rt->DrawLine(a, b, br, 1.5f * s, v.m_roundStroke.Get());
    }

    // Avant / après
    if (m_split) {
        const float mid = std::round((img.left + img.right) / 2);
        v.SetColor(0x000000, 0.6f);
        rt->FillRectangle(D2D1::RectF(mid - 1.5f * s, img.top, mid + 1.5f * s, img.bottom), br);
        v.SetColor(kWhite, 0.9f);
        rt->FillRectangle(D2D1::RectF(mid - 0.5f, img.top, mid + 0.5f, img.bottom), br);
        label(T("develop.view.before"), std::max(img.left, 0.0f) + 10 * s, std::max(img.top, 100 * s) + 10 * s);
        label(T("develop.view.after"), mid + 10 * s, std::max(img.top, 100 * s) + 10 * s);
    } else if (m_original) {
        label(T("develop.view.before"), std::max(img.left, 0.0f) + 10 * s, std::max(img.top, 100 * s) + 10 * s);
    }

    // Pipette de balance des blancs : zone échantillonnée sous la souris
    if (m_tool == kToolWb && v.m_mouseInside && !v.HitEdit(v.m_mouse) && m_drag == kDragNone) {
        const float r = std::max(4 * s, 3.5f * z);
        const D2D1_RECT_F q = D2D1::RectF(v.m_mouse.x - r, v.m_mouse.y - r, v.m_mouse.x + r, v.m_mouse.y + r);
        v.SetColor(0x000000, 0.8f);
        rt->DrawRectangle(q, br, 3 * s);
        v.SetColor(kWhite, 1);
        rt->DrawRectangle(q, br, 1.2f * s);
    }
}

// =============================================================================================
// Barres

float Viewer::DevelopMode::DrawTopButtons(float right, float y, float bs) {
    const float s = v.m_scale, gap = 2 * s;
    const D2D1_RECT_F redo = D2D1::RectF(right - bs, y, right, y + bs);
    v.EditBtn(EditCmd::ModeCtl, kCtlRedo, redo, T("develop.tip.redo"), false, !m_redo.empty());
    v.Text(L"", v.m_tfIcon.Get(), redo, kWhite, m_redo.empty() ? 0.3f : 0.92f);
    const D2D1_RECT_F undo = D2D1::RectF(redo.left - gap - bs, y, redo.left - gap, y + bs);
    v.EditBtn(EditCmd::ModeCtl, kCtlUndo, undo, T("develop.tip.undo"), false, !m_undo.empty());
    v.Text(L"", v.m_tfIcon.Get(), undo, kWhite, m_undo.empty() ? 0.3f : 0.92f);
    return undo.left;
}

void Viewer::DevelopMode::DrawOptionsBar(float cw, float y, float h) {
    const float s = v.m_scale;
    auto* rt = v.m_rt.Get();
    v.SetColor(0x18191C, 0.96f);
    rt->FillRectangle(D2D1::RectF(0, y, cw, y + h), v.m_brush.Get());
    v.SetColor(kWhite, 0.06f);
    rt->FillRectangle(D2D1::RectF(0, y + h - 1, cw, y + h), v.m_brush.Get());
    m_clip = D2D1::RectF(0, 0, cw, y + h);

    // Outils : pipette de balance des blancs, redressement, recadrage
    float x = 10 * s;
    const float bs = 32 * s, by = y + (h - bs) / 2;
    const D2D1_RECT_F wb = D2D1::RectF(x, by, x + bs, by + bs);
    Btn(kCtlWbTool, wb, T("develop.tip.wb_tool"), m_tool == kToolWb);
    v.DrawToolIcon(Tool::Picker, wb, m_tool == kToolWb ? kAccent : kWhite, 0.95f);
    x += bs + 2 * s;
    const D2D1_RECT_F st = D2D1::RectF(x, by, x + bs, by + bs);
    Btn(kCtlStraighten, st, T("develop.tip.straighten_tool"), m_tool == kToolStraighten);
    {  // niveau : ligne inclinée sur l'horizon
        const float cx = (st.left + st.right) / 2, cy = (st.top + st.bottom) / 2;
        v.SetColor(m_tool == kToolStraighten ? kAccent : kWhite, 0.95f);
        rt->DrawLine(D2D1::Point2F(cx - 9 * s, cy + 4 * s), D2D1::Point2F(cx + 9 * s, cy + 4 * s), v.m_brush.Get(), 1.6f * s);
        rt->DrawLine(D2D1::Point2F(cx - 8 * s, cy + 1 * s), D2D1::Point2F(cx + 8 * s, cy - 6 * s), v.m_brush.Get(), 1.6f * s,
                     v.m_roundStroke.Get());
    }
    x += bs + 2 * s;
    const D2D1_RECT_F cr = D2D1::RectF(x, by, x + bs, by + bs);
    Btn(kCtlCropTool, cr, T("develop.tip.crop_tool"), m_tool == kToolCrop);
    v.DrawToolIcon(Tool::Crop, cr, m_tool == kToolCrop ? kAccent : kWhite, 0.95f);
    x += bs + 10 * s;
    v.SetColor(kWhite, 0.1f);
    rt->FillRectangle(D2D1::RectF(x, y + 8 * s, x + 1, y + h - 8 * s), v.m_brush.Get());
    x += 10 * s;

    // Affichage (à droite) : avant / après, original, écrêtage
    auto width = [&](const std::wstring& t) {
        ComPtr<IDWriteTextLayout> lay;
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(v.m_dwrite->CreateTextLayout(t.c_str(), UINT32(t.size()), v.m_tfSmall.Get(), 1000, 100, &lay))) lay->GetMetrics(&tm);
        return tm.width;
    };
    float rx = cw - 10 * s;
    const struct { int id; const char* label; const char* tip; bool on; } views[] = {
        {kCtlClip, "develop.view.clipping", "develop.tip.clip", m_clipView},
        {kCtlOriginal, "develop.view.original", "develop.tip.original", m_original},
        {kCtlSplit, "develop.view.split", "develop.tip.split", m_split},
    };
    for (const auto& vb : views) {
        const std::wstring label = T(vb.label);
        const float bw = width(label) + 24 * s;
        const D2D1_RECT_F r = D2D1::RectF(rx - bw, by, rx, by + bs);
        TextButton(vb.id, r, label.c_str(), T(vb.tip), vb.on);
        rx = r.left - 4 * s;
    }

    // Aide de l'outil
    const char* hint = m_tool == kToolWb ? "develop.hint.wb" : m_tool == kToolStraighten ? "develop.hint.straighten"
                     : m_tool == kToolCrop ? "develop.hint.crop" : "develop.hint.default";
    v.Text(T(hint), v.m_tfSmall.Get(), D2D1::RectF(x, y, rx - 12 * s, y + h), kGrey, 1);
}

// =============================================================================================
// Panneau de droite

bool Viewer::DevelopMode::Btn(int id, const D2D1_RECT_F& r, const wchar_t* tip, bool active, bool enabled) {
    const D2D1_RECT_F hit = Intersect(r, m_clip);
    if (hit.right <= hit.left || hit.bottom <= hit.top) return false;
    const float s = v.m_scale;
    const bool hov = enabled && v.m_editHover && v.m_editHover->cmd == EditCmd::ModeCtl && v.m_editHover->arg == id;
    if (active) v.FillRound(r, 5 * s, kAccent, 0.25f);
    if (hov) v.FillRound(r, 5 * s, kWhite, 0.12f);
    if (enabled) v.m_editButtons.push_back({EditCmd::ModeCtl, id, hit, tip});
    return hov;
}

void Viewer::DevelopMode::TextButton(int id, const D2D1_RECT_F& r, const wchar_t* label, const wchar_t* tip, bool active, bool enabled) {
    const float s = v.m_scale;
    Btn(id, r, tip, active, enabled);
    v.SetColor(active ? kAccent : kWhite, active ? 0.7f : 0.2f);
    v.m_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 5 * s, 5 * s), v.m_brush.Get(), 1);
    v.Text(label, v.m_tfCenter.Get(), r, active ? kAccent : kWhite, enabled ? 0.95f : 0.35f);
}

void Viewer::DevelopMode::IconButton(int id, const D2D1_RECT_F& r, const wchar_t* glyph, const wchar_t* tip, bool active, bool enabled) {
    Btn(id, r, tip, active, enabled);
    v.Text(glyph, v.m_tfIcon.Get(), r, active ? kAccent : kWhite, enabled ? 0.92f : 0.3f);
}

void Viewer::DevelopMode::Gradient(const D2D1_RECT_F& r, const uint32_t* cols, int n, float radius) {
    std::vector<D2D1_GRADIENT_STOP> st(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) st[size_t(i)] = {float(i) / float(n - 1), Rgb(cols[i])};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> gb;
    if (SUCCEEDED(v.m_rt->CreateGradientStopCollection(st.data(), UINT32(n), &coll)) &&
        SUCCEEDED(v.m_rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, 0), D2D1::Point2F(r.right, 0)), coll.Get(), &gb)))
        v.m_rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), gb.Get());
}

void Viewer::DevelopMode::DrawTrack(int id, const D2D1_RECT_F& tr, int track, int extra) {
    const float s = v.m_scale, cy = (tr.top + tr.bottom) / 2;
    const dev::ParamDef& d = dev::Def(id);
    const float t = std::clamp((m_p.v[id] - d.min) / (d.max - d.min), 0.0f, 1.0f);
    const float kx = tr.left + t * (tr.right - tr.left);
    const D2D1_RECT_F bar = D2D1::RectF(tr.left, cy - 2.5f * s, tr.right, cy + 2.5f * s);
    switch (track) {
    case kTrackTemp: {
        const uint32_t c[] = {0x3E78D8, 0xC9CED6, 0xE7BE3A};
        Gradient(bar, c, 3, 2 * s);
        break;
    }
    case kTrackTint: {
        const uint32_t c[] = {0x37B24D, 0xC9CED6, 0xD24BCB};
        Gradient(bar, c, 3, 2 * s);
        break;
    }
    case kTrackRainbow: {
        uint32_t c[7];
        for (int i = 0; i < 7; ++i) c[i] = HueColor(i * 60.0f, 0.75f);
        Gradient(bar, c, 7, 2 * s);
        break;
    }
    case kTrackHslHue: {
        const uint32_t c[] = {HueColor(kHueDeg[extra] - 30, 0.75f), HueColor(kHueDeg[extra], 0.75f), HueColor(kHueDeg[extra] + 30, 0.75f)};
        Gradient(bar, c, 3, 2 * s);
        break;
    }
    case kTrackHslSat: case kTrackSatOf: {
        const float hue = track == kTrackHslSat ? kHueDeg[extra] : m_p.v[extra];
        const uint32_t c[] = {0x8A8D93, HueColor(hue, 0.85f)};
        Gradient(bar, c, 2, 2 * s);
        break;
    }
    case kTrackHslLum: {
        const uint32_t c[] = {HueColor(kHueDeg[extra], 0.8f, 0.3f), HueColor(kHueDeg[extra], 0.45f, 1.0f)};
        Gradient(bar, c, 2, 2 * s);
        break;
    }
    default: {  // barre neutre, remplie depuis zéro (ou le minimum)
        v.FillRound(bar, 2 * s, kWhite, 0.16f);
        const float zt = std::clamp((std::clamp(d.def, d.min, d.max) - d.min) / (d.max - d.min), 0.0f, 1.0f);
        const float zx = tr.left + (d.min < 0 && d.max > 0 ? (0 - d.min) / (d.max - d.min) : zt) * (tr.right - tr.left);
        v.FillRound(D2D1::RectF(std::min(zx, kx), bar.top, std::max(zx, kx), bar.bottom), 2 * s, kAccent, 0.9f);
        break;
    }
    }
    const bool hot = (m_drag == kDragSlider && m_dragId == id) ||
                     (v.m_editHover && v.m_editHover->cmd == EditCmd::ModeCtl && v.m_editHover->arg == id);
    const float kr = (hot ? 7.0f : 6.0f) * s;
    v.SetColor(0x000000, 0.5f);
    v.m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), kr + 1.2f * s, kr + 1.2f * s), v.m_brush.Get());
    v.SetColor(m_p.v[id] != d.def ? kWhite : 0xC8CCD2, 1);
    v.m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), kr, kr), v.m_brush.Get());
}

// Ligne de curseur : libellé, piste, valeur. Double-clic : valeur par défaut ; molette sur la piste : ajuste.
void Viewer::DevelopMode::Slider(int id, float& y, const wchar_t* label, int track, int extra) {
    const float s = v.m_scale, pad = 12 * s, rowH = 26 * s;
    const float l = m_panel.left + pad, r = m_panel.right - pad;
    const D2D1_RECT_F row = D2D1::RectF(l - 4 * s, y, r + 4 * s, y + rowH);
    y += rowH;
    if (!Visible(row)) {
        m_track[id] = D2D1::RectF(0, 0, 0, 0);
        return;
    }
    const dev::ParamDef& d = dev::Def(id);
    const float labelW = 118 * s, valW = 44 * s;
    m_track[id] = D2D1::RectF(l + labelW, row.top + rowH / 2 - 8 * s, r - valW - 4 * s, row.top + rowH / 2 + 8 * s);
    const D2D1_RECT_F hit = Intersect(row, m_clip);
    if (hit.bottom > hit.top) v.m_editButtons.push_back({EditCmd::ModeCtl, id, hit, nullptr});
    float lx = l;
    if (track == kTrackHslHue || track == kTrackHslSat || track == kTrackHslLum) {  // pastille de la couleur
        v.SetColor(HueColor(kHueDeg[extra], 0.75f), 1);
        v.m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(l + 5 * s, row.top + rowH / 2), 4.5f * s, 4.5f * s), v.m_brush.Get());
        lx += 16 * s;
    }
    const bool changed = m_p.v[id] != d.def;
    v.Text(label ? label : T(d.label), v.m_tfSmall.Get(), D2D1::RectF(lx, row.top, l + labelW - 4 * s, row.bottom), kWhite,
           changed ? 1.0f : 0.8f);
    DrawTrack(id, m_track[id], track, extra);
    v.Text(FormatValue(id, m_p.v[id]), v.m_tfCenter.Get(), D2D1::RectF(r - valW, row.top, r, row.bottom),
           changed ? kAccent : kWhite, changed ? 1.0f : 0.7f);
}

D2D1_RECT_F Viewer::DevelopMode::SubHeader(const wchar_t* label, float& y) {
    const float s = v.m_scale, pad = 12 * s;
    const D2D1_RECT_F r = D2D1::RectF(m_panel.left + pad, y + 4 * s, m_panel.right - pad, y + 30 * s);
    y += 30 * s;
    if (Visible(r)) v.Text(label, v.m_tfSmall.Get(), D2D1::RectF(r.left, r.top, r.right - 90 * s, r.bottom), kGrey, 1);
    return D2D1::RectF(r.right - 86 * s, r.top + 1 * s, r.right, r.bottom - 1 * s);  // place d'un bouton à droite
}

void Viewer::DevelopMode::DrawHistogram(const D2D1_RECT_F& r) {
    const float s = v.m_scale;
    auto* dc = v.m_dc.Get();
    v.FillRound(r, 5 * s, 0x0C0D0F, 0.95f);
    const D2D1_RECT_F g = D2D1::RectF(r.left + 4 * s, r.top + 6 * s, r.right - 4 * s, r.bottom - 4 * s);
    if (m_hist.valid) {
        uint32_t peak = 1;  // échelle : on ignore les extrémités (souvent des pics)
        for (int c = 0; c < 3; ++c)
            for (int i = 2; i < 254; ++i) peak = std::max(peak, m_hist.bins[c][i]);
        const uint32_t cols[3] = {0xE0302C, 0x2CC03A, 0x2F6BF0};
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);  // superpositions : jaune, cyan, magenta, blanc
        for (int c = 0; c < 3; ++c) {
            ComPtr<ID2D1PathGeometry> geo;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(v.m_factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) continue;
            sink->BeginFigure(D2D1::Point2F(g.left, g.bottom), D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 0; i < 256; ++i) {
                const float hv = std::min(1.0f, std::sqrt(float(m_hist.bins[c][i]) / float(peak)) * 0.92f);
                sink->AddLine(D2D1::Point2F(g.left + (g.right - g.left) * i / 255.0f, g.bottom - hv * (g.bottom - g.top)));
            }
            sink->AddLine(D2D1::Point2F(g.right, g.bottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            v.SetColor(cols[c], 0.85f);
            dc->FillGeometry(geo.Get(), v.m_brush.Get());
        }
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
    // Indicateurs d'écrêtage (clic : zones écrêtées sur l'image)
    const float ts = 9 * s;
    for (int k = 0; k < 2; ++k) {
        const bool high = k == 1;
        const float x0 = high ? g.right - ts : g.left, y0 = r.top + 4 * s;
        const D2D1_RECT_F hit = D2D1::RectF(x0 - 4 * s, y0 - 2 * s, x0 + ts + 4 * s, y0 + ts + 4 * s);
        Btn(kCtlClip, hit, T(high ? "develop.tip.clip_high" : "develop.tip.clip_low"), m_clipView);
        const bool on = m_hist.valid && (high ? m_hist.clipHigh : m_hist.clipLow) > 0.0005f;
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(v.m_factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) continue;
        sink->BeginFigure(D2D1::Point2F(high ? x0 + ts : x0, y0), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(high ? x0 : x0 + ts, y0));
        sink->AddLine(D2D1::Point2F(high ? x0 + ts : x0, y0 + ts));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        v.SetColor(on ? (high ? 0xFF5050 : 0x5090FF) : 0x5A5E66, on ? 1.0f : 0.8f);
        v.m_rt->FillGeometry(geo.Get(), v.m_brush.Get());
    }
    if (!m_hist.valid) v.Text(T("develop.hist.empty"), v.m_tfCenter.Get(), r, kGrey, 0.7f);
}

void Viewer::DevelopMode::DrawCurve(const D2D1_RECT_F& r) {
    const float s = v.m_scale;
    auto* rt = v.m_rt.Get();
    ID2D1Brush* br = v.m_brush.Get();
    m_curveRc = r;
    v.FillRound(r, 3 * s, 0x0C0D0F, 0.9f);
    const D2D1_RECT_F hit = Intersect(D2D1::RectF(r.left - 6 * s, r.top - 6 * s, r.right + 6 * s, r.bottom + 6 * s), m_clip);
    if (hit.bottom > hit.top) v.m_editButtons.push_back({EditCmd::ModeCtl, kCtlCurve, hit, nullptr});
    // histogramme du canal en fond
    if (m_hist.valid) {
        uint32_t peak = 1;
        float bins[256];
        for (int i = 0; i < 256; ++i) {
            const uint32_t b = m_curveCh == 0 ? (m_hist.bins[0][i] + m_hist.bins[1][i] + m_hist.bins[2][i]) / 3 : m_hist.bins[m_curveCh - 1][i];
            bins[i] = float(b);
            if (i >= 2 && i < 254) peak = std::max(peak, b);
        }
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(v.m_factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(r.left, r.bottom), D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 0; i < 256; ++i)
                sink->AddLine(D2D1::Point2F(r.left + (r.right - r.left) * i / 255.0f,
                                            r.bottom - std::min(1.0f, std::sqrt(bins[i] / float(peak))) * (r.bottom - r.top) * 0.9f));
            sink->AddLine(D2D1::Point2F(r.right, r.bottom));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            v.SetColor(kWhite, 0.07f);
            rt->FillGeometry(geo.Get(), br);
        }
    }
    v.SetColor(kWhite, 0.08f);
    for (int i = 1; i < 4; ++i) {
        const float x = r.left + (r.right - r.left) * i / 4, y = r.top + (r.bottom - r.top) * i / 4;
        rt->DrawLine(D2D1::Point2F(x, r.top), D2D1::Point2F(x, r.bottom), br, 1);
        rt->DrawLine(D2D1::Point2F(r.left, y), D2D1::Point2F(r.right, y), br, 1);
    }
    v.SetColor(kWhite, 0.15f);
    rt->DrawLine(D2D1::Point2F(r.left, r.bottom), D2D1::Point2F(r.right, r.top), br, 1);
    const uint32_t col = m_curveCh == 1 ? 0xFF5A5A : m_curveCh == 2 ? 0x5AD25A : m_curveCh == 3 ? 0x6A9AFF : kWhite;
    dev::Curve c = m_p.curve[m_curveCh];
    if (c.size() < 2) c = {{0, 0}, {1, 1}};
    auto pt = [&](float x, float y) { return D2D1::Point2F(r.left + x * (r.right - r.left), r.bottom - y * (r.bottom - r.top)); };
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(v.m_factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
        sink->BeginFigure(pt(0, dev::EvalCurve(c, 0)), D2D1_FIGURE_BEGIN_HOLLOW);
        for (int i = 1; i <= 96; ++i) {
            const float x = i / 96.0f;
            sink->AddLine(pt(x, std::clamp(dev::EvalCurve(c, x), 0.0f, 1.0f)));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        v.SetColor(col, 0.95f);
        rt->DrawGeometry(geo.Get(), br, 1.6f * s);
    }
    for (int i = 0; i < int(c.size()); ++i) {
        if (m_drag == kDragCurve && m_curveRemove && i == m_curvePt) continue;
        const D2D1_POINT_2F q = pt(c[size_t(i)].x, c[size_t(i)].y);
        const bool act = m_drag == kDragCurve && i == m_curvePt;
        v.SetColor(act ? kAccent : col, 1);
        rt->FillEllipse(D2D1::Ellipse(q, 4.5f * s, 4.5f * s), br);
        v.SetColor(0x000000, 0.6f);
        rt->DrawEllipse(D2D1::Ellipse(q, 4.5f * s, 4.5f * s), br, 1);
    }
}

float Viewer::DevelopMode::DrawSection(int sec, float y) {
    const float s = v.m_scale, pad = 12 * s;
    const float l = m_panel.left + pad, r = m_panel.right - pad;
    auto* rt = v.m_rt.Get();
    // En-tête (clic : replier / déplier), point si la section contient des réglages
    const D2D1_RECT_F head = D2D1::RectF(m_panel.left + 4 * s, y, m_panel.right - 4 * s, y + 32 * s);
    if (Visible(head)) {
        v.SetColor(kWhite, 0.07f);
        rt->FillRectangle(D2D1::RectF(l, head.top, r, head.top + 1), v.m_brush.Get());
        Btn(kCtlSection0 + sec, head, nullptr);
        v.Text(m_open[sec] ? L"" : L"", v.m_tfIcon.Get(), D2D1::RectF(l - 4 * s, head.top, l + 16 * s, head.bottom),
               kGrey, 0.9f);
        v.Text(T(kSectionKeys[sec]), v.m_tfTitle.Get(), D2D1::RectF(l + 20 * s, head.top, r - 20 * s, head.bottom), kWhite, 0.95f);
        static const int ranges[kSecCount][2] = {{dev::kTemp, dev::kSaturation},           {dev::kPcHighlights, dev::kPcShadows},
                                                 {dev::kHslHue0, dev::kHslLum0 + 7},       {dev::kCgShadowHue, dev::kCgBalance},
                                                 {dev::kSharpAmount, dev::kNrColor},       {dev::kVigAmount, dev::kGrainRoughness},
                                                 {dev::kAngle, dev::kAngle}};
        bool mod = sec == kSecCurve && std::any_of(std::begin(m_p.curve), std::end(m_p.curve), [](const dev::Curve& c) {
                       return c.size() > 2 || (c.size() == 2 && !(c[0] == dev::Pt{0, 0} && c[1] == dev::Pt{1, 1}));
                   });
        if (sec == kSecGeometry) mod = !m_p.GeoIdentity();
        for (int i = ranges[sec][0]; i <= ranges[sec][1] && !mod; ++i) mod = m_p.v[i] != dev::Def(i).def;
        if (mod) {
            v.SetColor(kAccent, 1);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r - 6 * s, (head.top + head.bottom) / 2), 3 * s, 3 * s), v.m_brush.Get());
        }
    }
    y = head.bottom;
    if (!m_open[sec]) return y;
    y += 2 * s;
    switch (sec) {
    case kSecBasic: {
        D2D1_RECT_F b = SubHeader(T("develop.sub.wb"), y);
        IconButton(kCtlWbTool, D2D1::RectF(b.right - 30 * s, b.top, b.right, b.bottom), L"", T("develop.tip.wb_tool"), m_tool == kToolWb);
        if (Visible(b)) v.DrawToolIcon(Tool::Picker, D2D1::RectF(b.right - 30 * s, b.top, b.right, b.bottom), m_tool == kToolWb ? kAccent : kWhite, 0.9f);
        Slider(dev::kTemp, y, nullptr, kTrackTemp);
        Slider(dev::kTint, y, nullptr, kTrackTint);
        b = SubHeader(T("develop.sub.tone"), y);
        TextButton(kCtlAuto, D2D1::RectF(b.right - 64 * s, b.top, b.right, b.bottom), T("develop.btn.auto"), T("develop.tip.auto"));
        for (int id : {dev::kExposure, dev::kContrast, dev::kHighlights, dev::kShadows, dev::kWhites, dev::kBlacks}) Slider(id, y);
        SubHeader(T("develop.sub.presence"), y);
        for (int id : {dev::kTexture, dev::kClarity, dev::kDehaze, dev::kVibrance, dev::kSaturation}) Slider(id, y);
        break;
    }
    case kSecCurve: {
        // Canal (RVB, R, V, B) et réinitialisation
        const wchar_t* names[4] = {T("channels.rgb"), T("channels.letter.r"), T("channels.letter.g"), T("channels.letter.b")};
        const D2D1_RECT_F row = D2D1::RectF(l, y + 2 * s, r, y + 28 * s);
        float x = l;
        for (int c = 0; c < 4; ++c) {
            const float bw = c == 0 ? 52 * s : 32 * s;
            TextButton(kCtlCurveCh0 + c, D2D1::RectF(x, row.top, x + bw, row.bottom), names[c], T("develop.tip.curve_channel"), m_curveCh == c);
            x += bw + 4 * s;
        }
        IconButton(kCtlCurveReset, D2D1::RectF(r - 30 * s, row.top, r, row.bottom), L"", T("develop.tip.curve_reset"));
        y = row.bottom + 8 * s;
        const float side = r - l;
        DrawCurve(D2D1::RectF(l, y, r, y + side * 0.9f));
        y += side * 0.9f + 4 * s;
        const D2D1_RECT_F hint = D2D1::RectF(l, y, r, y + 20 * s);
        if (Visible(hint)) v.Text(T("develop.curve.hint"), v.m_tfSmall.Get(), hint, kGrey, 0.8f);
        y += 22 * s;
        SubHeader(T("develop.sub.parametric"), y);
        for (int id : {dev::kPcHighlights, dev::kPcLights, dev::kPcDarks, dev::kPcShadows}) Slider(id, y);
        break;
    }
    case kSecHsl: {
        const char* tabs[3] = {"develop.hsl.hue", "develop.hsl.sat", "develop.hsl.lum"};
        const float tw = (r - l - 8 * s) / 3;
        for (int t = 0; t < 3; ++t)
            TextButton(kCtlHslTab0 + t, D2D1::RectF(l + t * (tw + 4 * s), y + 2 * s, l + t * (tw + 4 * s) + tw, y + 28 * s), T(tabs[t]),
                       nullptr, m_hslTab == t);
        y += 34 * s;
        const int base = m_hslTab == 0 ? dev::kHslHue0 : m_hslTab == 1 ? dev::kHslSat0 : dev::kHslLum0;
        const int track = m_hslTab == 0 ? kTrackHslHue : m_hslTab == 1 ? kTrackHslSat : kTrackHslLum;
        for (int j = 0; j < 8; ++j) Slider(base + j, y, T(kColorKeys[j]), track, j);
        break;
    }
    case kSecGrading: {
        const char* zones[3] = {"develop.cg.shadows", "develop.cg.midtones", "develop.cg.highlights"};
        for (int z = 0; z < 3; ++z) {
            const D2D1_RECT_F b = SubHeader(T(zones[z]), y);
            const int hue = dev::kCgShadowHue + 2 * z, sat = hue + 1;
            if (Visible(b) && m_p.v[sat] > 0) {  // aperçu de la teinte ajoutée
                v.SetColor(HueColor(m_p.v[hue], std::min(1.0f, m_p.v[sat] / 60)), 1);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(b.right - 8 * s, (b.top + b.bottom) / 2), 6 * s, 6 * s), v.m_brush.Get());
            }
            Slider(hue, y, nullptr, kTrackRainbow);
            Slider(sat, y, nullptr, kTrackSatOf, hue);
        }
        y += 4 * s;
        Slider(dev::kCgBlend, y);
        Slider(dev::kCgBalance, y);
        break;
    }
    case kSecDetail: {
        SubHeader(T("develop.sub.sharpening"), y);
        for (int id : {dev::kSharpAmount, dev::kSharpRadius, dev::kSharpDetail, dev::kSharpMasking}) Slider(id, y);
        SubHeader(T("develop.sub.noise"), y);
        Slider(dev::kNrLuma, y);
        Slider(dev::kNrColor, y);
        const D2D1_RECT_F hint = D2D1::RectF(l, y + 2 * s, r, y + 22 * s);
        if (Visible(hint)) v.Text(T("develop.detail.hint"), v.m_tfSmall.Get(), hint, kGrey, 0.8f);
        y += 24 * s;
        break;
    }
    case kSecEffects: {
        SubHeader(T("develop.sub.vignette"), y);
        for (int id : {dev::kVigAmount, dev::kVigMidpoint, dev::kVigRoundness, dev::kVigFeather}) Slider(id, y);
        SubHeader(T("develop.sub.grain"), y);
        for (int id : {dev::kGrainAmount, dev::kGrainSize, dev::kGrainRoughness}) Slider(id, y);
        break;
    }
    case kSecGeometry: {
        D2D1_RECT_F b = SubHeader(T("develop.sub.straighten"), y);
        Btn(kCtlStraighten, D2D1::RectF(b.right - 30 * s, b.top, b.right, b.bottom), T("develop.tip.straighten_tool"), m_tool == kToolStraighten);
        if (Visible(b)) {
            const float cx = b.right - 15 * s, cy = (b.top + b.bottom) / 2;
            v.SetColor(m_tool == kToolStraighten ? kAccent : kWhite, 0.9f);
            rt->DrawLine(D2D1::Point2F(cx - 8 * s, cy + 4 * s), D2D1::Point2F(cx + 8 * s, cy + 4 * s), v.m_brush.Get(), 1.5f * s);
            rt->DrawLine(D2D1::Point2F(cx - 7 * s, cy + 1 * s), D2D1::Point2F(cx + 7 * s, cy - 5 * s), v.m_brush.Get(), 1.5f * s);
        }
        Slider(dev::kAngle, y);
        b = SubHeader(T("develop.sub.crop"), y);
        Btn(kCtlCropTool, D2D1::RectF(b.right - 30 * s, b.top, b.right, b.bottom), T("develop.tip.crop_tool"), m_tool == kToolCrop);
        if (Visible(b)) v.DrawToolIcon(Tool::Crop, D2D1::RectF(b.right - 30 * s, b.top, b.right, b.bottom), m_tool == kToolCrop ? kAccent : kWhite, 0.9f);
        IconButton(kCtlAspectFlip, D2D1::RectF(b.right - 64 * s, b.top, b.right - 34 * s, b.bottom), L"",
                   T("develop.tip.aspect_flip"), m_p.aspectPortrait, m_p.aspect >= 2);
        const float bw = (r - l - 3 * 4 * s) / 4;
        for (int a = 0; a < dev::kAspectCount; ++a) {
            const float x = l + (a % 4) * (bw + 4 * s), yy = y + (a / 4) * 30 * s;
            const wchar_t* lab = dev::AspectLabel(a);
            TextButton(kCtlAspect0 + a, D2D1::RectF(x, yy, x + bw, yy + 26 * s),
                       lab ? lab : T(a == 0 ? "develop.aspect.free" : "develop.aspect.original"), T("develop.tip.aspect"), m_p.aspect == a);
        }
        y += 2 * 30 * s + 4 * s;
        TextButton(kCtlGeoReset, D2D1::RectF(l, y, r, y + 28 * s), T("develop.btn.geo_reset"), nullptr, false, !m_p.GeoIdentity());
        y += 34 * s;
        const D2D1_RECT_F hint = D2D1::RectF(l, y, r, y + 20 * s);
        if (Visible(hint)) v.Text(T("develop.geo.hint"), v.m_tfSmall.Get(), hint, kGrey, 0.8f);
        y += 22 * s;
        break;
    }
    default: break;
    }
    return y + 6 * s;
}

void Viewer::DevelopMode::DrawPanel(float cw, float ch) {
    const float s = v.m_scale, pad = 12 * s;
    auto* rt = v.m_rt.Get();
    m_panel = D2D1::RectF(cw - 314 * s, 108 * s, cw - 10 * s, std::max(300 * s, ch - 12 * s));
    v.FillRound(m_panel, 8 * s, kPanel, 0.96f);
    v.EditBtn(EditCmd::Panel, 0, m_panel, nullptr);
    m_clip = m_panel;

    // Histogramme
    float y = m_panel.top + 8 * s;
    DrawHistogram(D2D1::RectF(m_panel.left + pad, y, m_panel.right - pad, y + 96 * s));
    y += 104 * s;

    // Sections (zone défilante) ; pied : Réinitialiser tout, Précédent, Copier, Coller
    const float footH = 46 * s;
    m_scrollRc = D2D1::RectF(m_panel.left, y, m_panel.right, m_panel.bottom - footH);
    const float viewH = m_scrollRc.bottom - m_scrollRc.top;
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, m_contentH - viewH));
    m_clip = m_scrollRc;
    rt->PushAxisAlignedClip(m_scrollRc, D2D1_ANTIALIAS_MODE_ALIASED);
    float cy = m_scrollRc.top - m_scroll;
    for (int sec = 0; sec < kSecCount; ++sec) cy = DrawSection(sec, cy);
    m_contentH = cy + m_scroll - m_scrollRc.top + 8 * s;
    rt->PopAxisAlignedClip();
    if (m_contentH > viewH) {  // barre de défilement (glissable)
        const float th = std::max(24 * s, viewH * viewH / m_contentH);
        const float ty = m_scrollRc.top + (viewH - th) * m_scroll / std::max(1.0f, m_contentH - viewH);
        m_thumbRc = D2D1::RectF(m_panel.right - 7 * s, ty, m_panel.right - 2 * s, ty + th);
        v.FillRound(m_thumbRc, 2 * s, kWhite, m_drag == kDragScroll ? 0.5f : 0.28f);
        m_clip = m_panel;
        Btn(kCtlScrollbar, D2D1::RectF(m_thumbRc.left - 3 * s, m_thumbRc.top, m_thumbRc.right + 1 * s, m_thumbRc.bottom), nullptr);
    } else {
        m_thumbRc = D2D1::RectF(0, 0, 0, 0);
    }
    m_clip = m_panel;
    v.SetColor(kWhite, 0.07f);
    rt->FillRectangle(D2D1::RectF(m_panel.left + pad, m_scrollRc.bottom, m_panel.right - pad, m_scrollRc.bottom + 1), v.m_brush.Get());
    const float fy = m_scrollRc.bottom + 8 * s, fb = fy + 30 * s, ib = 34 * s;
    const float iconsLeft = m_panel.right - pad - 3 * ib - 2 * 2 * s;
    TextButton(kCtlResetAll, D2D1::RectF(m_panel.left + pad, fy, iconsLeft - 8 * s, fb), T("develop.btn.reset_all"),
               T("develop.tip.reset_all"), false, !m_p.Identity());
    IconButton(kCtlPrevious, D2D1::RectF(iconsLeft, fy, iconsLeft + ib, fb), L"", T("develop.tip.previous"));
    IconButton(kCtlCopy, D2D1::RectF(iconsLeft + ib + 2 * s, fy, iconsLeft + 2 * ib + 2 * s, fb), L"", T("develop.tip.copy"));
    IconButton(kCtlPaste, D2D1::RectF(iconsLeft + 2 * ib + 4 * s, fy, iconsLeft + 3 * ib + 4 * s, fb), L"", T("develop.tip.paste"));
}

// =============================================================================================
// Souris

D2D1_POINT_2F Viewer::DevelopMode::ScreenToSource(POINT p) const {
    D2D1_POINT_2F ip = v.ScreenToImage(p);
    const float a = m_split || m_original ? 0 : m_p.v[dev::kAngle];
    if (a == 0) return ip;
    const float rad = -a * kPi / 180, c = std::cos(rad), sn = std::sin(rad), cx = W() / 2, cy = H() / 2;
    const float x = ip.x - cx, y = ip.y - cy;
    return D2D1::Point2F(cx + x * c - y * sn, cy + x * sn + y * c);
}

void Viewer::DevelopMode::PickWhiteBalance(POINT p) {
    const D2D1_POINT_2F ip = ScreenToSource(p);
    const int w = int(W()), h = int(H()), x = int(std::floor(ip.x)), y = int(std::floor(ip.y));
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const D2D1_RECT_U rc = D2D1::RectU(UINT32(std::max(0, x - 3)), UINT32(std::max(0, y - 3)), UINT32(std::min(w, x + 4)),
                                       UINT32(std::min(h, y + 4)));
    PixelBuffer px;
    uint32_t stride = 0;
    if (!v.ReadBitmap(m_src.Get(), px, stride, &rc)) return;
    auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    double acc[3] = {0, 0, 0};
    int n = 0;
    for (uint32_t yy = 0; yy < rc.bottom - rc.top; ++yy)
        for (uint32_t xx = 0; xx < rc.right - rc.left; ++xx) {
            const uint8_t* q = px.Data() + size_t(yy) * stride + size_t(xx) * 4;
            if (q[3] < 128) continue;
            for (int k = 0; k < 3; ++k) acc[k] += lin(float(q[2 - k]) / q[3]);
            ++n;
        }
    if (!n) return;
    float temp, tint;
    dev::SolveWhiteBalance(float(acc[0] / n), float(acc[1] / n), float(acc[2] / n), temp, tint);
    PushUndo();
    m_p.v[dev::kTemp] = temp;
    m_p.v[dev::kTint] = tint;
    m_tool = kToolNone;
    Changed();
}

float Viewer::DevelopMode::AspectNow() const {
    float ratio = dev::AspectRatio(m_p.aspect, W(), H());
    if (ratio > 0 && m_p.aspectPortrait) ratio = 1 / ratio;
    return ratio;
}

void Viewer::DevelopMode::SetCrop(D2D1_RECT_F r) {
    const float w = W(), h = H();
    m_p.crop = D2D1::RectF(r.left / w, r.top / h, r.right / w, r.bottom / h);
    m_p.cropSet = true;
    Changed();
}

void Viewer::DevelopMode::CropDown(POINT p, bool dbl) {
    if (dbl) {  // double-clic : fin du recadrage
        m_tool = kToolNone;
        v.Invalidate();
        return;
    }
    float left, top, z;
    if (!v.ImageRect(left, top, z)) return;
    const D2D1_RECT_F c = dev::CropRect(m_p, W(), H());
    const D2D1_POINT_2F ip = v.ScreenToImage(p);
    PushUndo();
    m_drag = kDragCropNew;
    m_anchor = ip;
    for (int k = 0; k < 4; ++k) {  // poignée de coin : le coin opposé reste fixe
        const float x = (k & 1) ? c.right : c.left, y = (k & 2) ? c.bottom : c.top;
        if (std::fabs(left + x * z - p.x) <= 14 * v.m_scale && std::fabs(top + y * z - p.y) <= 14 * v.m_scale) {
            m_drag = kDragCropCorner;
            m_anchor = D2D1::Point2F((k & 1) ? c.left : c.right, (k & 2) ? c.top : c.bottom);
            break;
        }
    }
    if (m_drag == kDragCropNew && ip.x > c.left && ip.x < c.right && ip.y > c.top && ip.y < c.bottom) {
        m_drag = kDragCropMove;
        m_anchor = ip;
        m_cropStart = c;
    }
    m_dragStart = p;
    v.m_edit.drawing = true;
    SetCapture(v.m_hwnd);
}

void Viewer::DevelopMode::CropMove(POINT p) {
    const float w = W(), h = H(), angle = m_p.v[dev::kAngle];
    const D2D1_POINT_2F ip = v.ScreenToImage(p);
    auto inside = [&](const D2D1_RECT_F& r) {
        const D2D1_RECT_F f = dev::FitCrop(r, angle, w, h);
        return std::fabs(f.left - r.left) < 0.05f && std::fabs(f.right - r.right) < 0.05f && std::fabs(f.top - r.top) < 0.05f &&
               std::fabs(f.bottom - r.bottom) < 0.05f;
    };
    if (m_drag == kDragCropMove) {  // déplacement, arrêté au bord de l'image
        const float dx = ip.x - m_anchor.x, dy = ip.y - m_anchor.y;
        float lo = 0, hi = 1;
        auto at = [&](float t) {
            return D2D1::RectF(m_cropStart.left + dx * t, m_cropStart.top + dy * t, m_cropStart.right + dx * t, m_cropStart.bottom + dy * t);
        };
        if (inside(at(1))) lo = 1;
        else
            for (int i = 0; i < 20; ++i) {
                const float t = (lo + hi) / 2;
                (inside(at(t)) ? lo : hi) = t;
            }
        SetCrop(at(lo));
        return;
    }
    // Nouveau rectangle ou coin : aux proportions choisies, réduit pour rester dans l'image
    const float ratio = AspectNow();
    float dx = ip.x - m_anchor.x, dy = ip.y - m_anchor.y;
    if (ratio > 0) {
        const float wv = std::max(std::fabs(dx), std::fabs(dy) * ratio);
        dx = std::copysign(wv, dx == 0 ? 1.0f : dx);
        dy = std::copysign(wv / ratio, dy == 0 ? 1.0f : dy);
    }
    auto rectAt = [&](float k) {
        const float x2 = m_anchor.x + dx * k, y2 = m_anchor.y + dy * k;
        return D2D1::RectF(std::min(m_anchor.x, x2), std::min(m_anchor.y, y2), std::max(m_anchor.x, x2), std::max(m_anchor.y, y2));
    };
    D2D1_RECT_F r = rectAt(1);
    if (!inside(r)) {
        if (!inside(rectAt(0.001f))) {
            r = dev::FitCrop(r, angle, w, h);
        } else {
            float lo = 0, hi = 1;
            for (int i = 0; i < 20; ++i) {
                const float k = (lo + hi) / 2;
                (inside(rectAt(k)) ? lo : hi) = k;
            }
            r = rectAt(lo);
        }
    }
    if (r.right - r.left >= 8 && r.bottom - r.top >= 8) SetCrop(r);
}

void Viewer::DevelopMode::CurveDown(POINT p, bool right, bool dbl) {
    const D2D1_RECT_F& r = m_curveRc;
    const float rw = std::max(1.0f, r.right - r.left), rh = std::max(1.0f, r.bottom - r.top);
    dev::Curve& c = m_p.curve[m_curveCh];
    if (c.size() < 2) c = {{0, 0}, {1, 1}};
    int hitPt = -1;
    float best = 10 * v.m_scale;
    for (int i = 0; i < int(c.size()); ++i) {
        const float d = std::hypot(r.left + c[size_t(i)].x * rw - p.x, r.bottom - c[size_t(i)].y * rh - p.y);
        if (d < best) {
            best = d;
            hitPt = i;
        }
    }
    if (right || dbl) {  // clic droit / double-clic sur un point : supprimé ; double-clic ailleurs : courbe remise à zéro
        if (hitPt > 0 && hitPt < int(c.size()) - 1) {
            PushUndo();
            c.erase(c.begin() + hitPt);
            Changed();
        } else if (dbl && hitPt < 0) {
            PushUndo();
            c.clear();
            Changed();
        }
        return;
    }
    PushUndo();
    if (hitPt < 0) {  // nouveau point sur la courbe
        const float x = std::clamp((p.x - r.left) / rw, 0.0f, 1.0f);
        auto it = std::lower_bound(c.begin(), c.end(), x, [](const dev::Pt& a, float v2) { return a.x < v2; });
        if (it == c.begin() || it == c.end() || std::fabs(it->x - x) < 0.01f || std::fabs((it - 1)->x - x) < 0.01f) return;
        hitPt = int(it - c.begin());
        c.insert(it, dev::Pt{x, std::clamp(dev::EvalCurve(c, x), 0.0f, 1.0f)});
    }
    m_drag = kDragCurve;
    m_curvePt = hitPt;
    m_curveRemove = false;
    v.m_edit.drawing = true;
    SetCapture(v.m_hwnd);
    CurveMove(p);
}

void Viewer::DevelopMode::CurveMove(POINT p) {
    const D2D1_RECT_F& r = m_curveRc;
    dev::Curve& c = m_p.curve[m_curveCh];
    if (m_curvePt < 0 || m_curvePt >= int(c.size())) return;
    const float rw = std::max(1.0f, r.right - r.left), rh = std::max(1.0f, r.bottom - r.top);
    const size_t i = size_t(m_curvePt);
    const float lo = i == 0 ? 0.0f : c[i - 1].x + 0.01f, hi = i + 1 == c.size() ? 1.0f : c[i + 1].x - 0.01f;
    c[i].x = std::clamp((p.x - r.left) / rw, lo, hi);
    c[i].y = std::clamp((r.bottom - p.y) / rh, 0.0f, 1.0f);
    // Point intérieur tiré hors du graphique : supprimé au relâchement
    const float out = 24 * v.m_scale;
    m_curveRemove = i > 0 && i + 1 < c.size() && (p.x < r.left - out || p.x > r.right + out || p.y < r.top - out || p.y > r.bottom + out);
    Changed();
}

bool Viewer::DevelopMode::Control(int id, POINT p, bool right, bool dbl) {
    if (id >= 0 && id < dev::kParamCount) {
        const dev::ParamDef& d = dev::Def(id);
        if (dbl) {  // valeur par défaut
            if (m_p.v[id] != d.def) {
                PushUndo();
                SetValue(id, d.def);
            }
            return true;
        }
        const D2D1_RECT_F& tr = m_track[id];
        if (right || p.x < tr.left - 8 * v.m_scale || p.x > tr.right + 8 * v.m_scale) return true;
        PushUndo();
        const float kx = tr.left + (m_p.v[id] - d.min) / (d.max - d.min) * (tr.right - tr.left);
        if (std::fabs(p.x - kx) > 8 * v.m_scale)  // clic sur la piste : le bouton y saute
            SetValue(id, d.min + std::clamp((p.x - tr.left) / std::max(1.0f, tr.right - tr.left), 0.0f, 1.0f) * (d.max - d.min));
        m_drag = kDragSlider;
        m_dragId = id;
        m_dragStart = p;
        m_dragVal = m_p.v[id];
        v.m_edit.drawing = true;
        SetCapture(v.m_hwnd);
        return true;
    }
    if (id == kCtlCurve) {  // clic droit : suppression d'un point
        CurveDown(p, right, dbl);
        v.Invalidate();
        return true;
    }
    if (right) return true;
    switch (id) {
    case kCtlAuto:
        if (EnsureSmall()) {
            PushUndo();
            const D2D1_SIZE_U ss = m_small->GetPixelSize();
            dev::AutoTone(m_smallPx.Data(), ss.width, ss.height, m_smallStride, m_p);
            Changed();
        }
        break;
    case kCtlWbTool: m_tool = m_tool == kToolWb ? kToolNone : kToolWb; break;
    case kCtlStraighten: m_tool = m_tool == kToolStraighten ? kToolNone : kToolStraighten; break;
    case kCtlCropTool: m_tool = m_tool == kToolCrop ? kToolNone : kToolCrop; break;
    case kCtlResetAll:
        if (!m_p.Identity()) {
            PushUndo();
            m_p = Params();
            Changed();
        }
        break;
    case kCtlPrevious: ApplyPrevious(); break;
    case kCtlCopy: CopySettings(); break;
    case kCtlPaste: PasteSettings(); break;
    case kCtlSplit:
        m_split = !m_split;
        m_original = false;
        break;
    case kCtlOriginal:
        m_original = !m_original;
        m_split = false;
        break;
    case kCtlClip: m_clipView = !m_clipView; break;
    case kCtlUndo:
        if (!m_undo.empty()) {
            m_redo.push_back(m_p);
            m_p = m_undo.back();
            m_undo.pop_back();
            m_lastPushId = -1;
            Changed();
        }
        break;
    case kCtlRedo:
        if (!m_redo.empty()) {
            m_undo.push_back(m_p);
            m_p = m_redo.back();
            m_redo.pop_back();
            m_lastPushId = -1;
            Changed();
        }
        break;
    case kCtlCurveReset:
        if (!m_p.curve[m_curveCh].empty()) {
            PushUndo();
            m_p.curve[m_curveCh].clear();
            Changed();
        }
        break;
    case kCtlGeoReset:
        if (!m_p.GeoIdentity()) {
            PushUndo();
            m_p.ResetGeometry();
            Changed();
        }
        break;
    case kCtlAspectFlip:
        PushUndo();
        m_p.aspectPortrait = !m_p.aspectPortrait;
        m_p.cropSet = false;
        Changed();
        break;
    case kCtlScrollbar:
        m_drag = kDragScroll;
        m_dragStart = p;
        m_dragVal = m_scroll;
        v.m_edit.drawing = true;
        SetCapture(v.m_hwnd);
        break;
    default:
        if (id >= kCtlAspect0 && id < kCtlAspect0 + dev::kAspectCount) {
            PushUndo();
            m_p.aspect = id - kCtlAspect0;
            m_p.cropSet = false;
            if (m_p.aspect < 2) m_p.aspectPortrait = false;
            Changed();
        } else if (id >= kCtlCurveCh0 && id < kCtlCurveCh0 + 4) {
            m_curveCh = id - kCtlCurveCh0;
        } else if (id >= kCtlHslTab0 && id < kCtlHslTab0 + 3) {
            m_hslTab = id - kCtlHslTab0;
        } else if (id >= kCtlSection0 && id < int(kCtlSection0) + int(kSecCount)) {
            m_open[id - int(kCtlSection0)] = !m_open[id - int(kCtlSection0)];
        }
        break;
    }
    v.Invalidate();
    return true;
}

bool Viewer::DevelopMode::MouseDown(POINT p, bool right, bool dbl) {
    if (m_drag != kDragNone) return true;
    if (const EditButton* b = v.HitEdit(p)) {
        switch (b->cmd) {
        case EditCmd::ModeCtl: return Control(b->arg, p, right, dbl);
        case EditCmd::Done: case EditCmd::SaveAs: case EditCmd::OpenWith:
            if (!right) v.EditCommand(b->cmd, 0);
            return true;
        default: return true;  // panneau
        }
    }
    if (v.m_edit.spacePan) return false;
    switch (m_tool) {
    case kToolWb:
        if (!right) PickWhiteBalance(p);
        return true;
    case kToolStraighten:
        if (right) return true;
        m_drag = kDragStraighten;
        m_dragStart = m_dragCur = p;
        v.m_edit.drawing = true;
        SetCapture(v.m_hwnd);
        v.Invalidate();
        return true;
    case kToolCrop:
        if (!right) CropDown(p, dbl);
        return true;
    default: return false;  // déplacement de la vue
    }
}

void Viewer::DevelopMode::MouseMove(POINT p) {
    switch (m_drag) {
    case kDragSlider: {
        const dev::ParamDef& d = dev::Def(m_dragId);
        const D2D1_RECT_F& tr = m_track[m_dragId];
        const float k = GetKeyState(VK_SHIFT) < 0 ? 0.1f : 1.0f;  // Maj : réglage fin
        SetValue(m_dragId, m_dragVal + (p.x - m_dragStart.x) * k * (d.max - d.min) / std::max(1.0f, tr.right - tr.left));
        break;
    }
    case kDragCurve: CurveMove(p); break;
    case kDragStraighten:
        m_dragCur = p;
        v.Invalidate();
        break;
    case kDragCropNew: case kDragCropMove: case kDragCropCorner: CropMove(p); break;
    case kDragScroll: {
        const float viewH = m_scrollRc.bottom - m_scrollRc.top, th = m_thumbRc.bottom - m_thumbRc.top;
        const float range = std::max(1.0f, viewH - th);
        m_scroll = std::clamp(m_dragVal + (p.y - m_dragStart.y) * (m_contentH - viewH) / range, 0.0f, std::max(0.0f, m_contentH - viewH));
        v.Invalidate();
        break;
    }
    default: break;
    }
}

void Viewer::DevelopMode::MouseUp(POINT) {
    if (m_drag == kDragNone) return;
    const int drag = m_drag;
    m_drag = kDragNone;
    v.m_edit.drawing = false;
    if (GetCapture() == v.m_hwnd) ReleaseCapture();
    if (drag == kDragCurve && m_curveRemove) {
        dev::Curve& c = m_p.curve[m_curveCh];
        if (m_curvePt > 0 && m_curvePt + 1 < int(c.size())) c.erase(c.begin() + m_curvePt);
    }
    m_curveRemove = false;
    m_curvePt = -1;
    if (drag == kDragStraighten) {  // ligne d'horizon (ou verticale) : rotation qui la remet droite
        const float dx = float(m_dragCur.x - m_dragStart.x), dy = float(m_dragCur.y - m_dragStart.y);
        if (dx * dx + dy * dy >= 100 * v.m_scale * v.m_scale) {
            float a = std::atan2(dy, dx) * 180 / kPi;
            if (a > 90) a -= 180;
            if (a < -90) a += 180;
            if (a > 45) a -= 90;
            else if (a < -45) a += 90;
            PushUndo();
            SetValue(dev::kAngle, std::clamp(m_p.v[dev::kAngle] - a, -45.0f, 45.0f));
            m_tool = kToolNone;
        }
    }
    if (drag != kDragScroll) Changed();
    v.Invalidate();
}

bool Viewer::DevelopMode::Wheel(POINT p, double notches) {
    if (!Contains(m_panel, p)) return false;
    if (const EditButton* b = v.HitEdit(p); b && b->cmd == EditCmd::ModeCtl && b->arg >= 0 && b->arg < dev::kParamCount) {
        const D2D1_RECT_F& tr = m_track[b->arg];
        if (p.x >= tr.left - 8 * v.m_scale) {  // molette sur la piste ou la valeur : ajuste le curseur
            const float step = dev::Def(b->arg).step * (GetKeyState(VK_SHIFT) < 0 ? 5.0f : 1.0f);
            PushUndo(b->arg);
            SetValue(b->arg, m_p.v[b->arg] + float(notches) * step);
            return true;
        }
    }
    if (Contains(m_scrollRc, p)) {
        m_scroll -= float(notches) * 60 * v.m_scale;
        v.Invalidate();
    }
    return true;
}

const wchar_t* Viewer::DevelopMode::Cursor(POINT p) const {
    if (m_drag == kDragSlider) return IDC_SIZEWE;
    if (m_drag == kDragCurve || m_drag == kDragStraighten || m_drag == kDragCropNew || m_drag == kDragCropCorner) return IDC_CROSS;
    if (m_drag == kDragCropMove || v.m_dragging || v.m_edit.spacePan) return IDC_SIZEALL;
    if (const EditButton* b = v.HitEdit(p)) {
        if (b->cmd == EditCmd::Panel) return IDC_ARROW;
        if (b->cmd == EditCmd::ModeCtl && b->arg >= 0 && b->arg < dev::kParamCount)
            return p.x >= m_track[b->arg].left - 8 * v.m_scale && p.x <= m_track[b->arg].right + 8 * v.m_scale ? IDC_SIZEWE : IDC_ARROW;
        if (b->cmd == EditCmd::ModeCtl && b->arg == kCtlCurve) return IDC_CROSS;
        return IDC_HAND;
    }
    switch (m_tool) {
    case kToolWb: case kToolStraighten: return IDC_CROSS;
    case kToolCrop: {
        const D2D1_RECT_F c = dev::CropRect(m_p, W(), H());
        const D2D1_POINT_2F ip = v.ScreenToImage(p);
        return ip.x > c.left && ip.x < c.right && ip.y > c.top && ip.y < c.bottom ? IDC_SIZEALL : IDC_CROSS;
    }
    default: return IDC_SIZEALL;
    }
}

// =============================================================================================
// Clavier, réglages copiés / collés

void Viewer::DevelopMode::CopySettings() {
    m_clipboard = m_p;
    m_hasClip = true;
    WriteParams(L"copied", m_p);
    v.ShowToast(T("develop.toast.copied"));
}

void Viewer::DevelopMode::PasteSettings() {
    Params q = m_p;
    bool ok;
    if (m_hasClip) {  // la géométrie reste celle de l'image
        q = m_clipboard;
        q.v[dev::kAngle] = m_p.v[dev::kAngle];
        q.aspect = m_p.aspect;
        q.aspectPortrait = m_p.aspectPortrait;
        q.cropSet = m_p.cropSet;
        q.crop = m_p.crop;
        ok = true;
    } else {
        ok = ReadParams(L"copied", q);
    }
    if (!ok) {
        v.ShowToast(T("develop.toast.nothing"));
        return;
    }
    if (q != m_p) {
        PushUndo();
        m_p = q;
        Changed();
    }
    v.ShowToast(T("develop.toast.pasted"));
}

void Viewer::DevelopMode::ApplyPrevious() {
    Params q = m_p;
    if (!ReadParams(L"previous", q)) {
        v.ShowToast(T("develop.toast.no_previous"));
        return;
    }
    if (q != m_p) {
        PushUndo();
        m_p = q;
        Changed();
    }
    v.ShowToast(T("develop.toast.previous"));
}

void Viewer::DevelopMode::CopyImage() {
    const bool on = BeginExport();
    v.CopyToClipboard();
    if (on) EndExport();
}

bool Viewer::DevelopMode::Key(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (ctrl && alt) {  // Ctrl+Alt (= AltGr : les autres combinaisons donnent des caractères)
        if (vk == 'C') CopySettings();
        else if (vk == 'V') PasteSettings();
        else return false;
        return true;
    }
    if (ctrl) {
        switch (vk) {
        case 'Z': Control(shift ? kCtlRedo : kCtlUndo, POINT{}, false, false); return true;
        case 'Y': Control(kCtlRedo, POINT{}, false, false); return true;
        case 'S': v.SaveAs(); return true;
        case 'C': CopyImage(); return true;
        case 'U': Control(kCtlAuto, POINT{}, false, false); return true;
        case 'R':
            if (shift) Control(kCtlResetAll, POINT{}, false, false);
            return true;
        default: return false;
        }
    }
    switch (vk) {
    case VK_SPACE: v.m_edit.spacePan = true; return true;
    case VK_ESCAPE:
        if (m_drag != kDragNone) {
            MouseUp(v.m_mouse);
            if (!m_undo.empty()) Control(kCtlUndo, POINT{}, false, false);
        } else if (m_tool != kToolNone) {
            m_tool = kToolNone;
        } else if (m_split || m_original) {
            m_split = m_original = false;
        } else {
            v.EndEdit(true);
        }
        v.Invalidate();
        return true;
    case VK_RETURN:
        if (m_tool != kToolNone) m_tool = kToolNone;
        v.Invalidate();
        return true;
    case 'Y': Control(kCtlSplit, POINT{}, false, false); return true;
    case 'J': Control(kCtlClip, POINT{}, false, false); return true;
    case 'W': case 'I': Control(kCtlWbTool, POINT{}, false, false); return true;
    case 'A': Control(kCtlStraighten, POINT{}, false, false); return true;
    case 'C': case 'R': Control(kCtlCropTool, POINT{}, false, false); return true;
    case 'E': v.EndEdit(true); return true;
    default: return false;  // navigation, zoom, plein écran : visionneuse
    }
}

// « \ » : original (caractère, quelle que soit la disposition du clavier)
bool Viewer::DevelopMode::Char(wchar_t c) {
    if (c != L'\\') return false;
    Control(kCtlOriginal, POINT{}, false, false);
    return true;
}
