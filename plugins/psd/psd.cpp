// Plugin CyImgView : Adobe Photoshop PSD / PSB (image composite fusionnée).
//
// Lit la section « Image Data » (composite enregistré avec « Compatibilité maximale ») :
// compressions brute, RLE PackBits, ZIP et ZIP avec prédiction ; modes Bitmap, Niveaux de gris,
// Couleurs indexées, RVB, CMJN, Lab, Bichromie et Multicouche ; 1/8/16/32 bits par canal.
// Le premier canal supplémentaire n'est la transparence que si la section des calques l'indique
// (nombre de calques négatif ou bloc Mtrn/Mt16/Mt32) : sinon c'est une sélection enregistrée.
// Le composite transparent est « matté » sur blanc par Photoshop : le matting est retiré.
// Sans composite, la miniature JPEG des ressources (1036, ou 1033 en BGR) est affichée.

#include "cyimg_plugin_i18n.h"
#include "inflate.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

// Fichier projeté en mémoire en lecture seule (seules les parties lues sont chargées).
struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool Open(const wchar_t* path) {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0) return false;
        size = size_t(sz.QuadPart);
        map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map) return false;
        data = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
constexpr uint32_t FourCC(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}

enum : uint32_t { kBitmap = 0, kGray = 1, kIndexed = 2, kRgb = 3, kCmyk = 4, kMulti = 7, kDuotone = 8, kLab = 9 };

struct Psd {
    bool psb = false;
    uint32_t channels = 0, w = 0, h = 0, depth = 0, mode = 0;
    const uint8_t* colorData = nullptr;
    size_t colorLen = 0;
    // Miniature (ressource 1036, ou 1033 aux couleurs BGR)
    const uint8_t* thumb = nullptr;
    size_t thumbLen = 0;
    uint32_t thumbFormat = 0, thumbW = 0, thumbH = 0, thumbRowBytes = 0;
    bool thumbBgr = false;
    int hasRealMerged = -1;  // ressource 1057 (-1 = absente)
    int transpIndex = -1;    // ressource 1047 (couleurs indexées)
    int layerCount = 0;
    bool mergedAlpha = false;
    bool layersOk = false;   // section des calques lue entièrement
    size_t imageData = 0;    // début de la section Image Data
};

const wchar_t* ModeName(uint32_t m) {
    switch (m) {
    case kBitmap: return L"Bitmap";
    case kGray: return CYIMG_TR("plugin.psd.mode.gray", L"Niveaux de gris");
    case kIndexed: return CYIMG_TR("plugin.psd.mode.indexed", L"Couleurs indexées");
    case kRgb: return CYIMG_TR("plugin.psd.mode.rgb", L"RVB");
    case kCmyk: return CYIMG_TR("plugin.psd.mode.cmyk", L"CMJN");
    case kMulti: return CYIMG_TR("plugin.psd.mode.multi", L"Multicouche");
    case kDuotone: return CYIMG_TR("plugin.psd.mode.duotone", L"Bichromie");
    case kLab: return L"Lab";
    default: return CYIMG_TR("plugin.psd.mode.unknown", L"inconnu");
    }
}

uint32_t ColorChannels(uint32_t mode, uint32_t channels) {
    switch (mode) {
    case kRgb:
    case kLab: return 3;
    case kCmyk: return 4;
    case kMulti: return std::min<uint32_t>(channels, channels >= 4 ? 4 : channels >= 3 ? 3 : 1);
    default: return 1;
    }
}

