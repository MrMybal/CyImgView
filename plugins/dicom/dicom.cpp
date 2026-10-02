// Plugin CyImgView : DICOM (imagerie médicale, .dcm).
//
// Analyseur d'éléments : VR explicite/implicite, petit/grand-boutiste, longueurs indéfinies,
// séquences sautées, pixels encapsulés (fragments, table d'offsets). Syntaxes de transfert :
// natives, Deflated, RLE Lossless, JPEG Baseline/Extended (confié à WIC), JPEG sans perte (SOF3).
// MONOCHROME1/2 avec Rescale et fenêtre (WindowCenter/Width, sinon min/max), RGB, YBR, palette.
// Multi-images : animation. Les données nominatives du patient ne sont jamais affichées.

#include "cyimg_plugin.h"
#include "dicom_codecs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t Tag(uint16_t g, uint16_t e) { return uint32_t(g) << 16 | e; }
constexpr uint32_t kItem = Tag(0xFFFE, 0xE000), kItemEnd = Tag(0xFFFE, 0xE00D), kSeqEnd = Tag(0xFFFE, 0xE0DD);
constexpr uint32_t kPixelData = Tag(0x7FE0, 0x0010);
constexpr uint32_t kUndefined = 0xFFFFFFFFu;
constexpr size_t kNone = size_t(-1);

struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool Open(const wchar_t* path) {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart < 16 || uint64_t(sz.QuadPart) > (uint64_t(1) << 40)) return false;
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

// --- Lecture des éléments -------------------------------------------------------------------------

struct Enc {
    bool expl = true, be = false;
};

struct Buf {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint16_t U16(size_t i, bool be) const { return be ? uint16_t(p[i] << 8 | p[i + 1]) : uint16_t(p[i] | p[i + 1] << 8); }
    uint32_t U32(size_t i, bool be) const {
        return be ? uint32_t(p[i]) << 24 | uint32_t(p[i + 1]) << 16 | uint32_t(p[i + 2]) << 8 | p[i + 3]
                  : uint32_t(p[i]) | uint32_t(p[i + 1]) << 8 | uint32_t(p[i + 2]) << 16 | uint32_t(p[i + 3]) << 24;
    }
};

struct Elem {
    uint32_t tag = 0;
    char vr[2] = {};
    size_t val = 0;   // début de la valeur
    uint32_t len = 0;
    bool be = false;
};

bool IsVR(uint8_t a, uint8_t b) {
    static const char* list = "AEASATCSDADSDTFLFDISLOLTOBODOFOLOVOWPNSHSLSQSSSTSVTMUCUIULUNURUSUTUV";
    for (const char* s = list; *s; s += 2)
        if (s[0] == char(a) && s[1] == char(b)) return true;
    return false;
}
bool LongVR(const char vr[2]) {
    static const char* list = "OBODOFOLOVOWSQSVUCUNURUTUV";
    for (const char* s = list; *s; s += 2)
        if (s[0] == vr[0] && s[1] == vr[1]) return true;
    return false;
}

// En-tête d'élément à pos (les balises d'items FFFE,xxxx n'ont jamais de VR)
bool ReadHeader(const Buf& b, size_t pos, Enc enc, Elem& e) {
    if (pos > b.n || b.n - pos < 8) return false;
    const uint16_t g = b.U16(pos, enc.be), el = b.U16(pos + 2, enc.be);
    e.tag = Tag(g, el);
    e.be = enc.be;
    e.vr[0] = e.vr[1] = 0;
    if (g == 0xFFFE || !enc.expl) {
        e.len = b.U32(pos + 4, enc.be);
        e.val = pos + 8;
        return true;
    }
    if (!IsVR(b.p[pos + 4], b.p[pos + 5])) return false;
    e.vr[0] = char(b.p[pos + 4]), e.vr[1] = char(b.p[pos + 5]);
    if (LongVR(e.vr)) {
        if (b.n - pos < 12) return false;
        e.len = b.U32(pos + 8, enc.be);
        e.val = pos + 12;
    } else {
        e.len = b.U16(pos + 6, enc.be);
        e.val = pos + 8;
    }
    return true;
}

size_t SkipItem(const Buf& b, size_t pos, Enc enc, int depth);

// Saute le contenu d'une séquence de longueur indéfinie ; retourne la position qui la suit
size_t SkipSequence(const Buf& b, size_t pos, Enc enc, int depth) {
    if (depth > 64) return kNone;
    for (;;) {
        Elem e;
        if (!ReadHeader(b, pos, enc, e)) return kNone;
        if (e.tag == kSeqEnd) return e.val;
        if (e.tag == kItem) {
            pos = e.len == kUndefined ? SkipItem(b, e.val, enc, depth + 1) : e.val + e.len;
        } else {  // élément hors item (fichier mal formé) : sauté tel quel
            if (e.len == kUndefined) return kNone;
            pos = e.val + e.len;
        }
        if (pos == kNone || pos > b.n) return kNone;
    }
}

size_t SkipItem(const Buf& b, size_t pos, Enc enc, int depth) {
    if (depth > 64) return kNone;
    for (;;) {
        Elem e;
        if (!ReadHeader(b, pos, enc, e)) return kNone;
        if (e.tag == kItemEnd) return e.val;
        if (e.len == kUndefined) {
            Enc inner = enc;
            if (enc.expl && e.vr[0] == 'U' && e.vr[1] == 'N') inner = Enc{false, false};  // UN indéfini : implicite LE
            pos = SkipSequence(b, e.val, inner, depth + 1);
        } else {
            pos = e.val + e.len;
        }
        if (pos == kNone || pos > b.n) return kNone;
    }
}

