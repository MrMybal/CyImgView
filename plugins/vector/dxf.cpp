// DXF ASCII : rendu 2D de base (vue de dessus de l'espace objet) en Direct2D / DirectWrite.
//
// Entités : LINE, LWPOLYLINE (arrondis « bulge »), POLYLINE/VERTEX (dont maillages polyface),
// CIRCLE, ARC, ELLIPSE, SPLINE (B-spline rationnelle, sinon points d'ajustement), POINT, SOLID,
// TRACE, 3DFACE, HATCH (remplissage plein seulement), LEADER, TEXT, MTEXT, ATTRIB, INSERT de
// BLOCKS (échelle, rotation, réseaux ; récursion limitée) et DIMENSION (bloc anonyme associé).
// Couleurs ACI (table AutoCAD), vraies couleurs (420), BYLAYER / BYBLOCK, calques éteints/gelés.
// Fond sombre type AutoCAD ; la couleur 7 est donc blanche.

#include "vector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace vec {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kBackground = 0x212830;  // fond AutoCAD (RGB)
constexpr uint32_t kForeground = 0xFFFFFF;  // couleur 7 sur fond sombre
constexpr size_t kMaxPoints = 20'000'000;   // garde-fou (blocs imbriqués en réseau...)
constexpr int kMaxDepth = 16;

// --- Table des couleurs ACI ---------------------------------------------------------------------

uint32_t Aci(int i) {
    static const uint32_t base[10] = {0x000000, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF, kForeground, 0x808080, 0xC0C0C0};
    if (i <= 0 || i > 255) return kForeground;
    if (i < 10) return base[i];
    if (i >= 250) {
        static const uint8_t g[6] = {51, 91, 132, 173, 214, 255};
        const uint32_t v = g[i - 250];
        return v << 16 | v << 8 | v;
    }
    // 10..249 : 24 teintes (pas de 15°) x 5 luminosités x (saturé, pâle)
    static const double val[5] = {255, 165, 127, 76, 38};
    const int hue = (i - 10) / 10, sub = (i - 10) % 10;
    const double v = val[sub / 2];
    const double lo = (sub & 1) ? v * 0.5 : 0.0;  // composante minimale
    const double hdeg = hue * 15.0;
    const int sector = int(hdeg / 60.0);
    const double f = (hdeg - sector * 60.0) / 60.0;
    const double up = lo + (v - lo) * f, down = lo + (v - lo) * (1 - f);
    double r = 0, g = 0, b = 0;
    switch (sector) {
        case 0: r = v, g = up, b = lo; break;
        case 1: r = down, g = v, b = lo; break;
        case 2: r = lo, g = v, b = up; break;
        case 3: r = lo, g = down, b = v; break;
        case 4: r = up, g = lo, b = v; break;
        default: r = v, g = lo, b = down; break;
    }
    return uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
}

// --- Lecture des groupes (code, valeur) ----------------------------------------------------------

struct Group {
    int code;
    std::string_view v;
};

bool ParseGroups(const std::vector<uint8_t>& data, std::vector<Group>& out) {
    const char* p = reinterpret_cast<const char*>(data.data());
    const char* end = p + data.size();
    if (data.size() >= 3 && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
    auto line = [&](std::string_view& s) {
        if (p >= end) return false;
        const char* b = p;
        while (p < end && *p != '\n') ++p;
        const char* e = p;
        if (p < end) ++p;
        if (e > b && e[-1] == '\r') --e;
        s = std::string_view(b, size_t(e - b));
        return true;
    };
    out.reserve(data.size() / 12);
    std::string_view c, v;
    while (line(c) && line(v)) {
        size_t i = 0;
        while (i < c.size() && c[i] == ' ') ++i;
        bool neg = i < c.size() && c[i] == '-';
        if (neg) ++i;
        if (i >= c.size() || c[i] < '0' || c[i] > '9') return false;
        int code = 0;
        while (i < c.size() && c[i] >= '0' && c[i] <= '9' && code < 100000) code = code * 10 + (c[i++] - '0');
        out.push_back({neg ? -code : code, v});
        if (code == 0 && v == "EOF") break;
    }
    return !out.empty();
}

double Num(std::string_view s) {
    char buf[64];
    const size_t n = std::min(s.size(), sizeof(buf) - 1);
    memcpy(buf, s.data(), n);
    buf[n] = 0;
    const double d = strtod(buf, nullptr);
    return std::isfinite(d) ? d : 0.0;
}

std::string Upper(std::string_view s) {
    std::string r(s);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    for (char& ch : r)
        if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);
    return r;
}

// Entité : plage de groupes [beg, end) + sous-entités (VERTEX, ATTRIB)
struct Ent {
    const Group* g = nullptr;
    size_t n = 0;
    std::vector<Ent> sub;
    std::string_view Type() const { return g[0].v; }
    const Group* Find(int code) const {
        for (size_t i = 1; i < n; ++i)
            if (g[i].code == code) return &g[i];
        return nullptr;
    }
    double D(int code, double def = 0) const {
        const Group* f = Find(code);
        return f ? Num(f->v) : def;
    }
    int I(int code, int def = 0) const {
        const Group* f = Find(code);
        return f ? int(Num(f->v)) : def;
    }
    std::string_view S(int code) const {
        const Group* f = Find(code);
        return f ? f->v : std::string_view();
    }
};

struct Block {
    double bx = 0, by = 0;
    std::vector<Ent> ents;
};

struct Layer {
    uint32_t color = kForeground;
    bool visible = true;
};

