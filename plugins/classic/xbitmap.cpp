// XBM (bitmap X11/X10 en source C) et XPM (XPM2 / XPM3, nombre quelconque de caractères par pixel).

#include "classic.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace classic {
namespace {

bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

// Saute blancs et commentaires C au début du fichier
size_t SkipLead(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (Space(char(p[i]))) ++i;
        else if (i + 1 < n && p[i] == '/' && p[i + 1] == '*' && !(i + 8 < n && memcmp(p + i, "/* XPM */", 9) == 0)) {
            i += 2;
            while (i + 1 < n && !(p[i] == '*' && p[i + 1] == '/')) ++i;
            i += 2;
        } else break;
    }
    return i;
}

// --- Couleurs X11 courantes (valeurs X11, qui diffèrent parfois de CSS : gray, green, maroon, purple)
struct Named {
    const char* name;
    uint8_t r, g, b;
};
const Named kColors[] = {
    {"aliceblue", 240, 248, 255}, {"antiquewhite", 250, 235, 215}, {"aqua", 0, 255, 255},
    {"aquamarine", 127, 255, 212}, {"azure", 240, 255, 255}, {"beige", 245, 245, 220},
    {"bisque", 255, 228, 196}, {"black", 0, 0, 0}, {"blanchedalmond", 255, 235, 205},
    {"blue", 0, 0, 255}, {"blueviolet", 138, 43, 226}, {"brown", 165, 42, 42},
    {"burlywood", 222, 184, 135}, {"cadetblue", 95, 158, 160}, {"chartreuse", 127, 255, 0},
    {"chocolate", 210, 105, 30}, {"coral", 255, 127, 80}, {"cornflowerblue", 100, 149, 237},
    {"cornsilk", 255, 248, 220}, {"crimson", 220, 20, 60}, {"cyan", 0, 255, 255},
    {"darkblue", 0, 0, 139}, {"darkcyan", 0, 139, 139}, {"darkgoldenrod", 184, 134, 11},
    {"darkgray", 169, 169, 169}, {"darkgreen", 0, 100, 0}, {"darkkhaki", 189, 183, 107},
    {"darkmagenta", 139, 0, 139}, {"darkolivegreen", 85, 107, 47}, {"darkorange", 255, 140, 0},
    {"darkorchid", 153, 50, 204}, {"darkred", 139, 0, 0}, {"darksalmon", 233, 150, 122},
    {"darkseagreen", 143, 188, 143}, {"darkslateblue", 72, 61, 139}, {"darkslategray", 47, 79, 79},
    {"darkturquoise", 0, 206, 209}, {"darkviolet", 148, 0, 211}, {"deeppink", 255, 20, 147},
    {"deepskyblue", 0, 191, 255}, {"dimgray", 105, 105, 105}, {"dodgerblue", 30, 144, 255},
    {"firebrick", 178, 34, 34}, {"floralwhite", 255, 250, 240}, {"forestgreen", 34, 139, 34},
    {"fuchsia", 255, 0, 255}, {"gainsboro", 220, 220, 220}, {"ghostwhite", 248, 248, 255},
    {"gold", 255, 215, 0}, {"goldenrod", 218, 165, 32}, {"gray", 190, 190, 190},
    {"green", 0, 255, 0}, {"greenyellow", 173, 255, 47}, {"honeydew", 240, 255, 240},
    {"hotpink", 255, 105, 180}, {"indianred", 205, 92, 92}, {"indigo", 75, 0, 130},
    {"ivory", 255, 255, 240}, {"khaki", 240, 230, 140}, {"lavender", 230, 230, 250},
    {"lavenderblush", 255, 240, 245}, {"lawngreen", 124, 252, 0}, {"lemonchiffon", 255, 250, 205},
    {"lightblue", 173, 216, 230}, {"lightcoral", 240, 128, 128}, {"lightcyan", 224, 255, 255},
    {"lightgoldenrod", 238, 221, 130}, {"lightgoldenrodyellow", 250, 250, 210}, {"lightgray", 211, 211, 211},
    {"lightgreen", 144, 238, 144}, {"lightpink", 255, 182, 193}, {"lightsalmon", 255, 160, 122},
    {"lightseagreen", 32, 178, 170}, {"lightskyblue", 135, 206, 250}, {"lightslateblue", 132, 112, 255},
    {"lightslategray", 119, 136, 153}, {"lightsteelblue", 176, 196, 222}, {"lightyellow", 255, 255, 224},
    {"lime", 0, 255, 0}, {"limegreen", 50, 205, 50}, {"linen", 250, 240, 230},
    {"magenta", 255, 0, 255}, {"maroon", 176, 48, 96}, {"mediumaquamarine", 102, 205, 170},
    {"mediumblue", 0, 0, 205}, {"mediumorchid", 186, 85, 211}, {"mediumpurple", 147, 112, 219},
    {"mediumseagreen", 60, 179, 113}, {"mediumslateblue", 123, 104, 238}, {"mediumspringgreen", 0, 250, 154},
    {"mediumturquoise", 72, 209, 204}, {"mediumvioletred", 199, 21, 133}, {"midnightblue", 25, 25, 112},
    {"mintcream", 245, 255, 250}, {"mistyrose", 255, 228, 225}, {"moccasin", 255, 228, 181},
    {"navajowhite", 255, 222, 173}, {"navy", 0, 0, 128}, {"navyblue", 0, 0, 128},
    {"oldlace", 253, 245, 230}, {"olive", 128, 128, 0}, {"olivedrab", 107, 142, 35},
    {"orange", 255, 165, 0}, {"orangered", 255, 69, 0}, {"orchid", 218, 112, 214},
    {"palegoldenrod", 238, 232, 170}, {"palegreen", 152, 251, 152}, {"paleturquoise", 175, 238, 238},
    {"palevioletred", 219, 112, 147}, {"papayawhip", 255, 239, 213}, {"peachpuff", 255, 218, 185},
    {"peru", 205, 133, 63}, {"pink", 255, 192, 203}, {"plum", 221, 160, 221},
    {"powderblue", 176, 224, 230}, {"purple", 160, 32, 240}, {"rebeccapurple", 102, 51, 153},
    {"red", 255, 0, 0}, {"rosybrown", 188, 143, 143}, {"royalblue", 65, 105, 225},
    {"saddlebrown", 139, 69, 19}, {"salmon", 250, 128, 114}, {"sandybrown", 244, 164, 96},
    {"seagreen", 46, 139, 87}, {"seashell", 255, 245, 238}, {"sienna", 160, 82, 45},
    {"silver", 192, 192, 192}, {"skyblue", 135, 206, 235}, {"slateblue", 106, 90, 205},
    {"slategray", 112, 128, 144}, {"snow", 255, 250, 250}, {"springgreen", 0, 255, 127},
    {"steelblue", 70, 130, 180}, {"tan", 210, 180, 140}, {"teal", 0, 128, 128},
    {"thistle", 216, 191, 216}, {"tomato", 255, 99, 71}, {"turquoise", 64, 224, 208},
    {"violet", 238, 130, 238}, {"violetred", 208, 32, 144}, {"wheat", 245, 222, 179},
    {"white", 255, 255, 255}, {"whitesmoke", 245, 245, 245}, {"yellow", 255, 255, 0},
    {"yellowgreen", 154, 205, 50}, {"webgray", 128, 128, 128}, {"webgreen", 0, 128, 0},
    {"webmaroon", 128, 0, 0}, {"webpurple", 128, 0, 128},
};

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Couleur XPM -> RGBA. Retourne false si inconnue.
bool ParseColor(std::string s, uint8_t o[4]) {
    o[3] = 255;
    if (s.empty()) return false;
    if (s[0] == '#') {
        const size_t len = s.size() - 1;
        if (len == 0 || len % 3 || len > 12) return false;
        const size_t k = len / 3;
        for (size_t c = 0; c < 3; ++c) {
            uint32_t v = 0;
            for (size_t i = 0; i < k; ++i) {
                const int h = Hex(s[1 + c * k + i]);
                if (h < 0) return false;
                v = v << 4 | uint32_t(h);
            }
            // Ramène à 8 bits
            if (k == 1) v *= 17;
            else if (k > 2) v >>= (k - 2) * 4;
            o[c] = uint8_t(v);
        }
        return true;
    }
    std::string l;
    for (char c : s)
        if (!Space(c)) l += char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (l == "none" || l == "transparent") {
        o[0] = o[1] = o[2] = o[3] = 0;
        return true;
    }
    for (size_t p; (p = l.find("grey")) != std::string::npos;) l.replace(p, 4, "gray");
    // grayNN : pourcentage
    if (l.size() > 4 && l.compare(0, 4, "gray") == 0 && l[4] >= '0' && l[4] <= '9') {
        const int pct = std::min(100, atoi(l.c_str() + 4));
        o[0] = o[1] = o[2] = uint8_t((pct * 255 + 50) / 100);
        return true;
    }
    // Variantes numérotées X11 (red1..red4) : approximation par assombrissement
    int shade = 0;
    if (!l.empty() && l.back() >= '1' && l.back() <= '4') {
        shade = l.back() - '0';
        l.pop_back();
    }
    for (const Named& n : kColors) {
        if (l != n.name) continue;
        static const int f[5] = {1000, 1000, 932, 804, 545};
        o[0] = uint8_t(n.r * f[shade] / 1000), o[1] = uint8_t(n.g * f[shade] / 1000), o[2] = uint8_t(n.b * f[shade] / 1000);
        return true;
    }
    return false;
}

}  // namespace

bool IsXbm(const uint8_t* p, size_t n) {
    const size_t i = SkipLead(p, n);
    if (n - i < 8 || memcmp(p + i, "#define", 7) != 0) return false;
    const std::string s(reinterpret_cast<const char*>(p + i), std::min<size_t>(n - i, 200));
    const size_t eol = s.find('\n');
    return s.substr(0, eol).find("width") != std::string::npos;
}

bool IsXpm(const uint8_t* p, size_t n) {
    const size_t i = SkipLead(p, n);
    return (n - i >= 9 && memcmp(p + i, "/* XPM */", 9) == 0) || (n - i >= 6 && memcmp(p + i, "! XPM2", 6) == 0);
}

int DecodeXbm(Dec& d) {
    const char* s = reinterpret_cast<const char*>(d.d);
    const size_t n = d.n;
    long w = -1, h = -1;
    size_t i = 0;
    // #define xxx_width N / xxx_height N
    while (i < n) {
        if (s[i] == '{') break;
        if (s[i] == '#' && n - i > 8 && memcmp(s + i, "#define", 7) == 0) {
            size_t a = i + 7;
            while (a < n && Space(s[a])) ++a;
            size_t b = a;
            while (b < n && !Space(s[b])) ++b;
            const std::string name(s + a, b - a);
            char buf[24] = {};
            size_t c = b;
            while (c < n && (s[c] == ' ' || s[c] == '\t')) ++c;
            for (size_t k = 0; k < 20 && c < n && s[c] >= '0' && s[c] <= '9'; ++k) buf[k] = s[c++];
            const long v = atol(buf);
            const auto ends = [&](const char* suf) {
                const size_t l = strlen(suf);
                return name.size() >= l && name.compare(name.size() - l, l, suf) == 0;
            };
            if (ends("width") && w < 0) w = v;
            else if (ends("height") && h < 0) h = v;
            i = c;
            continue;
        }
        ++i;
    }
    if (w <= 0 || h <= 0 || !SizeOk(uint64_t(w), uint64_t(h))) return d.Fail(L"En-tête XBM invalide (largeur/hauteur)");
    if (i >= n) return d.Fail(L"Données XBM absentes");
    // X10 : valeurs 16 bits (« short »)
    const std::string decl(s + (i > 200 ? i - 200 : 0), s + i);
    const bool x10 = decl.find("short") != std::string::npos;
    const size_t unit = x10 ? 16 : 8;
    const size_t rowUnits = (size_t(w) + unit - 1) / unit;
    std::vector<uint16_t> vals;
    vals.reserve(rowUnits * size_t(h));
    ++i;
    while (i < n && vals.size() < rowUnits * size_t(h)) {
        if (s[i] == '}') break;
        if (s[i] >= '0' && s[i] <= '9') {
            char num[32] = {};  // copie bornée : strtoul ne doit pas lire au-delà du fichier
            size_t k = 0;
            while (k < 31 && i + k < n && isalnum(uint8_t(s[i + k]))) num[k] = s[i + k], ++k;
            vals.push_back(uint16_t(strtoul(num, nullptr, 0)));
            i += k;
        } else {
            ++i;
        }
    }
    if (vals.size() < rowUnits * size_t(h)) return d.Fail(L"Fichier XBM tronqué");
    Rgba img;
    img.Init(uint32_t(w), uint32_t(h), 255);
    for (uint32_t y = 0; y < uint32_t(h); ++y) {
        uint8_t* o = img.Row(y);
        for (uint32_t x = 0; x < uint32_t(w); ++x, o += 4) {
            const uint16_t v = vals[y * rowUnits + x / unit];
            const bool on = (v >> (x % unit)) & 1;  // bit de poids faible = pixel de gauche
            o[0] = o[1] = o[2] = on ? 0 : 255;
        }
    }
    d.Format(x10 ? L"XBM (X10)" : L"XBM");
    return Emit(d, img, false);
}

int DecodeXpm(Dec& d) {
    const char* s = reinterpret_cast<const char*>(d.d);
    const size_t n = d.n;
    const size_t lead = SkipLead(d.d, n);
    const bool xpm2 = n - lead >= 6 && memcmp(s + lead, "! XPM2", 6) == 0;
    std::vector<std::string> lines;
    size_t needed = 1;  // ajusté après lecture de l'en-tête
    auto push = [&](std::string&& l) {
        lines.push_back(std::move(l));
        if (lines.size() == 1) {
            unsigned long hv[4] = {};
            const char* p = lines[0].c_str();
            char* e = nullptr;
            for (int k = 0; k < 4; ++k) {
                hv[k] = strtoul(p, &e, 10);
                if (e == p) return false;
                p = e;
            }
            if (!hv[0] || !hv[1] || !hv[2] || !hv[3] || hv[3] > 16 || hv[2] > (1u << 24) || !SizeOk(hv[0], hv[1]))
                return false;
            needed = 1 + hv[2] + hv[1];
        }
        return true;
    };
    size_t i = lead;
    if (xpm2) {
        while (i < n && s[i] != '\n') ++i;  // ligne « ! XPM2 »
        ++i;
        while (i < n && lines.size() < needed) {
            size_t e = i;
            while (e < n && s[e] != '\n') ++e;
            std::string l(s + i, e - i);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            i = e + 1;
            if (lines.empty() && (l.empty() || l[0] == '!')) continue;
            if (!push(std::move(l))) return d.Fail(L"En-tête XPM invalide");
        }
    } else {
        while (i < n && lines.size() < needed) {
            if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
                i += 2;
                while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                i += 2;
            } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
                while (i < n && s[i] != '\n') ++i;
            } else if (s[i] == '"') {
                std::string l;
                ++i;
                while (i < n && s[i] != '"') {
                    if (s[i] == '\\' && i + 1 < n) ++i;
                    l += s[i++];
                }
                if (i >= n) break;
                ++i;
                if (!push(std::move(l))) return d.Fail(L"En-tête XPM invalide");
            } else {
                ++i;
            }
            if ((lines.size() & 1023) == 0 && d.Cancelled()) return CYIMG_E_CANCELLED;
        }
    }
    if (lines.empty()) return d.Fail(L"En-tête XPM absent");
    if (lines.size() < needed) return d.Fail(L"Fichier XPM tronqué");
    unsigned long hv[4];
    {
        const char* p = lines[0].c_str();
        char* e = nullptr;
        for (int k = 0; k < 4; ++k) hv[k] = strtoul(p, &e, 10), p = e;
    }
    const uint32_t w = uint32_t(hv[0]), h = uint32_t(hv[1]), ncol = uint32_t(hv[2]), cpp = uint32_t(hv[3]);

    // Table des couleurs : clé = cpp caractères
    std::unordered_map<std::string, uint32_t> map;
    std::vector<uint32_t> direct(cpp <= 2 ? (cpp == 1 ? 256 : 65536) : 0, 0xFF000000u);  // ABGR noir opaque
    bool anyAlpha = false;
    int unknown = 0;
    for (uint32_t c = 0; c < ncol; ++c) {
        const std::string& l = lines[1 + c];
        if (l.size() < cpp) continue;
        const std::string key = l.substr(0, cpp);
        // Paires « clé valeur » : on préfère c, puis g, g4, m
        std::vector<std::string> tok;
        {
            size_t p = cpp;
            while (p < l.size()) {
                while (p < l.size() && Space(l[p])) ++p;
                size_t q = p;
                while (q < l.size() && !Space(l[q])) ++q;
                if (q > p) tok.emplace_back(l.substr(p, q - p));
                p = q;
            }
        }
        std::string best;
        int bestRank = 99;
        for (size_t t = 0; t < tok.size();) {
            const std::string& k = tok[t];
            const int rank = k == "c" ? 0 : k == "g" ? 1 : k == "g4" ? 2 : k == "m" ? 3 : k == "s" ? 99 : -1;
            if (rank < 0) {
                ++t;
                continue;
            }
            std::string v;
            size_t u = t + 1;
            for (; u < tok.size(); ++u) {
                const std::string& x = tok[u];
                if (x == "c" || x == "g" || x == "g4" || x == "m" || x == "s") break;
                if (!v.empty()) v += ' ';
                v += x;
            }
            if (rank < bestRank && !v.empty()) best = v, bestRank = rank;
            t = u;
        }
        uint8_t rgba[4] = {0, 0, 0, 255};
        if (!ParseColor(best, rgba)) ++unknown;
        anyAlpha |= rgba[3] != 255;
        const uint32_t v = uint32_t(rgba[0]) | uint32_t(rgba[1]) << 8 | uint32_t(rgba[2]) << 16 | uint32_t(rgba[3]) << 24;
        if (cpp == 1) direct[uint8_t(key[0])] = v;
        else if (cpp == 2) direct[uint8_t(key[0]) | uint8_t(key[1]) << 8] = v;
        else map[key] = v;
    }

    Rgba img;
    img.Init(w, h, 255);
    for (uint32_t y = 0; y < h; ++y) {
        const std::string& l = lines[1 + ncol + y];
        uint8_t* o = img.Row(y);
        for (uint32_t x = 0; x < w; ++x, o += 4) {
            const size_t p = size_t(x) * cpp;
            uint32_t v = 0xFF000000u;
            if (p + cpp <= l.size()) {
                if (cpp == 1) v = direct[uint8_t(l[p])];
                else if (cpp == 2) v = direct[uint8_t(l[p]) | uint8_t(l[p + 1]) << 8];
                else if (auto it = map.find(l.substr(p, cpp)); it != map.end()) v = it->second;
            }
            memcpy(o, &v, 4);
        }
    }
    wchar_t fmt[64];
    swprintf_s(fmt, L"%ls, %u couleurs", xpm2 ? L"XPM2" : L"XPM", ncol);
    d.Format(fmt);
    if (unknown) {
        wchar_t v[32];
        swprintf_s(v, L"%d (affichées en noir)", unknown);
        d.Info(L"Couleurs inconnues", v);
    }
    return Emit(d, img, anyAlpha);
}

}  // namespace classic