struct Dataset {
    Buf buf;                 // données en cours (fichier projeté ou flux décompressé)
    std::vector<Elem> elems; // éléments de premier niveau
    std::string ts;          // syntaxe de transfert
    Enc enc;
    bool hasPixels = false;
    Elem pixels;
    bool encapsulated = false;
    std::vector<std::pair<size_t, uint32_t>> fragments;  // (position, taille) hors table d'offsets
    std::vector<size_t> fragItemPos;                     // position de l'en-tête de chaque fragment
    std::vector<uint32_t> bot;                           // table d'offsets de base
    bool truncated = false;

    const Elem* Find(uint32_t tag) const {
        for (auto& e : elems)
            if (e.tag == tag) return &e;
        return nullptr;
    }
    bool Valid(const Elem& e) const { return e.len != kUndefined && e.val <= buf.n && buf.n - e.val >= e.len; }
    std::string Str(uint32_t tag) const {
        const Elem* e = Find(tag);
        if (!e || !Valid(*e)) return {};
        std::string s(reinterpret_cast<const char*>(buf.p + e->val), e->len);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
        size_t a = 0;
        while (a < s.size() && s[a] == ' ') ++a;
        return s.substr(a);
    }
    // Valeur numérique n° idx d'une chaîne « a\b\c » (DS, IS)
    bool Num(uint32_t tag, double& v, int idx = 0) const {
        std::string s = Str(tag);
        size_t start = 0;
        for (int i = 0; i < idx; ++i) {
            start = s.find('\\', start);
            if (start == std::string::npos) return false;
            ++start;
        }
        const char* c = s.c_str() + start;
        char* end = nullptr;
        v = strtod(c, &end);
        return end != c && std::isfinite(v);
    }
    int US(uint32_t tag, int def, int idx = 0) const {
        const Elem* e = Find(tag);
        if (!e || !Valid(*e) || e->len < 2u * (idx + 1)) return def;
        if ((e->vr[0] == 'I' && e->vr[1] == 'S') || (e->vr[0] == 'D' && e->vr[1] == 'S')) {
            double v;
            return Num(tag, v, idx) ? int(v) : def;
        }
        if (e->len == 4 && (e->vr[0] == 'U' || e->vr[0] == 'S') && e->vr[1] == 'L') return int(buf.U32(e->val, e->be));
        return buf.U16(e->val + 2 * size_t(idx), e->be);
    }
};

// Analyse le fichier : méta-en-tête, puis éléments de premier niveau jusqu'aux pixels
bool Parse(const Buf& file, std::vector<uint8_t>& inflated, Dataset& ds, std::wstring& why) {
    ds.buf = file;
    size_t pos = 0;
    if (file.n >= 132 && memcmp(file.p + 128, "DICM", 4) == 0) pos = 132;
    // Méta-informations (groupe 0002) : VR explicite petit-boutiste (implicite toléré)
    while (pos + 8 <= file.n && file.U16(pos, false) == 0x0002) {
        Enc menc{IsVR(file.p[pos + 4], file.p[pos + 5]), false};
        Elem e;
        if (!ReadHeader(file, pos, menc, e) || e.len == kUndefined || e.val + e.len > file.n) {
            why = L"méta-en-tête DICOM invalide";
            return false;
        }
        ds.elems.push_back(e);
        pos = e.val + e.len;
    }
    ds.ts = ds.Str(Tag(0x0002, 0x0010));
    while (!ds.ts.empty() && (ds.ts.back() == '\0' || ds.ts.back() == ' ')) ds.ts.pop_back();
    ds.enc = Enc{true, false};
    if (ds.ts == "1.2.840.10008.1.2.2") ds.enc.be = true;
    if (ds.ts == "1.2.840.10008.1.2.1.99") {  // Deflated : le reste du fichier est un flux deflate brut
        if (!InflateUnknownSize(file.p + pos, file.n - pos, size_t(1) << 31, inflated)) {
            why = L"flux Deflate DICOM corrompu";
            return false;
        }
        ds.buf = Buf{inflated.data(), inflated.size()};
        pos = 0;
    }
    const Buf& b = ds.buf;
    // VR implicite ou explicite : déterminé d'après le premier élément (des fichiers mentent)
    if (!ds.enc.be && pos + 8 <= b.n) ds.enc.expl = IsVR(b.p[pos + 4], b.p[pos + 5]);
    while (pos + 8 <= b.n) {
        Elem e;
        if (!ReadHeader(b, pos, ds.enc, e)) break;
        if (e.tag == kPixelData) {
            ds.pixels = e;
            ds.hasPixels = true;
            if (e.len == kUndefined) {  // encapsulé : table d'offsets puis fragments
                ds.encapsulated = true;
                size_t p = e.val;
                bool firstItem = true;
                for (;;) {
                    Elem it;
                    if (!ReadHeader(b, p, Enc{false, ds.enc.be}, it) || it.tag == kSeqEnd) break;
                    if (it.tag != kItem || it.len == kUndefined) break;
                    if (it.val + it.len > b.n) {
                        ds.truncated = true;
                        if (!firstItem) {
                            ds.fragments.emplace_back(it.val, uint32_t(b.n - it.val));
                            ds.fragItemPos.push_back(p);
                        }
                        break;
                    }
                    if (firstItem) {
                        for (uint32_t k = 0; k + 4 <= it.len; k += 4) ds.bot.push_back(b.U32(it.val + k, ds.enc.be));
                        firstItem = false;
                    } else {
                        ds.fragments.emplace_back(it.val, it.len);
                        ds.fragItemPos.push_back(p);
                    }
                    p = it.val + it.len;
                }
            } else if (e.val + e.len > b.n) {
                ds.truncated = true;
            }
            break;
        }
        ds.elems.push_back(e);
        if (e.len == kUndefined) {
            Enc inner = ds.enc;
            if (ds.enc.expl && e.vr[0] == 'U' && e.vr[1] == 'N') inner = Enc{false, false};
            pos = SkipSequence(b, e.val, inner, 0);
            if (pos == kNone) break;
        } else {
            if (e.val + e.len > b.n) break;
            pos = e.val + e.len;
        }
    }
    return true;
}

// --- Syntaxes de transfert ------------------------------------------------------------------------

enum class Codec { Native, Rle, JpegBaseline, JpegLossless, EncapsulatedNative, Unsupported };

struct TsInfo {
    const char* uid;
    Codec codec;
    const wchar_t* name;
};
const TsInfo kTs[] = {
    {"1.2.840.10008.1.2", Codec::Native, L"Implicit VR Little Endian"},
    {"1.2.840.10008.1.2.1", Codec::Native, L"Explicit VR Little Endian"},
    {"1.2.840.10008.1.2.1.99", Codec::Native, L"Deflated Explicit VR Little Endian"},
    {"1.2.840.10008.1.2.2", Codec::Native, L"Explicit VR Big Endian"},
    {"1.2.840.10008.1.2.1.98", Codec::EncapsulatedNative, L"Encapsulated Uncompressed"},
    {"1.2.840.10008.1.2.5", Codec::Rle, L"RLE Lossless"},
    {"1.2.840.10008.1.2.4.50", Codec::JpegBaseline, L"JPEG Baseline"},
    {"1.2.840.10008.1.2.4.51", Codec::JpegBaseline, L"JPEG Extended (12 bits)"},
    {"1.2.840.10008.1.2.4.57", Codec::JpegLossless, L"JPEG Lossless"},
    {"1.2.840.10008.1.2.4.70", Codec::JpegLossless, L"JPEG Lossless SV1"},
    {"1.2.840.10008.1.2.4.80", Codec::Unsupported, L"JPEG-LS Lossless"},
    {"1.2.840.10008.1.2.4.81", Codec::Unsupported, L"JPEG-LS Near-Lossless"},
    {"1.2.840.10008.1.2.4.90", Codec::Unsupported, L"JPEG 2000 Lossless"},
    {"1.2.840.10008.1.2.4.91", Codec::Unsupported, L"JPEG 2000"},
    {"1.2.840.10008.1.2.4.92", Codec::Unsupported, L"JPEG 2000 Part 2 Lossless"},
    {"1.2.840.10008.1.2.4.93", Codec::Unsupported, L"JPEG 2000 Part 2"},
    {"1.2.840.10008.1.2.4.100", Codec::Unsupported, L"MPEG-2"},
    {"1.2.840.10008.1.2.4.101", Codec::Unsupported, L"MPEG-2 High Level"},
    {"1.2.840.10008.1.2.4.102", Codec::Unsupported, L"MPEG-4 AVC/H.264"},
    {"1.2.840.10008.1.2.4.103", Codec::Unsupported, L"MPEG-4 AVC/H.264 BD"},
    {"1.2.840.10008.1.2.4.104", Codec::Unsupported, L"MPEG-4 AVC/H.264 2D"},
    {"1.2.840.10008.1.2.4.105", Codec::Unsupported, L"MPEG-4 AVC/H.264 3D"},
    {"1.2.840.10008.1.2.4.106", Codec::Unsupported, L"MPEG-4 AVC/H.264 Stereo"},
    {"1.2.840.10008.1.2.4.107", Codec::Unsupported, L"HEVC/H.265"},
    {"1.2.840.10008.1.2.4.108", Codec::Unsupported, L"HEVC/H.265 Main 10"},
    {"1.2.840.10008.1.2.4.201", Codec::Unsupported, L"HTJ2K Lossless"},
    {"1.2.840.10008.1.2.4.202", Codec::Unsupported, L"HTJ2K Lossless RPCL"},
    {"1.2.840.10008.1.2.4.203", Codec::Unsupported, L"HTJ2K"},
};

