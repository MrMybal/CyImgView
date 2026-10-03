// Plugins d'édition dans l'éditeur (include/cyimg_edit_plugin.h) : fonctions de l'hôte, effets des
// plugins (menu Effets, panneau de réglages, aperçu), outils (colonne d'outils, barre d'options,
// souris, surcouche), modes (onglets, adaptateur PluginEditMode), actions « Ouvrir avec », aide.
//
// Effets et modes : process() tourne sur un thread de travail (editplug::StartJob) à partir d'une
// copie des pixels du calque. Pendant les réglages, sur une copie réduite à la taille de l'écran ;
// peu après le dernier réglage, en pleine résolution : ce résultat remplace l'aperçu et sert à
// « Appliquer » (ou à l'intégration du mode) sans attente. Un calcul périmé est abandonné
// (is_cancelled) et son résultat ignoré ; un compteur de génération écarte les résultats d'un effet
// déjà fermé.
//
// Outils : les primitives de dessin de l'hôte dessinent sur le calque actif avec Direct2D (limitées à
// la sélection, une entrée d'historique par geste), ou à l'écran pendant draw_overlay.

#include "viewer.h"
#include "viewer_modes.h"
#include "viewer_util.h"
#include "edit_plugins.h"
#include "i18n.h"

#include <commdlg.h>
#include <d2d1effects.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ui;

namespace {

constexpr UINT_PTR kTimerPlug = 0x504C;  // calcul complet peu après le dernier réglage
constexpr double kFullDelay = 0.4;
DWORD g_uiThread = 0;

void CALLBACK OnPlugTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    InvalidateRect(hwnd, nullptr, FALSE);
}

D2D1_COLOR_F Argb(uint32_t c) {
    return D2D1::ColorF(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
}

uint32_t ModsNow() {
    return (GetKeyState(VK_SHIFT) < 0 ? CYIMG_MOD_SHIFT : 0u) | (GetKeyState(VK_CONTROL) < 0 ? CYIMG_MOD_CTRL : 0u) |
           (GetKeyState(VK_MENU) < 0 ? CYIMG_MOD_ALT : 0u);
}

// Pixels lus sur le GPU (partagés avec les calculs)
struct Pixels {
    std::shared_ptr<PixelBuffer> px;
    uint32_t w = 0, h = 0, stride = 0;
};

std::wstring LocalDecimal(std::wstring s) {
    const wchar_t* sep = T("viewer.decimal");
    if (sep[0] && !sep[1]) std::replace(s.begin(), s.end(), L'.', sep[0]);
    return s;
}

// Valeur d'un curseur, avec les décimales qu'impose le pas
std::wstring FormatParam(const editplug::Param& p, double v) {
    int dec = 0;
    if (p.p.step > 0 && p.p.step < 1) dec = std::min(3, int(std::ceil(-std::log10(p.p.step) - 1e-9)));
    else if (p.p.step <= 0) dec = p.p.max - p.p.min <= 10 ? 2 : 0;
    wchar_t b[64];
    swprintf_s(b, L"%.*f", dec, v);
    std::wstring s = LocalDecimal(b);
    if (!p.unit.empty()) s += (p.unit == L"°" ? L"" : L" ") + p.unit;
    return s;
}

double Quantize(const editplug::Param& p, double v) {
    if (p.p.step > 0) v = p.p.min + std::round((v - p.p.min) / p.p.step) * p.p.step;
    return std::clamp(v, p.p.min, p.p.max);
}

bool ParamVisible(const std::vector<editplug::Param>& params, const std::vector<double>& vals, size_t k) {
    const uint32_t dep = params[k].p.visible_if;
    return dep == 0 || (dep - 1 < vals.size() && vals[dep - 1] == params[k].p.visible_value);
}

bool ChooseRgb(HWND owner, double& value) {
    static COLORREF custom[16] = {};
    const uint32_t rgb = uint32_t(value);
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = owner;
    cc.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    value = double(uint32_t(GetRValue(cc.rgbResult)) << 16 | uint32_t(GetGValue(cc.rgbResult)) << 8 | GetBValue(cc.rgbResult));
    return true;
}

// Icône (HICON 32 bits) dessinée à partir d'un caractère Segoe MDL2 / Fluent, blanche sur transparent
HICON GlyphIcon(uint32_t cp, int size) {
    const std::wstring g = editplug::GlyphString(cp);
    if (g.empty()) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        DeleteDC(dc);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(dc, color);
    HFONT font = CreateFontW(-size * 7 / 8, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT rc{0, 0, size, size};
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, g.c_str(), int(g.size()), &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    auto* px = static_cast<uint8_t*>(bits);  // gris -> blanc prémultiplié de même alpha
    for (int i = 0; i < size * size; ++i, px += 4) {
        const uint8_t a = std::max(px[0], std::max(px[1], px[2]));
        px[0] = px[1] = px[2] = px[3] = a;
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

// --- Saisie de texte (input_text) : boîte de dialogue en mémoire, thème sombre ---

struct InputDlgData {
    const wchar_t* title;
    std::wstring text;
    HBRUSH bg, editBg;
};

INT_PTR CALLBACK InputDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<InputDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        d = reinterpret_cast<InputDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(dlg, 20, &dark, sizeof(dark));
        SetWindowTextW(dlg, d->title);
        SetDlgItemTextW(dlg, 101, d->text.c_str());
        for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            wchar_t cls[16] = {};
            GetClassNameW(c, cls, ARRAYSIZE(cls));
            SetWindowTheme(c, _wcsicmp(cls, L"Edit") == 0 ? L"DarkMode_CFD" : L"DarkMode_Explorer", nullptr);
        }
        SendDlgItemMessageW(dlg, 101, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, 101));
        return FALSE;
    }
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
        SetBkColor(dc, msg == WM_CTLCOLOREDIT ? RGB(0x2B, 0x2C, 0x31) : RGB(0x1C, 0x1D, 0x21));
        if (!d) return reinterpret_cast<INT_PTR>(GetStockObject(BLACK_BRUSH));
        return reinterpret_cast<INT_PTR>(msg == WM_CTLCOLOREDIT ? d->editBg : d->bg);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            std::wstring s(size_t(GetWindowTextLengthW(GetDlgItem(dlg, 101))) + 1, L'\0');
            s.resize(size_t(GetDlgItemTextW(dlg, 101, s.data(), int(s.size()))));
            d->text = s;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    default: break;
    }
    return FALSE;
}

bool InputTextDialog(HWND owner, const wchar_t* title, std::wstring& text) {
    std::vector<WORD> t;
    auto word = [&](WORD w) { t.push_back(w); };
    auto dword = [&](DWORD v) { word(LOWORD(v)); word(HIWORD(v)); };
    auto str = [&](const wchar_t* s) { while (*s) word(WORD(*s++)); word(0); };
    auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* txt) {
        if (t.size() % 2) word(0);
        dword(style | WS_CHILD | WS_VISIBLE);
        dword(0);
        word(WORD(x)); word(WORD(y)); word(WORD(cx)); word(WORD(cy));
        word(id);
        word(0xFFFF);
        word(cls);
        str(txt);
        word(0);
    };
    const short W = 280, H = 112;
    dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    dword(0);
    word(3);
    word(0); word(0); word(WORD(W)); word(WORD(H));
    word(0);
    word(0);
    str(L"");
    word(9);
    str(L"Segoe UI");
    item(ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 8, 8, W - 16, H - 40, 101, 0x0081, L"");
    item(BS_DEFPUSHBUTTON | WS_TABSTOP, W - 8 - 2 * 60 - 6, H - 8 - 15, 60, 15, IDOK, 0x0080, T("openwith.dlg.ok"));
    item(BS_PUSHBUTTON | WS_TABSTOP, W - 8 - 60, H - 8 - 15, 60, 15, IDCANCEL, 0x0080, T("openwith.dlg.cancel"));
    InputDlgData data{title ? title : L"CyImgView", text, CreateSolidBrush(RGB(0x1C, 0x1D, 0x21)), CreateSolidBrush(RGB(0x2B, 0x2C, 0x31))};
    const INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<const DLGTEMPLATE*>(t.data()), owner,
                                              InputDlgProc, reinterpret_cast<LPARAM>(&data));
    DeleteObject(data.bg);
    DeleteObject(data.editBg);
    if (r != IDOK) return false;
    text = data.text;
    return true;
}

}  // namespace

// =============================================================================================
// État

struct Viewer::PlugState {
    // Outils : geste en cours
    bool begun = false;            // entrée d'historique déjà créée pour ce geste
    bool groupOpen = false;
    double groupOpacity = 1;
    uint32_t groupBlend = 0;
    int overlayLayers = 0;         // couches Direct2D ouvertes par begin_group pendant draw_overlay
    ComPtr<ID2D1Bitmap1> groupBuf;
    bool locked = false;
    PixelBuffer lockBuf;
    D2D1_RECT_U lockRect{};
    uint32_t lockStride = 0;
    ComPtr<ID2D1StrokeStyle> roundJoin;
    std::vector<std::wstring> toolTips;
    std::vector<ComPtr<ID2D1Bitmap>> toolPng;
    ComPtr<ID2D1RenderTarget> pngRt;
    std::vector<HICON> actionIcons;
    int provider = 0;

    // Effet de plugin en cours de réglage
    uint64_t gen = 0;
    int fx = -1;
    std::vector<double> vals;
    Pixels smallPx, fullPx;
    editplug::JobPtr preview, render;
    bool previewPending = false;
    PixelBuffer fullRes;
    std::vector<double> fullResVals;
    bool fullValid = false, shownFull = false;
    double changeTime = 0;
    bool applying = false;
    bool failedShown = false;
    ComPtr<ID2D1Bitmap1> smallBmp;
};

// =============================================================================================
// Fonctions de l'hôte

struct Viewer::PlugHost {
    static const CyImgEditHost table;

    static Viewer* Ui(CyImgEditCtx* c) {
        return c && !c->job && c->viewer && GetCurrentThreadId() == g_uiThread ? c->viewer : nullptr;
    }
    static Viewer* Doc(CyImgEditCtx* c) {  // document ouvert
        Viewer* v = Ui(c);
        return v && v->m_edit.active && !v->m_edit.layers.empty() && v->m_dc ? v : nullptr;
    }

    static const wchar_t* Translate(CyImgEditCtx*, const char* key, const wchar_t* fallback) {
        if (!fallback) fallback = L"";
        return key && *key ? TrOr(key, fallback) : fallback;
    }
    static int IsCancelled(CyImgEditCtx* c) { return c && c->job && c->job->cancel.load() ? 1 : 0; }

    static void DocSize(CyImgEditCtx* c, uint32_t* w, uint32_t* h) {
        Viewer* v = Doc(c);
        if (w) *w = v ? v->m_edit.w : 0;
        if (h) *h = v ? v->m_edit.h : 0;
    }
    static uint32_t Primary(CyImgEditCtx* c) { Viewer* v = Ui(c); return v ? v->m_edit.color : 0; }
    static uint32_t Secondary(CyImgEditCtx* c) { Viewer* v = Ui(c); return v ? v->m_edit.color2 : 0xFFFFFF; }
    static int Selection(CyImgEditCtx* c, CyImgRect* r) {
        Viewer* v = Doc(c);
        if (!v || !v->m_edit.selValid) return 0;
        if (r) *r = SelRect(*v);
        return 1;
    }
    static double Param(CyImgEditCtx* c, uint32_t i) {
        Viewer* v = Ui(c);
        auto& tools = editplug::Get().tools;
        if (!v || c->tool < 0 || c->tool >= int(tools.size()) || i >= tools[size_t(c->tool)].values.size()) return 0;
        return tools[size_t(c->tool)].values[i];
    }
    static void SetParam(CyImgEditCtx* c, uint32_t i, double value) {
        Viewer* v = Ui(c);
        auto& tools = editplug::Get().tools;
        if (!v || c->tool < 0 || c->tool >= int(tools.size()) || i >= tools[size_t(c->tool)].values.size()) return;
        editplug::Tool& t = tools[size_t(c->tool)];
        t.values[i] = Quantize(t.params[i], value);
        v->Invalidate();
    }
    static double ScreenPixel(CyImgEditCtx* c) { Viewer* v = Ui(c); return v && v->m_zoom > 0 ? 1.0 / v->m_zoom : 1.0; }
    static void Redraw(CyImgEditCtx* c) { if (Viewer* v = Ui(c)) v->Invalidate(); }
    static void Toast(CyImgEditCtx* c, const wchar_t* text) { if (Viewer* v = Ui(c); v && text) v->ShowToast(text, 2.5); }
    static void* Window(CyImgEditCtx* c) { Viewer* v = Ui(c); return v ? v->m_hwnd : nullptr; }
    static int InputText(CyImgEditCtx* c, const wchar_t* title, const wchar_t* initial, wchar_t* out, uint32_t len) {
        Viewer* v = Ui(c);
        if (!v || c->overlay || !out || !len) return 0;
        std::wstring text = initial ? initial : L"";
        if (!InputTextDialog(v->m_hwnd, title, text)) return 0;
        wcsncpy_s(out, len, text.c_str(), _TRUNCATE);
        return 1;
    }

    // --- Modification du calque actif ---

    static CyImgRect SelRect(const Viewer& v) {
        if (!v.m_edit.selValid) return CyImgRect{0, 0, v.m_edit.w, v.m_edit.h};
        const auto& s = v.m_edit.sel;
        return CyImgRect{int32_t(s.left), int32_t(s.top), uint32_t(s.right - s.left), uint32_t(s.bottom - s.top)};
    }
    static bool EnsureBegun(Viewer& v) {
        if (v.m_plug->begun) return true;
        if (!v.PushUndo()) return false;
        v.m_plug->begun = true;
        return true;
    }
    static int BeginEdit(CyImgEditCtx* c) {
        Viewer* v = Doc(c);
        if (!v || c->overlay || !v->PushUndo()) return 0;
        v->m_plug->begun = true;
        return 1;
    }
    static uint8_t* Lock(CyImgEditCtx* c, CyImgRect* r, uint32_t* stride) {
        Viewer* v = Doc(c);
        if (!v || c->overlay || !r || v->m_plug->locked) return nullptr;
        const int64_t l = std::clamp<int64_t>(r->x, 0, v->m_edit.w), t = std::clamp<int64_t>(r->y, 0, v->m_edit.h);
        const int64_t rr = std::clamp<int64_t>(int64_t(r->x) + r->w, 0, v->m_edit.w), b = std::clamp<int64_t>(int64_t(r->y) + r->h, 0, v->m_edit.h);
        if (rr <= l || b <= t) return nullptr;
        PlugState& ps = *v->m_plug;
        ps.lockRect = D2D1::RectU(UINT32(l), UINT32(t), UINT32(rr), UINT32(b));
        if (!v->ReadBitmap(v->CurLayer().bmp.Get(), ps.lockBuf, ps.lockStride, &ps.lockRect)) return nullptr;
        *r = CyImgRect{int32_t(l), int32_t(t), uint32_t(rr - l), uint32_t(b - t)};
        if (stride) *stride = ps.lockStride;
        ps.locked = true;
        return ps.lockBuf.Data();
    }
    static void Unlock(CyImgEditCtx* c, int modified) {
        Viewer* v = Ui(c);
        if (!v || !v->m_plug->locked) return;
        PlugState& ps = *v->m_plug;
        ps.locked = false;
        if (modified && v->m_edit.active && !v->m_edit.layers.empty() && EnsureBegun(*v)) {
            D2D1_RECT_U r = ps.lockRect;  // limité à la sélection
            if (v->m_edit.selValid) {
                const auto& s = v->m_edit.sel;
                r = D2D1::RectU(std::max(r.left, UINT32(s.left)), std::max(r.top, UINT32(s.top)),
                                std::min(r.right, UINT32(s.right)), std::min(r.bottom, UINT32(s.bottom)));
            }
            if (r.right > r.left && r.bottom > r.top) {
                const uint8_t* src = ps.lockBuf.Data() + size_t(r.top - ps.lockRect.top) * ps.lockStride + size_t(r.left - ps.lockRect.left) * 4;
                v->CurLayer().bmp->CopyFromMemory(&r, src, ps.lockStride);
                v->Touch(v->CurLayer());
                v->m_edit.dirty = true;
            }
            v->Invalidate();
        }
        ps.lockBuf.Free();
    }
    static int NewLayer(CyImgEditCtx* c, const wchar_t* name) {
        Viewer* v = Doc(c);
        if (!v || c->overlay || !v->CanAddLayer()) return 0;
        ComPtr<ID2D1Bitmap1> b = v->NewCanvas(v->m_edit.w, v->m_edit.h);
        if (!b || !v->DrawOn(b.Get(), [&] { v->m_dc->Clear(D2D1::ColorF(0, 0, 0, 0)); })) return 0;
        v->AddLayer(b, name ? name : L"");
        v->m_plug->begun = false;  // l'historique partage maintenant les textures : copie au prochain dessin
        return 1;
    }

    // --- Primitives de dessin ---

    template <class F>
    static void Draw(CyImgEditCtx* c, uint32_t argb, F&& f) {
        Viewer* v = Doc(c);
        if (!v) return;
        ID2D1DeviceContext* dc = v->m_dc.Get();
        ID2D1SolidColorBrush* br = v->m_brush.Get();
        br->SetColor(Argb(argb));
        if (c->overlay) {  // aperçu : la cible est l'écran, transformation image -> écran déjà posée
            f(dc, br);
            return;
        }
        PlugState& ps = *v->m_plug;
        if (ps.groupOpen) {
            if (ps.groupBuf) v->DrawOn(ps.groupBuf.Get(), [&] { f(dc, br); });
            return;
        }
        if (!EnsureBegun(*v)) return;
        v->DrawOnLayer([&] { f(dc, br); });
        v->Invalidate();
    }
    static ID2D1StrokeStyle* RoundJoin(Viewer& v) {
        PlugState& ps = *v.m_plug;
        if (!ps.roundJoin)
            v.m_factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                       D2D1_LINE_JOIN_ROUND),
                                           nullptr, 0, &ps.roundJoin);
        return ps.roundJoin.Get();
    }
    static ComPtr<ID2D1PathGeometry> Path(Viewer& v, const double* xy, uint32_t n, bool closed) {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (!xy || n < 2 || n > 1000000 || FAILED(v.m_factory->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return nullptr;
        sink->BeginFigure(D2D1::Point2F(float(xy[0]), float(xy[1])), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (uint32_t i = 1; i < n; ++i) sink->AddLine(D2D1::Point2F(float(xy[2 * i]), float(xy[2 * i + 1])));
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        if (FAILED(sink->Close())) return nullptr;
        return geo;
    }
    static void Line(CyImgEditCtx* c, double x1, double y1, double x2, double y2, double width, uint32_t argb) {
        Viewer* v = Doc(c);
        if (!v) return;
        ID2D1StrokeStyle* st = RoundJoin(*v);
        Draw(c, argb, [&](ID2D1DeviceContext* dc, ID2D1Brush* br) {
            dc->DrawLine(D2D1::Point2F(float(x1), float(y1)), D2D1::Point2F(float(x2), float(y2)), br, float(std::max(width, 0.1)), st);
        });
    }
    static void Polyline(CyImgEditCtx* c, const double* xy, uint32_t n, double width, uint32_t argb) {
        Viewer* v = Doc(c);
        if (!v) return;
        if (n == 1) return Line(c, xy[0], xy[1], xy[0], xy[1], width, argb);
        ComPtr<ID2D1PathGeometry> geo = Path(*v, xy, n, false);
        ID2D1StrokeStyle* st = RoundJoin(*v);
        if (geo) Draw(c, argb, [&](ID2D1DeviceContext* dc, ID2D1Brush* br) { dc->DrawGeometry(geo.Get(), br, float(std::max(width, 0.1)), st); });
    }
    static void Rect(CyImgEditCtx* c, double x, double y, double w, double h, double radius, double width, uint32_t argb) {
        const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(float(x), float(y), float(x + w), float(y + h)), float(radius), float(radius));
        Draw(c, argb, [&](ID2D1DeviceContext* dc, ID2D1Brush* br) {
            if (width > 0) {
                if (radius > 0) dc->DrawRoundedRectangle(rr, br, float(width));
                else dc->DrawRectangle(rr.rect, br, float(width));
            } else {
                if (radius > 0) dc->FillRoundedRectangle(rr, br);
                else dc->FillRectangle(rr.rect, br);
            }
        });
    }
    static void Ellipse(CyImgEditCtx* c, double cx, double cy, double rx, double ry, double width, uint32_t argb) {
        const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(float(cx), float(cy)), float(rx), float(ry));
        Draw(c, argb, [&](ID2D1DeviceContext* dc, ID2D1Brush* br) {
            if (width > 0) dc->DrawEllipse(e, br, float(width));
            else dc->FillEllipse(e, br);
        });
    }
    static void Polygon(CyImgEditCtx* c, const double* xy, uint32_t n, uint32_t argb) {
        Viewer* v = Doc(c);
        if (!v || n < 3) return;
        ComPtr<ID2D1PathGeometry> geo = Path(*v, xy, n, true);
        if (geo) Draw(c, argb, [&](ID2D1DeviceContext* dc, ID2D1Brush* br) { dc->FillGeometry(geo.Get(), br); });
    }
    static ComPtr<IDWriteTextLayout> Layout(Viewer& v, const wchar_t* text, double size, double boxW, uint32_t flags) {
        ComPtr<IDWriteTextFormat> tf;
        ComPtr<IDWriteTextLayout> lay;
        if (!text || size <= 0 ||
            FAILED(v.m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, (flags & CYIMG_TEXT_BOLD) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, float(std::min(size, 2000.0)), L"", &tf)))
            return nullptr;
        if (boxW > 0) {
            tf->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            if (flags & CYIMG_TEXT_CENTER) tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (FAILED(v.m_dwrite->CreateTextLayout(text, UINT32(wcslen(text)), tf.Get(), boxW > 0 ? float(boxW) : 100000.0f, 100000.0f, &lay)))
            return nullptr;
        return lay;
    }
    static void DrawStr(CyImgEditCtx* c, double x, double y, double boxW, const wchar_t* text, double size, uint32_t argb, uint32_t flags) {
        Viewer* v = Doc(c);
        if (!v) return;
        ComPtr<IDWriteTextLayout> lay = Layout(*v, text, size, boxW, flags);
        if (!lay) return;
        DWRITE_TEXT_METRICS tm{};
        lay->GetMetrics(&tm);
        if ((flags & CYIMG_TEXT_CENTER) && boxW <= 0) x -= tm.width / 2;
        if (flags & CYIMG_TEXT_VCENTER) y -= tm.height / 2;
        const D2D1_POINT_2F at = D2D1::Point2F(float(x), float(y));
        Draw(c, argb, [&](ID2D1DeviceContext* dc, ID2D1Brush* br) { dc->DrawTextLayout(at, lay.Get(), br); });
    }
    static void MeasureStr(CyImgEditCtx* c, const wchar_t* text, double size, double boxW, uint32_t flags, double* w, double* h) {
        Viewer* v = Ui(c);
        DWRITE_TEXT_METRICS tm{};
        if (v) {
            if (ComPtr<IDWriteTextLayout> lay = Layout(*v, text, size, boxW, flags)) lay->GetMetrics(&tm);
        }
        if (w) *w = tm.width;
        if (h) *h = tm.height;
    }
    static void BeginGroup(CyImgEditCtx* c, double opacity, uint32_t blend) {
        Viewer* v = Doc(c);
        if (!v) return;
        PlugState& ps = *v->m_plug;
        opacity = std::clamp(opacity, 0.0, 1.0);
        if (c->overlay) {
            v->m_dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), float(opacity)),
                               nullptr);
            ++ps.overlayLayers;
            return;
        }
        if (ps.groupOpen) return;  // pas de groupes imbriqués
        if (!ps.groupBuf || ps.groupBuf->GetPixelSize().width != v->m_edit.w || ps.groupBuf->GetPixelSize().height != v->m_edit.h)
            ps.groupBuf = v->NewCanvas(v->m_edit.w, v->m_edit.h);
        if (!ps.groupBuf || !v->DrawOn(ps.groupBuf.Get(), [&] { v->m_dc->Clear(D2D1::ColorF(0, 0, 0, 0)); })) {
            ps.groupBuf.Reset();
            return;
        }
        ps.groupOpen = true;
        ps.groupOpacity = opacity;
        ps.groupBlend = blend;
    }
    static void EndGroup(CyImgEditCtx* c) {
        Viewer* v = Doc(c);
        if (!v) return;
        PlugState& ps = *v->m_plug;
        if (c->overlay) {
            if (ps.overlayLayers > 0) {
                v->m_dc->PopLayer();
                --ps.overlayLayers;
            }
            return;
        }
        if (!ps.groupOpen) return;
        ps.groupOpen = false;
        if (!ps.groupBuf || !EnsureBegun(*v)) return;
        ID2D1DeviceContext* dc = v->m_dc.Get();
        ID2D1Bitmap1* buf = ps.groupBuf.Get();
        const float op = float(ps.groupOpacity);
        const D2D1_RECT_F doc = D2D1::RectF(0, 0, float(v->m_edit.w), float(v->m_edit.h));
        if (ps.groupBlend == CYIMG_BLEND_NORMAL) {
            v->DrawOnLayer([&] { dc->DrawBitmap(buf, doc, op, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr); });
        } else {  // produit / écran : effet Blend sur une copie du calque (un effet ne lit pas sa propre cible)
            ComPtr<ID2D1Bitmap1> back = v->NewCanvas(v->m_edit.w, v->m_edit.h);
            ComPtr<ID2D1Effect> fade, blend;
            if (!back || FAILED(back->CopyFromBitmap(nullptr, v->CurLayer().bmp.Get(), nullptr)) ||
                FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, &fade)) || FAILED(dc->CreateEffect(CLSID_D2D1Blend, &blend))) {
                v->ShowToast(T("edit.err.vram"));
                return;
            }
            fade->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, D2D1::Matrix5x4F(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, op, 0, 0, 0, 0));
            fade->SetInput(0, buf);
            blend->SetValue(D2D1_BLEND_PROP_MODE, ps.groupBlend == CYIMG_BLEND_SCREEN ? D2D1_BLEND_MODE_SCREEN : D2D1_BLEND_MODE_MULTIPLY);
            blend->SetInput(0, back.Get());
            blend->SetInputEffect(1, fade.Get());
            v->DrawOnLayer([&] { dc->DrawImage(blend.Get(), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY); });
        }
        v->Invalidate();
    }

    // Après un rappel d'outil : ce que le plugin a laissé ouvert est refermé
    static void Cleanup(Viewer& v, bool apply) {
        PlugState& ps = *v.m_plug;
        if (ps.groupOpen) {
            if (apply) {
                CyImgEditCtx ctx;
                ctx.viewer = &v;
                EndGroup(&ctx);
            }
            ps.groupOpen = false;
        }
        if (ps.locked) {
            ps.locked = false;
            ps.lockBuf.Free();
        }
    }

    // --- Effets des plugins ---

    static uint32_t PreviewSide(const Viewer& v) {
        RECT rc;
        GetClientRect(v.m_hwnd, &rc);
        return uint32_t(std::max<LONG>(256, std::max(rc.right, rc.bottom)));
    }
    // Pixels d'un calque, réduits si son grand côté dépasse longSide
    static bool ReadScaled(Viewer& v, ID2D1Bitmap1* src, uint32_t longSide, Pixels& out) {
        const D2D1_SIZE_U s = src->GetPixelSize();
        out = Pixels();
        auto px = std::make_shared<PixelBuffer>();
        ID2D1Bitmap1* from = src;
        ComPtr<ID2D1Bitmap1> canvas;
        uint32_t w = s.width, h = s.height;
        if (std::max(w, h) > longSide) {
            const double k = double(longSide) / std::max(w, h);
            w = std::max(1u, uint32_t(std::lround(w * k)));
            h = std::max(1u, uint32_t(std::lround(h * k)));
            canvas = v.NewCanvas(w, h);
            if (!canvas || !v.DrawOn(canvas.Get(), [&] {
                    v.m_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                    v.m_dc->DrawBitmap(src, D2D1::RectF(0, 0, float(w), float(h)), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                                       nullptr, nullptr);
                }))
                return false;
            from = canvas.Get();
        }
        uint32_t stride = 0;
        if (!v.ReadBitmap(from, *px, stride)) return false;
        out.px = std::move(px);
        out.w = w;
        out.h = h;
        out.stride = stride;
        return true;
    }
    static editplug::JobPtr NewJob(const Pixels& in, const CyImgRect& sel, uint32_t fullW, uint32_t fullH, const std::vector<double>& vals,
                                   int (*process)(const CyImgEditHost*, CyImgEditCtx*, CyImgImage*, const double*)) {
        auto j = std::make_shared<editplug::Job>();
        j->src = in.px;
        j->width = in.w;
        j->height = in.h;
        j->stride = in.stride;
        j->fullWidth = fullW;
        j->fullHeight = fullH;
        j->scale = fullW ? double(in.w) / fullW : 1.0;
        // sélection ramenée à l'échelle des pixels transmis (arrondie vers l'extérieur)
        const double k = j->scale;
        const int32_t x0 = int32_t(std::floor(sel.x * k)), y0 = int32_t(std::floor(sel.y * k));
        const int32_t x1 = std::min(int32_t(in.w), int32_t(std::ceil((sel.x + double(sel.w)) * k)));
        const int32_t y1 = std::min(int32_t(in.h), int32_t(std::ceil((sel.y + double(sel.h)) * k)));
        j->sel = CyImgRect{std::max(0, x0), std::max(0, y0), uint32_t(std::max(1, x1 - std::max(0, x0))), uint32_t(std::max(1, y1 - std::max(0, y0)))};
        j->values = vals;
        j->process = process;
        return j;
    }

    static editplug::Effect* CurFx(Viewer& v) {
        auto& fx = editplug::Get().effects;
        const int i = v.m_plug ? v.m_plug->fx : -1;
        return i >= 0 && i < int(fx.size()) ? &fx[size_t(i)] : nullptr;
    }
    static void StartFxPreview(Viewer& v) {
        PlugState& ps = *v.m_plug;
        editplug::Effect* e = CurFx(v);
        if (!e || !ps.smallPx.px) return;
        if (ps.preview) {  // un calcul tourne : abandonné, le suivant partira à sa fin
            ps.preview->cancel = true;
            ps.previewPending = true;
            return;
        }
        ps.preview = NewJob(ps.smallPx, SelRect(v), v.m_edit.w, v.m_edit.h, ps.vals, e->api.process);
        const uint64_t gen = ps.gen;
        Viewer* pv = &v;
        ps.preview->done = [pv, gen](editplug::Job& j) { OnFxJob(*pv, j, gen, false); };
        editplug::StartJob(ps.preview, &table, v.m_hwnd, kMsgPlugJob);
    }
    static void StartFxRender(Viewer& v) {
        PlugState& ps = *v.m_plug;
        editplug::Effect* e = CurFx(v);
        if (!e) return;
        if (!ps.fullPx.px && !ReadScaled(v, v.CurLayer().bmp.Get(), UINT32_MAX, ps.fullPx)) {
            v.ShowToast(T("edit.err.memory"));
            return;
        }
        if (ps.render) ps.render->cancel = true;
        ps.render = NewJob(ps.fullPx, SelRect(v), v.m_edit.w, v.m_edit.h, ps.vals, e->api.process);
        const uint64_t gen = ps.gen;
        Viewer* pv = &v;
        ps.render->done = [pv, gen](editplug::Job& j) { OnFxJob(*pv, j, gen, true); };
        editplug::StartJob(ps.render, &table, v.m_hwnd, kMsgPlugJob);
    }
    static void OnFxJob(Viewer& v, editplug::Job& j, uint64_t gen, bool full) {
        PlugState& ps = *v.m_plug;
        if (gen != ps.gen) return;  // effet fermé entre-temps
        editplug::Effect* e = CurFx(v);
        if (!e) return;
        if (full) {
            if (ps.render.get() == &j) ps.render.reset();
        } else if (ps.preview.get() == &j) {
            ps.preview.reset();
        }
        if (j.crashed) return FxCrashed(v, *e, j.crash);
        static const bool tracing = GetEnvironmentVariableW(L"CYIMG_TRACE", nullptr, 0) > 0;
        if (tracing) {
            wchar_t tr[160];
            swprintf_s(tr, L"effet de plugin %ls : %ls %u × %u en %.1f ms (%d)", e->name.c_str(), full ? L"rendu" : L"aperçu", j.width,
                       j.height, j.ms, j.result);
            Trace(tr);
        }
        const bool current = j.values == ps.vals;
        if (j.result == CYIMG_OK && current) {
            if (full) {
                ps.fullRes.Swap(j.dst);
                ps.fullResVals = j.values;
                ps.fullValid = true;
                ShowFxFull(v);
                if (ps.applying) return FinishApply(v);
            } else if (!ps.shownFull) {
                ShowFxSmall(v, j);
            }
        } else if (j.result != CYIMG_OK && j.result != CYIMG_E_CANCELLED) {
            if (!ps.failedShown) v.ShowToast(Tf("editapi.err.failed", e->name.c_str()), 3);
            ps.failedShown = true;
            if (full && ps.applying) ps.applying = false;
        }
        if (!full && ps.previewPending) {
            ps.previewPending = false;
            StartFxPreview(v);
        }
        if (full && ps.applying && !ps.render) StartFxRender(v);  // rendu abandonné pendant « Appliquer »
        v.Invalidate();
    }
    static bool EnsureFxOut(Viewer& v) {
        auto& o = v.m_edit.fxOut;
        if (!o || o->GetPixelSize().width != v.m_edit.w || o->GetPixelSize().height != v.m_edit.h) o = v.NewCanvas(v.m_edit.w, v.m_edit.h);
        return o != nullptr;
    }
    static void ShowFxSmall(Viewer& v, editplug::Job& j) {
        PlugState& ps = *v.m_plug;
        if (!ps.smallBmp || ps.smallBmp->GetPixelSize().width != j.width || ps.smallBmp->GetPixelSize().height != j.height)
            ps.smallBmp = v.NewCanvas(j.width, j.height);
        if (!ps.smallBmp || FAILED(ps.smallBmp->CopyFromMemory(nullptr, j.dst.Data(), j.stride)) || !EnsureFxOut(v)) return;
        ID2D1Bitmap1* layer = v.CurLayer().bmp.Get();
        ID2D1Bitmap1* reduced = ps.smallBmp.Get();
        const bool sel = v.m_edit.selValid;
        const D2D1_RECT_F selRc = v.m_edit.sel;
        const float kx = float(v.m_edit.w) / j.width, ky = float(v.m_edit.h) / j.height;
        v.DrawOn(v.m_edit.fxOut.Get(), [&] {
            v.m_dc->DrawImage(layer, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            if (sel) v.m_dc->PushAxisAlignedClip(selRc, D2D1_ANTIALIAS_MODE_ALIASED);
            v.m_dc->SetTransform(D2D1::Matrix3x2F::Scale(kx, ky));
            v.m_dc->DrawImage(reduced, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            v.m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
            if (sel) v.m_dc->PopAxisAlignedClip();
        });
        ps.shownFull = false;
        v.m_edit.fxVer = ++v.m_edit.verSeq;
        v.Invalidate();
    }
    static void ShowFxFull(Viewer& v) {
        PlugState& ps = *v.m_plug;
        if (!EnsureFxOut(v) || FAILED(v.m_edit.fxOut->CopyFromBitmap(nullptr, v.CurLayer().bmp.Get(), nullptr))) return;
        const CyImgRect s = SelRect(v);
        const D2D1_RECT_U r = D2D1::RectU(UINT32(s.x), UINT32(s.y), UINT32(s.x) + s.w, UINT32(s.y) + s.h);
        v.m_edit.fxOut->CopyFromMemory(&r, ps.fullRes.Data() + size_t(s.y) * ps.fullPx.stride + size_t(s.x) * 4, ps.fullPx.stride);
        ps.shownFull = true;
        v.m_edit.fxVer = ++v.m_edit.verSeq;
        v.Invalidate();
    }
    static void FinishApply(Viewer& v) {
        PlugState& ps = *v.m_plug;
        ps.applying = false;
        if (v.m_edit.fxOut && ps.shownFull) v.PushLayerSwap(std::move(v.m_edit.fxOut));
        v.ClosePluginEffect(false);
    }
    static void FxCrashed(Viewer& v, editplug::Effect& e, const editplug::Crash& c) {
        e.disabled = true;
        v.ClosePluginEffect(false);
        v.PluginCrashed(e.name, e.plugin->file, c.Where());
    }

    // --- Outils ---

    static editplug::Tool* CurTool(Viewer& v) {
        auto& tools = editplug::Get().tools;
        const int i = v.m_edit.ptool;
        return v.m_edit.tool == Tool::Plugin && i >= 0 && i < int(tools.size()) && !tools[size_t(i)].disabled ? &tools[size_t(i)] : nullptr;
    }
    static CyImgEditCtx ToolCtx(Viewer& v) {
        CyImgEditCtx c;
        c.viewer = &v;
        c.tool = v.m_edit.ptool;
        return c;
    }
    static CyImgToolEvent Event(D2D1_POINT_2F ip, uint32_t buttons, uint32_t clicks) {
        CyImgToolEvent ev{};
        ev.struct_size = sizeof(ev);
        ev.x = ip.x;
        ev.y = ip.y;
        ev.buttons = buttons;
        ev.mods = ModsNow();
        ev.clicks = clicks;
        return ev;
    }
    static void ToolCrashed(Viewer& v, int index, const editplug::Crash& c) {
        auto& tools = editplug::Get().tools;
        if (index < 0 || index >= int(tools.size())) return;
        editplug::Tool& t = tools[size_t(index)];
        t.disabled = true;
        Cleanup(v, false);
        if (v.m_edit.drawing && v.m_edit.tool == Tool::Plugin) {
            v.m_edit.drawing = false;
            if (GetCapture() == v.m_hwnd) ReleaseCapture();
        }
        if (v.m_edit.tool == Tool::Plugin && v.m_edit.ptool == index) v.m_edit.tool = Tool::Brush;
        v.PluginCrashed(t.name, t.plugin->file, c.Where());
    }
    static ID2D1Bitmap* ToolPng(Viewer& v, int i) {
        PlugState& ps = *v.m_plug;
        auto& tools = editplug::Get().tools;
        if (ps.pngRt.Get() != v.m_rt.Get()) {  // nouveau device
            ps.toolPng.clear();
            ps.pngRt = v.m_rt;
        }
        if (ps.toolPng.size() != tools.size()) ps.toolPng.resize(tools.size());
        const editplug::Tool& t = tools[size_t(i)];
        if (!t.api.png || !t.api.png_size || t.api.png_size > (4u << 20)) return nullptr;
        if (ps.toolPng[size_t(i)]) return ps.toolPng[size_t(i)].Get();
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> st;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE*>(t.api.png), t.api.png_size)) &&
            SUCCEEDED(wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            v.m_rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &ps.toolPng[size_t(i)]);
        return ps.toolPng[size_t(i)].Get();
    }
};

