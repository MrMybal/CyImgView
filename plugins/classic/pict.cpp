// PICT (QuickDraw, Mac) : versions 1 et 2, en-tête optionnel de 512 octets. Seules les images bitmap
// sont rendues : BitsRect/BitsRgn, PackBitsRect/PackBitsRgn (1-8 bits indexés), DirectBitsRect/Rgn
// (16/32 bits), et images QuickTime compressées (JPEG/PNG embarqué confié à l'hôte). Les bandes
// successives (Photoshop) sont assemblées ; le dessin vectoriel est ignoré.

#include "classic.h"

#include <algorithm>

namespace classic {
namespace {

struct Rect {
    int top = 0, left = 0, bottom = 0, right = 0;
    int W() const { return right - left; }
    int H() const { return bottom - top; }
};

struct Reader {
    const uint8_t* p;
    size_t n, pos;
    bool ok = true;
    bool Has(size_t k) {
        if (!ok || k > n - pos) ok = false;
        return ok;
    }
    uint8_t U8() { return Has(1) ? p[pos++] : 0; }
    uint16_t U16() {
        if (!Has(2)) return 0;
        pos += 2;
        return BE16(p + pos - 2);
    }
    uint32_t U32() {
        if (!Has(4)) return 0;
        pos += 4;
        return BE32(p + pos - 4);
    }
    void Skip(size_t k) {
        if (Has(k)) pos += k;
    }
    Rect R() {
        Rect r;
        r.top = int16_t(U16()), r.left = int16_t(U16()), r.bottom = int16_t(U16()), r.right = int16_t(U16());
        return r;
    }
};

struct Bitmap {
    Rgba img;  // taille des limites (bounds)
    Rect bounds, src, dst;
};

// PackBits sur des unités de `unit` octets (1, ou 2 pour les pixels 16 bits)
bool UnpackBits(const uint8_t* s, size_t n, uint8_t* out, size_t need, size_t unit) {
    size_t pos = 0, o = 0;
    while (o < need && pos < n) {
        const int8_t c = int8_t(s[pos++]);
        if (c >= 0) {
            const size_t cnt = (size_t(c) + 1) * unit;
            if (pos + cnt > n) return false;
            const size_t k = std::min(cnt, need - o);
            memcpy(out + o, s + pos, k);
            pos += cnt, o += k;
        } else if (c != -128) {
            const size_t cnt = size_t(1 - c);
            if (pos + unit > n) return false;
            for (size_t i = 0; i < cnt && o < need; ++i)
                for (size_t b = 0; b < unit && o < need; ++b) out[o++] = s[pos + b];
            pos += unit;
        }
    }
    return true;  // ligne incomplète tolérée (reste à zéro)
}

// Lit une PixMap/BitMap avec ses pixels (opcodes 0x90-0x9B et motifs PixPat).
// withRects = false pour un motif (pas de rectangles source/destination ni de mode).
bool ReadPixmap(Reader& r, uint16_t op, bool withRects, Bitmap* out) {
    const bool direct = op == 0x9A || op == 0x9B;
    if (direct) r.Skip(4);  // baseAddr
    const uint16_t rbRaw = r.U16();
    const bool pixmap = direct || (rbRaw & 0x8000);
    const size_t rowBytes = rbRaw & 0x3FFF;
    const Rect b = r.R();
    uint16_t packType = 0, pixelSize = 1, cmpCount = 1;
    uint8_t pal[256][3];
    for (int i = 0; i < 256; ++i) pal[i][0] = pal[i][1] = pal[i][2] = uint8_t(255 - i);  // défaut : 0 blanc
    if (pixmap) {
        r.U16();  // pmVersion
        packType = r.U16();
        r.Skip(12);  // packSize, hRes, vRes
        r.U16();     // pixelType
        pixelSize = r.U16();
        cmpCount = r.U16();
        r.Skip(2 + 12);  // cmpSize, planeBytes, pmTable, pmReserved
        if (!direct) {
            r.Skip(4);
            const uint16_t flags = r.U16();
            const uint32_t cnt = uint32_t(r.U16()) + 1;
            if (cnt > 4096) return r.ok = false;
            for (uint32_t i = 0; i < cnt && r.ok; ++i) {
                const uint16_t v = r.U16();
                const uint8_t c[3] = {uint8_t(r.U16() >> 8), uint8_t(r.U16() >> 8), uint8_t(r.U16() >> 8)};
                const uint32_t idx = (flags & 0x8000) ? i : v;
                if (idx < 256) memcpy(pal[idx], c, 3);
            }
        }
    } else {
        pal[0][0] = pal[0][1] = pal[0][2] = 255;
        pal[1][0] = pal[1][1] = pal[1][2] = 0;
    }
    Rect src, dst;
    if (withRects) {
        src = r.R(), dst = r.R();
        r.U16();  // mode
        if (op == 0x91 || op == 0x99 || op == 0x9B) {
            const uint16_t len = r.U16();
            if (len < 2) return r.ok = false;
            r.Skip(len - 2u);
        }
    }
    if (!r.ok) return false;
    const int w = b.W(), h = b.H();
    if (w <= 0 || h <= 0 || !SizeOk(uint32_t(w), uint32_t(h))) return r.ok = false;
    if (pixelSize != 1 && pixelSize != 2 && pixelSize != 4 && pixelSize != 8 && pixelSize != 16 && pixelSize != 32)
        return r.ok = false;
    if (pixelSize == 32 && packType == 0) packType = 4;
    if (pixelSize == 16 && packType == 0) packType = 3;
    // Taille d'une ligne décompressée
    size_t lineLen = rowBytes;
    if (pixelSize == 32 && packType == 2) lineLen = size_t(w) * 3;
    else if (pixelSize == 32 && packType == 4) lineLen = size_t(w) * cmpCount;
    if (rowBytes == 0 || (pixelSize < 32 && rowBytes * 8 < size_t(w) * pixelSize) || (pixelSize == 32 && cmpCount < 3))
        return r.ok = false;
    const bool packed = op >= 0x98 && rowBytes >= 8 && packType != 1 && packType != 2;  // 0x90/0x91 : jamais compressé
    std::vector<uint8_t> line(lineLen);
    if (out) {
        out->img.Init(uint32_t(w), uint32_t(h), 255);
        out->bounds = b, out->src = src, out->dst = dst;
    }
    for (int y = 0; y < h; ++y) {
        if (!packed) {
            if (!r.Has(lineLen)) return false;
            memcpy(line.data(), r.p + r.pos, lineLen);
            r.pos += lineLen;
        } else {
            const size_t cnt = rowBytes > 250 ? r.U16() : r.U8();
            if (!r.Has(cnt)) return false;
            std::fill(line.begin(), line.end(), uint8_t(0));
            if (!UnpackBits(r.p + r.pos, cnt, line.data(), lineLen, pixelSize == 16 ? 2 : 1)) return r.ok = false;
            r.pos += cnt;
        }
        if (!out) continue;
        uint8_t* o = out->img.Row(uint32_t(y));
        for (int x = 0; x < w; ++x, o += 4) {
            if (pixelSize == 32) {
                if (packType == 4) {
                    const size_t off = cmpCount >= 4 ? size_t(w) : 0;  // plan alpha ignoré
                    o[0] = line[off + x], o[1] = line[off + w + x], o[2] = line[off + 2 * size_t(w) + x];
                } else if (packType == 2) {
                    memcpy(o, &line[size_t(x) * 3], 3);
                } else {
                    memcpy(o, &line[size_t(x) * 4 + 1], 3);  // xRGB
                }
            } else if (pixelSize == 16) {
                const uint16_t v = BE16(&line[size_t(x) * 2]);
                const uint8_t rr = (v >> 10) & 31, gg = (v >> 5) & 31, bb = v & 31;
                o[0] = uint8_t(rr << 3 | rr >> 2), o[1] = uint8_t(gg << 3 | gg >> 2), o[2] = uint8_t(bb << 3 | bb >> 2);
            } else {
                const size_t bit = size_t(x) * pixelSize;
                const uint32_t idx = (line[bit >> 3] >> (8 - pixelSize - (bit & 7))) & ((1u << pixelSize) - 1);
                o[0] = pal[idx][0], o[1] = pal[idx][1], o[2] = pal[idx][2];
            }
        }
    }
    return r.ok;
}

// Motif PixPat (opcodes 0x12-0x14) : seulement à sauter
void SkipPixPat(Reader& r) {
    const uint16_t type = r.U16();
    r.Skip(8);
    if (type == 1) {
        // PixMap sans baseAddr, avec rowBytes marqué 0x8000
        ReadPixmap(r, 0x98, false, nullptr);
    } else if (type == 2) {
        r.Skip(6);
    }
}

// Cherche un flux JPEG ou PNG dans une image QuickTime
bool FindEmbedded(const uint8_t* s, size_t n, const uint8_t*& start, size_t& size) {
    for (size_t i = 0; i + 8 <= n; ++i) {
        if ((s[i] == 0xFF && s[i + 1] == 0xD8 && s[i + 2] == 0xFF) || (s[i] == 0x89 && !memcmp(s + i + 1, "PNG\r\n", 5))) {
            start = s + i, size = n - i;
            return true;
        }
    }
    return false;
}

bool VersionAt(const uint8_t* p, size_t n, size_t base, int& ver) {
    if (n < base + 14) return false;
    const uint8_t* v = p + base + 10;
    if (v[0] == 0x00 && v[1] == 0x11 && v[2] == 0x02 && v[3] == 0xFF) ver = 2;
    else if (v[0] == 0x11 && v[1] == 0x01) ver = 1;
    else return false;
    const int top = int16_t(BE16(p + base + 2)), left = int16_t(BE16(p + base + 4));
    const int bottom = int16_t(BE16(p + base + 6)), right = int16_t(BE16(p + base + 8));
    return bottom > top && right > left;
}

}  // namespace

bool IsPict(const uint8_t* p, size_t n) {
    int v;
    return VersionAt(p, n, 0, v) || VersionAt(p, n, 512, v);
}

int DecodePict(Dec& d) {
    int ver = 0;
    size_t base = 0;
    if (!VersionAt(d.d, d.n, 512, ver)) {
        if (!VersionAt(d.d, d.n, 0, ver)) return d.Fail(CYIMG_TR("plugin.classic.pict.err.unrecognized", L"Fichier PICT non reconnu"));
    } else {
        base = 512;
    }
    Reader r{d.d, d.n, base + 10};
    if (ver == 2) r.Skip(4);
    else r.Skip(2);

    std::vector<Bitmap> maps;
    const uint8_t* qtData = nullptr;
    size_t qtSize = 0;
    bool qtSeen = false, vector = false, truncated = false;
    for (int guard = 0; r.ok; ++guard) {
        if (ver == 2 && ((r.pos - base) & 1)) r.Skip(1);
        if (r.pos >= r.n) {
            truncated = true;
            break;
        }
        const uint16_t op = ver == 2 ? r.U16() : r.U8();
        if (!r.ok) break;
        if ((guard & 1023) == 0 && d.Cancelled()) return CYIMG_E_CANCELLED;
        if (op == 0x00FF) break;  // fin de l'image
        if (op >= 0x90 && op <= 0x9B && (op <= 0x91 || op >= 0x98)) {
            Bitmap bm;
            if (ReadPixmap(r, op, true, &bm)) maps.push_back(std::move(bm));
            else truncated = true;
            continue;
        }
        if (op == 0x8200 || op == 0x8201) {
            const uint32_t len = r.U32();
            if (!r.Has(len)) {
                truncated = true;
                break;
            }
            qtSeen = true;
            const uint8_t* s;
            size_t sz;
            if (op == 0x8200 && FindEmbedded(r.p + r.pos, len, s, sz) && sz > qtSize) qtData = s, qtSize = sz;
            r.pos += len;
            continue;
        }
        if (op >= 0x20 && op <= 0x8F) vector = true;  // lignes, rectangles, ovales, polygones...
        switch (op) {
            case 0x00: case 0x17: case 0x18: case 0x19: case 0x1C: case 0x1E: break;
            case 0x01: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
            case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: {
                const uint16_t len = r.U16();  // région / polygone : taille incluse
                if (len < 2) r.ok = false;
                else r.Skip(len - 2u);
                break;
            }
            case 0x03: case 0x05: case 0x08: case 0x0D: case 0x15: case 0x16: case 0x23: case 0xA0: r.Skip(2); break;
            case 0x04: r.Skip(1); break;
            case 0x06: case 0x07: case 0x0B: case 0x0C: case 0x0E: case 0x0F: case 0x21: r.Skip(4); break;
            case 0x02: case 0x09: case 0x0A: case 0x10: case 0x20: r.Skip(8); break;
            case 0x11: r.Skip(ver == 2 ? 2 : 1); break;
            case 0x12: case 0x13: case 0x14: SkipPixPat(r); break;
            case 0x1A: case 0x1B: case 0x1D: case 0x1F: case 0x22: r.Skip(6); break;
            case 0x28: r.Skip(4); r.Skip(r.U8()); break;
            case 0x29: case 0x2A: r.Skip(1); r.Skip(r.U8()); break;
            case 0x2B: r.Skip(2); r.Skip(r.U8()); break;
            case 0xA1: r.Skip(2); r.Skip(r.U16()); break;
            default:
                if ((op >= 0x24 && op <= 0x27) || (op >= 0x2C && op <= 0x2F) || (op >= 0x92 && op <= 0x97) ||
                    (op >= 0x9C && op <= 0x9F) || (op >= 0xA2 && op <= 0xAF))
                    r.Skip(r.U16());
                else if ((op >= 0x30 && op <= 0x37) || (op >= 0x40 && op <= 0x47) || (op >= 0x50 && op <= 0x57))
                    r.Skip(8);
                else if ((op >= 0x38 && op <= 0x3F) || (op >= 0x48 && op <= 0x4F) || (op >= 0x58 && op <= 0x5F) ||
                         (op >= 0x78 && op <= 0x7F) || (op >= 0x88 && op <= 0x8F) || (op >= 0xB0 && op <= 0xCF))
                    break;
                else if (op >= 0x60 && op <= 0x67) r.Skip(12);
                else if (op >= 0x68 && op <= 0x6F) r.Skip(4);
                else if (op >= 0xD0 && op <= 0xFE) r.Skip(r.U32());
                else if (op >= 0x100 && op <= 0x7FFF) r.Skip(size_t(op >> 8) * 2);
                else if (op >= 0x8000 && op <= 0x80FF) break;
                else if (op >= 0x8100) r.Skip(r.U32());
                break;
        }
        if (!r.ok) truncated = true;
    }

    // 1) image QuickTime JPEG/PNG
    if (qtData && d.host->decode_encoded(d.ctx, qtData, qtSize) == CYIMG_OK) {
        d.Format(ver == 2 ? L"PICT 2 (QuickTime)" : L"PICT (QuickTime)");
        return CYIMG_OK;
    }
    if (maps.empty()) {
        if (truncated) return d.Fail(CYIMG_TR("plugin.classic.pict.err.truncated", L"Fichier PICT tronqué ou invalide (aucune image bitmap lue)"));
        if (qtSeen) return d.Fail(CYIMG_TR("plugin.classic.pict.err.qt", L"PICT QuickTime : compression non prise en charge (seuls JPEG et PNG le sont)"));
        return d.Fail(vector ? CYIMG_TR("plugin.classic.pict.err.vector", L"PICT purement vectoriel : aucune image bitmap à afficher")
                             : CYIMG_TR("plugin.classic.pict.err.no_bitmap", L"Ce fichier PICT ne contient aucune image bitmap"));
    }

    // 2) assemblage des bitmaps : toile = union des rectangles de destination, à la résolution source
    Rect u = maps[0].dst;
    for (const Bitmap& m : maps) {
        u.top = std::min(u.top, m.dst.top), u.left = std::min(u.left, m.dst.left);
        u.bottom = std::max(u.bottom, m.dst.bottom), u.right = std::max(u.right, m.dst.right);
    }
    const Bitmap& m0 = maps[0];
    double scale = 1.0;
    if (m0.dst.W() > 0 && m0.src.W() > 0) scale = std::clamp(double(m0.src.W()) / m0.dst.W(), 0.05, 20.0);
    const int64_t cw = int64_t(u.W() * scale + 0.5), ch = int64_t(u.H() * scale + 0.5);
    if (cw <= 0 || ch <= 0 || !SizeOk(uint64_t(cw), uint64_t(ch))) {
        // Rectangles incohérents : on affiche la plus grande bitmap telle quelle
        const Bitmap* best = &maps[0];
        for (const Bitmap& m : maps)
            if (uint64_t(m.img.w) * m.img.h > uint64_t(best->img.w) * best->img.h) best = &m;
        d.Format(ver == 2 ? L"PICT 2 (bitmap)" : L"PICT 1 (bitmap)");
        return Emit(d, best->img, false);
    }
    Rgba canvas;
    canvas.Init(uint32_t(cw), uint32_t(ch), 255);
    for (const Bitmap& m : maps) {
        if (m.dst.W() <= 0 || m.dst.H() <= 0 || m.src.W() <= 0 || m.src.H() <= 0) continue;
        const int x0 = int((m.dst.left - u.left) * scale + 0.5), x1 = int((m.dst.right - u.left) * scale + 0.5);
        const int y0 = int((m.dst.top - u.top) * scale + 0.5), y1 = int((m.dst.bottom - u.top) * scale + 0.5);
        if (x1 <= x0 || y1 <= y0) continue;
        for (int y = std::max(0, y0); y < std::min<int>(int(ch), y1); ++y) {
            const int sy = m.src.top + int(int64_t(y - y0) * m.src.H() / (y1 - y0)) - m.bounds.top;
            if (sy < 0 || sy >= int(m.img.h)) continue;
            uint8_t* o = canvas.Row(uint32_t(y));
            const uint8_t* srow = m.img.px.data() + size_t(sy) * m.img.w * 4;
            for (int x = std::max(0, x0); x < std::min<int>(int(cw), x1); ++x) {
                const int sx = m.src.left + int(int64_t(x - x0) * m.src.W() / (x1 - x0)) - m.bounds.left;
                if (sx < 0 || sx >= int(m.img.w)) continue;
                memcpy(o + size_t(x) * 4, srow + size_t(sx) * 4, 4);
            }
        }
    }
    d.Format(ver == 2 ? L"PICT 2 (bitmap)" : L"PICT 1 (bitmap)");
    if (maps.size() > 1) {
        wchar_t v[256];
        swprintf_s(v, L"%zu", maps.size());
        d.Info(CYIMG_TR("plugin.classic.pict.info.bands", L"Bandes bitmap"), v);
    }
    if (vector) d.Info(CYIMG_TR("plugin.classic.pict.info.vector", L"Dessin vectoriel"), CYIMG_TR("plugin.classic.pict.ignored", L"ignoré"));
    if (truncated) d.Info(CYIMG_TR("plugin.classic.info.warning", L"Avertissement"), CYIMG_TR("plugin.classic.truncated", L"fichier tronqué"));
    return Emit(d, canvas, false);
}

}  // namespace classic
