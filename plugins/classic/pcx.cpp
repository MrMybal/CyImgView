// PCX (ZSoft Paintbrush) : 1/2/4/8 bits, plans EGA, palette VGA 256 couleurs, 24/32 bits en plans, RLE.

#include "classic.h"

#include <algorithm>

namespace classic {
namespace {

// Palette EGA par défaut (version 3 : pas de palette dans le fichier)
const uint8_t kEga[16][3] = {
    {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},   {170, 0, 0},     {170, 0, 170},
    {170, 85, 0},    {170, 170, 170}, {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
    {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255},
};

}  // namespace

int DecodePcx(Dec& d) {
    if (d.n < 128 || d.d[0] != 0x0A) return d.Fail(CYIMG_TR("plugin.classic.pcx.err.unrecognized", L"Fichier PCX non reconnu"));
    const uint8_t* h = d.d;
    const uint8_t ver = h[1], enc = h[2], bpp = h[3], planes = h[65];
    const uint32_t xmin = LE16(h + 4), ymin = LE16(h + 6), xmax = LE16(h + 8), ymax = LE16(h + 10);
    const uint32_t bpl = LE16(h + 66);
    if (xmax < xmin || ymax < ymin) return d.Fail(CYIMG_TR("plugin.classic.pcx.err.dimensions", L"Dimensions PCX invalides"));
    const uint32_t w = xmax - xmin + 1, H = ymax - ymin + 1;
    if (!SizeOk(w, H)) return d.Fail(CYIMG_TR("plugin.classic.pcx.err.dimensions", L"Dimensions PCX invalides"));
    const bool indexed = planes == 1 || (bpp == 1 && planes <= 4);
    const bool rgb = bpp == 8 && (planes == 3 || planes == 4);
    if (!(bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8) || (!indexed && !rgb) || enc > 1)
        return d.Fail(CYIMG_TR("plugin.classic.pcx.err.variant", L"Variante PCX non gérée"));
    if (bpl == 0 || uint64_t(bpl) * 8 < uint64_t(w) * bpp) return d.Fail(CYIMG_TR("plugin.classic.pcx.err.bpl", L"En-tête PCX invalide (octets par ligne)"));

    // Palette
    uint8_t pal[256][3] = {};
    int palSize = 0;
    if (indexed) {
        const int bits = bpp * planes;
        palSize = 1 << bits;
        if (bits == 8) {
            if (d.n >= 128 + 769 && d.d[d.n - 769] == 0x0C) memcpy(pal, d.d + d.n - 768, 768);
            else for (int i = 0; i < 256; ++i) pal[i][0] = pal[i][1] = pal[i][2] = uint8_t(i);  // pas de palette : gris
        } else if (bits == 1) {
            pal[0][0] = pal[0][1] = pal[0][2] = 0;
            pal[1][0] = pal[1][1] = pal[1][2] = 255;
        } else {
            bool empty = true;
            for (int i = 0; i < 48; ++i) empty &= h[16 + i] == 0;
            if (ver == 3 || empty) memcpy(pal, kEga, sizeof(kEga));
            else memcpy(pal, h + 16, 48);
        }
    }

    // Décompression de toutes les lignes (une ligne = planes * bpl octets)
    const size_t lineBytes = size_t(planes) * bpl;
    std::vector<uint8_t> buf(lineBytes * H);
    size_t pos = 128;
    const size_t end = (indexed && bpp * planes == 8 && d.n >= 128 + 769 && d.d[d.n - 769] == 0x0C) ? d.n - 769 : d.n;
    if (enc == 0) {
        if (end - pos < buf.size()) return d.Fail(CYIMG_TR("plugin.classic.pcx.err.truncated", L"Fichier PCX tronqué"));
        memcpy(buf.data(), d.d + pos, buf.size());
    } else {
        size_t o = 0;
        while (o < buf.size()) {
            if (pos >= end) {
                // Certains encodeurs s'arrêtent avant la dernière ligne : tolérer une perte minime
                if (o + lineBytes >= buf.size()) break;
                return d.Fail(CYIMG_TR("plugin.classic.pcx.err.truncated", L"Fichier PCX tronqué"));
            }
            uint8_t c = d.d[pos++];
            size_t cnt = 1;
            if ((c & 0xC0) == 0xC0) {
                cnt = c & 0x3F;
                if (pos >= end) return d.Fail(CYIMG_TR("plugin.classic.pcx.err.truncated", L"Fichier PCX tronqué"));
                c = d.d[pos++];
            }
            cnt = std::min(cnt, buf.size() - o);
            memset(buf.data() + o, c, cnt);
            o += cnt;
        }
    }

    Rgba img;
    img.Init(w, H, 255);
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t* line = buf.data() + y * lineBytes;
        uint8_t* o = img.Row(y);
        for (uint32_t x = 0; x < w; ++x, o += 4) {
            if (rgb) {
                o[0] = line[x], o[1] = line[bpl + x], o[2] = line[2 * size_t(bpl) + x];
                if (planes == 4) o[3] = line[3 * size_t(bpl) + x];
                continue;
            }
            uint32_t idx = 0;
            if (planes == 1) {
                const uint32_t bit = x * bpp;
                idx = (line[bit >> 3] >> (8 - bpp - (bit & 7))) & ((1u << bpp) - 1);
            } else {  // plans de 1 bit (EGA)
                for (uint32_t p = 0; p < planes; ++p) idx |= uint32_t((line[p * size_t(bpl) + (x >> 3)] >> (7 - (x & 7))) & 1) << p;
            }
            o[0] = pal[idx][0], o[1] = pal[idx][1], o[2] = pal[idx][2];
        }
    }
    wchar_t fmt[256];
    if (rgb) swprintf_s(fmt, CYIMG_TR("plugin.classic.pcx.format_bits", L"PCX %d bits"), planes * 8);
    else swprintf_s(fmt, CYIMG_TR("plugin.classic.pcx.format_colors", L"PCX %d couleurs"), palSize);
    d.Format(fmt);
    wchar_t v[256];
    swprintf_s(v, L"%u", unsigned(ver));
    d.Info(CYIMG_TR("plugin.classic.pcx.info.version", L"Version PCX"), v);
    return Emit(d, img, rgb && planes == 4);
}

}  // namespace classic