const CyImgEditHost Viewer::PlugHost::table = {
    CYIMG_EDIT_API_VERSION, sizeof(CyImgEditHost),
    Translate, IsCancelled,
    DocSize, Primary, Secondary, Selection, Param, SetParam, ScreenPixel, Redraw, Toast, Window, InputText,
    BeginEdit, Lock, Unlock, NewLayer,
    Line, Polyline, Rect, Ellipse, Polygon, DrawStr, MeasureStr, BeginGroup, EndGroup,
};

// =============================================================================================
// Chargement, actions « Ouvrir avec », plantages

void Viewer::EnsureEditPlugins() {
    if (!m_plug) m_plug = std::make_shared<PlugState>();
    editplug::Registry& reg = editplug::Get();
    if (reg.loaded) return;
    g_uiThread = GetCurrentThreadId();
    editplug::Load(m_exeDir, &PlugHost::table);
    for (const editplug::Tool& t : reg.tools) {
        std::wstring tip = t.name;
        if (t.api.shortcut >= 'A' && t.api.shortcut <= 'Z') tip += L" (" + std::wstring(1, wchar_t(t.api.shortcut)) + L")";
        m_plug->toolTips.push_back(std::move(tip));
    }
    if (reg.actions.empty()) return;
    // Icônes des actions (gardées jusqu'à la fin : le menu les copie à chaque ouverture)
    for (const editplug::Action& a : reg.actions) {
        HICON icon = nullptr;
        if (a.api.icon && *a.api.icon) {
            wchar_t path[MAX_PATH * 2] = {};
            if (!ExpandEnvironmentStringsW(a.api.icon, path, ARRAYSIZE(path))) wcscpy_s(path, a.api.icon);
            if (SHDefExtractIconW(path, 0, 0, &icon, nullptr, MAKELONG(32, 32)) != S_OK) icon = nullptr;
        }
        if (!icon && a.api.glyph) icon = GlyphIcon(a.api.glyph, 32);
        m_plug->actionIcons.push_back(icon);
    }
    m_plug->provider = RegisterOpenWithProvider([this](const std::wstring& path, std::vector<OpenWithEntry>& out) {
        auto& actions = editplug::Get().actions;
        const std::wstring ext = ExtensionOf(path);
        for (size_t i = 0; i < actions.size(); ++i) {
            const editplug::Action& a = actions[i];
            if (a.disabled || (!a.exts.empty() && std::find(a.exts.begin(), a.exts.end(), ext) == a.exts.end())) continue;
            OpenWithEntry e;
            e.name = a.name;
            e.icon = i < m_plug->actionIcons.size() ? m_plug->actionIcons[i] : nullptr;
            if (a.api.run) {
                e.args.clear();
                e.run = [this, i](const std::wstring& file, HWND owner) {
                    editplug::Action& act = editplug::Get().actions[i];
                    if (act.disabled) return false;
                    CyImgEditCtx ctx;
                    ctx.viewer = this;
                    int ok = 0;
                    editplug::Crash crash;
                    if (!editplug::Guard([&] { ok = act.api.run(&PlugHost::table, &ctx, file.c_str(), owner); }, crash)) {
                        act.disabled = true;
                        PluginCrashed(act.name, act.plugin->file, crash.Where());
                        return true;  // message déjà affiché
                    }
                    return ok != 0;
                };
            } else {
                wchar_t prog[MAX_PATH * 2] = {};
                if (!ExpandEnvironmentStringsW(a.api.program, prog, ARRAYSIZE(prog))) wcscpy_s(prog, a.api.program);
                e.path = prog;
                if (a.api.args) e.args = a.api.args;
            }
            out.push_back(std::move(e));
        }
    });
}

void Viewer::OnPluginJobs() { editplug::DispatchJobs(); }

void Viewer::PluginCrashed(const std::wstring& what, const std::wstring& file, const std::wstring& where) {
    Trace((L"plugin d'édition planté : " + what + L" (" + file + L") @ " + where).c_str());
    ShowToast(Tf("editapi.err.crashed", what.c_str(), file.c_str()), 6);
}

// =============================================================================================
// Effets des plugins

void Viewer::OpenPluginEffect(int i) {
    EnsureEditPlugins();
    auto& effects = editplug::Get().effects;
    if (i < 0 || i >= int(effects.size()) || m_edit.layers.empty()) return;
    editplug::Effect& e = effects[size_t(i)];
    if (e.disabled) {
        ShowToast(Tf("editapi.err.disabled", e.name.c_str()), 3);
        return;
    }
    PlugState& ps = *m_plug;
    ps.gen++;
    ps.fx = i;
    ps.vals = editplug::Defaults(e.params);
    ps.fullPx = Pixels();
    ps.fullRes.Free();
    ps.fullValid = ps.shownFull = ps.applying = ps.previewPending = ps.failedShown = false;
    ps.preview.reset();
    ps.render.reset();
    if (!PlugHost::ReadScaled(*this, CurLayer().bmp.Get(), PlugHost::PreviewSide(*this), ps.smallPx)) {
        ps.fx = -1;
        ShowToast(T("edit.err.memory"));
        return;
    }
    m_edit.fx = EffectCount() + i;
    m_edit.fxPreview = true;
    m_edit.fxDirty = true;
    m_edit.fxOut.Reset();
    Invalidate();
}

void Viewer::ClosePluginEffect(bool apply) {
    if (!m_plug) return;
    PlugState& ps = *m_plug;
    if (apply && ps.fx >= 0) {
        if (ps.applying) return;  // déjà en cours
        if (ps.fullValid && ps.fullResVals == ps.vals && ps.shownFull && m_edit.fxOut) return PlugHost::FinishApply(*this);
        ps.applying = true;  // rendu complet attendu : appliqué à son arrivée
        if (!(ps.render && ps.render->values == ps.vals)) PlugHost::StartFxRender(*this);
        if (!ps.render) ps.applying = false;
        Invalidate();
        return;
    }
    for (auto* j : {&ps.preview, &ps.render})
        if (*j) (*j)->cancel = true;
    ps.preview.reset();
    ps.render.reset();
    ps.gen++;
    ps.fx = -1;
    ps.smallPx = ps.fullPx = Pixels();
    ps.fullRes.Free();
    ps.fullValid = ps.shownFull = ps.applying = ps.previewPending = false;
    ps.smallBmp.Reset();
    m_edit.fx = -1;
    m_edit.fxOut.Reset();
    m_edit.fxDirty = false;
    KillTimer(m_hwnd, kTimerPlug);
    Invalidate();
}

// Chaque image tant que l'effet est ouvert : aperçu après un réglage, rendu complet à l'arrêt
void Viewer::UpdatePluginFxPreview() {
    if (!m_plug || m_plug->fx < 0) return;
    PlugState& ps = *m_plug;
    const double now = NowSeconds();
    if (m_edit.fxDirty) {
        m_edit.fxDirty = false;
        ps.changeTime = now;
        ps.failedShown = false;
        if (ps.render && ps.render->values != ps.vals && !ps.applying) {
            ps.render->cancel = true;
            ps.render.reset();
        }
        if (!(ps.fullValid && ps.fullResVals == ps.vals)) {
            if (ps.shownFull) ps.shownFull = false;
            PlugHost::StartFxPreview(*this);
            SetTimer(m_hwnd, kTimerPlug, UINT(kFullDelay * 1000) + 30, OnPlugTimer);
        } else if (!ps.shownFull) {
            PlugHost::ShowFxFull(*this);
        }
    }
    if (!ps.render && !ps.applying && m_edit.slider < 0 && now - ps.changeTime >= kFullDelay && !(ps.fullValid && ps.fullResVals == ps.vals))
        PlugHost::StartFxRender(*this);
}

void Viewer::PluginParamCmd(bool tool, int arg) {
    EnsureEditPlugins();
    std::vector<editplug::Param>* params = nullptr;
    std::vector<double>* vals = nullptr;
    if (tool) {
        if (editplug::Tool* t = PlugHost::CurTool(*this)) {
            params = &t->params;
            vals = &t->values;
        }
    } else if (editplug::Effect* e = PlugHost::CurFx(*this); e && !m_plug->applying) {
        params = &e->params;
        vals = &m_plug->vals;
    }
    const size_t k = size_t(arg & 0xFF);
    const int sub = arg >> 8;
    if (!params || k >= params->size() || k >= vals->size()) return;
    const editplug::Param& p = (*params)[k];
    double v = (*vals)[k];
    switch (p.p.type) {
    case CYIMG_PARAM_CHECK: v = v != 0 ? 0 : 1; break;
    case CYIMG_PARAM_CHOICE: if (sub > 0) v = sub - 1; break;
    case CYIMG_PARAM_COLOR: if (!ChooseRgb(m_hwnd, v)) return; break;
    default: return;
    }
    v = Quantize(p, v);
    if (v == (*vals)[k]) return;
    (*vals)[k] = v;
    if (!tool) m_edit.fxDirty = true;
    Invalidate();
}

float Viewer::PluginSliderT(bool tool, int k) const {
    auto& reg = editplug::Get();
    const std::vector<editplug::Param>* params = nullptr;
    const std::vector<double>* vals = nullptr;
    if (tool) {
        if (m_edit.ptool >= 0 && m_edit.ptool < int(reg.tools.size())) {
            params = &reg.tools[size_t(m_edit.ptool)].params;
            vals = &reg.tools[size_t(m_edit.ptool)].values;
        }
    } else if (m_plug && m_plug->fx >= 0 && m_plug->fx < int(reg.effects.size())) {
        params = &reg.effects[size_t(m_plug->fx)].params;
        vals = &m_plug->vals;
    }
    if (!params || k < 0 || size_t(k) >= params->size() || size_t(k) >= vals->size()) return 0;
    const editplug::Param& p = (*params)[size_t(k)];
    const double t = p.p.max > p.p.min ? ((*vals)[size_t(k)] - p.p.min) / (p.p.max - p.p.min) : 0;
    return float((p.p.flags & CYIMG_PARAM_SQUARE) ? std::sqrt(std::max(0.0, t)) : t);
}

void Viewer::SetPluginSliderT(bool tool, int k, float t) {
    auto& reg = editplug::Get();
    std::vector<editplug::Param>* params = nullptr;
    std::vector<double>* vals = nullptr;
    if (tool) {
        if (editplug::Tool* tl = PlugHost::CurTool(*this)) {
            params = &tl->params;
            vals = &tl->values;
        }
    } else if (m_plug && m_plug->fx >= 0 && m_plug->fx < int(reg.effects.size()) && !m_plug->applying) {
        params = &reg.effects[size_t(m_plug->fx)].params;
        vals = &m_plug->vals;
    }
    if (!params || k < 0 || size_t(k) >= params->size() || size_t(k) >= vals->size()) return;
    const editplug::Param& p = (*params)[size_t(k)];
    const double tt = (p.p.flags & CYIMG_PARAM_SQUARE) ? double(t) * t : t;
    const double v = Quantize(p, p.p.min + (p.p.max - p.p.min) * tt);
    if (v == (*vals)[size_t(k)]) return;
    (*vals)[size_t(k)] = v;
    if (!tool) m_edit.fxDirty = true;
}

// Panneau de réglages d'un effet de plugin (à la place du panneau Calques)
void Viewer::DrawPluginEffectPanel(float cw, float ch) {
    editplug::Effect* e = PlugHost::CurFx(*this);
    if (!e) return;
    PlugState& ps = *m_plug;
    const float s = m_scale, pad = 12 * s;
    const D2D1_RECT_F full = LayersPanelRect(cw, ch);
    const float iw = full.right - full.left - 2 * pad;
    // Hauteur : titre, note de sélection, réglages, état, aperçu / réinitialiser, boutons
    auto choiceRows = [&](const editplug::Param& p) { return (int(p.choices.size()) + 2) / 3; };
    float height = 62 * s + (m_edit.selValid ? 22 * s : 0) + 26 * s + 34 * s + 54 * s;
    for (size_t k = 0; k < e->params.size(); ++k) {
        if (!ParamVisible(e->params, ps.vals, k)) continue;
        const editplug::Param& p = e->params[k];
        height += p.p.type == CYIMG_PARAM_SLIDER ? 54 * s : p.p.type == CYIMG_PARAM_CHOICE ? 26 * s + choiceRows(p) * 32 * s : 34 * s;
    }
    const D2D1_RECT_F pr = D2D1::RectF(full.left, full.top, full.right, std::min(full.bottom, full.top + height));
    FillRound(pr, 8 * s, kPanel, 0.97f);
    SetColor(kAccent, 0.35f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(pr, 8 * s, 8 * s), m_brush.Get(), 1);
    EditBtn(EditCmd::Panel, 0, pr, nullptr);
    Text(e->name, m_tfTitle.Get(), D2D1::RectF(pr.left + pad, pr.top + 8 * s, pr.right - pad, pr.top + 32 * s), kAccent, 1);
    Text(e->plugin->Name(), m_tfSub.Get(), D2D1::RectF(pr.left + pad, pr.top + 32 * s, pr.right - pad, pr.top + 50 * s), kGrey, 0.9f);
    float y = pr.top + 58 * s;
    if (m_edit.selValid) {
        Text(T("edit.fx.selection_note"), m_tfSmall.Get(), D2D1::RectF(pr.left + pad, y, pr.right - pad, y + 20 * s), kGrey, 0.9f);
        y += 22 * s;
    }
    const bool busy = ps.applying;
    for (size_t k = 0; k < e->params.size(); ++k) {
        if (!ParamVisible(e->params, ps.vals, k)) continue;
        const editplug::Param& p = e->params[k];
        const double v = k < ps.vals.size() ? ps.vals[k] : p.p.def;
        const float l = pr.left + pad, r = pr.right - pad;
        switch (p.p.type) {
        case CYIMG_PARAM_SLIDER:
            Text(p.label, m_tfSmall.Get(), D2D1::RectF(l, y, r - 80 * s, y + 22 * s), kWhite, 0.95f);
            Text(FormatParam(p, v), m_tfSmall.Get(), D2D1::RectF(r - 80 * s, y, r, y + 22 * s), kAccent, 1);
            if (!busy) DrawSlider(kSlFx0 + int(k), D2D1::RectF(l, y + 24 * s, r, y + 42 * s), p.label.c_str());
            y += 54 * s;
            break;
        case CYIMG_PARAM_CHECK: {
            const D2D1_RECT_F row = D2D1::RectF(l - 4 * s, y + 2 * s, r, y + 30 * s);
            EditBtn(EditCmd::PlugFxParam, int(k), row, p.label.c_str(), false, !busy);
            const D2D1_RECT_F box = D2D1::RectF(l, y + 8 * s, l + 16 * s, y + 24 * s);
            if (v != 0) FillRound(box, 3 * s, kAccent, 1);
            SetColor(v != 0 ? kAccent : kWhite, v != 0 ? 1.0f : 0.6f);
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3 * s, 3 * s), m_brush.Get(), 1.5f * s);
            if (v != 0) Text(L"", m_tfIcon.Get(), box, 0x0B1A2A, 1);
            Text(p.label, m_tfSmall.Get(), D2D1::RectF(box.right + 8 * s, y + 2 * s, r, y + 30 * s), kWhite, 0.95f);
            y += 34 * s;
            break;
        }
        case CYIMG_PARAM_CHOICE: {
            Text(p.label, m_tfSmall.Get(), D2D1::RectF(l, y, r, y + 22 * s), kWhite, 0.95f);
            y += 26 * s;
            const int n = int(p.choices.size()), per = std::min(3, std::max(1, n));
            const float bw = (iw - float(per - 1) * 4 * s) / float(per);
            for (int c = 0; c < n; ++c) {
                const float bx = l + float(c % per) * (bw + 4 * s), by = y + float(c / per) * 32 * s;
                const D2D1_RECT_F b = D2D1::RectF(bx, by, bx + bw, by + 28 * s);
                const bool on = int(std::lround(v)) == c;
                EditBtn(EditCmd::PlugFxParam, int(k) | ((c + 1) << 8), b, nullptr, on, !busy);
                SetColor(on ? kAccent : kWhite, on ? 0.7f : 0.2f);
                m_rt->DrawRoundedRectangle(D2D1::RoundedRect(b, 5 * s, 5 * s), m_brush.Get(), 1);
                Text(p.choices[size_t(c)], m_tfCenter.Get(), b, on ? kAccent : kWhite, 0.95f);
            }
            y += float(choiceRows(p)) * 32 * s;
            break;
        }
        case CYIMG_PARAM_COLOR: {
            Text(p.label, m_tfSmall.Get(), D2D1::RectF(l, y + 2 * s, r - 60 * s, y + 30 * s), kWhite, 0.95f);
            const D2D1_RECT_F sw = D2D1::RectF(r - 52 * s, y + 4 * s, r, y + 28 * s);
            EditBtn(EditCmd::PlugFxParam, int(k), sw, T("editapi.tip.color"), false, !busy);
            SetColor(uint32_t(v), 1);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(sw, 4 * s, 4 * s), m_brush.Get());
            SetColor(kWhite, 0.5f);
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(sw, 4 * s, 4 * s), m_brush.Get(), 1);
            y += 34 * s;
            break;
        }
        default: break;
        }
    }
    // État du calcul
    const bool computing = ps.preview || ps.render;
    if (busy || computing) {
        Text(T(busy ? "editapi.fx.applying" : "editapi.fx.computing"), m_tfSmall.Get(),
             D2D1::RectF(pr.left + pad + 22 * s, y, pr.right - pad, y + 22 * s), kGrey, 0.9f);
        DrawSpinner(pr.left + pad + 8 * s, y + 11 * s, 6 * s, 0.8f);
        Invalidate();  // animation jusqu'à la fin du calcul
    }
    y += 26 * s;
    // Aperçu (case à cocher) et Réinitialiser
    const D2D1_RECT_F pv = D2D1::RectF(pr.left + pad - 4 * s, y, pr.left + pad + 110 * s, y + 26 * s);
    EditBtn(EditCmd::FxPreview, 0, pv, T("edit.tip.fx_preview"), false, !busy);
    const D2D1_RECT_F box = D2D1::RectF(pv.left + 4 * s, y + 5 * s, pv.left + 20 * s, y + 21 * s);
    if (m_edit.fxPreview) FillRound(box, 3 * s, kAccent, 1);
    SetColor(m_edit.fxPreview ? kAccent : kWhite, m_edit.fxPreview ? 1.0f : 0.6f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3 * s, 3 * s), m_brush.Get(), 1.5f * s);
    if (m_edit.fxPreview) Text(L"", m_tfIcon.Get(), box, 0x0B1A2A, 1);
    Text(T("edit.fx.preview"), m_tfSmall.Get(), D2D1::RectF(box.right + 8 * s, y, pv.right, y + 26 * s), kWhite, 0.95f);
    const D2D1_RECT_F rs = D2D1::RectF(pr.right - pad - 110 * s, y, pr.right - pad + 4 * s, y + 26 * s);
    EditBtn(EditCmd::FxReset, 0, rs, T("edit.tip.fx_reset"), false, !busy);
    Text(T("edit.fx.reset"), m_tfSmall.Get(), D2D1::RectF(rs.left, rs.top, rs.right - 6 * s, rs.bottom), kGrey, 1);
    y += 34 * s;
    // Appliquer / Annuler
    const float bw = (pr.right - pr.left - 2 * pad - 8 * s) / 2;
    const D2D1_RECT_F ok = D2D1::RectF(pr.left + pad, y, pr.left + pad + bw, y + 34 * s);
    const D2D1_RECT_F no = D2D1::RectF(ok.right + 8 * s, y, pr.right - pad, y + 34 * s);
    const bool hOk = EditBtn(EditCmd::FxApply, 0, ok, T("edit.tip.fx_apply"), false, !busy);
    FillRound(ok, 6 * s, kAccent, busy ? 0.4f : hOk ? 1.0f : 0.85f);
    Text(T("edit.fx.apply"), m_tfCenter.Get(), ok, 0x0B1A2A, 1);
    EditBtn(EditCmd::FxCancel, 0, no, T("edit.tip.fx_cancel"));
    SetColor(kWhite, 0.25f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(no, 6 * s, 6 * s), m_brush.Get(), 1);
    Text(T("edit.fx.cancel"), m_tfCenter.Get(), no, kWhite, 0.95f);
}

// Menu Effets : effets intégrés (par groupes), puis ceux des plugins, par catégorie
void Viewer::DrawEffectsMenu(float cw, float ch) {
    EnsureEditPlugins();
    const auto& effects = editplug::Get().effects;
    struct Row {
        int fx;              // -1 : en-tête de catégorie, -2 : séparateur
        std::wstring label;
        bool disabled;
    };
    std::vector<Row> rows;
    for (int i = 0; i < EffectCount(); ++i) {
        rows.push_back({i, T(EffectKey(i)), false});
        if (i == 2 || i == 5 || i == 9) rows.push_back({-2, {}, false});  // réglages / couleur / filtres / autres
    }
    // Catégories dans l'ordre de première apparition
    std::vector<std::wstring> cats;
    for (const auto& e : effects)
        if (std::find(cats.begin(), cats.end(), e.category) == cats.end()) cats.push_back(e.category);
    for (const std::wstring& c : cats) {
        rows.push_back({-1, c, false});
        for (int i = 0; i < int(effects.size()); ++i)
            if (effects[size_t(i)].category == c)
                rows.push_back({EffectCount() + i, effects[size_t(i)].name, effects[size_t(i)].disabled});
    }
    const float s = m_scale, iw = 240 * s;
    float ih = 30 * s, hh = 26 * s, sh = 9 * s;
    auto total = [&] {
        float h = 8 * s;
        for (const Row& r : rows) h += r.fx >= 0 ? ih : r.fx == -1 ? hh : sh;
        return h;
    };
    if (total() > ch - 12 * s) {  // trop long pour la fenêtre : lignes resserrées
        const float k = std::max(0.6f, (ch - 12 * s) / total());
        ih *= k;
        hh *= k;
        sh *= k;
    }
    const float height = total();
    const D2D1_RECT_F a = m_edit.menuAnchor;
    const float left = std::clamp(a.left, 6 * s, cw - iw - 6 * s);
    float top = a.bottom + 6 * s;
    if (top + height > ch - 6 * s) top = std::max(6 * s, std::min(a.top - 6 * s - height, ch - 6 * s - height));
    const D2D1_RECT_F menu = D2D1::RectF(left, top, left + iw, top + height);
    FillRound(menu, 8 * s, kPanel, 0.98f);
    SetColor(kWhite, 0.12f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(menu, 8 * s, 8 * s), m_brush.Get(), 1);
    m_editButtons.push_back({EditCmd::Panel, 0, menu, nullptr});
    float iy = menu.top + 4 * s;
    for (const Row& r : rows) {
        if (r.fx == -2) {
            SetColor(kWhite, 0.1f);
            m_rt->FillRectangle(D2D1::RectF(menu.left + 10 * s, iy + sh / 2 - 0.5f, menu.right - 10 * s, iy + sh / 2 + 0.5f), m_brush.Get());
            iy += sh;
        } else if (r.fx == -1) {  // catégorie d'un plugin
            SetColor(kAccent, 0.3f);
            m_rt->FillRectangle(D2D1::RectF(menu.left + 10 * s, iy + 3 * s, menu.right - 10 * s, iy + 3 * s + 1), m_brush.Get());
            Text(r.label, m_tfSub.Get(), D2D1::RectF(menu.left + 14 * s, iy + 4 * s, menu.right - 8 * s, iy + hh), kAccent, 0.9f);
            iy += hh;
        } else {
            const D2D1_RECT_F rc = D2D1::RectF(menu.left + 4 * s, iy, menu.right - 4 * s, iy + ih);
            EditBtn(EditCmd::FxPick, r.fx, rc, nullptr, false, !r.disabled);
            const std::wstring label = r.disabled ? r.label + L"  " + T("editapi.disabled") : r.label;
            Text(label, m_tfSmall.Get(), D2D1::RectF(rc.left + 10 * s, rc.top, rc.right - 4 * s, rc.bottom), kWhite, r.disabled ? 0.35f : 0.95f);
            iy += ih;
        }
    }
}

// =============================================================================================
// Outils des plugins

int Viewer::PluginToolCount() const { return int(editplug::Get().tools.size()); }

const wchar_t* Viewer::PluginToolName(int i) const {
    return m_plug && i >= 0 && i < int(m_plug->toolTips.size()) ? m_plug->toolTips[size_t(i)].c_str() : nullptr;
}

std::wstring Viewer::PluginToolHint() const {
    const auto& tools = editplug::Get().tools;
    return m_edit.ptool >= 0 && m_edit.ptool < int(tools.size()) ? tools[size_t(m_edit.ptool)].hint : std::wstring();
}

void Viewer::DrawPluginToolIcon(int i, const D2D1_RECT_F& r, uint32_t col, float a) {
    const auto& tools = editplug::Get().tools;
    if (i < 0 || i >= int(tools.size()) || !m_plug) return;
    const editplug::Tool& t = tools[size_t(i)];
    if (t.disabled) a *= 0.3f;
    if (ID2D1Bitmap* png = PlugHost::ToolPng(*this, i)) {
        const float s = m_scale, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, half = std::min(10 * s, (r.bottom - r.top) / 2 - 2 * s);
        m_rt->DrawBitmap(png, D2D1::RectF(cx - half, cy - half, cx + half, cy + half), a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else if (!t.glyph.empty()) {
        Text(t.glyph, m_tfIconMid.Get(), r, col, a);
    } else {
        Text(t.name.substr(0, 1), m_tfToast.Get(), r, col, a);
    }
}

void Viewer::ActivatePluginTool(bool on) {
    editplug::Tool* t = PlugHost::CurTool(*this);
    if (!t || !t->api.activate) return;
    CyImgEditCtx ctx = PlugHost::ToolCtx(*this);
    editplug::Crash crash;
    const int index = m_edit.ptool;
    if (!editplug::Guard([&] { t->api.activate(&PlugHost::table, &ctx, on ? 1 : 0); }, crash)) PlugHost::ToolCrashed(*this, index, crash);
    PlugHost::Cleanup(*this, true);
}

void Viewer::SetPluginTool(int i) {
    EnsureEditPlugins();
    auto& tools = editplug::Get().tools;
    if (i < 0 || i >= int(tools.size())) return;
    if (tools[size_t(i)].disabled) {
        ShowToast(Tf("editapi.err.disabled", tools[size_t(i)].name.c_str()), 3);
        return;
    }
    if (m_edit.tool == Tool::Plugin) {
        if (m_edit.ptool == i) return;
        ActivatePluginTool(false);
        m_edit.ptool = i;
        SetTool(Tool::Plugin);
        ActivatePluginTool(true);
    } else {
        m_edit.ptool = i;
        SetTool(Tool::Plugin);  // active l'outil
    }
}

bool Viewer::PluginToolKey(WPARAM vk) {
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 || vk < 'A' || vk > 'Z') return false;
    const auto& tools = editplug::Get().tools;
    for (int i = 0; i < int(tools.size()); ++i)
        if (!tools[size_t(i)].disabled && tools[size_t(i)].api.shortcut == vk) {
            SetPluginTool(i);
            return true;
        }
    return false;
}

bool Viewer::PluginToolDown(D2D1_POINT_2F ip, bool right, bool dbl) {
    editplug::Tool* t = PlugHost::CurTool(*this);
    if (!t || !t->api.mouse_down) return false;
    m_plug->begun = false;  // nouveau geste : nouvelle entrée d'historique au premier dessin
    CyImgEditCtx ctx = PlugHost::ToolCtx(*this);
    const CyImgToolEvent ev = PlugHost::Event(ip, right ? CYIMG_BTN_RIGHT : CYIMG_BTN_LEFT, dbl ? 2 : 1);
    int capture = 0;
    editplug::Crash crash;
    const int index = m_edit.ptool;
    if (!editplug::Guard([&] { capture = t->api.mouse_down(&PlugHost::table, &ctx, &ev); }, crash)) {
        PlugHost::ToolCrashed(*this, index, crash);
        return false;
    }
    if (!capture) PlugHost::Cleanup(*this, true);
    Invalidate();
    return capture != 0;
}

void Viewer::PluginToolMove(POINT p) {
    editplug::Tool* t = PlugHost::CurTool(*this);
    if (!t || !t->api.mouse_move) return;
    if (!m_edit.drawing && HitEdit(p)) return;  // survol de l'interface
    CyImgEditCtx ctx = PlugHost::ToolCtx(*this);
    const CyImgToolEvent ev = PlugHost::Event(ScreenToImage(p), m_edit.drawing ? (m_edit.secondary ? CYIMG_BTN_RIGHT : CYIMG_BTN_LEFT) : 0, 0);
    editplug::Crash crash;
    const int index = m_edit.ptool;
    if (!editplug::Guard([&] { t->api.mouse_move(&PlugHost::table, &ctx, &ev); }, crash)) PlugHost::ToolCrashed(*this, index, crash);
}

void Viewer::PluginToolUp(D2D1_POINT_2F ip) {
    editplug::Tool* t = PlugHost::CurTool(*this);
    if (!t) return;
    if (t->api.mouse_up) {
        CyImgEditCtx ctx = PlugHost::ToolCtx(*this);
        const CyImgToolEvent ev = PlugHost::Event(ip, m_edit.secondary ? CYIMG_BTN_RIGHT : CYIMG_BTN_LEFT, 1);
        editplug::Crash crash;
        const int index = m_edit.ptool;
        if (!editplug::Guard([&] { t->api.mouse_up(&PlugHost::table, &ctx, &ev); }, crash)) {
            PlugHost::ToolCrashed(*this, index, crash);
            return;
        }
    }
    PlugHost::Cleanup(*this, true);
    Invalidate();
}

void Viewer::PluginToolCancel() {
    editplug::Tool* t = PlugHost::CurTool(*this);
    m_edit.drawing = false;
    if (GetCapture() == m_hwnd) ReleaseCapture();
    if (t && t->api.cancel) {
        CyImgEditCtx ctx = PlugHost::ToolCtx(*this);
        editplug::Crash crash;
        const int index = m_edit.ptool;
        if (!editplug::Guard([&] { t->api.cancel(&PlugHost::table, &ctx); }, crash)) PlugHost::ToolCrashed(*this, index, crash);
    }
    PlugHost::Cleanup(*this, false);
    Invalidate();
}

// Surcouche : quand la souris est sur l'image (ou pendant un glissement), dans le repère de l'image
void Viewer::DrawPluginToolOverlay(float left, float top, float z) {
    editplug::Tool* t = PlugHost::CurTool(*this);
    if (!t || !t->api.draw_overlay) return;
    if (!m_edit.drawing && (!m_mouseInside || HitEdit(m_mouse) || m_edit.spacePan || m_edit.slider >= 0 || m_edit.menu != EditMenu::None)) return;
    CyImgEditCtx ctx = PlugHost::ToolCtx(*this);
    ctx.overlay = true;
    m_plug->overlayLayers = 0;
    m_dc->SetTransform(D2D1::Matrix3x2F::Scale(z, z) * D2D1::Matrix3x2F::Translation(left, top));
    editplug::Crash crash;
    const int index = m_edit.ptool;
    const bool ok = editplug::Guard([&] { t->api.draw_overlay(&PlugHost::table, &ctx); }, crash);
    for (; m_plug->overlayLayers > 0; --m_plug->overlayLayers) m_dc->PopLayer();
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    if (!ok) PlugHost::ToolCrashed(*this, index, crash);
}

// Paramètres de l'outil dans la barre d'options ; retourne la position suivante
float Viewer::DrawPluginToolParams(float x, float y, float h) {
    editplug::Tool* t = PlugHost::CurTool(*this);
    if (!t) return x;
    const float s = m_scale;
    auto width = [&](const std::wstring& txt) {
        ComPtr<IDWriteTextLayout> lay;
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(m_dwrite->CreateTextLayout(txt.c_str(), UINT32(txt.size()), m_tfSmall.Get(), 1000, 100, &lay))) lay->GetMetrics(&tm);
        return tm.width;
    };
    for (size_t k = 0; k < t->params.size(); ++k) {
        if (!ParamVisible(t->params, t->values, k)) continue;
        const editplug::Param& p = t->params[k];
        const double v = t->values[k];
        switch (p.p.type) {
        case CYIMG_PARAM_SLIDER: {
            const float lw = width(p.label);
            Text(p.label, m_tfSmall.Get(), D2D1::RectF(x, y, x + lw + 4 * s, y + h), kGrey, 1);
            x += lw + 12 * s;
            DrawSlider(kSlPlugTool0 + int(k), D2D1::RectF(x, y + h / 2 - 8 * s, x + 110 * s, y + h / 2 + 8 * s), p.label.c_str());
            x += 120 * s;
            Text(FormatParam(p, v), m_tfSmall.Get(), D2D1::RectF(x, y, x + 54 * s, y + h), kWhite, 0.95f);
            x += 64 * s;
            break;
        }
        case CYIMG_PARAM_CHECK: {
            const float lw = width(p.label);
            const D2D1_RECT_F row = D2D1::RectF(x, y + 5 * s, x + 30 * s + lw, y + h - 5 * s);
            EditBtn(EditCmd::PlugToolParam, int(k), row, p.label.c_str());
            const D2D1_RECT_F box = D2D1::RectF(x + 6 * s, y + h / 2 - 8 * s, x + 22 * s, y + h / 2 + 8 * s);
            if (v != 0) FillRound(box, 3 * s, kAccent, 1);
            SetColor(v != 0 ? kAccent : kWhite, v != 0 ? 1.0f : 0.6f);
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3 * s, 3 * s), m_brush.Get(), 1.5f * s);
            if (v != 0) Text(L"", m_tfIcon.Get(), box, 0x0B1A2A, 1);
            Text(p.label, m_tfSmall.Get(), D2D1::RectF(box.right + 6 * s, y, row.right + 4 * s, y + h), kWhite, 0.95f);
            x = row.right + 14 * s;
            break;
        }
        case CYIMG_PARAM_CHOICE: {
            if (!p.label.empty()) {
                const float lw = width(p.label);
                Text(p.label, m_tfSmall.Get(), D2D1::RectF(x, y, x + lw + 4 * s, y + h), kGrey, 1);
                x += lw + 10 * s;
            }
            for (size_t c = 0; c < p.choices.size(); ++c) {
                const float bw = width(p.choices[c]) + 20 * s;
                const D2D1_RECT_F b = D2D1::RectF(x, y + 6 * s, x + bw, y + h - 6 * s);
                const bool on = size_t(std::lround(v)) == c;
                EditBtn(EditCmd::PlugToolParam, int(k) | int((c + 1) << 8), b, nullptr, on);
                SetColor(on ? kAccent : kWhite, on ? 0.7f : 0.2f);
                m_rt->DrawRoundedRectangle(D2D1::RoundedRect(b, 5 * s, 5 * s), m_brush.Get(), 1);
                Text(p.choices[c], m_tfCenter.Get(), b, on ? kAccent : kWhite, 0.95f);
                x = b.right + 4 * s;
            }
            x += 10 * s;
            break;
        }
        case CYIMG_PARAM_COLOR: {
            const float lw = width(p.label);
            Text(p.label, m_tfSmall.Get(), D2D1::RectF(x, y, x + lw + 4 * s, y + h), kGrey, 1);
            x += lw + 10 * s;
            const D2D1_RECT_F sw = D2D1::RectF(x, y + 8 * s, x + 34 * s, y + h - 8 * s);
            EditBtn(EditCmd::PlugToolParam, int(k), sw, T("editapi.tip.color"));
            SetColor(uint32_t(v), 1);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(sw, 4 * s, 4 * s), m_brush.Get());
            SetColor(kWhite, 0.5f);
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(sw, 4 * s, 4 * s), m_brush.Get(), 1);
            x = sw.right + 18 * s;
            break;
        }
        default: break;
        }
    }
    return x;
}

// =============================================================================================
// Modes des plugins : onglet supplémentaire, non destructif comme le Développement

class Viewer::PluginEditMode final : public Viewer::EditMode {
public:
    PluginEditMode(Viewer& viewer, int index) : EditMode(viewer), m_index(index) {}
    const char* NameKey() const override { return ""; }
    const wchar_t* Name() const override { return Def().name.c_str(); }
    const wchar_t* Glyph() const override { return Def().glyph.empty() ? L"" : Def().glyph.c_str(); }

    bool Enter() override;
    void Leave() override;
    void Commit() override;
    void Close() override;
    bool BeginExport() override;
    void EndExport() override;
    void DropGpu() override { Close(); }

    void Prepare() override;
    ID2D1Bitmap1* LayerBitmap(int layer) const override;
    void DrawOverlay(float left, float top, float z) override;
    float DrawTopButtons(float right, float y, float size) override;
    void DrawOptionsBar(float cw, float y, float h) override;
    void DrawSide(float cw, float ch) override;
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
    enum { kCtlUndo = 1 << 20, kCtlRedo, kCtlReset, kCtlOriginal, kCtlScroll };
    enum { kDragNone, kDragSlider, kDragScroll };

