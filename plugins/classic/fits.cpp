// FITS (Flexible Image Transport System, astronomie) : HDU primaire ou première extension IMAGE,
// BITPIX 8/16/32/64/-32/-64, BZERO/BSCALE/BLANK, NAXIS 2 ou 3 (3 plans = RVB, sinon premier plan).
// Étirement automatique linéaire entre les centiles 0,5 % et 99,5 %. La ligne 1 est en bas.

#include "classic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace classic {
namespace {

constexpr size_t kBlock = 2880;

struct Hdu {
    std::map<std::string, std::string> kv;  // valeurs brutes (chaînes sans apostrophes)
    int bitpix = 0, naxis = 0;
    uint64_t axes[9] = {};
    size_t dataPos = 0;
    uint64_t dataSize = 0;  // octets, sans bourrage
    std::string Str(const char* k) const {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    }
    double Num(const char* k, double def) const {
        auto it = kv.find(k);
        if (it == kv.end() || it->second.empty()) return def;
        std::string v = it->second;
        std::replace(v.begin(), v.end(), 'D', 'E');  // exposant Fortran
        char* e = nullptr;
        const double r = strtod(v.c_str(), &e);
        return e == v.c_str() ? def : r;
    }
    bool Has(const char* k) const { return kv.count(k) != 0; }
};

// Lit un en-tête à partir de pos. Retourne false s'il est tronqué ou invalide.
bool ParseHdu(const uint8_t* f, size_t n, size_t pos, Hdu& h) {
    for (size_t p = pos;; p += 80) {
        if (p + 80 > n) return false;
        const char* c = reinterpret_cast<const char*>(f + p);
        std::string key(c, 8);
        key.erase(key.find_last_not_of(' ') + 1);
        if (key == "END") {
            h.dataPos = (p + 80 + kBlock - 1) / kBlock * kBlock;
            break;
        }
        if (c[8] != '=' || key.empty()) continue;
        std::string v;
        size_t i = 10;
        while (i < 80 && c[i] == ' ') ++i;
        if (i < 80 && c[i] == '\'') {
            for (++i; i < 80; ++i) {
                if (c[i] == '\'') {
                    if (i + 1 < 80 && c[i + 1] == '\'') v += '\'', ++i;
                    else break;
                } else v += c[i];
            }
            v.erase(v.find_last_not_of(' ') + 1);
        } else {
            while (i < 80 && c[i] != '/') v += c[i++];
            v.erase(v.find_last_not_of(' ') + 1);
        }
        if (!h.kv.count(key)) h.kv[key] = v;
        if (p - pos > (1u << 24)) return false;  // en-tête démesuré
    }
    h.bitpix = int(h.Num("BITPIX", 0));
    h.naxis = int(h.Num("NAXIS", 0));
    if (h.naxis < 0 || h.naxis > 999) return false;
    uint64_t prod = h.naxis ? 1 : 0;
    for (int i = 1; i <= h.naxis; ++i) {
        char k[16];
        sprintf_s(k, "NAXIS%d", i);
        const double a = h.Num(k, 0);
        if (a < 0 || a > 1e12) return false;
        if (i <= 9) h.axes[i - 1] = uint64_t(a);
        prod *= uint64_t(a);
        if (prod > (1ull << 50)) return false;
    }
    const int bp = std::abs(h.bitpix);
    if (bp != 8 && bp != 16 && bp != 32 && bp != 64) return false;
    const uint64_t pcount = uint64_t(std::max(0.0, h.Num("PCOUNT", 0))), gcount = uint64_t(std::max(1.0, h.Num("GCOUNT", 1)));
    h.dataSize = uint64_t(bp / 8) * gcount * (pcount + prod);
    return true;
}

}  // namespace

int DecodeFits(Dec& d) {
    // Recherche de l'image : HDU primaire, sinon première extension IMAGE
    Hdu h;
    if (!ParseHdu(d.d, d.n, 0, h)) return d.Fail(L"En-tête FITS invalide ou tronqué");
    const Hdu primary = h;
    bool found = h.naxis >= 2 && h.axes[0] && h.axes[1];
    std::string extName;
    for (int guard = 0; !found && guard < 1000; ++guard) {
        const uint64_t next = h.dataPos + (h.dataSize + kBlock - 1) / kBlock * kBlock;
        if (next + 80 > d.n) break;
        Hdu e;
        if (!ParseHdu(d.d, d.n, size_t(next), e)) break;
        h = std::move(e);
        const std::string x = h.Str("XTENSION");
        if ((x == "IMAGE" || x == "IUEIMAGE") && h.naxis >= 2 && h.axes[0] && h.axes[1]) {
            found = true;
            extName = h.Str("EXTNAME");
        }
    }
    if (!found) return d.Fail(L"Ce fichier FITS ne contient pas d'image (tableaux de données uniquement)");

    const uint64_t W64 = h.axes[0], H64 = h.axes[1];
    if (!SizeOk(W64, H64)) return d.Fail(L"Dimensions FITS non gérées");
    const uint32_t W = uint32_t(W64), H = uint32_t(H64);
    const uint64_t depth = h.naxis >= 3 ? std::max<uint64_t>(1, h.axes[2]) : 1;
    const bool rgb = depth == 3;
    const int planes = rgb ? 3 : 1;
    const int bpv = std::abs(h.bitpix) / 8;
    const uint64_t planeBytes = uint64_t(W) * H * bpv;
    if (h.dataPos > d.n || d.n - h.dataPos < planeBytes * planes) return d.Fail(L"Fichier FITS tronqué");
    const double bzero = h.Num("BZERO", 0), bscale = h.Num("BSCALE", 1);
    const bool hasBlank = h.Has("BLANK") && h.bitpix > 0;
    const double blank = h.Num("BLANK", 0);

    const uint32_t k = d.ThumbStep(W, H);
    const uint32_t ow = (W + k - 1) / k, oh = (H + k - 1) / k;
    std::vector<float> v(size_t(ow) * oh * planes);
    const uint8_t* base = d.d + h.dataPos;
    for (int p = 0; p < planes; ++p) {
        for (uint32_t yy = 0; yy < oh; ++yy) {
            const uint8_t* row = base + p * planeBytes + uint64_t(yy) * k * W * bpv;
            float* o = &v[(size_t(p) * oh + (oh - 1 - yy)) * ow];  // ligne 1 en bas
            for (uint32_t xx = 0; xx < ow; ++xx) {
                const uint8_t* s = row + size_t(xx) * k * bpv;
                double raw;
                switch (h.bitpix) {
                    case 8: raw = s[0]; break;
                    case 16: raw = int16_t(BE16(s)); break;
                    case 32: raw = int32_t(BE32(s)); break;
                    case 64: raw = double(int64_t(uint64_t(BE32(s)) << 32 | BE32(s + 4))); break;
                    case -32: {
                        const uint32_t u = BE32(s);
                        float f;
                        memcpy(&f, &u, 4);
                        raw = f;
                        break;
                    }
                    default: {
                        const uint64_t u = uint64_t(BE32(s)) << 32 | BE32(s + 4);
                        double f;
                        memcpy(&f, &u, 8);
                        raw = f;
                        break;
                    }
                }
                o[xx] = (hasBlank && raw == blank) ? NAN : float(bzero + bscale * raw);
            }
            if ((yy & 63) == 0 && d.Cancelled()) return CYIMG_E_CANCELLED;
        }
    }

    // Centiles 0,5 % / 99,5 % (sur un échantillon d'au plus ~2 M valeurs, toutes composantes)
    std::vector<float> smp;
    const size_t step = std::max<size_t>(1, v.size() / 2000000);
    smp.reserve(v.size() / step + 1);
    for (size_t i = 0; i < v.size(); i += step)
        if (std::isfinite(v[i])) smp.push_back(v[i]);
    if (smp.empty()) return d.Fail(L"Image FITS vide (aucune valeur valide)");
    auto pct = [&](double q) {
        const size_t i = std::min(smp.size() - 1, size_t(q * double(smp.size() - 1)));
        std::nth_element(smp.begin(), smp.begin() + ptrdiff_t(i), smp.end());
        return smp[i];
    };
    float lo = pct(0.005), hi = pct(0.995);
    if (!(hi > lo)) {
        lo = *std::min_element(smp.begin(), smp.end());
        hi = *std::max_element(smp.begin(), smp.end());
    }
    const float scale = hi > lo ? 255.f / (hi - lo) : 0.f;
    auto map = [&](float x) -> uint8_t {
        if (!std::isfinite(x)) return 0;
        if (scale == 0.f) return 128;
        const float r = (x - lo) * scale;
        return r <= 0.f ? 0 : r >= 255.f ? 255 : uint8_t(r + 0.5f);
    };
    Rgba img;
    img.Init(ow, oh, 255);
    const size_t plane = size_t(ow) * oh;
    for (size_t i = 0; i < plane; ++i) {
        uint8_t* o = img.px.data() + i * 4;
        if (rgb) o[0] = map(v[i]), o[1] = map(v[plane + i]), o[2] = map(v[2 * plane + i]);
        else o[0] = o[1] = o[2] = map(v[i]);
    }

    wchar_t buf[96];
    swprintf_s(buf, L"FITS %d bits%ls%ls", std::abs(h.bitpix), h.bitpix < 0 ? L" flottant" : L"", rgb ? L", RVB" : L"");
    d.Format(buf);
    auto info = [&](const wchar_t* label, const char* key) {
        std::string s = h.Str(key);
        if (s.empty()) s = primary.Str(key);
        d.InfoA(label, s);
    };
    info(L"Objet", "OBJECT");
    info(L"Télescope", "TELESCOP");
    info(L"Instrument", "INSTRUME");
    info(L"Date d'observation", "DATE-OBS");
    info(L"Temps de pose (s)", "EXPTIME");
    info(L"Filtre", "FILTER");
    info(L"Observateur", "OBSERVER");
    d.InfoA(L"Extension", extName);
    if (depth > 1 && !rgb) {
        swprintf_s(buf, L"1 / %llu", static_cast<unsigned long long>(depth));
        d.Info(L"Plan affiché", buf);
    }
    swprintf_s(buf, L"%g … %g (centiles 0,5 %% – 99,5 %%)", double(lo), double(hi));
    d.Info(L"Étirement", buf);
    if (k > 1) {
        swprintf_s(buf, L"%u × %u", W, H);
        d.Info(L"Taille réelle", buf);
    }
    return Emit(d, img, false);
}

}  // namespace classic
