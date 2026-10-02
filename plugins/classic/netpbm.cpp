// Netpbm : PBM/PGM/PPM texte et binaires (P1-P6, maxval jusqu'à 65535), PAM (P7) et PFM (PF/Pf).

#include "classic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace classic {
namespace {

bool Space(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

// Lecteur de jetons d'en-tête (les commentaires « # ... » vont jusqu'à la fin de ligne)
struct Reader {
    const uint8_t* p;
    size_t n, pos;
    void SkipSpace() {
        while (pos < n) {
            if (Space(p[pos])) ++pos;
            else if (p[pos] == '#') {
                while (pos < n && p[pos] != '\n' && p[pos] != '\r') ++pos;
            } else break;
        }
    }
    bool Uint(uint32_t& v) {
        SkipSpace();
        if (pos >= n || p[pos] < '0' || p[pos] > '9') return false;
        uint64_t r = 0;
        while (pos < n && p[pos] >= '0' && p[pos] <= '9') {
            r = r * 10 + (p[pos++] - '0');
            if (r > 0xFFFFFFFFull) return false;
        }
        v = uint32_t(r);
        return true;
    }
    bool Word(std::string& s) {
        SkipSpace();
        s.clear();
        while (pos < n && !Space(p[pos]) && s.size() < 64) s += char(p[pos++]);
        return !s.empty();
    }
};

inline uint8_t Scale(uint32_t v, uint32_t maxval) {
    if (v >= maxval) return 255;
    return uint8_t((uint64_t(v) * 255 + maxval / 2) / maxval);
}

int DecodePfm(Dec& d) {
    const bool color = d.d[1] == 'F';
    Reader r{d.d, d.n, 2};
    uint32_t w = 0, h = 0;
    std::string scaleStr;
    if (!r.Uint(w) || !r.Uint(h) || !r.Word(scaleStr)) return d.Fail(L"En-tête PFM invalide");
    if (!SizeOk(w, h)) return d.Fail(L"Dimensions PFM invalides");
    const double scale = atof(scaleStr.c_str());
    const bool le = scale < 0;
    size_t pos = r.pos + 1;  // un seul blanc après l'échelle
    const size_t ch = color ? 3 : 1;
    const size_t need = size_t(w) * h * ch * 4;
    if (pos > d.n || d.n - pos < need) return d.Fail(L"Fichier PFM tronqué");
    std::vector<float> rgb(size_t(w) * h * 3);
    float maxv = 0.f;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = d.d + pos + size_t(h - 1 - y) * w * ch * 4;  // lignes de bas en haut
        float* o = rgb.data() + size_t(y) * w * 3;
        for (size_t i = 0; i < size_t(w) * ch; ++i, s += 4) {
            uint32_t u = le ? LE32(s) : BE32(s);
            float f;
            memcpy(&f, &u, 4);
            if (!std::isfinite(f)) f = 0.f;
            if (f > maxv) maxv = f;
            if (color) o[i] = f;
            else o[i * 3] = o[i * 3 + 1] = o[i * 3 + 2] = f;
        }
    }
    Rgba img;
    const bool hdr = maxv > 1.f;  // valeurs > 1 : vraie image HDR, sinon simple écrêtage
    const double ev = ToneMap(rgb.data(), w, h, img, hdr);
    d.Format(color ? L"PFM (RVB flottant)" : L"PFM (gris flottant)");
    if (hdr) {
        wchar_t buf[64];
        swprintf_s(buf, L"%+.2f IL (automatique)", ev);
        d.Info(L"Exposition", buf);
    }
    return Emit(d, img, false);
}

int DecodePam(Dec& d) {
    size_t pos = 3;
    uint32_t w = 0, h = 0, depth = 0, maxval = 0;
    std::string tupl;
    bool end = false;
    while (pos < d.n && !end) {
        size_t e = pos;
        while (e < d.n && d.d[e] != '\n') ++e;
        std::string line(reinterpret_cast<const char*>(d.d + pos), e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        size_t b = line.find_first_of(" \t", a);
        const std::string key = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
        std::string val = b == std::string::npos ? "" : line.substr(b);
        val.erase(0, std::min(val.size(), val.find_first_not_of(" \t")));
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        const unsigned long v = strtoul(val.c_str(), nullptr, 10);
        if (key == "ENDHDR") end = true;
        else if (key == "WIDTH") w = uint32_t(v);
        else if (key == "HEIGHT") h = uint32_t(v);
        else if (key == "DEPTH") depth = uint32_t(v);
        else if (key == "MAXVAL") maxval = uint32_t(v);
        else if (key == "TUPLTYPE") tupl += (tupl.empty() ? "" : " ") + val;
    }
    if (!end) return d.Fail(L"En-tête PAM invalide (ENDHDR absent)");
    if (!SizeOk(w, h)) return d.Fail(L"Dimensions PAM invalides");
    if (depth < 1 || depth > 64 || maxval < 1 || maxval > 65535) return d.Fail(L"En-tête PAM invalide");
    const bool bw = tupl.rfind("BLACKANDWHITE", 0) == 0;
    const bool alpha = depth == 2 || depth >= 4 || tupl.find("_ALPHA") != std::string::npos;
    const bool gray = depth <= 2;
    const size_t bps = maxval > 255 ? 2 : 1;
    const size_t need = size_t(w) * h * depth * bps;
    if (pos > d.n || d.n - pos < need) return d.Fail(L"Fichier PAM tronqué");
    Rgba img;
    img.Init(w, h, 255);
    const uint8_t* s = d.d + pos;
    auto rd = [&](size_t i) -> uint32_t { return bps == 2 ? BE16(s + i * 2) : s[i]; };
    for (size_t i = 0, px = 0; px < size_t(w) * h; ++px, i += depth) {
        uint8_t* o = img.px.data() + px * 4;
        if (gray) {
            const uint8_t g = bw ? (rd(i) ? 255 : 0) : Scale(rd(i), maxval);
            o[0] = o[1] = o[2] = g;
            if (alpha && depth == 2) o[3] = Scale(rd(i + 1), maxval);
        } else {
            o[0] = Scale(rd(i), maxval), o[1] = Scale(rd(i + 1), maxval), o[2] = Scale(rd(i + 2), maxval);
            if (alpha && depth >= 4) o[3] = Scale(rd(i + 3), maxval);
        }
    }
    std::wstring fmt = L"PAM";
    if (!tupl.empty()) fmt += L" " + Widen(tupl);
    if (bps == 2) fmt += L", 16 bits";
    d.Format(fmt.c_str());
    return Emit(d, img, alpha);
}

}  // namespace

int DecodeNetpbm(Dec& d) {
    if (d.n < 3 || d.d[0] != 'P') return d.Fail(L"Fichier Netpbm non reconnu");
    const char kind = char(d.d[1]);
    if (kind == 'F' || kind == 'f') return DecodePfm(d);
    if (kind == '7') return DecodePam(d);
    if (kind < '1' || kind > '6') return d.Fail(L"Fichier Netpbm non reconnu");
    const int k = kind - '0';
    const bool bit = k == 1 || k == 4, color = k == 3 || k == 6, ascii = k <= 3;
    Reader r{d.d, d.n, 2};
    uint32_t w = 0, h = 0, maxval = 1;
    if (!r.Uint(w) || !r.Uint(h) || (!bit && !r.Uint(maxval))) return d.Fail(L"En-tête Netpbm invalide");
    if (!SizeOk(w, h)) return d.Fail(L"Dimensions Netpbm invalides");
    if (maxval < 1 || maxval > 65535) return d.Fail(L"Valeur maximale Netpbm invalide");
    Rgba img;
    img.Init(w, h, 255);
    const size_t count = size_t(w) * h;
    const size_t ch = color ? 3 : 1;
    if (ascii) {
        for (size_t px = 0; px < count; ++px) {
            uint8_t* o = img.px.data() + px * 4;
            uint32_t v[3] = {};
            for (size_t c = 0; c < ch; ++c) {
                if (bit) {  // P1 : chiffres pas forcément séparés
                    r.SkipSpace();
                    if (r.pos >= r.n || (r.p[r.pos] != '0' && r.p[r.pos] != '1')) return d.Fail(L"Fichier PBM tronqué ou invalide");
                    v[c] = r.p[r.pos++] - '0';
                } else if (!r.Uint(v[c])) {
                    return d.Fail(L"Fichier Netpbm tronqué ou invalide");
                }
            }
            if (bit) o[0] = o[1] = o[2] = v[0] ? 0 : 255;
            else if (color) o[0] = Scale(v[0], maxval), o[1] = Scale(v[1], maxval), o[2] = Scale(v[2], maxval);
            else o[0] = o[1] = o[2] = Scale(v[0], maxval);
            if ((px & 0xFFFF) == 0 && d.Cancelled()) return CYIMG_E_CANCELLED;
        }
    } else {
        const size_t pos = r.pos + 1;  // un seul blanc avant les données binaires
        if (pos > d.n) return d.Fail(L"Fichier Netpbm tronqué");
        const uint8_t* s = d.d + pos;
        const size_t avail = d.n - pos;
        if (bit) {
            const size_t rowBytes = (size_t(w) + 7) / 8;
            if (avail < rowBytes * h) return d.Fail(L"Fichier PBM tronqué");
            for (uint32_t y = 0; y < h; ++y) {
                uint8_t* o = img.Row(y);
                const uint8_t* row = s + y * rowBytes;
                for (uint32_t x = 0; x < w; ++x, o += 4) o[0] = o[1] = o[2] = (row[x >> 3] & (0x80 >> (x & 7))) ? 0 : 255;
            }
        } else {
            const size_t bps = maxval > 255 ? 2 : 1;
            if (avail / bps / ch < count) return d.Fail(L"Fichier Netpbm tronqué");
            for (size_t px = 0; px < count; ++px) {
                uint8_t* o = img.px.data() + px * 4;
                const uint8_t* p = s + px * ch * bps;
                if (bps == 1 && maxval == 255) {
                    if (color) o[0] = p[0], o[1] = p[1], o[2] = p[2];
                    else o[0] = o[1] = o[2] = p[0];
                } else {
                    for (size_t c = 0; c < ch; ++c) o[c] = Scale(bps == 2 ? BE16(p + c * 2) : p[c], maxval);
                    if (!color) o[1] = o[2] = o[0];
                }
            }
        }
    }
    static const wchar_t* names[] = {L"", L"PBM (texte)", L"PGM (texte)", L"PPM (texte)", L"PBM", L"PGM", L"PPM"};
    std::wstring fmt = names[k];
    if (maxval > 255) fmt += L", 16 bits";
    d.Format(fmt.c_str());
    return Emit(d, img, false);
}

}  // namespace classic