    editplug::Mode& Def() const { return editplug::Get().modes[size_t(m_index)]; }
    Layer* ModeLayer() const { return m_layer >= 0 && m_layer < int(v.m_edit.layers.size()) ? &v.m_edit.layers[size_t(m_layer)] : nullptr; }
    float W() const { return m_src ? float(m_src->GetPixelSize().width) : float(v.m_edit.w); }
    float H() const { return m_src ? float(m_src->GetPixelSize().height) : float(v.m_edit.h); }
    void Changed();
    void PushUndo(int id = -1);
    void SetValue(size_t k, double val);
    bool Visible(size_t k) const;
    void StartPreview();
    void StartRender();
    void OnJob(editplug::Job& j, uint64_t gen, bool full);
    bool EnsureShown();
    void ShowFull();
    bool EnsureFull();
    bool RenderNow();
    void Crashed(const editplug::Crash& c);
    void CancelJobs();
    void Control(int id, POINT p, bool right, bool dbl);
    CyImgEditCtx Ctx() const {
        CyImgEditCtx c;
        c.viewer = &v;
        return c;
    }
    bool Btn(int id, const D2D1_RECT_F& r, const wchar_t* tip, bool active = false, bool enabled = true);
    void TextButton(int id, const D2D1_RECT_F& r, const std::wstring& label, const wchar_t* tip, bool active = false, bool enabled = true);
    bool InClip(const D2D1_RECT_F& r) const { return r.bottom > m_clip.top && r.top < m_clip.bottom; }
    float TextWidth(const std::wstring& t) const {
        ComPtr<IDWriteTextLayout> lay;
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(v.m_dwrite->CreateTextLayout(t.c_str(), UINT32(t.size()), v.m_tfSmall.Get(), 1000, 100, &lay))) lay->GetMetrics(&tm);
        return tm.width;
    }

    int m_index;
    bool m_active = false;
    int m_layer = -1;
    uint64_t m_gen = 0;
    ComPtr<ID2D1Bitmap1> m_src;    // pixels du calque avant le mode
    ComPtr<ID2D1Bitmap1> m_baked;  // résultat intégré au dernier Commit (reprise possible)
    ComPtr<ID2D1Bitmap1> m_shown;  // aperçu affiché à la place du calque (taille du document)
    ComPtr<ID2D1Bitmap1> m_smallBmp;
    std::vector<double> m_vals, m_bakedVals;
    std::vector<std::vector<double>> m_undo, m_redo;
    double m_lastPush = 0;
    int m_lastPushId = -1;
    Pixels m_small, m_full;
    editplug::JobPtr m_preview, m_render;
    bool m_previewPending = false;
    PixelBuffer m_fullRes;
    std::vector<double> m_fullResVals;
    bool m_fullValid = false, m_shownFull = false;
    double m_changeTime = -10;
    bool m_original = false;
    bool m_failedShown = false;
    bool m_dirtyAtEnter = false;   // état « modifié » du document à l'entrée (rétabli si le plugin plante)
    std::vector<bool> m_open;      // sections dépliées
    // Panneau
    D2D1_RECT_F m_panel{}, m_scrollRc{}, m_clip{}, m_thumbRc{};
    std::vector<D2D1_RECT_F> m_track;
    float m_scroll = 0, m_contentH = 0;
    int m_drag = kDragNone, m_dragId = -1;
    POINT m_dragStart{};
    double m_dragVal = 0;
    // Enregistrer / copier
    bool m_exporting = false;
    std::vector<Layer> m_savedLayers;
};

std::vector<std::shared_ptr<Viewer::EditMode>> Viewer::NewPluginModes() {
    EnsureEditPlugins();
    std::vector<std::shared_ptr<EditMode>> out;
    for (int i = 0; i < int(editplug::Get().modes.size()); ++i) out.push_back(std::make_shared<PluginEditMode>(*this, i));
    return out;
}

bool Viewer::PluginEditMode::Enter() {
    auto& e = v.m_edit;
    editplug::Mode& d = Def();
    if (d.disabled) {
        v.ShowToast(Tf("editapi.err.disabled", d.name.c_str()), 3);
        return false;
    }
    if (e.layers.empty() || !v.m_dc) return false;
    if (e.textActive) v.CommitText();
    if (e.fx >= 0) v.CloseEffect(false);
    e.cropValid = false;
    e.menu = EditMenu::None;
    Layer& l = v.CurLayer();
    const bool resume = m_src && m_baked && l.bmp == m_baked && m_layer == e.layer && m_src->GetPixelSize().width == e.w &&
                        m_src->GetPixelSize().height == e.h;
    if (!resume) {
        m_src = l.bmp;
        m_baked.Reset();
        m_vals = editplug::Defaults(d.controls);
        m_bakedVals.clear();
        m_undo.clear();
        m_redo.clear();
        if (d.api.on_enter) {
            CyImgEditCtx ctx = Ctx();
            editplug::Crash crash;
            if (!editplug::Guard([&] { d.api.on_enter(&PlugHost::table, &ctx, m_vals.data()); }, crash)) {
                d.disabled = true;
                v.PluginCrashed(d.name, d.plugin->file, crash.Where());
                return false;
            }
            for (size_t k = 0; k < m_vals.size(); ++k) m_vals[k] = Quantize(d.controls[k], m_vals[k]);
        }
    }
    m_layer = e.layer;
    if (m_open.size() != d.controls.size()) {
        m_open.assign(d.controls.size(), true);
        for (size_t k = 0; k < d.controls.size(); ++k)
            if (d.controls[k].p.type == CYIMG_PARAM_SECTION) m_open[k] = d.controls[k].p.def != 0;
    }
    if (!PlugHost::ReadScaled(v, m_src.Get(), PlugHost::PreviewSide(v), m_small)) {
        v.ShowToast(T("edit.err.memory"));
        return false;
    }
    m_dirtyAtEnter = e.dirty;
    m_active = true;
    m_original = false;
    m_drag = kDragNone;
    m_failedShown = false;
    m_gen++;
    m_fullValid = m_shownFull = false;
    m_shown.Reset();
    Changed();
    return true;
}

void Viewer::PluginEditMode::CancelJobs() {
    for (auto* j : {&m_preview, &m_render})
        if (*j) (*j)->cancel = true;
    m_preview.reset();
    m_render.reset();
    m_previewPending = false;
    m_gen++;
}

void Viewer::PluginEditMode::Leave() {
    Commit();
    editplug::Mode& d = Def();
    if (m_active && d.api.on_leave && !d.disabled) {
        CyImgEditCtx ctx = Ctx();
        editplug::Crash crash;
        if (!editplug::Guard([&] { d.api.on_leave(&PlugHost::table, &ctx, m_vals.data()); }, crash)) {
            d.disabled = true;
            v.PluginCrashed(d.name, d.plugin->file, crash.Where());
        }
    }
    CancelJobs();
    m_active = false;
    m_small = m_full = Pixels();
    m_fullRes.Free();
    m_fullValid = m_shownFull = false;
    m_shown.Reset();
    m_smallBmp.Reset();
    m_drag = kDragNone;
    if (v.m_hwnd) KillTimer(v.m_hwnd, kTimerPlug);
}

void Viewer::PluginEditMode::Close() {
    CancelJobs();
    m_active = false;
    m_src.Reset();
    m_baked.Reset();
    m_vals.clear();
    m_bakedVals.clear();
    m_undo.clear();
    m_redo.clear();
    m_small = m_full = Pixels();
    m_fullRes.Free();
    m_fullValid = m_shownFull = false;
    m_shown.Reset();
    m_smallBmp.Reset();
    m_savedLayers.clear();
    m_exporting = false;
    m_drag = kDragNone;
    if (v.m_hwnd) KillTimer(v.m_hwnd, kTimerPlug);
}

// Intégration : ce qui est affiché devient le calque (une entrée d'historique)
void Viewer::PluginEditMode::Commit() {
    Layer* l = ModeLayer();
    if (!m_active || !m_src || !l || m_exporting || Def().disabled) return;
    if (l->bmp == m_baked && m_bakedVals == m_vals) return;
    if (!(m_fullValid && m_fullResVals == m_vals) && !RenderNow()) return;
    ComPtr<ID2D1Bitmap1> res = v.NewCanvas(m_full.w, m_full.h);
    if (!res || FAILED(res->CopyFromMemory(nullptr, m_fullRes.Data(), m_full.stride))) {
        v.ShowToast(T("edit.err.vram"));
        return;
    }
    v.m_edit.layer = m_layer;
    v.PushLayerSwap(res);
    m_baked = res;
    m_bakedVals = m_vals;
    v.Invalidate();
}

bool Viewer::PluginEditMode::BeginExport() {
    Layer* l = ModeLayer();
    if (!m_active || !m_src || !l || m_exporting || Def().disabled) return false;
    if (l->bmp == m_baked && m_bakedVals == m_vals) return false;  // déjà intégré
    if (!(m_fullValid && m_fullResVals == m_vals) && !RenderNow()) return false;
    ComPtr<ID2D1Bitmap1> res = v.NewCanvas(m_full.w, m_full.h);
    if (!res || FAILED(res->CopyFromMemory(nullptr, m_fullRes.Data(), m_full.stride))) return false;
    m_savedLayers = v.m_edit.layers;
    v.m_edit.layers[size_t(m_layer)].bmp = res;
    m_exporting = true;
    return true;
}

void Viewer::PluginEditMode::EndExport() {
    if (!m_exporting) return;
    v.m_edit.layers = std::move(m_savedLayers);
    m_savedLayers.clear();
    m_exporting = false;
    v.Invalidate();
}

void Viewer::PluginEditMode::Changed() {
    m_changeTime = NowSeconds();
    m_failedShown = false;
    if (m_render && m_render->values != m_vals) {
        m_render->cancel = true;
        m_render.reset();
    }
    if (m_fullValid && m_fullResVals == m_vals) {
        if (!m_shownFull) ShowFull();  // réglage revenu à la valeur du dernier rendu complet
    } else {
        StartPreview();
        SetTimer(v.m_hwnd, kTimerPlug, UINT(kFullDelay * 1000) + 30, OnPlugTimer);
    }
    v.m_edit.dirty = true;
    v.Invalidate();
}

void Viewer::PluginEditMode::PushUndo(int id) {
    const double now = NowSeconds();
    if (id >= 0 && id == m_lastPushId && now - m_lastPush < 0.8) {
        m_lastPush = now;
        return;
    }
    m_undo.push_back(m_vals);
    if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_lastPushId = id;
    m_lastPush = now;
}

void Viewer::PluginEditMode::SetValue(size_t k, double val) {
    const auto& c = Def().controls;
    if (k >= c.size() || k >= m_vals.size()) return;
    val = Quantize(c[k], val);
    if (val == m_vals[k]) return;
    m_vals[k] = val;
    Changed();
}

bool Viewer::PluginEditMode::Visible(size_t k) const {
    const auto& c = Def().controls;
    if (!ParamVisible(c, m_vals, k)) return false;
    for (size_t i = k; i-- > 0;)  // section repliée au-dessus
        if (c[i].p.type == CYIMG_PARAM_SECTION) return c[k].p.type == CYIMG_PARAM_SECTION || m_open[i];
    return true;
}

void Viewer::PluginEditMode::StartPreview() {
    if (!m_small.px) return;
    if (m_preview) {
        m_preview->cancel = true;
        m_previewPending = true;
        return;
    }
    m_preview = PlugHost::NewJob(m_small, CyImgRect{0, 0, uint32_t(W()), uint32_t(H())}, uint32_t(W()), uint32_t(H()), m_vals, Def().api.process);
    const uint64_t gen = m_gen;
    m_preview->done = [this, gen](editplug::Job& j) { OnJob(j, gen, false); };
    editplug::StartJob(m_preview, &PlugHost::table, v.m_hwnd, kMsgPlugJob);
}

bool Viewer::PluginEditMode::EnsureFull() {
    return m_full.px || PlugHost::ReadScaled(v, m_src.Get(), UINT32_MAX, m_full);
}

void Viewer::PluginEditMode::StartRender() {
    if (!EnsureFull()) {
        v.ShowToast(T("edit.err.memory"));
        return;
    }
    if (m_render) m_render->cancel = true;
    m_render = PlugHost::NewJob(m_full, CyImgRect{0, 0, m_full.w, m_full.h}, m_full.w, m_full.h, m_vals, Def().api.process);
    const uint64_t gen = m_gen;
    m_render->done = [this, gen](editplug::Job& j) { OnJob(j, gen, true); };
    editplug::StartJob(m_render, &PlugHost::table, v.m_hwnd, kMsgPlugJob);
}

// Calcul complet sur le thread de l'interface (intégration demandée avant la fin du rendu)
bool Viewer::PluginEditMode::RenderNow() {
    if (m_render && m_render->values == m_vals) {  // rendu en cours : on l'abandonne et on le refait ici
        m_render->cancel = true;
        m_render.reset();
    }
    if (!EnsureFull()) {
        v.ShowToast(T("edit.err.memory"));
        return false;
    }
    editplug::JobPtr j = PlugHost::NewJob(m_full, CyImgRect{0, 0, m_full.w, m_full.h}, m_full.w, m_full.h, m_vals, Def().api.process);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    editplug::RunJob(*j, &PlugHost::table);
    SetCursor(old);
    if (j->crashed) {
        Crashed(j->crash);
        return false;
    }
    if (j->result != CYIMG_OK) {
        v.ShowToast(Tf("editapi.err.failed", Def().name.c_str()), 3);
        return false;
    }
    m_fullRes.Swap(j->dst);
    m_fullResVals = m_vals;
    m_fullValid = true;
    return true;
}

bool Viewer::PluginEditMode::EnsureShown() {
    const uint32_t w = uint32_t(W()), h = uint32_t(H());
    if (!m_shown || m_shown->GetPixelSize().width != w || m_shown->GetPixelSize().height != h) m_shown = v.NewCanvas(w, h);
    return m_shown != nullptr;
}

void Viewer::PluginEditMode::ShowFull() {
    if (!m_fullValid || !EnsureShown() || FAILED(m_shown->CopyFromMemory(nullptr, m_fullRes.Data(), m_full.stride))) return;
    m_shownFull = true;
    v.m_edit.fxVer = ++v.m_edit.verSeq;  // composition des calques à refaire
    v.Invalidate();
}

