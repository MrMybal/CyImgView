// Mode édition (façon Paint) et « Enregistrer sous ».
//
// Le document est une texture Direct2D (cible de rendu) aux dimensions de l'image : pinceau,
// formes et texte y sont dessinés directement par le GPU, avec antialiasing. L'orientation EXIF
// et la rotation d'affichage sont intégrées aux pixels à l'entrée du mode. Chaque modification
// empile une copie de la texture (annuler / rétablir), dans la limite d'un budget mémoire.

#include "viewer.h"
#include "viewer_util.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

using namespace ui;

namespace {

constexpr size_t kUndoBudget = 1200ull << 20;  // mémoire vidéo maximale pour l'historique
constexpr size_t kUndoMaxSteps = 50;

const uint32_t kPalette[16] = {0x000000, 0x7F7F7F, 0xC3C3C3, 0xFFFFFF, 0x880015, 0xE53935, 0xFF7F27, 0xFFC90E,
                               0x22B14C, 0x00A2E8, 0x3F48CC, 0xA349A4, 0xB97A57, 0xFFAEC9, 0x99D9EA, 0x7092BE};

struct ResizeOption {
    const wchar_t* label;
    bool percent;
    int value;
};
const ResizeOption kResize[] = {
    {L"25 %", true, 25},          {L"50 %", true, 50},         {L"75 %", true, 75},
    {L"150 %", true, 150},        {L"200 %", true, 200},       {L"Grand côté 800 px", false, 800},
    {L"Grand côté 1280 px", false, 1280}, {L"Grand côté 1920 px", false, 1920}, {L"Grand côté 2560 px", false, 2560},
    {L"Grand côté 3840 px", false, 3840},
};

struct ToolDef {
    int tool;  // Viewer::Tool (privé : on passe par int)
    const wchar_t* tip;
};
const ToolDef kTools[] = {
    {0, L"Déplacer la vue (M, ou Espace maintenu)"},
    {1, L"Pinceau (B) — clic droit : 2e couleur"},
    {2, L"Gomme (G)"},
    {3, L"Ligne (L) — Maj : angles de 45°"},
    {4, L"Flèche (A)"},
    {5, L"Rectangle (R) — Maj : carré"},
    {6, L"Ellipse (O) — Maj : cercle"},
    {7, L"Texte (T)"},
    {8, L"Remplissage (F)"},
    {9, L"Pipette (K)"},
    {10, L"Recadrer (C) — Entrée pour valider"},
};

D2D1_RECT_F Norm(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    return D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
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
        ShowToast(L"Aucune image à enregistrer");
        return;
    }
    if (m_edit.textActive) CommitText();
    if (m_saveFormats.empty()) m_saveFormats = EnumerateSaveFormats();
    if (m_saveFormats.empty()) {
        MessageBoxW(m_hwnd, L"Aucun encodeur d'image n'est disponible.", L"CyImgView", MB_ICONERROR);
        return;
    }

    // Pixels tels qu'affichés : document édité, ou image orientée (EXIF + rotation manuelle)
    PixelBuffer px;
    uint32_t w = 0, h = 0, stride = 0;
    bool alpha = false;
    if (m_edit.active) {
        if (!ReadCanvas(px, stride)) {
            ShowToast(L"Lecture de l'image impossible");
            return;
        }
        w = m_edit.w;
        h = m_edit.h;
        alpha = m_edit.hasAlpha;
    } else {
        bool flip;
        int rot;
        FlipRot(flip, rot);
        if (!OrientPixels(CurPixels(), m_cur->width, m_cur->height, m_cur->stride, flip, rot, px, w, h, stride)) {
            ShowToast(L"Mémoire insuffisante");
            return;
        }
        alpha = m_cur->hasAlpha;
    }

    // Proposition : même nom, même format si on sait l'écrire, sinon JPEG (ou PNG si transparence)
    std::wstring ext = ExtensionOf(cur->path);
    bool known = std::any_of(m_saveFormats.begin(), m_saveFormats.end(), [&](const SaveFormat& f) {
        return std::find(f.exts.begin(), f.exts.end(), ext) != f.exts.end();
    });
    if (!known) ext = alpha ? L"png" : L"jpg";
    std::wstring base = cur->name;
    if (size_t dot = base.find_last_of(L'.'); dot != std::wstring::npos) base.resize(dot);

    SaveRequest req;
    if (!ShowSaveDialog(m_hwnd, m_saveFormats, m_folder, base, ext, req)) return;
    const SaveFormat& fmt = m_saveFormats[size_t(req.format)];
    std::wstring err;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = SaveImage(req, fmt, px.Data(), w, h, stride, alpha, err);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!ok) {
        MessageBoxW(m_hwnd, (L"L'enregistrement a échoué :\n" + err).c_str(), L"CyImgView", MB_ICONERROR);
        return;
    }
    ShowToast(L"Enregistré : " + FileNameOf(req.path));
    if (m_edit.active) m_edit.dirty = false;
    ForgetFile(req.path);
    const bool overwroteCurrent = Key(req.path) == Key(cur->path);
    if (Key(ParentOf(req.path)) == Key(m_folder)) ReloadList();
    if (overwroteCurrent && !m_edit.active) GoTo(m_index);  // recharge l'image réécrite
}

// =============================================================================================
// Document (texture GPU)

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

bool Viewer::ReadCanvas(PixelBuffer& out, uint32_t& stride) {
    if (!m_edit.canvas) return false;
    D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1Bitmap1> rb;
    if (FAILED(m_dc->CreateBitmap(D2D1::SizeU(m_edit.w, m_edit.h), nullptr, 0, p, &rb)) ||
        FAILED(rb->CopyFromBitmap(nullptr, m_edit.canvas.Get(), nullptr)))
        return false;
    D2D1_MAPPED_RECT m{};
    if (FAILED(rb->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
    stride = m_edit.w * 4;
    bool ok = out.Alloc(size_t(stride) * m_edit.h);
    if (ok)
        for (uint32_t y = 0; y < m_edit.h; ++y) memcpy(out.Data() + size_t(y) * stride, m.bits + size_t(y) * m.pitch, stride);
    rb->Unmap();
    return ok;
}

bool Viewer::PushUndo() {
    ComPtr<ID2D1Bitmap1> copy = NewCanvas(m_edit.w, m_edit.h);
    if (!copy || FAILED(copy->CopyFromBitmap(nullptr, m_edit.canvas.Get(), nullptr))) return false;
    m_edit.undo.push_back(m_edit.canvas);  // l'ancienne texture part dans l'historique...
    m_edit.canvas = copy;                  // ...et on dessine sur la copie
    m_edit.redo.clear();
    // Budget : on oublie les plus vieux états
    auto bytes = [](const ComPtr<ID2D1Bitmap1>& b) {
        D2D1_SIZE_U s = b->GetPixelSize();
        return size_t(s.width) * s.height * 4;
    };
    size_t total = bytes(m_edit.canvas);
    for (auto& b : m_edit.undo) total += bytes(b);
    while (!m_edit.undo.empty() && (total > kUndoBudget || m_edit.undo.size() > kUndoMaxSteps)) {
        total -= bytes(m_edit.undo.front());
        m_edit.undo.erase(m_edit.undo.begin());
    }
    m_edit.dirty = true;
    return true;
}

void Viewer::ReplaceCanvas(ComPtr<ID2D1Bitmap1> c) {
    if (!c) return;
    m_edit.undo.push_back(m_edit.canvas);
    m_edit.redo.clear();
    m_edit.canvas = c;
    D2D1_SIZE_U s = c->GetPixelSize();
    m_edit.w = s.width;
    m_edit.h = s.height;
    m_edit.dirty = true;
    m_edit.cropValid = false;
    ApplyFit(false);
}

void Viewer::Undo() {
    if (m_edit.textActive) {
        m_edit.textActive = false;
        m_edit.text.clear();
        Invalidate();
        return;
    }
    if (m_edit.undo.empty()) return;
    m_edit.redo.push_back(m_edit.canvas);
    m_edit.canvas = m_edit.undo.back();
    m_edit.undo.pop_back();
    D2D1_SIZE_U s = m_edit.canvas->GetPixelSize();
    bool resized = s.width != m_edit.w || s.height != m_edit.h;
    m_edit.w = s.width;
    m_edit.h = s.height;
    m_edit.dirty = true;
    m_edit.cropValid = false;
    if (resized) ApplyFit(false);
    Invalidate();
}

void Viewer::Redo() {
    if (m_edit.redo.empty()) return;
    m_edit.undo.push_back(m_edit.canvas);
    m_edit.canvas = m_edit.redo.back();
    m_edit.redo.pop_back();
    D2D1_SIZE_U s = m_edit.canvas->GetPixelSize();
    bool resized = s.width != m_edit.w || s.height != m_edit.h;
    m_edit.w = s.width;
    m_edit.h = s.height;
    m_edit.dirty = true;
    if (resized) ApplyFit(false);
    Invalidate();
}

// =============================================================================================
// Entrée / sortie du mode

bool Viewer::BeginEdit() {
    if (m_edit.active) return true;
    if (!m_cur || !CreateDeviceResources()) {
        ShowToast(m_loading ? L"Image en cours de chargement…" : L"Aucune image à modifier");
        return false;
    }
    bool flip;
    int rot;
    FlipRot(flip, rot);
    PixelBuffer px;
    uint32_t w, h, stride;
    if (!OrientPixels(CurPixels(), m_cur->width, m_cur->height, m_cur->stride, flip, rot, px, w, h, stride)) {
        ShowToast(L"Mémoire insuffisante");
        return false;
    }
    if (std::max(w, h) > m_dc->GetMaximumBitmapSize()) {
        ShowToast(L"Image trop grande pour l'édition");
        return false;
    }
    ComPtr<ID2D1Bitmap1> canvas = NewCanvas(w, h);
    if (!canvas || FAILED(canvas->CopyFromMemory(nullptr, px.Data(), stride))) {
        ShowToast(L"Mémoire vidéo insuffisante");
        return false;
    }
    EditState fresh;
    fresh.tool = m_edit.tool;  // on garde les réglages d'une session à l'autre
    fresh.color = m_edit.color;
    fresh.color2 = m_edit.color2;
    fresh.size = m_edit.size;
    fresh.fontSize = m_edit.fontSize;
    fresh.fill = m_edit.fill;
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
    m_edit.canvas = canvas;
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
    if (ask && m_edit.dirty) {
        TASKDIALOG_BUTTON buttons[] = {{100, L"Enregistrer sous…"}, {101, L"Ne pas enregistrer"}};
        TASKDIALOGCONFIG tc{sizeof(tc)};
        tc.hwndParent = m_hwnd;
        tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        tc.pszWindowTitle = L"CyImgView";
        tc.pszMainIcon = TD_WARNING_ICON;
        tc.pszMainInstruction = L"Enregistrer les modifications ?";
        tc.pszContent = L"L'image a été modifiée. Les changements seront perdus si vous ne les enregistrez pas.";
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
    keep.fill = m_edit.fill;
    m_edit = std::move(keep);
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

float& Viewer::SliderValue() { return m_edit.tool == Tool::Text ? m_edit.fontSize : m_edit.size; }

// =============================================================================================
// Outils

void Viewer::SetTool(Tool t) {
    if (m_edit.textActive && t != Tool::Text) CommitText();
    if (t != Tool::Crop) m_edit.cropValid = false;
    if (t == Tool::Picker && m_edit.tool != Tool::Picker) m_edit.prevTool = m_edit.tool;
    m_edit.tool = t;
    m_edit.resizeMenu = false;
    Invalidate();
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

void Viewer::StrokeTo(D2D1_POINT_2F a, D2D1_POINT_2F b, bool dot) {
    const bool erase = m_edit.tool == Tool::Eraser;
    const bool transparent = erase && m_edit.hasAlpha;
    uint32_t col = erase ? m_edit.color2 : (m_edit.secondary ? m_edit.color2 : m_edit.color);
    DrawOn(m_edit.canvas.Get(), [&] {
        if (transparent) m_dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        m_brush->SetColor(transparent ? D2D1::ColorF(0, 0, 0, 0) : Rgb(col));
        if (dot) m_dc->FillEllipse(D2D1::Ellipse(b, m_edit.size / 2, m_edit.size / 2), m_brush.Get());
        else m_dc->DrawLine(a, b, m_brush.Get(), m_edit.size, m_roundStroke.Get());
        m_dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    });
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
    DrawOn(m_edit.canvas.Get(), [&] {
        m_brush->SetColor(Rgb(m_edit.color));
        m_dc->DrawTextLayout(m_edit.textPos, lay.Get(), m_brush.Get());
    });
    m_edit.text.clear();
    Invalidate();
}

void Viewer::FloodFill(D2D1_POINT_2F p, uint32_t rgb) {
    const int x0 = int(p.x), y0 = int(p.y);
    if (x0 < 0 || y0 < 0 || x0 >= int(m_edit.w) || y0 >= int(m_edit.h)) return;
    PixelBuffer px;
    uint32_t stride;
    if (!ReadCanvas(px, stride)) return;
    const int W = int(m_edit.w), H = int(m_edit.h);
    uint32_t* pix = reinterpret_cast<uint32_t*>(px.Data());
    const uint32_t target = pix[size_t(y0) * W + x0];
    const uint32_t fillc = 0xFF000000u | rgb;  // BGRA en mémoire = 0xAARRGGBB
    if (target == fillc) return;
    auto close = [&](uint32_t c) {  // tolérance : les bords antialiasés sont inclus
        int d = 0;
        for (int s = 0; s < 32; s += 8) d = std::max(d, std::abs(int((c >> s) & 0xFF) - int((target >> s) & 0xFF)));
        return d <= 40;
    };
    std::vector<uint8_t> seen(size_t(W) * H, 0);
    std::vector<std::pair<int, int>> stack{{x0, y0}};
    while (!stack.empty()) {  // remplissage par segments horizontaux
        auto [x, y] = stack.back();
        stack.pop_back();
        size_t row = size_t(y) * W;
        if (seen[row + x] || !close(pix[row + x])) continue;
        int l = x, r = x;
        while (l > 0 && !seen[row + l - 1] && close(pix[row + l - 1])) --l;
        while (r < W - 1 && !seen[row + r + 1] && close(pix[row + r + 1])) ++r;
        for (int i = l; i <= r; ++i) {
            pix[row + i] = fillc;
            seen[row + i] = 1;
            if (y > 0 && !seen[row - W + i]) stack.push_back({i, y - 1});
            if (y < H - 1 && !seen[row + W + i]) stack.push_back({i, y + 1});
        }
    }
    if (!PushUndo()) return;
    m_edit.canvas->CopyFromMemory(nullptr, px.Data(), stride);
    Invalidate();
}

void Viewer::PickColor(D2D1_POINT_2F p, bool secondary) {
    const int x = int(p.x), y = int(p.y);
    if (x < 0 || y < 0 || x >= int(m_edit.w) || y >= int(m_edit.h)) return;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1Bitmap1> rb;
    D2D1_RECT_U src = D2D1::RectU(x, y, x + 1, y + 1);
    D2D1_POINT_2U dst = D2D1::Point2U(0, 0);
    D2D1_MAPPED_RECT m{};
    if (FAILED(m_dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, bp, &rb)) ||
        FAILED(rb->CopyFromBitmap(&dst, m_edit.canvas.Get(), &src)) || FAILED(rb->Map(D2D1_MAP_OPTIONS_READ, &m)))
        return;
    uint8_t b = m.bits[0], g = m.bits[1], r = m.bits[2], a = m.bits[3];
    rb->Unmap();
    if (a > 0 && a < 255) {  // dé-prémultiplication
        b = uint8_t(std::min(255, b * 255 / a));
        g = uint8_t(std::min(255, g * 255 / a));
        r = uint8_t(std::min(255, r * 255 / a));
    }
    (secondary ? m_edit.color2 : m_edit.color) = uint32_t(r) << 16 | uint32_t(g) << 8 | b;
    SetTool(m_edit.prevTool);
}

// =============================================================================================
// Transformations du document

void Viewer::TransformCanvas(EditCmd kind) {
    const float W = float(m_edit.w), H = float(m_edit.h);
    const bool swap = kind == EditCmd::RotL || kind == EditCmd::RotR;
    ComPtr<ID2D1Bitmap1> out = swap ? NewCanvas(m_edit.h, m_edit.w) : NewCanvas(m_edit.w, m_edit.h);
    if (!out) return;
    D2D1::Matrix3x2F m;
    switch (kind) {
    case EditCmd::RotR: m = D2D1::Matrix3x2F(0, 1, -1, 0, H, 0); break;   // quart de tour horaire
    case EditCmd::RotL: m = D2D1::Matrix3x2F(0, -1, 1, 0, 0, W); break;   // quart de tour antihoraire
    case EditCmd::FlipH: m = D2D1::Matrix3x2F(-1, 0, 0, 1, W, 0); break;
    default: m = D2D1::Matrix3x2F(1, 0, 0, -1, 0, H); break;              // FlipV
    }
    ID2D1Bitmap1* src = m_edit.canvas.Get();
    DrawOn(out.Get(), [&] {
        m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        m_dc->SetTransform(m);
        m_dc->DrawBitmap(src, D2D1::RectF(0, 0, W, H), 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
    });
    ReplaceCanvas(out);
}

void Viewer::ResizeCanvas(uint32_t w, uint32_t h) {
    w = std::clamp<uint32_t>(w, 1, m_dc->GetMaximumBitmapSize());
    h = std::clamp<uint32_t>(h, 1, m_dc->GetMaximumBitmapSize());
    if (w == m_edit.w && h == m_edit.h) return;
    ComPtr<ID2D1Bitmap1> out = NewCanvas(w, h);
    if (!out) {
        ShowToast(L"Mémoire vidéo insuffisante");
        return;
    }
    ID2D1Bitmap1* src = m_edit.canvas.Get();
    DrawOn(out.Get(), [&] {
        m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        m_dc->DrawBitmap(src, D2D1::RectF(0, 0, float(w), float(h)), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                         nullptr, nullptr);
    });
    ReplaceCanvas(out);
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
    ComPtr<ID2D1Bitmap1> out = NewCanvas(r.right - r.left, r.bottom - r.top);
    D2D1_POINT_2U origin = D2D1::Point2U(0, 0);
    if (!out || FAILED(out->CopyFromBitmap(&origin, m_edit.canvas.Get(), &r))) return;
    ReplaceCanvas(out);
}

// =============================================================================================
// Commandes de l'interface

void Viewer::EditCommand(EditCmd cmd, int arg) {
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
    case EditCmd::RotL: case EditCmd::RotR: case EditCmd::FlipH: case EditCmd::FlipV:
        if (m_edit.textActive) CommitText();
        TransformCanvas(cmd);
        break;
    case EditCmd::ResizeMenu: m_edit.resizeMenu = !m_edit.resizeMenu; break;
    case EditCmd::ResizeOpt: {
        m_edit.resizeMenu = false;
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
    default: break;
    }
    Invalidate();
}

const Viewer::EditButton* Viewer::HitEdit(POINT p) const {
    for (auto it = m_editButtons.rbegin(); it != m_editButtons.rend(); ++it)  // menus dessinés en dernier
        if (Contains(it->rc, p)) return &*it;
    return nullptr;
}

// =============================================================================================
// Souris et clavier

bool Viewer::EditMouseDown(POINT p, bool right, bool dbl) {
    if (const EditButton* b = HitEdit(p)) {
        if (b->cmd == EditCmd::Slider) {
            m_edit.sliderDrag = true;
            SetCapture(m_hwnd);
            EditMouseMove(p);
        } else if (b->cmd == EditCmd::Color && right) {
            EditCommand(EditCmd::Color2, b->arg);
        } else {
            EditCommand(b->cmd, b->arg);
        }
        return true;
    }
    if (m_edit.resizeMenu) {
        m_edit.resizeMenu = false;
        Invalidate();
        return true;
    }
    if (m_edit.tool == Tool::Hand || m_edit.spacePan) return false;  // déplacement de la vue

    D2D1_POINT_2F ip = ScreenToImage(p);
    m_edit.secondary = right;
    m_edit.shift = GetKeyState(VK_SHIFT) < 0;
    switch (m_edit.tool) {
    case Tool::Brush:
    case Tool::Eraser:
        if (!PushUndo()) return true;
        m_edit.drawing = true;
        m_edit.start = m_edit.cur = ip;
        StrokeTo(ip, ip, true);
        break;
    case Tool::Line: case Tool::Arrow: case Tool::Rect: case Tool::Ellipse:
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
    if (m_edit.sliderDrag) {
        float t = std::clamp((p.x - m_sliderRect.left) / std::max(1.0f, m_sliderRect.right - m_sliderRect.left), 0.0f, 1.0f);
        if (m_edit.tool == Tool::Text) m_edit.fontSize = std::round(8.0f * std::pow(50.0f, t));  // 8..400
        else m_edit.size = std::max(1.0f, std::round(std::pow(200.0f, t)));                    // 1..200
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
    if (m_edit.shift) {  // contraintes : carré / cercle / angles de 45°
        float dx = ip.x - m_edit.start.x, dy = ip.y - m_edit.start.y;
        if (m_edit.tool == Tool::Rect || m_edit.tool == Tool::Ellipse || m_edit.tool == Tool::Crop) {
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
    if (m_edit.sliderDrag) {
        m_edit.sliderDrag = false;
        ReleaseCapture();
        return;
    }
    if (!m_edit.drawing) return;
    m_edit.drawing = false;
    ReleaseCapture();
    switch (m_edit.tool) {
    case Tool::Line: case Tool::Arrow: case Tool::Rect: case Tool::Ellipse: {
        float dx = m_edit.cur.x - m_edit.start.x, dy = m_edit.cur.y - m_edit.start.y;
        if (dx * dx + dy * dy < 1) break;
        if (!PushUndo()) break;
        uint32_t col = m_edit.secondary ? m_edit.color2 : m_edit.color;
        Tool t = m_edit.tool;
        D2D1_POINT_2F a = m_edit.start, b = m_edit.cur;
        DrawOn(m_edit.canvas.Get(), [&] { DrawShape(t, a, b, col, m_edit.size, m_edit.fill); });
        break;
    }
    case Tool::Crop: {
        D2D1_RECT_F r = Norm(m_edit.start, m_edit.cur);
        r.left = std::round(std::max(0.0f, r.left));
        r.top = std::round(std::max(0.0f, r.top));
        r.right = std::round(std::min(float(m_edit.w), r.right));
        r.bottom = std::round(std::min(float(m_edit.h), r.bottom));
        m_edit.crop = r;
        m_edit.cropValid = r.right - r.left >= 2 && r.bottom - r.top >= 2;
        break;
    }
    default: break;
    }
    Invalidate();
}

bool Viewer::EditChar(wchar_t c) {
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
    if (ctrl) {
        switch (vk) {
        case 'Z': shift ? Redo() : Undo(); return true;
        case 'Y': Redo(); return true;
        case 'S': SaveAs(); return true;
        case 'C': CopyToClipboard(); return true;
        default: return false;
        }
    }
    switch (vk) {
    case VK_SPACE:
        m_edit.spacePan = true;
        return true;
    case VK_ESCAPE:
        if (m_edit.resizeMenu) m_edit.resizeMenu = false;
        else if (m_edit.cropValid || m_edit.drawing) {
            m_edit.cropValid = false;
            if (m_edit.drawing && m_edit.tool != Tool::Brush && m_edit.tool != Tool::Eraser) m_edit.drawing = false;
        } else EndEdit(true);
        Invalidate();
        return true;
    case VK_RETURN:
        if (m_edit.cropValid) ApplyCrop();
        return true;
    case 'M': SetTool(Tool::Hand); return true;
    case 'B': SetTool(Tool::Brush); return true;
    case 'G': SetTool(Tool::Eraser); return true;
    case 'L': SetTool(Tool::Line); return true;
    case 'A': SetTool(Tool::Arrow); return true;
    case 'R': SetTool(Tool::Rect); return true;
    case 'O': SetTool(Tool::Ellipse); return true;
    case 'T': SetTool(Tool::Text); return true;
    case 'F': SetTool(Tool::Fill); return true;
    case 'K': SetTool(Tool::Picker); return true;
    case 'C': SetTool(Tool::Crop); return true;
    case 'E': EndEdit(true); return true;
    case VK_OEM_4: case VK_OEM_6: {  // [ et ] : taille
        float& v = SliderValue();
        v = vk == VK_OEM_6 ? std::min(m_edit.tool == Tool::Text ? 400.0f : 200.0f, std::ceil(v * 1.2f))
                           : std::max(1.0f, std::floor(v / 1.2f));
        Invalidate();
        return true;
    }
    // Navigation, zoom, plein écran : gérés par le mode visionneuse (avec confirmation si besoin)
    default: return false;
    }
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

void Viewer::DrawEditOverlay(float left, float top, float z) {
    const float s = m_scale;
    const auto toImage = D2D1::Matrix3x2F::Scale(z, z) * D2D1::Matrix3x2F::Translation(left, top);

    // Aperçu de la forme en cours
    if (m_edit.drawing && (m_edit.tool == Tool::Line || m_edit.tool == Tool::Arrow || m_edit.tool == Tool::Rect ||
                           m_edit.tool == Tool::Ellipse)) {
        m_dc->SetTransform(toImage);
        DrawShape(m_edit.tool, m_edit.start, m_edit.cur, m_edit.secondary ? m_edit.color2 : m_edit.color, m_edit.size, m_edit.fill);
        m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // Recadrage : extérieur assombri, grille des tiers, boutons
    if (m_edit.tool == Tool::Crop && (m_edit.drawing || m_edit.cropValid)) {
        D2D1_RECT_F ir = m_edit.drawing ? Norm(m_edit.start, m_edit.cur) : m_edit.crop;
        D2D1_RECT_F r = D2D1::RectF(left + ir.left * z, top + ir.top * z, left + ir.right * z, top + ir.bottom * z);
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
        wchar_t b[48];
        swprintf_s(b, L"%.0f × %.0f", ir.right - ir.left, ir.bottom - ir.top);
        D2D1_RECT_F lab = D2D1::RectF(r.left, r.top - 24 * s, r.left + 110 * s, r.top - 4 * s);
        FillRound(lab, 4 * s, 0x000000, 0.6f);
        Text(b, m_tfCenter.Get(), lab, kWhite, 1);
        if (m_edit.cropValid && !m_edit.drawing) {
            D2D1_RECT_F ok = D2D1::RectF(r.right - 196 * s, r.bottom + 8 * s, r.right - 64 * s, r.bottom + 40 * s);
            D2D1_RECT_F no = D2D1::RectF(r.right - 56 * s, r.bottom + 8 * s, r.right, r.bottom + 40 * s);
            FillRound(ok, 6 * s, kAccent, 0.95f);
            IconLabel(ok, L"", L"Recadrer", 0x0B1A2A, 1);
            FillRound(no, 6 * s, kPanel, 0.95f);
            Text(L"", m_tfIcon.Get(), no, kWhite, 1);
            m_editButtons.push_back({EditCmd::CropApply, 0, ok, L"Valider le recadrage (Entrée)"});
            m_editButtons.push_back({EditCmd::CropCancel, 0, no, L"Annuler (Échap)"});
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
            m_brush->SetColor(Rgb(m_edit.color));
            m_dc->DrawTextLayout(m_edit.textPos, lay.Get(), m_brush.Get());
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

    // Taille du pinceau sous le curseur
    if ((m_edit.tool == Tool::Brush || m_edit.tool == Tool::Eraser) && m_mouseInside && !HitEdit(m_mouse) && !m_edit.spacePan) {
        float r = std::max(1.5f, m_edit.size * z / 2);
        SetColor(0x000000, 0.6f);
        m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(float(m_mouse.x), float(m_mouse.y)), r + 1, r + 1), m_brush.Get(), 1);
        SetColor(kWhite, 0.9f);
        m_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(float(m_mouse.x), float(m_mouse.y)), r, r), m_brush.Get(), 1);
    }
}

void Viewer::DrawEditUi(float cw, float ch) {
    const float s = m_scale;
    const bool hoverOk = m_mouseInside && !m_edit.drawing && !m_edit.sliderDrag;
    const EditButton* hov = hoverOk ? HitEdit(m_mouse) : nullptr;
    // Les boutons du recadrage ont été ajoutés par DrawEditOverlay : on garde ceux-là
    std::vector<EditButton> overlayButtons = std::move(m_editButtons);
    m_editButtons.clear();

    auto button = [&](EditCmd cmd, int arg, const D2D1_RECT_F& r, const wchar_t* tip, bool active = false, bool enabled = true) {
        bool isHov = hov && hov->cmd == cmd && hov->arg == arg && enabled;
        if (active) FillRound(r, 6 * s, kAccent, 0.25f);
        if (isHov) FillRound(r, 6 * s, kWhite, 0.12f);
        if (enabled) m_editButtons.push_back({cmd, arg, r, tip});
        return isHov;
    };

    // --- Barre du haut ---
    const float barH = 52 * s;
    SetColor(kPanel, 0.96f);
    m_rt->FillRectangle(D2D1::RectF(0, 0, cw, barH), m_brush.Get());
    SetColor(kWhite, 0.06f);
    m_rt->FillRectangle(D2D1::RectF(0, barH - 1, cw, barH), m_brush.Get());

    const float bs = 36 * s, by = 8 * s, gap = 2 * s;
    float x = cw - 10 * s;
    // Terminer
    D2D1_RECT_F done = D2D1::RectF(x - 110 * s, by, x, by + bs);
    bool hDone = button(EditCmd::Done, 0, done, L"Quitter le mode édition (E / Échap)");
    FillRound(done, 6 * s, kAccent, hDone ? 1.0f : 0.85f);
    IconLabel(done, L"", L"Terminer", 0x0B1A2A, 1);
    x = done.left - 8 * s;
    // Enregistrer sous
    D2D1_RECT_F save = D2D1::RectF(x - 150 * s, by, x, by + bs);
    button(EditCmd::SaveAs, 0, save, L"Enregistrer sous… (Ctrl+S)");
    SetColor(kWhite, 0.25f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(save, 6 * s, 6 * s), m_brush.Get(), 1);
    IconLabel(save, L"", m_edit.dirty ? L"Enregistrer sous •" : L"Enregistrer sous", kWhite, 0.95f);
    x = save.left - 14 * s;

    struct Ico { EditCmd cmd; const wchar_t* glyph; const wchar_t* tip; };
    const Ico transforms[] = {{EditCmd::FlipV, L"⇅", L"Miroir vertical"},
                              {EditCmd::FlipH, L"⇄", L"Miroir horizontal"},
                              {EditCmd::RotR, L"", L"Pivoter à droite"},
                              {EditCmd::RotL, L"", L"Pivoter à gauche"}};
    // Redimensionner
    D2D1_RECT_F rs = D2D1::RectF(x - bs, by, x, by + bs);
    button(EditCmd::ResizeMenu, 0, rs, L"Redimensionner", m_edit.resizeMenu);
    Text(L"", m_tfIcon.Get(), rs, kWhite, 0.92f);
    D2D1_RECT_F resizeBtn = rs;
    x = rs.left - gap;
    for (auto& t : transforms) {
        D2D1_RECT_F r = D2D1::RectF(x - bs, by, x, by + bs);
        button(t.cmd, 0, r, t.tip);
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
    // Remplir les formes
    D2D1_RECT_F fr = D2D1::RectF(x - bs, by, x, by + bs);
    button(EditCmd::FillToggle, 0, fr, m_edit.fill ? L"Formes pleines (cliquer : contour)" : L"Formes en contour (cliquer : pleines)", m_edit.fill);
    {
        D2D1_RECT_F sq = D2D1::RectF(fr.left + 10 * s, fr.top + 10 * s, fr.right - 10 * s, fr.bottom - 10 * s);
        SetColor(m_edit.color, 1);
        if (m_edit.fill) m_rt->FillRectangle(sq, m_brush.Get());
        else m_rt->DrawRectangle(sq, m_brush.Get(), 2 * s);
    }
    x = fr.left - 14 * s;
    // Taille (curseur logarithmique)
    const bool textTool = m_edit.tool == Tool::Text;
    float val = textTool ? m_edit.fontSize : m_edit.size;
    float t = textTool ? std::log(val / 8.0f) / std::log(50.0f) : std::log(val) / std::log(200.0f);
    wchar_t vb[32];
    swprintf_s(vb, L"%.0f px", val);
    D2D1_RECT_F vr = D2D1::RectF(x - 54 * s, by, x, by + bs);
    Text(vb, m_tfSmall.Get(), vr, kWhite, 0.95f);
    m_sliderRect = D2D1::RectF(vr.left - 130 * s, by + 8 * s, vr.left - 8 * s, by + bs - 8 * s);
    m_editButtons.push_back({EditCmd::Slider, 0, m_sliderRect, textTool ? L"Taille du texte ( [ et ] )" : L"Épaisseur ( [ et ] )"});
    float sy = (m_sliderRect.top + m_sliderRect.bottom) / 2;
    SetColor(kWhite, 0.2f);
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(m_sliderRect.left, sy - 2 * s, m_sliderRect.right, sy + 2 * s), 2 * s, 2 * s), m_brush.Get());
    float kx = m_sliderRect.left + std::clamp(t, 0.0f, 1.0f) * (m_sliderRect.right - m_sliderRect.left);
    SetColor(kAccent, 1);
    m_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(m_sliderRect.left, sy - 2 * s, kx, sy + 2 * s), 2 * s, 2 * s), m_brush.Get());
    m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, sy), 7 * s, 7 * s), m_brush.Get());
    D2D1_RECT_F lbl = D2D1::RectF(m_sliderRect.left - 70 * s, by, m_sliderRect.left - 8 * s, by + bs);
    Text(textTool ? L"Police" : L"Taille", m_tfSmall.Get(), lbl, kGrey, 1);
    x = lbl.left - 14 * s;
    // Annuler / Rétablir
    D2D1_RECT_F redo = D2D1::RectF(x - bs, by, x, by + bs);
    button(EditCmd::Redo, 0, redo, L"Rétablir (Ctrl+Y)", false, !m_edit.redo.empty());
    Text(L"", m_tfIcon.Get(), redo, kWhite, m_edit.redo.empty() ? 0.3f : 0.92f);
    D2D1_RECT_F undo = D2D1::RectF(redo.left - gap - bs, by, redo.left - gap, by + bs);
    button(EditCmd::Undo, 0, undo, L"Annuler (Ctrl+Z)", false, !m_edit.undo.empty() || m_edit.textActive);
    Text(L"", m_tfIcon.Get(), undo, kWhite, (m_edit.undo.empty() && !m_edit.textActive) ? 0.3f : 0.92f);
    // Titre
    if (const FileEntry* cur = Current()) {
        float tr = undo.left - 16 * s;
        Text(L"Édition", m_tfTitle.Get(), D2D1::RectF(16 * s, 7 * s, tr, 28 * s), kAccent, 1);
        wchar_t dims[64];
        swprintf_s(dims, L"   ·   %u × %u", m_edit.w, m_edit.h);
        Text(cur->name + dims + (m_edit.dirty ? L"   ·   modifiée" : L""), m_tfSub.Get(), D2D1::RectF(16 * s, 28 * s, tr, 46 * s), kGrey, 1);
    }

    // --- Outils (colonne de gauche) ---
    const float tb = 40 * s, tx = 10 * s;
    float ty = barH + 12 * s;
    D2D1_RECT_F panel = D2D1::RectF(tx - 4 * s, ty - 4 * s, tx + tb + 4 * s, ty + tb * ARRAYSIZE(kTools) + 4 * s);
    FillRound(panel, 8 * s, kPanel, 0.93f);
    for (auto& td : kTools) {
        D2D1_RECT_F r = D2D1::RectF(tx, ty, tx + tb, ty + tb);
        bool active = int(m_edit.tool) == td.tool;
        button(EditCmd::Tool, td.tool, r, td.tip, active);
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
                             L"Inverser couleur principale / secondaire"});
    float sx = px + 54 * s;
    for (int i = 0; i < 16; ++i) {
        int col = i / 2, row = i % 2;
        D2D1_RECT_F r = D2D1::RectF(sx + col * (sw + sg), pyb - 48 * s + row * (sw + sg), sw + sx + col * (sw + sg),
                                    pyb - 48 * s + row * (sw + sg) + sw);
        SetColor(kPalette[i], 1);
        m_rt->FillRectangle(r, m_brush.Get());
        bool h = hov && hov->cmd == EditCmd::Color && hov->arg == i;
        SetColor(kWhite, h ? 0.9f : 0.18f);
        m_rt->DrawRectangle(r, m_brush.Get(), h ? 2 * s : 1);
        m_editButtons.push_back({EditCmd::Color, i, r, L"Clic : couleur principale — clic droit : secondaire"});
    }
    D2D1_RECT_F plus = D2D1::RectF(sx + 8 * (sw + sg) + 4 * s, pyb - 46 * s, sx + 8 * (sw + sg) + 36 * s, pyb - 14 * s);
    button(EditCmd::CustomColor, 0, plus, L"Autre couleur…");
    Text(L"", m_tfIcon.Get(), plus, kWhite, 0.9f);

    // --- Menu Redimensionner ---
    if (m_edit.resizeMenu) {
        const float iw = 200 * s, ih = 30 * s;
        D2D1_RECT_F menu = D2D1::RectF(resizeBtn.right - iw, resizeBtn.bottom + 6 * s, resizeBtn.right,
                                       resizeBtn.bottom + 6 * s + ih * ARRAYSIZE(kResize) + 8 * s);
        FillRound(menu, 8 * s, kPanel, 0.98f);
        SetColor(kWhite, 0.12f);
        m_rt->DrawRoundedRectangle(D2D1::RoundedRect(menu, 8 * s, 8 * s), m_brush.Get(), 1);
        float iy = menu.top + 4 * s;
        for (int i = 0; i < int(ARRAYSIZE(kResize)); ++i) {
            D2D1_RECT_F r = D2D1::RectF(menu.left + 4 * s, iy, menu.right - 4 * s, iy + ih);
            button(EditCmd::ResizeOpt, i, r, nullptr);
            const ResizeOption& o = kResize[i];
            double k = o.percent ? o.value / 100.0 : double(o.value) / std::max(m_edit.w, m_edit.h);
            wchar_t b[64];
            swprintf_s(b, L"%ls   (%ld × %ld)", o.label, std::lround(m_edit.w * k), std::lround(m_edit.h * k));
            Text(b, m_tfSmall.Get(), D2D1::RectF(r.left + 10 * s, r.top, r.right, r.bottom), kWhite, 0.95f);
            iy += ih;
        }
    }

    // Boutons de l'overlay (recadrage) au-dessus de tout le reste
    for (auto& b : overlayButtons) m_editButtons.push_back(b);

    // Info-bulle
    if (hov && hov->tip && !m_edit.resizeMenu) {
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
