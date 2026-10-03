// Mode édition : calques, historique (annuler / rétablir), sélection rectangulaire, coller comme
// calque, export PSD.
//
// Chaque calque est une texture Direct2D (cible de rendu) de la taille du document. Une texture
// présente dans l'historique n'est jamais modifiée : avant de dessiner sur un calque on le copie
// (PushUndo) ; une opération sur la pile mémorise simplement la liste des calques (textures partagées).
// Affichage : les calques en mode Normal sont dessinés directement à l'écran ; dès qu'un calque visible
// utilise un autre mode de fusion, le document est composé dans une texture (effet Blend), recomposée
// seulement quand un calque change.

#include "viewer.h"
#include "viewer_modes.h"
#include "viewer_util.h"
#include "i18n.h"

#include <d2d1effects.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

using namespace ui;

namespace {

constexpr size_t kEditBudget = 1200ull << 20;  // mémoire vidéo : calques + historique + tampons
constexpr size_t kUndoMaxSteps = 50;

const D2D1_BLEND_MODE kBlendFx[] = {D2D1_BLEND_MODE_MULTIPLY, D2D1_BLEND_MODE_MULTIPLY,   D2D1_BLEND_MODE_SCREEN,
                                    D2D1_BLEND_MODE_OVERLAY,  D2D1_BLEND_MODE_DARKEN,     D2D1_BLEND_MODE_LIGHTEN,
                                    D2D1_BLEND_MODE_DIFFERENCE, D2D1_BLEND_MODE_LINEAR_DODGE};
const char* const kBlendKeys[] = {"edit.blend.normal",  "edit.blend.multiply", "edit.blend.screen",
                                  "edit.blend.overlay", "edit.blend.darken",   "edit.blend.lighten",
                                  "edit.blend.difference", "edit.blend.add"};
const char kPsdBlend[][5] = {"norm", "mul ", "scrn", "over", "dark", "lite", "diff", "lddg"};

size_t Bytes(ID2D1Bitmap1* b) {
    if (!b) return 0;
    const D2D1_SIZE_U s = b->GetPixelSize();
    return size_t(s.width) * s.height * 4;
}

bool AnyTransparent(const uint8_t* px, uint32_t w, uint32_t h, uint32_t stride) {
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* p = px + size_t(y) * stride + 3;
        for (uint32_t x = 0; x < w; ++x, p += 4)
            if (*p != 255) return true;
    }
    return false;
}

// Image du presse-papier (PNG, sinon DIB) en BGRA prémultiplié.
bool ReadClipboardImage(HWND hwnd, PixelBuffer& px, uint32_t& w, uint32_t& h, uint32_t& stride) {
    if (!OpenClipboard(hwnd)) return false;
    std::vector<uint8_t> data;
    bool isPng = false;
    HANDLE hd = GetClipboardData(RegisterClipboardFormatW(L"PNG"));
    if (hd) isPng = true;
    else if (!(hd = GetClipboardData(CF_DIBV5))) hd = GetClipboardData(CF_DIB);
    if (hd) {
        const size_t n = GlobalSize(hd);
        if (const void* p = GlobalLock(hd)) {
            data.assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
            GlobalUnlock(hd);
        }
    }
    CloseClipboard();
    if (data.size() < sizeof(BITMAPINFOHEADER)) return false;
    if (!isPng) {  // DIB : on reconstitue un fichier BMP pour le décodeur WIC
        const auto* bi = reinterpret_cast<const BITMAPINFOHEADER*>(data.data());
        size_t off = bi->biSize;
        if (bi->biSize == sizeof(BITMAPINFOHEADER) && bi->biCompression == BI_BITFIELDS) off += 12;
        size_t colors = bi->biClrUsed ? bi->biClrUsed : (bi->biBitCount <= 8 ? size_t(1) << bi->biBitCount : 0);
        off += colors * 4;
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;
        fh.bfSize = DWORD(sizeof(fh) + data.size());
        fh.bfOffBits = DWORD(sizeof(fh) + off);
        data.insert(data.begin(), reinterpret_cast<const uint8_t*>(&fh), reinterpret_cast<const uint8_t*>(&fh) + sizeof(fh));
    }
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(data.data(), DWORD(data.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&w, &h)) || !w || !h)
        return false;
    stride = w * 4;
    if (!px.Alloc(size_t(stride) * h) || FAILED(conv->CopyPixels(nullptr, stride, UINT(size_t(stride) * h), px.Data())))
        return false;
    // DIB 32 bits dont l'alpha est inutilisé (tout à zéro) : image opaque
    bool allZero = true;
    for (size_t i = 3; i < size_t(stride) * h && allZero; i += 4) allZero = px.Data()[i] == 0;
    if (allZero) {
        if (FAILED(frame->CopyPixels(nullptr, stride, UINT(size_t(stride) * h), px.Data()))) return false;
        for (size_t i = 3; i < size_t(stride) * h; i += 4) px.Data()[i] = 255;
    }
    return true;
}

// Écriture séquentielle en gros-boutiste (format PSD)
struct BeWriter {
    FILE* f;
    bool ok = true;
    void Bytes(const void* p, size_t n) { ok = ok && fwrite(p, 1, n, f) == n; }
    void U8(uint8_t v) { Bytes(&v, 1); }
    void U16(uint16_t v) {
        uint8_t b[2] = {uint8_t(v >> 8), uint8_t(v)};
        Bytes(b, 2);
    }
    void U32(uint32_t v) {
        uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        Bytes(b, 4);
    }
    void Str(const char* s) { Bytes(s, strlen(s)); }
};

}  // namespace

// =============================================================================================
// Calques

const char* Viewer::BlendKey(int mode) { return kBlendKeys[std::clamp(mode, 0, 7)]; }

std::wstring Viewer::LayerName(const Layer& l) const {
    if (!l.name.empty()) return l.name;
    return l.num == 0 ? std::wstring(T("edit.layer.background")) : Tf("edit.layer.name", l.num);
}

ID2D1Bitmap1* Viewer::LayerImage(int i) const {
    if (i == m_edit.layer && m_edit.fx >= 0 && m_edit.fxPreview && m_edit.fxOut) return m_edit.fxOut.Get();
    if (!m_modes.empty())
        if (ID2D1Bitmap1* b = Mode().LayerBitmap(i)) return b;  // aperçu du mode (Développement)
    return m_edit.layers[size_t(i)].bmp.Get();
}

std::vector<Viewer::ComposeItem> Viewer::VisibleItems() const {
    std::vector<ComposeItem> v;
    for (int i = 0; i < int(m_edit.layers.size()); ++i) {
        const Layer& l = m_edit.layers[size_t(i)];
        if (l.visible && l.opacity > 0) v.push_back({LayerImage(i), l.opacity, l.blend});
    }
    return v;
}

bool Viewer::NeedComposite() const {
    return std::any_of(m_edit.layers.begin(), m_edit.layers.end(),
                       [](const Layer& l) { return l.visible && l.opacity > 0 && l.blend != 0; });
}

// Compose des calques dans target (effacée d'abord). xf : document -> target.
bool Viewer::ComposeTo(ID2D1Bitmap1* target, const D2D1_MATRIX_3X2_F& xf, const std::vector<ComposeItem>& items) {
    if (!m_dc || !target) return false;
    const D2D1_SIZE_U ts = target->GetPixelSize();
    const bool unscaled = xf._11 == 1 && xf._22 == 1 && xf._12 == 0 && xf._21 == 0;
    const bool identity = unscaled && xf._31 == 0 && xf._32 == 0;
    const D2D1_INTERPOLATION_MODE interp =
        unscaled ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    const bool docSized = ts.width == m_edit.w && ts.height == m_edit.h;
    ComPtr<ID2D1Bitmap1> localScratch;
    m_dc->SetTarget(target);
    m_dc->BeginDraw();
    bool inDraw = true;
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    HRESULT hr = S_OK;
    for (const ComposeItem& it : items) {
        const D2D1_SIZE_U bs = it.bmp->GetPixelSize();
        const D2D1_RECT_F src = D2D1::RectF(0, 0, float(bs.width), float(bs.height));
        if (it.blend == 0) {
            m_dc->SetTransform(xf);
            m_dc->DrawBitmap(it.bmp, src, it.opacity, interp, nullptr, nullptr);
            continue;
        }
        // Un effet ne peut pas lire sa propre cible : copie du fond composé jusqu'ici
        hr = m_dc->EndDraw();
        inDraw = false;
        if (FAILED(hr)) break;
        ComPtr<ID2D1Bitmap1>& scratch = docSized ? m_edit.scratch : localScratch;
        if (!scratch || scratch->GetPixelSize().width != ts.width || scratch->GetPixelSize().height != ts.height)
            scratch = NewCanvas(ts.width, ts.height);
        m_dc->SetTarget(target);
        if (!scratch || FAILED(scratch->CopyFromBitmap(nullptr, target, nullptr))) {
            hr = E_OUTOFMEMORY;
            break;
        }
        m_dc->BeginDraw();
        inDraw = true;
        ComPtr<ID2D1Effect> op, aff, blend;
        ComPtr<ID2D1Image> img = it.bmp;
        if (it.opacity < 1 && SUCCEEDED(m_dc->CreateEffect(CLSID_D2D1ColorMatrix, &op))) {
            D2D1_MATRIX_5X4_F m = D2D1::Matrix5x4F(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, it.opacity, 0, 0, 0, 0);
            op->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
            op->SetInput(0, img.Get());
            img.Reset();
            op->GetOutput(&img);
        }
        if (!identity && SUCCEEDED(m_dc->CreateEffect(CLSID_D2D12DAffineTransform, &aff))) {
            aff->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, xf);
            aff->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                          unscaled ? D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                   : D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            aff->SetInput(0, img.Get());
            img.Reset();
            aff->GetOutput(&img);
        }
        m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
        if (FAILED(m_dc->CreateEffect(CLSID_D2D1Blend, &blend))) {
            m_dc->DrawImage(img.Get());
            continue;
        }
        blend->SetValue(D2D1_BLEND_PROP_MODE, kBlendFx[std::clamp(it.blend, 0, 7)]);
        blend->SetInput(0, scratch.Get());  // fond (destination)
        blend->SetInput(1, img.Get());      // calque (source)
        m_dc->DrawImage(blend.Get(), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
    }
    if (inDraw) hr = m_dc->EndDraw();
    m_dc->SetTarget(m_target.Get());
    return SUCCEEDED(hr);
}

