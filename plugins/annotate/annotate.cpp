// Plugin CyImgView : outils d'annotation pour les captures d'écran (modèle d'un plugin d'édition qui
// ajoute des outils).
//
//  - Pastille numérotée : un clic pose un disque de la couleur principale avec un numéro, qui
//    s'incrémente (clic droit : couleur secondaire).
//  - Surligneur : trait épais en mode produit (le texte reste lisible dessous) ; Maj : trait droit.
//  - Flou de masquage : le rectangle tracé est pixellisé, flouté ou noirci (cacher une information).
//  - Bulle de texte : glisser de la pointe vers l'emplacement de la bulle (ou simple clic), puis saisir
//    le texte.
//
// Un outil ne dessine pas lui-même les pixels : il utilise les primitives de l'hôte (Direct2D,
// antialiasées, limitées à la sélection, annulables) ou lock_pixels pour un traitement direct. Les
// rappels sont appelés sur le thread de l'interface, un seul à la fois : l'état d'un geste peut
// rester dans des variables statiques.

#include "cyimg_edit_plugin.h"
#include "badge_icon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

inline uint32_t Opaque(uint32_t rgb) { return 0xFF000000u | (rgb & 0xFFFFFFu); }
// Texte lisible sur un fond : noir sur les couleurs claires, blanc sur les sombres
inline uint32_t Readable(uint32_t rgb) {
    const double l = 0.299 * ((rgb >> 16) & 0xFF) + 0.587 * ((rgb >> 8) & 0xFF) + 0.114 * (rgb & 0xFF);
    return l > 160 ? 0xFF111111u : 0xFFFFFFFFu;
}
inline const wchar_t* Tr(const CyImgEditHost* h, CyImgEditCtx* c, const char* key, const wchar_t* def) {
    return cyimg_edit_tr(h, c, key, def);
}

struct Hover {
    bool on = false;
    double x = 0, y = 0;
};

// Tailles en pixels de l'image : adaptées à la définition du document quand l'outil est choisi
// (44 px conviennent à une capture d'écran, pas à une photo de 24 Mpx). Référence : 1600 px.
struct SizeRef {
    double longSide = 0;  // grand côté du document lors de la dernière adaptation
};
void AdaptSize(const CyImgEditHost* h, CyImgEditCtx* c, uint32_t param, double def, SizeRef& ref) {
    uint32_t w = 0, hh = 0;
    h->doc_size(c, &w, &hh);
    const double l = double(std::max(w, hh));
    if (l <= 0) return;
    if (ref.longSide <= 0) {
        if (h->param(c, param) == def && l > 1600) h->set_param(c, param, std::round(def * l / 1600));
    } else if (l != ref.longSide) {
        h->set_param(c, param, std::round(h->param(c, param) * l / ref.longSide));
    }
    ref.longSide = l;
}

// --- Pastille numérotée ----------------------------------------------------------------------

enum { kBadgeSize, kBadgeNumber };
const CyImgParam kBadgeParams[] = {
    CYIMG_SLIDER("plugin.annotate.size", L"Taille", 12, 400, 44, 1, L"px"),
    CYIMG_SLIDER("plugin.annotate.number", L"Numéro", 1, 999, 1, 1, nullptr),
};
Hover g_badgeHover;

void DrawBadge(const CyImgEditHost* h, CyImgEditCtx* c, double x, double y, uint32_t rgb) {
    const double size = h->param(c, kBadgeSize), r = size / 2;
    const int num = int(h->param(c, kBadgeNumber));
    h->draw_ellipse(c, x, y, r, r, 0, Opaque(rgb));
    h->draw_ellipse(c, x, y, r - size * 0.035, r - size * 0.035, size * 0.07, 0xFFFFFFFFu);  // liseré blanc
    wchar_t t[16];
    swprintf(t, 16, L"%d", num);
    const double ts = size * (num < 10 ? 0.6 : num < 100 ? 0.48 : 0.38);
    h->draw_text(c, x, y - ts * 0.04, 0, t, ts, Readable(rgb), CYIMG_TEXT_BOLD | CYIMG_TEXT_CENTER | CYIMG_TEXT_VCENTER);
}

