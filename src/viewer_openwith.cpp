// « Ouvrir avec » : menu des applications (configurées, associées par Windows, fournies par les plugins),
// lancement, réglages du panneau Paramètres et export PNG temporaire pour le mode édition.

#include "viewer.h"
#include "viewer_util.h"
#include "i18n.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>

using namespace ui;

namespace {

// Identifiants relatifs à OpenWithMenu::first
constexpr UINT kMaxItems = 500, kCmdOther = 500, kCmdConfigure = 501;
constexpr size_t kMaxApps = 100;

// Lignes du panneau Paramètres (les réglages de viewer_settings.cpp restent sous 1000)
enum OwSetting { kOwRow0 = 1000, kOwAdd = 2000, kOwEdit, kOwRemove, kOwUp, kOwDown };

struct Provider {
    int id;
    OpenWithProvider fn;
};
std::vector<Provider>& Providers() {
    static std::vector<Provider> p;
    return p;
}

bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring StemOf(const std::wstring& path) {
    std::wstring n = FileNameOf(path);
    const size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? n : n.substr(0, dot);
}

bool IsSelf(const std::wstring& exe) { return _wcsicmp(FileNameOf(exe).c_str(), L"CyImgView.exe") == 0; }

std::wstring ErrorText(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                   reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    if (s.empty()) s = L"0x" + std::to_wstring(err);
    return s;
}

// « & » est un préfixe d'accélérateur dans les menus Win32
std::wstring MenuText(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'&') r += L'&';
        r += c;
    }
    return r;
}

// Description du programme (ressource de version « FileDescription »), sinon nom de l'exe.
std::wstring ProgramName(const std::wstring& exe) {
    DWORD handle = 0;
    const DWORD n = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (n) {
        std::vector<uint8_t> buf(n);
        if (GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) {
            struct LangCp {
                WORD lang, cp;
            };
            std::vector<LangCp> tries;
            LangCp* tr = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len))
                tries.assign(tr, tr + len / sizeof(LangCp));
            tries.push_back({0x0409, 0x04B0});
            tries.push_back({0x0409, 0x04E4});
            for (const LangCp& t : tries) {
                wchar_t key[64];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", t.lang, t.cp);
                wchar_t* val = nullptr;
                UINT vl = 0;
                if (VerQueryValueW(buf.data(), key, reinterpret_cast<void**>(&val), &vl) && val && vl > 1) {
                    std::wstring d(val);
                    while (!d.empty() && iswspace(d.back())) d.pop_back();
                    if (!d.empty()) return d;
                }
            }
        }
    }
    return StemOf(exe);
}

// --- Icônes : pixels BGRA prémultipliés, carrés de size pixels ---

ComPtr<IWICImagingFactory> Wic() {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool SourcePixels(IWICImagingFactory* wic, IWICBitmapSource* src, int size, std::vector<uint8_t>& out) {
    UINT w = 0, h = 0;
    if (!src || FAILED(src->GetSize(&w, &h)) || !w || !h) return false;
    ComPtr<IWICBitmapSource> cur = src;
    if (int(w) != size || int(h) != size) {
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(src, UINT(size), UINT(size), WICBitmapInterpolationModeFant)))
            return false;
        cur = sc;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(cur.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    out.resize(size_t(size) * size * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, UINT(size) * 4, UINT(out.size()), out.data()));
}

// Icône d'un fichier (exe, dll + index) ou ressource d'application du Store (« @{...} »)
bool IconPixels(const std::wstring& file, int index, int size, std::vector<uint8_t>& out) {
    if (file.empty()) return false;
    ComPtr<IWICImagingFactory> wic = Wic();
    if (!wic) return false;
    if (file[0] == L'@') {
        wchar_t res[MAX_PATH * 2] = {};
        if (FAILED(SHLoadIndirectString(file.c_str(), res, ARRAYSIZE(res), nullptr))) return false;
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> frame;
        return SUCCEEDED(wic->CreateDecoderFromFilename(res, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
               SUCCEEDED(dec->GetFrame(0, &frame)) && SourcePixels(wic.Get(), frame.Get(), size, out);
    }
    wchar_t expanded[MAX_PATH * 2] = {};
    const std::wstring path = ExpandEnvironmentStringsW(file.c_str(), expanded, ARRAYSIZE(expanded)) ? expanded : file;
    // Icône principale vue par l'Explorateur : gère aussi les icônes déplacées dans SystemResources\*.mun
    // (notepad.exe de Windows 10 récents n'a plus d'icône dans l'exe)
    auto shellIcon = [&] {
        ComPtr<IShellItemImageFactory> factory;
        HBITMAP hb = nullptr;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->GetImage(SIZE{size, size}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)))
            return false;
        ComPtr<IWICBitmap> bmp;
        const bool ok = SUCCEEDED(wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, &bmp)) &&
                        SourcePixels(wic.Get(), bmp.Get(), size, out);
        DeleteObject(hb);
        return ok;
    };
    if (index == 0 && shellIcon()) return true;
    HICON icon = nullptr;
    if (SHDefExtractIconW(path.c_str(), index, 0, &icon, nullptr, MAKELONG(size, size)) != S_OK) icon = nullptr;
    if (!icon) return index != 0 && shellIcon();
    ComPtr<IWICBitmap> bmp;
    const bool ok = SUCCEEDED(wic->CreateBitmapFromHICON(icon, &bmp)) && SourcePixels(wic.Get(), bmp.Get(), size, out);
    DestroyIcon(icon);
    return ok;
}

bool HIconPixels(HICON icon, int size, std::vector<uint8_t>& out) {
    ComPtr<IWICImagingFactory> wic = Wic();
    ComPtr<IWICBitmap> bmp;
    return icon && wic && SUCCEEDED(wic->CreateBitmapFromHICON(icon, &bmp)) && SourcePixels(wic.Get(), bmp.Get(), size, out);
}

// HBITMAP 32 bits ARGB prémultiplié (ce qu'attendent les menus Win32 avec MIIM_BITMAP)
HBITMAP MenuBitmap(const std::vector<uint8_t>& px, int size) {
    if (px.size() != size_t(size) * size * 4) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;  // lignes de haut en bas
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP h = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (h && bits) memcpy(bits, px.data(), px.size());
    return h;
}

bool InvokeHandler(IAssocHandler* h, const std::wstring& path) {
    ComPtr<IShellItem> item;
    ComPtr<IDataObject> data;
    return SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
           SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))) && SUCCEEDED(h->Invoke(data.Get()));
}

// Valeur d'ini entourée de guillemets : GetPrivateProfileString retire une paire de guillemets englobants,
// ce qui abîmerait des arguments comme "%1".
void PutIni(const std::wstring& ini, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"openwith", key, (L"\"" + v + L"\"").c_str(), ini.c_str());
}
std::wstring GetIni(const std::wstring& ini, const wchar_t* key, const wchar_t* def) {
    std::vector<wchar_t> buf(4096);
    GetPrivateProfileStringW(L"openwith", key, def, buf.data(), DWORD(buf.size()), ini.c_str());
    return buf.data();
}

// --- Petite boîte de dialogue « nom / programme / arguments » (modèle en mémoire, thème sombre) ---

enum { kIdName = 101, kIdPath, kIdArgs };

struct EditDlgData {
    OpenWithEntry* e;
    HBRUSH bg;
};

INT_PTR CALLBACK EditDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<EditDlgData*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        d = reinterpret_cast<EditDlgData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(dlg, 20, &dark, sizeof(dark));
        SetWindowTextW(dlg, T("openwith.dlg.edit_title"));
        SetDlgItemTextW(dlg, kIdName, d->e->name.c_str());
        SetDlgItemTextW(dlg, kIdPath, d->e->path.c_str());
        SetDlgItemTextW(dlg, kIdArgs, d->e->args.c_str());
        for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            wchar_t cls[16] = {};
            GetClassNameW(c, cls, ARRAYSIZE(cls));
            SetWindowTheme(c, _wcsicmp(cls, L"Edit") == 0 ? L"DarkMode_CFD" : L"DarkMode_Explorer", nullptr);
        }
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(0xF0, 0xF0, 0xF0));
        SetBkColor(dc, msg == WM_CTLCOLOREDIT ? RGB(0x2B, 0x2C, 0x31) : RGB(0x1C, 0x1D, 0x21));
        if (msg == WM_CTLCOLOREDIT) {
            static HBRUSH editBg = CreateSolidBrush(RGB(0x2B, 0x2C, 0x31));
            return reinterpret_cast<INT_PTR>(editBg);
        }
        return reinterpret_cast<INT_PTR>(d ? d->bg : GetStockObject(BLACK_BRUSH));
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            auto get = [&](int id) {
                std::wstring s(size_t(GetWindowTextLengthW(GetDlgItem(dlg, id))) + 1, L'\0');
                s.resize(size_t(GetDlgItemTextW(dlg, id, s.data(), int(s.size()))));
                return s;
            };
            std::wstring name = get(kIdName), path = get(kIdPath);
            if (path.empty()) {
                SetFocus(GetDlgItem(dlg, kIdPath));
                return TRUE;
            }
            d->e->name = name.empty() ? ProgramName(path) : name;
            d->e->path = path;
            d->e->args = get(kIdArgs);
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

class DlgTemplate {
public:
    void Word(WORD w) { m_buf.push_back(w); }
    void Dword(DWORD d) {
        Word(LOWORD(d));
        Word(HIWORD(d));
    }
    void Str(const wchar_t* s) {
        while (*s) Word(WORD(*s++));
        Word(0);
    }
    void Align() {
        if (m_buf.size() % 2) Word(0);
    }
    void Item(DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        Align();
        Dword(style | WS_CHILD | WS_VISIBLE);
        Dword(0);
        Word(WORD(x));
        Word(WORD(y));
        Word(WORD(cx));
        Word(WORD(cy));
        Word(id);
        Word(0xFFFF);
        Word(cls);
        Str(text);
        Word(0);
    }
    const DLGTEMPLATE* Get() const { return reinterpret_cast<const DLGTEMPLATE*>(m_buf.data()); }
    std::vector<WORD> m_buf;
};

bool EditEntryDialog(HWND owner, OpenWithEntry& e) {
    constexpr WORD kButton = 0x0080, kEdit = 0x0081, kStatic = 0x0082;
    const short W = 300, H = 132;
    DlgTemplate t;
    t.Dword(DS_SETFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t.Dword(0);
    t.Word(8);  // nombre de contrôles
    t.Word(0);
    t.Word(0);
    t.Word(WORD(W));
    t.Word(WORD(H));
    t.Word(0);  // pas de menu
    t.Word(0);  // classe par défaut
    t.Str(L"");
    t.Word(9);
    t.Str(L"Segoe UI");
    t.Item(SS_LEFT, 8, 8, W - 16, 9, 0xFFFF, kStatic, T("openwith.dlg.name"));
    t.Item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 18, W - 16, 13, kIdName, kEdit, L"");
    t.Item(SS_LEFT, 8, 38, W - 16, 9, 0xFFFF, kStatic, T("openwith.dlg.path"));
    t.Item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 48, W - 16, 13, kIdPath, kEdit, L"");
    t.Item(SS_LEFT, 8, 68, W - 16, 9, 0xFFFF, kStatic, T("openwith.dlg.args"));
    t.Item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 78, W - 16, 13, kIdArgs, kEdit, L"");
    t.Item(BS_DEFPUSHBUTTON | WS_TABSTOP, W - 8 - 2 * 60 - 6, H - 8 - 15, 60, 15, IDOK, kButton, T("openwith.dlg.ok"));
    t.Item(BS_PUSHBUTTON | WS_TABSTOP, W - 8 - 60, H - 8 - 15, 60, 15, IDCANCEL, kButton, T("openwith.dlg.cancel"));
    OpenWithEntry copy = e;
    EditDlgData data{&copy, CreateSolidBrush(RGB(0x1C, 0x1D, 0x21))};
    const INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.Get(), owner, EditDlgProc, reinterpret_cast<LPARAM>(&data));
    DeleteObject(data.bg);
    if (r != IDOK) return false;
    e = copy;
    return true;
}

}  // namespace

// =============================================================================================
// Registre des fournisseurs (plugins), export temporaire

int RegisterOpenWithProvider(OpenWithProvider provider) {
    static int next = 1;
    Providers().push_back({next, std::move(provider)});
    return next++;
}

void UnregisterOpenWithProvider(int id) {
    auto& p = Providers();
    p.erase(std::remove_if(p.begin(), p.end(), [&](const Provider& x) { return x.id == id; }), p.end());
}

// Confidentialité : les images modifiées exportées pour « Ouvrir avec » ne restent pas indéfiniment dans
// %TEMP%\CyImgView (supprimées au démarrage et à chaque export au-delà de maxAgeHours).
void CleanTempExports(int maxAgeHours) {
    wchar_t tmp[MAX_PATH + 1] = {};
    if (!GetTempPathW(ARRAYSIZE(tmp), tmp)) return;
    const std::wstring dir = std::wstring(tmp) + L"CyImgView";
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG nowT = (ULONGLONG(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const ULONGLONG maxAge = ULONGLONG(maxAgeHours) * 3600ull * 10000000ull;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const ULONGLONG t = (ULONGLONG(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (nowT > t && nowT - t > maxAge) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());  // échoue sans gravité si ouvert
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring WriteTempPng(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t stride, bool hasAlpha,
                          const std::wstring& imageName, std::wstring& error) {
    CleanTempExports(24);
    wchar_t tmp[MAX_PATH + 1] = {};
    if (!GetTempPathW(ARRAYSIZE(tmp), tmp)) {
        error = ErrorText(GetLastError());
        return {};
    }
    const std::wstring dir = std::wstring(tmp) + L"CyImgView";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring stem = StemOf(imageName);
    if (stem.empty()) stem = L"image";
    const std::vector<SaveFormat> formats = EnumerateSaveFormats();
    for (size_t i = 0; i < formats.size(); ++i) {
        if (formats[i].container != GUID_ContainerFormatPng) continue;
        SaveRequest req;
        req.path = dir + L"\\" + stem + T("openwith.temp_suffix") + L".png";
        req.format = int(i);
        if (!SaveImage(req, formats[i], pixels, width, height, stride, hasAlpha, error)) return {};
        return req.path;
    }
    error = L"PNG";
    return {};
}

// =============================================================================================
// Menu

Viewer::OpenWithMenu::~OpenWithMenu() {
    for (HBITMAP b : bitmaps) DeleteObject(b);
}

bool Viewer::OpenWithTestMode() { return GetEnvironmentVariableW(L"CYIMG_OPENWITH_DUMP", nullptr, 0) > 0; }

UINT Viewer::OpenWithTestCmd(const OpenWithMenu& m) {
    wchar_t v[16] = {};
    if (!GetEnvironmentVariableW(L"CYIMG_OPENWITH_RUN", v, ARRAYSIZE(v))) return 0;
    return m.first + UINT(_wtoi(v));
}

void Viewer::EnsureOpenWith() {
    if (m_openWithLoaded) return;
    m_openWithLoaded = true;
    if (m_iniPath.empty()) return;
    const int n = std::clamp(int(GetPrivateProfileIntW(L"openwith", L"count", 0, m_iniPath.c_str())), 0, int(kMaxApps));
    for (int i = 0; i < n; ++i) {
        const std::wstring k = L"app" + std::to_wstring(i) + L"_";
        OpenWithEntry e;
        e.path = GetIni(m_iniPath, (k + L"path").c_str(), L"");
        if (e.path.empty()) continue;
        e.name = GetIni(m_iniPath, (k + L"name").c_str(), L"");
        if (e.name.empty()) e.name = ProgramName(e.path);
        const std::wstring args = GetIni(m_iniPath, (k + L"args").c_str(), L"\x01");
        if (args != L"\x01") e.args = args;  // clé absente : "%1"
        m_openWith.push_back(std::move(e));
    }
}

void Viewer::SaveOpenWith() {
    if (m_iniPath.empty()) return;
    WritePrivateProfileStringW(L"openwith", nullptr, nullptr, m_iniPath.c_str());  // efface la section
    WritePrivateProfileStringW(L"openwith", L"count", std::to_wstring(m_openWith.size()).c_str(), m_iniPath.c_str());
    for (size_t i = 0; i < m_openWith.size(); ++i) {
        const std::wstring k = L"app" + std::to_wstring(i) + L"_";
        PutIni(m_iniPath, (k + L"name").c_str(), m_openWith[i].name);
        PutIni(m_iniPath, (k + L"path").c_str(), m_openWith[i].path);
        PutIni(m_iniPath, (k + L"args").c_str(), m_openWith[i].args);
    }
}

void Viewer::BuildOpenWithMenu(OpenWithMenu& m, const std::wstring& path, UINT firstId) {
    EnsureOpenWith();
    EnsureEditPlugins();  // actions des plugins d'édition (fournisseurs enregistrés au chargement)
    m.first = firstId;
    m.menu = CreatePopupMenu();
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(m_hwnd));
    std::vector<std::wstring> groups;  // pour le fichier de test
    auto add = [&](OpenWithEntry e, const std::vector<uint8_t>& px, const wchar_t* group) {
        if (m.items.size() >= kMaxItems) return;
        HBITMAP bmp = px.empty() ? nullptr : MenuBitmap(px, size);
        const std::wstring text = MenuText(e.name);
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_ID | MIIM_STRING | MIIM_FTYPE | (bmp ? MIIM_BITMAP : 0u);
        mi.fType = MFT_STRING;
        mi.wID = m.first + UINT(m.items.size());
        mi.dwTypeData = const_cast<wchar_t*>(text.c_str());
        mi.hbmpItem = bmp;
        InsertMenuItemW(m.menu, UINT(GetMenuItemCount(m.menu)), TRUE, &mi);
        if (bmp) m.bitmaps.push_back(bmp);
        m.items.push_back(std::move(e));
        groups.push_back(std::wstring(group) + (bmp ? L"+icon" : L""));
    };
    auto separator = [&] {
        if (GetMenuItemCount(m.menu) > 0) AppendMenuW(m.menu, MF_SEPARATOR, 0, nullptr);
    };
    std::vector<uint8_t> px;

    // a. Applications configurées
    for (const OpenWithEntry& e : m_openWith) {
        px.clear();
        if (!(e.icon && HIconPixels(e.icon, size, px))) IconPixels(e.path, 0, size, px);
        add(e, px, L"app");
    }

    // b. Applications que Windows associe à l'extension
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    ComPtr<IEnumAssocHandlers> handlers;
    if (ext && *ext && SUCCEEDED(SHAssocEnumHandlers(ext, ASSOC_FILTER_RECOMMENDED, &handlers))) {
        bool first = true;
        ComPtr<IAssocHandler> h;
        ULONG got = 0;
        while (handlers->Next(1, h.ReleaseAndGetAddressOf(), &got) == S_OK && got) {
            LPWSTR exe = nullptr, ui = nullptr, icon = nullptr;
            int index = 0;
            if (SUCCEEDED(h->GetName(&exe)) && exe && SUCCEEDED(h->GetUIName(&ui)) && ui && !IsSelf(exe) &&
                std::none_of(m_openWith.begin(), m_openWith.end(), [&](const OpenWithEntry& e) { return SamePath(e.path, exe); })) {
                if (first) separator();
                first = false;
                OpenWithEntry e;
                e.name = ui;
                e.path = exe;
                e.args.clear();
                ComPtr<IAssocHandler> keep = h;
                e.run = [keep](const std::wstring& p, HWND) { return InvokeHandler(keep.Get(), p); };
                px.clear();
                if (SUCCEEDED(h->GetIconLocation(&icon, &index)) && icon) IconPixels(icon, index, size, px);
                if (px.empty()) IconPixels(exe, 0, size, px);
                add(std::move(e), px, L"assoc");
            }
            CoTaskMemFree(exe);
            CoTaskMemFree(ui);
            CoTaskMemFree(icon);
        }
    }

    // c. Actions des plugins
    std::vector<OpenWithEntry> plugin;
    for (const Provider& p : std::vector<Provider>(Providers())) p.fn(path, plugin);  // copie : un fournisseur peut se retirer
    if (!plugin.empty()) separator();
    for (OpenWithEntry& e : plugin) {
        px.clear();
        if (!(e.icon && HIconPixels(e.icon, size, px)) && !e.path.empty()) IconPixels(e.path, 0, size, px);
        add(std::move(e), px, L"plugin");
    }

    // d. Autre application, réglages (le panneau Paramètres n'existe pas en mode édition)
    separator();
    AppendMenuW(m.menu, MF_STRING, m.first + kCmdOther, T("openwith.other"));
    if (!m_edit.active) AppendMenuW(m.menu, MF_STRING, m.first + kCmdConfigure, T("openwith.configure"));

    wchar_t dump[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"CYIMG_OPENWITH_DUMP", dump, ARRAYSIZE(dump))) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, dump, L"w, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"file\t%ls\n", path.c_str());
            for (size_t i = 0; i < m.items.size(); ++i) {
                const OpenWithEntry& e = m.items[i];
                fwprintf(f, L"%zu\t%ls\t%ls\t%ls\t%ls\n", i, groups[i].c_str(), e.name.c_str(), e.path.c_str(), e.args.c_str());
            }
            fwprintf(f, L"icons\t%zu\n", m.bitmaps.size());
            fwprintf(f, L"menu_items\t%d\n", GetMenuItemCount(m.menu));
            fclose(f);
        }
    }
}

