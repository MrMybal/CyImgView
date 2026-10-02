// Plugin CyImgView : formats vectoriels rastérisés avec les API de Windows 10.
//
//  - SVG / SVGZ : Direct2D (ID2D1DeviceContext5::CreateSvgDocument) ;
//  - PDF / AI   : Windows.Data.Pdf (page 1) ;
//  - EMF / WMF / EMZ / WMZ : Direct2D (ID2D1GdiMetafile), GDI en secours ;
//  - DXF        : lecteur 2D maison rendu en Direct2D / DirectWrite.
//
// Le format est reconnu par le contenu (l'extension ne sert qu'à départager .ai / PostScript).

#include "vector.h"

#include "inflate.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <mutex>


namespace vec {

namespace {

struct Factories {
    ID2D1Factory1* d2d = nullptr;
    IWICImagingFactory* wic = nullptr;
    IDWriteFactory* dw = nullptr;
};

// Créées une fois, jamais libérées (la DLL peut être déchargée après l'arrêt de COM).
Factories& Shared() {
    static Factories f;
    static std::once_flag once;
    std::call_once(once, [] {
        D2D1_FACTORY_OPTIONS opt{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &opt,
                          reinterpret_cast<void**>(&f.d2d));
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f.wic));
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&f.dw));
    });
    return f;
}

}  // namespace

ID2D1Factory1* D2DFactory() { return Shared().d2d; }
IWICImagingFactory* WicFactory() { return Shared().wic; }
IDWriteFactory* DWriteFactory() { return Shared().dw; }

std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, f);
    _vsnwprintf_s(buf, _TRUNCATE, f, ap);
    va_end(ap);
    return buf;
}

void RenderSize(const Call& c, double w, double h, uint32_t& outW, uint32_t& outH) {
    if (!(w > 0) || !(h > 0)) w = h = 1024;
    const double big = std::max(w, h);
    double s;
    if (uint32_t t = c.Target()) {
        s = double(t) / big;  // vignette : le plus grand côté fait exactement la taille demandée
    } else {
        s = big < kMinFull ? kMinFull / big : 1.0;
        if (big * s > kMaxFull) s = kMaxFull / big;
    }
    outW = uint32_t(std::clamp(w * s + 0.5, 1.0, kMaxFull));
    outH = uint32_t(std::clamp(h * s + 0.5, 1.0, kMaxFull));
}

bool WicTarget::Create(uint32_t width, uint32_t height) {
    w = width, h = height;
    if (!D2DFactory() || !WicFactory()) return false;
    if (FAILED(WicFactory()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return false;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                    96.f, 96.f);
    if (FAILED(D2DFactory()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return false;
    rt.As(&dc);
    return true;
}

int DeliverPixels(const Call& c, const uint8_t* px, uint32_t stride, uint32_t w, uint32_t h, bool forceOpaque) {
    bool alpha = false;
    if (!forceOpaque) {
        for (uint32_t y = 0; y < h && !alpha; ++y) {
            const uint8_t* s = px + size_t(y) * stride;
            for (uint32_t x = 0; x < w; ++x)
                if (s[x * 4 + 3] != 255) {
                    alpha = true;
                    break;
                }
        }
    }
    uint32_t ostride = 0;
    uint8_t* out = c.host->alloc_bgra(c.ctx, w, h, alpha ? 1 : 0, &ostride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* d = out + size_t(y) * ostride;
        memcpy(d, px + size_t(y) * stride, size_t(w) * 4);
        if (!alpha)
            for (uint32_t x = 0; x < w; ++x) d[x * 4 + 3] = 255;
    }
    return CYIMG_OK;
}

int WicTarget::Deliver(const Call& c, bool forceOpaque) {
    WICRect r{0, 0, int(w), int(h)};
    ComPtr<IWICBitmapLock> lock;
    UINT size = 0, stride = 0;
    BYTE* p = nullptr;
    if (FAILED(bitmap->Lock(&r, WICBitmapLockRead, &lock)) || FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&size, &p)) || !p)
        return c.Fail(L"Lecture du rendu impossible");
    return DeliverPixels(c, p, stride, w, h, forceOpaque);
}

bool ReadAll(const wchar_t* path, std::vector<uint8_t>& out, size_t maxSize) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && uint64_t(sz.QuadPart) <= maxSize;
    if (ok) {
        try {
            out.resize(size_t(sz.QuadPart));
        } catch (...) {
            ok = false;
        }
    }
    if (ok) {
        DWORD got = 0;
        ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

bool IsGzip(const std::vector<uint8_t>& d) { return d.size() >= 18 && d[0] == 0x1F && d[1] == 0x8B && d[2] == 8; }

bool Gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    if (!IsGzip(in)) return false;
    const uint8_t flags = in[3];
    size_t pos = 10;
    if (flags & 4) {  // FEXTRA
        if (pos + 2 > in.size()) return false;
        pos += 2 + size_t(in[pos] | in[pos + 1] << 8);
    }
    for (int bit : {8, 16}) {  // FNAME, FCOMMENT : chaînes terminées par 0
        if (!(flags & bit)) continue;
        while (pos < in.size() && in[pos]) ++pos;
        ++pos;
    }
    if (flags & 2) pos += 2;  // FHCRC
    if (pos + 8 > in.size()) return false;
    const size_t n = in.size();
    const uint32_t isize = uint32_t(in[n - 4] | in[n - 3] << 8 | in[n - 2] << 16 | uint32_t(in[n - 1]) << 24);
    if (!isize || isize > (512u << 20)) return false;
    try {
        out.resize(isize);
    } catch (...) {
        return false;
    }
    return InflateRaw(&in[pos], n - 8 - pos, out.data(), out.size());
}

bool IsEmf(const uint8_t* p, size_t n) { return n >= 88 && p[0] == 1 && !p[1] && !p[2] && !p[3] && !memcmp(p + 40, " EMF", 4); }

bool IsWmf(const uint8_t* p, size_t n) {
    if (n >= 22 && p[0] == 0xD7 && p[1] == 0xCD && p[2] == 0xC6 && p[3] == 0x9A) return true;  // en-tête « placeable »
    // en-tête standard : type 1 (mémoire) ou 2 (disque), taille d'en-tête 9 mots, version 0x100/0x300
    return n >= 18 && (p[0] == 1 || p[0] == 2) && !p[1] && p[2] == 9 && !p[3] && (p[5] == 1 || p[5] == 3) && !p[4];
}

namespace {

// Recherche d'une sous-chaîne dans les premiers octets
bool Has(const uint8_t* p, size_t n, const char* s) {
    const size_t l = strlen(s);
    for (size_t i = 0; i + l <= n; ++i)
        if (!memcmp(p + i, s, l)) return true;
    return false;
}

bool IsPdfHeader(const uint8_t* p, size_t n) { return Has(p, std::min<size_t>(n, 1024), "%PDF-"); }

bool IsPostScript(const uint8_t* p, size_t n) {
    return (n >= 4 && !memcmp(p, "%!PS", 4)) || (n >= 4 && p[0] == 0xC5 && p[1] == 0xD0 && p[2] == 0xD3 && p[3] == 0xC6);
}

bool LooksLikeSvg(const uint8_t* p, size_t n) { return Has(p, std::min<size_t>(n, 8192), "<svg"); }

bool LooksLikeDxf(const uint8_t* p, size_t n) {
    size_t i = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) ++i;
    if (i >= n || p[i] != '0') return false;
    ++i;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) ++i;
    return i + 7 <= n && !memcmp(p + i, "SECTION", 7);
}

bool ExtIs(const wchar_t* path, const wchar_t* ext) {
    const wchar_t* dot = wcsrchr(path, L'.');
    return dot && !_wcsicmp(dot + 1, ext);
}

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    const Call c{host, ctx};
    // En-tête seul d'abord : un PDF est relu par Windows.Data.Pdf, inutile de le charger ici.
    uint8_t head[1024];
    DWORD got = 0;
    {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return c.Fail(L"Ouverture du fichier impossible");
        if (!ReadFile(h, head, sizeof(head), &got, nullptr)) got = 0;
        CloseHandle(h);
    }
    if (!got) return c.Fail(L"Fichier vide ou illisible");
    const bool ai = ExtIs(path, L"ai");
    const bool meta = IsEmf(head, got) || IsWmf(head, got);  // un EMF peut embarquer un PDF (commentaire)
    if (!meta && IsPdfHeader(head, got)) return DecodePdf(c, path, ai);
    if (IsPostScript(head, got)) {
        return c.Fail(ai ? L"Illustrator ancien (PostScript) non supporté : réenregistrez-le avec la compatibilité PDF"
                         : L"Fichier PostScript non supporté");
    }
    if (got >= 18 && !memcmp(head, "AutoCAD Binary DXF", 18)) return c.Fail(L"DXF binaire non supporté (seul le DXF ASCII l'est)");
    if (ai) return c.Fail(L"Fichier Illustrator non reconnu (ni PDF, ni PostScript)");

    std::vector<uint8_t> data;
    if (!ReadAll(path, data)) return c.Fail(L"Lecture du fichier impossible (trop gros ?)");
    bool gz = false;
    if (IsGzip(data)) {
        std::vector<uint8_t> raw;
        if (!Gunzip(data, raw)) return c.Fail(L"Données compressées (gzip) invalides");
        data.swap(raw);
        gz = true;
    }
    const uint8_t* p = data.data();
    const size_t n = data.size();
    if (IsEmf(p, n) || IsWmf(p, n)) return DecodeMetafile(c, data, gz);
    if (LooksLikeSvg(p, n)) return DecodeSvg(c, data, gz);
    if (LooksLikeDxf(p, n)) return DecodeDxf(c, data);
    if (ExtIs(path, L"dxf")) return c.Fail(L"Fichier DXF non reconnu (section manquante)");
    if (ExtIs(path, L"svg") || ExtIs(path, L"svgz")) return c.Fail(L"Fichier SVG non reconnu (élément <svg> absent)");
    return c.Fail(L"Format vectoriel non reconnu");
}

int Probe(const uint8_t* p, size_t n) {
    return IsEmf(p, n) || IsPdfHeader(p, n) || (n >= 4 && p[0] == 0xD7 && p[1] == 0xCD && p[2] == 0xC6 && p[3] == 0x9A) ||
           LooksLikeSvg(p, n) || LooksLikeDxf(p, n);
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"Vectoriel (SVG, PDF, EMF/WMF, DXF)", L"1.0", L"svg;svgz;pdf;ai;wmf;emf;emz;wmz;dxf", Probe, Decode,
};

}  // namespace
}  // namespace vec

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &vec::g_plugin; }
