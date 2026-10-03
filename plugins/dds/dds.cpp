// Plugin CyImgView : DirectDraw Surface (.dds), textures DirectX.
//
// En-tête classique (masques RGB, FourCC DXT1-5/ATI1/ATI2, codes D3DFMT numériques) et extension
// DX10 (DXGI_FORMAT). Formats par blocs BC1 à BC7 (dont BC6H HDR) via GpuDecodeBlocks, formats non
// compressés 8/16/32 bits, demi-flottants et flottants (HDR ramené en sRGB).
// Seule la mip 0 de la première face/tranche est affichée ; un cubemap complet est déplié en croix.
// Vignette : la plus petite mip suffisante est décodée (rapide sur les grosses textures).

#include "cyimg_plugin_i18n.h"
#include "gpu_decode.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

// --- Fichier projeté en mémoire (seules les pages lues sont chargées) ---------------------------

struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool Open(const wchar_t* path) {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart < 128 || uint64_t(sz.QuadPart) > (uint64_t(1) << 40)) return false;
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

inline uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

float HalfToFloat(uint16_t h) {
    const uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023;
    uint32_t bits;
    if (e == 0) {
        if (!m) bits = s;
        else {  // dénormalisé
            float f = std::ldexp(float(m), -24);
            return s ? -f : f;
        }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 112) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// Petit flottant sans signe (R11G11B10) : 5 bits d'exposant, mbits de mantisse
float SmallFloat(uint32_t v, int mbits) {
    const uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return std::ldexp(float(m), -14 - mbits);
    if (e == 31) return 65504.0f;
    return std::ldexp(1.0f + float(m) / float(1u << mbits), int(e) - 15);
}

// --- Description du format des pixels -----------------------------------------------------------

enum class Kind {
    Block,      // BC1..BC7
    Mask,       // entier jusqu'à 32 bits décrit par des masques (formats classiques et DXGI simples)
    RGBA16,     // 4 x 16 bits non signés
    RGBA16S,    // 4 x 16 bits signés
    RGBA16F, RG16F, R16F,
    RGBA32F, RGB32F, RG32F, R32F,
    R11G11B10F, RGB9E5,
};

struct Format {
    Kind kind = Kind::Mask;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;  // formats par blocs
    uint32_t bits = 0;                       // bits par pixel (non compressés)
    uint32_t mask[4] = {};                   // R, G, B, A (Kind::Mask)
    bool lum = false;                        // masque R = luminance (gris)
    bool alphaOnly = false;                  // seul l'alpha existe : affiché en gris
    bool snorm = false;                      // composantes signées
    bool srgb = false;
    bool premul = false;                     // DXT2/DXT4, alphaMode DX10 prémultiplié
    bool rxgb = false;                       // DXT5 « RXGB » (Doom 3) : rouge stocké dans l'alpha
    bool opaque = false;                     // alphaMode DX10 « opaque » : alpha ignoré
    std::wstring name;
};

Format MaskFormat(uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a, const wchar_t* name) {
    Format f;
    f.kind = Kind::Mask;
    f.bits = bits;
    f.mask[0] = r, f.mask[1] = g, f.mask[2] = b, f.mask[3] = a;
    f.name = name;
    return f;
}
Format SimpleFormat(Kind k, uint32_t bits, const wchar_t* name) {
    Format f;
    f.kind = k;
    f.bits = bits;
    f.name = name;
    return f;
}
Format BlockFormat(DXGI_FORMAT d, const wchar_t* name) {
    Format f;
    f.kind = Kind::Block;
    f.dxgi = d;
    f.name = name;
    return f;
}

// DXGI_FORMAT -> description (false si non géré)
bool FromDxgi(uint32_t d, Format& f) {
    switch (d) {
    case 70: case 71: f = BlockFormat(DXGI_FORMAT_BC1_UNORM, L"BC1"); break;
    case 72: f = BlockFormat(DXGI_FORMAT_BC1_UNORM, L"BC1"); f.srgb = true; break;
    case 73: case 74: f = BlockFormat(DXGI_FORMAT_BC2_UNORM, L"BC2"); break;
    case 75: f = BlockFormat(DXGI_FORMAT_BC2_UNORM, L"BC2"); f.srgb = true; break;
    case 76: case 77: f = BlockFormat(DXGI_FORMAT_BC3_UNORM, L"BC3"); break;
    case 78: f = BlockFormat(DXGI_FORMAT_BC3_UNORM, L"BC3"); f.srgb = true; break;
    case 79: case 80: f = BlockFormat(DXGI_FORMAT_BC4_UNORM, L"BC4"); break;
    case 81: f = BlockFormat(DXGI_FORMAT_BC4_SNORM, CYIMG_TR("plugin.dds.fmt.bc4_signed", L"BC4 signé")); f.snorm = true; break;
    case 82: case 83: f = BlockFormat(DXGI_FORMAT_BC5_UNORM, L"BC5"); break;
    case 84: f = BlockFormat(DXGI_FORMAT_BC5_SNORM, CYIMG_TR("plugin.dds.fmt.bc5_signed", L"BC5 signé")); f.snorm = true; break;
    case 94: case 95: f = BlockFormat(DXGI_FORMAT_BC6H_UF16, L"BC6H"); break;
    case 96: f = BlockFormat(DXGI_FORMAT_BC6H_SF16, CYIMG_TR("plugin.dds.fmt.bc6h_signed", L"BC6H signé")); break;
    case 97: case 98: f = BlockFormat(DXGI_FORMAT_BC7_UNORM, L"BC7"); break;
    case 99: f = BlockFormat(DXGI_FORMAT_BC7_UNORM, L"BC7"); f.srgb = true; break;
    case 1: case 2: f = SimpleFormat(Kind::RGBA32F, 128, L"R32G32B32A32_FLOAT"); break;
    case 5: case 6: f = SimpleFormat(Kind::RGB32F, 96, L"R32G32B32_FLOAT"); break;
    case 9: case 10: f = SimpleFormat(Kind::RGBA16F, 64, L"R16G16B16A16_FLOAT"); break;
    case 11: case 12: f = SimpleFormat(Kind::RGBA16, 64, L"R16G16B16A16_UNORM"); break;
    case 13: f = SimpleFormat(Kind::RGBA16S, 64, L"R16G16B16A16_SNORM"); break;
    case 15: case 16: f = SimpleFormat(Kind::RG32F, 64, L"R32G32_FLOAT"); break;
    case 23: case 24: case 25: f = MaskFormat(32, 0x3FF, 0xFFC00, 0x3FF00000, 0xC0000000, L"R10G10B10A2_UNORM"); break;
    case 26: f = SimpleFormat(Kind::R11G11B10F, 32, L"R11G11B10_FLOAT"); break;
    case 27: case 28: case 30: f = MaskFormat(32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000, L"R8G8B8A8_UNORM"); break;
    case 29: f = MaskFormat(32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000, L"R8G8B8A8_UNORM_SRGB"); f.srgb = true; break;
    case 31: f = MaskFormat(32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000, L"R8G8B8A8_SNORM"); f.snorm = true; break;
    case 33: case 34: f = SimpleFormat(Kind::RG16F, 32, L"R16G16_FLOAT"); break;
    case 35: case 36: f = MaskFormat(32, 0xFFFF, 0xFFFF0000, 0, 0, L"R16G16_UNORM"); break;
    case 37: f = MaskFormat(32, 0xFFFF, 0xFFFF0000, 0, 0, L"R16G16_SNORM"); f.snorm = true; break;
    case 39: case 41: f = SimpleFormat(Kind::R32F, 32, L"R32_FLOAT"); break;
    case 48: case 49: case 50: f = MaskFormat(16, 0xFF, 0xFF00, 0, 0, L"R8G8_UNORM"); break;
    case 51: f = MaskFormat(16, 0xFF, 0xFF00, 0, 0, L"R8G8_SNORM"); f.snorm = true; break;
    case 53: case 54: f = SimpleFormat(Kind::R16F, 16, L"R16_FLOAT"); break;
    case 56: case 57: f = MaskFormat(16, 0xFFFF, 0, 0, 0, L"R16_UNORM"); f.lum = true; break;
    case 60: case 61: case 62: f = MaskFormat(8, 0xFF, 0, 0, 0, L"R8_UNORM"); f.lum = true; break;
    case 65: f = MaskFormat(8, 0, 0, 0, 0xFF, L"A8_UNORM"); f.alphaOnly = true; break;
    case 67: f = SimpleFormat(Kind::RGB9E5, 32, L"R9G9B9E5_SHAREDEXP"); break;
    case 85: f = MaskFormat(16, 0xF800, 0x07E0, 0x001F, 0, L"B5G6R5_UNORM"); break;
    case 86: f = MaskFormat(16, 0x7C00, 0x03E0, 0x001F, 0x8000, L"B5G5R5A1_UNORM"); break;
    case 87: case 90: f = MaskFormat(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, L"B8G8R8A8_UNORM"); break;
    case 91: f = MaskFormat(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, L"B8G8R8A8_UNORM_SRGB"); f.srgb = true; break;
    case 88: case 92: f = MaskFormat(32, 0xFF0000, 0xFF00, 0xFF, 0, L"B8G8R8X8_UNORM"); break;
    case 93: f = MaskFormat(32, 0xFF0000, 0xFF00, 0xFF, 0, L"B8G8R8X8_UNORM_SRGB"); f.srgb = true; break;
    case 115: f = MaskFormat(16, 0x0F00, 0x00F0, 0x000F, 0xF000, L"B4G4R4A4_UNORM"); break;
    default: return false;
    }
    return true;
}

const wchar_t* DxgiHint(uint32_t d) {  // formats connus mais non affichables
    if (d >= 100 && d <= 114) return CYIMG_TR("plugin.dds.hint.yuv", L"vidéo YUV");
    if (d == 19 || d == 20 || (d >= 39 && d <= 47)) return CYIMG_TR("plugin.dds.hint.depth_stencil", L"profondeur/stencil");
    if (d >= 132) return CYIMG_TR("plugin.dds.hint.astc", L"ASTC ou format spécifique");
    return nullptr;
}

// FourCC classique (ou code D3DFMT numérique) -> description
bool FromFourCC(uint32_t cc, Format& f) {
    switch (cc) {
    case FourCC('D', 'X', 'T', '1'): f = BlockFormat(DXGI_FORMAT_BC1_UNORM, L"DXT1"); return true;
    case FourCC('D', 'X', 'T', '2'): f = BlockFormat(DXGI_FORMAT_BC2_UNORM, L"DXT2"); f.premul = true; return true;
    case FourCC('D', 'X', 'T', '3'): f = BlockFormat(DXGI_FORMAT_BC2_UNORM, L"DXT3"); return true;
    case FourCC('D', 'X', 'T', '4'): f = BlockFormat(DXGI_FORMAT_BC3_UNORM, L"DXT4"); f.premul = true; return true;
    case FourCC('D', 'X', 'T', '5'): f = BlockFormat(DXGI_FORMAT_BC3_UNORM, L"DXT5"); return true;
    case FourCC('R', 'X', 'G', 'B'): f = BlockFormat(DXGI_FORMAT_BC3_UNORM, L"DXT5 RXGB"); f.rxgb = true; return true;
    case FourCC('A', 'T', 'I', '1'): case FourCC('B', 'C', '4', 'U'): f = BlockFormat(DXGI_FORMAT_BC4_UNORM, L"BC4 (ATI1)"); return true;
    case FourCC('B', 'C', '4', 'S'): f = BlockFormat(DXGI_FORMAT_BC4_SNORM, CYIMG_TR("plugin.dds.fmt.bc4_signed", L"BC4 signé")); f.snorm = true; return true;
    case FourCC('A', 'T', 'I', '2'): case FourCC('B', 'C', '5', 'U'): f = BlockFormat(DXGI_FORMAT_BC5_UNORM, L"BC5 (ATI2)"); return true;
    case FourCC('B', 'C', '5', 'S'): f = BlockFormat(DXGI_FORMAT_BC5_SNORM, CYIMG_TR("plugin.dds.fmt.bc5_signed", L"BC5 signé")); f.snorm = true; return true;
    case 36: f = SimpleFormat(Kind::RGBA16, 64, L"A16B16G16R16"); return true;
    case 110: f = SimpleFormat(Kind::RGBA16S, 64, L"Q16W16V16U16"); return true;
    case 111: f = SimpleFormat(Kind::R16F, 16, L"R16F"); return true;
    case 112: f = SimpleFormat(Kind::RG16F, 32, L"G16R16F"); return true;
    case 113: f = SimpleFormat(Kind::RGBA16F, 64, L"A16B16G16R16F"); return true;
    case 114: f = SimpleFormat(Kind::R32F, 32, L"R32F"); return true;
    case 115: f = SimpleFormat(Kind::RG32F, 64, L"G32R32F"); return true;
    case 116: f = SimpleFormat(Kind::RGBA32F, 128, L"A32B32G32R32F"); return true;
    default: return false;
    }
}

// Nom D3DFMT d'un format classique décrit par masques (nullptr si inconnu)
const wchar_t* MaskName(uint32_t bits, uint32_t r, uint32_t g, uint32_t b, uint32_t a, bool bump) {
    static const struct {
        uint32_t bits, r, g, b, a;
        bool bump;
        const wchar_t* name;
    } t[] = {
        {32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, false, L"A8R8G8B8"}, {32, 0xFF0000, 0xFF00, 0xFF, 0, false, L"X8R8G8B8"},
        {32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000, false, L"A8B8G8R8"}, {32, 0xFF, 0xFF00, 0xFF0000, 0, false, L"X8B8G8R8"},
        {24, 0xFF0000, 0xFF00, 0xFF, 0, false, L"R8G8B8"},           {16, 0xF800, 0x7E0, 0x1F, 0, false, L"R5G6B5"},
        {16, 0x7C00, 0x3E0, 0x1F, 0x8000, false, L"A1R5G5B5"},       {16, 0x7C00, 0x3E0, 0x1F, 0, false, L"X1R5G5B5"},
        {16, 0xF00, 0xF0, 0xF, 0xF000, false, L"A4R4G4B4"},          {16, 0xF00, 0xF0, 0xF, 0, false, L"X4R4G4B4"},
        {32, 0x3FF, 0xFFC00, 0x3FF00000, 0xC0000000, false, L"A2B10G10R10"},
        {32, 0x3FF00000, 0xFFC00, 0x3FF, 0xC0000000, false, L"A2R10G10B10"},
        {32, 0xFFFF, 0xFFFF0000, 0, 0, false, L"G16R16"},            {8, 0xE0, 0x1C, 0x3, 0, false, L"R3G3B2"},
        {16, 0xE0, 0x1C, 0x3, 0xFF00, false, L"A8R3G3B2"},           {16, 0xFF, 0xFF00, 0, 0, true, L"V8U8"},
        {32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000, true, L"Q8W8V8U8"}, {32, 0xFFFF, 0xFFFF0000, 0, 0, true, L"V16U16"},
    };
    for (auto& e : t)
        if (e.bits == bits && e.r == r && e.g == g && e.b == b && e.a == a && e.bump == bump) return e.name;
    return nullptr;
}

std::wstring FourCCText(uint32_t cc) {
    std::wstring s;
    for (int i = 0; i < 4; ++i) {
        const wchar_t c = wchar_t((cc >> (8 * i)) & 0xFF);
        s += (c >= 32 && c < 127) ? c : L'?';
    }
    return s;
}

// Taille d'une surface w x h
uint64_t SurfaceBytes(const Format& f, uint32_t w, uint32_t h) {
    w = std::max(w, 1u), h = std::max(h, 1u);
    if (f.kind == Kind::Block) return uint64_t((w + 3) / 4) * ((h + 3) / 4) * BlockBytes(f.dxgi);
    return (uint64_t(w) * f.bits + 7) / 8 * h;
}

// --- Décodage d'une surface ---------------------------------------------------------------------

// Résultat intermédiaire : RGBA 8 bits non prémultiplié, ou RGBA flottant (HDR / données)
struct Surface {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;   // RGBA8
    std::vector<float> fpx;    // RGBA32F si flottant
    bool isFloat = false;
};

struct MaskChannel {
    uint32_t mask = 0, shift = 0, max = 0;
    void Init(uint32_t m) {
        mask = m;
        if (!m) return;
        unsigned long s = 0;
        _BitScanForward(&s, m);
        shift = s;
        max = m >> s;
    }
    // Valeur sur 0..255 ; signée : -max/2..max/2 ramené sur 0..255
    uint8_t Get(uint32_t v, bool snorm) const {
        uint32_t x = (v & mask) >> shift;
        if (snorm) {
            const uint32_t half = (max >> 1) + 1;
            x = (x ^ half) & max;  // complément à deux -> excès (0 = -1)
        }
        if (max == 255) return uint8_t(x);
        return uint8_t((uint64_t(x) * 255 + max / 2) / max);
    }
};

bool DecodeSurface(const Format& f, const uint8_t* data, uint32_t w, uint32_t h, Surface& s, std::wstring& why) {
    s.w = w, s.h = h;
    const size_t n = size_t(w) * h;
    if (f.kind == Kind::Block) {
        DXGI_FORMAT d = f.dxgi;
        const bool hdr = d == DXGI_FORMAT_BC6H_UF16 || d == DXGI_FORMAT_BC6H_SF16;
        const bool sn = d == DXGI_FORMAT_BC4_SNORM || d == DXGI_FORMAT_BC5_SNORM;
        std::vector<uint8_t> out;
        // sRGB : les octets sont déjà encodés en sRGB, on les lit tels quels (variante UNORM)
        if (!GpuDecodeBlocks(d, w, h, data, size_t(SurfaceBytes(f, w, h)), hdr || sn, out, why)) return false;
        if (hdr) {
            s.isFloat = true;
            s.fpx.resize(n * 4);
            memcpy(s.fpx.data(), out.data(), n * 16);
            return true;
        }
        s.px.resize(n * 4);
        if (sn) {  // -1..1 -> 0..255
            const float* p = reinterpret_cast<const float*>(out.data());
            for (size_t i = 0; i < n; ++i, p += 4) {
                float r = std::clamp(p[0], -1.0f, 1.0f), g = std::clamp(p[1], -1.0f, 1.0f);
                uint8_t* o = &s.px[i * 4];
                if (d == DXGI_FORMAT_BC4_SNORM) {
                    o[0] = o[1] = o[2] = uint8_t((r + 1) * 127.5f + 0.5f);
                } else {
                    const float z = std::sqrt(std::max(0.0f, 1 - r * r - g * g));
                    o[0] = uint8_t((r + 1) * 127.5f + 0.5f);
                    o[1] = uint8_t((g + 1) * 127.5f + 0.5f);
                    o[2] = uint8_t((z + 1) * 127.5f + 0.5f);
                }
                o[3] = 255;
            }
            return true;
        }
        memcpy(s.px.data(), out.data(), n * 4);
        uint8_t* o = s.px.data();
        if (d == DXGI_FORMAT_BC4_UNORM) {
            for (size_t i = 0; i < n; ++i, o += 4) o[1] = o[2] = o[0], o[3] = 255;
        } else if (d == DXGI_FORMAT_BC5_UNORM) {  // normal map : Z reconstruit
            for (size_t i = 0; i < n; ++i, o += 4) {
                const float x = o[0] / 127.5f - 1, y = o[1] / 127.5f - 1;
                const float z = std::sqrt(std::max(0.0f, 1 - x * x - y * y));
                o[2] = uint8_t(z * 127.5f + 127.5f);
                o[3] = 255;
            }
        } else if (f.rxgb) {
            for (size_t i = 0; i < n; ++i, o += 4) o[0] = o[3], o[3] = 255;
        }
        return true;
    }

    const size_t pitch = size_t((uint64_t(w) * f.bits + 7) / 8);
    if (f.kind == Kind::Mask) {
        s.px.resize(n * 4);
        MaskChannel ch[4];
        for (int c = 0; c < 4; ++c) ch[c].Init(f.mask[c]);
        const uint32_t bytes = f.bits / 8;
        // 2 composantes signées (V8U8, R8G8_SNORM...) : Z reconstruit comme une normal map
        const bool twoSigned = f.snorm && ch[0].mask && ch[1].mask && !ch[2].mask;
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* p = data + y * pitch;
            uint8_t* o = &s.px[size_t(y) * w * 4];
            for (uint32_t x = 0; x < w; ++x, p += bytes, o += 4) {
                uint32_t v = p[0];
                if (bytes > 1) v |= uint32_t(p[1]) << 8;
                if (bytes > 2) v |= uint32_t(p[2]) << 16;
                if (bytes > 3) v |= uint32_t(p[3]) << 24;
                if (f.alphaOnly) {
                    o[0] = o[1] = o[2] = ch[3].Get(v, false);
                    o[3] = 255;
                    continue;
                }
                const uint8_t r = ch[0].Get(v, f.snorm);
                if (f.lum) {
                    o[0] = o[1] = o[2] = r;
                } else {
                    o[0] = r;
                    o[1] = ch[1].mask ? ch[1].Get(v, f.snorm) : 0;
                    if (twoSigned) {
                        const float nx = o[0] / 127.5f - 1, ny = o[1] / 127.5f - 1;
                        o[2] = uint8_t(std::sqrt(std::max(0.0f, 1 - nx * nx - ny * ny)) * 127.5f + 127.5f);
                    } else {
                        o[2] = ch[2].mask ? ch[2].Get(v, f.snorm) : 0;
                    }
                }
                o[3] = ch[3].mask ? ch[3].Get(v, false) : 255;
            }
        }
        return true;
    }
    if (f.kind == Kind::RGBA16 || f.kind == Kind::RGBA16S) {
        s.px.resize(n * 4);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* p = data + y * pitch;
            uint8_t* o = &s.px[size_t(y) * w * 4];
            for (uint32_t x = 0; x < w * 4; ++x, p += 2) {
                uint16_t v = uint16_t(p[0] | p[1] << 8);
                if (f.kind == Kind::RGBA16S) v ^= 0x8000;  // signé -> excès
                o[x] = uint8_t(v >> 8);
            }
        }
        return true;
    }
    // Flottants
    s.isFloat = true;
    s.fpx.resize(n * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* p = data + y * pitch;
        float* o = &s.fpx[size_t(y) * w * 4];
        for (uint32_t x = 0; x < w; ++x, o += 4) {
            float r = 0, g = 0, b = 0, a = 1;
            auto H = [&](int i) { return HalfToFloat(uint16_t(p[2 * i] | p[2 * i + 1] << 8)); };
            auto F = [&](int i) {
                float v;
                memcpy(&v, p + 4 * i, 4);
                return v;
            };
            switch (f.kind) {
            case Kind::RGBA16F: r = H(0), g = H(1), b = H(2), a = H(3), p += 8; break;
            case Kind::RG16F: r = H(0), g = H(1), p += 4; break;
            case Kind::R16F: r = g = b = H(0), p += 2; break;
            case Kind::RGBA32F: r = F(0), g = F(1), b = F(2), a = F(3), p += 16; break;
            case Kind::RGB32F: r = F(0), g = F(1), b = F(2), p += 12; break;
            case Kind::RG32F: r = F(0), g = F(1), p += 8; break;
            case Kind::R32F: r = g = b = F(0), p += 4; break;
            case Kind::R11G11B10F: {
                const uint32_t v = U32(p);
                r = SmallFloat(v & 0x7FF, 6), g = SmallFloat((v >> 11) & 0x7FF, 6), b = SmallFloat(v >> 22, 5);
                p += 4;
                break;
            }
            case Kind::RGB9E5: {
                const uint32_t v = U32(p);
                const float sc = std::ldexp(1.0f, int(v >> 27) - 15 - 9);
                r = float(v & 511) * sc, g = float((v >> 9) & 511) * sc, b = float((v >> 18) & 511) * sc;
                p += 4;
                break;
            }
            default: break;
            }
            o[0] = r, o[1] = g, o[2] = b, o[3] = a;
        }
    }
    return true;
}

inline uint8_t LinearToSrgb8(float v) {
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    return uint8_t(s * 255.0f + 0.5f);
}

struct SrgbTable {  // linéaire -> sRGB sur 4096 niveaux
    uint8_t t[4097];
    SrgbTable() {
        for (int i = 0; i <= 4096; ++i) t[i] = LinearToSrgb8(float(i) / 4096.0f);
    }
    uint8_t operator()(float v) const {
        if (!(v > 0)) return 0;
        if (v >= 1) return 255;
        return t[int(v * 4096.0f + 0.5f)];
    }
};

// Surfaces flottantes -> RGBA8. Couleur : linéaire -> sRGB, compression de Reinhard étendue si
// des valeurs dépassent 1 (le maximum devient blanc). Canal unique (données : hauteur, profondeur) :
// gris, normalisé entre min et max s'il sort de 0..1.
void ToneMap(std::vector<Surface>& ss, bool data, std::wstring& note) {
    float lo = 0, hi = 0;
    bool first = true;
    for (auto& s : ss) {
        if (!s.isFloat) continue;
        for (size_t i = 0; i < s.fpx.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {
                const float v = s.fpx[i + size_t(c)];
                if (!std::isfinite(v)) continue;
                if (first) lo = hi = v, first = false;
                lo = std::min(lo, v), hi = std::max(hi, v);
            }
        }
    }
    static const SrgbTable srgb;
    const bool norm = data && (lo < 0 || hi > 1) && hi > lo;
    const bool hdr = !data && hi > 1;
    const float white2 = hi * hi;
    if (norm) {
        wchar_t b[256];
        swprintf_s(b, CYIMG_TR("plugin.dds.values_normalized", L"valeurs normalisées (%g à %g)"), double(lo), double(hi));
        note = b;
    } else if (hdr) {
        wchar_t b[256];
        swprintf_s(b, CYIMG_TR("plugin.dds.values_hdr", L"HDR, maximum %g (ramené au blanc)"), double(hi));
        note = b;
    }
    for (auto& s : ss) {
        if (!s.isFloat) continue;
        const size_t n = size_t(s.w) * s.h;
        s.px.resize(n * 4);
        for (size_t i = 0; i < n; ++i) {
            const float* p = &s.fpx[i * 4];
            uint8_t* o = &s.px[i * 4];
            for (int c = 0; c < 3; ++c) {
                float v = std::isfinite(p[c]) ? p[c] : (p[c] > 0 ? hi : lo);
                if (data) {
                    if (norm) v = (v - lo) / (hi - lo);
                    o[c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f);
                } else {
                    if (hdr && v > 0) v = v * (1 + v / white2) / (1 + v);
                    o[c] = srgb(v);
                }
            }
            o[3] = uint8_t(std::clamp(std::isfinite(p[3]) ? p[3] : 1.0f, 0.0f, 1.0f) * 255 + 0.5f);
        }
        std::vector<float>().swap(s.fpx);
        s.isFloat = false;
    }
}

// --- Décodage du fichier ------------------------------------------------------------------------

// Message traduit avec un seul argument texte (longueur quelconque)
std::wstring Format1(const wchar_t* fmt, const wchar_t* arg) {
    std::wstring s(wcslen(fmt) + wcslen(arg) + 1, L'\0');
    const int n = swprintf(s.data(), s.size(), fmt, arg);
    s.resize(n > 0 ? size_t(n) : 0);
    return s;
}

int DecodeImpl(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    cyimg_tr_begin(host, ctx);
    std::wstring why;
    auto fail = [&](const std::wstring& msg, int code = CYIMG_E_FAIL) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg.c_str());
        return code;
    };
    MappedFile mf;
    if (!mf.Open(path)) return fail(CYIMG_TR("plugin.dds.err.read", L"Lecture du fichier DDS impossible"));
    const uint8_t* fdata = mf.data;
    const size_t fsize = mf.size;
    if (U32(fdata) != FourCC('D', 'D', 'S', ' ') || U32(fdata + 4) != 124) return fail(CYIMG_TR("plugin.dds.err.unrecognized", L"Fichier DDS non reconnu"));
    uint32_t H = U32(fdata + 12), W = U32(fdata + 16), depth = U32(fdata + 24), mips = U32(fdata + 28);
    const uint32_t pfFlags = U32(fdata + 80), cc = U32(fdata + 84), bitCount = U32(fdata + 88);
    const uint32_t caps2 = U32(fdata + 112);
    size_t pos = 128;

    Format f;
    uint32_t arraySize = 1, faces = 1;
    bool volume = false, cube = false, partialCube = false;
    if ((pfFlags & 4) && cc == FourCC('D', 'X', '1', '0')) {
        if (fsize < 148) return fail(CYIMG_TR("plugin.dds.err.truncated", L"Fichier DDS tronqué"));
        const uint32_t dxgi = U32(fdata + 128), dim = U32(fdata + 132), misc = U32(fdata + 136);
        arraySize = std::max(U32(fdata + 140), 1u);
        const uint32_t alphaMode = U32(fdata + 144) & 7;
        pos = 148;
        if (!FromDxgi(dxgi, f)) {
            const wchar_t* hint = DxgiHint(dxgi);
            wchar_t msg[256];
            if (hint) swprintf_s(msg, CYIMG_TR("plugin.dds.err.dxgi_hint", L"Format DXGI %u non pris en charge (%ls)"), unsigned(dxgi), hint);
            else swprintf_s(msg, CYIMG_TR("plugin.dds.err.dxgi", L"Format DXGI %u non pris en charge"), unsigned(dxgi));
            return fail(msg, CYIMG_E_UNSUPPORTED);
        }
        if (dim == 2) H = 1;          // texture 1D
        volume = dim == 4;
        cube = (misc & 4) != 0;
        if (cube) faces = 6;
        if (alphaMode == 2) f.premul = true;
        if (alphaMode == 4) f.opaque = true;
    } else if (pfFlags & 4) {
        if (!FromFourCC(cc, f)) {
            const bool text = cc > 0xFFFF;
            wchar_t msg[256];
            if (text) swprintf_s(msg, CYIMG_TR("plugin.dds.err.fourcc", L"Format DDS « %ls » non pris en charge"), FourCCText(cc).c_str());
            else swprintf_s(msg, CYIMG_TR("plugin.dds.err.fourcc_num", L"Format DDS %u non pris en charge"), unsigned(cc));
            return fail(msg, CYIMG_E_UNSUPPORTED);
        }
    } else {
        // Masques : RGB (0x40), luminance (0x20000), alpha seul (0x2), YUV (0x200), bump DuDv (0x80000)
        if (bitCount != 8 && bitCount != 16 && bitCount != 24 && bitCount != 32) {
            wchar_t msg[256];
            swprintf_s(msg, CYIMG_TR("plugin.dds.err.depth", L"Profondeur DDS non gérée : %u bits"), unsigned(bitCount));
            return fail(msg, CYIMG_E_UNSUPPORTED);
        }
        const uint32_t rm = U32(fdata + 92), gm = U32(fdata + 96), bm = U32(fdata + 100), am = U32(fdata + 104);
        const bool hasA = (pfFlags & 1) != 0;
        wchar_t nm[256];
        if (pfFlags & 0x20000) {
            f = MaskFormat(bitCount, rm, 0, 0, hasA ? am : 0, L"");
            f.lum = true;
            swprintf_s(nm, hasA ? L"A%uL%u" : L"L%u", hasA ? unsigned(__popcnt(am)) : unsigned(__popcnt(rm)), unsigned(__popcnt(rm)));
        } else if ((pfFlags & 2) && !(pfFlags & 0x40)) {
            f = MaskFormat(bitCount, 0, 0, 0, am ? am : 0xFF, L"");
            f.alphaOnly = true;
            swprintf_s(nm, L"A%u", unsigned(__popcnt(am ? am : 0xFF)));
        } else if (pfFlags & (0x40 | 0x200 | 0x80000)) {
            f = MaskFormat(bitCount, rm, gm, bm, hasA ? am : 0, L"");
            f.snorm = (pfFlags & 0x80000) != 0;
            if (hasA)
                swprintf_s(nm, f.snorm ? CYIMG_TR("plugin.dds.mask_rgba_signed", L"signé %u bits RGBA (%u%u%u%u)")
                                       : CYIMG_TR("plugin.dds.mask_rgba", L"%u bits RGBA (%u%u%u%u)"),
                           unsigned(bitCount), unsigned(__popcnt(rm)),
                           unsigned(__popcnt(gm)), unsigned(__popcnt(bm)), unsigned(__popcnt(am)));
            else
                swprintf_s(nm, f.snorm ? CYIMG_TR("plugin.dds.mask_rgb_signed", L"signé %u bits RGB (%u%u%u)")
                                       : CYIMG_TR("plugin.dds.mask_rgb", L"%u bits RGB (%u%u%u)"),
                           unsigned(bitCount), unsigned(__popcnt(rm)),
                           unsigned(__popcnt(gm)), unsigned(__popcnt(bm)));
        } else {
            return fail(CYIMG_TR("plugin.dds.err.pixel_format", L"Format de pixels DDS non reconnu"), CYIMG_E_UNSUPPORTED);
        }
        f.name = nm;
        if (const wchar_t* known = MaskName(bitCount, f.mask[0], f.mask[1], f.mask[2], f.mask[3], f.snorm)) f.name = known;
        if (!f.mask[0] && !f.mask[1] && !f.mask[2] && !f.mask[3]) return fail(CYIMG_TR("plugin.dds.err.masks", L"Masques de couleur DDS invalides"));
    }
    if (caps2 & 0x200) {  // cubemap classique : faces présentes indiquées par 6 bits
        cube = true;
        faces = __popcnt((caps2 >> 10) & 63);
        if (!faces) faces = 6;
        partialCube = faces != 6;
    }
    if (caps2 & 0x200000) volume = true;
    if (!volume) depth = 1;
    depth = std::max(depth, 1u);
    mips = std::clamp(mips, 1u, 32u);
    if (!W || !H || W > 65536 || H > 65536) return fail(CYIMG_TR("plugin.dds.err.dimensions", L"Dimensions DDS invalides"));
    if (f.kind == Kind::Block && (W > 16384 || H > 16384)) return fail(CYIMG_TR("plugin.dds.err.too_big", L"Texture compressée trop grande (16384 px maximum)"));

    // Mips : tailles et décalages dans un élément (face / tranche de tableau)
    struct Mip {
        uint32_t w, h, d;
        uint64_t offset, bytes;
    };
    std::vector<Mip> chain;
    uint64_t elemBytes = 0;
    for (uint32_t i = 0; i < mips; ++i) {
        Mip m{std::max(W >> i, 1u), std::max(H >> i, 1u), std::max(depth >> i, 1u), elemBytes, 0};
        m.bytes = SurfaceBytes(f, m.w, m.h);
        chain.push_back(m);
        elemBytes += m.bytes * m.d;
        if (m.w == 1 && m.h == 1 && m.d == 1) break;
    }
    // Fichiers dont le nombre de mips annoncé dépasse les données : on ne garde que celles présentes
    const uint64_t avail = fsize - pos;
    const bool showCross = cube && !partialCube;
    const uint32_t needFaces = showCross ? 6 : 1;
    if (chain[0].bytes > avail) return fail(CYIMG_TR("plugin.dds.err.truncated", L"Fichier DDS tronqué"));
    uint64_t totalElems = uint64_t(arraySize) * faces;
    if (elemBytes * totalElems > avail) {
        // mips manquantes ou tableau incomplet : éléments contigus de la seule mip 0 ? on se limite à la mip 0
        chain.resize(1);
        elemBytes = chain[0].bytes * chain[0].d;
        if (elemBytes * needFaces > avail) return fail(CYIMG_TR("plugin.dds.err.truncated", L"Fichier DDS tronqué"));
    }

    // Mip affichée : la plus petite suffisante pour une vignette
    size_t mi = 0;
    if (const uint32_t target = host->target_size(ctx)) {
        const uint32_t scale = showCross ? 4 : 1;
        for (size_t i = 1; i < chain.size(); ++i)
            if (std::max(chain[i].w * scale, chain[i].h * (showCross ? 3 : 1)) >= target) mi = i;
    }
    const Mip& m = chain[mi];

    std::vector<Surface> surf(needFaces);
    for (uint32_t i = 0; i < needFaces; ++i) {
        if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
        const uint8_t* src = fdata + pos + elemBytes * i + m.offset;
        if (!DecodeSurface(f, src, m.w, m.h, surf[i], why)) return fail(Format1(CYIMG_TR("plugin.dds.err.decode", L"Décodage DDS impossible : %ls"), why.c_str()));
    }
    // Flottants -> 8 bits
    std::wstring toneNote;
    const bool dataFloat = f.kind == Kind::R16F || f.kind == Kind::R32F;
    if (surf[0].isFloat) ToneMap(surf, dataFloat, toneNote);

    // Assemblage : croix horizontale pour un cubemap complet (+Y au-dessus, -X +Z +X -Z, -Y dessous)
    const uint32_t fw = m.w, fh = m.h;
    const uint32_t OW = showCross ? fw * 4 : fw, OH = showCross ? fh * 3 : fh;
    static const uint32_t cellX[6] = {2, 0, 1, 1, 1, 3}, cellY[6] = {1, 1, 0, 2, 1, 1};

    // Transparence réelle ? (canal entièrement opaque ou entièrement nul = inutilisé)
    bool anyAlpha = false, anyNonZero = false;
    for (auto& s : surf) {
        const uint8_t* p = s.px.data();
        for (size_t i = 0, n = size_t(s.w) * s.h; i < n; ++i, p += 4) {
            anyAlpha |= p[3] != 255 && !f.opaque;
            anyNonZero |= p[3] != 0;
        }
    }
    const bool alphaEmpty = anyAlpha && !anyNonZero;
    const bool alpha = (anyAlpha && !alphaEmpty) || showCross;
    uint32_t stride = 0;
    uint8_t* out = host->alloc_bgra(ctx, OW, OH, alpha ? 1 : 0, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    if (showCross)
        for (uint32_t y = 0; y < OH; ++y) memset(out + size_t(y) * stride, 0, size_t(OW) * 4);
    for (uint32_t i = 0; i < needFaces; ++i) {
        const uint32_t ox = showCross ? cellX[i] * fw : 0, oy = showCross ? cellY[i] * fh : 0;
        for (uint32_t y = 0; y < fh; ++y) {
            const uint8_t* s = &surf[i].px[size_t(y) * fw * 4];
            uint8_t* d = out + size_t(oy + y) * stride + size_t(ox) * 4;
            for (uint32_t x = 0; x < fw; ++x, s += 4, d += 4) {
                const uint8_t a = alphaEmpty || f.opaque ? 255 : s[3];
                if (a == 255 || f.premul) {
                    d[0] = s[2], d[1] = s[1], d[2] = s[0];
                } else {
                    d[0] = uint8_t((s[2] * a + 127) / 255), d[1] = uint8_t((s[1] * a + 127) / 255), d[2] = uint8_t((s[0] * a + 127) / 255);
                }
                d[3] = a;
            }
        }
    }

    host->set_format(ctx, (L"DDS " + f.name + (f.srgb && f.name.find(L"SRGB") == std::wstring::npos ? L" sRGB" : L"")).c_str());
    host->set_info(ctx, CYIMG_TR("plugin.dds.info.format", L"Format"), f.name.c_str());
    host->set_info(ctx, CYIMG_TR("plugin.dds.info.mipmaps", L"Mipmaps"), std::to_wstring(chain.size()).c_str());
    if (mi) host->set_info(ctx, CYIMG_TR("plugin.dds.info.mip_shown", L"Mip affichée"), (std::to_wstring(mi) + L" (" + std::to_wstring(m.w) + L" x " + std::to_wstring(m.h) + L")").c_str());
    wchar_t buf[256];
    if (cube) {
        if (partialCube) swprintf_s(buf, CYIMG_TR("plugin.dds.cube_partial", L"%u faces (première affichée)"), unsigned(faces));
        else wcscpy_s(buf, CYIMG_TR("plugin.dds.cube_full", L"6 faces (croix : +Y / -X +Z +X -Z / -Y)"));
        host->set_info(ctx, CYIMG_TR("plugin.dds.info.cubemap", L"Cubemap"), buf);
    }
    if (arraySize > 1) {
        swprintf_s(buf, CYIMG_TR("plugin.dds.array_value", L"%u textures (première affichée)"), unsigned(arraySize));
        host->set_info(ctx, CYIMG_TR("plugin.dds.info.array", L"Tableau"), buf);
    }
    if (volume) {
        swprintf_s(buf, CYIMG_TR("plugin.dds.volume_value", L"profondeur %u (première tranche affichée)"), unsigned(depth));
        host->set_info(ctx, CYIMG_TR("plugin.dds.info.volume", L"Volume"), buf);
    }
    if (f.dxgi == DXGI_FORMAT_BC5_UNORM || f.dxgi == DXGI_FORMAT_BC5_SNORM)
        host->set_info(ctx, CYIMG_TR("plugin.dds.info.normal_map", L"Normal map"), CYIMG_TR("plugin.dds.normal_map_value", L"bleu = Z reconstruit depuis R et G"));
    if (f.premul) host->set_info(ctx, CYIMG_TR("plugin.dds.info.alpha", L"Alpha"), CYIMG_TR("plugin.dds.alpha_premul", L"prémultiplié"));
    if (alphaEmpty) host->set_info(ctx, CYIMG_TR("plugin.dds.info.alpha", L"Alpha"), CYIMG_TR("plugin.dds.alpha_ignored", L"ignoré (canal vide)"));
    if (!toneNote.empty()) host->set_info(ctx, CYIMG_TR("plugin.dds.info.values", L"Valeurs"), toneNote.c_str());
    return CYIMG_OK;
}

// Aucune exception ne doit traverser l'API C (mémoire insuffisante sur une image géante)
int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    try {
        return DecodeImpl(host, ctx, path);
    } catch (...) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, CYIMG_TR("plugin.dds.err.memory", L"Mémoire insuffisante pour décoder l'image"));
        return CYIMG_E_OUTOFMEMORY;
    }
}

int Probe(const uint8_t* h, size_t size) { return size >= 8 && U32(h) == FourCC('D', 'D', 'S', ' ') && U32(h + 4) == 124; }

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"DDS (DirectDraw Surface)", L"1.0", L"dds", Probe, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }
