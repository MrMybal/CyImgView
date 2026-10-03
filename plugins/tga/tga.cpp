// Plugin CyImgView : Truevision TGA (format courant des textures de jeu).
//
// Types 1/2/3 (palette, couleurs, niveaux de gris) et leurs versions compressées RLE 9/10/11,
// 8/15/16/24/32 bits, origine en haut ou en bas, alpha. Beaucoup d'outils écrivent du 32 bits
// avec un alpha entièrement à 0 alors qu'il n'est pas utilisé : dans ce cas l'image est opaque
// (comme le prévoit le champ « attributs » du pied TGA 2.0 quand il est présent).

#include "cyimg_plugin.h"
#include "cyimg_plugin_i18n.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

bool ReadAll(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 18 && sz.QuadPart < (1ll << 31);
    if (ok) {
        out.resize(size_t(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

inline uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// Un pixel source (bpp octets) -> B, G, R, A
inline void ToBgra(const uint8_t* s, int bpp, bool gray, uint8_t o[4]) {
    if (gray) {
        o[0] = o[1] = o[2] = s[0];
        o[3] = bpp == 2 ? s[1] : 255;  // gris + alpha (rare)
    } else if (bpp == 2) {          // A1 R5 G5 B5
        uint16_t v = U16(s);
        uint8_t r = uint8_t((v >> 10) & 31), g = uint8_t((v >> 5) & 31), b = uint8_t(v & 31);
        o[0] = uint8_t(b << 3 | b >> 2);
        o[1] = uint8_t(g << 3 | g >> 2);
        o[2] = uint8_t(r << 3 | r >> 2);
        o[3] = (v & 0x8000) ? 255 : 0;
    } else {
        o[0] = s[0];
        o[1] = s[1];
        o[2] = s[2];
        o[3] = bpp == 4 ? s[3] : 255;
    }
}

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    auto fail = [&](const wchar_t* msg) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg);
        return CYIMG_E_FAIL;
    };
    cyimg_tr_begin(host, ctx);
    std::vector<uint8_t> f;
    if (!ReadAll(path, f)) return fail(CYIMG_TR("plugin.tga.err.read", L"Lecture du fichier TGA impossible"));
    const uint8_t* h = f.data();
    const uint8_t idLen = h[0], cmapType = h[1], type = h[2];
    const uint16_t cmapFirst = U16(h + 3), cmapLen = U16(h + 5);
    const uint8_t cmapBits = h[7];
    const uint32_t W = U16(h + 12), H = U16(h + 14);
    const uint8_t depth = h[16], desc = h[17];
    const bool rle = type >= 9 && type <= 11;
    const int base = rle ? type - 8 : type;  // 1 palette, 2 couleurs, 3 gris
    if (base < 1 || base > 3 || !W || !H || cmapType > 1) return fail(CYIMG_TR("plugin.tga.err.unknown", L"Fichier TGA non reconnu"));
    const bool gray = base == 3;
    if ((base == 1 && (depth != 8 && depth != 16)) || (base == 2 && depth != 15 && depth != 16 && depth != 24 && depth != 32) ||
        (base == 3 && depth != 8 && depth != 16))
        return fail(CYIMG_TR("plugin.tga.err.depth", L"Profondeur de couleur TGA non gérée"));

    size_t pos = 18 + size_t(idLen);
    // Palette
    std::vector<uint8_t> palette;  // BGRA par entrée
    if (cmapType == 1) {
        const int eb = (cmapBits + 7) / 8;
        if (eb < 2 || eb > 4 || pos + size_t(cmapLen) * eb > f.size()) return fail(CYIMG_TR("plugin.tga.err.palette", L"Palette TGA invalide"));
        palette.resize(size_t(cmapFirst + cmapLen) * 4, 0);
        for (uint32_t i = 0; i < cmapLen; ++i) ToBgra(&f[pos + size_t(i) * eb], eb, false, &palette[size_t(cmapFirst + i) * 4]);
        pos += size_t(cmapLen) * eb;
    }
    if (base == 1 && palette.empty()) return fail(CYIMG_TR("plugin.tga.err.no_palette", L"Palette TGA absente"));

    // Pixels (décompression RLE éventuelle) dans l'ordre du fichier
    const int bpp = (depth + 7) / 8;
    const size_t count = size_t(W) * H;
    std::vector<uint8_t> raw(count * size_t(bpp));
    if (!rle) {
        if (pos + raw.size() > f.size()) return fail(CYIMG_TR("plugin.tga.err.truncated", L"Fichier TGA tronqué"));
        memcpy(raw.data(), &f[pos], raw.size());
    } else {
        size_t px = 0;
        while (px < count) {
            if (pos >= f.size()) return fail(CYIMG_TR("plugin.tga.err.truncated", L"Fichier TGA tronqué"));
            const uint8_t c = f[pos++];
            const size_t n = size_t(c & 0x7F) + 1;
            if (px + n > count) return fail(CYIMG_TR("plugin.tga.err.rle", L"Données RLE TGA invalides"));
            if (c & 0x80) {  // paquet répété
                if (pos + size_t(bpp) > f.size()) return fail(CYIMG_TR("plugin.tga.err.truncated", L"Fichier TGA tronqué"));
                for (size_t i = 0; i < n; ++i) memcpy(&raw[(px + i) * size_t(bpp)], &f[pos], size_t(bpp));
                pos += size_t(bpp);
            } else {         // paquet brut
                if (pos + n * size_t(bpp) > f.size()) return fail(CYIMG_TR("plugin.tga.err.truncated", L"Fichier TGA tronqué"));
                memcpy(&raw[px * size_t(bpp)], &f[pos], n * size_t(bpp));
                pos += n * size_t(bpp);
            }
            px += n;
        }
    }

    // Pied TGA 2.0 : « attributs » = 0 (pas d'alpha), 3 (alpha), 4 (alpha prémultiplié)
    int alphaType = -1;
    if (f.size() >= 26 && memcmp(&f[f.size() - 18], "TRUEVISION-XFILE.", 17) == 0) {
        uint32_t ext;
        memcpy(&ext, &f[f.size() - 26], 4);
        if (ext && size_t(ext) + 495 <= f.size() - 26 && U16(&f[ext]) == 495) alphaType = f[size_t(ext) + 494];
    }
    const bool hasAlphaBits = (desc & 15) != 0 || depth == 32 || (base == 1 && cmapBits == 32);

    // Conversion BGRA + orientation (bit 5 : origine en haut, bit 4 : de droite à gauche)
    std::vector<uint8_t> bgra(count * 4);
    bool anyAlpha = false, anyNonZeroAlpha = false;
    const bool top = (desc & 0x20) != 0, right = (desc & 0x10) != 0;
    for (uint32_t y = 0; y < H; ++y) {
        const uint32_t dy = top ? y : H - 1 - y;
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t dx = right ? W - 1 - x : x;
            const uint8_t* s = &raw[(size_t(y) * W + x) * size_t(bpp)];
            uint8_t* o = &bgra[(size_t(dy) * W + dx) * 4];
            if (base == 1) {
                const size_t idx = bpp == 2 ? U16(s) : s[0];
                if (idx * 4 + 4 <= palette.size()) memcpy(o, &palette[idx * 4], 4);
                else o[0] = o[1] = o[2] = 0, o[3] = 255;
            } else {
                ToBgra(s, bpp, gray, o);
            }
            if (!hasAlphaBits || alphaType == 0 || alphaType == 1 || alphaType == 2) o[3] = 255;
            anyAlpha |= o[3] != 255;
            anyNonZeroAlpha |= o[3] != 0;
        }
    }
    const bool forceOpaque = anyAlpha && !anyNonZeroAlpha;  // alpha à 0 partout : canal inutilisé
    const bool alpha = anyAlpha && !forceOpaque;
    const bool premultiplied = alphaType == 4;

    uint32_t stride = 0;
    uint8_t* out = host->alloc_bgra(ctx, W, H, alpha ? 1 : 0, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t* s = &bgra[size_t(y) * W * 4];
        uint8_t* d = out + size_t(y) * stride;
        for (uint32_t x = 0; x < W; ++x, s += 4, d += 4) {
            const uint8_t a = alpha ? s[3] : 255;
            if (premultiplied || a == 255) {
                d[0] = s[0], d[1] = s[1], d[2] = s[2];
            } else {
                d[0] = uint8_t(s[0] * a / 255), d[1] = uint8_t(s[1] * a / 255), d[2] = uint8_t(s[2] * a / 255);
            }
            d[3] = a;
        }
    }
    wchar_t fmt[64];
    const std::wstring pal = std::wstring(L", ") + CYIMG_TR("plugin.tga.palette", L"palette");
    swprintf_s(fmt, CYIMG_TR("plugin.tga.format", L"TGA %u bits%ls%ls"), unsigned(depth), rle ? L", RLE" : L"",
               base == 1 ? pal.c_str() : L"");
    host->set_format(ctx, fmt);
    if (forceOpaque)
        host->set_info(ctx, CYIMG_TR("plugin.tga.info.alpha", L"Alpha"), CYIMG_TR("plugin.tga.alpha.ignored", L"ignoré (canal vide)"));
    return CYIMG_OK;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"TGA (Truevision)", L"1.0", L"tga;icb;vda;vst", nullptr, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }
