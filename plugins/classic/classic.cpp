// Plugin CyImgView « classic » : formats d'images historiques sans bibliothèque tierce.
//
//  - Netpbm : PBM/PGM/PPM (P1-P6), PAM (P7), PFM (PF/Pf) ;
//  - PCX (ZSoft) ; XBM et XPM (X Window) ; IFF ILBM/PBM (Amiga, Deluxe Paint) ;
//  - Radiance HDR (RGBE) ; FITS (astronomie) ; ICNS (icônes Apple) ; PICT (QuickDraw).
//
// Le format est reconnu par le contenu (signature), l'extension ne sert qu'en dernier recours.
// Le fichier est projeté en mémoire : une vignette d'un gros FITS ne lit que les lignes utiles.

#include "classic.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <new>

namespace classic {

int Dec::Fail(const wchar_t* msg) const {
    if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg);
    return CYIMG_E_FAIL;
}

std::wstring Widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w += wchar_t(c);
    return w;
}

void Dec::InfoA(const wchar_t* key, const std::string& value) const {
    if (!value.empty()) Info(key, Widen(value).c_str());
}

uint32_t Dec::ThumbStep(uint32_t w, uint32_t h) const {
    const uint32_t t = Target();
    if (!t) return 1;
    return std::max(1u, std::max(w, h) / t);
}