int BadgeDown(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent* ev) {
    const bool right = (ev->buttons & CYIMG_BTN_RIGHT) != 0;
    DrawBadge(h, c, ev->x, ev->y, right ? h->secondary_color(c) : h->primary_color(c));
    h->set_param(c, kBadgeNumber, h->param(c, kBadgeNumber) + 1);  // numéro suivant (affiché dans la barre)
    return 0;  // pas de glissement
}
void BadgeMove(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent* ev) {
    g_badgeHover = {true, ev->x, ev->y};
    h->redraw(c);
}
void BadgeOverlay(const CyImgEditHost* h, CyImgEditCtx* c) {
    if (!g_badgeHover.on) return;
    h->begin_group(c, 0.55, CYIMG_BLEND_NORMAL);  // aperçu de la prochaine pastille
    DrawBadge(h, c, g_badgeHover.x, g_badgeHover.y, h->primary_color(c));
    h->end_group(c);
}
SizeRef g_badgeRef;
void BadgeActivate(const CyImgEditHost* h, CyImgEditCtx* c, int on) {
    g_badgeHover.on = false;
    if (on) AdaptSize(h, c, kBadgeSize, 44, g_badgeRef);
}

// --- Surligneur ------------------------------------------------------------------------------

enum { kHlWidth, kHlColor, kHlOpacity };
const CyImgParam kHighlightParams[] = {
    CYIMG_SLIDER("plugin.annotate.width", L"Épaisseur", 2, 300, 24, 1, L"px"),
    CYIMG_COLOR("plugin.annotate.color", L"Couleur", 0xFFE81F),
    CYIMG_SLIDER("plugin.annotate.opacity", L"Opacité", 10, 100, 100, 1, L"%"),
};
std::vector<double> g_hlPts;  // x0, y0, x1, y1...
Hover g_hlHover;

int HighlightDown(const CyImgEditHost*, CyImgEditCtx*, const CyImgToolEvent* ev) {
    g_hlPts = {ev->x, ev->y};
    return 1;
}
void HighlightMove(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent* ev) {
    g_hlHover = {true, ev->x, ev->y};
    if (ev->buttons && g_hlPts.size() >= 2) {
        if (ev->mods & CYIMG_MOD_SHIFT) {  // trait droit depuis le point de départ
            g_hlPts.resize(2);
            g_hlPts.push_back(ev->x);
            g_hlPts.push_back(ev->y);
        } else {
            const double dx = ev->x - g_hlPts[g_hlPts.size() - 2], dy = ev->y - g_hlPts.back();
            const double minStep = 1.5 * h->screen_pixel(c);
            if (dx * dx + dy * dy >= minStep * minStep) {
                g_hlPts.push_back(ev->x);
                g_hlPts.push_back(ev->y);
            }
        }
    }
    h->redraw(c);
}
void HighlightUp(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent*) {
    if (g_hlPts.size() == 2) g_hlPts.insert(g_hlPts.end(), {g_hlPts[0], g_hlPts[1]});  // simple clic : un point
    // Groupe en mode produit : les recouvrements du trait ne foncent pas, le texte reste lisible
    h->begin_group(c, h->param(c, kHlOpacity) / 100, CYIMG_BLEND_MULTIPLY);
    h->draw_polyline(c, g_hlPts.data(), uint32_t(g_hlPts.size() / 2), h->param(c, kHlWidth), Opaque(uint32_t(h->param(c, kHlColor))));
    h->end_group(c);
    g_hlPts.clear();
}
void HighlightOverlay(const CyImgEditHost* h, CyImgEditCtx* c) {
    const double w = h->param(c, kHlWidth), px = h->screen_pixel(c);
    if (g_hlPts.size() >= 2) {
        h->begin_group(c, 0.5 * h->param(c, kHlOpacity) / 100, CYIMG_BLEND_NORMAL);
        h->draw_polyline(c, g_hlPts.data(), uint32_t(g_hlPts.size() / 2), w, Opaque(uint32_t(h->param(c, kHlColor))));
        h->end_group(c);
    } else if (g_hlHover.on) {  // épaisseur du trait sous le curseur
        h->draw_ellipse(c, g_hlHover.x, g_hlHover.y, w / 2 + px, w / 2 + px, px, 0x99000000u);
        h->draw_ellipse(c, g_hlHover.x, g_hlHover.y, w / 2, w / 2, px, 0xE6FFFFFFu);
    }
}
void HighlightCancel(const CyImgEditHost*, CyImgEditCtx*) { g_hlPts.clear(); }
SizeRef g_hlRef;
void HighlightActivate(const CyImgEditHost* h, CyImgEditCtx* c, int on) {
    g_hlPts.clear();
    g_hlHover.on = false;
    if (on) AdaptSize(h, c, kHlWidth, 24, g_hlRef);
}