void Viewer::PluginEditMode::OnJob(editplug::Job& j, uint64_t gen, bool full) {
    if (gen != m_gen) return;  // mode quitté ou calcul remplacé
    if (full && m_render.get() == &j) m_render.reset();
    if (!full && m_preview.get() == &j) m_preview.reset();
    if (j.crashed) return Crashed(j.crash);
    if (!m_active) return;
    static const bool tracing = GetEnvironmentVariableW(L"CYIMG_TRACE", nullptr, 0) > 0;
    if (tracing) {
        wchar_t tr[160];
        swprintf_s(tr, L"mode %ls : %ls %u × %u en %.1f ms (%d)", Def().name.c_str(), full ? L"rendu" : L"aperçu", j.width, j.height, j.ms, j.result);
        Trace(tr);
    }
    if (j.result != CYIMG_OK && j.result != CYIMG_E_CANCELLED && !m_failedShown) {
        m_failedShown = true;
        v.ShowToast(Tf("editapi.err.failed", Def().name.c_str()), 3);
    }
    if (j.result == CYIMG_OK && j.values == m_vals) {
        if (full) {
            m_fullRes.Swap(j.dst);
            m_fullResVals = j.values;
            m_fullValid = true;
            ShowFull();
        } else if (!m_shownFull && EnsureShown()) {  // aperçu réduit, agrandi à la taille du document
            if (!m_smallBmp || m_smallBmp->GetPixelSize().width != j.width || m_smallBmp->GetPixelSize().height != j.height)
                m_smallBmp = v.NewCanvas(j.width, j.height);
            if (m_smallBmp && SUCCEEDED(m_smallBmp->CopyFromMemory(nullptr, j.dst.Data(), j.stride))) {
                ID2D1Bitmap1* sb = m_smallBmp.Get();
                const float kx = W() / float(j.width), ky = H() / float(j.height);
                v.DrawOn(m_shown.Get(), [&] {
                    v.m_dc->SetTransform(D2D1::Matrix3x2F::Scale(kx, ky));
                    v.m_dc->DrawImage(sb, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
                    v.m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
                });
                v.m_edit.fxVer = ++v.m_edit.verSeq;
            }
        }
    }
    if (!full && m_previewPending) {
        m_previewPending = false;
        StartPreview();
    }
    v.Invalidate();
}

void Viewer::PluginEditMode::Crashed(const editplug::Crash& c) {
    editplug::Mode& d = Def();
    d.disabled = true;
    CancelJobs();
    m_shown.Reset();
    m_drag = kDragNone;
    v.m_edit.drawing = false;
    if (GetCapture() == v.m_hwnd) ReleaseCapture();
    v.PluginCrashed(d.name, d.plugin->file, c.Where());
    if (m_active && !(m_baked && ModeLayer() && ModeLayer()->bmp == m_baked)) v.m_edit.dirty = m_dirtyAtEnter;  // rien d'intégré
    if (&v.Mode() == this) v.SwitchEditMode(0);  // retour en Retouche, calque intact
}

void Viewer::PluginEditMode::Prepare() {
    if (!m_active || m_exporting || Def().disabled) return;
    if (!m_render && m_drag != kDragSlider && NowSeconds() - m_changeTime >= kFullDelay && !(m_fullValid && m_fullResVals == m_vals))
        StartRender();
}

ID2D1Bitmap1* Viewer::PluginEditMode::LayerBitmap(int layer) const {
    if (!m_active || m_exporting || layer != m_layer || !m_src) return nullptr;
    if (m_original || !m_shown) return m_src.Get();
    return m_shown.Get();
}

void Viewer::PluginEditMode::DrawOverlay(float left, float top, float z) {
    if (!m_active) return;
    const float s = v.m_scale;
    if (m_original) {
        const D2D1_RECT_F lr = D2D1::RectF(std::max(left, 0.0f) + 10 * s, std::max(top, 100 * s) + 10 * s,
                                           std::max(left, 0.0f) + 106 * s, std::max(top, 100 * s) + 34 * s);
        v.FillRound(lr, 4 * s, 0x000000, 0.6f);
        v.Text(T("develop.view.before"), v.m_tfCenter.Get(), lr, kWhite, 0.95f);
    }
    (void)z;
}

float Viewer::PluginEditMode::DrawTopButtons(float right, float y, float bs) {
    const float s = v.m_scale, gap = 2 * s;
    const D2D1_RECT_F redo = D2D1::RectF(right - bs, y, right, y + bs);
    v.EditBtn(EditCmd::ModeCtl, kCtlRedo, redo, T("develop.tip.redo"), false, !m_redo.empty());
    v.Text(L"", v.m_tfIcon.Get(), redo, kWhite, m_redo.empty() ? 0.3f : 0.92f);
    const D2D1_RECT_F undo = D2D1::RectF(redo.left - gap - bs, y, redo.left - gap, y + bs);
    v.EditBtn(EditCmd::ModeCtl, kCtlUndo, undo, T("develop.tip.undo"), false, !m_undo.empty());
    v.Text(L"", v.m_tfIcon.Get(), undo, kWhite, m_undo.empty() ? 0.3f : 0.92f);
    return undo.left;
}

bool Viewer::PluginEditMode::Btn(int id, const D2D1_RECT_F& r, const wchar_t* tip, bool active, bool enabled) {
    const D2D1_RECT_F hit = D2D1::RectF(std::max(r.left, m_clip.left), std::max(r.top, m_clip.top), std::min(r.right, m_clip.right),
                                        std::min(r.bottom, m_clip.bottom));
    if (hit.right <= hit.left || hit.bottom <= hit.top) return false;
    const float s = v.m_scale;
    const bool hov = enabled && v.m_editHover && v.m_editHover->cmd == EditCmd::ModeCtl && v.m_editHover->arg == id;
    if (active) v.FillRound(r, 5 * s, kAccent, 0.25f);
    if (hov) v.FillRound(r, 5 * s, kWhite, 0.12f);
    if (enabled) v.m_editButtons.push_back({EditCmd::ModeCtl, id, hit, tip});
    return hov;
}

void Viewer::PluginEditMode::TextButton(int id, const D2D1_RECT_F& r, const std::wstring& label, const wchar_t* tip, bool active, bool enabled) {
    const float s = v.m_scale;
    Btn(id, r, tip, active, enabled);
    v.SetColor(active ? kAccent : kWhite, active ? 0.7f : 0.2f);
    v.m_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 5 * s, 5 * s), v.m_brush.Get(), 1);
    v.Text(label, v.m_tfCenter.Get(), r, active ? kAccent : kWhite, enabled ? 0.95f : 0.35f);
}

void Viewer::PluginEditMode::DrawOptionsBar(float cw, float y, float h) {
    const float s = v.m_scale;
    auto* rt = v.m_rt.Get();
    v.SetColor(0x18191C, 0.96f);
    rt->FillRectangle(D2D1::RectF(0, y, cw, y + h), v.m_brush.Get());
    v.SetColor(kWhite, 0.06f);
    rt->FillRectangle(D2D1::RectF(0, y + h - 1, cw, y + h), v.m_brush.Get());
    m_clip = D2D1::RectF(0, 0, cw, y + h);
    v.Text(Glyph(), v.m_tfIconMid.Get(), D2D1::RectF(12 * s, y, 52 * s, y + h), kAccent, 0.95f);
    v.SetColor(kWhite, 0.1f);
    rt->FillRectangle(D2D1::RectF(60 * s, y + 8 * s, 60 * s + 1, y + h - 8 * s), v.m_brush.Get());
    // À droite : Original, Réinitialiser
    const float bs = 32 * s, by = y + (h - bs) / 2;
    auto width = [&](const std::wstring& t) {
        ComPtr<IDWriteTextLayout> lay;
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(v.m_dwrite->CreateTextLayout(t.c_str(), UINT32(t.size()), v.m_tfSmall.Get(), 1000, 100, &lay))) lay->GetMetrics(&tm);
        return tm.width;
    };
    float rx = cw - 10 * s;
    const std::wstring reset = T("editapi.mode.reset"), orig = T("develop.view.original");
    const D2D1_RECT_F rr = D2D1::RectF(rx - width(reset) - 24 * s, by, rx, by + bs);
    TextButton(kCtlReset, rr, reset, T("editapi.tip.mode_reset"), false, m_vals != editplug::Defaults(Def().controls));
    const D2D1_RECT_F ro = D2D1::RectF(rr.left - 4 * s - width(orig) - 24 * s, by, rr.left - 4 * s, by + bs);
    TextButton(kCtlOriginal, ro, orig, T("develop.tip.original"), m_original);
    const bool computing = m_preview || m_render;
    float x = 74 * s;
    if (computing) {
        v.DrawSpinner(x + 6 * s, y + h / 2, 6 * s, 0.8f);
        x += 20 * s;
        v.Invalidate();
    }
    v.Text(Def().hint.empty() ? Def().name : Def().hint, v.m_tfSmall.Get(), D2D1::RectF(x, y, ro.left - 12 * s, y + h), kGrey, 1);
}

void Viewer::PluginEditMode::DrawSide(float cw, float ch) {
    const float s = v.m_scale, pad = 12 * s;
    auto* rt = v.m_rt.Get();
    const editplug::Mode& d = Def();
    m_panel = D2D1::RectF(cw - 314 * s, 108 * s, cw - 10 * s, std::max(300 * s, ch - 12 * s));
    v.FillRound(m_panel, 8 * s, kPanel, 0.96f);
    v.EditBtn(EditCmd::Panel, 0, m_panel, nullptr);
    m_clip = m_panel;
    v.Text(d.name, v.m_tfTitle.Get(), D2D1::RectF(m_panel.left + pad, m_panel.top + 8 * s, m_panel.right - pad, m_panel.top + 32 * s), kWhite, 1);
    const std::wstring pluginName = d.plugin->Name();
    const bool sub = pluginName != d.name;  // nom du plugin s'il diffère de celui du mode
    if (sub)
        v.Text(pluginName, v.m_tfSub.Get(), D2D1::RectF(m_panel.left + pad, m_panel.top + 32 * s, m_panel.right - pad, m_panel.top + 50 * s),
               kGrey, 0.9f);
    m_scrollRc = D2D1::RectF(m_panel.left, m_panel.top + (sub ? 56 : 38) * s, m_panel.right, m_panel.bottom - 6 * s);
    const float viewH = m_scrollRc.bottom - m_scrollRc.top;
    m_scroll = std::clamp(m_scroll, 0.0f, std::max(0.0f, m_contentH - viewH));
    m_clip = m_scrollRc;
    rt->PushAxisAlignedClip(m_scrollRc, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = m_scrollRc.top - m_scroll;
    const float l = m_panel.left + pad, r = m_panel.right - pad;
    m_track.assign(d.controls.size(), D2D1::RectF(0, 0, 0, 0));
    for (size_t k = 0; k < d.controls.size(); ++k) {
        if (!Visible(k)) continue;
        const editplug::Param& p = d.controls[k];
        const double val = k < m_vals.size() ? m_vals[k] : p.p.def;
        const int id = int(k) * 64;
        switch (p.p.type) {
        case CYIMG_PARAM_SECTION: {
            const D2D1_RECT_F head = D2D1::RectF(m_panel.left + 4 * s, y, m_panel.right - 4 * s, y + 32 * s);
            if (InClip(head)) {
                v.SetColor(kWhite, 0.07f);
                rt->FillRectangle(D2D1::RectF(l, head.top, r, head.top + 1), v.m_brush.Get());
                Btn(id, head, nullptr);
                v.Text(m_open[k] ? L"" : L"", v.m_tfIcon.Get(), D2D1::RectF(l - 4 * s, head.top, l + 16 * s, head.bottom), kGrey, 0.9f);
                v.Text(p.label, v.m_tfTitle.Get(), D2D1::RectF(l + 20 * s, head.top, r, head.bottom), kWhite, 0.95f);
            }
            y = head.bottom + 2 * s;
            break;
        }
        case CYIMG_PARAM_SLIDER: {
            // Libellé à gauche de la piste, ou au-dessus s'il est trop long
            const float labelW = 118 * s, valW = 50 * s;
            const bool above = TextWidth(p.label) > labelW - 6 * s;
            const float rowH = above ? 44 * s : 26 * s, trackY = above ? y + 31 * s : y + 13 * s;
            const D2D1_RECT_F row = D2D1::RectF(l - 4 * s, y, r + 4 * s, y + rowH);
            y += rowH;
            if (!InClip(row)) break;
            m_track[k] = D2D1::RectF(above ? l : l + labelW, trackY - 8 * s, r - valW - 4 * s, trackY + 8 * s);
            const D2D1_RECT_F hit = D2D1::RectF(row.left, std::max(row.top, m_clip.top), row.right, std::min(row.bottom, m_clip.bottom));
            if (hit.bottom > hit.top) v.m_editButtons.push_back({EditCmd::ModeCtl, id, hit, nullptr});
            const bool changed = val != p.p.def;
            v.Text(p.label, v.m_tfSmall.Get(), above ? D2D1::RectF(l, row.top + 2 * s, r, row.top + 20 * s)
                                                    : D2D1::RectF(l, row.top, l + labelW - 4 * s, row.bottom),
                   kWhite, changed ? 1.0f : 0.8f);
            const D2D1_RECT_F& tr = m_track[k];
            const float cy = (tr.top + tr.bottom) / 2;
            const float t = p.p.max > p.p.min ? float((val - p.p.min) / (p.p.max - p.p.min)) : 0;
            const float kx = tr.left + std::clamp(t, 0.0f, 1.0f) * (tr.right - tr.left);
            const D2D1_RECT_F bar = D2D1::RectF(tr.left, cy - 2.5f * s, tr.right, cy + 2.5f * s);
            v.FillRound(bar, 2 * s, kWhite, 0.16f);
            const float zt = p.p.min < 0 && p.p.max > 0 ? float(-p.p.min / (p.p.max - p.p.min))
                                                        : float((p.p.def - p.p.min) / std::max(1e-9, p.p.max - p.p.min));
            const float zx = tr.left + std::clamp(zt, 0.0f, 1.0f) * (tr.right - tr.left);
            v.FillRound(D2D1::RectF(std::min(zx, kx), bar.top, std::max(zx, kx), bar.bottom), 2 * s, kAccent, 0.9f);
            const bool hot = (m_drag == kDragSlider && m_dragId == int(k)) ||
                             (v.m_editHover && v.m_editHover->cmd == EditCmd::ModeCtl && v.m_editHover->arg == id);
            const float kr = (hot ? 7.0f : 6.0f) * s;
            v.SetColor(0x000000, 0.5f);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), kr + 1.2f * s, kr + 1.2f * s), v.m_brush.Get());
            v.SetColor(changed ? kWhite : 0xC8CCD2, 1);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), kr, kr), v.m_brush.Get());
            v.Text(FormatParam(p, val), v.m_tfCenter.Get(), D2D1::RectF(r - valW, trackY - 13 * s, r, trackY + 13 * s), changed ? kAccent : kWhite,
                   changed ? 1.0f : 0.7f);
            break;
        }
        case CYIMG_PARAM_CHECK: {
            const D2D1_RECT_F row = D2D1::RectF(l - 4 * s, y + 1 * s, r, y + 29 * s);
            y += 30 * s;
            if (!InClip(row)) break;
            Btn(id, row, nullptr);
            const D2D1_RECT_F box = D2D1::RectF(l, row.top + 6 * s, l + 16 * s, row.top + 22 * s);
            if (val != 0) v.FillRound(box, 3 * s, kAccent, 1);
            v.SetColor(val != 0 ? kAccent : kWhite, val != 0 ? 1.0f : 0.6f);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 3 * s, 3 * s), v.m_brush.Get(), 1.5f * s);
            if (val != 0) v.Text(L"", v.m_tfIcon.Get(), box, 0x0B1A2A, 1);
            v.Text(p.label, v.m_tfSmall.Get(), D2D1::RectF(box.right + 8 * s, row.top, r, row.bottom), kWhite, 0.95f);
            break;
        }
        case CYIMG_PARAM_CHOICE: {
            if (!p.label.empty()) {
                const D2D1_RECT_F lr = D2D1::RectF(l, y + 2 * s, r, y + 24 * s);
                if (InClip(lr)) v.Text(p.label, v.m_tfSmall.Get(), lr, kGrey, 1);
                y += 24 * s;
            }
            const int n = int(p.choices.size()), per = std::min(3, std::max(1, n));
            const float bw = (r - l - float(per - 1) * 4 * s) / float(per);
            for (int c = 0; c < n; ++c) {
                const float bx = l + float(c % per) * (bw + 4 * s), by = y + float(c / per) * 30 * s;
                const D2D1_RECT_F b = D2D1::RectF(bx, by, bx + bw, by + 26 * s);
                if (InClip(b)) TextButton(id + c + 1, b, p.choices[size_t(c)], nullptr, int(std::lround(val)) == c);
            }
            y += float((n + per - 1) / per) * 30 * s + 2 * s;
            break;
        }
        case CYIMG_PARAM_COLOR: {
            const D2D1_RECT_F row = D2D1::RectF(l, y + 2 * s, r, y + 30 * s);
            y += 32 * s;
            if (!InClip(row)) break;
            v.Text(p.label, v.m_tfSmall.Get(), D2D1::RectF(l, row.top, r - 60 * s, row.bottom), kWhite, 0.95f);
            const D2D1_RECT_F sw = D2D1::RectF(r - 52 * s, row.top + 3 * s, r, row.bottom - 3 * s);
            Btn(id, sw, T("editapi.tip.color"));
            v.SetColor(uint32_t(val), 1);
            rt->FillRoundedRectangle(D2D1::RoundedRect(sw, 4 * s, 4 * s), v.m_brush.Get());
            v.SetColor(kWhite, 0.5f);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(sw, 4 * s, 4 * s), v.m_brush.Get(), 1);
            break;
        }
        case CYIMG_PARAM_BUTTON: {
            // Boutons consécutifs : deux par ligne
            size_t first = k;
            while (first > 0 && d.controls[first - 1].p.type == CYIMG_PARAM_BUTTON && Visible(first - 1)) --first;
            const size_t pos = k - first;
            const bool lastInRow = pos % 2 == 1 || k + 1 >= d.controls.size() || d.controls[k + 1].p.type != CYIMG_PARAM_BUTTON || !Visible(k + 1);
            const float bw = (r - l - 6 * s) / 2;
            const float bx = pos % 2 ? l + bw + 6 * s : l;
            const D2D1_RECT_F b = D2D1::RectF(bx, y + 2 * s, bx + bw, y + 30 * s);
            if (InClip(b)) TextButton(id, b, p.label, nullptr);
            if (lastInRow) y += 34 * s;
            break;
        }
        case CYIMG_PARAM_LABEL: {
            ComPtr<IDWriteTextLayout> lay;
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(v.m_dwrite->CreateTextLayout(p.label.c_str(), UINT32(p.label.size()), v.m_tfSmall.Get(), r - l, 1000, &lay))) {
                lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                lay->GetMetrics(&tm);
                const D2D1_RECT_F lr = D2D1::RectF(l, y + 2 * s, r, y + 2 * s + tm.height);
                if (InClip(lr)) {
                    v.SetColor(kGrey, 0.9f);
                    rt->DrawTextLayout(D2D1::Point2F(l, y + 2 * s), lay.Get(), v.m_brush.Get());
                }
            }
            y += tm.height + 8 * s;
            break;
        }
        case CYIMG_PARAM_SEPARATOR:
            v.SetColor(kWhite, 0.08f);
            rt->FillRectangle(D2D1::RectF(l, y + 4 * s, r, y + 4 * s + 1), v.m_brush.Get());
            y += 9 * s;
            break;
        default: break;
        }
    }
    m_contentH = y + m_scroll - m_scrollRc.top + 8 * s;
    rt->PopAxisAlignedClip();
    m_clip = m_panel;
    if (m_contentH > viewH) {
        const float th = std::max(24 * s, viewH * viewH / m_contentH);
        const float ty = m_scrollRc.top + (viewH - th) * m_scroll / std::max(1.0f, m_contentH - viewH);
        m_thumbRc = D2D1::RectF(m_panel.right - 7 * s, ty, m_panel.right - 2 * s, ty + th);
        v.FillRound(m_thumbRc, 2 * s, kWhite, m_drag == kDragScroll ? 0.5f : 0.28f);
        Btn(kCtlScroll, D2D1::RectF(m_thumbRc.left - 3 * s, m_thumbRc.top, m_thumbRc.right + 1 * s, m_thumbRc.bottom), nullptr);
    } else {
        m_thumbRc = D2D1::RectF(0, 0, 0, 0);
    }
}

void Viewer::PluginEditMode::Control(int id, POINT p, bool right, bool dbl) {
    editplug::Mode& d = Def();
    if (right) return;
    switch (id) {
    case kCtlUndo:
        if (!m_undo.empty()) {
            m_redo.push_back(m_vals);
            m_vals = m_undo.back();
            m_undo.pop_back();
            m_lastPushId = -1;
            Changed();
        }
        return;
    case kCtlRedo:
        if (!m_redo.empty()) {
            m_undo.push_back(m_vals);
            m_vals = m_redo.back();
            m_redo.pop_back();
            m_lastPushId = -1;
            Changed();
        }
        return;
    case kCtlReset:
        if (m_vals != editplug::Defaults(d.controls)) {
            PushUndo();
            m_vals = editplug::Defaults(d.controls);
            Changed();
        }
        return;
    case kCtlOriginal:
        m_original = !m_original;
        v.Invalidate();
        return;
    case kCtlScroll:
        m_drag = kDragScroll;
        m_dragStart = p;
        m_dragVal = m_scroll;
        v.m_edit.drawing = true;
        SetCapture(v.m_hwnd);
        return;
    default: break;
    }
    const size_t k = size_t(id / 64);
    const int sub = id % 64;
    if (k >= d.controls.size() || k >= m_vals.size()) return;
    const editplug::Param& c = d.controls[k];
    switch (c.p.type) {
    case CYIMG_PARAM_SECTION: m_open[k] = !m_open[k]; break;
    case CYIMG_PARAM_SLIDER: {
        if (dbl) {
            if (m_vals[k] != c.p.def) {
                PushUndo();
                SetValue(k, c.p.def);
            }
            return;
        }
        const D2D1_RECT_F& tr = m_track[k];
        if (p.x < tr.left - 8 * v.m_scale || p.x > tr.right + 8 * v.m_scale) return;
        PushUndo();
        const float kx = tr.left + float((m_vals[k] - c.p.min) / std::max(1e-9, c.p.max - c.p.min)) * (tr.right - tr.left);
        if (std::fabs(p.x - kx) > 8 * v.m_scale)  // clic sur la piste : le bouton y saute
            SetValue(k, c.p.min + std::clamp((p.x - tr.left) / std::max(1.0f, tr.right - tr.left), 0.0f, 1.0f) * (c.p.max - c.p.min));
        m_drag = kDragSlider;
        m_dragId = int(k);
        m_dragStart = p;
        m_dragVal = m_vals[k];
        v.m_edit.drawing = true;
        SetCapture(v.m_hwnd);
        break;
    }
    case CYIMG_PARAM_CHECK:
        PushUndo();
        SetValue(k, m_vals[k] != 0 ? 0 : 1);
        break;
    case CYIMG_PARAM_CHOICE:
        if (sub > 0 && double(sub - 1) != m_vals[k]) {
            PushUndo();
            SetValue(k, sub - 1);
        }
        break;
    case CYIMG_PARAM_COLOR: {
        double col = m_vals[k];
        if (ChooseRgb(v.m_hwnd, col) && col != m_vals[k]) {
            PushUndo();
            SetValue(k, col);
        }
        break;
    }
    case CYIMG_PARAM_BUTTON:
        if (d.api.on_button) {
            std::vector<double> next = m_vals;
            CyImgEditCtx ctx = Ctx();
            editplug::Crash crash;
            if (!editplug::Guard([&] { d.api.on_button(&PlugHost::table, &ctx, uint32_t(k), next.data()); }, crash)) return Crashed(crash);
            for (size_t i = 0; i < next.size(); ++i) next[i] = Quantize(d.controls[i], next[i]);
            if (next != m_vals) {
                PushUndo();
                m_vals = std::move(next);
                Changed();
            }
        }
        break;
    default: break;
    }
    v.Invalidate();
}

bool Viewer::PluginEditMode::MouseDown(POINT p, bool right, bool dbl) {
    if (m_drag != kDragNone) return true;
    if (const EditButton* b = v.HitEdit(p)) {
        switch (b->cmd) {
        case EditCmd::ModeCtl: Control(b->arg, p, right, dbl); return true;
        case EditCmd::Done: case EditCmd::SaveAs: case EditCmd::OpenWith:
            if (!right) v.EditCommand(b->cmd, 0);
            return true;
        default: return true;  // panneau
        }
    }
    return false;  // déplacement de la vue
}

void Viewer::PluginEditMode::MouseMove(POINT p) {
    if (m_drag == kDragSlider) {
        const auto& c = Def().controls;
        if (m_dragId < 0 || size_t(m_dragId) >= c.size()) return;
        const editplug::Param& prm = c[size_t(m_dragId)];
        const D2D1_RECT_F& tr = m_track[size_t(m_dragId)];
        const double k = GetKeyState(VK_SHIFT) < 0 ? 0.1 : 1.0;  // Maj : réglage fin
        SetValue(size_t(m_dragId), m_dragVal + (p.x - m_dragStart.x) * k * (prm.p.max - prm.p.min) / std::max(1.0f, tr.right - tr.left));
    } else if (m_drag == kDragScroll) {
        const float viewH = m_scrollRc.bottom - m_scrollRc.top, th = m_thumbRc.bottom - m_thumbRc.top;
        m_scroll = std::clamp(float(m_dragVal) + (p.y - m_dragStart.y) * (m_contentH - viewH) / std::max(1.0f, viewH - th), 0.0f,
                              std::max(0.0f, m_contentH - viewH));
        v.Invalidate();
    }
}

void Viewer::PluginEditMode::MouseUp(POINT) {
    if (m_drag == kDragNone) return;
    const bool slider = m_drag == kDragSlider;
    m_drag = kDragNone;
    v.m_edit.drawing = false;
    if (GetCapture() == v.m_hwnd) ReleaseCapture();
    if (slider) m_changeTime = NowSeconds();  // rendu complet peu après le relâchement
    v.Invalidate();
}

bool Viewer::PluginEditMode::Wheel(POINT p, double notches) {
    if (!Contains(m_panel, p)) return false;
    if (const EditButton* b = v.HitEdit(p); b && b->cmd == EditCmd::ModeCtl && b->arg < kCtlUndo && b->arg % 64 == 0) {
        const size_t k = size_t(b->arg / 64);
        const auto& c = Def().controls;
        if (k < c.size() && c[k].p.type == CYIMG_PARAM_SLIDER && p.x >= m_track[k].left - 8 * v.m_scale) {
            double step = c[k].p.step > 0 ? c[k].p.step : (c[k].p.max - c[k].p.min) / 100;
            if (GetKeyState(VK_SHIFT) < 0) step *= 5;
            PushUndo(int(k));
            SetValue(k, m_vals[k] + notches * step);
            return true;
        }
    }
    if (Contains(m_scrollRc, p)) {
        m_scroll -= float(notches) * 60 * v.m_scale;
        v.Invalidate();
    }
    return true;
}

bool Viewer::PluginEditMode::Key(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl) {
        switch (vk) {
        case 'Z': Control(shift ? kCtlRedo : kCtlUndo, POINT{}, false, false); return true;
        case 'Y': Control(kCtlRedo, POINT{}, false, false); return true;
        case 'S': v.SaveAs(); return true;
        case 'C': {
            const bool on = BeginExport();
            v.CopyToClipboard();
            if (on) EndExport();
            return true;
        }
        default: return false;
        }
    }
    switch (vk) {
    case VK_SPACE: v.m_edit.spacePan = true; return true;
    case VK_ESCAPE:
        if (m_drag != kDragNone) MouseUp(v.m_mouse);
        else if (m_original) m_original = false;
        else v.EndEdit(true);
        v.Invalidate();
        return true;
    case 'E': v.EndEdit(true); return true;
    default: return false;
    }
}

bool Viewer::PluginEditMode::Char(wchar_t c) {
    if (c != L'\\') return false;
    Control(kCtlOriginal, POINT{}, false, false);
    return true;
}

const wchar_t* Viewer::PluginEditMode::Cursor(POINT p) const {
    if (m_drag == kDragSlider) return IDC_SIZEWE;
    if (m_drag == kDragScroll || v.m_dragging || v.m_edit.spacePan) return IDC_SIZEALL;
    if (const EditButton* b = v.HitEdit(p)) {
        if (b->cmd == EditCmd::Panel) return IDC_ARROW;
        if (b->cmd == EditCmd::ModeCtl && b->arg < kCtlUndo && b->arg % 64 == 0) {
            const size_t k = size_t(b->arg / 64);
            const auto& c = Def().controls;
            if (k < c.size() && c[k].p.type == CYIMG_PARAM_SLIDER)
                return p.x >= m_track[k].left - 8 * v.m_scale && p.x <= m_track[k].right + 8 * v.m_scale ? IDC_SIZEWE : IDC_ARROW;
        }
        return IDC_HAND;
    }
    return IDC_SIZEALL;
}

// =============================================================================================
// Aide : liste des plugins d'édition

float Viewer::EditPluginsHelpHeight(float rowH) {
    const size_t n = editplug::Get().plugins.size();
    return 40 * m_scale + rowH * float((std::max<size_t>(1, n) + 1) / 2);
}

void Viewer::DrawEditPluginsHelp(float left, float right, float y, float rowH, float col) {
    const auto& reg = editplug::Get();
    const float s = m_scale, half = (right - left) / 2;
    Text(Tf("editapi.help.title", int(reg.plugins.size())), m_tfTitle.Get(), D2D1::RectF(left, y, right, y + 24 * s), kWhite, 1);
    y += 28 * s;
    if (reg.plugins.empty()) {
        Text(T("editapi.help.none"), m_tfSmall.Get(), D2D1::RectF(left, y, right, y + rowH), kGrey, 1);
        return;
    }
    const size_t rows = (reg.plugins.size() + 1) / 2;
    for (size_t i = 0; i < reg.plugins.size(); ++i) {
        const editplug::Plugin& p = *reg.plugins[i];
        const float x = left + (i < rows ? 0 : half + 12 * s), yy = y + rowH * float(i % rows);
        std::wstring what;
        auto add = [&](const char* key, uint32_t n) {
            if (!n) return;
            if (!what.empty()) what += L"  ·  ";
            what += Tf(key, int(n));
        };
        auto count = [&](auto& list) {
            uint32_t n = 0;
            for (const auto& item : list) n += item.plugin == &p ? 1u : 0u;
            return n;
        };
        add("editapi.help.effects", count(reg.effects));
        add("editapi.help.tools", count(reg.tools));
        add("editapi.help.modes", count(reg.modes));
        add("editapi.help.actions", count(reg.actions));
        Text(p.Name(), m_tfSmall.Get(), D2D1::RectF(x, yy, x + col - 8 * s, yy + rowH), kAccent, 1);
        Text(what, m_tfSmall.Get(), D2D1::RectF(x + col, yy, x + half - 12 * s, yy + rowH), kGrey, 1);
    }
}
