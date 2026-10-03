// Mode édition (« Photoshop simplifié ») : outils, interface, et « Enregistrer sous ».
//
// Le document est une pile de calques (viewer_layers.cpp), textures Direct2D aux dimensions de
// l'image : pinceau, formes, dégradés et texte y sont dessinés directement par le GPU, avec
// antialiasing. L'orientation EXIF et la rotation d'affichage sont intégrées aux pixels à l'entrée du
// mode. Pinceau et gomme : chaque segment est tracé dans un tampon (mélange MAX, donc ni surépaisseur
// aux jointures ni accumulation de l'opacité), puis seule la zone touchée du calque est recomposée
// depuis son état d'avant le tracé.

#include "viewer.h"
#include "viewer_util.h"
#include "i18n.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ui;

namespace {

const uint32_t kPalette[16] = {0x000000, 0x7F7F7F, 0xC3C3C3, 0xFFFFFF, 0x880015, 0xE53935, 0xFF7F27, 0xFFC90E,
                               0x22B14C, 0x00A2E8, 0x3F48CC, 0xA349A4, 0xB97A57, 0xFFAEC9, 0x99D9EA, 0x7092BE};

struct ResizeOption {
    bool percent;  // libellé « 25 % » ou « Grand côté 800 px », traduit à l'affichage
    int value;
};
const ResizeOption kResize[] = {
    {true, 25},    {true, 50},     {true, 75},     {true, 150},    {true, 200},
    {false, 800},  {false, 1280},  {false, 1920},  {false, 2560},  {false, 3840},
};

struct ToolDef {
    int tool;         // Viewer::Tool (privé : on passe par int)
    const char* tip;  // clé de traduction (la langue peut changer en cours d'exécution)
};
const ToolDef kTools[] = {
    {12, "edit.tool.move"},  {11, "edit.tool.select"}, {10, "edit.tool.crop"},     {9, "edit.tool.picker"},
    {1, "edit.tool.brush"},  {2, "edit.tool.eraser"},  {8, "edit.tool.fill"},      {13, "edit.tool.gradient"},
    {7, "edit.tool.text"},   {3, "edit.tool.line"},    {4, "edit.tool.arrow"},     {5, "edit.tool.rect"},
    {6, "edit.tool.ellipse"}, {0, "edit.tool.hand"},
};

D2D1_RECT_F Norm(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    return D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
}

D2D1_RECT_F Intersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return D2D1::RectF(std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom));
}

// Opacité d'outil sur un groupe de primitives (formes, texte) : couche Direct2D
void PushOpacity(ID2D1DeviceContext* dc, float op) {
    if (op < 0.999f)
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), op),
                      nullptr);
}
void PopOpacity(ID2D1DeviceContext* dc, float op) {
    if (op < 0.999f) dc->PopLayer();
}

}  // namespace

// =============================================================================================
// Enregistrer sous

void Viewer::ForgetFile(const std::wstring& path) {
    const std::wstring key = Key(path);
    m_fullCache.erase(key);
    m_failed.erase(key);
    m_thumbCache.erase(key);
    m_gpuThumbs.erase(key);
    m_gpu.erase(std::remove_if(m_gpu.begin(), m_gpu.end(), [&](const GpuImage& g) { return g.key == key; }), m_gpu.end());
}

// Relit le dossier (nouveau fichier enregistré) sans changer d'image ni quitter l'édition.
void Viewer::ReloadList() {
    if (m_folder.empty()) return;
    const std::wstring curKey = Current() ? Key(Current()->path) : std::wstring();
    std::vector<FileEntry> files = ListImages(m_folder, m_decoder);
    if (!curKey.empty() && std::none_of(files.begin(), files.end(), [&](const FileEntry& e) { return Key(e.path) == curKey; }))
        files.push_back(*Current());  // fichier courant hors liste (extension inconnue)
    m_files = std::move(files);
    m_pathIndex.clear();
    for (int i = 0; i < int(m_files.size()); ++i) m_pathIndex[Key(m_files[i].path)] = i;
    auto it = m_pathIndex.find(curKey);
    m_index = it != m_pathIndex.end() ? it->second : std::min(m_index, int(m_files.size()) - 1);
    m_stripPos = m_stripPosT = m_index;
    m_thumbReqLo = -1;
    RequestThumbs();
    Invalidate();
}

void Viewer::SaveAs() {
    const FileEntry* cur = Current();
    if (!cur || (!m_cur && !m_edit.active)) {
        ShowToast(T("edit.save.no_image"));
        return;
    }
    if (m_edit.textActive) CommitText();
    if (m_edit.fx >= 0) CloseEffect(false);
    if (m_saveFormats.empty()) m_saveFormats = EnumerateSaveFormats();
    if (m_saveFormats.empty()) {
        MessageBoxW(m_hwnd, T("edit.save.no_encoder"), L"CyImgView", MB_ICONERROR);
        return;
    }

    // Pixels tels qu'affichés : document édité aplati, ou image orientée (EXIF + rotation manuelle)
    PixelBuffer px;
    uint32_t w = 0, h = 0, stride = 0;
    bool alpha = false;
    if (m_edit.active) {
        if (!ReadFlattened(px, stride, alpha)) {
            ShowToast(T("edit.err.read"));
            return;
        }
        w = m_edit.w;
        h = m_edit.h;
    } else {
        bool flip;
        int rot;
        FlipRot(flip, rot);
        if (!OrientPixels(CurPixels(), m_cur->width, m_cur->height, m_cur->stride, flip, rot, px, w, h, stride)) {
            ShowToast(T("edit.err.memory"));
            return;
        }
        alpha = m_cur->hasAlpha;
    }

    // Plusieurs calques : Photoshop (.psd) proposé en plus, pour les garder
    std::vector<SaveFormat> formats = m_saveFormats;
    const bool layered = m_edit.active && m_edit.layers.size() > 1;
    if (layered) {
        SaveFormat psd;
        psd.name = T("edit.save.psd");
        psd.exts = {L"psd"};
        psd.alpha = true;
        formats.push_back(std::move(psd));
    }

    // Proposition : même nom, même format si on sait l'écrire, sinon JPEG (ou PNG si transparence)
    std::wstring ext = ExtensionOf(cur->path);
    bool known = std::any_of(formats.begin(), formats.end(), [&](const SaveFormat& f) {
        return std::find(f.exts.begin(), f.exts.end(), ext) != f.exts.end();
    });
    if (!known) ext = alpha ? L"png" : L"jpg";
    std::wstring base = cur->name;
    if (size_t dot = base.find_last_of(L'.'); dot != std::wstring::npos) base.resize(dot);

    SaveRequest req;
    if (!ShowSaveDialog(m_hwnd, formats, m_folder, base, ext, req)) return;
    std::wstring err;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = layered && req.format == int(formats.size()) - 1
                  ? WritePsd(req.path, err)
                  : SaveImage(req, formats[size_t(req.format)], px.Data(), w, h, stride, alpha, err);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!ok) {
        MessageBoxW(m_hwnd, Tf("edit.save.failed", err.c_str()).c_str(), L"CyImgView", MB_ICONERROR);
        return;
    }
    ShowToast(Tf("edit.save.done", FileNameOf(req.path).c_str()));
    if (m_edit.active) m_edit.dirty = false;
    ForgetFile(req.path);
    const bool overwroteCurrent = Key(req.path) == Key(cur->path);
    if (Key(ParentOf(req.path)) == Key(m_folder)) ReloadList();
    if (overwroteCurrent && !m_edit.active) GoTo(m_index);  // recharge l'image réécrite
}

// =============================================================================================
// Textures

ComPtr<ID2D1Bitmap1> Viewer::NewCanvas(uint32_t w, uint32_t h) {
    ComPtr<ID2D1Bitmap1> b;
    if (!m_dc || !w || !h) return b;
    D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(m_dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, p, &b))) b.Reset();
    return b;
}

bool Viewer::DrawOn(ID2D1Bitmap1* target, const std::function<void()>& fn) {
    if (!m_dc || !target) return false;
    m_dc->SetTarget(target);
    m_dc->BeginDraw();
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    fn();
    HRESULT hr = m_dc->EndDraw();
    m_dc->SetTarget(m_target.Get());
    return SUCCEEDED(hr);
}

bool Viewer::DrawOnLayer(const std::function<void()>& fn, bool clipToSel) {
    Layer& l = CurLayer();
    const bool clip = clipToSel && m_edit.selValid;
    const bool ok = DrawOn(l.bmp.Get(), [&] {
        if (clip) m_dc->PushAxisAlignedClip(m_edit.sel, D2D1_ANTIALIAS_MODE_ALIASED);
        fn();
        if (clip) m_dc->PopAxisAlignedClip();
    });
    Touch(l);
    return ok;
}

// =============================================================================================
// Entrée / sortie du mode

bool Viewer::BeginEdit() {
    if (m_edit.active) return true;
    if (!m_cur || !CreateDeviceResources()) {
        ShowToast(T(m_loading ? "edit.loading" : "edit.no_image"));
        return false;
    }
    bool flip;
    int rot;
    FlipRot(flip, rot);
    PixelBuffer px;
    uint32_t w, h, stride;
    if (!OrientPixels(CurPixels(), m_cur->width, m_cur->height, m_cur->stride, flip, rot, px, w, h, stride)) {
        ShowToast(T("edit.err.memory"));
        return false;
    }
    if (std::max(w, h) > m_dc->GetMaximumBitmapSize()) {
        ShowToast(T("edit.err.too_big"));
        return false;
    }
    ComPtr<ID2D1Bitmap1> canvas = NewCanvas(w, h);
    if (!canvas || FAILED(canvas->CopyFromMemory(nullptr, px.Data(), stride))) {
        ShowToast(T("edit.err.vram"));
        return false;
    }
    EditState fresh;  // on garde les réglages d'une session à l'autre
    fresh.tool = m_edit.tool;
    fresh.color = m_edit.color;
    fresh.color2 = m_edit.color2;
    fresh.size = m_edit.size;
    fresh.fontSize = m_edit.fontSize;
    fresh.opacity = m_edit.opacity;
    fresh.hardness = m_edit.hardness;
    fresh.fill = m_edit.fill;
    fresh.gradTransparent = m_edit.gradTransparent;
    fresh.showLayers = m_edit.showLayers;
    if (fresh.tool == Tool::Crop || fresh.tool == Tool::Picker) fresh.tool = Tool::Brush;
    // Tailles d'outil en pixels de l'image : on les adapte à sa définition (8 px est invisible sur 24 Mpx)
    const float longSide = float(std::max(w, h));
    if (m_editRefLong > 0) {
        const float k = longSide / m_editRefLong;
        fresh.size = std::clamp(std::round(fresh.size * k), 1.0f, 200.0f);
        fresh.fontSize = std::clamp(std::round(fresh.fontSize * k), 8.0f, 400.0f);
    } else {
        fresh.size = std::clamp(std::round(longSide / 250), 2.0f, 200.0f);
        fresh.fontSize = std::clamp(std::round(longSide / 25), 12.0f, 400.0f);
    }
    m_editRefLong = longSide;
    m_edit = std::move(fresh);
    m_edit.active = true;
    Layer bg;  // calque « Fond » = l'image ouverte
    bg.bmp = canvas;
    Touch(bg);
    m_edit.layers.push_back(std::move(bg));
    m_edit.w = w;
    m_edit.h = h;
    m_edit.hasAlpha = m_cur->hasAlpha;
    m_userRot = 0;
    m_showHelp = false;
    m_showInfo = false;
    ApplyFit(false);
    Invalidate();
    return true;
}