std::wstring Widen(const std::string& s, bool utf8) {
    if (s.empty()) return {};
    const UINT cp = utf8 ? CP_UTF8 : 28591;  // sinon ISO 8859-1
    int n = MultiByteToWideChar(cp, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// --- Conversion des pixels -------------------------------------------------------------------------

struct Image {
    uint32_t rows = 0, cols = 0, spp = 1, ba = 16, bs = 16, hb = 15, frames = 1;
    bool sign = false, planar = false;
    bool bits1 = false;  // 1 bit par pixel (compacté) : déplié en octets
    std::string photo;
    double slope = 1, intercept = 0;
    bool hasWindow = false;
    double wc = 0, ww = 0;
    bool hasPadding = false;
    int padding = 0;
    // Palette : tables 8 bits
    std::vector<uint8_t> lut[3];
    int lutFirst = 0;
    // Fenêtre effective (valeurs « modalité ») et table stockée -> gris
    double lo = 0, hi = 1;
    std::vector<uint8_t> grayLut;
};

inline uint8_t Clamp8(double v) { return uint8_t(v <= 0 ? 0 : v >= 255 ? 255 : int(v + 0.5)); }

// Valeur stockée (entier signé si PixelRepresentation = 1) du pixel/échantillon i d'une image native LE
inline int Stored(const uint8_t* d, size_t i, const Image& im) {
    uint32_t raw;
    if (im.ba == 8) raw = d[i];
    else if (im.ba == 16) raw = uint32_t(d[2 * i] | d[2 * i + 1] << 8);
    else raw = uint32_t(d[4 * i]) | uint32_t(d[4 * i + 1]) << 8 | uint32_t(d[4 * i + 2]) << 16 | uint32_t(d[4 * i + 3]) << 24;
    const uint32_t shift = im.hb + 1 - im.bs;
    uint32_t v = raw >> shift;
    if (im.bs < 32) v &= (1u << im.bs) - 1;
    if (im.sign && im.bs < 32 && (v >> (im.bs - 1)) & 1) return int(int64_t(v) - (int64_t(1) << im.bs));
    return int(v);
}

// Valeur « modalité » -> gris 0..255 (fenêtre linéaire DICOM, inversion MONOCHROME1)
inline uint8_t Window(double m, const Image& im, bool mono1) {
    double g;
    if (im.hasWindow) {
        const double c = im.wc - 0.5, w = im.ww - 1;
        if (m <= c - w / 2) g = 0;
        else if (m > c + w / 2) g = 255;
        else g = ((m - c) / w + 0.5) * 255;
    } else {
        g = im.hi > im.lo ? (m - im.lo) / (im.hi - im.lo) * 255 : 128;
    }
    return mono1 ? uint8_t(255 - Clamp8(g)) : Clamp8(g);
}

// Fenêtre automatique d'après les valeurs de la première image
void AutoWindow(const uint8_t* d, Image& im) {
    const size_t n = size_t(im.rows) * im.cols;
    int mn = INT32_MAX, mx = INT32_MIN;
    for (size_t i = 0; i < n; ++i) {
        const int v = Stored(d, i, im);
        if (im.hasPadding && v == im.padding) continue;
        mn = std::min(mn, v), mx = std::max(mx, v);
    }
    if (mn > mx) mn = mx = 0;
    double a = mn * im.slope + im.intercept, b = mx * im.slope + im.intercept;
    im.lo = std::min(a, b), im.hi = std::max(a, b);
}

void BuildGrayLut(Image& im) {
    if (im.bs > 16) return;
    const bool mono1 = im.photo == "MONOCHROME1";
    const uint32_t count = 1u << im.bs;
    im.grayLut.resize(count);
    for (uint32_t u = 0; u < count; ++u) {
        int v = int(u);
        if (im.sign && (u >> (im.bs - 1)) & 1) v = int(int64_t(u) - (int64_t(1) << im.bs));
        im.grayLut[u] = Window(v * im.slope + im.intercept, im, mono1);
    }
}

inline void YbrToRgb(int y, int cb, int cr, bool partial, uint8_t o[3]) {
    double Y = y, Cb = cb - 128.0, Cr = cr - 128.0;
    if (partial) Y = (y - 16) * 1.164383, Cb *= 1.138393, Cr *= 1.138393;
    o[0] = Clamp8(Y + 1.402 * Cr);
    o[1] = Clamp8(Y - 0.344136 * Cb - 0.714136 * Cr);
    o[2] = Clamp8(Y + 1.772 * Cb);
}

// Image native (LE, entrelacée sauf planar) -> BGRA opaque
void Render(const uint8_t* d, const Image& im, uint8_t* out, uint32_t stride) {
    const uint32_t W = im.cols, H = im.rows;
    const size_t n = size_t(W) * H;
    const std::string& ph = im.photo;
    if (ph == "MONOCHROME1" || ph == "MONOCHROME2") {
        const bool mono1 = ph == "MONOCHROME1";
        const uint32_t mask = im.bs < 32 ? (1u << im.bs) - 1 : 0xFFFFFFFFu;
        for (uint32_t y = 0; y < H; ++y) {
            uint8_t* o = out + size_t(y) * stride;
            for (uint32_t x = 0; x < W; ++x, o += 4) {
                const size_t i = size_t(y) * W + x;
                uint8_t g;
                const int v = Stored(d, i, im);
                if (!im.grayLut.empty()) g = im.grayLut[uint32_t(v) & mask];
                else g = Window(v * im.slope + im.intercept, im, mono1);
                o[0] = o[1] = o[2] = g;
                o[3] = 255;
            }
        }
        return;
    }
    if (ph == "PALETTE COLOR") {
        const size_t ln = im.lut[0].size();
        for (uint32_t y = 0; y < H; ++y) {
            uint8_t* o = out + size_t(y) * stride;
            for (uint32_t x = 0; x < W; ++x, o += 4) {
                int idx = Stored(d, size_t(y) * W + x, im) - im.lutFirst;
                idx = std::clamp(idx, 0, int(ln) - 1);
                o[2] = im.lut[0][size_t(idx)], o[1] = im.lut[1][size_t(idx)], o[0] = im.lut[2][size_t(idx)];
                o[3] = 255;
            }
        }
        return;
    }
    const bool partial = ph.rfind("YBR_PARTIAL", 0) == 0;
    const bool ybr = ph.rfind("YBR", 0) == 0;
    if (ph == "YBR_FULL_422" || ph == "YBR_PARTIAL_422") {  // natif : Y0 Y1 Cb Cr pour 2 pixels
        for (uint32_t y = 0; y < H; ++y) {
            uint8_t* o = out + size_t(y) * stride;
            const uint8_t* s = d + size_t(y) * ((size_t(W) + 1) / 2) * 4;
            for (uint32_t x = 0; x < W; ++x, o += 4) {
                const uint8_t* q = s + (x / 2) * 4;
                uint8_t rgb[3];
                YbrToRgb(q[x & 1], q[2], q[3], partial, rgb);
                o[0] = rgb[2], o[1] = rgb[1], o[2] = rgb[0], o[3] = 255;
            }
        }
        return;
    }
    // RGB / YBR_FULL : 8 ou 16 bits par échantillon
    const int shift = im.bs > 8 ? int(im.bs) - 8 : 0;
    auto sample = [&](size_t pix, int c) -> int {
        const size_t i = im.planar ? size_t(c) * n + pix : pix * 3 + size_t(c);
        int v = Stored(d, i, im);
        if (im.sign) v += 1 << (im.bs - 1);
        return std::clamp(v >> shift, 0, 255);
    };
    for (uint32_t y = 0; y < H; ++y) {
        uint8_t* o = out + size_t(y) * stride;
        for (uint32_t x = 0; x < W; ++x, o += 4) {
            const size_t pix = size_t(y) * W + x;
            int r = sample(pix, 0), g = sample(pix, 1), b = sample(pix, 2);
            if (ybr) {
                uint8_t rgb[3];
                YbrToRgb(r, g, b, partial, rgb);
                r = rgb[0], g = rgb[1], b = rgb[2];
            }
            o[0] = uint8_t(b), o[1] = uint8_t(g), o[2] = uint8_t(r), o[3] = 255;
        }
    }
}

// Table de couleurs (palette) : descripteur [entrées, première valeur, bits] + données
bool ReadLut(const Dataset& ds, int c, Image& im) {
    const uint32_t descTag = Tag(0x0028, uint16_t(0x1101 + c)), dataTag = Tag(0x0028, uint16_t(0x1201 + c));
    int count = ds.US(descTag, -1, 0), first = ds.US(descTag, 0, 1), bits = ds.US(descTag, 16, 2);
    const Elem* e = ds.Find(dataTag);
    if (count < 0 || !e || !ds.Valid(*e)) return false;
    if (count == 0) count = 65536;
    if (first >= 32768 && im.sign) first -= 65536;  // première valeur signée (SS)
    im.lutFirst = first;
    std::vector<uint8_t>& l = im.lut[c];
    l.resize(size_t(count));
    const uint8_t* p = ds.buf.p + e->val;
    if (bits == 8 && e->len >= uint32_t(count) && e->len < uint32_t(count) * 2) {  // octets compactés
        for (int i = 0; i < count; ++i) l[size_t(i)] = p[i];
        return true;
    }
    if (e->len < uint32_t(count) * 2) return false;
    for (int i = 0; i < count; ++i) {
        const uint16_t v = ds.buf.U16(e->val + size_t(i) * 2, e->be);
        l[size_t(i)] = bits <= 8 ? uint8_t(v) : uint8_t(v >> 8);
    }
    return true;
}

// --- Décodage ---------------------------------------------------------------------------------------

struct FrameSource {
    const Dataset& ds;
    const Image& im;
    Codec codec;
    size_t frameBytes = 0;                           // taille d'une image native
    std::vector<std::pair<size_t, size_t>> frameFrags;  // (premier fragment, nombre)

    // Répartition des fragments entre les images
    bool MapFragments(std::wstring& why) {
        const size_t nf = ds.fragments.size();
        if (!nf) {
            why = L"aucun fragment de pixels";
            return false;
        }
        if (im.frames == 1) {
            frameFrags.emplace_back(0, nf);
            return true;
        }
        if (ds.bot.size() >= im.frames && nf >= im.frames) {
            const size_t base = ds.fragItemPos[0];
            size_t f = 0;
            for (uint32_t k = 0; k < im.frames; ++k) {
                const size_t startOff = ds.bot[k], endOff = k + 1 < im.frames ? ds.bot[k + 1] : SIZE_MAX;
                while (f < nf && ds.fragItemPos[f] - base < startOff) ++f;
                size_t g = f;
                while (g < nf && ds.fragItemPos[g] - base < endOff) ++g;
                frameFrags.emplace_back(f, g - f);
                f = g;
            }
            return true;
        }
        if (nf == im.frames) {
            for (size_t k = 0; k < nf; ++k) frameFrags.emplace_back(k, 1);
            return true;
        }
        if (codec == Codec::JpegBaseline || codec == Codec::JpegLossless) {  // découpe sur les marqueurs SOI
            for (size_t k = 0; k < nf; ++k) {
                const uint8_t* p = ds.buf.p + ds.fragments[k].first;
                if (ds.fragments[k].second >= 2 && p[0] == 0xFF && p[1] == 0xD8) frameFrags.emplace_back(k, 1);
                else if (!frameFrags.empty()) frameFrags.back().second++;
            }
            if (!frameFrags.empty()) return true;
        }
        why = L"répartition des fragments entre les images impossible";
        return false;
    }

    // Données compressées d'une image (fragments concaténés si besoin)
    bool Compressed(uint32_t k, std::vector<uint8_t>& tmp, const uint8_t*& p, size_t& size) const {
        if (k >= frameFrags.size() || !frameFrags[k].second) return false;
        const auto [first, count] = frameFrags[k];
        if (count == 1) {
            p = ds.buf.p + ds.fragments[first].first;
            size = ds.fragments[first].second;
            return true;
        }
        tmp.clear();
        for (size_t i = first; i < first + count; ++i) {
            const uint8_t* s = ds.buf.p + ds.fragments[i].first;
            tmp.insert(tmp.end(), s, s + ds.fragments[i].second);
        }
        p = tmp.data();
        size = tmp.size();
        return true;
    }

    // Image k au format natif petit-boutiste
    bool Native(uint32_t k, std::vector<uint8_t>& work, std::vector<uint8_t>& tmp, const uint8_t*& out, std::wstring& why) const {
        if (codec == Codec::Native) {
            const Elem& px = ds.pixels;
            const size_t avail = std::min<size_t>(px.len, ds.buf.n - px.val);
            if (im.bits1) {  // bits compactés (poids faible d'abord) -> 1 octet par pixel
                const size_t pixels = size_t(im.rows) * im.cols, bit0 = pixels * k;
                if ((bit0 + pixels + 7) / 8 > avail) {
                    why = L"données de pixels tronquées";
                    return false;
                }
                work.resize(pixels);
                const uint8_t* s = ds.buf.p + px.val;
                for (size_t i = 0; i < pixels; ++i) work[i] = uint8_t((s[(bit0 + i) >> 3] >> ((bit0 + i) & 7)) & 1);
                out = work.data();
                return true;
            }
            if (uint64_t(frameBytes) * (k + 1) > avail) {
                why = L"données de pixels tronquées";
                return false;
            }
            out = ds.buf.p + px.val + frameBytes * k;
            if (ds.enc.be && im.ba > 8) {  // grand-boutiste : inversion des octets
                work.assign(out, out + frameBytes);
                const size_t w = im.ba / 8;
                for (size_t i = 0; i + w <= work.size(); i += w) std::reverse(work.begin() + ptrdiff_t(i), work.begin() + ptrdiff_t(i + w));
                out = work.data();
            }
            return true;
        }
        const uint8_t* p;
        size_t size;
        if (!Compressed(k, tmp, p, size)) {
            why = L"image " + std::to_wstring(k + 1) + L" absente des fragments";
            return false;
        }
        if (codec == Codec::EncapsulatedNative) {
            if (size < frameBytes) {
                why = L"fragment trop court";
                return false;
            }
            out = p;
            return true;
        }
        if (codec == Codec::Rle) {
            if (!DecodeDicomRle(p, size, im.rows, im.cols, im.spp, im.ba / 8, work, why)) return false;
            out = work.data();
            return true;
        }
        if (codec == Codec::JpegLossless) {
            uint32_t w, h, nc, prec;
            if (!DecodeLosslessJpeg(p, size, im.ba / 8, work, w, h, nc, prec, why)) return false;
            if (w != im.cols || h != im.rows || nc != im.spp) {
                why = L"dimensions du JPEG sans perte différentes de l'en-tête DICOM";
                return false;
            }
            out = work.data();
            return true;
        }
        why = L"codec inattendu";
        return false;
    }
};

int Probe(const uint8_t* h, size_t size);

int DecodeImpl(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    std::wstring why;
    auto fail = [&](const std::wstring& msg, int code = CYIMG_E_FAIL) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg.c_str());
        return code;
    };
    MappedFile mf;
    if (!mf.Open(path)) return fail(L"Lecture du fichier DICOM impossible");
    if (!Probe(mf.data, std::min<size_t>(mf.size, 512))) return fail(L"Fichier DICOM non reconnu");
    std::vector<uint8_t> inflated;
    Dataset ds;
    if (!Parse(Buf{mf.data, mf.size}, inflated, ds, why)) return fail(L"DICOM : " + why);
    if (!ds.hasPixels) {
        if (ds.elems.empty()) return fail(L"Fichier DICOM non reconnu");
        return fail(ds.Find(Tag(0x0004, 0x1220)) ? L"DICOMDIR : répertoire d'examen sans image"
                                                  : L"Ce fichier DICOM ne contient pas d'image (rapport, document ou fichier tronqué)");
    }

    // Syntaxe de transfert
    Codec codec = ds.encapsulated ? Codec::Unsupported : Codec::Native;
    std::wstring tsName = ds.ts.empty() ? (ds.enc.expl ? L"Explicit VR Little Endian (sans méta-en-tête)" : L"Implicit VR Little Endian (sans méta-en-tête)")
                                        : Widen(ds.ts, false);
    for (auto& t : kTs)
        if (ds.ts == t.uid) {
            tsName = t.name;
            codec = ds.encapsulated ? t.codec : Codec::Native;
            if (ds.encapsulated && t.codec == Codec::Native) codec = Codec::Unsupported;
        }
    if (codec == Codec::Unsupported) return fail(L"Compression DICOM non supportée : " + tsName, CYIMG_E_UNSUPPORTED);

    // Description de l'image
    Image im;
    im.rows = uint32_t(ds.US(Tag(0x0028, 0x0010), 0));
    im.cols = uint32_t(ds.US(Tag(0x0028, 0x0011), 0));
    im.spp = uint32_t(ds.US(Tag(0x0028, 0x0002), 1));
    im.ba = uint32_t(ds.US(Tag(0x0028, 0x0100), 16));
    im.bs = uint32_t(ds.US(Tag(0x0028, 0x0101), int(im.ba)));
    im.hb = uint32_t(ds.US(Tag(0x0028, 0x0102), int(im.bs) - 1));
    im.sign = ds.US(Tag(0x0028, 0x0103), 0) == 1;
    im.planar = ds.US(Tag(0x0028, 0x0006), 0) == 1;
    im.photo = ds.Str(Tag(0x0028, 0x0004));
    if (im.photo.empty()) im.photo = im.spp == 3 ? "RGB" : "MONOCHROME2";
    double v;
    if (ds.Num(Tag(0x0028, 0x0008), v) && v >= 1) im.frames = uint32_t(std::min(v, 1e6));
    if (ds.Num(Tag(0x0028, 0x1053), v) && v != 0) im.slope = v;
    if (ds.Num(Tag(0x0028, 0x1052), v)) im.intercept = v;
    double wc, ww;
    if (ds.Num(Tag(0x0028, 0x1050), wc) && ds.Num(Tag(0x0028, 0x1051), ww) && ww >= 1) im.hasWindow = true, im.wc = wc, im.ww = ww;
    if (const Elem* pe = ds.Find(Tag(0x0028, 0x0120)); pe && ds.Valid(*pe) && pe->len >= 2) {
        im.hasPadding = true;
        im.padding = ds.buf.U16(pe->val, pe->be);
        if (im.sign && im.padding >= 32768) im.padding -= 65536;
    }

    if (!im.rows || !im.cols || im.rows > 65535 || im.cols > 65535) return fail(L"Dimensions DICOM invalides");
    if (im.ba != 1 && im.ba != 8 && im.ba != 16 && im.ba != 32) return fail(L"BitsAllocated DICOM non géré : " + std::to_wstring(im.ba), CYIMG_E_UNSUPPORTED);
    if (!im.bs || im.bs > im.ba || im.hb >= im.ba || im.hb + 1 < im.bs) im.bs = im.ba, im.hb = im.ba - 1;
    const bool mono = im.photo == "MONOCHROME1" || im.photo == "MONOCHROME2";
    const bool palette = im.photo == "PALETTE COLOR";
    const bool sub422 = im.photo == "YBR_FULL_422" || im.photo == "YBR_PARTIAL_422";
    const bool color = im.photo == "RGB" || im.photo == "YBR_FULL" || im.photo == "YBR_PARTIAL_420" || sub422;
    if (!mono && !palette && !color && codec != Codec::JpegBaseline)
        return fail(L"Interprétation photométrique DICOM non gérée : " + Widen(im.photo, false), CYIMG_E_UNSUPPORTED);
    if (codec != Codec::JpegBaseline) {
        if ((mono || palette) && im.spp != 1) return fail(L"Nombre d'échantillons DICOM incohérent");
        if (color && im.spp != 3) return fail(L"Nombre d'échantillons DICOM incohérent");
        if (im.ba == 1 && (!mono || codec != Codec::Native)) return fail(L"Image DICOM 1 bit non gérée", CYIMG_E_UNSUPPORTED);
        if (color && im.ba > 16) return fail(L"Couleurs DICOM 32 bits non gérées", CYIMG_E_UNSUPPORTED);
        if (sub422 && (im.ba != 8 || codec != Codec::Native)) {
            // décompressé (RLE, JPEG sans perte) : pleine résolution
            if (codec == Codec::Native) return fail(L"YBR 4:2:2 DICOM non géré", CYIMG_E_UNSUPPORTED);
            im.photo = "YBR_FULL";
        }
        if (im.photo == "YBR_PARTIAL_420") return fail(L"YBR 4:2:0 DICOM non compressé non géré", CYIMG_E_UNSUPPORTED);
    }
    if (palette)
        for (int c = 0; c < 3; ++c)
            if (!ReadLut(ds, c, im)) return fail(L"Palette de couleurs DICOM invalide");
    if (codec == Codec::Rle || codec == Codec::JpegLossless) im.planar = false;  // sortie entrelacée

    // Infos (jamais de données nominatives du patient)
    const bool utf8 = ds.Str(Tag(0x0008, 0x0005)).find("192") != std::string::npos;
    auto info = [&](const wchar_t* key, uint32_t tag) {
        std::string s = ds.Str(tag);
        if (!s.empty()) host->set_info(ctx, key, Widen(s, utf8).c_str());
    };
    info(L"Modalité", Tag(0x0008, 0x0060));
    info(L"Étude", Tag(0x0008, 0x1030));
    info(L"Série", Tag(0x0008, 0x103E));
    info(L"Partie du corps", Tag(0x0018, 0x0015));
    info(L"Fabricant", Tag(0x0008, 0x0070));
    wchar_t buf[160];
    swprintf_s(buf, L"%u x %u", im.cols, im.rows);
    host->set_info(ctx, L"Dimensions", buf);
    swprintf_s(buf, L"%u alloués, %u stockés%ls%ls", im.ba, im.bs, im.sign ? L", signés" : L"", im.spp == 3 ? L" (x3)" : L"");
    host->set_info(ctx, L"Bits", buf);
    host->set_info(ctx, L"Photométrie", Widen(im.photo, false).c_str());
    host->set_info(ctx, L"Syntaxe de transfert", tsName.c_str());
    if (im.frames > 1 && codec != Codec::JpegBaseline) host->set_info(ctx, L"Images", std::to_wstring(im.frames).c_str());
    if (ds.truncated) host->set_info(ctx, L"Attention", L"fichier tronqué");

    FrameSource src{ds, im, codec};
    if (sub422) src.frameBytes = (size_t(im.cols) + 1) / 2 * 4 * im.rows;
    else src.frameBytes = size_t(im.rows) * im.cols * im.spp * (im.ba / 8);
    if (ds.encapsulated && !src.MapFragments(why)) return fail(L"DICOM : " + why);

    // JPEG Baseline/Extended : confié à WIC (première image)
    if (codec == Codec::JpegBaseline) {
        std::vector<uint8_t> tmp;
        const uint8_t* p;
        size_t size;
        if (!src.Compressed(0, tmp, p, size)) return fail(L"DICOM : fragment JPEG absent");
        host->set_format(ctx, L"DICOM (JPEG)");
        if (im.frames > 1) host->set_info(ctx, L"Images", (std::to_wstring(im.frames) + L" (seule la première est affichée)").c_str());
        const int r = host->decode_encoded(ctx, p, size);
        if (r == CYIMG_OK) return r;
        for (size_t i = 0; i + 4 < size; ++i)  // précision 12 bits : non lisible par WIC
            if (p[i] == 0xFF && (p[i + 1] == 0xC1 || p[i + 1] == 0xC0) && p[i + 4] == 12)
                return fail(L"JPEG 12 bits (DICOM Extended) non pris en charge par Windows", CYIMG_E_UNSUPPORTED);
        return fail(L"Décodage du JPEG DICOM impossible");
    }

    im.bits1 = im.ba == 1;
    std::vector<uint8_t> work, tmp;
    const uint8_t* frame0 = nullptr;
    if (!src.Native(0, work, tmp, frame0, why)) return fail(L"DICOM : " + why);
    if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
    if (im.bits1) im.ba = 8, im.bs = 1, im.hb = 0;  // bits dépliés en octets
    if (mono) {
        if (!im.hasWindow) AutoWindow(frame0, im);
        BuildGrayLut(im);
        if (im.hasWindow) swprintf_s(buf, L"centre %g, largeur %g", im.wc, im.ww);
        else swprintf_s(buf, L"automatique (%g à %g)", im.lo, im.hi);
        host->set_info(ctx, L"Fenêtre", buf);
        if (im.slope != 1 || im.intercept != 0) {
            swprintf_s(buf, L"pente %g, ordonnée %g", im.slope, im.intercept);
            host->set_info(ctx, L"Rescale", buf);
        }
    }
    host->set_format(ctx, codec == Codec::Rle ? L"DICOM (RLE)" : codec == Codec::JpegLossless ? L"DICOM (JPEG sans perte)" : L"DICOM");

    uint32_t stride = 0;
    uint8_t* out = host->alloc_bgra(ctx, im.cols, im.rows, 0, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    Render(frame0, im, out, stride);

    // Multi-images : animation (pas pour une vignette)
    if (im.frames > 1 && !host->target_size(ctx) && CYIMG_HOST_HAS(host, add_frame)) {
        uint32_t delay = 100;
        if (ds.Num(Tag(0x0018, 0x1063), v) && v > 0) delay = uint32_t(std::clamp(v, 10.0, 10000.0));
        else if (ds.Num(Tag(0x0018, 0x0040), v) && v > 0) delay = uint32_t(std::clamp(1000.0 / v, 10.0, 10000.0));
        const bool delays = CYIMG_HOST_HAS(host, set_frame_delay);
        if (delays) host->set_frame_delay(ctx, 0, delay);
        uint32_t shown = 1;
        for (uint32_t k = 1; k < im.frames; ++k) {
            if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
            const uint8_t* fr;
            if (!src.Native(k, work, tmp, fr, why)) break;  // images suivantes illisibles : on garde les précédentes
            uint32_t st = 0;
            uint8_t* o = host->add_frame(ctx, &st);
            if (!o) break;
            Render(fr, im, o, st);
            if (delays) host->set_frame_delay(ctx, k, delay);
            ++shown;
        }
        if (shown < im.frames) host->set_info(ctx, L"Images", (std::to_wstring(im.frames) + L" (" + std::to_wstring(shown) + L" chargées)").c_str());
    }
    return CYIMG_OK;
}

// Aucune exception ne doit traverser l'API C (mémoire insuffisante sur une image géante)
int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    try {
        return DecodeImpl(host, ctx, path);
    } catch (...) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, L"Mémoire insuffisante pour décoder l'image");
        return CYIMG_E_OUTOFMEMORY;
    }
}