void Viewer::PrepareEditFrame() {
    if (!m_edit.active || !m_dc || m_edit.layers.empty()) return;
    if (!m_modes.empty()) Mode().Prepare();
    if (m_edit.fx >= 0 && (m_edit.fxDirty || m_edit.fx >= EffectCount())) UpdateFxPreview();  // effet de plugin : calculs à suivre

    if (NeedComposite()) {
        std::vector<uint64_t> sig{m_edit.w, m_edit.h};
        for (int i = 0; i < int(m_edit.layers.size()); ++i) {
            const Layer& l = m_edit.layers[size_t(i)];
            if (!l.visible) continue;
            sig.push_back(LayerImage(i) != l.bmp.Get() ? (m_edit.fxVer | (1ull << 63)) : l.ver);
            sig.push_back(uint64_t(l.blend) << 32 | uint32_t(std::lround(l.opacity * 10000)));
        }
        if (!m_edit.comp || m_edit.comp->GetPixelSize().width != m_edit.w || m_edit.comp->GetPixelSize().height != m_edit.h) {
            m_edit.comp = NewCanvas(m_edit.w, m_edit.h);
            m_edit.compSig.clear();
        }
        if (m_edit.comp && sig != m_edit.compSig) {
            if (ComposeTo(m_edit.comp.Get(), D2D1::Matrix3x2F::Identity(), VisibleItems())) m_edit.compSig = std::move(sig);
            else m_edit.comp.Reset();
        }
    } else if (m_edit.comp) {
        m_edit.comp.Reset();
        m_edit.scratch.Reset();
        m_edit.compSig.clear();
    }

    // Miniatures du panneau Calques (pas pendant un tracé : mises à jour au relâchement)
    if (!m_edit.showLayers || m_edit.drawing || m_edit.slider >= 0) return;
    const float s = m_scale;
    const float k = std::min(52 * s / m_edit.w, 38 * s / m_edit.h);
    const UINT32 tw = std::max(1u, UINT32(m_edit.w * k)), th = std::max(1u, UINT32(m_edit.h * k));
    std::vector<LayerThumb> next(m_edit.layers.size());
    for (size_t i = 0; i < m_edit.layers.size(); ++i) {
        const Layer& l = m_edit.layers[i];
        for (auto& t : m_edit.thumbs)
            if (t.bmp && t.ver == l.ver && t.bmp->GetPixelSize().width == tw && t.bmp->GetPixelSize().height == th) {
                next[i] = std::move(t);
                break;
            }
    }
    for (size_t i = 0; i < m_edit.layers.size(); ++i) {
        if (next[i].bmp) continue;
        LayerThumb t;
        for (auto& old : m_edit.thumbs)  // réutilise une texture libre de la bonne taille
            if (old.bmp && old.bmp->GetPixelSize().width == tw && old.bmp->GetPixelSize().height == th) {
                t.bmp = std::move(old.bmp);
                break;
            }
        if (!t.bmp) t.bmp = NewCanvas(tw, th);
        if (!t.bmp) continue;
        ID2D1Bitmap1* src = m_edit.layers[i].bmp.Get();
        DrawOn(t.bmp.Get(), [&] {
            m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            m_dc->DrawBitmap(src, D2D1::RectF(0, 0, float(tw), float(th)), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                             nullptr, nullptr);
        });
        t.ver = m_edit.layers[i].ver;
        next[i] = std::move(t);
    }
    m_edit.thumbs = std::move(next);
}

void Viewer::DrawEditDocument(const D2D1_RECT_F& r, D2D1_INTERPOLATION_MODE mode) {
    // Le mode peut transformer tout le document (redressement) et dessiner un calque à sa façon (aperçu)
    D2D1_MATRIX_3X2_F old, xf;
    const bool turned = Mode().DocTransform(r, xf);
    if (turned) {
        m_dc->GetTransform(&old);
        m_dc->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&xf) * *D2D1::Matrix3x2F::ReinterpretBaseType(&old));
    }
    if (NeedComposite() && m_edit.comp) {
        m_dc->DrawBitmap(m_edit.comp.Get(), r, 1.0f, mode, nullptr, nullptr);
    } else {
        for (int i = 0; i < int(m_edit.layers.size()); ++i) {
            const Layer& l = m_edit.layers[size_t(i)];
            if (!l.visible || l.opacity <= 0 || Mode().DrawLayer(i, r, mode, l.opacity)) continue;
            m_dc->DrawBitmap(LayerImage(i), r, l.opacity, mode, nullptr, nullptr);
        }
    }
    if (turned) m_dc->SetTransform(old);
}

// =============================================================================================
// Historique

bool Viewer::PushUndo() {
    ComPtr<ID2D1Bitmap1> copy = NewCanvas(m_edit.w, m_edit.h);
    if (!copy || FAILED(copy->CopyFromBitmap(nullptr, CurLayer().bmp.Get(), nullptr))) {
        ShowToast(T("edit.err.vram"));
        return false;
    }
    PushLayerSwap(copy);
    return true;
}

void Viewer::PushLayerSwap(ComPtr<ID2D1Bitmap1> bmp) {
    Layer& l = CurLayer();
    EditUndo e;
    e.layer = m_edit.layer;
    e.bmp = l.bmp;
    e.ver = l.ver;
    m_edit.undo.push_back(std::move(e));
    m_edit.redo.clear();
    l.bmp = std::move(bmp);
    Touch(l);
    m_edit.dirty = true;
    TrimHistory();
}

void Viewer::PushDocUndo() {
    EditUndo e;
    e.layers = m_edit.layers;
    e.cur = m_edit.layer;
    e.w = m_edit.w;
    e.h = m_edit.h;
    m_edit.undo.push_back(std::move(e));
    m_edit.redo.clear();
    m_edit.dirty = true;
    TrimHistory();
}

void Viewer::SetDocument(std::vector<ComPtr<ID2D1Bitmap1>> bmps) {
    if (bmps.size() != m_edit.layers.size() || !bmps[0]) return;
    PushDocUndo();
    for (size_t i = 0; i < bmps.size(); ++i) {
        m_edit.layers[i].bmp = std::move(bmps[i]);
        Touch(m_edit.layers[i]);
    }
    const D2D1_SIZE_U s = m_edit.layers[0].bmp->GetPixelSize();
    m_edit.w = s.width;
    m_edit.h = s.height;
    m_edit.cropValid = false;
    m_edit.selValid = false;
    ApplyFit(false);
}

size_t Viewer::EditBytes() const {
    std::unordered_set<ID2D1Bitmap1*> seen;
    size_t total = 0;
    auto add = [&](ID2D1Bitmap1* b) {
        if (b && seen.insert(b).second) total += Bytes(b);
    };
    for (auto& l : m_edit.layers) add(l.bmp.Get());
    for (auto* list : {&m_edit.undo, &m_edit.redo})
        for (auto& e : *list) {
            add(e.bmp.Get());
            for (auto& l : e.layers) add(l.bmp.Get());
        }
    add(m_edit.strokeBuf.Get());
    add(m_edit.comp.Get());
    add(m_edit.scratch.Get());
    add(m_edit.fxOut.Get());
    return total;
}