// Ressources d'image : miniature, composite réel, index transparent.
void ParseResources(const uint8_t* p, size_t n, Psd& d) {
    size_t pos = 0;
    while (pos + 12 <= n) {
        const uint32_t sig = Be32(p + pos);
        if (sig != FourCC("8BIM") && sig != FourCC("MeSa") && sig != FourCC("PHUT") && sig != FourCC("AgHg") &&
            sig != FourCC("DCSR"))
            break;
        const uint16_t id = Be16(p + pos + 4);
        const size_t nameLen = p[pos + 6];
        size_t q = pos + 6 + ((nameLen + 2) & ~size_t(1));  // nom Pascal, longueur totale paire
        if (q + 4 > n) break;
        const size_t len = Be32(p + q);
        q += 4;
        if (len > n - q) break;
        const uint8_t* r = p + q;
        if ((id == 1036 || (id == 1033 && !d.thumb)) && len > 28) {
            d.thumb = r + 28;
            d.thumbLen = len - 28;
            d.thumbFormat = Be32(r);
            d.thumbW = Be32(r + 4);
            d.thumbH = Be32(r + 8);
            d.thumbRowBytes = Be32(r + 12);
            d.thumbBgr = id == 1033;
        } else if (id == 1057 && len >= 5) {
            d.hasRealMerged = r[4];
        } else if (id == 1047 && len >= 2) {
            d.transpIndex = Be16(r);
        }
        pos = q + ((len + 1) & ~size_t(1));
    }
}

// Section « Layer and Mask Information » : nombre de calques et transparence du composite.
bool ParseLayers(const uint8_t* f, size_t size, size_t pos, Psd& d, size_t& end) {
    const size_t lw = d.psb ? 8 : 4;
    if (pos + lw > size) return false;
    const uint64_t lmLen = d.psb ? Be64(f + pos) : Be32(f + pos);
    pos += lw;
    if (lmLen > size - pos) return false;
    end = pos + size_t(lmLen);
    if (lmLen < lw) return true;
    const uint64_t liLen = d.psb ? Be64(f + pos) : Be32(f + pos);
    pos += lw;
    if (liLen > end - pos) return true;
    if (liLen >= 2) d.layerCount = int16_t(Be16(f + pos));
    size_t t = pos + size_t(liLen);
    // Masque global puis blocs d'informations supplémentaires
    if (t + 4 > end) return true;
    const size_t gm = Be32(f + t);
    if (gm > end - t - 4) return true;
    t += 4 + gm;
    while (t + 12 <= end) {
        uint32_t sig = Be32(f + t);
        if (sig != FourCC("8BIM") && sig != FourCC("8B64")) {  // remplissage variable selon l'application
            size_t a = (t + 1) & ~size_t(1), b = (t + 3) & ~size_t(3);
            if (a + 12 <= end && (Be32(f + a) == FourCC("8BIM") || Be32(f + a) == FourCC("8B64"))) t = a;
            else if (b + 12 <= end && (Be32(f + b) == FourCC("8BIM") || Be32(f + b) == FourCC("8B64"))) t = b;
            else break;
        }
        const uint32_t key = Be32(f + t + 4);
        const bool long8 = d.psb && (key == FourCC("LMsk") || key == FourCC("Lr16") || key == FourCC("Lr32") ||
                                     key == FourCC("Layr") || key == FourCC("Mt16") || key == FourCC("Mt32") ||
                                     key == FourCC("Mtrn") || key == FourCC("Alph") || key == FourCC("FMsk") ||
                                     key == FourCC("lnk2") || key == FourCC("FEid") || key == FourCC("FXid") ||
                                     key == FourCC("PxSD"));
        size_t q = t + 8;
        if (q + (long8 ? 8 : 4) > end) break;
        const uint64_t len = long8 ? Be64(f + q) : Be32(f + q);
        q += long8 ? 8 : 4;
        if (len > end - q) break;
        if ((key == FourCC("Lr16") || key == FourCC("Lr32") || key == FourCC("Layr")) && len >= 2 && d.layerCount == 0)
            d.layerCount = int16_t(Be16(f + q));
        if (key == FourCC("Mtrn") || key == FourCC("Mt16") || key == FourCC("Mt32")) d.mergedAlpha = true;
        t = q + size_t(len);
    }
    return true;
}

// Tables de conversion
struct Luts {
    uint8_t lin2srgb[16385];
    Luts() {
        for (int i = 0; i <= 16384; ++i) {
            const double v = i / 16384.0;
            const double s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            lin2srgb[i] = uint8_t(std::clamp(s * 255.0 + 0.5, 0.0, 255.0));
        }
    }
    uint8_t Srgb(float v) const {
        if (!(v > 0)) return 0;  // NaN compris
        if (v >= 1) return 255;
        return lin2srgb[int(v * 16384.0f + 0.5f)];
    }
};
const Luts& GetLuts() {
    static const Luts l;  // initialisation thread-safe (C++11)
    return l;
}