// En-tête reconnu : « DICM » à 128, ou éléments valides dès le début (fichier sans préambule)
int Probe(const uint8_t* h, size_t size) {
    if (size >= 132 && memcmp(h + 128, "DICM", 4) == 0) return 1;
    if (size < 32) return 0;
    const uint16_t g = uint16_t(h[0] | h[1] << 8);
    if (g != 0x0002 && g != 0x0008) return 0;
    Buf b{h, size};
    Enc enc{IsVR(h[4], h[5]), false};
    size_t pos = 0;
    uint32_t prev = 0;
    for (int i = 0; i < 3; ++i) {
        Elem e;
        if (!ReadHeader(b, pos, enc, e) || e.len == kUndefined || e.len > 4096) return 0;
        if (e.tag <= prev || (e.tag >> 16) > 0x0010 || (e.tag >> 16) & 1) return 0;
        prev = e.tag;
        pos = e.val + e.len;
        if (pos + 8 > size) return i >= 1;
    }
    return 1;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"DICOM (imagerie médicale)", L"1.0", L"dcm;dicom;dic", Probe, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }

// Filtre de listage : « .dic » sert aussi aux dictionnaires d'orthographe, on vérifie l'en-tête
extern "C" __declspec(dllexport) int CyImg_Accept(const wchar_t* path) {
    const wchar_t* dot = wcsrchr(path, L'.');
    if (!dot || _wcsicmp(dot, L".dic") != 0) return 1;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint8_t head[512];
    DWORD got = 0;
    const BOOL ok = ReadFile(h, head, sizeof(head), &got, nullptr);
    CloseHandle(h);
    return ok && Probe(head, got);
}