// --- Flou de masquage ------------------------------------------------------------------------

enum { kRedactStyle, kRedactStrength };
const CyImgParam kRedactParams[] = {
    CYIMG_CHOICE("plugin.annotate.style", L"Style", L"Pixellisation|Flou|Bloc noir", 0),
    CYIMG_SLIDER("plugin.annotate.strength", L"Force", 2, 100, 14, 1, L"px"),
};
bool g_redactDrag = false;
double g_rx0 = 0, g_ry0 = 0, g_rx1 = 0, g_ry1 = 0;

void Pixelate(uint8_t* px, uint32_t w, uint32_t h, uint32_t stride, uint32_t b) {
    for (uint32_t by = 0; by < h; by += b)
        for (uint32_t bx = 0; bx < w; bx += b) {
            const uint32_t ex = std::min(w, bx + b), ey = std::min(h, by + b);
            uint64_t sum[4] = {0, 0, 0, 0};
            for (uint32_t y = by; y < ey; ++y)
                for (uint32_t x = bx; x < ex; ++x)
                    for (int k = 0; k < 4; ++k) sum[k] += px[size_t(y) * stride + size_t(x) * 4 + k];
            const uint64_t n = uint64_t(ex - bx) * (ey - by);
            uint8_t avg[4];
            for (int k = 0; k < 4; ++k) avg[k] = uint8_t((sum[k] + n / 2) / n);
            for (uint32_t y = by; y < ey; ++y)
                for (uint32_t x = bx; x < ex; ++x)
                    for (int k = 0; k < 4; ++k) px[size_t(y) * stride + size_t(x) * 4 + k] = avg[k];
        }
}

// Flou en boîte (3 passes ≈ gaussien), bords prolongés, sur des pixels prémultipliés
void BoxBlur(uint8_t* px, uint32_t w, uint32_t h, uint32_t stride, int r) {
    std::vector<uint8_t> line(size_t(std::max(w, h)) * 4);
    auto pass = [&](uint32_t count, uint32_t len, size_t step, size_t next) {
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t* p = px + i * next;
            for (uint32_t k = 0; k < len; ++k) memcpy(&line[size_t(k) * 4], p + k * step, 4);
            int sum[4] = {0, 0, 0, 0};
            auto at = [&](int k, int c) { return int(line[size_t(std::clamp(k, 0, int(len) - 1)) * 4 + c]); };
            for (int c = 0; c < 4; ++c)
                for (int k = -r; k <= r; ++k) sum[c] += at(k, c);
            const int n = 2 * r + 1;
            for (uint32_t k = 0; k < len; ++k) {
                for (int c = 0; c < 4; ++c) {
                    p[k * step + c] = uint8_t((sum[c] + n / 2) / n);
                    sum[c] += at(int(k) + r + 1, c) - at(int(k) - r, c);
                }
            }
        }
    };
    for (int it = 0; it < 3; ++it) {
        pass(h, w, 4, stride);  // lignes
        pass(w, h, stride, 4);  // colonnes
    }
}

int RedactDown(const CyImgEditHost*, CyImgEditCtx*, const CyImgToolEvent* ev) {
    g_redactDrag = true;
    g_rx0 = g_rx1 = ev->x;
    g_ry0 = g_ry1 = ev->y;
    return 1;
}
void RedactMove(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent* ev) {
    if (!g_redactDrag) return;
    g_rx1 = ev->x;
    g_ry1 = ev->y;
    h->redraw(c);
}
void RedactUp(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent*) {
    if (!g_redactDrag) return;
    g_redactDrag = false;
    CyImgRect r{int32_t(std::floor(std::min(g_rx0, g_rx1))), int32_t(std::floor(std::min(g_ry0, g_ry1))), 0, 0};
    r.w = uint32_t(std::max(0.0, std::ceil(std::max(g_rx0, g_rx1)) - r.x));
    r.h = uint32_t(std::max(0.0, std::ceil(std::max(g_ry0, g_ry1)) - r.y));
    if (r.w < 2 || r.h < 2) return;
    uint32_t stride = 0;
    uint8_t* px = h->lock_pixels(c, &r, &stride);  // rectangle ramené au document
    if (!px) return;
    const int style = int(h->param(c, kRedactStyle));
    const uint32_t force = uint32_t(std::max(2.0, h->param(c, kRedactStrength)));
    if (style == 0) {
        Pixelate(px, r.w, r.h, stride, force);
    } else if (style == 1) {
        BoxBlur(px, r.w, r.h, stride, int(force / 2 + 1));
    } else {
        for (uint32_t y = 0; y < r.h; ++y)
            for (uint32_t x = 0; x < r.w; ++x) {
                uint8_t* p = px + size_t(y) * stride + size_t(x) * 4;
                p[0] = p[1] = p[2] = 0;
                p[3] = 255;
            }
    }
    h->unlock_pixels(c, 1);
}
void RedactOverlay(const CyImgEditHost* h, CyImgEditCtx* c) {
    if (!g_redactDrag) return;
    const double px = h->screen_pixel(c);
    const double x = std::min(g_rx0, g_rx1), y = std::min(g_ry0, g_ry1), w = std::fabs(g_rx1 - g_rx0), hh = std::fabs(g_ry1 - g_ry0);
    h->draw_rect(c, x, y, w, hh, 0, 0, 0x40FFFFFFu);
    h->draw_rect(c, x, y, w, hh, 0, 3 * px, 0xB0000000u);
    h->draw_rect(c, x, y, w, hh, 0, 1.2 * px, 0xFFFFFFFFu);
}
void RedactCancel(const CyImgEditHost*, CyImgEditCtx*) { g_redactDrag = false; }
SizeRef g_redactRef;
void RedactActivate(const CyImgEditHost* h, CyImgEditCtx* c, int on) {
    if (on) AdaptSize(h, c, kRedactStrength, 14, g_redactRef);
}

// --- Bulle de texte --------------------------------------------------------------------------

enum { kBubbleSize, kBubbleShape };
const CyImgParam kBubbleParams[] = {
    CYIMG_SLIDER("plugin.annotate.text_size", L"Taille du texte", 8, 300, 28, 1, L"px"),
    CYIMG_CHOICE("plugin.annotate.shape", L"Forme", L"Arrondie|Rectangle", 0),
};
bool g_bubbleDrag = false;
double g_bx0 = 0, g_by0 = 0, g_bx1 = 0, g_by1 = 0;

// Bulle centrée sur (cx, cy), pointe vers (tx, ty) si tail
void DrawBubble(const CyImgEditHost* h, CyImgEditCtx* c, const wchar_t* text, double cx, double cy, bool tail, double tx, double ty) {
    const double size = h->param(c, kBubbleSize);
    double tw = 0, th = 0;
    h->measure_text(c, text, size, 0, 0, &tw, &th);
    const double pad = size * 0.5, bw = tw + 2 * pad, bh = th + 1.4 * pad;
    const uint32_t rgb = h->primary_color(c);
    if (tail) {  // triangle partant du centre : sa base est cachée par la bulle
        const double dx = cx - tx, dy = cy - ty, len = std::max(1e-6, std::hypot(dx, dy));
        const double base = std::min(bw, bh) * 0.45, nx = -dy / len * base / 2, ny = dx / len * base / 2;
        const double tri[6] = {tx, ty, cx + nx, cy + ny, cx - nx, cy - ny};
        h->fill_polygon(c, tri, 3, Opaque(rgb));
    }
    const double radius = h->param(c, kBubbleShape) == 0 ? std::min(bh / 2, size * 0.7) : 0;
    h->draw_rect(c, cx - bw / 2, cy - bh / 2, bw, bh, radius, 0, Opaque(rgb));
    h->draw_text(c, cx - tw / 2, cy - th / 2, 0, text, size, Readable(rgb), 0);
}

int BubbleDown(const CyImgEditHost*, CyImgEditCtx*, const CyImgToolEvent* ev) {
    g_bubbleDrag = true;
    g_bx0 = g_bx1 = ev->x;
    g_by0 = g_by1 = ev->y;
    return 1;
}
void BubbleMove(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent* ev) {
    if (!g_bubbleDrag) return;
    g_bx1 = ev->x;
    g_by1 = ev->y;
    h->redraw(c);
}
void BubbleUp(const CyImgEditHost* h, CyImgEditCtx* c, const CyImgToolEvent*) {
    if (!g_bubbleDrag) return;
    g_bubbleDrag = false;
    h->redraw(c);
    wchar_t buf[2048] = {};
    if (!h->input_text(c, Tr(h, c, "plugin.annotate.bubble_title", L"Texte de la bulle"), L"", buf, 2048)) return;
    std::wstring text;
    for (const wchar_t* p = buf; *p; ++p)
        if (*p != L'\r') text += *p;  // retours à la ligne : \n seul
    while (!text.empty() && (text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    if (text.empty()) return;
    const bool tail = std::hypot(g_bx1 - g_bx0, g_by1 - g_by0) > 6 * h->screen_pixel(c);
    h->begin_edit(c);
    DrawBubble(h, c, text.c_str(), g_bx1, g_by1, tail, g_bx0, g_by0);
}
void BubbleOverlay(const CyImgEditHost* h, CyImgEditCtx* c) {
    if (!g_bubbleDrag) return;
    h->begin_group(c, 0.6, CYIMG_BLEND_NORMAL);
    DrawBubble(h, c, Tr(h, c, "plugin.annotate.bubble_sample", L"Texte"), g_bx1, g_by1,
               std::hypot(g_bx1 - g_bx0, g_by1 - g_by0) > 6 * h->screen_pixel(c), g_bx0, g_by0);
    h->end_group(c);
}
void BubbleCancel(const CyImgEditHost*, CyImgEditCtx*) { g_bubbleDrag = false; }
SizeRef g_bubbleRef;
void BubbleActivate(const CyImgEditHost* h, CyImgEditCtx* c, int on) {
    if (on) AdaptSize(h, c, kBubbleSize, 28, g_bubbleRef);
}

// --- Description du plugin -------------------------------------------------------------------

#define COUNT(a) uint32_t(sizeof(a) / sizeof(a[0]))

const CyImgTool kBadge = {
    sizeof(CyImgTool), "badge", "plugin.annotate.badge", L"Pastille numérotée", "plugin.annotate.badge_hint",
    L"Clic : pastille numérotée (couleur principale ; clic droit : secondaire). Le numéro s'incrémente.",
    0, kBadgeIconPng, uint32_t(sizeof(kBadgeIconPng)), 'Q', kBadgeParams, COUNT(kBadgeParams),
    BadgeDown, BadgeMove, nullptr, BadgeOverlay, BadgeActivate, nullptr,
};
const CyImgTool kHighlight = {
    sizeof(CyImgTool), "highlighter", "plugin.annotate.highlighter", L"Surligneur", "plugin.annotate.highlighter_hint",
    L"Glisser pour surligner (mode produit) ; Maj : trait droit.",
    0xE7E6, nullptr, 0, 'U', kHighlightParams, COUNT(kHighlightParams),
    HighlightDown, HighlightMove, HighlightUp, HighlightOverlay, HighlightActivate, HighlightCancel,
};
const CyImgTool kRedact = {
    sizeof(CyImgTool), "redact", "plugin.annotate.redact", L"Flou de masquage", "plugin.annotate.redact_hint",
    L"Tracer un rectangle pour masquer une information (pixellisation, flou ou bloc noir).",
    0xE72E, nullptr, 0, 'Y', kRedactParams, COUNT(kRedactParams),
    RedactDown, RedactMove, RedactUp, RedactOverlay, RedactActivate, RedactCancel,
};
const CyImgTool kBubble = {
    sizeof(CyImgTool), "bubble", "plugin.annotate.bubble", L"Bulle de texte", "plugin.annotate.bubble_hint",
    L"Glisser de la pointe vers l'emplacement de la bulle (ou cliquer), puis saisir le texte.",
    0xE8BD, nullptr, 0, 'W', kBubbleParams, COUNT(kBubbleParams),
    BubbleDown, BubbleMove, BubbleUp, BubbleOverlay, BubbleActivate, BubbleCancel,
};
const CyImgTool* const kTools[] = {&kBadge, &kHighlight, &kRedact, &kBubble};

const CyImgEditPlugin kPlugin = {
    CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "annotate", "plugin.annotate.name", L"Annotation", L"1.0",
    nullptr, 0, kTools, COUNT(kTools), nullptr, 0, nullptr, 0,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) { return &kPlugin; }