bool Viewer::EndEdit(bool ask) {
    if (!m_edit.active) return true;
    if (m_edit.textActive) CommitText();
    if (m_edit.fx >= 0) CloseEffect(false);
    if (ask && m_edit.dirty) {
        TASKDIALOG_BUTTON buttons[] = {{100, T("edit.ask.save")}, {101, T("edit.ask.discard")}};
        TASKDIALOGCONFIG tc{sizeof(tc)};
        tc.hwndParent = m_hwnd;
        tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        tc.pszWindowTitle = L"CyImgView";
        tc.pszMainIcon = TD_WARNING_ICON;
        tc.pszMainInstruction = T("edit.ask.title");
        tc.pszContent = T("edit.ask.content");
        tc.cButtons = ARRAYSIZE(buttons);
        tc.pButtons = buttons;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.nDefaultButton = 100;
        int choice = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&tc, &choice, nullptr, nullptr))) choice = IDCANCEL;
        if (choice == IDCANCEL) return false;
        if (choice == 100) {
            SaveAs();
            if (m_edit.dirty) return false;  // enregistrement annulé ou en échec
        }
    }
    EditState keep;  // réglages conservés
    keep.tool = m_edit.tool;
    keep.color = m_edit.color;
    keep.color2 = m_edit.color2;
    keep.size = m_edit.size;
    keep.fontSize = m_edit.fontSize;
    keep.opacity = m_edit.opacity;
    keep.hardness = m_edit.hardness;
    keep.fill = m_edit.fill;
    keep.gradTransparent = m_edit.gradTransparent;
    keep.showLayers = m_edit.showLayers;
    m_edit = std::move(keep);
    m_editHover.reset();
    m_dragging = false;
    if (m_gpuSwitchPending) {  // bascule WARP -> GPU reportée pendant l'édition
        m_gpuSwitchPending = false;
        DiscardDeviceResources();
    }
    ApplyFit(false);
    RequestLoads();
    Invalidate();
    return true;
}

// =============================================================================================
// Géométrie écran <-> image

bool Viewer::ImageRect(float& left, float& top, float& z) const {
    float w, h;
    if (!LogicalSize(w, h)) return false;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    z = float(m_zoom);
    left = float(rc.right / 2.0 + m_panX - w * m_zoom / 2.0);
    top = float(rc.bottom / 2.0 + m_panY - h * m_zoom / 2.0);
    return true;
}

D2D1_POINT_2F Viewer::ScreenToImage(POINT p) const {
    float l, t, z;
    if (!ImageRect(l, t, z) || z <= 0) return {};
    return D2D1::Point2F((p.x - l) / z, (p.y - t) / z);
}

// =============================================================================================
// Outils

void Viewer::SetTool(Tool t) {
    if (m_edit.textActive && t != Tool::Text) CommitText();
    if (t != Tool::Crop) m_edit.cropValid = false;
    if (t == Tool::Picker && m_edit.tool != Tool::Picker) m_edit.prevTool = m_edit.tool;
    m_edit.tool = t;
    m_edit.menu = EditMenu::None;
    Invalidate();
}

bool Viewer::ErasesToColor() const {
    return !m_edit.layers.empty() && m_edit.layers[size_t(m_edit.layer)].num == 0 && !m_edit.hasAlpha;
}

void Viewer::DrawShape(Tool t, D2D1_POINT_2F a, D2D1_POINT_2F b, uint32_t col, float size, bool fill) {
    m_brush->SetColor(Rgb(col));
    ID2D1Brush* br = m_brush.Get();
    switch (t) {
    case Tool::Line:
        m_dc->DrawLine(a, b, br, size, m_roundStroke.Get());
        break;
    case Tool::Arrow: {
        float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1) break;
        float ux = dx / len, uy = dy / len;
        float head = std::min(len, std::max(12.0f, size * 4.0f)), half = head * 0.45f;
        D2D1_POINT_2F base = D2D1::Point2F(b.x - ux * head, b.y - uy * head);
        m_dc->DrawLine(a, D2D1::Point2F(base.x + ux * 1, base.y + uy * 1), br, size, m_roundStroke.Get());
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(m_factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
            sink->BeginFigure(b, D2D1_FIGURE_BEGIN_FILLED);
            D2D1_POINT_2F pts[2] = {D2D1::Point2F(base.x - uy * half, base.y + ux * half),
                                    D2D1::Point2F(base.x + uy * half, base.y - ux * half)};
            sink->AddLines(pts, 2);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            m_dc->FillGeometry(geo.Get(), br);
        }
        break;
    }
    case Tool::Rect: {
        D2D1_RECT_F r = Norm(a, b);
        if (fill) m_dc->FillRectangle(r, br);
        else m_dc->DrawRectangle(r, br, size);
        break;
    }
    case Tool::Ellipse: {
        D2D1_RECT_F r = Norm(a, b);
        D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2),
                                       (r.right - r.left) / 2, (r.bottom - r.top) / 2);
        if (fill) m_dc->FillEllipse(e, br);
        else m_dc->DrawEllipse(e, br, size);
        break;
    }
    default: break;
    }
}

// Début d'un tracé : copie du calque pour l'historique, tampon vidé
bool Viewer::BeginStroke() {
    if (!PushUndo()) return false;
    m_edit.strokeBase = m_edit.undo.back().bmp;  // état d'origine (n'est plus modifié)
    if (!m_edit.strokeBuf || m_edit.strokeBuf->GetPixelSize().width != m_edit.w || m_edit.strokeBuf->GetPixelSize().height != m_edit.h)
        m_edit.strokeBuf = NewCanvas(m_edit.w, m_edit.h);
    if (!m_edit.strokeBuf || !DrawOn(m_edit.strokeBuf.Get(), [&] { m_dc->Clear(D2D1::ColorF(0, 0, 0, 0)); })) {
        ShowToast(T("edit.err.vram"));
        m_edit.strokeBase.Reset();
        return false;
    }
    m_edit.dabCarry = 0;
    return true;
}

void Viewer::StrokeTo(D2D1_POINT_2F a, D2D1_POINT_2F b, bool dot) {
    if (!m_edit.strokeBuf || !m_edit.strokeBase) return;
    const bool erase = m_edit.tool == Tool::Eraser && !ErasesToColor();
    const uint32_t rgb = m_edit.tool == Tool::Eraser || m_edit.secondary ? m_edit.color2 : m_edit.color;
    const float op = m_edit.opacity / 100, r = m_edit.size / 2, hard = m_edit.hardness / 100;
    const D2D1_COLOR_F col = erase ? D2D1::ColorF(0, 0, 0, op) : Rgb(rgb, op);
    // Zone touchée (pixels entiers), limitée au document et à la sélection
    D2D1_RECT_F area = D2D1::RectF(std::floor(std::min(a.x, b.x) - r - 2), std::floor(std::min(a.y, b.y) - r - 2),
                                   std::ceil(std::max(a.x, b.x) + r + 2), std::ceil(std::max(a.y, b.y) + r + 2));
    area = Intersect(area, D2D1::RectF(0, 0, float(m_edit.w), float(m_edit.h)));
    if (m_edit.selValid) area = Intersect(area, m_edit.sel);
    if (area.right <= area.left || area.bottom <= area.top) return;

    // 1) tracé dans le tampon
    DrawOn(m_edit.strokeBuf.Get(), [&] {
        m_dc->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
        m_dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_MAX);
        if (hard >= 0.995f) {
            m_brush->SetColor(col);
            if (dot) m_dc->FillEllipse(D2D1::Ellipse(b, r, r), m_brush.Get());
            else m_dc->DrawLine(a, b, m_brush.Get(), m_edit.size, m_roundStroke.Get());
        } else {  // pinceau doux : empreintes à dégradé radial, rapprochées
            D2D1_GRADIENT_STOP stops[3] = {{0, col}, {hard, col}, {1, D2D1::ColorF(col.r, col.g, col.b, 0)}};
            ComPtr<ID2D1GradientStopCollection> coll;
            ComPtr<ID2D1RadialGradientBrush> rb;
            if (SUCCEEDED(m_dc->CreateGradientStopCollection(stops, 3, &coll)) &&
                SUCCEEDED(m_dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(b, D2D1::Point2F(), r, r), coll.Get(), &rb))) {
                auto dab = [&](D2D1_POINT_2F c) {
                    rb->SetCenter(c);
                    m_dc->FillEllipse(D2D1::Ellipse(c, r, r), rb.Get());
                };
                const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
                const float step = std::max(1.0f, m_edit.size * 0.1f);
                if (dot || len < 1e-3f) {
                    if (dot) dab(b);
                } else {
                    float t = step - m_edit.dabCarry, last = -m_edit.dabCarry;
                    for (; t <= len; t += step) {
                        dab(D2D1::Point2F(a.x + dx * t / len, a.y + dy * t / len));
                        last = t;
                    }
                    m_edit.dabCarry = len - last;
                }
            }
        }
        m_dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        m_dc->PopAxisAlignedClip();
    });
    // 2) calque = état d'origine + tampon, dans la zone touchée seulement
    ID2D1Bitmap1* base = m_edit.strokeBase.Get();
    ID2D1Bitmap1* buf = m_edit.strokeBuf.Get();
    DrawOn(CurLayer().bmp.Get(), [&] {
        m_dc->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
        m_dc->DrawImage(base, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
        m_dc->DrawImage(buf, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                        erase ? D2D1_COMPOSITE_MODE_DESTINATION_OUT : D2D1_COMPOSITE_MODE_SOURCE_OVER);
        m_dc->PopAxisAlignedClip();
    });
    Touch(CurLayer());
    Invalidate();
}

void Viewer::ApplyGradient(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    const float op = m_edit.opacity / 100;
    const uint32_t c1 = m_edit.secondary ? m_edit.color2 : m_edit.color, c2 = m_edit.secondary ? m_edit.color : m_edit.color2;
    D2D1_GRADIENT_STOP stops[2] = {{0, Rgb(c1, op)}, {1, m_edit.gradTransparent ? Rgb(c1, 0) : Rgb(c2, op)}};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> br;
    if (FAILED(m_dc->CreateGradientStopCollection(stops, 2, &coll)) ||
        FAILED(m_dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll.Get(), &br)) || !PushUndo())
        return;
    DrawOnLayer([&] { m_dc->FillRectangle(D2D1::RectF(0, 0, float(m_edit.w), float(m_edit.h)), br.Get()); });
}

// Outil Déplacer : contenu du calque (ou de la sélection) décalé depuis son état au début du glissement
void Viewer::MoveLayerTo(float dx, float dy) {
    if (!m_edit.strokeBase) return;
    dx = std::round(dx);
    dy = std::round(dy);
    ID2D1Bitmap1* base = m_edit.strokeBase.Get();
    const D2D1_RECT_F doc = D2D1::RectF(0, 0, float(m_edit.w), float(m_edit.h));
    const D2D1_COLOR_F hole = ErasesToColor() ? Rgb(m_edit.color2) : D2D1::ColorF(0, 0, 0, 0);
    const bool sel = m_edit.selValid;
    const D2D1_RECT_F src = m_edit.moveSel;
    DrawOn(CurLayer().bmp.Get(), [&] {
        if (sel) {
            m_dc->DrawImage(base, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            m_dc->PushAxisAlignedClip(src, D2D1_ANTIALIAS_MODE_ALIASED);
            m_dc->Clear(hole);
            m_dc->PopAxisAlignedClip();
            m_dc->DrawBitmap(base, D2D1::RectF(src.left + dx, src.top + dy, src.right + dx, src.bottom + dy), 1.0f,
                             D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src, nullptr);
        } else {
            m_dc->Clear(hole);
            m_dc->DrawBitmap(base, D2D1::RectF(dx, dy, doc.right + dx, doc.bottom + dy), 1.0f,
                             D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
        }
    });
    Touch(CurLayer());
    if (sel) m_edit.sel = D2D1::RectF(src.left + dx, src.top + dy, src.right + dx, src.bottom + dy);
    Invalidate();
}

void Viewer::CommitText() {
    if (!m_edit.textActive) return;
    m_edit.textActive = false;
    if (m_edit.text.empty()) {
        Invalidate();
        return;
    }
    ComPtr<IDWriteTextFormat> tf;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, m_edit.fontSize, L"fr-fr", &tf)) ||
        FAILED(m_dwrite->CreateTextLayout(m_edit.text.c_str(), UINT32(m_edit.text.size()), tf.Get(), 100000, 100000, &lay)))
        return;
    if (!PushUndo()) return;
    const float op = m_edit.opacity / 100;
    DrawOnLayer([&] {
        PushOpacity(m_dc.Get(), op);
        m_brush->SetColor(Rgb(m_edit.color));
        m_dc->DrawTextLayout(m_edit.textPos, lay.Get(), m_brush.Get());
        PopOpacity(m_dc.Get(), op);
    });
    m_edit.text.clear();
    Invalidate();
}

void Viewer::FloodFill(D2D1_POINT_2F p, uint32_t rgb) {
    const int x0 = int(std::floor(p.x)), y0 = int(std::floor(p.y));
    int lx = 0, ty = 0, rx = int(m_edit.w), by = int(m_edit.h);  // zone autorisée
    if (m_edit.selValid) {
        lx = int(m_edit.sel.left);
        ty = int(m_edit.sel.top);
        rx = int(m_edit.sel.right);
        by = int(m_edit.sel.bottom);
    }
    if (x0 < lx || y0 < ty || x0 >= rx || y0 >= by) return;
    PixelBuffer px;
    uint32_t stride;
    if (!ReadBitmap(CurLayer().bmp.Get(), px, stride)) return;
    const int W = int(m_edit.w);
    uint32_t* pix = reinterpret_cast<uint32_t*>(px.Data());
    const uint32_t target = pix[size_t(y0) * W + x0];
    const uint32_t op = uint32_t(std::lround(m_edit.opacity * 2.55f));
    const uint32_t fillc = 0xFF000000u | rgb;  // BGRA en mémoire = 0xAARRGGBB
    if (target == fillc) return;
    auto blend = [&](uint32_t c) {  // couleur de remplissage (opacité de l'outil) sur le pixel
        if (op >= 255) return fillc;
        uint32_t out = 0;
        for (int s = 0; s < 32; s += 8) {
            const uint32_t f = s == 24 ? 255 : (fillc >> s) & 0xFF;
            const uint32_t d = (c >> s) & 0xFF;
            out |= ((f * op + d * (255 - op) + 127) / 255) << s;
        }
        return out;
    };
    auto close = [&](uint32_t c) {  // tolérance : les bords antialiasés sont inclus
        int d = 0;
        for (int s = 0; s < 32; s += 8) d = std::max(d, std::abs(int((c >> s) & 0xFF) - int((target >> s) & 0xFF)));
        return d <= 40;
    };
    std::vector<uint8_t> seen(size_t(W) * m_edit.h, 0);
    std::vector<std::pair<int, int>> stack{{x0, y0}};
    while (!stack.empty()) {  // remplissage par segments horizontaux
        auto [x, y] = stack.back();
        stack.pop_back();
        size_t row = size_t(y) * W;
        if (seen[row + x] || !close(pix[row + x])) continue;
        int l = x, r = x;
        while (l > lx && !seen[row + l - 1] && close(pix[row + l - 1])) --l;
        while (r < rx - 1 && !seen[row + r + 1] && close(pix[row + r + 1])) ++r;
        for (int i = l; i <= r; ++i) {
            pix[row + i] = blend(pix[row + i]);
            seen[row + i] = 1;
            if (y > ty && !seen[row - W + i]) stack.push_back({i, y - 1});
            if (y < by - 1 && !seen[row + W + i]) stack.push_back({i, y + 1});
        }
    }
    ComPtr<ID2D1Bitmap1> nb = NewCanvas(m_edit.w, m_edit.h);
    if (!nb || FAILED(nb->CopyFromMemory(nullptr, px.Data(), stride))) {
        ShowToast(T("edit.err.vram"));
        return;
    }
    PushLayerSwap(nb);
    Invalidate();
}

// Pipette : couleur de l'image composée (tous les calques visibles)
void Viewer::PickColor(D2D1_POINT_2F p, bool secondary) {
    const int x = int(std::floor(p.x)), y = int(std::floor(p.y));
    if (x < 0 || y < 0 || x >= int(m_edit.w) || y >= int(m_edit.h)) return;
    ComPtr<ID2D1Bitmap1> one = NewCanvas(1, 1);
    PixelBuffer px;
    uint32_t stride;
    if (!one || !ComposeTo(one.Get(), D2D1::Matrix3x2F::Translation(-float(x), -float(y)), VisibleItems()) ||
        !ReadBitmap(one.Get(), px, stride))
        return;
    uint8_t b = px.Data()[0], g = px.Data()[1], r = px.Data()[2], a = px.Data()[3];
    if (a > 0 && a < 255) {  // dé-prémultiplication
        b = uint8_t(std::min(255, b * 255 / a));
        g = uint8_t(std::min(255, g * 255 / a));
        r = uint8_t(std::min(255, r * 255 / a));
    }
    (secondary ? m_edit.color2 : m_edit.color) = uint32_t(r) << 16 | uint32_t(g) << 8 | b;
    SetTool(m_edit.prevTool);
}

// =============================================================================================
// Transformations du document (tous les calques)

void Viewer::TransformCanvas(EditCmd kind) {
    const float W = float(m_edit.w), H = float(m_edit.h);
    const bool swap = kind == EditCmd::RotL || kind == EditCmd::RotR;
    D2D1::Matrix3x2F m;
    switch (kind) {
    case EditCmd::RotR: m = D2D1::Matrix3x2F(0, 1, -1, 0, H, 0); break;   // quart de tour horaire
    case EditCmd::RotL: m = D2D1::Matrix3x2F(0, -1, 1, 0, 0, W); break;   // quart de tour antihoraire
    case EditCmd::FlipH: m = D2D1::Matrix3x2F(-1, 0, 0, 1, W, 0); break;
    default: m = D2D1::Matrix3x2F(1, 0, 0, -1, 0, H); break;              // FlipV
    }
    std::vector<ComPtr<ID2D1Bitmap1>> out;
    for (auto& l : m_edit.layers) {
        ComPtr<ID2D1Bitmap1> b = swap ? NewCanvas(m_edit.h, m_edit.w) : NewCanvas(m_edit.w, m_edit.h);
        if (!b) {
            ShowToast(T("edit.err.vram"));
            return;
        }
        ID2D1Bitmap1* src = l.bmp.Get();
        DrawOn(b.Get(), [&] {
            m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            m_dc->SetTransform(m);
            m_dc->DrawBitmap(src, D2D1::RectF(0, 0, W, H), 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
        });
        out.push_back(std::move(b));
    }
    SetDocument(std::move(out));
}

void Viewer::ResizeCanvas(uint32_t w, uint32_t h) {
    w = std::clamp<uint32_t>(w, 1, m_dc->GetMaximumBitmapSize());
    h = std::clamp<uint32_t>(h, 1, m_dc->GetMaximumBitmapSize());
    if (w == m_edit.w && h == m_edit.h) return;
    std::vector<ComPtr<ID2D1Bitmap1>> out;
    for (auto& l : m_edit.layers) {
        ComPtr<ID2D1Bitmap1> b = NewCanvas(w, h);
        if (!b) {
            ShowToast(T("edit.err.vram"));
            return;
        }
        ID2D1Bitmap1* src = l.bmp.Get();
        DrawOn(b.Get(), [&] {
            m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            m_dc->DrawBitmap(src, D2D1::RectF(0, 0, float(w), float(h)), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                             nullptr, nullptr);
        });
        out.push_back(std::move(b));
    }
    SetDocument(std::move(out));
    wchar_t b[64];
    swprintf_s(b, L"%u × %u", w, h);
    ShowToast(b);
}

void Viewer::ApplyCrop() {
    if (!m_edit.cropValid) return;
    D2D1_RECT_U r = D2D1::RectU(UINT32(std::clamp(m_edit.crop.left, 0.0f, float(m_edit.w))),
                                UINT32(std::clamp(m_edit.crop.top, 0.0f, float(m_edit.h))),
                                UINT32(std::clamp(m_edit.crop.right, 0.0f, float(m_edit.w))),
                                UINT32(std::clamp(m_edit.crop.bottom, 0.0f, float(m_edit.h))));
    if (r.right <= r.left + 1 || r.bottom <= r.top + 1) return;
    std::vector<ComPtr<ID2D1Bitmap1>> out;
    for (auto& l : m_edit.layers) {
        ComPtr<ID2D1Bitmap1> b = NewCanvas(r.right - r.left, r.bottom - r.top);
        D2D1_POINT_2U origin = D2D1::Point2U(0, 0);
        if (!b || FAILED(b->CopyFromBitmap(&origin, l.bmp.Get(), &r))) {
            ShowToast(T("edit.err.vram"));
            return;
        }
        out.push_back(std::move(b));
    }
    SetDocument(std::move(out));
}

// =============================================================================================
// Commandes de l'interface

void Viewer::EditCommand(EditCmd cmd, int arg) {
    // Un effet en cours de réglage est abandonné par toute commande qui touche au document
    switch (cmd) {
    case EditCmd::Panel: case EditCmd::Slider: case EditCmd::Color: case EditCmd::Color2: case EditCmd::SwapColors:
    case EditCmd::CustomColor: case EditCmd::FxMenu: case EditCmd::FxPick: case EditCmd::FxApply: case EditCmd::FxCancel:
    case EditCmd::FxPreview: case EditCmd::FxReset: case EditCmd::ResizeMenu: case EditCmd::Undo:
        break;
    default:
        if (m_edit.fx >= 0) CloseEffect(false);
        break;
    }
    if (cmd != EditCmd::ResizeMenu && cmd != EditCmd::FxMenu && cmd != EditCmd::BlendMenu) m_edit.menu = EditMenu::None;
    auto toggleMenu = [&](EditMenu m) { m_edit.menu = m_edit.menu == m ? EditMenu::None : m; };
    switch (cmd) {
    case EditCmd::Tool: SetTool(Tool(arg)); break;
    case EditCmd::Color: m_edit.color = kPalette[arg]; break;
    case EditCmd::Color2: m_edit.color2 = kPalette[arg]; break;
    case EditCmd::SwapColors: std::swap(m_edit.color, m_edit.color2); break;
    case EditCmd::CustomColor: {
        static COLORREF custom[16] = {};
        CHOOSECOLORW cc{sizeof(cc)};
        cc.hwndOwner = m_hwnd;
        cc.rgbResult = RGB((m_edit.color >> 16) & 0xFF, (m_edit.color >> 8) & 0xFF, m_edit.color & 0xFF);
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) m_edit.color = uint32_t(GetRValue(cc.rgbResult)) << 16 | uint32_t(GetGValue(cc.rgbResult)) << 8 | GetBValue(cc.rgbResult);
        break;
    }
    case EditCmd::Undo: Undo(); break;
    case EditCmd::Redo: Redo(); break;
    case EditCmd::FillToggle: m_edit.fill = !m_edit.fill; break;
    case EditCmd::GradToggle: m_edit.gradTransparent = !m_edit.gradTransparent; break;
    case EditCmd::RotL: case EditCmd::RotR: case EditCmd::FlipH: case EditCmd::FlipV:
        if (m_edit.textActive) CommitText();
        TransformCanvas(cmd);
        break;
    case EditCmd::ResizeMenu: toggleMenu(EditMenu::Resize); break;
    case EditCmd::ResizeOpt: {
        if (m_edit.textActive) CommitText();
        const ResizeOption& o = kResize[arg];
        double s = o.percent ? o.value / 100.0 : double(o.value) / std::max(m_edit.w, m_edit.h);
        ResizeCanvas(uint32_t(std::lround(m_edit.w * s)), uint32_t(std::lround(m_edit.h * s)));
        break;
    }
    case EditCmd::SaveAs: SaveAs(); break;
    case EditCmd::Done: EndEdit(true); break;
    case EditCmd::CropApply: ApplyCrop(); break;
    case EditCmd::CropCancel: m_edit.cropValid = false; break;
    case EditCmd::LayerSelect: case EditCmd::LayerEye: case EditCmd::LayerNew: case EditCmd::LayerDup: case EditCmd::LayerDel:
    case EditCmd::LayerUp: case EditCmd::LayerDown: case EditCmd::LayerMerge: case EditCmd::LayerFlatten: case EditCmd::BlendOpt:
        LayerCommand(cmd, arg);
        break;
    case EditCmd::BlendMenu: toggleMenu(EditMenu::Blend); break;
    case EditCmd::FxMenu: toggleMenu(EditMenu::Effects); break;
    case EditCmd::FxPick: OpenEffect(arg); break;
    case EditCmd::FxApply: CloseEffect(true); break;
    case EditCmd::FxCancel: CloseEffect(false); break;
    case EditCmd::FxPreview: m_edit.fxPreview = !m_edit.fxPreview; break;
    case EditCmd::FxReset: OpenEffect(m_edit.fx); break;
    default: break;
    }
    Invalidate();
}

const Viewer::EditButton* Viewer::HitEdit(POINT p) const {
    for (auto it = m_editButtons.rbegin(); it != m_editButtons.rend(); ++it)  // menus dessinés en dernier
        if (Contains(it->rc, p)) return &*it;
    return nullptr;
}

// Bouton de l'interface : fond actif / survolé, zone de clic enregistrée. Retourne true si survolé.
bool Viewer::EditBtn(EditCmd cmd, int arg, const D2D1_RECT_F& r, const wchar_t* tip, bool active, bool enabled) {
    const float s = m_scale;
    const bool isHov = enabled && cmd != EditCmd::Panel && m_editHover && m_editHover->cmd == cmd && m_editHover->arg == arg;
    if (cmd != EditCmd::Panel) {
        if (active) FillRound(r, 6 * s, kAccent, 0.25f);
        if (isHov) FillRound(r, 6 * s, kWhite, 0.12f);
    }
    if (enabled) m_editButtons.push_back({cmd, arg, r, tip});
    return isHov;
}

// =============================================================================================
// Souris et clavier

bool Viewer::EditMouseDown(POINT p, bool right, bool dbl) {
    if (m_edit.renaming >= 0) EditChar(L'\r');  // clic ailleurs : renommage validé
    if (const EditButton* b = HitEdit(p)) {
        if (b->cmd == EditCmd::Slider) {
            if (b->arg == kSlLayerOpacity) PushDocUndo();
            m_edit.slider = b->arg;
            m_edit.sliderRc = D2D1::RectF(b->rc.left + 6 * m_scale, b->rc.top, b->rc.right - 6 * m_scale, b->rc.bottom);
            m_edit.menu = EditMenu::None;
            SetCapture(m_hwnd);
            EditMouseMove(p);
        } else if (b->cmd == EditCmd::Color && right) {
            EditCommand(EditCmd::Color2, b->arg);
        } else if (b->cmd == EditCmd::LayerSelect && dbl) {
            m_edit.renaming = b->arg;
            m_edit.renameText = LayerName(m_edit.layers[size_t(b->arg)]);
            Invalidate();
        } else if (b->cmd == EditCmd::Panel) {
            if (m_edit.menu != EditMenu::None) m_edit.menu = EditMenu::None;
            Invalidate();
        } else {
            EditCommand(b->cmd, b->arg);
        }
        return true;
    }
    if (m_edit.menu != EditMenu::None) {
        m_edit.menu = EditMenu::None;
        Invalidate();
        return true;
    }
    if (m_edit.tool == Tool::Hand || m_edit.spacePan) return false;  // déplacement de la vue
    if (m_edit.fx >= 0) return true;  // réglage d'un effet : le document est figé

    D2D1_POINT_2F ip = ScreenToImage(p);
    m_edit.secondary = right;
    m_edit.shift = GetKeyState(VK_SHIFT) < 0;
    switch (m_edit.tool) {
    case Tool::Brush:
    case Tool::Eraser:
        if (!BeginStroke()) return true;
        m_edit.drawing = true;
        m_edit.start = m_edit.cur = ip;
        StrokeTo(ip, ip, true);
        break;
    case Tool::Line: case Tool::Arrow: case Tool::Rect: case Tool::Ellipse: case Tool::Gradient: case Tool::Select:
        m_edit.drawing = true;
        m_edit.start = m_edit.cur = ip;
        break;
    case Tool::Move:
        if (right || !PushUndo()) return true;
        m_edit.strokeBase = m_edit.undo.back().bmp;
        m_edit.moveSel = m_edit.sel;
        m_edit.drawing = true;
        m_edit.start = m_edit.cur = ip;
        break;
    case Tool::Crop:
        if (dbl && m_edit.cropValid) {
            ApplyCrop();
            return true;
        }
        m_edit.drawing = true;
        m_edit.cropValid = false;
        m_edit.start = m_edit.cur = ip;
        break;
    case Tool::Text:
        CommitText();
        m_edit.textActive = true;
        m_edit.textPos = ip;
        m_edit.text.clear();
        break;
    case Tool::Fill: FloodFill(ip, right ? m_edit.color2 : m_edit.color); break;
    case Tool::Picker: PickColor(ip, right); break;
    default: break;
    }
    if (m_edit.drawing) SetCapture(m_hwnd);
    Invalidate();
    return true;
}

void Viewer::EditMouseMove(POINT p) {
    if (m_edit.slider >= 0) {
        const D2D1_RECT_F& r = m_edit.sliderRc;
        SetSliderT(m_edit.slider, (p.x - r.left) / std::max(1.0f, r.right - r.left));
        Invalidate();
        return;
    }
    if (!m_edit.drawing) return;
    D2D1_POINT_2F ip = ScreenToImage(p);
    m_edit.shift = GetKeyState(VK_SHIFT) < 0;
    if (m_edit.tool == Tool::Brush || m_edit.tool == Tool::Eraser) {
        StrokeTo(m_edit.cur, ip, false);
        m_edit.cur = ip;
        return;
    }
    if (m_edit.tool == Tool::Move) {
        MoveLayerTo(ip.x - m_edit.start.x, ip.y - m_edit.start.y);
        m_edit.cur = ip;
        return;
    }
    if (m_edit.shift) {  // contraintes : carré / cercle / angles de 45°
        float dx = ip.x - m_edit.start.x, dy = ip.y - m_edit.start.y;
        if (m_edit.tool == Tool::Rect || m_edit.tool == Tool::Ellipse || m_edit.tool == Tool::Crop || m_edit.tool == Tool::Select) {
            float s = std::max(std::fabs(dx), std::fabs(dy));
            ip = D2D1::Point2F(m_edit.start.x + std::copysign(s, dx), m_edit.start.y + std::copysign(s, dy));
        } else {
            float ang = std::round(std::atan2(dy, dx) / (3.14159265f / 4)) * (3.14159265f / 4);
            float len = std::sqrt(dx * dx + dy * dy);
            ip = D2D1::Point2F(m_edit.start.x + std::cos(ang) * len, m_edit.start.y + std::sin(ang) * len);
        }
    }
    m_edit.cur = ip;
    Invalidate();
}

void Viewer::EditMouseUp(POINT) {
    if (m_edit.slider >= 0) {
        m_edit.slider = -1;
        ReleaseCapture();
        Invalidate();
        return;
    }
    if (!m_edit.drawing) return;
    m_edit.drawing = false;
    ReleaseCapture();
    const float dx = m_edit.cur.x - m_edit.start.x, dy = m_edit.cur.y - m_edit.start.y;
    switch (m_edit.tool) {
    case Tool::Brush: case Tool::Eraser: case Tool::Move:
        m_edit.strokeBase.Reset();
        break;
    case Tool::Line: case Tool::Arrow: case Tool::Rect: case Tool::Ellipse: {
        if (dx * dx + dy * dy < 1 || !PushUndo()) break;
        uint32_t col = m_edit.secondary ? m_edit.color2 : m_edit.color;
        Tool t = m_edit.tool;
        D2D1_POINT_2F a = m_edit.start, b = m_edit.cur;
        const float op = m_edit.opacity / 100;
        DrawOnLayer([&] {
            PushOpacity(m_dc.Get(), op);
            DrawShape(t, a, b, col, m_edit.size, m_edit.fill);
            PopOpacity(m_dc.Get(), op);
        });
        break;
    }
    case Tool::Gradient:
        if (dx * dx + dy * dy >= 1) ApplyGradient(m_edit.start, m_edit.cur);
        break;
    case Tool::Select:
    case Tool::Crop: {
        D2D1_RECT_F r = Norm(m_edit.start, m_edit.cur);
        r.left = std::round(std::max(0.0f, r.left));
        r.top = std::round(std::max(0.0f, r.top));
        r.right = std::round(std::min(float(m_edit.w), r.right));
        r.bottom = std::round(std::min(float(m_edit.h), r.bottom));
        const bool valid = r.right - r.left >= 1 && r.bottom - r.top >= 1;
        if (m_edit.tool == Tool::Select) {
            m_edit.sel = r;
            m_edit.selValid = valid && (std::fabs(dx) >= 1 || std::fabs(dy) >= 1);  // simple clic : désélectionne
        } else {
            m_edit.crop = r;
            m_edit.cropValid = r.right - r.left >= 2 && r.bottom - r.top >= 2;
        }
        break;
    }
    default: break;
    }
    Invalidate();
}

bool Viewer::EditChar(wchar_t c) {
    if (m_edit.renaming >= 0) {  // renommage d'un calque
        if (c == L'\b') {
            if (!m_edit.renameText.empty()) m_edit.renameText.pop_back();
        } else if (c == L'\r') {
            const int i = m_edit.renaming;
            m_edit.renaming = -1;
            if (i < int(m_edit.layers.size()) && m_edit.renameText != LayerName(m_edit.layers[size_t(i)])) {
                PushDocUndo();
                m_edit.layers[size_t(i)].name = m_edit.renameText;  // vide : nom automatique
            }
        } else if (c >= 32 && m_edit.renameText.size() < 100) {
            m_edit.renameText += c;
        }
        Invalidate();
        return true;
    }
    if (!m_edit.textActive) return false;
    if (c == L'\b') {
        if (!m_edit.text.empty()) m_edit.text.pop_back();
    } else if (c == L'\r') {
        if (GetKeyState(VK_SHIFT) < 0) m_edit.text += L'\n';  // Maj+Entrée : nouvelle ligne
        else CommitText();
    } else if (c >= 32 || c == L'\t') {
        m_edit.text += c;
    }
    Invalidate();
    return true;
}

bool Viewer::EditKey(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    if (m_edit.renaming >= 0) {  // saisie du nom : le reste arrive par WM_CHAR
        if (vk == VK_ESCAPE) {
            m_edit.renaming = -1;
            Invalidate();
        }
        return true;
    }
    if (m_edit.textActive) {  // saisie de texte : seules quelques touches sont des commandes
        if (vk == VK_ESCAPE) {
            m_edit.textActive = false;
            m_edit.text.clear();
            Invalidate();
        } else if (ctrl && vk == 'S') {
            SaveAs();
        }
        return true;  // le reste arrive par WM_CHAR
    }
    if (m_edit.fx >= 0) {  // réglage d'un effet : Entrée applique, Échap annule
        if (vk == VK_RETURN) CloseEffect(true);
        else if (vk == VK_ESCAPE || (ctrl && vk == 'Z')) CloseEffect(false);
        else if (vk == VK_SPACE) m_edit.spacePan = true;
        return true;
    }
    if (ctrl) {
        switch (vk) {
        case 'Z': shift ? Redo() : Undo(); return true;
        case 'Y': Redo(); return true;
        case 'S': SaveAs(); return true;
        case 'C': CopyToClipboard(); return true;
        case 'X':
            if (m_edit.selValid) {
                CopyToClipboard();
                ClearSelection();
            }
            return true;
        case 'V': PasteAsLayer(); return true;
        case 'A': SelectAll(); return true;
        case 'D': Deselect(); return true;
        case 'J': LayerCommand(EditCmd::LayerDup, 0); return true;
        case 'E': LayerCommand(shift ? EditCmd::LayerFlatten : EditCmd::LayerMerge, 0); return true;
        case 'N':
            if (shift) LayerCommand(EditCmd::LayerNew, 0);
            return true;
        case VK_OEM_6: LayerCommand(EditCmd::LayerUp, 0); return true;
        case VK_OEM_4: LayerCommand(EditCmd::LayerDown, 0); return true;
        default: return false;
        }
    }
    switch (vk) {
    case VK_SPACE:
        m_edit.spacePan = true;
        return true;
    case VK_ESCAPE:
        if (m_edit.menu != EditMenu::None) {
            m_edit.menu = EditMenu::None;
        } else if (m_edit.drawing && m_edit.tool == Tool::Move) {  // déplacement annulé
            MoveLayerTo(0, 0);
            m_edit.sel = m_edit.moveSel;
            m_edit.drawing = false;
            m_edit.strokeBase.Reset();
        } else if (m_edit.cropValid || (m_edit.drawing && m_edit.tool != Tool::Brush && m_edit.tool != Tool::Eraser)) {
            m_edit.cropValid = false;
            m_edit.drawing = false;
        } else if (m_edit.selValid) {
            Deselect();
        } else {
            EndEdit(true);
        }
        Invalidate();
        return true;
    case VK_RETURN:
        if (m_edit.cropValid) ApplyCrop();
        return true;
    case VK_DELETE: case VK_BACK: ClearSelection(); return true;
    case 'M': SetTool(Tool::Hand); return true;
    case 'V': SetTool(Tool::Move); return true;
    case 'S': SetTool(Tool::Select); return true;
    case 'B': SetTool(Tool::Brush); return true;
    case 'G': SetTool(Tool::Eraser); return true;
    case 'L': SetTool(Tool::Line); return true;
    case 'A': SetTool(Tool::Arrow); return true;
    case 'R': SetTool(Tool::Rect); return true;
    case 'O': SetTool(Tool::Ellipse); return true;
    case 'T': SetTool(Tool::Text); return true;
    case 'F': SetTool(Tool::Fill); return true;
    case 'D': SetTool(Tool::Gradient); return true;
    case 'K': SetTool(Tool::Picker); return true;
    case 'C': SetTool(Tool::Crop); return true;
    case 'X': EditCommand(EditCmd::SwapColors, 0); return true;    case 'E': EndEdit(true); return true;
    case VK_F7:
        m_edit.showLayers = !m_edit.showLayers;
        if (m_fit) ApplyFit(true);
        Invalidate();
        return true;
    case VK_OEM_4: case VK_OEM_6: {  // [ et ] : taille
        float& v = m_edit.tool == Tool::Text ? m_edit.fontSize : m_edit.size;
        v = vk == VK_OEM_6 ? std::min(m_edit.tool == Tool::Text ? 400.0f : 200.0f, std::ceil(v * 1.2f))
                           : std::max(1.0f, std::floor(v / 1.2f));
        Invalidate();
        return true;
    }
    // Navigation, zoom, plein écran : gérés par le mode visionneuse (avec confirmation si besoin)
    default: return false;
    }
}

// Molette sur la liste des calques : défilement (ailleurs : zoom)
bool Viewer::EditWheel(POINT p, double notches) {
    if (!m_edit.showLayers || m_edit.fx >= 0) return false;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (!Contains(LayersPanelRect(float(rc.right), float(rc.bottom)), p)) return false;
    m_edit.layerScroll = std::max(0, m_edit.layerScroll - int(std::lround(notches)));
    Invalidate();
    return true;
}

// =============================================================================================
// Dessin de l'interface d'édition

void Viewer::DrawToolIcon(Tool t, const D2D1_RECT_F& r, uint32_t col, float a) {
    const float s = m_scale;
    const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, k = 8 * s;
    SetColor(col, a);
    ID2D1Brush* br = m_brush.Get();
    const float sw = 1.6f * s;
    switch (t) {
    case Tool::Hand:  // flèches dans 4 directions
        m_rt->DrawLine(D2D1::Point2F(cx - k, cy), D2D1::Point2F(cx + k, cy), br, sw);
        m_rt->DrawLine(D2D1::Point2F(cx, cy - k), D2D1::Point2F(cx, cy + k), br, sw);
        for (int d = 0; d < 4; ++d) {
            float dx = d == 0 ? 1.f : d == 1 ? -1.f : 0.f, dy = d == 2 ? 1.f : d == 3 ? -1.f : 0.f;
            D2D1_POINT_2F tip = D2D1::Point2F(cx + dx * k, cy + dy * k);
            m_rt->DrawLine(tip, D2D1::Point2F(tip.x - dx * 3 * s + dy * 3 * s, tip.y - dy * 3 * s + dx * 3 * s), br, sw);
            m_rt->DrawLine(tip, D2D1::Point2F(tip.x - dx * 3 * s - dy * 3 * s, tip.y - dy * 3 * s - dx * 3 * s), br, sw);
        }
        break;
    case Tool::Move: {  // pointeur + croix
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(m_factory->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
            const float px = cx - 5 * s, py = cy - 9 * s;
            const D2D1_POINT_2F pts[] = {{px, py + 13 * s}, {px + 3.2f * s, py + 10 * s}, {px + 5.5f * s, py + 15 * s},
                                         {px + 7.5f * s, py + 14 * s}, {px + 5.3f * s, py + 9.2f * s}, {px + 9.5f * s, py + 9.2f * s}};
            sink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLines(pts, ARRAYSIZE(pts));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            m_rt->FillGeometry(geo.Get(), br);
        }
        const float qx = cx + 5 * s, qy = cy + 5 * s;
        m_rt->DrawLine(D2D1::Point2F(qx - 4 * s, qy), D2D1::Point2F(qx + 4 * s, qy), br, 1.3f * s);
        m_rt->DrawLine(D2D1::Point2F(qx, qy - 4 * s), D2D1::Point2F(qx, qy + 4 * s), br, 1.3f * s);
        break;
    }
    case Tool::Select: {  // rectangle en pointillés
        ComPtr<ID2D1StrokeStyle> dash;
        m_factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                 D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH),
                                     nullptr, 0, &dash);
        m_rt->DrawRectangle(D2D1::RectF(cx - k, cy - k * 0.75f, cx + k, cy + k * 0.75f), br, sw, dash.Get());
        break;
    }
    case Tool::Gradient: {
        D2D1_GRADIENT_STOP st[2] = {{0, Rgb(col, a)}, {1, Rgb(col, 0)}};
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> gb;
        const D2D1_RECT_F gr = D2D1::RectF(cx - k, cy - k * 0.7f, cx + k, cy + k * 0.7f);
        if (SUCCEEDED(m_rt->CreateGradientStopCollection(st, 2, &coll)) &&
            SUCCEEDED(m_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(gr.left, cy), D2D1::Point2F(gr.right, cy)), coll.Get(), &gb)))
            m_rt->FillRectangle(gr, gb.Get());
        m_rt->DrawRectangle(gr, br, 1.2f * s);
        break;
    }
    case Tool::Brush: Text(L"", m_tfIconMid.Get(), r, col, a); break;
    case Tool::Eraser: Text(L"", m_tfIconMid.Get(), r, col, a); break;
    case Tool::Line: m_rt->DrawLine(D2D1::Point2F(cx - k, cy + k), D2D1::Point2F(cx + k, cy - k), br, 2 * s, m_roundStroke.Get()); break;
    case Tool::Arrow:
        m_rt->DrawLine(D2D1::Point2F(cx - k, cy + k), D2D1::Point2F(cx + k, cy - k), br, 2 * s, m_roundStroke.Get());
        m_rt->DrawLine(D2D1::Point2F(cx + k, cy - k), D2D1::Point2F(cx + k - 6 * s, cy - k), br, 2 * s, m_roundStroke.Get());
        m_rt->DrawLine(D2D1::Point2F(cx + k, cy - k), D2D1::Point2F(cx + k, cy - k + 6 * s), br, 2 * s, m_roundStroke.Get());
        break;
    case Tool::Rect: m_rt->DrawRectangle(D2D1::RectF(cx - k, cy - k * 0.7f, cx + k, cy + k * 0.7f), br, sw); break;
    case Tool::Ellipse: m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), k, k * 0.7f), br, sw); break;
    case Tool::Text: Text(L"A", m_tfToast.Get(), r, col, a); break;
    case Tool::Fill: {  // seau incliné + goutte
        m_rt->SetTransform(D2D1::Matrix3x2F::Rotation(-30, D2D1::Point2F(cx - 2 * s, cy)));
        m_rt->DrawRectangle(D2D1::RectF(cx - 8 * s, cy - 5 * s, cx + 4 * s, cy + 7 * s), br, sw);
        m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 8 * s, cy + 6 * s), 2.5f * s, 3.2f * s), br);
        break;
    }
    case Tool::Picker:  // pipette
        m_rt->DrawLine(D2D1::Point2F(cx - k, cy + k), D2D1::Point2F(cx + 3 * s, cy - 3 * s), br, 2.4f * s, m_roundStroke.Get());
        m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + 5 * s, cy - 5 * s), 4 * s, 4 * s), br);
        break;
    case Tool::Crop: Text(L"", m_tfIconMid.Get(), r, col, a); break;
    }
}

// Icône (police Segoe MDL2) + libellé (Segoe UI), centrés ensemble dans r
void Viewer::IconLabel(const D2D1_RECT_F& r, const wchar_t* glyph, const std::wstring& label, uint32_t col, float a) {
    const float s = m_scale, iconW = 18 * s, gapW = 8 * s;
    float textW = 0;
    ComPtr<IDWriteTextLayout> lay;
    if (SUCCEEDED(m_dwrite->CreateTextLayout(label.c_str(), UINT32(label.size()), m_tfLabel.Get(), 1000, 100, &lay))) {
        DWRITE_TEXT_METRICS tm;
        lay->GetMetrics(&tm);
        textW = tm.width;
    }
    float x = (r.left + r.right) / 2 - (iconW + gapW + textW) / 2;
    Text(glyph, m_tfIcon.Get(), D2D1::RectF(x, r.top, x + iconW, r.bottom), col, a);
    Text(label, m_tfLabel.Get(), D2D1::RectF(x + iconW + gapW - 2 * s, r.top, x + iconW + gapW + textW + 4 * s, r.bottom), col, a);
}

// Curseur horizontal (zone de clic un peu plus grande que la piste)
void Viewer::DrawSlider(int id, const D2D1_RECT_F& r, const wchar_t* tip) {
    const float s = m_scale;
    m_editButtons.push_back({EditCmd::Slider, id, D2D1::RectF(r.left - 6 * s, r.top - 4 * s, r.right + 6 * s, r.bottom + 4 * s), tip});
    const float sy = (r.top + r.bottom) / 2;
    const float kx = r.left + std::clamp(SliderT(id), 0.0f, 1.0f) * (r.right - r.left);
    SetColor(kWhite, 0.2f);
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, sy - 2 * s, r.right, sy + 2 * s), 2 * s, 2 * s), m_brush.Get());
    SetColor(kAccent, 1);
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, sy - 2 * s, kx, sy + 2 * s), 2 * s, 2 * s), m_brush.Get());
    const bool hot = m_edit.slider == id || (m_editHover && m_editHover->cmd == EditCmd::Slider && m_editHover->arg == id);
    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, sy), (hot ? 8.0f : 7.0f) * s, (hot ? 8.0f : 7.0f) * s), m_brush.Get());
}

void Viewer::DrawEditOverlay(float left, float top, float z) {
    const float s = m_scale;
    const auto toImage = D2D1::Matrix3x2F::Scale(z, z) * D2D1::Matrix3x2F::Translation(left, top);
    auto screen = [&](const D2D1_RECT_F& ir) {
        return D2D1::RectF(left + ir.left * z, top + ir.top * z, left + ir.right * z, top + ir.bottom * z);
    };
    auto sizeLabel = [&](const D2D1_RECT_F& ir, const D2D1_RECT_F& r) {
        wchar_t b[48];
        swprintf_s(b, L"%.0f × %.0f", ir.right - ir.left, ir.bottom - ir.top);
        D2D1_RECT_F lab = D2D1::RectF(r.left, r.top - 24 * s, r.left + 110 * s, r.top - 4 * s);
        FillRound(lab, 4 * s, 0x000000, 0.6f);
        Text(b, m_tfCenter.Get(), lab, kWhite, 1);
    };

    // Aperçu de la forme en cours (avec l'opacité de l'outil, limitée à la sélection)
    if (m_edit.drawing && (m_edit.tool == Tool::Line || m_edit.tool == Tool::Arrow || m_edit.tool == Tool::Rect ||
                           m_edit.tool == Tool::Ellipse)) {
        m_dc->SetTransform(toImage);
        if (m_edit.selValid) m_dc->PushAxisAlignedClip(m_edit.sel, D2D1_ANTIALIAS_MODE_ALIASED);
        const float op = m_edit.opacity / 100;
        PushOpacity(m_dc.Get(), op);
        DrawShape(m_edit.tool, m_edit.start, m_edit.cur, m_edit.secondary ? m_edit.color2 : m_edit.color, m_edit.size, m_edit.fill);
        PopOpacity(m_dc.Get(), op);
        if (m_edit.selValid) m_dc->PopAxisAlignedClip();
        m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // Dégradé en cours : segment entre les deux points
    if (m_edit.drawing && m_edit.tool == Tool::Gradient) {
        const D2D1_POINT_2F a = D2D1::Point2F(left + m_edit.start.x * z, top + m_edit.start.y * z);
        const D2D1_POINT_2F b = D2D1::Point2F(left + m_edit.cur.x * z, top + m_edit.cur.y * z);
        SetColor(0x000000, 0.7f);
        m_rt->DrawLine(a, b, m_brush.Get(), 3 * s, m_roundStroke.Get());
        SetColor(kWhite, 0.95f);
        m_rt->DrawLine(a, b, m_brush.Get(), 1.2f * s, m_roundStroke.Get());
        for (const auto& pt : {a, b}) {
            SetColor(pt.x == a.x && pt.y == a.y ? m_edit.color : (m_edit.gradTransparent ? kGrey : m_edit.color2), 1);
            m_rt->FillEllipse(D2D1::Ellipse(pt, 5 * s, 5 * s), m_brush.Get());
            SetColor(kWhite, 0.95f);
            m_rt->DrawEllipse(D2D1::Ellipse(pt, 5 * s, 5 * s), m_brush.Get(), 1.2f);
        }
    }

    // Sélection : pointillés animés (minuterie de l'interface, 5 images/s)
    const bool selecting = m_edit.drawing && m_edit.tool == Tool::Select;
    if (m_edit.selValid || selecting) {
        const D2D1_RECT_F ir = selecting ? Norm(m_edit.start, m_edit.cur) : m_edit.sel;
        D2D1_RECT_F r = screen(ir);
        r = D2D1::RectF(std::round(r.left) + 0.5f, std::round(r.top) + 0.5f, std::round(r.right) - 0.5f, std::round(r.bottom) - 0.5f);
        SetColor(0x000000, 0.9f);
        m_rt->DrawRectangle(r, m_brush.Get(), 1);
        const float dashes[] = {4, 4};
        ComPtr<ID2D1StrokeStyle> ants;
        m_factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                 D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM,
                                                                 float(int(NowSeconds() * 5) % 8)),
                                     dashes, 2, &ants);
        SetColor(kWhite, 1);
        m_rt->DrawRectangle(r, m_brush.Get(), 1, ants.Get());
        if (selecting) sizeLabel(ir, r);
    }

    // Recadrage : extérieur assombri, grille des tiers, boutons
    if (m_edit.tool == Tool::Crop && (m_edit.drawing || m_edit.cropValid)) {
        D2D1_RECT_F ir = m_edit.drawing ? Norm(m_edit.start, m_edit.cur) : m_edit.crop;
        D2D1_RECT_F r = screen(ir);
        D2D1_RECT_F img = D2D1::RectF(left, top, left + m_edit.w * z, top + m_edit.h * z);
        SetColor(0x000000, 0.55f);
        m_rt->FillRectangle(D2D1::RectF(img.left, img.top, img.right, r.top), m_brush.Get());
        m_rt->FillRectangle(D2D1::RectF(img.left, r.bottom, img.right, img.bottom), m_brush.Get());
        m_rt->FillRectangle(D2D1::RectF(img.left, r.top, r.left, r.bottom), m_brush.Get());
        m_rt->FillRectangle(D2D1::RectF(r.right, r.top, img.right, r.bottom), m_brush.Get());
        SetColor(kWhite, 0.35f);
        for (int i = 1; i < 3; ++i) {
            float x = r.left + (r.right - r.left) * i / 3, y = r.top + (r.bottom - r.top) * i / 3;
            m_rt->DrawLine(D2D1::Point2F(x, r.top), D2D1::Point2F(x, r.bottom), m_brush.Get(), 1);
            m_rt->DrawLine(D2D1::Point2F(r.left, y), D2D1::Point2F(r.right, y), m_brush.Get(), 1);
        }
        SetColor(kWhite, 0.95f);
        m_rt->DrawRectangle(r, m_brush.Get(), 1.5f * s);
        sizeLabel(ir, r);
        if (m_edit.cropValid && !m_edit.drawing) {
            D2D1_RECT_F ok = D2D1::RectF(r.right - 196 * s, r.bottom + 8 * s, r.right - 64 * s, r.bottom + 40 * s);
            D2D1_RECT_F no = D2D1::RectF(r.right - 56 * s, r.bottom + 8 * s, r.right, r.bottom + 40 * s);
            FillRound(ok, 6 * s, kAccent, 0.95f);
            IconLabel(ok, L"", T("edit.crop.apply"), 0x0B1A2A, 1);
            FillRound(no, 6 * s, kPanel, 0.95f);
            Text(L"", m_tfIcon.Get(), no, kWhite, 1);
            m_editButtons.push_back({EditCmd::CropApply, 0, ok, T("edit.tip.crop_apply")});
            m_editButtons.push_back({EditCmd::CropCancel, 0, no, T("edit.tip.crop_cancel")});
        }
    }

    // Texte en cours de saisie, avec curseur clignotant
    if (m_edit.textActive) {
        ComPtr<IDWriteTextFormat> tf;
        ComPtr<IDWriteTextLayout> lay;
        m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, m_edit.fontSize, L"fr-fr", &tf);
        if (tf && SUCCEEDED(m_dwrite->CreateTextLayout(m_edit.text.c_str(), UINT32(m_edit.text.size()), tf.Get(), 100000,
                                                       100000, &lay))) {
            m_dc->SetTransform(toImage);
            const float op = m_edit.opacity / 100;
            PushOpacity(m_dc.Get(), op);
            m_brush->SetColor(Rgb(m_edit.color));
            m_dc->DrawTextLayout(m_edit.textPos, lay.Get(), m_brush.Get());
            PopOpacity(m_dc.Get(), op);
            DWRITE_HIT_TEST_METRICS hm{};
            FLOAT cxp = 0, cyp = 0;
            lay->HitTestTextPosition(UINT32(m_edit.text.size()), FALSE, &cxp, &cyp, &hm);
            if (std::fmod(NowSeconds(), 1.0) < 0.6) {
                m_brush->SetColor(Rgb(m_edit.color));
                float x = m_edit.textPos.x + cxp, y = m_edit.textPos.y + cyp;
                m_dc->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(x, y + hm.height), m_brush.Get(), std::max(1.0f, 1.5f / z));
            }
            m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }

    // Taille du pinceau sous le curseur (cercle intérieur : partie pleine d'un pinceau doux)
    if ((m_edit.tool == Tool::Brush || m_edit.tool == Tool::Eraser) && m_mouseInside && !HitEdit(m_mouse) &&
        !m_edit.spacePan && m_edit.fx < 0 && m_edit.slider < 0) {
        const D2D1_POINT_2F c = D2D1::Point2F(float(m_mouse.x), float(m_mouse.y));
        float r = std::max(1.5f, m_edit.size * z / 2);
        SetColor(0x000000, 0.6f);
        m_rt->DrawEllipse(D2D1::Ellipse(c, r + 1, r + 1), m_brush.Get(), 1);
        SetColor(kWhite, 0.9f);
        m_rt->DrawEllipse(D2D1::Ellipse(c, r, r), m_brush.Get(), 1);
        if (m_edit.hardness < 99.5f && r * m_edit.hardness / 100 > 3) {
            SetColor(kWhite, 0.35f);
            m_rt->DrawEllipse(D2D1::Ellipse(c, r * m_edit.hardness / 100, r * m_edit.hardness / 100), m_brush.Get(), 1);
        }
    }
}

// Barre d'options de l'outil (sous la barre du haut), comme dans Photoshop
void Viewer::DrawOptionsBar(float cw, float y, float h) {
    const float s = m_scale;
    SetColor(0x18191C, 0.96f);
    m_rt->FillRectangle(D2D1::RectF(0, y, cw, y + h), m_brush.Get());
    SetColor(kWhite, 0.06f);
    m_rt->FillRectangle(D2D1::RectF(0, y + h - 1, cw, y + h), m_brush.Get());
    DrawToolIcon(m_edit.tool, D2D1::RectF(12 * s, y, 52 * s, y + h), kAccent, 0.95f);
    SetColor(kWhite, 0.1f);
    m_rt->FillRectangle(D2D1::RectF(60 * s, y + 8 * s, 60 * s + 1, y + h - 8 * s), m_brush.Get());
    float x = 74 * s;
    auto width = [&](const std::wstring& t) {
        ComPtr<IDWriteTextLayout> lay;
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(m_dwrite->CreateTextLayout(t.c_str(), UINT32(t.size()), m_tfSmall.Get(), 1000, 100, &lay))) lay->GetMetrics(&tm);
        return tm.width;
    };
    auto slider = [&](int id, const wchar_t* label, const wchar_t* value, const wchar_t* tip) {
        const float lw = width(label);
        Text(label, m_tfSmall.Get(), D2D1::RectF(x, y, x + lw + 4 * s, y + h), kGrey, 1);
        x += lw + 12 * s;
        DrawSlider(id, D2D1::RectF(x, y + h / 2 - 8 * s, x + 110 * s, y + h / 2 + 8 * s), tip);
        x += 120 * s;
        Text(value, m_tfSmall.Get(), D2D1::RectF(x, y, x + 54 * s, y + h), kWhite, 0.95f);
        x += 64 * s;
    };
    wchar_t sizeTxt[32], opTxt[32], hardTxt[32];
    swprintf_s(sizeTxt, L"%.0f px", m_edit.tool == Tool::Text ? m_edit.fontSize : m_edit.size);
    swprintf_s(opTxt, L"%.0f %%", m_edit.opacity);
    swprintf_s(hardTxt, L"%.0f %%", m_edit.hardness);
    const Tool t = m_edit.tool;
    const bool painting = t == Tool::Brush || t == Tool::Eraser;
    const bool shape = t == Tool::Line || t == Tool::Arrow || t == Tool::Rect || t == Tool::Ellipse;
    if (painting || shape) slider(kSlSize, T("edit.label.size"), sizeTxt, T("edit.tip.thickness"));
    if (t == Tool::Text) slider(kSlSize, T("edit.label.font"), sizeTxt, T("edit.tip.font_size"));
    if (painting || shape || t == Tool::Text || t == Tool::Fill || t == Tool::Gradient)
        slider(kSlOpacity, T("edit.label.opacity"), opTxt, T("edit.tip.opacity"));
    if (painting) slider(kSlHardness, T("edit.label.hardness"), hardTxt, T("edit.tip.hardness"));
    if (t == Tool::Rect || t == Tool::Ellipse) {  // formes pleines / contour
        const D2D1_RECT_F fr = D2D1::RectF(x, y + 4 * s, x + 32 * s, y + h - 4 * s);
        EditBtn(EditCmd::FillToggle, 0, fr, T(m_edit.fill ? "edit.tip.fill_on" : "edit.tip.fill_off"), m_edit.fill);
        const D2D1_RECT_F sq = D2D1::RectF(fr.left + 8 * s, fr.top + 8 * s, fr.right - 8 * s, fr.bottom - 8 * s);
        SetColor(m_edit.color, 1);
        if (m_edit.fill) m_rt->FillRectangle(sq, m_brush.Get());
        else m_rt->DrawRectangle(sq, m_brush.Get(), 2 * s);
        x = fr.right + 12 * s;
    }
    if (t == Tool::Gradient) {  // vers la couleur secondaire / vers transparent
        const D2D1_RECT_F gr = D2D1::RectF(x, y + 5 * s, x + 64 * s, y + h - 5 * s);
        EditBtn(EditCmd::GradToggle, 0, gr, T(m_edit.gradTransparent ? "edit.tip.grad_transparent" : "edit.tip.grad_color"));
        const D2D1_RECT_F sw = D2D1::RectF(gr.left + 6 * s, gr.top + 6 * s, gr.right - 6 * s, gr.bottom - 6 * s);
        if (m_checker) m_rt->FillRectangle(sw, m_checker.Get());
        D2D1_GRADIENT_STOP st[2] = {{0, Rgb(m_edit.color)}, {1, m_edit.gradTransparent ? Rgb(m_edit.color, 0) : Rgb(m_edit.color2)}};
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> gb;
        if (SUCCEEDED(m_rt->CreateGradientStopCollection(st, 2, &coll)) &&
            SUCCEEDED(m_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(sw.left, 0), D2D1::Point2F(sw.right, 0)), coll.Get(), &gb)))
            m_rt->FillRectangle(sw, gb.Get());
        SetColor(kWhite, 0.4f);
        m_rt->DrawRectangle(sw, m_brush.Get(), 1);
        x = gr.right + 8 * s;
        Text(T(m_edit.gradTransparent ? "edit.label.grad_transparent" : "edit.label.grad_color"), m_tfSmall.Get(),
             D2D1::RectF(x, y, x + 220 * s, y + h), kGrey, 1);
        x += 230 * s;
    }
    // Outils sans réglage : aide
    const char* hint = nullptr;
    switch (t) {
    case Tool::Hand: hint = "edit.hint.hand"; break;
    case Tool::Move: hint = "edit.hint.move"; break;
    case Tool::Select: hint = "edit.hint.select"; break;
    case Tool::Picker: hint = "edit.hint.picker"; break;
    case Tool::Crop: hint = "edit.hint.crop"; break;
    default: break;
    }
    std::wstring hintText = hint ? T(hint) : L"";
    if (t == Tool::Select && m_edit.selValid) {
        wchar_t b[64];
        swprintf_s(b, L"%.0f × %.0f   ·   ", m_edit.sel.right - m_edit.sel.left, m_edit.sel.bottom - m_edit.sel.top);
        hintText = b + hintText;
    }
    if (!hintText.empty()) Text(hintText, m_tfSmall.Get(), D2D1::RectF(x, y, cw - 260 * s, y + h), kGrey, 1);
}

// Menus déroulants (Redimensionner, Effets, Mode de fusion), dessinés par-dessus tout
void Viewer::DrawEditMenu(float cw, float ch) {
    if (m_edit.menu == EditMenu::None) return;
    const float s = m_scale, ih = 30 * s;
    const D2D1_RECT_F a = m_edit.menuAnchor;
    int n = 0;
    float iw = 200 * s;
    switch (m_edit.menu) {
    case EditMenu::Resize: n = int(ARRAYSIZE(kResize)); iw = 220 * s; break;
    case EditMenu::Effects: n = EffectCount(); iw = 230 * s; break;
    case EditMenu::Blend: n = 8; iw = a.right - a.left; break;
    default: return;
    }
    const float seps = m_edit.menu == EditMenu::Effects ? 3 * 9 * s : 0;
    float left = m_edit.menu == EditMenu::Resize ? a.right - iw : a.left;
    left = std::clamp(left, 6 * s, cw - iw - 6 * s);
    float top = a.bottom + 6 * s;
    const float height = ih * n + seps + 8 * s;
    if (top + height > ch - 6 * s) top = std::max(6 * s, a.top - 6 * s - height);
    const D2D1_RECT_F menu = D2D1::RectF(left, top, left + iw, top + height);
    FillRound(menu, 8 * s, kPanel, 0.98f);
    SetColor(kWhite, 0.12f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(menu, 8 * s, 8 * s), m_brush.Get(), 1);
    m_editButtons.push_back({EditCmd::Panel, 0, menu, nullptr});
    float iy = menu.top + 4 * s;
    for (int i = 0; i < n; ++i) {
        const D2D1_RECT_F r = D2D1::RectF(menu.left + 4 * s, iy, menu.right - 4 * s, iy + ih);
        std::wstring label;
        switch (m_edit.menu) {
        case EditMenu::Resize: {
            EditBtn(EditCmd::ResizeOpt, i, r, nullptr);
            const ResizeOption& o = kResize[i];
            double k = o.percent ? o.value / 100.0 : double(o.value) / std::max(m_edit.w, m_edit.h);
            const std::wstring base = o.percent ? Tf("edit.resize.percent", o.value) : Tf("edit.resize.long_side", o.value);
            wchar_t b[160];
            swprintf_s(b, L"%ls   (%ld × %ld)", base.c_str(), std::lround(m_edit.w * k), std::lround(m_edit.h * k));
            label = b;
            break;
        }
        case EditMenu::Effects:
            EditBtn(EditCmd::FxPick, i, r, nullptr);
            label = T(EffectKey(i));
            break;
        default:
            EditBtn(EditCmd::BlendOpt, i, r, nullptr, i == CurLayer().blend);
            label = T(BlendKey(i));
            break;
        }
        Text(label, m_tfSmall.Get(), D2D1::RectF(r.left + 10 * s, r.top, r.right - 4 * s, r.bottom), kWhite, 0.95f);
        iy += ih;
        if (m_edit.menu == EditMenu::Effects && (i == 2 || i == 5 || i == 9)) {  // réglages / couleur / filtres / autres
            SetColor(kWhite, 0.1f);
            m_rt->FillRectangle(D2D1::RectF(menu.left + 10 * s, iy + 4 * s, menu.right - 10 * s, iy + 4 * s + 1), m_brush.Get());
            iy += 9 * s;
        }
    }
}

void Viewer::DrawEditUi(float cw, float ch) {
    const float s = m_scale;
    // Survol : cherché parmi les boutons de l'image précédente (ceux de cette image ne sont pas encore
    // construits). Copie : la liste est reconstruite pendant le dessin.
    m_editHover.reset();
    if (m_mouseInside && !m_edit.drawing && m_edit.slider < 0)
        for (auto it = m_prevEditButtons.rbegin(); it != m_prevEditButtons.rend(); ++it)
            if (Contains(it->rc, m_mouse)) {
                m_editHover = *it;
                break;
            }
    // Les boutons du recadrage ont été ajoutés par DrawEditOverlay : on garde ceux-là
    std::vector<EditButton> overlayButtons = std::move(m_editButtons);
    m_editButtons.clear();

    // --- Barre du haut ---
    const float barH = 52 * s, optH = 40 * s;
    SetColor(kPanel, 0.96f);
    m_rt->FillRectangle(D2D1::RectF(0, 0, cw, barH), m_brush.Get());
    SetColor(kWhite, 0.06f);
    m_rt->FillRectangle(D2D1::RectF(0, barH - 1, cw, barH), m_brush.Get());

    const float bs = 36 * s, by = 8 * s, gap = 2 * s;
    float x = cw - 10 * s;
    // Terminer
    D2D1_RECT_F done = D2D1::RectF(x - 110 * s, by, x, by + bs);
    bool hDone = EditBtn(EditCmd::Done, 0, done, T("edit.tip.done"));
    FillRound(done, 6 * s, kAccent, hDone ? 1.0f : 0.85f);
    IconLabel(done, L"", T("edit.btn.done"), 0x0B1A2A, 1);
    x = done.left - 8 * s;
    // Enregistrer sous
    D2D1_RECT_F save = D2D1::RectF(x - 150 * s, by, x, by + bs);
    EditBtn(EditCmd::SaveAs, 0, save, T("edit.tip.save_as"));
    SetColor(kWhite, 0.25f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(save, 6 * s, 6 * s), m_brush.Get(), 1);
    IconLabel(save, L"", T(m_edit.dirty ? "edit.btn.save_as_dirty" : "edit.btn.save_as"), kWhite, 0.95f);
    x = save.left - 14 * s;

    struct Ico { EditCmd cmd; const wchar_t* glyph; const wchar_t* tip; };
    const Ico transforms[] = {{EditCmd::FlipV, L"⇅", T("edit.tip.flip_v")},
                              {EditCmd::FlipH, L"⇄", T("edit.tip.flip_h")},
                              {EditCmd::RotR, L"", T("edit.tip.rot_right")},
                              {EditCmd::RotL, L"", T("edit.tip.rot_left")}};
    // Redimensionner
    D2D1_RECT_F rs = D2D1::RectF(x - bs, by, x, by + bs);
    EditBtn(EditCmd::ResizeMenu, 0, rs, T("edit.tip.resize"), m_edit.menu == EditMenu::Resize);
    Text(L"", m_tfIcon.Get(), rs, kWhite, 0.92f);
    if (m_edit.menu == EditMenu::Resize) m_edit.menuAnchor = rs;
    x = rs.left - gap;
    for (auto& t : transforms) {
        D2D1_RECT_F r = D2D1::RectF(x - bs, by, x, by + bs);
        EditBtn(t.cmd, 0, r, t.tip);
        if (t.cmd == EditCmd::RotL) {  // même icône, en miroir
            float c = (r.left + r.right) / 2;
            m_rt->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1, D2D1::Point2F(c, 0)));
            Text(t.glyph, m_tfIcon.Get(), r, kWhite, 0.92f);
            m_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        } else {
            Text(t.glyph, t.cmd == EditCmd::RotR ? m_tfIcon.Get() : m_tfToast.Get(), r, kWhite, 0.92f);
        }
        x = r.left - gap;
    }
    x -= 12 * s;
    // Effets
    {
        const std::wstring label = T("edit.btn.effects");
        ComPtr<IDWriteTextLayout> lay;
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(m_dwrite->CreateTextLayout(label.c_str(), UINT32(label.size()), m_tfLabel.Get(), 1000, 100, &lay))) lay->GetMetrics(&tm);
        const float w = std::max(100 * s, tm.width + 64 * s);
        D2D1_RECT_F fxr = D2D1::RectF(x - w, by, x, by + bs);
        EditBtn(EditCmd::FxMenu, 0, fxr, T("edit.tip.effects"), m_edit.menu == EditMenu::Effects || m_edit.fx >= 0);
        SetColor(kWhite, 0.18f);
        m_rt->DrawRoundedRectangle(D2D1::RoundedRect(fxr, 6 * s, 6 * s), m_brush.Get(), 1);
        IconLabel(D2D1::RectF(fxr.left, fxr.top, fxr.right - 14 * s, fxr.bottom), L"", label, kWhite, 0.95f);
        Text(L"", m_tfIcon.Get(), D2D1::RectF(fxr.right - 24 * s, fxr.top, fxr.right - 4 * s, fxr.bottom), kGrey, 0.8f);
        if (m_edit.menu == EditMenu::Effects) m_edit.menuAnchor = fxr;
        x = fxr.left - 14 * s;
    }
    // Annuler / Rétablir
    D2D1_RECT_F redo = D2D1::RectF(x - bs, by, x, by + bs);
    EditBtn(EditCmd::Redo, 0, redo, T("edit.tip.redo"), false, !m_edit.redo.empty());
    Text(L"", m_tfIcon.Get(), redo, kWhite, m_edit.redo.empty() ? 0.3f : 0.92f);
    const bool canUndo = !m_edit.undo.empty() || m_edit.textActive || m_edit.fx >= 0;
    D2D1_RECT_F undo = D2D1::RectF(redo.left - gap - bs, by, redo.left - gap, by + bs);
    EditBtn(EditCmd::Undo, 0, undo, T("edit.tip.undo"), false, canUndo);
    Text(L"", m_tfIcon.Get(), undo, kWhite, canUndo ? 0.92f : 0.3f);
    // Titre
    if (const FileEntry* cur = Current()) {
        float tr = undo.left - 16 * s;
        Text(T("edit.title"), m_tfTitle.Get(), D2D1::RectF(16 * s, 7 * s, tr, 28 * s), kAccent, 1);
        wchar_t dims[64];
        swprintf_s(dims, L"   ·   %u × %u", m_edit.w, m_edit.h);
        Text(cur->name + dims + (m_edit.dirty ? L"   ·   " + std::wstring(T("edit.modified")) : std::wstring()), m_tfSub.Get(), D2D1::RectF(16 * s, 28 * s, tr, 46 * s), kGrey, 1);
    }

    // --- Barre d'options de l'outil ---
    DrawOptionsBar(cw, barH, optH);

    // --- Outils (colonne de gauche) ---
    const int nt = int(ARRAYSIZE(kTools));
    const float tx = 10 * s;
    float ty = barH + optH + 10 * s;
    const float tb = std::clamp((ch - ty - 80 * s) / nt, 26 * s, 40 * s);
    D2D1_RECT_F panel = D2D1::RectF(tx - 4 * s, ty - 4 * s, tx + tb + 4 * s, ty + tb * nt + 4 * s);
    FillRound(panel, 8 * s, kPanel, 0.93f);
    for (int i = 0; i < nt; ++i) {
        const ToolDef& td = kTools[i];
        if (i == 4 || i == 8) {  // groupes : sélection / peinture / texte et formes
            SetColor(kWhite, 0.08f);
            m_rt->FillRectangle(D2D1::RectF(tx + 6 * s, ty - 0.5f, tx + tb - 6 * s, ty + 0.5f), m_brush.Get());
        }
        D2D1_RECT_F r = D2D1::RectF(tx, ty, tx + tb, ty + tb);
        bool active = int(m_edit.tool) == td.tool;
        EditBtn(EditCmd::Tool, td.tool, r, T(td.tip), active);
        DrawToolIcon(Tool(td.tool), r, active ? kAccent : kWhite, 0.95f);
        ty += tb;
    }

    // --- Couleurs (bas) ---
    const float sw = 22 * s, sg = 4 * s;
    float px = tx, pyb = ch - 12 * s;
    D2D1_RECT_F cpanel = D2D1::RectF(px - 4 * s, pyb - 52 * s, px + 50 * s + 8 * (sw + sg) + 40 * s, pyb);
    FillRound(cpanel, 8 * s, kPanel, 0.93f);
    // couleur principale (devant) et secondaire (derrière)
    D2D1_RECT_F c2 = D2D1::RectF(px + 18 * s, pyb - 34 * s, px + 44 * s, pyb - 8 * s);
    D2D1_RECT_F c1 = D2D1::RectF(px + 4 * s, pyb - 46 * s, px + 30 * s, pyb - 20 * s);
    SetColor(m_edit.color2, 1);
    m_rt->FillRectangle(c2, m_brush.Get());
    SetColor(kWhite, 0.5f);
    m_rt->DrawRectangle(c2, m_brush.Get(), 1);
    SetColor(m_edit.color, 1);
    m_rt->FillRectangle(c1, m_brush.Get());
    SetColor(kWhite, 0.9f);
    m_rt->DrawRectangle(c1, m_brush.Get(), 1.5f * s);
    m_editButtons.push_back({EditCmd::SwapColors, 0, D2D1::RectF(c1.left, c1.top, c2.right, c2.bottom),
                             T("edit.tip.swap_colors")});
    float sx = px + 54 * s;
    for (int i = 0; i < 16; ++i) {
        int col = i / 2, row = i % 2;
        D2D1_RECT_F r = D2D1::RectF(sx + col * (sw + sg), pyb - 48 * s + row * (sw + sg), sw + sx + col * (sw + sg),
                                    pyb - 48 * s + row * (sw + sg) + sw);
        SetColor(kPalette[i], 1);
        m_rt->FillRectangle(r, m_brush.Get());
        bool h = m_editHover && m_editHover->cmd == EditCmd::Color && m_editHover->arg == i;
        SetColor(kWhite, h ? 0.9f : 0.18f);
        m_rt->DrawRectangle(r, m_brush.Get(), h ? 2 * s : 1);
        m_editButtons.push_back({EditCmd::Color, i, r, T("edit.tip.palette")});
    }
    D2D1_RECT_F plus = D2D1::RectF(sx + 8 * (sw + sg) + 4 * s, pyb - 46 * s, sx + 8 * (sw + sg) + 36 * s, pyb - 14 * s);
    EditBtn(EditCmd::CustomColor, 0, plus, T("edit.tip.custom_color"));
    Text(L"", m_tfIcon.Get(), plus, kWhite, 0.9f);

    // --- Panneau de droite : réglages de l'effet, ou calques ---
    if (m_edit.fx >= 0) DrawEffectPanel(cw, ch);
    else if (m_edit.showLayers) DrawLayersPanel(cw, ch);

    // Boutons de l'overlay (recadrage) au-dessus du reste, puis menus
    for (auto& b : overlayButtons) m_editButtons.push_back(b);
    DrawEditMenu(cw, ch);

    // Info-bulle
    const EditButton* hov = m_editHover ? &*m_editHover : nullptr;
    if (hov && hov->tip && m_edit.menu == EditMenu::None) {
        std::wstring tip = hov->tip;
        ComPtr<IDWriteTextLayout> lay;
        m_dwrite->CreateTextLayout(tip.c_str(), UINT32(tip.size()), m_tfSmall.Get(), 500 * s, 30 * s, &lay);
        if (lay) {
            DWRITE_TEXT_METRICS tm;
            lay->GetMetrics(&tm);
            float w = tm.width + 20 * s;
            float l = std::clamp(hov->rc.left, 6 * s, cw - w - 6 * s);
            float top = hov->rc.bottom + 6 * s;
            if (hov->rc.left < 60 * s && hov->cmd == EditCmd::Tool) {  // outils : à droite de la colonne
                l = hov->rc.right + 10 * s;
                top = hov->rc.top + 6 * s;
            }
            if (top + 26 * s > ch) top = hov->rc.top - 32 * s;
            D2D1_RECT_F r = D2D1::RectF(l, top, l + w, top + 26 * s);
            FillRound(r, 4 * s, 0x000000, 0.85f);
            Text(tip, m_tfCenter.Get(), r, kWhite, 1);
        }
    }
}