// Budget : on oublie les plus vieux états (le dernier est toujours gardé)
void Viewer::TrimHistory() {
    while (m_edit.undo.size() > kUndoMaxSteps) m_edit.undo.erase(m_edit.undo.begin());
    while (m_edit.undo.size() > 1 && EditBytes() > kEditBudget) m_edit.undo.erase(m_edit.undo.begin());
}

bool Viewer::CanAddLayer() {
    std::unordered_set<ID2D1Bitmap1*> seen;
    size_t total = Bytes(m_edit.strokeBuf.Get()) + Bytes(m_edit.comp.Get()) + Bytes(m_edit.scratch.Get());
    for (auto& l : m_edit.layers)
        if (seen.insert(l.bmp.Get()).second) total += Bytes(l.bmp.Get());
    if (total + size_t(m_edit.w) * m_edit.h * 4 > kEditBudget) {
        ShowToast(T("edit.err.layer_budget"));
        return false;
    }
    return true;
}

void Viewer::RestoreUndo(EditUndo& e, std::vector<EditUndo>& opposite) {
    EditUndo back;
    if (e.layer >= 0) {
        if (e.layer >= int(m_edit.layers.size())) return;
        Layer& l = m_edit.layers[size_t(e.layer)];
        back.layer = e.layer;
        back.bmp = l.bmp;
        back.ver = l.ver;
        l.bmp = e.bmp;
        l.ver = e.ver;
    } else {
        back.layers = m_edit.layers;
        back.cur = m_edit.layer;
        back.w = m_edit.w;
        back.h = m_edit.h;
        const bool resized = e.w != m_edit.w || e.h != m_edit.h;
        m_edit.layers = std::move(e.layers);
        m_edit.layer = std::clamp(e.cur, 0, int(m_edit.layers.size()) - 1);
        m_edit.w = e.w;
        m_edit.h = e.h;
        if (resized) {
            m_edit.selValid = false;
            ApplyFit(false);
        }
    }
    opposite.push_back(std::move(back));
    m_edit.dirty = true;
    m_edit.cropValid = false;
    Invalidate();
}

void Viewer::Undo() {
    if (m_edit.renaming >= 0) {
        m_edit.renaming = -1;
        Invalidate();
        return;
    }
    if (m_edit.fx >= 0) {
        CloseEffect(false);
        return;
    }
    if (m_edit.textActive) {
        m_edit.textActive = false;
        m_edit.text.clear();
        Invalidate();
        return;
    }
    if (m_edit.undo.empty() || m_edit.drawing) return;
    EditUndo e = std::move(m_edit.undo.back());
    m_edit.undo.pop_back();
    RestoreUndo(e, m_edit.redo);
}

void Viewer::Redo() {
    if (m_edit.redo.empty() || m_edit.drawing || m_edit.fx >= 0) return;
    EditUndo e = std::move(m_edit.redo.back());
    m_edit.redo.pop_back();
    RestoreUndo(e, m_edit.undo);
}

// =============================================================================================
// Lecture des pixels

bool Viewer::ReadBitmap(ID2D1Bitmap1* bmp, PixelBuffer& out, uint32_t& stride, const D2D1_RECT_U* rc) {
    if (!bmp || !m_dc) return false;
    const D2D1_SIZE_U bs = bmp->GetPixelSize();
    const D2D1_RECT_U r = rc ? *rc : D2D1::RectU(0, 0, bs.width, bs.height);
    const uint32_t w = r.right - r.left, h = r.bottom - r.top;
    if (!w || !h) return false;
    D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1Bitmap1> rb;
    D2D1_POINT_2U origin = D2D1::Point2U(0, 0);
    if (FAILED(m_dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, p, &rb)) || FAILED(rb->CopyFromBitmap(&origin, bmp, &r)))
        return false;
    D2D1_MAPPED_RECT m{};
    if (FAILED(rb->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
    stride = w * 4;
    bool ok = out.Alloc(size_t(stride) * h);
    if (ok)
        for (uint32_t y = 0; y < h; ++y) memcpy(out.Data() + size_t(y) * stride, m.bits + size_t(y) * m.pitch, stride);
    rb->Unmap();
    return ok;
}

// Image aplatie (opacités et modes de fusion appliqués). alpha : au moins un pixel non opaque.
bool Viewer::ReadFlattened(PixelBuffer& out, uint32_t& stride, bool& alpha) {
    if (m_edit.layers.empty()) return false;
    const std::vector<ComposeItem> items = VisibleItems();
    ID2D1Bitmap1* src = nullptr;
    ComPtr<ID2D1Bitmap1> tmp;
    if (items.size() == 1 && items[0].opacity >= 1 && items[0].blend == 0) {
        src = items[0].bmp;
    } else {
        tmp = NewCanvas(m_edit.w, m_edit.h);
        if (!tmp || !ComposeTo(tmp.Get(), D2D1::Matrix3x2F::Identity(), items)) return false;
        src = tmp.Get();
    }
    if (!ReadBitmap(src, out, stride)) return false;
    alpha = AnyTransparent(out.Data(), m_edit.w, m_edit.h, stride);
    return true;
}

// =============================================================================================
// Opérations sur la pile

void Viewer::AddLayer(ComPtr<ID2D1Bitmap1> bmp, std::wstring name) {
    Layer l;
    l.bmp = std::move(bmp);
    l.num = ++m_edit.layerSeq;
    l.name = std::move(name);
    Touch(l);
    PushDocUndo();
    m_edit.layers.insert(m_edit.layers.begin() + m_edit.layer + 1, std::move(l));
    ++m_edit.layer;
    Invalidate();
}

void Viewer::LayerCommand(EditCmd cmd, int arg) {
    if (m_edit.textActive) CommitText();
    const int n = int(m_edit.layers.size());
    const int cur = m_edit.layer;
    switch (cmd) {
    case EditCmd::LayerSelect:
        if (arg >= 0 && arg < n) m_edit.layer = arg;
        break;
    case EditCmd::LayerEye:
        if (arg >= 0 && arg < n) {
            m_edit.layers[size_t(arg)].visible = !m_edit.layers[size_t(arg)].visible;
            m_edit.dirty = true;
        }
        break;
    case EditCmd::LayerNew: {
        if (!CanAddLayer()) break;
        ComPtr<ID2D1Bitmap1> b = NewCanvas(m_edit.w, m_edit.h);
        if (!b || !DrawOn(b.Get(), [&] { m_dc->Clear(D2D1::ColorF(0, 0, 0, 0)); })) {
            ShowToast(T("edit.err.vram"));
            break;
        }
        AddLayer(b, {});
        break;
    }
    case EditCmd::LayerDup: {
        if (!CanAddLayer()) break;
        ComPtr<ID2D1Bitmap1> b = NewCanvas(m_edit.w, m_edit.h);
        if (!b || FAILED(b->CopyFromBitmap(nullptr, CurLayer().bmp.Get(), nullptr))) {
            ShowToast(T("edit.err.vram"));
            break;
        }
        Layer l = CurLayer();
        l.bmp = b;
        l.name = Tf("edit.layer.copy_name", LayerName(CurLayer()).c_str());
        l.num = ++m_edit.layerSeq;
        Touch(l);
        PushDocUndo();
        m_edit.layers.insert(m_edit.layers.begin() + cur + 1, std::move(l));
        ++m_edit.layer;
        break;
    }
    case EditCmd::LayerDel:
        if (n <= 1) {
            ShowToast(T("edit.msg.last_layer"));
            break;
        }
        PushDocUndo();
        m_edit.layers.erase(m_edit.layers.begin() + cur);
        m_edit.layer = std::max(0, cur - 1);
        break;
    case EditCmd::LayerUp:
    case EditCmd::LayerDown: {
        const int other = cmd == EditCmd::LayerUp ? cur + 1 : cur - 1;
        if (other < 0 || other >= n) break;
        PushDocUndo();
        std::swap(m_edit.layers[size_t(cur)], m_edit.layers[size_t(other)]);
        m_edit.layer = other;
        break;
    }
    case EditCmd::LayerMerge: {
        if (cur <= 0) break;
        const Layer& up = m_edit.layers[size_t(cur)];
        Layer& low = m_edit.layers[size_t(cur - 1)];
        ComPtr<ID2D1Bitmap1> b = NewCanvas(m_edit.w, m_edit.h);
        std::vector<ComposeItem> items{{low.bmp.Get(), 1.0f, 0}};
        if (up.visible) items.push_back({up.bmp.Get(), up.opacity, up.blend});
        if (!b || !ComposeTo(b.Get(), D2D1::Matrix3x2F::Identity(), items)) {
            ShowToast(T("edit.err.vram"));
            break;
        }
        PushDocUndo();
        m_edit.layers[size_t(cur - 1)].bmp = b;
        Touch(m_edit.layers[size_t(cur - 1)]);
        m_edit.layers.erase(m_edit.layers.begin() + cur);
        m_edit.layer = cur - 1;
        break;
    }
    case EditCmd::LayerFlatten: {
        if (n <= 1) break;
        ComPtr<ID2D1Bitmap1> b = NewCanvas(m_edit.w, m_edit.h);
        if (!b || !ComposeTo(b.Get(), D2D1::Matrix3x2F::Identity(), VisibleItems())) {
            ShowToast(T("edit.err.vram"));
            break;
        }
        PushDocUndo();
        Layer l;
        l.bmp = b;
        Touch(l);
        m_edit.layers.assign(1, std::move(l));
        m_edit.layer = 0;
        break;
    }
    case EditCmd::BlendOpt:
        m_edit.menu = EditMenu::None;
        if (arg != CurLayer().blend) {
            PushDocUndo();
            CurLayer().blend = std::clamp(arg, 0, 7);
        }
        break;
    default: break;
    }
    m_edit.layerScroll = std::clamp(m_edit.layerScroll, 0, std::max(0, int(m_edit.layers.size()) - 1));
    Invalidate();
}

// =============================================================================================
// Panneau Calques (à droite)

D2D1_RECT_F Viewer::LayersPanelRect(float cw, float ch) const {
    const float s = m_scale;
    return D2D1::RectF(cw - 246 * s, 100 * s, cw - 10 * s, std::max(160 * s, ch - 12 * s));
}

void Viewer::DrawLayersPanel(float cw, float ch) {
    const float s = m_scale, pad = 10 * s;
    const D2D1_RECT_F p = LayersPanelRect(cw, ch);
    FillRound(p, 8 * s, kPanel, 0.95f);
    EditBtn(EditCmd::Panel, 0, p, nullptr);  // le fond absorbe les clics
    Text(T("edit.layers.title"), m_tfTitle.Get(), D2D1::RectF(p.left + pad, p.top + 6 * s, p.right - pad, p.top + 30 * s), kWhite, 1);
    const Layer& cl = CurLayer();

    // Mode de fusion et opacité du calque actif
    float y = p.top + 36 * s;
    D2D1_RECT_F bm = D2D1::RectF(p.left + pad, y, p.right - pad, y + 28 * s);
    EditBtn(EditCmd::BlendMenu, 0, bm, T("edit.tip.blend"), m_edit.menu == EditMenu::Blend);
    SetColor(kWhite, 0.18f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(bm, 5 * s, 5 * s), m_brush.Get(), 1);
    Text(T(BlendKey(cl.blend)), m_tfSmall.Get(), D2D1::RectF(bm.left + 10 * s, bm.top, bm.right - 24 * s, bm.bottom), kWhite, 0.95f);
    Text(L"", m_tfIcon.Get(), D2D1::RectF(bm.right - 26 * s, bm.top, bm.right - 2 * s, bm.bottom), kGrey, 1);
    if (m_edit.menu == EditMenu::Blend) m_edit.menuAnchor = bm;
    y = bm.bottom + 6 * s;
    Text(T("edit.label.opacity"), m_tfSmall.Get(), D2D1::RectF(p.left + pad, y, p.left + pad + 70 * s, y + 24 * s), kGrey, 1);
    wchar_t vb[32];
    swprintf_s(vb, L"%.0f %%", cl.opacity * 100);
    Text(vb, m_tfSmall.Get(), D2D1::RectF(p.right - pad - 44 * s, y, p.right - pad, y + 24 * s), kWhite, 0.95f);
    DrawSlider(kSlLayerOpacity, D2D1::RectF(p.left + pad + 72 * s, y + 4 * s, p.right - pad - 58 * s, y + 20 * s), T("edit.tip.layer_opacity"));
    y += 30 * s;
    SetColor(kWhite, 0.08f);
    m_rt->FillRectangle(D2D1::RectF(p.left + pad, y, p.right - pad, y + 1), m_brush.Get());
    y += 4 * s;

    // Liste (calque du haut en premier)
    const float rowH = 46 * s, btnH = 34 * s;
    const float listBottom = p.bottom - btnH - 10 * s;
    const int n = int(m_edit.layers.size());
    const int rows = std::max(1, int((listBottom - y) / rowH));
    m_edit.layerScroll = std::clamp(m_edit.layerScroll, 0, std::max(0, n - rows));
    m_rt->PushAxisAlignedClip(D2D1::RectF(p.left, y, p.right, listBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int k = 0; k < rows && k + m_edit.layerScroll < n; ++k) {
        const int i = n - 1 - (k + m_edit.layerScroll);
        const Layer& l = m_edit.layers[size_t(i)];
        const D2D1_RECT_F row = D2D1::RectF(p.left + 4 * s, y + k * rowH, p.right - 4 * s, y + (k + 1) * rowH - 2 * s);
        const bool active = i == m_edit.layer;
        EditBtn(EditCmd::LayerSelect, i, row, T("edit.tip.layer_select"), active);
        // œil
        const D2D1_RECT_F eye = D2D1::RectF(row.left + 2 * s, row.top, row.left + 30 * s, row.bottom);
        EditBtn(EditCmd::LayerEye, i, eye, T("edit.tip.layer_eye"));
        Text(L"", m_tfIcon.Get(), eye, kWhite, l.visible ? 0.9f : 0.2f);
        // miniature
        const D2D1_RECT_F tb = D2D1::RectF(eye.right + 2 * s, row.top + 4 * s, eye.right + 56 * s, row.bottom - 4 * s);
        if (size_t(i) < m_edit.thumbs.size() && m_edit.thumbs[size_t(i)].bmp) {
            ID2D1Bitmap1* t = m_edit.thumbs[size_t(i)].bmp.Get();
            const D2D1_SIZE_U ts = t->GetPixelSize();
            const float k2 = std::min((tb.right - tb.left) / ts.width, (tb.bottom - tb.top) / ts.height);
            const float w = ts.width * k2, h = ts.height * k2;
            const float l2 = std::round((tb.left + tb.right - w) / 2), t2 = std::round((tb.top + tb.bottom - h) / 2);
            const D2D1_RECT_F r = D2D1::RectF(l2, t2, l2 + w, t2 + h);
            if (m_checker) m_rt->FillRectangle(r, m_checker.Get());
            m_dc->DrawBitmap(t, r, l.visible ? 1.0f : 0.4f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            SetColor(active ? kAccent : kWhite, active ? 0.9f : 0.2f);
            m_rt->DrawRectangle(r, m_brush.Get(), 1);
        }
        // nom (ou zone de saisie du renommage) et mode / opacité
        const D2D1_RECT_F nr = D2D1::RectF(tb.right + 8 * s, row.top + 4 * s, row.right - 4 * s, row.top + 24 * s);
        if (m_edit.renaming == i) {
            const D2D1_RECT_F box = D2D1::RectF(nr.left - 4 * s, nr.top - 1 * s, nr.right, nr.bottom + 2 * s);
            FillRound(box, 3 * s, 0x000000, 0.6f);
            SetColor(kAccent, 1);
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3 * s, 3 * s), m_brush.Get(), 1);
            const bool caret = std::fmod(NowSeconds(), 1.0) < 0.6;
            Text(m_edit.renameText + (caret ? L"|" : L""), m_tfSmall.Get(), nr, kWhite, 1);
        } else {
            Text(LayerName(l), m_tfSmall.Get(), nr, active ? kAccent : kWhite, l.visible ? 1.0f : 0.5f);
        }
        std::wstring sub;
        if (l.blend != 0) sub = T(BlendKey(l.blend));
        if (l.opacity < 0.995f) {
            wchar_t ob[24];
            swprintf_s(ob, L"%.0f %%", l.opacity * 100);
            sub += (sub.empty() ? L"" : L" · ") + std::wstring(ob);
        }
        if (!sub.empty())
            Text(sub, m_tfSmall.Get(), D2D1::RectF(nr.left, nr.bottom, nr.right, row.bottom - 2 * s), kGrey, 0.8f);
    }
    m_rt->PopAxisAlignedClip();
    if (n > rows) {  // barre de défilement
        const float th = (listBottom - y) * rows / n, ty = y + (listBottom - y - th) * m_edit.layerScroll / std::max(1, n - rows);
        FillRound(D2D1::RectF(p.right - 5 * s, ty, p.right - 2 * s, ty + th), 1.5f * s, kWhite, 0.3f);
    }

    // Boutons
    struct B { EditCmd cmd; const wchar_t* glyph; const char* tip; bool enabled; };
    const B btns[] = {
        {EditCmd::LayerNew, L"", "edit.tip.layer_new", true},
        {EditCmd::LayerDup, L"", "edit.tip.layer_dup", true},
        {EditCmd::LayerUp, L"", "edit.tip.layer_up", m_edit.layer < n - 1},
        {EditCmd::LayerDown, L"", "edit.tip.layer_down", m_edit.layer > 0},
        {EditCmd::LayerMerge, nullptr, "edit.tip.layer_merge", m_edit.layer > 0},
        {EditCmd::LayerFlatten, L"", "edit.tip.layer_flatten", n > 1},
        {EditCmd::LayerDel, L"", "edit.tip.layer_del", n > 1},
    };
    const float bw = std::min(32 * s, (p.right - p.left - 2 * pad) / ARRAYSIZE(btns));
    float bx = (p.left + p.right) / 2 - bw * ARRAYSIZE(btns) / 2;
    const float by = p.bottom - btnH - 6 * s;
    for (const B& b : btns) {
        const D2D1_RECT_F r = D2D1::RectF(bx, by, bx + bw, by + btnH);
        EditBtn(b.cmd, 0, r, T(b.tip), false, b.enabled);
        const float a = b.enabled ? 0.92f : 0.28f;
        if (b.glyph) {
            Text(b.glyph, m_tfIcon.Get(), r, kWhite, a);
        } else {  // fusionner : flèche vers une barre
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            SetColor(kWhite, a);
            m_rt->DrawLine(D2D1::Point2F(cx, cy - 7 * s), D2D1::Point2F(cx, cy + 3 * s), m_brush.Get(), 1.5f * s);
            m_rt->DrawLine(D2D1::Point2F(cx - 4 * s, cy - 1 * s), D2D1::Point2F(cx, cy + 3 * s), m_brush.Get(), 1.5f * s);
            m_rt->DrawLine(D2D1::Point2F(cx + 4 * s, cy - 1 * s), D2D1::Point2F(cx, cy + 3 * s), m_brush.Get(), 1.5f * s);
            m_rt->DrawLine(D2D1::Point2F(cx - 7 * s, cy + 7 * s), D2D1::Point2F(cx + 7 * s, cy + 7 * s), m_brush.Get(), 1.5f * s);
        }
        bx += bw;
    }
}

// =============================================================================================
// Sélection, presse-papier

void Viewer::SelectAll() {
    m_edit.sel = D2D1::RectF(0, 0, float(m_edit.w), float(m_edit.h));
    m_edit.selValid = true;
    Invalidate();
}

void Viewer::Deselect() {
    m_edit.selValid = false;
    Invalidate();
}

// Suppr : efface la sélection du calque actif (couleur secondaire sur le fond d'une image opaque)
void Viewer::ClearSelection() {
    if (!m_edit.selValid || !PushUndo()) return;
    const bool toColor = ErasesToColor();
    DrawOnLayer([&] { m_dc->Clear(toColor ? Rgb(m_edit.color2) : D2D1::ColorF(0, 0, 0, 0)); });
    Invalidate();
}

void Viewer::PasteAsLayer() {
    if (m_edit.textActive) CommitText();
    PixelBuffer px;
    uint32_t w = 0, h = 0, stride = 0;
    if (!ReadClipboardImage(m_hwnd, px, w, h, stride)) {
        ShowToast(T("edit.msg.paste_empty"));
        return;
    }
    if (!CanAddLayer()) return;
    // Position : coin de la sélection, sinon centré ; ce qui dépasse du document est perdu
    int x0 = m_edit.selValid ? int(m_edit.sel.left) : (int(m_edit.w) - int(w)) / 2;
    int y0 = m_edit.selValid ? int(m_edit.sel.top) : (int(m_edit.h) - int(h)) / 2;
    const int l = std::max(0, x0), t = std::max(0, y0);
    const int r = std::min(int(m_edit.w), x0 + int(w)), b = std::min(int(m_edit.h), y0 + int(h));
    ComPtr<ID2D1Bitmap1> bmp = NewCanvas(m_edit.w, m_edit.h);
    if (!bmp || !DrawOn(bmp.Get(), [&] { m_dc->Clear(D2D1::ColorF(0, 0, 0, 0)); })) {
        ShowToast(T("edit.err.vram"));
        return;
    }
    if (r > l && b > t) {
        const D2D1_RECT_U dst = D2D1::RectU(UINT32(l), UINT32(t), UINT32(r), UINT32(b));
        const uint8_t* src = px.Data() + size_t(t - y0) * stride + size_t(l - x0) * 4;
        bmp->CopyFromMemory(&dst, src, stride);
    }
    AddLayer(bmp, {});
    SetTool(Tool::Move);
    ShowToast(T("edit.msg.pasted"));
}

// =============================================================================================
// Export Photoshop (.psd) : calques RVBA non compressés + image composée

bool Viewer::WritePsd(const std::wstring& path, std::wstring& err) {
    const uint32_t W = m_edit.w, H = m_edit.h;
    if (W > 30000 || H > 30000) {
        err = T("edit.err.psd_size");
        return false;
    }
    const size_t plane = size_t(W) * H;
    const int n = int(m_edit.layers.size());
    // Image composée (lue d'abord : on sait s'il y a de la transparence)
    PixelBuffer merged;
    uint32_t mstride = 0;
    bool alpha = false;
    if (!ReadFlattened(merged, mstride, alpha)) {
        err = T("edit.err.read");
        return false;
    }
    std::vector<std::string> names(static_cast<size_t>(n));
    std::vector<std::wstring> wnames(static_cast<size_t>(n));
    size_t recordsLen = 0;
    auto extraLen = [&](int i) {
        const size_t pas = (1 + names[size_t(i)].size() + 3) / 4 * 4;
        const size_t luni = (4 + wnames[size_t(i)].size() * 2 + 3) / 4 * 4;
        return 4 + 4 + pas + 12 + luni;
    };
    for (int i = 0; i < n; ++i) {
        wnames[size_t(i)] = LayerName(m_edit.layers[size_t(i)]);
        char buf[256];
        int len = WideCharToMultiByte(CP_ACP, 0, wnames[size_t(i)].c_str(), int(wnames[size_t(i)].size()), buf, 255, nullptr, nullptr);
        names[size_t(i)].assign(buf, size_t(std::max(0, len)));
        recordsLen += 16 + 2 + 4 * 6 + 12 + 4 + extraLen(i);
    }
    const uint64_t channelData = uint64_t(n) * 4 * (2 + plane);
    uint64_t layerInfoLen = 2 + recordsLen + channelData;
    if (layerInfoLen & 1) ++layerInfoLen;
    const uint64_t sectionLen = 4 + layerInfoLen + 4;
    if (sectionLen > 0xFFFFFFF0ull) {
        err = T("edit.err.psd_size");
        return false;
    }

    const std::wstring tmp = path + L".cyimg-tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) {
        err = T("save.err.cannot_write");
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 1 << 20);
    BeWriter o{f};
    const int channels = alpha ? 4 : 3;
    o.Str("8BPS");
    o.U16(1);
    o.Bytes("\0\0\0\0\0\0", 6);
    o.U16(uint16_t(channels));
    o.U32(H);
    o.U32(W);
    o.U16(8);
    o.U16(3);  // RVB
    o.U32(0);  // palette
    o.U32(0);  // ressources
    o.U32(uint32_t(sectionLen));
    o.U32(uint32_t(layerInfoLen));
    o.U16(uint16_t(int16_t(alpha ? -n : n)));  // négatif : le 1er canal alpha est la transparence de l'image composée
    for (int i = 0; i < n; ++i) {  // enregistrements, du bas vers le haut
        const Layer& l = m_edit.layers[size_t(i)];
        o.U32(0);
        o.U32(0);
        o.U32(H);
        o.U32(W);
        o.U16(4);
        for (int16_t id : {int16_t(-1), int16_t(0), int16_t(1), int16_t(2)}) {
            o.U16(uint16_t(id));
            o.U32(uint32_t(2 + plane));
        }
        o.Str("8BIM");
        o.Bytes(kPsdBlend[std::clamp(l.blend, 0, 7)], 4);
        o.U8(uint8_t(std::lround(l.opacity * 255)));
        o.U8(0);
        o.U8(l.visible ? 0 : 2);
        o.U8(0);
        o.U32(uint32_t(extraLen(i)));
        o.U32(0);  // masque
        o.U32(0);  // plages de fusion
        const std::string& nm = names[size_t(i)];
        o.U8(uint8_t(nm.size()));
        o.Bytes(nm.data(), nm.size());
        for (size_t k = 1 + nm.size(); k % 4; ++k) o.U8(0);
        const std::wstring& wn = wnames[size_t(i)];
        const size_t luni = (4 + wn.size() * 2 + 3) / 4 * 4;
        o.Str("8BIMluni");
        o.U32(uint32_t(luni));
        o.U32(uint32_t(wn.size()));
        for (wchar_t c : wn) o.U16(uint16_t(c));
        for (size_t k = 4 + wn.size() * 2; k < luni; ++k) o.U8(0);
    }
    // Plans de chaque calque : alpha, R, V, B (couleurs non prémultipliées)
    std::vector<uint8_t> row(W);
    auto writePlanes = [&](const uint8_t* px, uint32_t stride, const int* order, int count, bool withCompression) {
        for (int c = 0; c < count; ++c) {
            if (withCompression) o.U16(0);
            const int ch = order[c];  // 3 = alpha, 2 = R, 1 = V, 0 = B (ordre BGRA en mémoire)
            for (uint32_t y = 0; y < H && o.ok; ++y) {
                const uint8_t* p = px + size_t(y) * stride;
                for (uint32_t x = 0; x < W; ++x, p += 4) {
                    const uint32_t a = p[3];
                    if (ch == 3) row[x] = uint8_t(a);
                    else row[x] = a == 0 ? 0 : uint8_t(std::min<uint32_t>(255, (p[ch] * 255 + a / 2) / a));
                }
                o.Bytes(row.data(), W);
            }
        }
    };
    bool ok = true;
    for (int i = 0; i < n && ok; ++i) {
        PixelBuffer px;
        uint32_t stride = 0;
        if (!ReadBitmap(m_edit.layers[size_t(i)].bmp.Get(), px, stride)) {
            ok = false;
            break;
        }
        const int order[] = {3, 2, 1, 0};
        writePlanes(px.Data(), stride, order, 4, true);
    }
    if (((2 + recordsLen + channelData) & 1)) o.U8(0);
    o.U32(0);  // masque global
    // Image composée : R, V, B (+ alpha), non compressée
    o.U16(0);
    const int morder[] = {2, 1, 0, 3};
    writePlanes(merged.Data(), mstride, morder, channels, false);
    ok = ok && o.ok;
    if (fclose(f)) ok = false;
    if (ok && !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = T("save.err.replace");
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!ok) {
        DeleteFileW(tmp.c_str());
        if (err.empty()) err = T("save.err.cannot_write");
    }
    return ok;
}

// =============================================================================================
// Perte du device : les textures du document disparaissent avec lui

void Viewer::DropEditGpu() {
    if (!m_edit.active) return;
    EditState keep;
    keep.tool = m_edit.tool;
    keep.ptool = m_edit.ptool;
    keep.color = m_edit.color;
    keep.color2 = m_edit.color2;
    keep.size = m_edit.size;
    keep.fontSize = m_edit.fontSize;
    keep.opacity = m_edit.opacity;
    keep.hardness = m_edit.hardness;
    keep.fill = m_edit.fill;
    keep.gradTransparent = m_edit.gradTransparent;
    m_edit = std::move(keep);
    for (auto& m : m_modes) m->DropGpu();
    m_editHover.reset();
    m_editButtons.clear();
    m_prevEditButtons.clear();
    ShowToast(T("viewer.toast.device_lost"));
}
