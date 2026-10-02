// SVG / SVGZ rendu par Direct2D (ID2D1SvgDocument, Windows 10 1703+).
//
// Limites de Direct2D : pas de <text>, pas de filtres, pas de masques, pas d'<image> externes,
// pas de CSS. Pour les fichiers courants (Illustrator, Inkscape, icônes web), le texte est
// prétraité : les règles simples des feuilles <style> (élément, .classe, #id) et les attributs
// style="..." sont convertis en attributs de présentation, que Direct2D comprend.

#include "vector.h"

#include <shlwapi.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace vec {

namespace {

inline bool IsSpace(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

struct Attr {
    std::string name, value;
};

// Balise ouvrante en s[i] == '<' : nom (sans préfixe d'espace de noms), attributs, fin ('>' compris).
bool ParseTag(const std::string& s, size_t i, std::string& name, std::vector<Attr>& attrs, size_t& end) {
    size_t j = i + 1;
    while (j < s.size() && !IsSpace(s[j]) && s[j] != '>' && s[j] != '/') ++j;
    name = s.substr(i + 1, j - i - 1);
    if (auto c = name.find(':'); c != std::string::npos) name = name.substr(c + 1);
    attrs.clear();
    while (j < s.size()) {
        while (j < s.size() && IsSpace(s[j])) ++j;
        if (j >= s.size()) return false;
        if (s[j] == '>' || (s[j] == '/' && j + 1 < s.size() && s[j + 1] == '>')) {
            end = s[j] == '>' ? j + 1 : j + 2;
            return true;
        }
        Attr a;
        const size_t nb = j;
        while (j < s.size() && !IsSpace(s[j]) && s[j] != '=' && s[j] != '>' && s[j] != '/') ++j;
        a.name = s.substr(nb, j - nb);
        while (j < s.size() && IsSpace(s[j])) ++j;
        if (j >= s.size() || s[j] != '=') return false;
        ++j;
        while (j < s.size() && IsSpace(s[j])) ++j;
        if (j >= s.size() || (s[j] != '"' && s[j] != '\'')) return false;
        const char q = s[j++];
        const size_t ve = s.find(q, j);
        if (ve == std::string::npos) return false;
        a.value = s.substr(j, ve - j);
        j = ve + 1;
        if (a.name.empty()) return false;
        attrs.push_back(std::move(a));
    }
    return false;
}

// Saute commentaires, instructions, DOCTYPE (avec sous-ensemble interne) et CDATA en s[i] == '<'.
// Retourne la position suivante, ou i si ce n'est pas une de ces constructions.
size_t SkipMarkup(const std::string& s, size_t i) {
    auto after = [&](const char* tok, size_t from) {
        const size_t p = s.find(tok, from);
        return p == std::string::npos ? s.size() : p + strlen(tok);
    };
    if (!s.compare(i, 4, "<!--")) return after("-->", i + 4);
    if (!s.compare(i, 9, "<![CDATA[")) return after("]]>", i + 9);
    if (!s.compare(i, 2, "<?")) return after("?>", i + 2);
    if (!s.compare(i, 2, "<!")) {
        size_t j = i + 2;
        int depth = 0;
        while (j < s.size() && (s[j] != '>' || depth > 0)) {
            if (s[j] == '[') ++depth;
            else if (s[j] == ']') --depth;
            ++j;
        }
        return std::min(j + 1, s.size());
    }
    return i;
}

// Longueur SVG en pixels (96 ppp). false si absente, nulle ou en pourcentage.
bool ParseLength(const std::string& v, double& px) {
    const char* p = v.c_str();
    char* e = nullptr;
    const double x = strtod(p, &e);
    if (e == p || !(x > 0)) return false;
    while (*e == ' ') ++e;
    std::string u(e);
    while (!u.empty() && IsSpace(u.back())) u.pop_back();
    double k = 1;
    if (u.empty() || u == "px") k = 1;
    else if (u == "pt") k = 96.0 / 72;
    else if (u == "pc") k = 16;
    else if (u == "mm") k = 96 / 25.4;
    else if (u == "cm") k = 96 / 2.54;
    else if (u == "in") k = 96;
    else if (u == "em") k = 16;
    else if (u == "ex") k = 8;
    else return false;  // %, unité inconnue
    px = x * k;
    return px > 0 && px < 1e7;
}

bool ParseViewBox(const std::string& v, double vb[4]) {
    const char* p = v.c_str();
    for (int i = 0; i < 4; ++i) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        char* e = nullptr;
        vb[i] = strtod(p, &e);
        if (e == p) return false;
        p = e;
    }
    return vb[2] > 0 && vb[3] > 0 && vb[2] < 1e9 && vb[3] < 1e9;
}

// --- Mini cascade CSS -------------------------------------------------------------------------

using Decls = std::vector<std::pair<std::string, std::string>>;

struct CssRule {
    std::string sel;  // "*", "rect", ".a", ".a.b", "rect.a", "#id"
    Decls props;
    int Specificity() const { return sel[0] == '#' ? 2 : (sel.find('.') != std::string::npos ? 1 : 0); }
};

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) ++a;
    while (b > a && IsSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

void ParseDecls(const std::string& body, Decls& out) {
    size_t k = 0;
    while (k < body.size()) {
        size_t sc = body.find(';', k);
        if (sc == std::string::npos) sc = body.size();
        const std::string decl = body.substr(k, sc - k);
        k = sc + 1;
        const size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        std::string name = Trim(decl.substr(0, colon)), val = Trim(decl.substr(colon + 1));
        if (auto imp = val.find("!important"); imp != std::string::npos) val = Trim(val.substr(0, imp));
        for (char& ch : name) ch = char(tolower(uint8_t(ch)));
        if (!name.empty() && !val.empty()) out.emplace_back(name, val);
    }
}

using Vars = std::map<std::string, std::string>;

void ParseCss(std::string css, std::vector<CssRule>& rules, Vars& vars) {
    for (size_t i; (i = css.find("/*")) != std::string::npos;) {
        const size_t j = css.find("*/", i + 2);
        css.erase(i, j == std::string::npos ? std::string::npos : j + 2 - i);
    }
    for (const char* tok : {"<![CDATA[", "]]>"})
        for (size_t i; (i = css.find(tok)) != std::string::npos;) css.erase(i, strlen(tok));
    size_t i = 0;
    while (i < css.size()) {
        const size_t ob = css.find('{', i);
        if (ob == std::string::npos) break;
        const std::string sels = Trim(css.substr(i, ob - i));
        if (!sels.empty() && sels[0] == '@') {  // @media {...{...}} : bloc entier ignoré
            int depth = 0;
            size_t j = ob;
            for (; j < css.size(); ++j) {
                if (css[j] == '{') ++depth;
                else if (css[j] == '}' && --depth == 0) break;
            }
            i = j + 1;
            continue;
        }
        const size_t cb = css.find('}', ob);
        if (cb == std::string::npos) break;
        Decls props;
        ParseDecls(css.substr(ob + 1, cb - ob - 1), props);
        i = cb + 1;
        for (auto& [k, v] : props)  // propriétés personnalisées (--nom), quel que soit le sélecteur
            if (k.size() > 2 && k[0] == '-' && k[1] == '-') vars[k] = v;
        if (props.empty()) continue;
        size_t s0 = 0;
        while (s0 <= sels.size()) {
            size_t comma = sels.find(',', s0);
            if (comma == std::string::npos) comma = sels.size();
            const std::string sel = Trim(sels.substr(s0, comma - s0));
            s0 = comma + 1;
            // sélecteurs simples seulement (pas de descendants ni de pseudo-classes)
            if (sel.empty() || sel.find_first_of(" >+~:[") != std::string::npos) continue;
            rules.push_back({sel, props});
        }
    }
    std::stable_sort(rules.begin(), rules.end(), [](const CssRule& a, const CssRule& b) { return a.Specificity() < b.Specificity(); });
}

// Propriétés que Direct2D accepte comme attributs de présentation
bool KnownProperty(const std::string& n) {
    static const char* k[] = {"fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity",
                              "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
                              "stroke-dashoffset", "opacity", "display", "visibility", "color", "stop-color",
                              "stop-opacity", "overflow", "clip-path", "clip-rule"};
    for (auto* s : k)
        if (n == s) return true;
    return false;
}

bool HasClass(const std::string& cls, const std::string& want) {
    size_t a = 0;
    while (a < cls.size()) {
        while (a < cls.size() && IsSpace(cls[a])) ++a;
        size_t b = a;
        while (b < cls.size() && !IsSpace(cls[b])) ++b;
        if (b > a && cls.compare(a, b - a, want) == 0) return true;
        a = b;
    }
    return false;
}

bool Matches(const CssRule& r, const std::string& tag, const std::string& cls, const std::string& id) {
    const std::string& s = r.sel;
    if (s == "*") return true;
    if (s[0] == '#') return id == s.substr(1);
    const size_t dot = s.find('.');
    if (dot == std::string::npos) return s == tag;
    if (dot > 0 && s.compare(0, dot, tag) != 0) return false;
    size_t p = dot + 1;
    while (p <= s.size()) {
        size_t q = s.find('.', p);
        if (q == std::string::npos) q = s.size();
        if (!HasClass(cls, s.substr(p, q - p))) return false;
        p = q + 1;
    }
    return true;
}

// var(--nom, secours) -> valeur de --nom si connue, sinon valeur de secours
std::string ResolveVars(std::string v, const Vars& vars) {
    for (int guard = 0; guard < 16; ++guard) {
        const size_t p = v.find("var(");
        if (p == std::string::npos) break;
        size_t e = p + 4;
        int depth = 1;
        for (; e < v.size() && depth; ++e) {
            if (v[e] == '(') ++depth;
            else if (v[e] == ')') --depth;
        }
        const std::string inner = v.substr(p + 4, (depth ? v.size() : e - 1) - p - 4);
        const size_t comma = inner.find(',');
        std::string name = Trim(inner.substr(0, comma)), repl;
        if (auto it = vars.find(name); it != vars.end()) repl = it->second;
        else if (comma != std::string::npos) repl = Trim(inner.substr(comma + 1));
        v = v.substr(0, p) + repl + v.substr(e);
    }
    return Trim(v);
}

std::string EscapeAttr(const std::string& v) {
    std::string o;
    for (char ch : v) {
        if (ch == '"') o += "&quot;";
        else if (ch == '<') o += "&lt;";
        else o += ch;
    }
    return o;
}

// Réécrit les balises : règles CSS puis style="..." -> attributs de présentation
std::string ExpandStyles(const std::string& s, const std::vector<CssRule>& rules, const Vars& vars) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    size_t i = 0, copied = 0;
    std::string name;
    std::vector<Attr> attrs;
    while ((i = s.find('<', i)) != std::string::npos) {
        const size_t skip = SkipMarkup(s, i);
        if (skip != i) {
            i = skip;
            continue;
        }
        if (i + 1 < s.size() && s[i + 1] == '/') {
            ++i;
            continue;
        }
        size_t end = 0;
        if (!ParseTag(s, i, name, attrs, end)) {
            ++i;
            continue;
        }
        // <mask> et <filter> ne sont pas gérés par Direct2D, qui en dessinerait le contenu : retirés
        if (name == "mask" || name == "filter") {
            size_t close = end;
            if (s[end - 2] != '/') {
                close = s.find("</" + name, end);  // préfixe d'espace de noms ignoré (rare)
                if (close != std::string::npos) close = s.find('>', close);
                close = close == std::string::npos ? s.size() : close + 1;
            }
            out.append(s, copied, i - copied);
            copied = i = close;
            continue;
        }
        std::string cls, id, style;
        bool hasStyle = false, hasVar = false;
        for (Attr& a : attrs) {
            if (a.name == "mask" || a.name == "filter") a.value.clear(), hasVar = true;  // retirés
            if (a.value.find("var(") != std::string::npos) a.value = ResolveVars(a.value, vars), hasVar = true;
            if (a.name == "class") cls = a.value;
            else if (a.name == "id") id = a.value;
            else if (a.name == "style") style = a.value, hasStyle = true;
        }
        std::map<std::string, std::string> props;
        for (const CssRule& r : rules)
            if (Matches(r, name, cls, id))
                for (auto& [k, v] : r.props) props[k] = v;
        if (hasStyle) {
            Decls d;
            ParseDecls(style, d);
            for (auto& [k, v] : d) props[k] = v;
        }
        for (auto& [k, v] : props)
            if (v.find("var(") != std::string::npos) v = ResolveVars(v, vars);
        if (hasStyle || hasVar || !props.empty()) {
            std::string tag = s.substr(i, s.find_first_of(" \t\r\n/>", i + 1) - i);  // nom d'origine (préfixe compris)
            for (const Attr& a : attrs) {
                if (a.name == "style" || a.value.empty()) continue;
                if (KnownProperty(a.name) && props.count(a.name)) continue;  // CSS prioritaire
                tag += " " + a.name + "=\"" + EscapeAttr(a.value) + "\"";
            }
            for (auto& [k, v] : props)
                if (KnownProperty(k) && !v.empty()) tag += " " + k + "=\"" + EscapeAttr(v) + "\"";
            tag += s[end - 2] == '/' ? "/>" : ">";
            out.append(s, copied, i - copied);
            out += tag;
            copied = end;
        }
        i = end;
        if (name == "style") {  // contenu de <style> recopié tel quel
            const size_t close = s.find("</", i);
            if (close == std::string::npos) break;
            i = close;
        }
    }
    out.append(s, copied, std::string::npos);
    return out;
}

}  // namespace

int DecodeSvg(const Call& c, const std::vector<uint8_t>& data, bool gz) {
    std::string s(reinterpret_cast<const char*>(data.data()), data.size());
    // UTF-16 (rare) -> UTF-8
    if (s.size() >= 2 && uint8_t(s[0]) == 0xFF && uint8_t(s[1]) == 0xFE) {
        const wchar_t* w = reinterpret_cast<const wchar_t*>(data.data() + 2);
        const int wn = int((data.size() - 2) / 2);
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, wn, nullptr, 0, nullptr, nullptr);
        s.assign(size_t(std::max(n, 0)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, wn, s.data(), n, nullptr, nullptr);
    }

    // Feuilles de style et attributs style="..."
    std::vector<CssRule> rules;
    Vars vars;
    for (size_t i = 0; (i = s.find("<style", i)) != std::string::npos;) {
        const size_t gt = s.find('>', i);
        if (gt == std::string::npos) break;
        const size_t end = s.find("</style", gt);
        if (end == std::string::npos) break;
        if (s[gt - 1] != '/') ParseCss(s.substr(gt + 1, end - gt - 1), rules, vars);
        i = end + 7;
    }
    if (!rules.empty() || s.find("style=") != std::string::npos || s.find("var(") != std::string::npos ||
        s.find("<mask") != std::string::npos || s.find("<filter") != std::string::npos)
        s = ExpandStyles(s, rules, vars);

    // Racine : premier élément
    size_t root = 0, rootEnd = 0;
    std::string name;
    std::vector<Attr> attrs;
    for (size_t i = 0;;) {
        i = s.find('<', i);
        if (i == std::string::npos) return c.Fail(L"SVG invalide : élément racine <svg> introuvable");
        const size_t skip = SkipMarkup(s, i);
        if (skip != i) {
            i = skip;
            continue;
        }
        if (!ParseTag(s, i, name, attrs, rootEnd) || name != "svg")
            return c.Fail(L"SVG invalide : élément racine <svg> introuvable");
        root = i;
        break;
    }

    // Taille naturelle : width/height, sinon viewBox, sinon 1024
    double w = 0, h = 0, vb[4] = {};
    bool hasW = false, hasH = false, hasVb = false;
    for (const Attr& a : attrs) {
        if (a.name == "width") hasW = ParseLength(a.value, w);
        else if (a.name == "height") hasH = ParseLength(a.value, h);
        else if (a.name == "viewBox") hasVb = ParseViewBox(a.value, vb);
    }
    if (!hasW && !hasH) {
        if (hasVb) w = vb[2], h = vb[3];
        else w = h = 1024;
    } else if (!hasW) {
        w = hasVb ? h * vb[2] / vb[3] : h;
    } else if (!hasH) {
        h = hasVb ? w * vb[3] / vb[2] : w;
    }
    uint32_t rw = 0, rh = 0;
    RenderSize(c, w, h, rw, rh);

    // Réécriture de la racine : taille = celle du rendu, viewBox ajouté si absent
    {
        std::string tag = s.substr(root, s.find_first_of(" \t\r\n/>", root + 1) - root);
        for (const Attr& a : attrs)
            if (a.name != "width" && a.name != "height") tag += " " + a.name + "=\"" + EscapeAttr(a.value) + "\"";
        tag += " width=\"" + std::to_string(rw) + "\" height=\"" + std::to_string(rh) + "\"";
        if (!hasVb) {
            char b[96];
            sprintf_s(b, " viewBox=\"0 0 %g %g\"", w, h);
            tag += b;
        }
        tag += s[rootEnd - 2] == '/' ? "/>" : ">";
        s.replace(root, rootEnd - root, tag);
    }

    if (!D2DFactory()) return c.Fail(L"Direct2D indisponible");
    WicTarget t;
    if (!t.Create(rw, rh)) return c.Fail(L"Création de la surface de rendu impossible");
    ComPtr<ID2D1DeviceContext5> dc5;
    if (!t.dc || FAILED(t.dc.As(&dc5))) return c.Fail(L"Le rendu SVG demande Windows 10 version 1703 ou plus récente");

    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(s.data()), UINT(s.size())));
    if (!stream) return CYIMG_E_OUTOFMEMORY;
    ComPtr<ID2D1SvgDocument> doc;
    HRESULT hr = dc5->CreateSvgDocument(stream.Get(), D2D1::SizeF(float(rw), float(rh)), &doc);
    if (FAILED(hr)) return c.Fail(Fmt(L"SVG mal formé (XML invalide ou tronqué) ou non pris en charge par Direct2D (erreur 0x%08X)", unsigned(hr)).c_str());
    if (c.Cancelled()) return CYIMG_E_CANCELLED;

    dc5->BeginDraw();
    dc5->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc5->DrawSvgDocument(doc.Get());
    hr = dc5->EndDraw();
    if (FAILED(hr)) return c.Fail(Fmt(L"Rendu SVG impossible (erreur 0x%08X)", unsigned(hr)).c_str());

    const int r = t.Deliver(c, false);
    if (r != CYIMG_OK) return r;
    c.host->set_format(c.ctx, gz ? L"SVGZ (SVG compressé)" : L"SVG");
    c.Info(L"Taille d'origine", Fmt(L"%.0f × %.0f px", w, h));
    c.Info(L"Rendu", L"Direct2D (sans texte ni filtres)");
    return CYIMG_OK;
}

}  // namespace vec