// Affine 2D : x' = a x + c y + e ; y' = b x + d y + f
struct Xf {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    void Apply(double x, double y, double& ox, double& oy) const { ox = a * x + c * y + e, oy = b * x + d * y + f; }
    Xf operator*(const Xf& o) const {  // this ∘ o (o appliquée d'abord)
        return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
    }
    static Xf T(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Xf S(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    static Xf R(double deg) {
        const double r = deg * kPi / 180, cs = cos(r), sn = sin(r);
        return {cs, sn, -sn, cs, 0, 0};
    }
};

struct Figure {
    size_t first, count;
    bool closed;
};

struct TextPrim {
    ComPtr<IDWriteTextLayout> layout;
    Xf m;  // espace du texte (unités DirectWrite, y vers le bas) -> monde
    uint32_t color;
};

struct Scene {
    std::vector<std::pair<double, double>> pts;
    std::unordered_map<uint32_t, std::vector<Figure>> strokes, fills;
    std::vector<std::pair<std::pair<double, double>, uint32_t>> dots;
    std::vector<TextPrim> texts;
    double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
    bool overflow = false;

    void Grow(double x, double y) {
        minx = std::min(minx, x), maxx = std::max(maxx, x);
        miny = std::min(miny, y), maxy = std::max(maxy, y);
    }
    // points déjà dans le repère monde
    void Add(const std::vector<std::pair<double, double>>& p, bool closed, bool fill, uint32_t color) {
        if (p.size() < 2 || overflow) return;
        if (pts.size() + p.size() > kMaxPoints) {
            overflow = true;
            return;
        }
        for (auto& q : p)
            if (!std::isfinite(q.first) || !std::isfinite(q.second) || std::fabs(q.first) > 1e15 || std::fabs(q.second) > 1e15)
                return;
        for (auto& q : p) Grow(q.first, q.second);
        (fill ? fills : strokes)[color].push_back({pts.size(), p.size(), closed});
        pts.insert(pts.end(), p.begin(), p.end());
    }
};

// --- Géométrie --------------------------------------------------------------------------------

int Segments(double sweep) { return std::clamp(int(std::ceil(std::fabs(sweep) / (2 * kPi) * 144)), 4, 1024); }

void ArcPoints(double cx, double cy, double rx, double ry, double rot, double a0, double sweep,
               std::vector<std::pair<double, double>>& out, bool skipFirst = false) {
    const int n = Segments(sweep);
    const double cr = cos(rot), sr = sin(rot);
    for (int i = skipFirst ? 1 : 0; i <= n; ++i) {
        const double t = a0 + sweep * i / n;
        const double x = rx * cos(t), y = ry * sin(t);
        out.emplace_back(cx + x * cr - y * sr, cy + x * sr + y * cr);
    }
}

// Segment p0 -> p1 avec arrondi « bulge » (tan(angle/4)) ; ajoute les points sans p0.
void BulgeTo(double x0, double y0, double x1, double y1, double bulge, std::vector<std::pair<double, double>>& out) {
    const double dx = x1 - x0, dy = y1 - y0, L = std::hypot(dx, dy);
    if (std::fabs(bulge) < 1e-9 || L < 1e-12) {
        out.emplace_back(x1, y1);
        return;
    }
    const double theta = 4 * atan(bulge);
    const double k = (1 - bulge * bulge) / (2 * bulge) * 0.5;  // distance milieu -> centre / L
    const double cx = (x0 + x1) / 2 - dy * k, cy = (y0 + y1) / 2 + dx * k;
    const double r = std::hypot(x0 - cx, y0 - cy);
    ArcPoints(cx, cy, r, r, 0, atan2(y0 - cy, x0 - cx), theta, out, true);
}

// B-spline (rationnelle) par l'algorithme de De Boor
bool SplinePoints(int p, const std::vector<double>& U, const std::vector<std::pair<double, double>>& P,
                  const std::vector<double>& W, std::vector<std::pair<double, double>>& out) {
    const size_t n = P.size();
    if (p < 1 || p > 15 || n < size_t(p) + 1 || U.size() != n + size_t(p) + 1) return false;
    for (size_t i = 1; i < U.size(); ++i)
        if (U[i] < U[i - 1]) return false;
    const double t0 = U[size_t(p)], t1 = U[n];
    if (!(t1 > t0)) return false;
    const bool rational = W.size() == n;
    const int samples = int(std::clamp<size_t>(n * 12, 32, 4000));
    std::vector<double> dx(size_t(p) + 1), dy(size_t(p) + 1), dw(size_t(p) + 1);
    for (int s = 0; s <= samples; ++s) {
        const double t = s == samples ? t1 : t0 + (t1 - t0) * s / samples;
        size_t k = size_t(p);
        while (k + 1 < n && U[k + 1] <= t) ++k;
        for (int j = 0; j <= p; ++j) {
            const size_t idx = k - size_t(p) + size_t(j);
            const double w = rational ? W[idx] : 1.0;
            dx[size_t(j)] = P[idx].first * w, dy[size_t(j)] = P[idx].second * w, dw[size_t(j)] = w;
        }
        for (int r = 1; r <= p; ++r)
            for (int j = p; j >= r; --j) {
                const size_t i = k - size_t(p) + size_t(j);
                const double den = U[i + size_t(p) + 1 - size_t(r)] - U[i];
                const double a = den > 0 ? (t - U[i]) / den : 0;
                dx[size_t(j)] = (1 - a) * dx[size_t(j) - 1] + a * dx[size_t(j)];
                dy[size_t(j)] = (1 - a) * dy[size_t(j) - 1] + a * dy[size_t(j)];
                dw[size_t(j)] = (1 - a) * dw[size_t(j) - 1] + a * dw[size_t(j)];
            }
        const double w = dw[size_t(p)];
        if (std::fabs(w) < 1e-12) return false;
        out.emplace_back(dx[size_t(p)] / w, dy[size_t(p)] / w);
    }
    return true;
}

// --- Texte -----------------------------------------------------------------------------------

std::wstring Decode(std::string_view s, bool utf8) {
    if (s.empty()) return {};
    std::wstring w(s.size(), L'\0');
    int n = utf8 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), w.data(), int(w.size())) : 0;
    if (n <= 0) n = MultiByteToWideChar(1252, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    w.resize(size_t(std::max(n, 0)));
    return w;
}

// Codes %%x et \U+XXXX ; mtext : retire la mise en forme (\P = saut de ligne, {\f...;}, etc.)
std::wstring CleanText(const std::wstring& in, bool mtext) {
    std::wstring o;
    o.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const wchar_t ch = in[i];
        if (ch == L'%' && i + 2 < in.size() && in[i + 1] == L'%') {
            const wchar_t k = wchar_t(towlower(in[i + 2]));
            if (k == L'c') o += L'\x2300';
            else if (k == L'd') o += L'\xB0';
            else if (k == L'p') o += L'\xB1';
            else if (k == L'%') o += L'%';
            else if (k >= L'0' && k <= L'9') {  // %%nnn : code de caractère
                int v = 0;
                size_t j = i + 2;
                while (j < in.size() && j < i + 5 && in[j] >= L'0' && in[j] <= L'9') v = v * 10 + (in[j++] - L'0');
                if (v > 0) o += wchar_t(v);
                i = j - 1;
                continue;
            }
            i += 2;  // %%u, %%o : soulignement / surlignement ignorés
            continue;
        }
        if (ch == L'\\' && i + 1 < in.size()) {
            const wchar_t k = in[i + 1];
            if ((k == L'U' || k == L'u') && i + 6 < in.size() && in[i + 2] == L'+') {
                o += wchar_t(wcstoul(in.substr(i + 3, 4).c_str(), nullptr, 16));
                i += 6;
                continue;
            }
            if (mtext) {
                if (k == L'P' || k == L'X') o += L'\n', ++i;
                else if (k == L'~') o += L'\xA0', ++i;
                else if (k == L'\\' || k == L'{' || k == L'}') o += k, ++i;
                else if (wcschr(L"LlOoKkNn", k)) ++i;
                else if (k == L'S') {  // fraction \Sa^b; -> a/b
                    size_t e = in.find(L';', i);
                    if (e == std::wstring::npos) e = in.size();
                    for (size_t j = i + 2; j < e; ++j) o += (in[j] == L'^' || in[j] == L'#') ? L'/' : in[j];
                    i = e;
                } else {  // \fArial|b0;  \H2.5x;  \C1; ...
                    size_t e = in.find(L';', i);
                    i = e == std::wstring::npos ? in.size() : e;
                }
                continue;
            }
        }
        if (mtext && (ch == L'{' || ch == L'}')) continue;
        o += ch;
    }
    return o;
}

// --- Construction de la scène ------------------------------------------------------------------

struct Builder {
    Scene& sc;
    const std::unordered_map<std::string, Block>& blocks;
    const std::unordered_map<std::string, Layer>& layers;
    IDWriteFactory* dw;
    IDWriteTextFormat* fmt;
    bool utf8;
    size_t inserts = 0;

    // Couleur d'une entité ; false si son calque est masqué
    bool Color(const Ent& e, uint32_t byBlock, const std::string& parentLayer, uint32_t& out, std::string& layerOut) const {
        std::string layer = Upper(e.S(8));
        if (layer.empty()) layer = "0";
        if (layer == "0" && !parentLayer.empty()) layer = parentLayer;  // calque 0 dans un bloc : hérite
        layerOut = layer;
        const auto it = layers.find(layer);
        if (it != layers.end() && !it->second.visible) return false;
        const uint32_t layerColor = it != layers.end() ? it->second.color : kForeground;
        if (const Group* tc = e.Find(420)) {
            out = uint32_t(Num(tc->v)) & 0xFFFFFF;
            return true;
        }
        const int aci = e.I(62, 256);
        if (aci < 0) return false;  // entité invisible
        out = aci == 256 ? layerColor : aci == 0 ? byBlock : Aci(aci);
        return true;
    }

    // OCS : extrusion (0,0,-1) -> miroir sur X
    static Xf Ocs(const Ent& e) { return e.D(230, 1) < 0 ? Xf::S(-1, 1) : Xf{}; }

    void Emit(const Xf& m, std::vector<std::pair<double, double>>& p, bool closed, bool fill, uint32_t color) {
        for (auto& q : p) m.Apply(q.first, q.second, q.first, q.second);
        sc.Add(p, closed, fill, color);
    }

    void Text(const Xf& m, const std::wstring& s, double x, double y, double h, double rotDeg, double wf, double wrap,
              int hAlign, int vAlign, uint32_t color) {
        if (s.empty() || !(h > 0) || h > 1e12) return;
        // fonte de 100 unités ; hauteur DXF = hauteur des capitales (~0,716 em pour Arial)
        const double em = 100, cap = 71.6, k = h / cap;
        ComPtr<IDWriteTextLayout> lay;
        const float maxW = wrap > 0 ? float(wrap / k) : 1e7f;
        if (FAILED(dw->CreateTextLayout(s.c_str(), UINT32(s.size()), fmt, maxW, 1e7f, &lay))) return;
        if (wrap > 0) lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS tm{};
        lay->GetMetrics(&tm);
        UINT32 nl = 0;
        DWRITE_LINE_METRICS lm[1]{};
        lay->GetLineMetrics(lm, 1, &nl);
        const double base = nl ? lm[0].baseline : em * 0.9;
        const double width = wrap > 0 && hAlign != 0 ? maxW : tm.widthIncludingTrailingWhitespace;
        const double left = wrap > 0 ? 0 : tm.left;
        double u0 = -left, v0 = 0;
        if (hAlign == 1) u0 = -left - width / 2;
        else if (hAlign == 2) u0 = -left - width;
        switch (vAlign) {
            case 0: v0 = -base; break;                           // ligne de base
            case 1: v0 = -tm.height; break;                      // bas
            case 2: v0 = nl > 1 ? -tm.height / 2 : -(base - cap / 2); break;  // milieu
            default: v0 = -(base - cap); break;                  // haut
        }
        // texte (u, v vers le bas) -> local (u*wf, -v) * k -> rotation -> position -> m
        TextPrim t;
        t.layout = lay;
        t.m = m * Xf::T(x, y) * Xf::R(rotDeg) * Xf::S(k * wf, -k) * Xf::T(u0, v0);
        t.color = color;
        const double cw = tm.left + tm.widthIncludingTrailingWhitespace, ch = tm.height;
        const double cx[4] = {tm.left, cw, cw, tm.left}, cy[4] = {0, 0, ch, ch};
        for (int i = 0; i < 4; ++i) {
            double ox, oy;
            t.m.Apply(cx[i], cy[i], ox, oy);
            if (std::isfinite(ox) && std::isfinite(oy)) sc.Grow(ox, oy);
        }
        sc.texts.push_back(std::move(t));
    }

    void Entity(const Ent& e, const Xf& m, uint32_t byBlock, const std::string& parentLayer, int depth) {
        if (sc.overflow || e.I(67) == 1) return;  // espace papier ignoré
        uint32_t col;
        std::string layer;
        if (!Color(e, byBlock, parentLayer, col, layer)) return;
        const std::string_view t = e.Type();
        std::vector<std::pair<double, double>> p;
        if (t == "LINE") {
            p = {{e.D(10), e.D(20)}, {e.D(11), e.D(21)}};
            Emit(m, p, false, false, col);
        } else if (t == "CIRCLE" || t == "ARC") {
            const double r = e.D(40);
            if (!(r > 0)) return;
            double a0 = 0, sw = 2 * kPi;
            if (t == "ARC") {
                a0 = e.D(50) * kPi / 180;
                double a1 = e.D(51) * kPi / 180;
                while (a1 <= a0) a1 += 2 * kPi;
                sw = a1 - a0;
            }
            ArcPoints(e.D(10), e.D(20), r, r, 0, a0, sw, p);
            Emit(m * Ocs(e), p, t == "CIRCLE", false, col);
        } else if (t == "ELLIPSE") {
            const double mx = e.D(11), my = e.D(21), ratio = e.D(40, 1);
            const double R = std::hypot(mx, my);
            if (!(R > 0) || !(ratio > 0)) return;
            double a0 = e.D(41, 0), a1 = e.D(42, 2 * kPi);
            while (a1 <= a0) a1 += 2 * kPi;
            const bool flip = e.D(230, 1) < 0;  // extrusion inverse : petit axe de l'autre côté
            ArcPoints(e.D(10), e.D(20), R, flip ? -R * ratio : R * ratio, atan2(my, mx), a0, a1 - a0, p);
            Emit(m, p, std::fabs(a1 - a0 - 2 * kPi) < 1e-6, false, col);
        } else if (t == "LWPOLYLINE") {
            const bool closed = (e.I(70) & 1) != 0;
            std::vector<std::pair<double, double>> v;
            std::vector<double> bul;
            for (size_t i = 1; i < e.n; ++i) {
                if (e.g[i].code == 10) v.emplace_back(Num(e.g[i].v), 0), bul.push_back(0);
                else if (e.g[i].code == 20 && !v.empty()) v.back().second = Num(e.g[i].v);
                else if (e.g[i].code == 42 && !bul.empty()) bul.back() = Num(e.g[i].v);
            }
            Poly(v, bul, closed, m * Ocs(e), col);
        } else if (t == "POLYLINE") {
            const int flags = e.I(70);
            if (flags & 64) {  // maillage polyface : arêtes des faces
                std::vector<std::pair<double, double>> v;
                for (const Ent& s : e.sub)
                    if (s.Type() == "VERTEX" && (s.I(70) & 64)) v.emplace_back(s.D(10), s.D(20));
                for (const Ent& s : e.sub) {
                    if (s.Type() != "VERTEX" || (s.I(70) & 192) != 128) continue;
                    p.clear();
                    for (int c = 71; c <= 74; ++c) {
                        const int idx = std::abs(s.I(c));
                        if (idx >= 1 && size_t(idx) <= v.size()) p.push_back(v[size_t(idx) - 1]);
                    }
                    Emit(m, p, true, false, col);
                }
                return;
            }
            if (flags & 16) return;  // maillage polygonal 3D : ignoré
            std::vector<std::pair<double, double>> v;
            std::vector<double> bul;
            for (const Ent& s : e.sub) {
                if (s.Type() != "VERTEX" || (s.I(70) & 16)) continue;  // points de contrôle de lissage
                v.emplace_back(s.D(10), s.D(20));
                bul.push_back(s.D(42));
            }
            Poly(v, bul, (flags & 1) != 0, (flags & 8) ? m : m * Ocs(e), col);
        } else if (t == "SPLINE") {
            const int deg = e.I(71, 3), flags = e.I(70);
            std::vector<double> U, W;
            std::vector<std::pair<double, double>> P, F;
            for (size_t i = 1; i < e.n; ++i) {
                const int c = e.g[i].code;
                if (c == 40) U.push_back(Num(e.g[i].v));
                else if (c == 41) W.push_back(Num(e.g[i].v));
                else if (c == 10) P.emplace_back(Num(e.g[i].v), 0);
                else if (c == 20 && !P.empty()) P.back().second = Num(e.g[i].v);
                else if (c == 11) F.emplace_back(Num(e.g[i].v), 0);
                else if (c == 21 && !F.empty()) F.back().second = Num(e.g[i].v);
            }
            if (!SplinePoints(deg, U, P, W, p)) p = F.size() >= 2 ? F : P;
            Emit(m, p, (flags & 1) != 0 && p == P, false, col);
        } else if (t == "POINT") {
            double x, y;
            m.Apply(e.D(10), e.D(20), x, y);
            if (std::isfinite(x) && std::isfinite(y)) {
                sc.dots.push_back({{x, y}, col});
                sc.Grow(x, y);
            }
        } else if (t == "SOLID" || t == "TRACE") {
            p = {{e.D(10), e.D(20)}, {e.D(11), e.D(21)}, {e.D(13, e.D(12)), e.D(23, e.D(22))}, {e.D(12), e.D(22)}};
            Emit(m * Ocs(e), p, true, true, col);
        } else if (t == "3DFACE") {
            p = {{e.D(10), e.D(20)}, {e.D(11), e.D(21)}, {e.D(12), e.D(22)}, {e.D(13, e.D(12)), e.D(23, e.D(22))}};
            Emit(m, p, true, false, col);
        } else if (t == "LEADER") {
            for (size_t i = 1; i < e.n; ++i) {
                if (e.g[i].code == 10) p.emplace_back(Num(e.g[i].v), 0);
                else if (e.g[i].code == 20 && !p.empty()) p.back().second = Num(e.g[i].v);
            }
            Emit(m, p, false, false, col);
        } else if (t == "HATCH") {
            Hatch(e, m * Ocs(e), col);
        } else if (t == "TEXT" || t == "ATTRIB") {
            if (t == "ATTRIB" && (e.I(70) & 1)) return;  // attribut invisible
            const int h = e.I(72), v = e.I(t == "ATTRIB" ? 74 : 73);
            const bool second = (h || v) && e.Find(11);
            double x = second ? e.D(11) : e.D(10), y = second ? e.D(21) : e.D(20);
            double rot = e.D(50), wf = e.D(41, 1);
            int ha = h == 1 || h == 4 ? 1 : h == 2 ? 2 : 0, va = h == 4 ? 2 : v;
            if (h == 3 || h == 5) {  // aligné / ajusté : entre les deux points
                const double x0 = e.D(10), y0 = e.D(20), x1 = e.D(11), y1 = e.D(21);
                rot = atan2(y1 - y0, x1 - x0) * 180 / kPi;
                x = x0, y = y0, ha = 0, va = 0;
            }
            if (!(wf > 0)) wf = 1;
            Text(m * Ocs(e), CleanText(Decode(e.S(1), utf8), false), x, y, e.D(40), rot, wf, 0, ha, va, col);
        } else if (t == "MTEXT") {
            std::string raw;
            for (size_t i = 1; i < e.n; ++i)
                if (e.g[i].code == 3) raw += e.g[i].v;
            raw += e.S(1);
            double rot = e.D(50);
            if (e.Find(11)) rot = atan2(e.D(21), e.D(11)) * 180 / kPi;
            const int att = std::clamp(e.I(71, 1), 1, 9);
            const int ha = (att - 1) % 3, va = att <= 3 ? 3 : att <= 6 ? 2 : 1;
            Text(m, CleanText(Decode(raw, utf8), true), e.D(10), e.D(20), e.D(40, 1), rot, 1, e.D(41), ha, va, col);
        } else if (t == "INSERT") {
            if (depth >= kMaxDepth) return;
            const auto it = blocks.find(Upper(e.S(2)));
            if (it != blocks.end()) {
                const Block& b = it->second;
                double sx = e.D(41, 1), sy = e.D(42, 1);
                if (sx == 0) sx = 1;
                if (sy == 0) sy = 1;
                const int cols = std::clamp(e.I(70, 1), 1, 1000), rows = std::clamp(e.I(71, 1), 1, 1000);
                const double cs = e.D(44), rs = e.D(45);
                const Xf base = m * Ocs(e) * Xf::T(e.D(10), e.D(20)) * Xf::R(e.D(50));
                for (int r = 0; r < rows && !sc.overflow; ++r)
                    for (int c = 0; c < cols && !sc.overflow; ++c) {
                        if (++inserts > 2'000'000) {
                            sc.overflow = true;
                            break;
                        }
                        const Xf bm = base * Xf::T(c * cs, r * rs) * Xf::S(sx, sy) * Xf::T(-b.bx, -b.by);
                        for (const Ent& s : b.ents) {
                            if (s.Type() == "ATTDEF") continue;  // modèles d'attributs
                            Entity(s, bm, col, layer, depth + 1);
                        }
                    }
            }
            for (const Ent& s : e.sub)  // attributs (déjà en coordonnées du dessin)
                if (s.Type() == "ATTRIB") Entity(s, m, col, layer, depth + 1);
        } else if (t == "DIMENSION") {
            if (depth >= kMaxDepth) return;
            const auto it = blocks.find(Upper(e.S(2)));
            if (it == blocks.end()) return;
            for (const Ent& s : it->second.ents) Entity(s, m, col, layer, depth + 1);
        }
    }

    void Poly(const std::vector<std::pair<double, double>>& v, const std::vector<double>& bul, bool closed, const Xf& m,
              uint32_t col) {
        if (v.size() < 2) return;
        std::vector<std::pair<double, double>> p{v[0]};
        for (size_t i = 0; i + 1 < v.size(); ++i) BulgeTo(v[i].first, v[i].second, v[i + 1].first, v[i + 1].second, bul[i], p);
        if (closed && bul.back() != 0) {
            BulgeTo(v.back().first, v.back().second, v[0].first, v[0].second, bul.back(), p);
            p.pop_back();  // le point de départ est refermé par la figure
        }
        Emit(m, p, closed, false, col);
    }

    // HATCH plein : contours (polylignes ou arêtes lignes/arcs/ellipses/splines), remplissage pair-impair
    void Hatch(const Ent& e, const Xf& m, uint32_t col) {
        if (e.I(70) != 1) return;  // motifs non rendus
        size_t i = 1;
        while (i < e.n && e.g[i].code != 91) ++i;
        if (i >= e.n) return;
        const int paths = std::clamp(int(Num(e.g[i++].v)), 0, 100000);
        auto next = [&](int code, double def = 0) {  // prochain groupe attendu (tolère les absents)
            for (size_t j = i; j < e.n && j < i + 4; ++j)
                if (e.g[j].code == code) {
                    i = j + 1;
                    return Num(e.g[j].v);
                }
            return def;
        };
        std::vector<std::vector<std::pair<double, double>>> loops;
        for (int pi = 0; pi < paths && i < e.n; ++pi) {
            while (i < e.n && e.g[i].code != 92) ++i;
            if (i >= e.n) break;
            const int type = int(Num(e.g[i++].v));
            std::vector<std::pair<double, double>> loop;
            if (type & 2) {  // polyligne
                const bool hasBulge = next(72) != 0;
                next(73);
                const int nv = std::clamp(int(next(93)), 0, 1000000);
                std::vector<std::pair<double, double>> v;
                std::vector<double> bul;
                for (int k = 0; k < nv && i < e.n; ++k) {
                    const double x = next(10), y = next(20);
                    v.emplace_back(x, y);
                    bul.push_back(hasBulge ? next(42) : 0);
                }
                if (!v.empty()) {
                    loop.push_back(v[0]);
                    for (size_t k = 0; k < v.size(); ++k) {
                        const auto& b = v[(k + 1) % v.size()];
                        BulgeTo(v[k].first, v[k].second, b.first, b.second, bul[k], loop);
                    }
                }
            } else {
                const int ne = std::clamp(int(next(93)), 0, 1000000);
                for (int k = 0; k < ne && i < e.n; ++k) {
                    const int et = int(next(72));
                    if (et == 1) {
                        const double x0 = next(10), y0 = next(20), x1 = next(11), y1 = next(21);
                        loop.emplace_back(x0, y0), loop.emplace_back(x1, y1);
                    } else if (et == 2 || et == 3) {
                        const double cx = next(10), cy = next(20);
                        double rx, ry, rot = 0;
                        if (et == 2) {
                            rx = ry = next(40);
                        } else {
                            const double mx = next(11), my = next(21), ratio = next(40);
                            rx = std::hypot(mx, my), ry = rx * ratio, rot = atan2(my, mx);
                        }
                        double a0 = next(50) * kPi / 180, a1 = next(51) * kPi / 180;
                        const bool ccw = next(73, 1) != 0;
                        if (!ccw) a0 = -a0, a1 = -a1;
                        while (ccw ? a1 <= a0 : a1 >= a0) a1 += ccw ? 2 * kPi : -2 * kPi;
                        ArcPoints(cx, cy, rx, ry, rot, a0, a1 - a0, loop);
                    } else if (et == 4) {
                        const int deg = int(next(94)), rat = int(next(73));
                        next(74);
                        const int nk = std::clamp(int(next(95)), 0, 100000), nc = std::clamp(int(next(96)), 0, 100000);
                        std::vector<double> U, W;
                        std::vector<std::pair<double, double>> P;
                        for (int q = 0; q < nk && i < e.n; ++q) U.push_back(next(40));
                        for (int q = 0; q < nc && i < e.n; ++q) {
                            const double x = next(10), y = next(20);
                            P.emplace_back(x, y);
                            if (rat) W.push_back(next(42, 1));
                        }
                        if (!SplinePoints(deg, U, P, W, loop)) loop.insert(loop.end(), P.begin(), P.end());
                        // points d'ajustement éventuels (97) : ignorés
                    } else {
                        break;  // arête inconnue : contour abandonné
                    }
                }
            }
            if (loop.size() >= 3) loops.push_back(std::move(loop));
        }
        for (auto& l : loops) Emit(m, l, true, true, col);
    }
};

}  // namespace

int DecodeDxf(const Call& c, const std::vector<uint8_t>& data) {
    std::vector<Group> g;
    if (!ParseGroups(data, g)) return c.Fail(CYIMG_TR("plugin.vector.dxf.err.groups", L"DXF illisible (groupes code/valeur invalides)"));
    if (c.Cancelled()) return CYIMG_E_CANCELLED;

    // Découpage en sections et entités
    std::unordered_map<std::string, Layer> layers;
    std::unordered_map<std::string, Block> blocks;
    std::vector<Ent> entities;
    std::string version;
    bool utf8 = false;
    size_t i = 0;
    auto readEntities = [&](size_t& k, std::vector<Ent>& out, std::string_view stop1, std::string_view stop2) {
        // de g[k] (code 0) jusqu'à ENDSEC / ENDBLK
        while (k < g.size()) {
            if (g[k].code != 0) {
                ++k;
                continue;
            }
            if (g[k].v == stop1 || g[k].v == stop2) return;
            Ent e;
            e.g = &g[k];
            size_t j = k + 1;
            while (j < g.size() && g[j].code != 0) ++j;
            e.n = j - k;
            k = j;
            const bool hasSub = e.Type() == "POLYLINE" || (e.Type() == "INSERT" && e.I(66) == 1);
            if (hasSub) {
                while (k < g.size() && g[k].code == 0 && g[k].v != "SEQEND" && g[k].v != stop1 && g[k].v != stop2 &&
                       (g[k].v == "VERTEX" || g[k].v == "ATTRIB")) {
                    Ent s;
                    s.g = &g[k];
                    size_t q = k + 1;
                    while (q < g.size() && g[q].code != 0) ++q;
                    s.n = q - k;
                    k = q;
                    e.sub.push_back(std::move(s));
                }
                if (k < g.size() && g[k].v == "SEQEND") {
                    ++k;
                    while (k < g.size() && g[k].code != 0) ++k;
                }
            }
            out.push_back(std::move(e));
        }
    };
    while (i < g.size()) {
        if (!(g[i].code == 0 && g[i].v == "SECTION") || i + 1 >= g.size()) {
            ++i;
            continue;
        }
        const std::string_view name = g[i + 1].v;
        i += 2;
        if (name == "HEADER") {
            for (; i < g.size() && !(g[i].code == 0 && g[i].v == "ENDSEC"); ++i)
                if (g[i].code == 9 && g[i].v == "$ACADVER" && i + 1 < g.size()) version = std::string(g[i + 1].v);
        } else if (name == "TABLES") {
            for (; i < g.size() && !(g[i].code == 0 && g[i].v == "ENDSEC"); ++i) {
                if (!(g[i].code == 0 && g[i].v == "LAYER")) continue;
                Ent e;
                e.g = &g[i];
                size_t j = i + 1;
                while (j < g.size() && g[j].code != 0) ++j;
                e.n = j - i;
                if (!e.Find(2)) continue;  // en-tête de la table
                Layer l;
                const int col = e.I(62, 7);
                l.visible = col >= 0 && !(e.I(70) & 1);
                l.color = e.Find(420) ? uint32_t(e.D(420)) & 0xFFFFFF : Aci(std::abs(col));
                layers[Upper(e.S(2))] = l;
            }
        } else if (name == "BLOCKS") {
            while (i < g.size() && !(g[i].code == 0 && g[i].v == "ENDSEC")) {
                if (!(g[i].code == 0 && g[i].v == "BLOCK")) {
                    ++i;
                    continue;
                }
                Ent be;
                be.g = &g[i];
                size_t j = i + 1;
                while (j < g.size() && g[j].code != 0) ++j;
                be.n = j - i;
                i = j;
                Block b;
                b.bx = be.D(10), b.by = be.D(20);
                readEntities(i, b.ents, "ENDBLK", "ENDSEC");
                std::string bn = Upper(be.S(2));
                if (!bn.empty()) blocks[bn] = std::move(b);
                if (i < g.size() && g[i].v == "ENDBLK") ++i;
            }
        } else if (name == "ENTITIES") {
            readEntities(i, entities, "ENDSEC", "ENDSEC");
        }
        while (i < g.size() && !(g[i].code == 0 && g[i].v == "ENDSEC")) ++i;
    }
    if (entities.empty()) return c.Fail(CYIMG_TR("plugin.vector.dxf.err.no_entity", L"DXF sans entité dans l'espace objet"));
    utf8 = version >= "AC1021";  // AutoCAD 2007+ : UTF-8
    if (c.Cancelled()) return CYIMG_E_CANCELLED;

    IDWriteFactory* dw = DWriteFactory();
    if (!dw || !D2DFactory()) return c.Fail(CYIMG_TR("plugin.vector.err.d2d_dwrite", L"Direct2D / DirectWrite indisponible"));
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dw->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 100.f, L"fr-fr", &fmt)))
        return c.Fail(CYIMG_TR("plugin.vector.err.font", L"Police de caractères indisponible"));
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    Scene sc;
    Builder b{sc, blocks, layers, dw, fmt.Get(), utf8};
    for (const Ent& e : entities) {
        b.Entity(e, Xf{}, kForeground, std::string(), 0);
        if (sc.overflow || c.Cancelled()) break;
    }
    if (c.Cancelled()) return CYIMG_E_CANCELLED;
    if (!(sc.maxx >= sc.minx) || !(sc.maxy >= sc.miny)) return c.Fail(CYIMG_TR("plugin.vector.dxf.err.no_geometry", L"DXF sans géométrie 2D affichable"));

    // Taille : plus grand côté 4096 px (2048 au moins), marge de 2 %
    double ew = sc.maxx - sc.minx, eh = sc.maxy - sc.miny;
    const double ext = std::max(ew, eh);
    if (!(ext > 0)) ew = eh = 1;
    else ew = std::max(ew, ext * 1e-3), eh = std::max(eh, ext * 1e-3);
    uint32_t rw = 0, rh = 0;
    const double k0 = (c.Target() ? 1024.0 : 4096.0) / std::max(ew, eh);
    RenderSize(c, ew * k0, eh * k0, rw, rh);
    const double margin = std::max(2.0, std::max(rw, rh) * 0.02);
    const double s = std::min((rw - 2 * margin) / ew, (rh - 2 * margin) / eh);
    const double ox = (rw - ew * s) / 2, oy = (rh - eh * s) / 2;
    const double minx = sc.minx, maxy = sc.maxy;
    auto px = [&](double x, double y) { return D2D1::Point2F(float(ox + (x - minx) * s), float(oy + (maxy - y) * s)); };

    WicTarget t;
    if (!t.Create(rw, rh)) return c.Fail(CYIMG_TR("plugin.vector.err.surface", L"Création de la surface de rendu impossible"));
    ID2D1RenderTarget* rt = t.rt.Get();
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const float stroke = std::max(1.0f, float(std::max(rw, rh)) / 2048.f);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(kBackground));
    auto brushFor = [&](uint32_t col) {
        ComPtr<ID2D1SolidColorBrush> br;
        rt->CreateSolidColorBrush(D2D1::ColorF(col), &br);
        return br;
    };
    auto build = [&](const std::vector<Figure>& figs, bool fill) {
        ComPtr<ID2D1PathGeometry> geo;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(D2DFactory()->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) return ComPtr<ID2D1PathGeometry>();
        sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
        std::vector<D2D1_POINT_2F> tmp;
        for (const Figure& f : figs) {
            tmp.clear();
            for (size_t q = 0; q < f.count; ++q) tmp.push_back(px(sc.pts[f.first + q].first, sc.pts[f.first + q].second));
            sink->BeginFigure(tmp[0], fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLines(tmp.data() + 1, UINT32(tmp.size() - 1));
            sink->EndFigure(f.closed || fill ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        }
        sink->Close();
        return geo;
    };
    // remplissages (hachures pleines, solides) d'abord, puis traits, points et textes
    for (auto& [col, figs] : sc.fills)
        if (auto geo = build(figs, true)) rt->FillGeometry(geo.Get(), brushFor(col).Get());
    if (c.Cancelled()) {
        rt->EndDraw();
        return CYIMG_E_CANCELLED;
    }
    ComPtr<ID2D1StrokeStyle> round;
    D2DFactory()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_LINE_JOIN_ROUND),
                                    nullptr, 0, &round);
    for (auto& [col, figs] : sc.strokes)
        if (auto geo = build(figs, false)) rt->DrawGeometry(geo.Get(), brushFor(col).Get(), stroke, round.Get());
    for (auto& [pt, col] : sc.dots) {
        const auto q = px(pt.first, pt.second);
        rt->FillEllipse(D2D1::Ellipse(q, stroke * 1.5f, stroke * 1.5f), brushFor(col).Get());
    }
    for (const TextPrim& tp : sc.texts) {
        // espace du texte -> pixels : monde -> pixels (échelle s, Y inversé) appliqué après tp.m
        const Xf w2p{s, 0, 0, -s, ox - minx * s, oy + maxy * s};
        const Xf m = w2p * tp.m;
        const double scale = std::sqrt(std::fabs(m.a * m.d - m.b * m.c)) * 100;  // hauteur d'em en pixels
        if (!(scale > 0.5)) continue;  // texte illisible : inutile de le dessiner
        rt->SetTransform(D2D1::Matrix3x2F(float(m.a), float(m.b), float(m.c), float(m.d), float(m.e), float(m.f)));
        rt->DrawTextLayout(D2D1::Point2F(0, 0), tp.layout.Get(), brushFor(tp.color).Get());
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT hr = rt->EndDraw();
    if (FAILED(hr)) return c.Fail(Fmt(CYIMG_TR("plugin.vector.dxf.err.render", L"Rendu DXF impossible (erreur 0x%08X)"), unsigned(hr)).c_str());
    const int r = t.Deliver(c, true);
    if (r != CYIMG_OK) return r;

    c.host->set_format(c.ctx, L"DXF (AutoCAD)");
    if (!version.empty()) c.Info(CYIMG_TR("plugin.vector.info.dxf_version", L"Version DXF"), Decode(version, false));
    c.Info(CYIMG_TR("plugin.vector.info.entities", L"Entités"), Fmt(L"%zu", entities.size()));
    c.Info(CYIMG_TR("plugin.vector.info.blocks", L"Blocs"), Fmt(L"%zu", blocks.size()));
    c.Info(CYIMG_TR("plugin.vector.info.extent", L"Étendue"), Fmt(CYIMG_TR("plugin.vector.dxf.extent_value", L"%g × %g unités"), sc.maxx - sc.minx, sc.maxy - sc.miny));
    if (sc.overflow) c.Info(CYIMG_TR("plugin.vector.info.warning", L"Avertissement"), CYIMG_TR("plugin.vector.dxf.truncated", L"dessin tronqué (trop d'éléments)"));
    return CYIMG_OK;
}

}  // namespace vec