// Lab D50 (L 0..100, a/b -128..127) -> sRGB 8 bits
void LabToRgb(float L, float a, float b, const Luts& lut, uint8_t& R, uint8_t& G, uint8_t& B) {
    const float fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    auto inv = [](float t) { return t > 6.0f / 29 ? t * t * t : 3 * (6.0f / 29) * (6.0f / 29) * (t - 4.0f / 29); };
    const float X = 0.96422f * inv(fx), Y = inv(fy), Z = 0.82521f * inv(fz);
    // XYZ D50 -> sRGB linéaire (adaptation de Bradford incluse)
    const float r = 3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z;
    const float g = -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z;
    const float bl = 0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z;
    R = lut.Srgb(r), G = lut.Srgb(g), B = lut.Srgb(bl);
}

struct Fail {
    const wchar_t* msg;
};

// Accès aux lignes de la section Image Data, canal par canal.
class ImageData {
public:
    ImageData(const uint8_t* f, size_t size, const Psd& d) : m_f(f), m_size(size), m_d(d) {
        m_rowBytes = (size_t(d.w) * d.depth + 7) / 8;
    }

    // Prépare la lecture des canaux 0..maxChannel. Lève Fail si les données sont absentes ou tronquées.
    void Init(uint32_t maxChannel) {
        size_t pos = m_d.imageData;
        if (pos + 2 > m_size) throw Fail{CYIMG_TR("plugin.psd.err.no_composite", L"composite absent")};
        m_comp = Be16(m_f + pos);
        pos += 2;
        const uint64_t rows = uint64_t(maxChannel + 1) * m_d.h;
        if (m_comp == 0) {
            if (rows * m_rowBytes > m_size - pos) throw Fail{CYIMG_TR("plugin.psd.err.composite_truncated", L"composite tronqué")};
            m_base = pos;
        } else if (m_comp == 1) {
            const size_t cw = m_d.psb ? 4 : 2;
            const uint64_t table = uint64_t(m_d.channels) * m_d.h * cw;
            if (table > m_size - pos) throw Fail{CYIMG_TR("plugin.psd.err.composite_truncated", L"composite tronqué")};
            m_offsets.resize(size_t(rows) + 1);
            uint64_t off = pos + table;
            for (size_t i = 0; i < rows; ++i) {
                m_offsets[i] = off;
                off += cw == 4 ? Be32(m_f + pos + i * 4) : Be16(m_f + pos + i * 2);
            }
            m_offsets[size_t(rows)] = off;
            if (off > m_size) throw Fail{CYIMG_TR("plugin.psd.err.composite_truncated", L"composite tronqué")};
        } else if (m_comp == 2 || m_comp == 3) {
            const uint64_t raw = uint64_t(m_d.channels) * m_d.h * m_rowBytes;
            // deflate ne dépasse pas un rapport ~1032:1 : écarte les en-têtes incohérents avant d'allouer
            if (raw > (uint64_t(1) << 40) || raw / 1032 > m_size - pos) throw Fail{CYIMG_TR("plugin.psd.err.zip_truncated", L"données ZIP tronquées")};
            m_zip.resize(size_t(raw));
            if (!ZlibDecompress(m_f + pos, m_size - pos, m_zip.data(), m_zip.size())) throw Fail{CYIMG_TR("plugin.psd.err.zip_invalid", L"données ZIP invalides ou tronquées")};
            if (m_comp == 3) Unpredict();
        } else {
            throw Fail{CYIMG_TR("plugin.psd.err.composite_comp", L"compression du composite inconnue")};
        }
    }

    // scratch : tampon de la taille d'une ligne (un par thread)
    const uint8_t* Row(uint32_t c, uint32_t y, uint8_t* scratch) const {
        const size_t idx = size_t(c) * m_d.h + y;
        if (m_comp == 0) return m_f + m_base + idx * m_rowBytes;
        if (m_comp != 1) return m_zip.data() + idx * m_rowBytes;
        // PackBits
        const uint8_t* s = m_f + m_offsets[idx];
        const size_t n = size_t(m_offsets[idx + 1] - m_offsets[idx]);
        uint8_t* o = scratch;
        size_t i = 0, op = 0;
        while (i < n && op < m_rowBytes) {
            const int c8 = int8_t(s[i++]);
            if (c8 >= 0) {
                const size_t k = std::min({size_t(c8) + 1, n - i, m_rowBytes - op});
                memcpy(o + op, s + i, k);
                i += size_t(c8) + 1;
                op += k;
            } else if (c8 != -128) {
                if (i >= n) break;
                const size_t k = std::min(size_t(1 - c8), m_rowBytes - op);
                memset(o + op, s[i++], k);
                op += k;
            }
        }
        if (op < m_rowBytes) memset(o + op, 0, m_rowBytes - op);
        return o;
    }

    uint16_t Compression() const { return m_comp; }

private:
    void Unpredict() {
        const size_t rows = size_t(m_d.channels) * m_d.h, w = m_d.w;
        std::vector<uint8_t> tmp(m_d.depth == 32 ? m_rowBytes : 0);
        for (size_t r = 0; r < rows; ++r) {
            uint8_t* p = m_zip.data() + r * m_rowBytes;
            if (m_d.depth == 8) {
                for (size_t i = 1; i < m_rowBytes; ++i) p[i] = uint8_t(p[i] + p[i - 1]);
            } else if (m_d.depth == 16) {
                for (size_t i = 1; i < w; ++i) {
                    const uint16_t v = uint16_t(Be16(p + i * 2) + Be16(p + i * 2 - 2));
                    p[i * 2] = uint8_t(v >> 8), p[i * 2 + 1] = uint8_t(v);
                }
            } else if (m_d.depth == 32) {  // octets répartis en 4 plans puis delta sur la ligne
                for (size_t i = 1; i < m_rowBytes; ++i) p[i] = uint8_t(p[i] + p[i - 1]);
                for (size_t x = 0; x < w; ++x)
                    for (size_t k = 0; k < 4; ++k) tmp[x * 4 + k] = p[k * w + x];
                memcpy(p, tmp.data(), m_rowBytes);
            }
        }
    }

    const uint8_t* m_f;
    size_t m_size;
    const Psd& m_d;
    size_t m_rowBytes = 0, m_base = 0;
    uint16_t m_comp = 0;
    std::vector<uint64_t> m_offsets;
    std::vector<uint8_t> m_zip;
};

