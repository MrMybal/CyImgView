// Radiance HDR (RGBE / XYZE) : RLE nouveau (par composante) et ancien, orientations -Y +X et autres,
// tone mapping Reinhard avec exposition automatique puis sRGB. Vignette : sous-échantillonnage.

#include "classic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace classic {
namespace {

// Lit une ligne de balayage de w pixels RGBE. Retourne false si les données sont invalides/tronquées.
bool ReadScanline(const uint8_t* s, size_t n, size_t& pos, uint32_t w, uint8_t* out) {
    if (pos + 4 > n) return false;
    // Nouveau RLE : 2 2 hi lo, une composante après l'autre
    if (w >= 8 && w < 32768 && s[pos] == 2 && s[pos + 1] == 2 && !(s[pos + 2] & 0x80)) {
        if ((uint32_t(s[pos + 2]) << 8 | s[pos + 3]) != w) return false;
        pos += 4;
        for (int c = 0; c < 4; ++c) {
            uint32_t x = 0;
            while (x < w) {
                if (pos >= n) return false;
                uint32_t cnt = s[pos++];
                if (cnt > 128) {
                    cnt -= 128;
                    if (pos >= n || x + cnt > w) return false;
                    const uint8_t v = s[pos++];
                    for (uint32_t k = 0; k < cnt; ++k) out[(x + k) * 4 + c] = v;
                } else {
                    if (cnt == 0 || x + cnt > w || pos + cnt > n) return false;
                    for (uint32_t k = 0; k < cnt; ++k) out[(x + k) * 4 + c] = s[pos++];
                }
                x += cnt;
            }
        }
        return true;
    }
    // Ancien RLE (1 1 1 n = répéter le pixel précédent) ou pixels bruts
    uint32_t x = 0;
    int shift = 0;
    while (x < w) {
        if (pos + 4 > n) return false;
        const uint8_t* p = s + pos;
        pos += 4;
        if (p[0] == 1 && p[1] == 1 && p[2] == 1) {
            if (x == 0) return false;
            const uint64_t cnt = uint64_t(p[3]) << shift;
            if (x + cnt > w) return false;
            for (uint64_t k = 0; k < cnt; ++k, ++x) memcpy(out + x * 4, out + (x - 1) * 4, 4);
            shift += 8;
            if (shift > 24) return false;
        } else {
            memcpy(out + x * 4, p, 4);
            ++x;
            shift = 0;
        }
    }
    return true;
}

}  // namespace

int DecodeHdr(Dec& d) {
    // En-tête : lignes jusqu'à une ligne vide, puis la ligne de résolution
    size_t pos = 0;
    bool xyz = false, formatSeen = false;
    double exposure = 1.0;
    bool hasExposure = false;
    std::string software;
    auto readLine = [&](std::string& l) {
        if (pos >= d.n) return false;
        size_t e = pos;
        while (e < d.n && d.d[e] != '\n' && e - pos < 4096) ++e;
        l.assign(reinterpret_cast<const char*>(d.d + pos), e - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        pos = e + 1;
        return true;
    };
    std::string l;
    if (!readLine(l) || l.rfind("#?", 0) != 0) return d.Fail(L"Fichier Radiance HDR non reconnu");
    for (;;) {
        if (!readLine(l)) return d.Fail(L"En-tête HDR tronqué");
        if (l.empty()) break;
        if (l.rfind("FORMAT=", 0) == 0) {
            formatSeen = true;
            if (l.find("32-bit_rle_xyze") != std::string::npos) xyz = true;
            else if (l.find("32-bit_rle_rgbe") == std::string::npos) return d.Fail(L"Format de pixels HDR non géré");
        } else if (l.rfind("EXPOSURE=", 0) == 0) {
            const double e = atof(l.c_str() + 9);
            if (e > 0 && std::isfinite(e)) exposure *= e, hasExposure = true;
        } else if (l.rfind("SOFTWARE=", 0) == 0) {
            software = l.substr(9);
        }
    }
    (void)formatSeen;
    if (!readLine(l)) return d.Fail(L"Fichier HDR tronqué (résolution absente)");
    char s1 = 0, a1 = 0, s2 = 0, a2 = 0;
    unsigned n1 = 0, n2 = 0;
    if (sscanf_s(l.c_str(), "%c%c %u %c%c %u", &s1, 1, &a1, 1, &n1, &s2, 1, &a2, 1, &n2) != 6 ||
        (s1 != '+' && s1 != '-') || (s2 != '+' && s2 != '-') || !((a1 == 'Y' && a2 == 'X') || (a1 == 'X' && a2 == 'Y')))
        return d.Fail(L"Ligne de résolution HDR invalide");
    const bool yFirst = a1 == 'Y';
    const uint32_t W = yFirst ? n2 : n1, H = yFirst ? n1 : n2;
    if (!SizeOk(W, H)) return d.Fail(L"Dimensions HDR invalides");
    const uint32_t lines = n1, lineLen = n2;

    // Vignette : un pixel sur k dans chaque direction
    const uint32_t k = d.ThumbStep(W, H);
    const uint32_t ow = (W + k - 1) / k, oh = (H + k - 1) / k;
    std::vector<float> rgb(size_t(ow) * oh * 3, 0.f);
    std::vector<uint8_t> scan(size_t(lineLen) * 4);
    const float invExp = float(1.0 / exposure);
    for (uint32_t i = 0; i < lines; ++i) {
        if (!ReadScanline(d.d, d.n, pos, lineLen, scan.data())) {
            if (i == 0) return d.Fail(L"Données HDR invalides ou tronquées");
            d.Info(L"Avertissement", L"fichier tronqué");
            break;
        }
        if ((i & 63) == 0 && d.Cancelled()) return CYIMG_E_CANCELLED;
        for (uint32_t j = 0; j < lineLen; ++j) {
            uint32_t x, y;
            if (yFirst) {
                y = s1 == '-' ? i : H - 1 - i;
                x = s2 == '+' ? j : W - 1 - j;
            } else {
                x = s1 == '+' ? i : W - 1 - i;
                y = s2 == '-' ? j : H - 1 - j;
            }
            if (x % k || y % k) continue;
            const uint8_t* p = &scan[size_t(j) * 4];
            float* o = &rgb[(size_t(y / k) * ow + x / k) * 3];
            if (!p[3]) continue;
            const float f = std::ldexp(1.f, int(p[3]) - 136) * invExp;
            float r = (p[0] + 0.5f) * f, g = (p[1] + 0.5f) * f, b = (p[2] + 0.5f) * f;
            if (xyz) {  // XYZ -> sRGB linéaire
                const float X = r, Y = g, Z = b;
                r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
                g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
                b = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
            }
            o[0] = r, o[1] = g, o[2] = b;
        }
    }
    Rgba img;
    const double ev = ToneMap(rgb.data(), ow, oh, img, true);
    d.Format(xyz ? L"Radiance HDR (XYZE)" : L"Radiance HDR (RGBE)");
    wchar_t buf[64];
    swprintf_s(buf, L"%+.2f IL (automatique, Reinhard)", ev);
    d.Info(L"Exposition", buf);
    if (hasExposure) {
        swprintf_s(buf, L"%g", exposure);
        d.Info(L"EXPOSURE (fichier)", buf);
    }
    d.InfoA(L"Logiciel", software);
    if (k > 1) {
        swprintf_s(buf, L"%u × %u", W, H);
        d.Info(L"Taille réelle", buf);
    }
    return Emit(d, img, false);
}

}  // namespace classic
