#include "viewer.h"
#include "viewer_modes.h"
#include "viewer_util.h"
#include "i18n.h"
#include "new_document_dialog.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <shellapi.h>

using namespace ui;

void Viewer::NewDocument() {
    if (!CreateDeviceResources()) return;
    newdoc::Options options;
    options.maxDimension=std::min(16384u,m_dc->GetMaximumBitmapSize());
    if (!newdoc::Show(m_hwnd,options)) return;
    const auto w=options.width,h=options.height;
    auto canvas = NewCanvas(w, h);
    if (!canvas || !DrawOn(canvas.Get(), [&] {
        m_dc->Clear(options.background==1 ? D2D1::ColorF(1,1,1,1) :
            options.background==2 ? D2D1::ColorF(0,0,0,1) : D2D1::ColorF(0,0,0,0));
    })) { ShowToast(T("edit.err.vram")); return; }
    if (!EndEdit(true)) return;
    EnsureEditPlugins();
    m_projectPath.clear();
    m_edit = EditState{};
    m_edit.active = m_edit.untitled = m_edit.dirty = true;
    m_edit.hasAlpha = true;
    m_edit.w = w; m_edit.h = h;
    Layer layer; layer.bmp = canvas; Touch(layer);
    m_edit.layers.push_back(std::move(layer));
    InitEditModes();
    m_mode = 0;
    m_userRot = 0;
    m_showHelp = m_showInfo = m_showSettings = false;
    ApplyFit(false);
    Invalidate();
}

bool Viewer::RenderObject(Layer& l) {
    auto bitmap = NewCanvas(m_edit.w, m_edit.h);
    if (!bitmap) return false;
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    if (l.objectTool == Tool::Text) {
        if (FAILED(m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, l.objectSize, L"", &format)) ||
            FAILED(m_dwrite->CreateTextLayout(l.objectText.c_str(), UINT32(l.objectText.size()),
                format.Get(), 100000, 100000, &layout))) return false;
    }
    if (!DrawOn(bitmap.Get(), [&] {
        m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        m_dc->SetTransform(l.objectTransform);
        m_dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::Matrix3x2F::Identity(), l.objectOpacity), nullptr);
        if (layout) {
            m_brush->SetColor(Rgb(l.objectColor));
            m_dc->DrawTextLayout(l.a, layout.Get(), m_brush.Get());
        } else DrawShape(l.objectTool, l.a, l.b, l.objectColor, l.objectSize, l.objectFill);
        m_dc->PopLayer();
    })) return false;
    l.bmp = bitmap;
    Touch(l);
    return true;
}

void Viewer::AddObject(Tool tool, D2D1_POINT_2F a, D2D1_POINT_2F b, const std::wstring& text) {
    if (!CanAddLayer()) return;
    Layer l;
    l.object = true; l.objectTool = tool; l.a = a; l.b = b;
    l.objectColor = m_edit.secondary ? m_edit.color2 : m_edit.color;
    l.objectSize = tool == Tool::Text ? m_edit.fontSize : m_edit.size;
    l.objectFill = m_edit.fill; l.objectOpacity = m_edit.opacity / 100;
    l.objectText = text;
    if (m_edit.selValid) {
        l.mask = std::make_shared<std::vector<uint8_t>>(size_t(m_edit.w) * m_edit.h, uint8_t(0));
        for (int y = int(m_edit.sel.top); y < int(m_edit.sel.bottom); ++y)
            for (int x = int(m_edit.sel.left); x < int(m_edit.sel.right); ++x)
                (*l.mask)[size_t(y) * m_edit.w + x] = 255;
    }
    l.name = tool == Tool::Text ? text.substr(0, 40) : T("document.object");
    if (!RenderObject(l)) { ShowToast(T("edit.err.vram")); return; }
    PushDocUndo();
    l.num = ++m_edit.layerSeq;
    m_edit.layers.insert(m_edit.layers.begin() + ++m_edit.layer, std::move(l));
    m_edit.editMask = false;
    Invalidate();
}

bool Viewer::RequirePixels() {
    if (!CurLayer().object) return true;
    ShowToast(T("document.raster.required"), 5);
    return false;
}

