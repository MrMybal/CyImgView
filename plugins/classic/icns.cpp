// ICNS (icônes Apple) : choisit la plus grande icône (ou la plus proche de la vignette demandée).
// Entrées PNG / JPEG 2000 confiées à l'hôte (WIC), RLE RVB it32/ih32/il32/is32 + masques
// t8mk/h8mk/l8mk/s8mk, ARGB RLE ic04/ic05/icsb. Le JPEG 2000 n'est utilisé qu'en dernier recours.

#include "classic.h"

#include <algorithm>

namespace classic {
namespace {

enum class Kind { Png, Jp2, Argb, Rgb, Raw };

struct Entry {
    char type[5] = {};
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t dim = 0;
    Kind kind = Kind::Rgb;
};

uint32_t DimOf(const char* t) {
    static const struct {
        const char* t;
        uint32_t d;
    } k[] = {{"is32", 16}, {"il32", 32}, {"ih32", 48}, {"it32", 128}, {"icp4", 16}, {"icp5", 32},
             {"icp6", 64}, {"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024}, {"ic11", 32},
             {"ic12", 64}, {"ic13", 256}, {"ic14", 512}, {"ic04", 16}, {"ic05", 32}, {"ic06", 64},
             {"icsb", 18}, {"icsB", 36}, {"sb24", 24}, {"SB24", 48}};
    for (auto& e : k)
        if (!memcmp(t, e.t, 4)) return e.d;
    return 0;
}

const char* MaskFor(uint32_t dim) {
    switch (dim) {
        case 16: return "s8mk";
        case 32: return "l8mk";
        case 48: return "h8mk";
        case 128: return "t8mk";
        default: return nullptr;
    }
}

// RLE ICNS : c < 128 => c+1 octets littéraux ; c >= 128 => (c - 125) répétitions de l'octet suivant
bool Unrle(const uint8_t* s, size_t n, size_t& pos, uint8_t* out, size_t count, size_t outStride) {
    size_t o = 0;
    while (o < count) {
        if (pos >= n) return false;
        const uint8_t c = s[pos++];
        if (c & 0x80) {
            const size_t cnt = size_t(c) - 125;
            if (pos >= n || o + cnt > count) return false;
            const uint8_t v = s[pos++];
            for (size_t k = 0; k < cnt; ++k) out[(o + k) * outStride] = v;
            o += cnt;
        } else {
            const size_t cnt = size_t(c) + 1;
            if (pos + cnt > n || o + cnt > count) return false;
            for (size_t k = 0; k < cnt; ++k) out[(o + k) * outStride] = s[pos++];
            o += cnt;
        }
    }
    return true;
}

}  // namespace

int DecodeIcns(Dec& d) {
    if (d.n < 8 || memcmp(d.d, "icns", 4) != 0) return d.Fail(L"Fichier ICNS non reconnu");
    const size_t end = std::min<size_t>(d.n, BE32(d.d + 4));
    std::vector<Entry> list, masks;
    for (size_t pos = 8; pos + 8 <= end;) {
        const size_t len = BE32(d.d + pos + 4);
        if (len < 8 || len > end - pos) break;
        Entry e;
        memcpy(e.type, d.d + pos, 4);
        e.data = d.d + pos + 8;
        e.size = len - 8;
        pos += len;
        if (!strcmp(e.type, "s8mk") || !strcmp(e.type, "l8mk") || !strcmp(e.type, "h8mk") || !strcmp(e.type, "t8mk")) {
            masks.push_back(e);
            continue;
        }
        e.dim = DimOf(e.type);
        if (!e.dim || e.size < 4) continue;
        static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        if (e.size >= 24 && !memcmp(e.data, png, 8)) {
            e.kind = Kind::Png;
            const uint32_t pw = BE32(e.data + 16);
            if (pw && pw <= 65535) e.dim = pw;
        } else if ((e.size >= 12 && !memcmp(e.data, "\0\0\0\x0CjP  ", 8)) || (e.data[0] == 0xFF && e.data[1] == 0x4F)) {
            e.kind = Kind::Jp2;
        } else if (!memcmp(e.data, "ARGB", 4)) {
            e.kind = Kind::Argb;
        } else if (!memcmp(e.type + 2, "32", 2)) {
            e.kind = e.size == size_t(e.dim) * e.dim * 4 ? Kind::Raw : Kind::Rgb;
        } else {
            continue;  // format inconnu pour ce type
        }
        list.push_back(e);
    }
    if (list.empty()) return d.Fail(L"Aucune icône exploitable dans ce fichier ICNS");

    // Ordre de préférence : JPEG 2000 en dernier ; vignette = la plus petite assez grande ; sinon la plus grande
    const uint32_t target = d.Target();
    auto score = [&](const Entry& e) {
        int64_t s = e.kind == Kind::Jp2 ? -(1ll << 40) : 0;
        if (target && e.dim >= target) s += (1ll << 30) - e.dim;  // assez grande : préférer la plus petite
        else s += int64_t(e.dim) * 4;
        if (e.kind == Kind::Png) s += 1;
        return s;
    };
    std::stable_sort(list.begin(), list.end(), [&](const Entry& a, const Entry& b) { return score(a) > score(b); });

    wchar_t fmt[64];
    for (const Entry& e : list) {
        if (d.Cancelled()) return CYIMG_E_CANCELLED;
        if (e.kind == Kind::Png || e.kind == Kind::Jp2) {
            if (d.host->decode_encoded(d.ctx, e.data, e.size) == CYIMG_OK) {
                swprintf_s(fmt, L"ICNS (%hs, %ls)", e.type, e.kind == Kind::Png ? L"PNG" : L"JPEG 2000");
                d.Format(fmt);
                return CYIMG_OK;
            }
            continue;
        }
        const uint32_t w = e.dim;
        Rgba img;
        img.Init(w, w, 255);
        const size_t count = size_t(w) * w;
        size_t pos = 0;
        bool ok = true;
        if (e.kind == Kind::Argb) {
            pos = 4;
            const int order[4] = {3, 0, 1, 2};  // A R G B -> RGBA
            for (int c = 0; c < 4 && ok; ++c) ok = Unrle(e.data, e.size, pos, img.px.data() + order[c], count, 4);
        } else if (e.kind == Kind::Raw) {
            for (size_t i = 0; i < count; ++i) memcpy(&img.px[i * 4], e.data + i * 4 + 1, 3);
        } else {
            if (!strcmp(e.type, "it32") && e.size >= 4 && !BE32(e.data)) pos = 4;  // 4 octets nuls en tête
            for (int c = 0; c < 3 && ok; ++c) ok = Unrle(e.data, e.size, pos, img.px.data() + c, count, 4);
        }
        if (!ok) continue;
        bool masked = false;
        if (e.kind != Kind::Argb)
            if (const char* mt = MaskFor(w))
                for (const Entry& m : masks)
                    if (!strcmp(m.type, mt) && m.size >= count) {
                        for (size_t i = 0; i < count; ++i) img.px[i * 4 + 3] = m.data[i];
                        masked = true;
                        break;
                    }
        swprintf_s(fmt, L"ICNS (%hs%ls)", e.type, masked ? L" + masque" : L"");
        d.Format(fmt);
        return Emit(d, img, masked || e.kind == Kind::Argb);
    }
    const bool onlyJp2 = std::all_of(list.begin(), list.end(), [](const Entry& e) { return e.kind == Kind::Jp2; });
    return d.Fail(onlyJp2 ? L"Icônes ICNS en JPEG 2000 : aucun décodeur JPEG 2000 n'est installé dans Windows"
                          : L"Icônes ICNS invalides ou tronquées");
}

}  // namespace classic