int Emit(const Dec& d, const Rgba& img, bool alpha) {
    if (alpha) {
        alpha = false;
        for (size_t i = 3; i < img.px.size(); i += 4)
            if (img.px[i] != 255) {
                alpha = true;
                break;
            }
    }
    uint32_t stride = 0;
    uint8_t* out = d.host->alloc_bgra(d.ctx, img.w, img.h, alpha ? 1 : 0, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    for (uint32_t y = 0; y < img.h; ++y) {
        const uint8_t* s = img.px.data() + size_t(y) * img.w * 4;
        uint8_t* o = out + size_t(y) * stride;
        for (uint32_t x = 0; x < img.w; ++x, s += 4, o += 4) {
            const uint32_t a = alpha ? s[3] : 255;
            if (a == 255) {
                o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = 255;
            } else {
                o[0] = uint8_t((s[2] * a + 127) / 255);
                o[1] = uint8_t((s[1] * a + 127) / 255);
                o[2] = uint8_t((s[0] * a + 127) / 255);
                o[3] = uint8_t(a);
            }
        }
    }
    return CYIMG_OK;
}

// --- Tone mapping ------------------------------------------------------------------------------

namespace {
struct SrgbLut {
    uint8_t v[4097];
    SrgbLut() {
        for (int i = 0; i <= 4096; ++i) {
            const double l = i / 4096.0;
            const double s = l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
            v[i] = uint8_t(std::clamp(s * 255.0 + 0.5, 0.0, 255.0));
        }
    }
};
const SrgbLut& Lut() {
    static const SrgbLut lut;  // initialisation thread-safe (C++11)
    return lut;
}
}  // namespace

uint8_t LinearToSrgb8(float v) {
    if (!(v > 0.f)) return 0;  // NaN inclus
    if (v >= 1.f) return 255;
    return Lut().v[int(v * 4096.f + 0.5f)];
}

double ToneMap(const float* rgb, uint32_t w, uint32_t h, Rgba& out, bool autoExposure) {
    const size_t count = size_t(w) * h;
    out.Init(w, h, 255);
    double scale = 1.0;
    float white2 = 0.f;  // 0 = simple écrêtage
    if (autoExposure && count) {
        const size_t step = std::max<size_t>(1, count / 1000000);  // ~1 M d'échantillons
        double sumLog = 0;
        size_t used = 0;
        std::vector<float> lums;
        lums.reserve(count / step + 1);
        for (size_t i = 0; i < count; i += step) {
            const float* p = rgb + i * 3;
            const float y = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            if (!std::isfinite(y) || y <= 0.f) continue;  // noir (ou zone manquante) : ignoré
            sumLog += std::log(1e-6 + y);
            lums.push_back(y);
            ++used;
        }
        if (used) {
            const double avg = std::exp(sumLog / double(used));
            scale = avg > 1e-9 ? 0.18 / avg : 1.0;
            // Blanc = luminance au centile 99,5 (le soleil ne doit pas tout assombrir)
            const size_t k = std::min(lums.size() - 1, size_t(double(lums.size()) * 0.995));
            std::nth_element(lums.begin(), lums.begin() + ptrdiff_t(k), lums.end());
            const float lw = float(std::max(1.0, lums[k] * scale));
            white2 = lw * lw;
        }
    }
    const float s = float(scale);
    const SrgbLut& lut = Lut();
    auto enc = [&](float v) -> uint8_t {
        if (!(v > 0.f)) return 0;
        if (v >= 1.f) return 255;
        return lut.v[int(v * 4096.f + 0.5f)];
    };
    uint8_t* o = out.px.data();
    for (size_t i = 0; i < count; ++i, o += 4) {
        float r = rgb[i * 3] * s, g = rgb[i * 3 + 1] * s, b = rgb[i * 3 + 2] * s;
        if (white2 > 0.f) {
            const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (l > 0.f && std::isfinite(l)) {
                const float f = (1.f + l / white2) / (1.f + l);
                r *= f, g *= f, b *= f;
            }
        }
        o[0] = enc(r), o[1] = enc(g), o[2] = enc(b);
    }
    return std::log2(scale);
}

// --- Détection ---------------------------------------------------------------------------------

namespace {

enum class Fmt { None, Netpbm, Pcx, Iff, Hdr, Fits, Icns, Xpm, Xbm, Pict };

bool IsSpace(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

bool IsNetpbm(const uint8_t* p, size_t n) {
    if (n < 3 || p[0] != 'P') return false;
    if ((p[1] >= '1' && p[1] <= '7') || p[1] == 'F' || p[1] == 'f') return IsSpace(p[2]);
    return false;
}

bool IsPcx(const uint8_t* p, size_t n) {
    if (n < 128 || p[0] != 0x0A) return false;
    const uint8_t ver = p[1], enc = p[2], bpp = p[3], planes = p[65];
    if (ver != 0 && ver != 2 && ver != 3 && ver != 4 && ver != 5) return false;
    if (enc > 1 || (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) || planes < 1 || planes > 4) return false;
    return LE16(p + 8) >= LE16(p + 4) && LE16(p + 10) >= LE16(p + 6) && LE16(p + 66) != 0;
}

bool StartsWith(const uint8_t* p, size_t n, const char* s) {
    const size_t l = strlen(s);
    return n >= l && memcmp(p, s, l) == 0;
}

Fmt Detect(const uint8_t* p, size_t n) {
    if (IsNetpbm(p, n)) return Fmt::Netpbm;
    if (IsPcx(p, n)) return Fmt::Pcx;
    if (StartsWith(p, n, "#?RADIANCE") || StartsWith(p, n, "#?RGBE")) return Fmt::Hdr;
    if (StartsWith(p, n, "SIMPLE  =")) return Fmt::Fits;
    if (n >= 8 && StartsWith(p, n, "icns")) return Fmt::Icns;
    if (n >= 12 && (StartsWith(p, n, "FORM") || StartsWith(p, n, "FOR4") || StartsWith(p, n, "FOR8") ||
                    StartsWith(p, n, "LIST") || StartsWith(p, n, "CAT ")))
        return Fmt::Iff;
    if (IsXpm(p, n)) return Fmt::Xpm;
    if (IsXbm(p, n)) return Fmt::Xbm;
    if (IsPict(p, n)) return Fmt::Pict;
    return Fmt::None;
}

Fmt FromExtension(const std::wstring& e) {
    if (e == L"pbm" || e == L"pgm" || e == L"ppm" || e == L"pnm" || e == L"pam" || e == L"pfm") return Fmt::Netpbm;
    if (e == L"pcx") return Fmt::Pcx;
    if (e == L"iff" || e == L"lbm" || e == L"ilbm") return Fmt::Iff;
    if (e == L"hdr" || e == L"rgbe") return Fmt::Hdr;
    if (e == L"fits" || e == L"fit" || e == L"fts") return Fmt::Fits;
    if (e == L"icns") return Fmt::Icns;
    if (e == L"xpm") return Fmt::Xpm;
    if (e == L"xbm") return Fmt::Xbm;
    if (e == L"pct" || e == L"pict") return Fmt::Pict;
    return Fmt::None;
}

int Run(Fmt f, Dec& d) {
    switch (f) {
        case Fmt::Netpbm: return DecodeNetpbm(d);
        case Fmt::Pcx: return DecodePcx(d);
        case Fmt::Iff: return DecodeIff(d);
        case Fmt::Hdr: return DecodeHdr(d);
        case Fmt::Fits: return DecodeFits(d);
        case Fmt::Icns: return DecodeIcns(d);
        case Fmt::Xpm: return DecodeXpm(d);
        case Fmt::Xbm: return DecodeXbm(d);
        case Fmt::Pict: return DecodePict(d);
        default: return d.Fail(CYIMG_TR("plugin.classic.err.unrecognized", L"Format d'image non reconnu"));
    }
}

// Erreur de lecture d'une page projetée (fichier réseau coupé, disque retiré) : échec propre.
int Guarded(Fmt f, Dec* d) {
    __try {
        return Run(f, *d);
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return CYIMG_E_FAIL;
    }
}

struct Mapping {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* view = nullptr;
    size_t size = 0;
    ~Mapping() {
        if (view) UnmapViewOfFile(view);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    bool Open(const wchar_t* path) {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0) return false;
        if (uint64_t(sz.QuadPart) > SIZE_MAX) return false;
        size = size_t(sz.QuadPart);
        map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map) return false;
        view = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
        return view != nullptr;
    }
};

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    cyimg_tr_begin(host, ctx);
    Dec d;
    d.host = host;
    d.ctx = ctx;
    try {
        if (const wchar_t* dot = wcsrchr(path, L'.'); dot && !wcspbrk(dot, L"\\/"))
            for (const wchar_t* c = dot + 1; *c; ++c) d.ext += wchar_t(towlower(*c));
        Mapping m;
        if (!m.Open(path)) return d.Fail(CYIMG_TR("plugin.classic.err.read", L"Lecture du fichier impossible (ou fichier vide)"));
        d.d = m.view;
        d.n = m.size;
        Fmt f = Detect(d.d, d.n);
        if (f == Fmt::None) f = FromExtension(d.ext);
        const int r = Guarded(f, &d);
        return host->is_cancelled(ctx) && r != CYIMG_OK ? CYIMG_E_CANCELLED : r;
    } catch (const std::bad_alloc&) {
        d.Fail(CYIMG_TR("plugin.classic.err.memory", L"Mémoire insuffisante"));
        return CYIMG_E_OUTOFMEMORY;
    } catch (...) {
        return d.Fail(CYIMG_TR("plugin.classic.err.internal", L"Erreur interne lors du décodage"));
    }
}

int Probe(const uint8_t* p, size_t n) {
    const Fmt f = Detect(p, n);
    return f != Fmt::None ? 1 : 0;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION,
    L"Formats classiques (Netpbm, PCX, XBM/XPM, IFF, HDR, FITS, ICNS, PICT)",
    L"1.0",
    L"pbm;pgm;ppm;pnm;pam;pfm;pcx;xbm;xpm;iff;lbm;ilbm;hdr;rgbe;fits;fit;fts;icns;pct;pict",
    Probe,
    Decode,
};

}  // namespace
}  // namespace classic

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &classic::g_plugin; }