bool Viewer::OpenWithCommand(OpenWithMenu& m, UINT cmd, const std::wstring& path) {
    if (!cmd || cmd < m.first || cmd > m.first + kCmdConfigure) return false;
    const UINT i = cmd - m.first;
    if (i == kCmdOther) {
        OPENASINFO oi{path.c_str(), nullptr, OAIF_EXEC | OAIF_HIDE_REGISTRATION};
        SHOpenWithDialog(m_hwnd, &oi);  // annulation : rien à signaler
    } else if (i == kCmdConfigure) {
        m_showSettings = true;
        m_showInfo = m_showChannels = m_showHelp = false;
        m_settingsScrollToOpenWith = true;
    } else if (i < m.items.size()) {
        OpenWith(m.items[i], path);
    } else {
        return false;
    }
    Invalidate();
    return true;
}

void Viewer::ShowOpenWithMenu(POINT clientPt, const std::wstring& path) {
    if (path.empty()) {
        ShowToast(T("openwith.err.no_file"));
        return;
    }
    EnableDarkMenus();
    OpenWithMenu m;
    BuildOpenWithMenu(m, path, 1);
    UINT cmd;
    if (OpenWithTestMode()) {
        cmd = OpenWithTestCmd(m);
    } else {
        POINT sp = clientPt;
        ClientToScreen(m_hwnd, &sp);
        SetForegroundWindow(m_hwnd);  // sinon le menu ne se ferme pas en cliquant ailleurs
        cmd = UINT(TrackPopupMenu(m.menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, sp.x, sp.y, 0, m_hwnd, nullptr));
    }
    DestroyMenu(m.menu);
    m.menu = nullptr;
    OpenWithCommand(m, cmd, path);
    Invalidate();
}

void Viewer::ShowOpenWithForCurrent() {
    const FileEntry* cur = Current();
    if (!cur) {
        ShowToast(T("openwith.err.no_file"));
        return;
    }
    POINT pt = ClientCenter();
    for (const ButtonRect& b : m_buttons)  // sous le bouton de la barre d'outils s'il est affiché
        if (b.id == Btn::OpenWith) pt = POINT{LONG(b.rc.left), LONG(b.rc.bottom + 4 * m_scale)};
    ShowOpenWithMenu(pt, cur->path);
}

// =============================================================================================
// Lancement

bool Viewer::OpenWith(const OpenWithEntry& e, const std::wstring& path) {
    if (path.empty()) {
        ShowToast(T("openwith.err.no_file"));
        return false;
    }
    if (e.run) {
        if (e.run(path, m_hwnd)) return true;
        ShowToast(Tf("openwith.err.failed", e.name.c_str()));
        return false;
    }
    // %1 (entre guillemets ou non) devient le chemin entre guillemets ; sans %1, le chemin est ajouté à la fin
    const std::wstring quoted = L"\"" + path + L"\"";
    std::wstring args;
    bool found = false;
    for (size_t i = 0; i < e.args.size();) {
        const size_t n = e.args.compare(i, 4, L"\"%1\"") == 0 ? 4 : e.args.compare(i, 2, L"%1") == 0 ? 2 : 0;
        if (n) {
            args += quoted;
            found = true;
            i += n;
        } else {
            args += e.args[i++];
        }
    }
    if (!found) args += (args.empty() ? L"" : L" ") + quoted;
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + (slash == 2 ? 1 : 0));
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = m_hwnd;
    sei.lpFile = e.path.c_str();
    sei.lpParameters = args.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;
    ShowToast(Tf("openwith.err.launch", e.name.c_str(), ErrorText(GetLastError()).c_str()));
    return false;
}

// =============================================================================================
// Panneau Paramètres : section « Ouvrir avec — applications »

void Viewer::AddOpenWithApp() {
    EnsureOpenWith();
    if (m_openWith.size() >= kMaxApps) return;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const COMDLG_FILTERSPEC spec[] = {{T("openwith.dlg.programs"), L"*.exe"}, {T("openwith.dlg.all_files"), L"*.*"}};
    dlg->SetFileTypes(ARRAYSIZE(spec), spec);
    dlg->SetTitle(T("openwith.dlg.pick_title"));
    FILEOPENDIALOGOPTIONS opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    ComPtr<IShellItem> programs;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&programs))))
        dlg->SetDefaultFolder(programs.Get());
    ComPtr<IShellItem> item;
    PWSTR file = nullptr;
    if (FAILED(dlg->Show(m_hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &file))) return;
    OpenWithEntry e;
    e.path = file;
    CoTaskMemFree(file);
    e.name = ProgramName(e.path);
    m_openWith.push_back(std::move(e));
    m_openWithSel = int(m_openWith.size()) - 1;
    SaveOpenWith();
    Invalidate();
}

void Viewer::EditOpenWithApp(int i) {
    if (i < 0 || i >= int(m_openWith.size())) return;
    if (EditEntryDialog(m_hwnd, m_openWith[size_t(i)])) SaveOpenWith();
    Invalidate();
}

void Viewer::OnOpenWithSetting(int id) {
    EnsureOpenWith();
    const int n = int(m_openWith.size()), sel = m_openWithSel;
    const bool valid = sel >= 0 && sel < n;
    if (id >= kOwRow0 && id < kOwRow0 + n) {
        m_openWithSel = id - kOwRow0;
    } else if (id == kOwAdd) {
        AddOpenWithApp();
    } else if (id == kOwEdit && valid) {
        EditOpenWithApp(sel);
    } else if (id == kOwRemove && valid) {
        m_openWith.erase(m_openWith.begin() + sel);
        m_openWithSel = std::min(sel, int(m_openWith.size()) - 1);
        SaveOpenWith();
    } else if ((id == kOwUp && valid && sel > 0) || (id == kOwDown && valid && sel + 1 < n)) {
        const int to = id == kOwUp ? sel - 1 : sel + 1;
        std::swap(m_openWith[size_t(sel)], m_openWith[size_t(to)]);
        m_openWithSel = to;
        SaveOpenWith();
    }
    Invalidate();
}

ID2D1Bitmap* Viewer::OpenWithIcon(const std::wstring& exe) {
    if (m_openWithIconsRt.Get() != m_rt.Get()) {  // nouveau device : les bitmaps de l'ancien sont inutilisables
        m_openWithIcons.clear();
        m_openWithIconsRt = m_rt;
    }
    const int size = int(24 * m_scale + 0.5f);
    const std::wstring key = ToLower(exe) + L"|" + std::to_wstring(size);
    auto it = m_openWithIcons.find(key);
    if (it != m_openWithIcons.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap> bmp;
    std::vector<uint8_t> px;
    if (IconPixels(exe, 0, size, px))
        m_rt->CreateBitmap(D2D1::SizeU(UINT32(size), UINT32(size)), px.data(), UINT32(size) * 4,
                           D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
    return (m_openWithIcons[key] = bmp).Get();  // échec mémorisé aussi (pas de nouvel essai à chaque image)
}

float Viewer::OpenWithSettingsHeight() {
    EnsureOpenWith();
    return (52 + 44 * float(std::max<size_t>(1, m_openWith.size())) + 46) * m_scale;
}

float Viewer::DrawOpenWithSettings(float left, float right, float y) {
    EnsureOpenWith();
    const float s = m_scale, y0 = y;
    Text(T("openwith.settings.title"), m_tfSmall.Get(), D2D1::RectF(left + 12 * s, y + 6 * s, right - 12 * s, y + 28 * s), kWhite, 1);
    Text(T("openwith.settings.desc"), m_tfSmall.Get(), D2D1::RectF(left + 12 * s, y + 28 * s, right - 12 * s, y + 50 * s), kGrey, 0.9f);
    y += 52 * s;
    if (m_openWith.empty()) {
        Text(T("openwith.settings.empty"), m_tfSmall.Get(), D2D1::RectF(left + 12 * s, y, right - 12 * s, y + 40 * s), kGrey, 0.75f);
        y += 44 * s;
    }
    for (size_t i = 0; i < m_openWith.size(); ++i) {
        const OpenWithEntry& e = m_openWith[i];
        const D2D1_RECT_F rc = D2D1::RectF(left, y, right, y + 40 * s);
        if (int(i) == m_openWithSel) FillRound(rc, 6 * s, kAccent, 0.2f);
        else if (m_mouseInside && Contains(rc, m_mouse)) FillRound(rc, 6 * s, kWhite, 0.06f);
        m_settingRows.push_back({kOwRow0 + int(i), rc});
        const D2D1_RECT_F ir = D2D1::RectF(left + 12 * s, y + 8 * s, left + 36 * s, y + 32 * s);
        if (ID2D1Bitmap* icon = OpenWithIcon(e.path)) m_rt->DrawBitmap(icon, ir);
        else Text(L"", m_tfIcon.Get(), ir, kGrey, 0.8f);  // programme introuvable
        Text(e.name, m_tfSmall.Get(), D2D1::RectF(left + 48 * s, y + 2 * s, right - 12 * s, y + 21 * s), kWhite, 1);
        Text(e.path + L"   " + e.args, m_tfSub.Get(), D2D1::RectF(left + 48 * s, y + 20 * s, right - 12 * s, y + 38 * s), kGrey, 0.85f);
        y += 44 * s;
    }
    // Boutons : Ajouter, Modifier, Supprimer, monter, descendre
    y += 2 * s;
    const bool valid = m_openWithSel >= 0 && m_openWithSel < int(m_openWith.size());
    float x = left + 12 * s;
    auto button = [&](int id, const wchar_t* glyph, const wchar_t* label, float w, bool enabled) {
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + 32 * s);
        const bool hov = enabled && m_mouseInside && Contains(r, m_mouse);
        FillRound(r, 6 * s, kWhite, enabled ? (hov ? 0.16f : 0.09f) : 0.04f);
        if (label) IconLabel(r, glyph, label, kWhite, enabled ? 1.0f : 0.35f);
        else Text(glyph, m_tfIcon.Get(), r, kWhite, enabled ? 1.0f : 0.35f);
        if (enabled) m_settingRows.push_back({id, r});
        x += w + 6 * s;
    };
    button(kOwAdd, L"", T("openwith.settings.add"), 132 * s, m_openWith.size() < kMaxApps);
    button(kOwEdit, L"", T("openwith.settings.edit"), 132 * s, valid);
    button(kOwRemove, L"", T("openwith.settings.remove"), 132 * s, valid);
    button(kOwUp, L"", nullptr, 32 * s, valid && m_openWithSel > 0);
    button(kOwDown, L"", nullptr, 32 * s, valid && m_openWithSel + 1 < int(m_openWith.size()));
    y += 32 * s + 12 * s;
    return y - y0;
}
