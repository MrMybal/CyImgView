// GIMP XCF : composition des calques visibles (fond transparent).
//
// Versions 0 à 11+ (pointeurs 64 bits à partir de la version 11), précisions 8/16/32 bits entières,
// demi-flottants, flottants et doubles (linéaires ou non), compressions aucune / RLE / zlib, tuiles
// 64×64, opacité, masques de calque, groupes (visibilité et opacité héritées). Tous les modes de
// fusion sont traités comme « normal ».

#include "docs_common.h"

#include "inflate.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <thread>

namespace docs {
namespace {

enum : uint32_t {
    PROP_END = 0,
    PROP_COLORMAP = 1,
    PROP_OPACITY = 6,
    PROP_MODE = 7,
    PROP_VISIBLE = 8,
    PROP_APPLY_MASK = 11,
    PROP_OFFSETS = 15,
    PROP_COMPRESSION = 17,
    PROP_GROUP_ITEM = 29,
    PROP_ITEM_PATH = 30,
    PROP_FLOAT_OPACITY = 33,
};

struct Fail {
    const wchar_t* msg;
};

// Format d'un composant selon la précision
struct Precision {
    int bytes = 1;      // 1, 2, 4, 8
    bool isFloat = false;
    bool linear = false;
};

bool GetPrecision(uint32_t version, uint32_t v, Precision& p) {
    if (version < 4) return true;  // 8 bits gamma
    if (version == 4) {
        switch (v) {
        case 0: p = {1, false, false}; return true;
        case 1: p = {2, false, false}; return true;
        case 2: p = {4, false, true}; return true;
        case 3: p = {2, true, true}; return true;
        case 4: p = {4, true, true}; return true;
        default: return false;
        }
    }
    const bool lin = (v % 100) == 0;
    switch (v - v % 100) {
    case 100: p = {1, false, lin}; return true;
    case 200: p = {2, false, lin}; return true;
    case 300: p = {4, false, lin}; return true;
    case 400: p = version <= 6 ? Precision{2, true, lin} : Precision{}; return version <= 6;
    case 500: p = version <= 6 ? Precision{4, true, lin} : Precision{2, true, lin}; return true;
    case 600: p = {4, true, lin}; return version >= 7;
    case 700: p = {8, true, lin}; return version >= 7;
    default: return false;
    }
}

float HalfToFloat(uint16_t h) {
    const uint32_t s = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023;
    uint32_t u;
    if (e == 0) {
        if (!m) {
            u = s;
        } else {  // dénormalisé
            int ee = -1;
            uint32_t mm = m;
            do {
                ++ee;
                mm <<= 1;
            } while (!(mm & 1024));
            u = s | uint32_t(127 - 15 - ee) << 23 | (mm & 1023) << 13;
        }
    } else if (e == 31) {
        u = s | 0x7F800000 | m << 13;
    } else {
        u = s | (e + 127 - 15) << 23 | m << 13;
    }
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// Découpe [0, n) en tranches traitées en parallèle : fn(début, fin).
template <class Fn>
void ParallelFor(uint32_t n, Fn&& fn) {
    const uint32_t nt = std::min(std::clamp<uint32_t>(std::thread::hardware_concurrency(), 1, 8), std::max<uint32_t>(1, n / 8));
    if (nt <= 1) {
        fn(0u, n);
        return;
    }
    std::vector<std::thread> th;
    for (uint32_t t = 0; t < nt; ++t)
        th.emplace_back([&, t] { fn(uint32_t(uint64_t(n) * t / nt), uint32_t(uint64_t(n) * (t + 1) / nt)); });
    for (auto& x : th) x.join();
}

struct SrgbLut {
    uint8_t t[4097];
    SrgbLut() {
        for (int i = 0; i <= 4096; ++i) {
            const double v = i / 4096.0;
            const double s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            t[i] = uint8_t(std::clamp(s * 255 + 0.5, 0.0, 255.0));
        }
    }
};

class Xcf {
public:
    Xcf(const Job& job, const uint8_t* f, size_t n) : m_job(job), m_f(f), m_n(n) {}

    int Run();

private:
    struct Layer {
        uint32_t w = 0, h = 0, type = 0;
        int32_t ox = 0, oy = 0;
        bool visible = true, group = false, applyMask = true;
        float opacity = 1;
        uint32_t mode = 0;
        std::vector<uint32_t> path;
        uint64_t hier = 0, mask = 0;
    };

    uint32_t U32(uint64_t pos) const {
        if (pos > m_n || m_n - pos < 4) throw Fail{CYIMG_TR("plugin.docs.xcf.err.truncated", L"GIMP : fichier tronqué")};
        return Be32(m_f + pos);
    }
    uint64_t Ptr(uint64_t pos) const {
        if (!m_ptr64) return U32(pos);
        if (pos > m_n || m_n - pos < 8) throw Fail{CYIMG_TR("plugin.docs.xcf.err.truncated", L"GIMP : fichier tronqué")};
        return Be64(m_f + pos);
    }
    // Propriétés : appelle fn(type, début, longueur), retourne la position après PROP_END
    template <class Fn>
    uint64_t Props(uint64_t pos, Fn&& fn) {
        for (int guard = 0; guard < 100000; ++guard) {
            const uint32_t type = U32(pos), len = U32(pos + 4);
            pos += 8;
            if (type == PROP_END) return pos;
            if (len > m_n - pos) throw Fail{CYIMG_TR("plugin.docs.xcf.err.property", L"GIMP : propriété invalide")};
            fn(type, pos, len);
            pos += len;
        }
        throw Fail{CYIMG_TR("plugin.docs.xcf.err.properties", L"GIMP : propriétés invalides")};
    }
    void ReadLayer(uint64_t pos, Layer& L);
    // Pixels d'une hiérarchie (premier niveau) : composants entrelacés, w*h*bpp octets
    bool ReadHierarchy(uint64_t pos, uint32_t w, uint32_t h, uint32_t comps, std::vector<uint8_t>& out);
    bool ReadTile(uint64_t start, uint64_t end, uint32_t tw, uint32_t th, uint32_t bpp, uint8_t* out);
    // Composant i d'un pixel -> 8 bits (gamma sRGB si couleur)
    uint8_t Comp(const uint8_t* p, bool color) const;

    const Job& m_job;
    const uint8_t* m_f;
    size_t m_n;
    uint32_t m_version = 0, m_w = 0, m_h = 0, m_base = 0, m_comp = 0;
    bool m_ptr64 = false;
    Precision m_prec;
    std::vector<uint8_t> m_cmap;  // RVB
    SrgbLut m_lut;
};

void Xcf::ReadLayer(uint64_t pos, Layer& L) {
    L.w = U32(pos), L.h = U32(pos + 4), L.type = U32(pos + 8);
    const uint32_t nameLen = U32(pos + 12);
    if (nameLen > m_n - pos - 16) throw Fail{CYIMG_TR("plugin.docs.xcf.err.layer", L"GIMP : calque invalide")};
    pos += 16 + nameLen;
    pos = Props(pos, [&](uint32_t type, uint64_t p, uint32_t len) {
        const uint8_t* d = m_f + p;
        switch (type) {
        case PROP_OPACITY:
            if (len >= 4) L.opacity = std::min(Be32(d), 255u) / 255.0f;
            break;
        case PROP_FLOAT_OPACITY:
            if (len >= 4) {
                const uint32_t u = Be32(d);
                float v;
                memcpy(&v, &u, 4);
                if (v >= 0 && v <= 1) L.opacity = v;
            }
            break;
        case PROP_VISIBLE: if (len >= 4) L.visible = Be32(d) != 0; break;
        case PROP_APPLY_MASK: if (len >= 4) L.applyMask = Be32(d) != 0; break;
        case PROP_MODE: if (len >= 4) L.mode = Be32(d); break;
        case PROP_OFFSETS:
            if (len >= 8) L.ox = int32_t(Be32(d)), L.oy = int32_t(Be32(d + 4));
            break;
        case PROP_GROUP_ITEM: L.group = true; break;
        case PROP_ITEM_PATH:
            for (uint32_t i = 0; i + 4 <= len && i < 4096; i += 4) L.path.push_back(Be32(d + i));
            break;
        default: break;
        }
    });
    L.hier = Ptr(pos);
    L.mask = Ptr(pos + (m_ptr64 ? 8 : 4));
}

bool Xcf::ReadTile(uint64_t start, uint64_t end, uint32_t tw, uint32_t th, uint32_t bpp, uint8_t* out) {
    if (start >= m_n) return false;
    end = std::min<uint64_t>(end, m_n);
    if (end <= start) return false;
    const uint8_t* s = m_f + start;
    const size_t n = size_t(end - start), count = size_t(tw) * th, total = count * bpp;
    switch (m_comp) {
    case 0:
        if (n < total) return false;
        memcpy(out, s, total);
        return true;
    case 1: {  // RLE par plan d'octets
        size_t i = 0;
        for (uint32_t k = 0; k < bpp; ++k) {
            size_t px = 0;
            while (px < count) {
                if (i >= n) return false;
                uint32_t op = s[i++];
                if (op >= 128) {  // suite d'octets différents
                    size_t len;
                    if (op == 128) {
                        if (i + 2 > n) return false;
                        len = size_t(s[i]) << 8 | s[i + 1];
                        i += 2;
                    } else {
                        len = 256 - op;
                    }
                    if (px + len > count || i + len > n) return false;
                    for (size_t j = 0; j < len; ++j) out[(px + j) * bpp + k] = s[i + j];
                    i += len;
                    px += len;
                } else {  // répétition
                    size_t len;
                    if (op == 127) {
                        if (i + 2 > n) return false;
                        len = size_t(s[i]) << 8 | s[i + 1];
                        i += 2;
                    } else {
                        len = op + 1;
                    }
                    if (px + len > count || i >= n) return false;
                    const uint8_t v = s[i++];
                    for (size_t j = 0; j < len; ++j) out[(px + j) * bpp + k] = v;
                    px += len;
                }
            }
        }
        return true;
    }
    case 2: return ZlibDecompress(s, n, out, total);
    default: return false;
    }
}

bool Xcf::ReadHierarchy(uint64_t pos, uint32_t w, uint32_t h, uint32_t comps, std::vector<uint8_t>& out) {
    const uint32_t hw = U32(pos), hh = U32(pos + 4), bpp = U32(pos + 8);
    if (hw != w || hh != h || bpp != comps * uint32_t(m_prec.bytes)) return false;
    const uint64_t level = Ptr(pos + 12);
    if (U32(level) != w || U32(level + 4) != h) return false;
    const uint32_t ntx = (w + 63) / 64, nty = (h + 63) / 64;
    const uint64_t ntiles = uint64_t(ntx) * nty;
    const uint64_t ptrs = level + 8, ps = m_ptr64 ? 8 : 4;
    if (ptrs + ntiles * ps > m_n) return false;
    if (uint64_t(w) * h * bpp > (uint64_t(4) << 30)) return false;
    if (ntiles > 0x7FFFFFFF) return false;
    std::vector<uint64_t> tp(size_t(ntiles) + 1);
    for (uint64_t t = 0; t <= ntiles; ++t) {  // pointeurs vérifiés avant d'allouer
        tp[size_t(t)] = Ptr(ptrs + t * ps);  // le dernier est 0 (fin de liste) ou un niveau suivant
        if (t < ntiles && (!tp[size_t(t)] || tp[size_t(t)] >= m_n)) return false;
    }
    out.assign(size_t(w) * h * bpp, 0);
    std::atomic<bool> bad{false}, cancelled{false};
    ParallelFor(uint32_t(ntiles), [&](uint32_t t0, uint32_t t1) {
        std::vector<uint8_t> tile(64 * 64 * size_t(bpp));
        for (uint32_t t = t0; t < t1 && !bad && !cancelled; ++t) {
            if ((t & 15) == 0 && m_job.Cancelled()) cancelled = true;
            const uint64_t start = tp[t];
            uint64_t end = tp[size_t(t) + 1];  // tuile suivante (ou 0 : fin de liste)
            if (end <= start) end = start + 64 * 64 * uint64_t(bpp) * 2 + 1024;
            const uint32_t tx = t % ntx, ty = t / ntx;
            const uint32_t tw = std::min(64u, w - tx * 64), th = std::min(64u, h - ty * 64);
            if (!ReadTile(start, end, tw, th, bpp, tile.data())) {
                bad = true;
                break;
            }
            for (uint32_t y = 0; y < th; ++y)
                memcpy(&out[((size_t(ty) * 64 + y) * w + size_t(tx) * 64) * bpp], &tile[size_t(y) * tw * bpp], size_t(tw) * bpp);
        }
    });
    if (cancelled) throw Fail{nullptr};
    return !bad;
}

uint8_t Xcf::Comp(const uint8_t* p, bool color) const {
    float v;
    switch (m_prec.bytes) {
    case 1:
        if (!m_prec.linear || !color) return p[0];
        v = p[0] / 255.0f;
        break;
    case 2:
        if (m_prec.isFloat) v = HalfToFloat(Be16(p));
        else if (!m_prec.linear || !color) return uint8_t((Be16(p) * 255u + 32767) / 65535);
        else v = Be16(p) / 65535.0f;
        break;
    case 4:
        if (m_prec.isFloat) {
            const uint32_t u = Be32(p);
            memcpy(&v, &u, 4);
        } else {
            v = float(Be32(p) / 4294967295.0);
        }
        break;
    default: {
        const uint64_t u = Be64(p);
        double dv;
        memcpy(&dv, &u, 8);
        v = float(dv);
    }
    }
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    if (m_prec.linear && color) return m_lut.t[int(v * 4096 + 0.5f)];
    return uint8_t(v * 255 + 0.5f);
}

int Xcf::Run() {
    m_job.host->set_format(m_job.ctx, L"GIMP XCF");
    if (m_n < 30) throw Fail{CYIMG_TR("plugin.docs.xcf.err.truncated", L"GIMP : fichier tronqué")};
    if (memcmp(m_f + 9, "file", 4) == 0) m_version = 0;
    else if (m_f[9] == 'v') m_version = uint32_t(atoi(reinterpret_cast<const char*>(m_f + 10)) & 0xFFFF);  // "v011\0"
    else throw Fail{CYIMG_TR("plugin.docs.xcf.err.version", L"GIMP : version inconnue")};
    if (m_f[13] != 0) throw Fail{CYIMG_TR("plugin.docs.xcf.err.header", L"GIMP : en-tête invalide")};
    m_ptr64 = m_version >= 11;
    m_w = U32(14), m_h = U32(18), m_base = U32(22);
    uint64_t pos = 26;
    if (m_version >= 4) {
        if (!GetPrecision(m_version, U32(26), m_prec)) throw Fail{CYIMG_TR("plugin.docs.xcf.err.precision", L"GIMP : précision non gérée")};
        pos = 30;
    }
    if (!m_w || !m_h || m_w > 65535 || m_h > 65535 || m_base > 2) throw Fail{CYIMG_TR("plugin.docs.xcf.err.header", L"GIMP : en-tête invalide")};
    if (uint64_t(m_w) * m_h > (uint64_t(1) << 29)) throw Fail{CYIMG_TR("plugin.docs.xcf.err.too_big", L"GIMP : image trop grande")};

    pos = Props(pos, [&](uint32_t type, uint64_t p, uint32_t len) {
        if (type == PROP_COLORMAP && len >= 4) {
            const uint32_t nc = std::min<uint32_t>(Be32(m_f + p), 256);
            if (4 + nc * 3 <= len) m_cmap.assign(m_f + p + 4, m_f + p + 4 + nc * 3);
        } else if (type == PROP_COMPRESSION && len >= 1) {
            m_comp = m_f[p];
        }
    });
    if (m_comp > 2) throw Fail{CYIMG_TR("plugin.docs.xcf.err.compression", L"GIMP : compression non gérée")};

    // Liste des calques (du haut vers le bas)
    std::vector<Layer> layers;
    int badHeaders = 0;
    for (int i = 0; i < 100000; ++i) {
        const uint64_t lp = Ptr(pos);
        pos += m_ptr64 ? 8 : 4;
        if (!lp) break;
        Layer L;
        try {
            ReadLayer(lp, L);
        } catch (const Fail&) {  // en-tête de calque tronqué : les calques lisibles sont gardés
            ++badHeaders;
            continue;
        }
        layers.push_back(std::move(L));
    }

    wchar_t buf[256];
    const wchar_t* const bases[] = {CYIMG_TR("plugin.docs.xcf.mode.rgb", L"RVB"), CYIMG_TR("plugin.docs.xcf.mode.gray", L"Niveaux de gris"),
                                    CYIMG_TR("plugin.docs.xcf.mode.indexed", L"Couleurs indexées")};
    m_job.Info(CYIMG_TR("plugin.docs.info.mode", L"Mode"), bases[m_base]);
    const wchar_t* precFmt = m_prec.isFloat ? (m_prec.linear ? CYIMG_TR("plugin.docs.xcf.prec_float_linear", L"%d bits flottant linéaire")
                                                             : CYIMG_TR("plugin.docs.xcf.prec_float", L"%d bits flottant"))
                                            : (m_prec.linear ? CYIMG_TR("plugin.docs.xcf.prec_linear", L"%d bits linéaire")
                                                             : CYIMG_TR("plugin.docs.xcf.prec_int", L"%d bits"));
    swprintf_s(buf, precFmt, m_prec.bytes * 8);
    m_job.Info(CYIMG_TR("plugin.docs.info.precision", L"Précision"), buf);
    swprintf_s(buf, L"%zu", layers.size());
    m_job.Info(CYIMG_TR("plugin.docs.info.layers", L"Calques"), buf);
    swprintf_s(buf, L"%u", m_version);
    m_job.Info(CYIMG_TR("plugin.docs.info.xcf_version", L"Version XCF"), buf);

    // Groupes : visibilité et opacité héritées via le chemin de l'élément
    std::map<std::vector<uint32_t>, const Layer*> groups;
    for (const auto& L : layers)
        if (L.group && !L.path.empty()) groups[L.path] = &L;

    std::vector<uint8_t> canvas(size_t(m_w) * m_h * 4, 0);  // BGRA prémultiplié
    std::vector<uint8_t> px, mask;
    int drawn = 0, otherModes = 0, unreadable = badHeaders;
    for (size_t li = layers.size(); li-- > 0;) {
        const Layer& L = layers[li];
        if (L.group || !L.visible || L.type > 5 || !L.w || !L.h || L.w > 65535 || L.h > 65535) continue;
        float opacity = L.opacity;
        bool visible = true;
        for (size_t k = L.path.size(); k-- > 1 && visible;) {  // ancêtres
            const std::vector<uint32_t> parent(L.path.begin(), L.path.begin() + k);
            auto it = groups.find(parent);
            if (it != groups.end()) visible = it->second->visible, opacity *= it->second->opacity;
        }
        if (!visible || opacity <= 0) continue;
        static const uint32_t compsOf[] = {3, 4, 1, 2, 1, 2};
        const uint32_t comps = compsOf[L.type];
        bool hasMask = false, pixelsOk = false;
        try {
            if (!ReadHierarchy(L.hier, L.w, L.h, comps, px)) {
                ++unreadable;
                continue;
            }
            pixelsOk = true;
            if (L.mask && L.applyMask) {
                // Canal de masque : largeur, hauteur, nom, propriétés, hiérarchie
                const uint32_t nl = U32(L.mask + 8);
                if (nl <= m_n - L.mask - 12) {
                    const uint64_t mp = Props(L.mask + 12 + nl, [](uint32_t, uint64_t, uint32_t) {});
                    hasMask = ReadHierarchy(Ptr(mp), L.w, L.h, 1, mask);
                }
            }
        } catch (const Fail& e) {  // calque tronqué ou corrompu : ignoré (annulation : propagée)
            if (!e.msg) throw;
            if (!pixelsOk) {
                ++unreadable;
                continue;
            }
            hasMask = false;  // masque illisible : calque affiché sans masque
        }
        if (L.mode != 0 && L.mode != 28) ++otherModes;  // normal (ancien et nouveau)
        const bool indexed = L.type >= 4, gray = L.type == 2 || L.type == 3, alpha = (L.type & 1) != 0;
        const uint32_t bpp = comps * uint32_t(m_prec.bytes), cb = uint32_t(m_prec.bytes);
        const uint32_t op = uint32_t(opacity * 255 + 0.5f);
        ParallelFor(L.h, [&](uint32_t y0, uint32_t y1) {  // lignes indépendantes
            for (uint32_t y = y0; y < y1; ++y) {
                const int64_t cy = int64_t(L.oy) + y;
                if (cy < 0 || cy >= m_h) continue;
                for (uint32_t x = 0; x < L.w; ++x) {
                    const int64_t cx = int64_t(L.ox) + x;
                    if (cx < 0 || cx >= m_w) continue;
                    const size_t i = size_t(y) * L.w + x;
                    const uint8_t* s = &px[i * bpp];
                    uint8_t r, g, b, a;
                    if (indexed) {
                        const size_t k = s[0];
                        if (k * 3 + 3 <= m_cmap.size()) r = m_cmap[k * 3], g = m_cmap[k * 3 + 1], b = m_cmap[k * 3 + 2];
                        else r = g = b = 0;
                        a = alpha ? s[1] : 255;
                    } else if (gray) {
                        r = g = b = Comp(s, true);
                        a = alpha ? Comp(s + cb, false) : 255;
                    } else {
                        r = Comp(s, true), g = Comp(s + cb, true), b = Comp(s + 2 * cb, true);
                        a = alpha ? Comp(s + 3 * cb, false) : 255;
                    }
                    uint32_t sa = a * op;  // 0..65025
                    if (hasMask) sa = sa * Comp(&mask[i * cb], false) / 255;
                    if (!sa) continue;
                    uint8_t* d = &canvas[(size_t(cy) * m_w + size_t(cx)) * 4];
                    const uint32_t inv = 65025 - sa;
                    d[0] = uint8_t((b * sa + d[0] * inv + 32512) / 65025);
                    d[1] = uint8_t((g * sa + d[1] * inv + 32512) / 65025);
                    d[2] = uint8_t((r * sa + d[2] * inv + 32512) / 65025);
                    d[3] = uint8_t((255 * sa + d[3] * inv + 32512) / 65025);
                }
            }
        });
        ++drawn;
        if (m_job.Cancelled()) return CYIMG_E_CANCELLED;
    }
    if (unreadable) {
        if (!drawn) throw Fail{CYIMG_TR("plugin.docs.xcf.err.layers_data", L"GIMP : données des calques illisibles ou tronquées")};
        swprintf_s(buf, CYIMG_TR("plugin.docs.xcf.unreadable_value", L"%d (fichier tronqué ou corrompu)"), unreadable);
        m_job.Info(CYIMG_TR("plugin.docs.info.unreadable_layers", L"Calques illisibles"), buf);
    }
    if (otherModes) m_job.Info(CYIMG_TR("plugin.docs.info.blend_modes", L"Modes de fusion"), CYIMG_TR("plugin.docs.xcf.blend_approx", L"approximés (fusion normale)"));

    bool opaque = true;
    for (size_t i = 3; i < canvas.size() && opaque; i += 4) opaque = canvas[i] == 255;
    uint32_t stride = 0;
    uint8_t* out = m_job.host->alloc_bgra(m_job.ctx, m_w, m_h, opaque ? 0 : 1, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    for (uint32_t y = 0; y < m_h; ++y) memcpy(out + size_t(y) * stride, &canvas[size_t(y) * m_w * 4], size_t(m_w) * 4);
    return CYIMG_OK;
}

}  // namespace

int DecodeXcf(const Job& job, const uint8_t* f, size_t n) {
    try {
        Xcf x(job, f, n);
        return x.Run();
    } catch (const Fail& e) {
        return e.msg ? job.Fail(e.msg) : CYIMG_E_CANCELLED;
    }
}

}  // namespace docs