void Viewer::PrepareMasks() {
    for (int i = 0; i < int(m_edit.layers.size()); ++i) {
        Layer& l = m_edit.layers[size_t(i)];
        if (!l.mask || !l.maskEnabled) continue;
        ID2D1Bitmap1* src = l.bmp.Get();
        uint64_t version = l.ver;
        if (!m_modes.empty()) if (auto* b = Mode().LayerBitmap(i)) { src = b; version = (version * 131) ^ m_edit.fxVer; }
        if (!m_edit.fxWhole && i == m_edit.layer && m_edit.fx >= 0 && m_edit.fxPreview && m_edit.fxOut) {
            src = m_edit.fxOut.Get(); version = (version * 131) ^ m_edit.fxVer;
        }
        if (l.masked && l.maskedVer == version) continue;
        PixelBuffer pixels; uint32_t stride;
        if (l.mask->size() != size_t(m_edit.w) * m_edit.h || !ReadBitmap(src, pixels, stride)) continue;
        for (uint32_t y = 0; y < m_edit.h; ++y)
            for (uint32_t x = 0; x < m_edit.w; ++x) {
                uint8_t* p = pixels.Data() + size_t(y) * stride + x * 4;
                const unsigned a = (*l.mask)[size_t(y) * m_edit.w + x];
                for (int c = 0; c < 4; ++c) p[c] = uint8_t((unsigned(p[c]) * a + 127) / 255);
            }
        auto out = NewCanvas(m_edit.w, m_edit.h);
        if (out && SUCCEEDED(out->CopyFromMemory(nullptr, pixels.Data(), stride))) {
            l.masked = out; l.maskedVer = version;
        }
    }
}

void Viewer::PaintMask(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    Layer& l = CurLayer();
    if (!l.mask) return;
    if (l.mask.use_count() > 1) l.mask = std::make_shared<std::vector<uint8_t>>(*l.mask);
    const float radius = std::max(0.5f, m_edit.size / 2);
    const float dx = b.x - a.x, dy = b.y - a.y, length2 = dx * dx + dy * dy;
    const uint32_t color = m_edit.secondary ? m_edit.color2 : m_edit.color;
    const float value = m_edit.tool == Tool::Eraser ? 0.0f :
        float(((color >> 16) & 255) * 54 + ((color >> 8) & 255) * 183 + (color & 255) * 19) / 256;
    const int x0 = std::max(0, int(std::floor(std::min(a.x, b.x) - radius)));
    const int y0 = std::max(0, int(std::floor(std::min(a.y, b.y) - radius)));
    const int x1 = std::min(int(m_edit.w), int(std::ceil(std::max(a.x, b.x) + radius)));
    const int y1 = std::min(int(m_edit.h), int(std::ceil(std::max(a.y, b.y) + radius)));
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        if (m_edit.selValid && (x < m_edit.sel.left || x >= m_edit.sel.right || y < m_edit.sel.top || y >= m_edit.sel.bottom)) continue;
        const float t = length2 > 0 ? std::clamp(((x + .5f - a.x) * dx + (y + .5f - a.y) * dy) / length2, 0.0f, 1.0f) : 0;
        const float dist = std::hypot(x + .5f - a.x - t * dx, y + .5f - a.y - t * dy);
        if (dist > radius) continue;
        const float soft = std::max(.5f, radius * (1 - m_edit.hardness / 100));
        const float opacity = std::clamp((radius - dist) / soft, 0.0f, 1.0f) * m_edit.opacity / 100;
        auto& v = (*l.mask)[size_t(y) * m_edit.w + x];
        v = uint8_t(std::lround(v + (value - v) * opacity));
    }
    Touch(l); Invalidate();
}

void Viewer::ShowLayerMenu(POINT client) {
    if (!m_edit.active || m_edit.drawing || m_edit.fx >= 0) return;
    if (m_edit.textActive) CommitText();
    if (const auto* hit = HitEdit(client); hit && (hit->cmd == EditCmd::LayerSelect || hit->cmd == EditCmd::LayerEye)) {
        if (hit->arg != m_edit.layer) m_edit.editMask = false;
        m_edit.layer = hit->arg;
    }
    HMENU menu = CreatePopupMenu(), blends = CreatePopupMenu();
    auto add = [&](UINT id, const char* key, bool enabled = true, bool checked = false) {
        AppendMenuW(menu, MF_STRING | (enabled ? 0 : MF_GRAYED) | (checked ? MF_CHECKED : 0), id, T(key));
    };
    add(1, "edit.tip.layer_new"); add(2, "edit.tip.layer_dup"); add(3, "edit.tip.layer_del", m_edit.layers.size() > 1);
    add(4, "document.rename"); add(5, "document.object.edit", CurLayer().object);
    add(6, "document.raster", CurLayer().object);
    for (int i = 0; i < 8; ++i) AppendMenuW(blends, MF_STRING | (CurLayer().blend == i ? MF_CHECKED : 0), 100 + i, T(BlendKey(i)));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(blends), T("edit.tip.blend"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(10, "document.mask.add", !CurLayer().mask);
    add(11, "document.mask.edit", bool(CurLayer().mask), m_edit.editMask);
    add(12, "document.mask.toggle", bool(CurLayer().mask), CurLayer().mask && CurLayer().maskEnabled);
    add(13, "document.mask.invert", bool(CurLayer().mask));
    add(14, "document.mask.remove", bool(CurLayer().mask));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(20, "edit.tip.layer_merge", m_edit.layer > 0); add(21, "edit.tip.layer_flatten", m_edit.layers.size() > 1);
    POINT screen = client; ClientToScreen(m_hwnd, &screen);
    const UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
    if (id >= 100 && id < 108) LayerCommand(EditCmd::BlendOpt, int(id - 100));
    switch (id) {
    case 1: LayerCommand(EditCmd::LayerNew, 0); break;
    case 2: LayerCommand(EditCmd::LayerDup, 0); break;
    case 3: LayerCommand(EditCmd::LayerDel, 0); break;
    case 4: { std::wstring name = LayerName(CurLayer()); if (InputText(T("document.rename"), name)) { PushDocUndo(); CurLayer().name = name; } break; }
    case 5: {
        Layer edited = CurLayer();
        if (edited.objectTool == Tool::Text && !InputText(T("document.object.edit"), edited.objectText)) break;
        edited.objectColor = m_edit.color;
        edited.objectSize = edited.objectTool == Tool::Text ? m_edit.fontSize : m_edit.size;
        edited.objectFill = m_edit.fill;
        if (RenderObject(edited)) { PushDocUndo(); CurLayer() = std::move(edited); }
        break;
    }
    case 6: PushDocUndo(); CurLayer().object = false; CurLayer().objectText.clear(); break;
    case 10: {
        auto mask = std::make_shared<std::vector<uint8_t>>(size_t(m_edit.w) * m_edit.h, uint8_t(m_edit.selValid ? 0 : 255));
        if (m_edit.selValid) for (int y = int(m_edit.sel.top); y < int(m_edit.sel.bottom); ++y)
            for (int x = int(m_edit.sel.left); x < int(m_edit.sel.right); ++x)
                (*mask)[size_t(y) * m_edit.w + x] = 255;
        PushDocUndo(); CurLayer().mask = mask; CurLayer().maskEnabled = true;
        m_edit.editMask = true; SetTool(Tool::Brush); m_edit.color = 0; m_edit.color2 = 0xFFFFFF; Touch(CurLayer()); break;
    }
    case 11: m_edit.editMask = !m_edit.editMask; if (m_edit.editMask) SetTool(Tool::Brush); break;
    case 12: PushDocUndo(); CurLayer().maskEnabled = !CurLayer().maskEnabled; Touch(CurLayer()); break;
    case 13: {
        auto mask = std::make_shared<std::vector<uint8_t>>(*CurLayer().mask);
        for (auto& value : *mask) value = uint8_t(255 - value);
        PushDocUndo(); CurLayer().mask = mask; Touch(CurLayer()); break;
    }
    case 14: PushDocUndo(); CurLayer().mask.reset(); CurLayer().masked.Reset(); m_edit.editMask = false; Touch(CurLayer()); break;
    case 20: LayerCommand(EditCmd::LayerMerge, 0); break;
    case 21: LayerCommand(EditCmd::LayerFlatten, 0); break;
    default: break;
    }
    Invalidate();
}

ID2D1Bitmap1* Viewer::EffectSource() const {
    return m_edit.fxSource ? m_edit.fxSource.Get() : CurLayer().bmp.Get();
}

void Viewer::ApplyEffectResult(ComPtr<ID2D1Bitmap1> result) {
    if (!result) return;
    if (!m_edit.fxWhole) { PushLayerSwap(result); return; }
    PushDocUndo();
    for (auto& layer : m_edit.layers) layer.visible = false;
    Layer layer; layer.bmp = result; layer.name = T("document.fx.result");
    layer.num = ++m_edit.layerSeq; Touch(layer);
    m_edit.layers.push_back(std::move(layer));
    m_edit.layer = int(m_edit.layers.size()) - 1;
    m_edit.editMask = false;
}

void Viewer::OpenExtensions() {
    const std::wstring dir = m_exeDir + L"\\plugins";
    CreateDirectoryW(dir.c_str(), nullptr);
    ShellExecuteW(m_hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