// Miniature des ressources : JPEG (décodé par l'hôte, ou ici si BGR) ou RVB brut.
int DecodeThumb(const CyImgHost* host, CyImgContext* ctx, const Psd& d) {
    if (d.thumbFormat == 1 && !d.thumbBgr) return host->decode_encoded(ctx, d.thumb, d.thumbLen);
    const uint32_t tw = d.thumbW, th = d.thumbH;
    if (!tw || !th || tw > 4096 || th > 4096) return CYIMG_E_FAIL;
    if (d.thumbFormat == 0) {  // RVB 24 bits brut
        if (d.thumbRowBytes < tw * 3 || uint64_t(d.thumbRowBytes) * th > d.thumbLen) return CYIMG_E_FAIL;
        uint32_t stride = 0;
        uint8_t* out = host->alloc_bgra(ctx, tw, th, 0, &stride);
        if (!out) return CYIMG_E_OUTOFMEMORY;
        for (uint32_t y = 0; y < th; ++y) {
            const uint8_t* s = d.thumb + size_t(y) * d.thumbRowBytes;
            uint8_t* o = out + size_t(y) * stride;
            for (uint32_t x = 0; x < tw; ++x, s += 3, o += 4) {
                o[0] = d.thumbBgr ? s[0] : s[2], o[1] = s[1], o[2] = d.thumbBgr ? s[2] : s[0], o[3] = 255;
            }
        }
        return CYIMG_OK;
    }
    if (d.thumbFormat != 1) return CYIMG_E_FAIL;
    // JPEG aux canaux inversés (Photoshop 4) : décodage WIC local puis échange R/B
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int rc = CYIMG_E_FAIL;
    {
        IWICImagingFactory* fac = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapSource* conv = nullptr;
        UINT fw = 0, fh = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
            SUCCEEDED(fac->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(d.thumb), DWORD(d.thumbLen))) &&
            SUCCEEDED(fac->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) &&
            SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame, &conv)) &&
            SUCCEEDED(conv->GetSize(&fw, &fh)) && fw && fh && fw <= 4096 && fh <= 4096) {
            uint32_t stride = 0;
            uint8_t* out = host->alloc_bgra(ctx, fw, fh, 0, &stride);
            if (!out) {
                rc = CYIMG_E_OUTOFMEMORY;
            } else if (SUCCEEDED(conv->CopyPixels(nullptr, stride, stride * fh, out))) {
                for (UINT y = 0; y < fh; ++y)
                    for (UINT x = 0; x < fw; ++x) {
                        uint8_t* o = out + size_t(y) * stride + x * 4;
                        std::swap(o[0], o[2]);
                        o[3] = 255;
                    }
                rc = CYIMG_OK;
            }
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (stream) stream->Release();
        if (fac) fac->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return rc;
}

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    cyimg_tr_begin(host, ctx);
    auto fail = [&](const wchar_t* msg) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg);
        return CYIMG_E_FAIL;
    };
    MappedFile mf;
    if (!mf.Open(path)) return fail(CYIMG_TR("plugin.psd.err.read", L"Lecture du fichier Photoshop impossible"));
    const uint8_t* f = mf.data;
    const size_t size = mf.size;
    if (size < 26 + 12 || memcmp(f, "8BPS", 4) != 0) return fail(CYIMG_TR("plugin.psd.err.unrecognized", L"Fichier Photoshop non reconnu"));

    Psd d;
    const uint16_t version = Be16(f + 4);
    if (version != 1 && version != 2) return fail(CYIMG_TR("plugin.psd.err.version", L"Version de fichier Photoshop inconnue"));
    d.psb = version == 2;
    d.channels = Be16(f + 12);
    d.h = Be32(f + 14);
    d.w = Be32(f + 18);
    d.depth = Be16(f + 22);
    d.mode = Be16(f + 24);
    const uint32_t maxDim = d.psb ? 300000 : 30000;
    if (!d.channels || d.channels > 56 || !d.w || !d.h || d.w > maxDim || d.h > maxDim)
        return fail(CYIMG_TR("plugin.psd.err.header", L"En-tête Photoshop invalide"));
    if (d.depth != 1 && d.depth != 8 && d.depth != 16 && d.depth != 32) return fail(CYIMG_TR("plugin.psd.err.depth", L"Profondeur Photoshop non gérée"));
    if (d.mode != kBitmap && d.mode != kGray && d.mode != kIndexed && d.mode != kRgb && d.mode != kCmyk &&
        d.mode != kMulti && d.mode != kDuotone && d.mode != kLab)
        return fail(CYIMG_TR("plugin.psd.err.mode", L"Mode colorimétrique Photoshop non géré"));
    if ((d.mode == kBitmap) != (d.depth == 1)) return fail(CYIMG_TR("plugin.psd.err.depth_mode", L"Profondeur incompatible avec le mode colorimétrique"));
    if (d.mode == kLab && d.depth == 32) return fail(CYIMG_TR("plugin.psd.err.lab32", L"Lab 32 bits non géré"));

    host->set_format(ctx, d.psb ? L"Photoshop PSB" : L"Photoshop PSD");

    // Données du mode colorimétrique, ressources, calques
    size_t pos = 26;
    const size_t cmLen = Be32(f + pos);
    pos += 4;
    if (cmLen > size - pos) return fail(CYIMG_TR("plugin.psd.err.truncated", L"Fichier Photoshop tronqué"));
    d.colorData = f + pos;
    d.colorLen = cmLen;
    pos += cmLen;
    if (pos + 4 > size) return fail(CYIMG_TR("plugin.psd.err.truncated", L"Fichier Photoshop tronqué"));
    const size_t resLen = Be32(f + pos);
    pos += 4;
    if (resLen > size - pos) return fail(CYIMG_TR("plugin.psd.err.truncated", L"Fichier Photoshop tronqué"));
    ParseResources(f + pos, resLen, d);
    pos += resLen;
    size_t lmEnd = size;
    d.layersOk = ParseLayers(f, size, pos, d, lmEnd);
    d.imageData = lmEnd;
    d.mergedAlpha |= d.layerCount < 0;

    wchar_t buf[256];
    host->set_info(ctx, CYIMG_TR("plugin.psd.info.mode", L"Mode"), ModeName(d.mode));
    swprintf_s(buf, d.depth > 1 ? CYIMG_TR("plugin.psd.depth_many", L"%u bits/canal") : CYIMG_TR("plugin.psd.depth_one", L"%u bit/canal"),
               d.depth);
    host->set_info(ctx, CYIMG_TR("plugin.psd.info.depth", L"Profondeur"), buf);
    swprintf_s(buf, L"%u × %u", d.w, d.h);
    host->set_info(ctx, CYIMG_TR("plugin.psd.info.dimensions", L"Dimensions"), buf);
    swprintf_s(buf, L"%d", d.layerCount < 0 ? -d.layerCount : d.layerCount);
    host->set_info(ctx, CYIMG_TR("plugin.psd.info.layers", L"Calques"), buf);

    const uint32_t target = host->target_size(ctx);
    auto useThumb = [&](const wchar_t* why) -> int {
        if (!d.thumb) return CYIMG_E_FAIL;
        const int rc = DecodeThumb(host, ctx, d);
        if (rc == CYIMG_OK) {
            if (why) swprintf_s(buf, CYIMG_TR("plugin.psd.thumb_why", L"miniature intégrée (%u × %u, %ls)"), d.thumbW, d.thumbH, why);
            else swprintf_s(buf, CYIMG_TR("plugin.psd.thumb", L"miniature intégrée (%u × %u)"), d.thumbW, d.thumbH);
            host->set_info(ctx, CYIMG_TR("plugin.psd.info.preview", L"Aperçu"), buf);
        }
        return rc;
    };

    // Vignette : la miniature suffit si elle est assez grande
    if (target && d.thumb && std::max(d.thumbW, d.thumbH) >= target && useThumb(nullptr) == CYIMG_OK) return CYIMG_OK;

    if (!d.layersOk || d.hasRealMerged == 0) {
        const wchar_t* why = d.layersOk ? CYIMG_TR("plugin.psd.err.no_composite", L"composite absent") : CYIMG_TR("plugin.psd.why.truncated", L"fichier tronqué");
        if (useThumb(why) == CYIMG_OK) return CYIMG_OK;
        return fail(d.layersOk ? CYIMG_TR("plugin.psd.err.no_composite_thumb", L"Composite absent (fichier enregistré sans « Compatibilité maximale ») et aucune miniature")
                               : CYIMG_TR("plugin.psd.err.truncated", L"Fichier Photoshop tronqué"));
    }

    const uint32_t nColor = ColorChannels(d.mode, d.channels);
    if (d.channels < nColor) return fail(CYIMG_TR("plugin.psd.err.channels", L"Nombre de canaux insuffisant pour le mode colorimétrique"));
    const bool hasAlphaCh = d.mergedAlpha && d.channels > nColor;
    const uint32_t nUsed = nColor + (hasAlphaCh ? 1 : 0);
    if (d.mode == kIndexed && d.colorLen < 768) return fail(CYIMG_TR("plugin.psd.err.palette", L"Palette Photoshop absente"));

    // Facteur de réduction : vignette (≥ 2× la cible, l'hôte affine) et limite 65535 px de l'hôte
    uint32_t fct = 1;
    if (target) fct = std::max<uint32_t>(1, std::max(d.w, d.h) / (target * 2));
    while ((d.w + fct - 1) / fct > 65535 || (d.h + fct - 1) / fct > 65535) ++fct;
    const uint32_t ow = (d.w + fct - 1) / fct, oh = (d.h + fct - 1) / fct;
    if (!target && fct > 1) host->set_info(ctx, CYIMG_TR("plugin.psd.info.display", L"Affichage"), CYIMG_TR("plugin.psd.display_reduced", L"réduit (dimensions supérieures à 65535 px)"));

    try {
        ImageData img(f, size, d);
        img.Init(nUsed - 1);
        const wchar_t* const comps[] = {CYIMG_TR("plugin.psd.comp.none", L"aucune"), L"RLE", L"ZIP",
                                        CYIMG_TR("plugin.psd.comp.zip_pred", L"ZIP avec prédiction")};
        host->set_info(ctx, CYIMG_TR("plugin.psd.info.compression", L"Compression"), comps[img.Compression()]);

        // Lignes traitées en parallèle : échantillonnage des canaux utiles (8 bits) puis BGRA (alpha droit)
        const Luts& lut = GetLuts();
        const bool linear32 = d.depth == 32;
        const bool transpIdx = d.mode == kIndexed && d.transpIndex >= 0 && d.transpIndex < 256;
        const bool outAlpha = hasAlphaCh || transpIdx;
        uint32_t stride = 0;
        uint8_t* out = host->alloc_bgra(ctx, ow, oh, outAlpha ? 1 : 0, &stride);
        if (!out) return CYIMG_E_OUTOFMEMORY;
        std::vector<uint32_t> xs(ow);
        for (uint32_t ox = 0; ox < ow; ++ox) xs[ox] = std::min(d.w - 1, ox * fct + fct / 2);
        const uint8_t* pal = d.colorData;
        std::atomic<bool> cancelled{false}, notMatted{false};

        std::atomic<bool> oom{false};
        auto work = [&](uint32_t y0, uint32_t y1) {
            std::vector<uint8_t> scratch, planes;
            try {
                scratch.resize((size_t(d.w) * d.depth + 7) / 8);
                planes.resize(size_t(nUsed) * ow);
            } catch (const std::bad_alloc&) {
                oom = true;
                return;
            }
            for (uint32_t oy = y0; oy < y1; ++oy) {
                if ((oy & 31) == 0 && (cancelled || host->is_cancelled(ctx))) {
                    cancelled = true;
                    return;
                }
                const uint32_t y = std::min(d.h - 1, oy * fct + fct / 2);
                for (uint32_t c = 0; c < nUsed; ++c) {
                    const bool isAlpha = hasAlphaCh && c == nColor;
                    const uint8_t* row = img.Row(c, y, scratch.data());
                    uint8_t* o = planes.data() + size_t(c) * ow;
                    if (d.depth == 8 && fct == 1) {
                        memcpy(o, row, ow);
                        continue;
                    }
                    for (uint32_t ox = 0; ox < ow; ++ox) {
                        const uint32_t x = xs[ox];
                        switch (d.depth) {
                        case 1: o[ox] = (row[x >> 3] >> (7 - (x & 7)) & 1) ? 0 : 255; break;
                        case 8: o[ox] = row[x]; break;
                        case 16: o[ox] = uint8_t((Be16(row + size_t(x) * 2) * 255u + 32767) / 65535); break;
                        default: {
                            const uint32_t u = Be32(row + size_t(x) * 4);
                            float v;
                            memcpy(&v, &u, 4);
                            if (isAlpha || !linear32) o[ox] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                            else o[ox] = lut.Srgb(v);
                        }
                        }
                    }
                }
                const uint8_t* p0 = planes.data();
                const uint8_t* p1 = nColor > 1 ? p0 + ow : p0;
                const uint8_t* p2 = nColor > 2 ? p0 + size_t(ow) * 2 : p0;
                const uint8_t* p3 = nColor > 3 ? p0 + size_t(ow) * 3 : nullptr;
                const uint8_t* pa = hasAlphaCh ? p0 + size_t(ow) * nColor : nullptr;
                uint8_t* o = out + size_t(oy) * stride;
                switch (d.mode) {
                case kIndexed:
                    for (uint32_t x = 0; x < ow; ++x, o += 4) {
                        const uint8_t k = p0[x];
                        o[0] = pal[512 + k], o[1] = pal[256 + k], o[2] = pal[k];
                        o[3] = transpIdx && k == d.transpIndex ? 0 : 255;
                    }
                    break;
                case kRgb:
                    for (uint32_t x = 0; x < ow; ++x, o += 4) o[0] = p2[x], o[1] = p1[x], o[2] = p0[x], o[3] = 255;
                    break;
                case kCmyk:  // valeurs stockées inversées (255 = sans encre)
                case kMulti:
                    if (nColor >= 3) {
                        for (uint32_t x = 0; x < ow; ++x, o += 4) {
                            const unsigned k = p3 ? p3[x] : 255;
                            o[0] = uint8_t(p2[x] * k / 255), o[1] = uint8_t(p1[x] * k / 255), o[2] = uint8_t(p0[x] * k / 255);
                            o[3] = 255;
                        }
                    } else {
                        for (uint32_t x = 0; x < ow; ++x, o += 4) o[0] = o[1] = o[2] = p0[x], o[3] = 255;
                    }
                    break;
                case kLab:
                    for (uint32_t x = 0; x < ow; ++x, o += 4) {
                        LabToRgb(p0[x] * (100.0f / 255), p1[x] - 128.0f, p2[x] - 128.0f, lut, o[2], o[1], o[0]);
                        o[3] = 255;
                    }
                    break;
                default:  // bitmap, gris, bichromie
                    for (uint32_t x = 0; x < ow; ++x, o += 4) o[0] = o[1] = o[2] = p0[x], o[3] = 255;
                }
                if (pa) {  // composite matté sur blanc ? (couleur ≥ 255 - alpha partout)
                    o = out + size_t(oy) * stride;
                    bool bad = false;
                    for (uint32_t x = 0; x < ow; ++x, o += 4) {
                        o[3] = pa[x];
                        bad |= pa[x] < 255 && std::min({o[0], o[1], o[2]}) + 3 < 255 - pa[x];
                    }
                    if (bad) notMatted = true;
                }
            }
        };
        const uint32_t nThreads = std::clamp<uint32_t>(std::min(std::thread::hardware_concurrency(), oh / 64), 1, 8);
        if (nThreads == 1) {
            work(0, oh);
        } else {
            std::vector<std::thread> threads;
            for (uint32_t t = 0; t < nThreads; ++t)
                threads.emplace_back(work, uint32_t(uint64_t(oh) * t / nThreads), uint32_t(uint64_t(oh) * (t + 1) / nThreads));
            for (auto& th : threads) th.join();
        }
        if (cancelled) return CYIMG_E_CANCELLED;
        if (oom) return fail(CYIMG_TR("plugin.psd.err.memory", L"Mémoire insuffisante pour décoder le fichier Photoshop"));

        // Prémultiplication ; matting retiré : c = (c' - (1-a)·255) / a, donc c·a = c' - 255 + a
        if (outAlpha) {
            const bool matted = hasAlphaCh && !notMatted;
            for (uint32_t y = 0; y < oh; ++y) {
                uint8_t* o = out + size_t(y) * stride;
                for (uint32_t x = 0; x < ow; ++x, o += 4) {
                    const int a = o[3];
                    if (a == 255) continue;
                    if (matted)
                        for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::clamp(int(o[k]) + a - 255, 0, a));
                    else
                        for (int k = 0; k < 3; ++k) o[k] = uint8_t((o[k] * a + 127) / 255);
                }
            }
        }
        if (hasAlphaCh) host->set_info(ctx, CYIMG_TR("plugin.psd.info.transparency", L"Transparence"), CYIMG_TR("plugin.psd.yes", L"oui"));
        if (d.channels > nUsed) {
            swprintf_s(buf, L"%u", d.channels - nUsed);
            host->set_info(ctx, CYIMG_TR("plugin.psd.info.alpha_channels", L"Couches alpha"), buf);
        }
        return CYIMG_OK;
    } catch (const Fail& e) {
        if (useThumb(e.msg) == CYIMG_OK) return CYIMG_OK;
        swprintf_s(buf, CYIMG_TR("plugin.psd.err.prefix", L"Photoshop : %ls"), e.msg);
        return fail(buf);
    } catch (const std::bad_alloc&) {
        return fail(CYIMG_TR("plugin.psd.err.memory", L"Mémoire insuffisante pour décoder le fichier Photoshop"));
    }
}

int Probe(const uint8_t* h, size_t n) { return n >= 6 && memcmp(h, "8BPS", 4) == 0 && (h[5] == 1 || h[5] == 2); }

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"Adobe Photoshop", L"1.0", L"psd;psb", Probe, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }
