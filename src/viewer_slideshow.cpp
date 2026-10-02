// Diaporama : plein écran, interface masquée, passage automatique d'une image à l'autre avec un
// fondu enchaîné (l'image entrante arrive aussi avec un léger zoom). Durée par image réglable.

#include "viewer.h"
#include "viewer_util.h"

#include <algorithm>
#include <cmath>

using namespace ui;

namespace {

constexpr double kFadeSeconds = 0.9;
constexpr int kDelays[] = {1, 2, 3, 4, 5, 7, 10, 15, 20, 30, 60};

double Ease(double t) { return t * t * (3 - 2 * t); }  // départ et arrivée en douceur

}  // namespace

std::wstring Viewer::SlideDelayLabel() const { return std::to_wstring(m_slideDelay) + L" s"; }

void Viewer::ChangeSlideDelay(int dir) {
    int i = 0;
    while (i + 1 < int(ARRAYSIZE(kDelays)) && kDelays[i] < m_slideDelay) ++i;
    i = std::clamp(i + dir, 0, int(ARRAYSIZE(kDelays)) - 1);
    m_slideDelay = kDelays[i];
    SaveSetting(L"slideshow", L"delay", m_slideDelay);
    if (m_slideshow && !m_slidePaused) SetTimer(m_hwnd, kTimerSlide, UINT(m_slideDelay * 1000), nullptr);
    ShowToast(L"Diaporama : " + SlideDelayLabel() + L" par image");
    Invalidate();
}

void Viewer::StartSlideshow() {
    if (m_slideshow) return;
    if (m_files.size() < 2) {
        ShowToast(L"Il faut au moins deux images dans le dossier");
        return;
    }
    if (m_edit.active && !EndEdit(true)) return;
    m_slideshow = true;
    m_slidePaused = false;
    m_slideWasFull = m_fullscreen;
    m_showInfo = m_showHelp = m_showSettings = m_showChannels = false;
    if (!m_fullscreen) ToggleFullscreen();
    m_lastDir = 1;
    m_lastActivity = 0;  // interface masquée tout de suite
    m_hud = 0;
    m_fadeTiles.clear();
    ApplyFit(false);
    RequestLoads();
    SetTimer(m_hwnd, kTimerSlide, UINT(m_slideDelay * 1000), nullptr);
    ShowToast(L"Diaporama · " + SlideDelayLabel() + L" par image · Échap pour quitter");
    Invalidate();
}

void Viewer::StopSlideshow() {
    if (!m_slideshow) return;
    m_slideshow = false;
    KillTimer(m_hwnd, kTimerSlide);
    m_fadeTiles.clear();
    if (!m_slideWasFull && m_fullscreen) ToggleFullscreen();
    ApplyFit(false);
    Poke();
    Invalidate();
}

void Viewer::ToggleSlidePause() {
    m_slidePaused = !m_slidePaused;
    if (m_slidePaused) KillTimer(m_hwnd, kTimerSlide);
    else SetTimer(m_hwnd, kTimerSlide, UINT(m_slideDelay * 1000), nullptr);
    ShowToast(m_slidePaused ? L"Diaporama en pause" : L"Lecture du diaporama");
    Invalidate();
}

int Viewer::SlideTarget(int delta) const {
    const int n = int(m_files.size());
    if (n < 2) return m_index;
    if (m_slideShuffle && delta > 0) {  // ordre aléatoire : n'importe quelle autre image
        int t = int(GetTickCount64() * 2654435761ull % uint64_t(n - 1));
        return t >= m_index ? t + 1 : t;
    }
    return ((m_index + delta) % n + n) % n;  // boucle au début / à la fin
}

// Image suivante (automatique) ou précédente/suivante (manuelle) avec fondu enchaîné.
void Viewer::SlideGo(int delta, bool automatic) {
    if (m_files.size() < 2) return;
    const int target = SlideTarget(delta);
    if (automatic) {
        // On attend que l'image suivante soit décodée : un fondu vers un écran vide serait laid.
        const std::wstring key = Key(m_files[target].path);
        if (!m_fullCache.count(key) && !m_failed.count(key)) {
            m_lastDir = 1;
            if (!m_slideShuffle) RequestLoads();
            SetTimer(m_hwnd, kTimerSlide, 150, nullptr);
            return;
        }
    }
    // L'image actuelle devient l'image sortante du fondu (les bitmaps GPU restent en vie ici).
    m_fadeTiles.clear();
    if (const FileEntry* cur = Current(); cur && m_cur) {
        if (GpuImage* g = GetGpuImage(m_cur, Key(cur->path))) {
            m_fadeTiles = g->tiles;
            m_fadeW = float(m_cur->width);
            m_fadeH = float(m_cur->height);
            FlipRot(m_fadeFlip, m_fadeRot);
        }
    }
    m_fadeStart = NowSeconds();
    m_lastDir = delta >= 0 ? 1 : -1;
    GoTo(target);
    if (!m_slidePaused) SetTimer(m_hwnd, kTimerSlide, UINT(m_slideDelay * 1000), nullptr);
}

void Viewer::OnSlideTimer() {
    if (!m_slideshow || m_slidePaused) {
        KillTimer(m_hwnd, kTimerSlide);
        return;
    }
    if (m_occluded) return;  // fenêtre cachée : on reprendra au retour
    SlideGo(1, true);
}

bool Viewer::SlideKey(WPARAM vk) {
    switch (vk) {
    case VK_ESCAPE: case 'D': StopSlideshow(); return true;
    case VK_SPACE: ToggleSlidePause(); return true;
    case VK_RIGHT: case VK_NEXT: SlideGo(1, false); return true;
    case VK_LEFT: case VK_PRIOR: case VK_BACK: SlideGo(-1, false); return true;
    case VK_UP: ChangeSlideDelay(1); return true;
    case VK_DOWN: ChangeSlideDelay(-1); return true;
    case 'F': case VK_F11: case VK_RETURN: StopSlideshow(); return true;
    default: return false;
    }
}

// Dessine une image ajustée à l'écran, centrée.
void Viewer::DrawSlideImage(const std::vector<Tile>& tiles, float W, float H, bool flip, int rot, float cw, float ch,
                            float scale, float opacity, bool moving) {
    const float dw = (rot & 1) ? H : W, dh = (rot & 1) ? W : H;
    float fit = std::min(cw / dw, ch / dh);
    fit = std::min(fit, 4.0f);  // les toutes petites images ne sont pas agrandies à l'infini
    const float z = fit * scale;
    D2D1_INTERPOLATION_MODE mode = moving ? D2D1_INTERPOLATION_MODE_LINEAR
                                   : z < 1.0f ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                                              : D2D1_INTERPOLATION_MODE_CUBIC;
    m_dc->SetTransform(ImageTransform(W, H, flip, rot, z, std::round(cw / 2), std::round(ch / 2)));
    for (const Tile& t : tiles) m_dc->DrawBitmap(t.bmp.Get(), t.dst, opacity, mode, nullptr, nullptr);
    m_dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

// Retourne true tant que le fondu est en cours (il faut redessiner).
bool Viewer::DrawSlideshow(float cw, float ch) {
    SetColor(0x000000, 1);
    m_rt->FillRectangle(D2D1::RectF(0, 0, cw, ch), m_brush.Get());
    const double t = std::clamp((NowSeconds() - m_fadeStart) / kFadeSeconds, 0.0, 1.0);
    const bool fading = t < 1.0;
    const float e = float(Ease(t));
    if (!fading) m_fadeTiles.clear();

    if (fading && !m_fadeTiles.empty())
        DrawSlideImage(m_fadeTiles, m_fadeW, m_fadeH, m_fadeFlip, m_fadeRot, cw, ch, 1.0f + 0.02f * e, 1.0f - e, true);

    const float inScale = fading ? 1.035f - 0.035f * e : 1.0f;
    const float inAlpha = fading ? e : 1.0f;
    bool flip;
    int rot;
    FlipRot(flip, rot);
    if (const FileEntry* cur = Current()) {
        if (m_cur) {
            if (GpuImage* g = GetGpuImage(m_cur, Key(cur->path))) {
                SyncAnimation();
                SyncGpuFrame(g);
                DrawSlideImage(g->tiles, float(m_cur->width), float(m_cur->height), flip, rot, cw, ch, inScale, inAlpha, fading);
            }
        } else if (m_placeholder) {  // navigation manuelle vers une image pas encore décodée
            if (ID2D1Bitmap* b = GetGpuThumb(Key(cur->path))) {
                const float W = float(m_placeholder->fullWidth ? m_placeholder->fullWidth : m_placeholder->width);
                const float H = float(m_placeholder->fullHeight ? m_placeholder->fullHeight : m_placeholder->height);
                std::vector<Tile> one{{b, D2D1::RectF(0, 0, W, H)}};
                DrawSlideImage(one, W, H, flip, rot, cw, ch, inScale, inAlpha, true);
            }
        }
    }
    return fading;
}

// Barre de contrôle discrète en bas, visible quand la souris bouge.
void Viewer::DrawSlideBar(float cw, float ch, float a) {
    const float s = m_scale;
    const FileEntry* cur = Current();
    const float bs = 40 * s, h = 52 * s, w = 470 * s;
    D2D1_RECT_F bar = D2D1::RectF(cw / 2 - w / 2, ch - h - 28 * s, cw / 2 + w / 2, ch - 28 * s);
    FillRound(bar, h / 2, 0x000000, 0.62f * a);
    SetColor(kWhite, 0.10f * a);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(bar, h / 2, h / 2), m_brush.Get(), 1);

    float x = bar.left + 14 * s;
    const float y = bar.top + (h - bs) / 2;
    auto button = [&](Btn id, const wchar_t* glyph, const wchar_t* tip, bool on = false) {
        D2D1_RECT_F r = D2D1::RectF(x, y, x + bs, y + bs);
        m_buttons.push_back({id, r, glyph, tip});
        const bool hov = m_mouseInside && Contains(r, m_mouse);
        if (hov) FillRound(r, bs / 2, kWhite, 0.16f * a);
        Text(glyph, m_tfIcon.Get(), r, on ? kAccent : kWhite, 0.95f * a);
        x += bs + 2 * s;
    };
    button(Btn::SlidePrev, L"", L"Précédente (←)");
    button(Btn::SlidePlay, m_slidePaused ? L"" : L"", m_slidePaused ? L"Lecture (Espace)" : L"Pause (Espace)");
    button(Btn::SlideNext, L"", L"Suivante (→)");
    x += 8 * s;
    button(Btn::SlideLess, L"", L"Moins de temps par image (↓)");
    Text(SlideDelayLabel(), m_tfLabel.Get(), D2D1::RectF(x - 2 * s, y, x + 48 * s, y + bs), kWhite, a);
    x += 48 * s;
    button(Btn::SlideMore, L"", L"Plus de temps par image (↑)");
    button(Btn::SlideShuffle, L"", L"Ordre aléatoire", m_slideShuffle);
    x += 8 * s;
    if (cur) {
        std::wstring pos = std::to_wstring(m_index + 1) + L" / " + std::to_wstring(m_files.size());
        Text(pos, m_tfSmall.Get(), D2D1::RectF(x, y, bar.right - bs - 20 * s, y + bs), kGrey, a);
    }
    x = bar.right - bs - 14 * s;
    button(Btn::SlideExit, L"", L"Quitter le diaporama (Échap)");

    // Nom du fichier au-dessus de la barre
    if (cur) Text(cur->name, m_tfCenter.Get(), D2D1::RectF(0, bar.top - 30 * s, cw, bar.top - 6 * s), kWhite, 0.85f * a);

    // Info-bulle
    for (auto& b : m_buttons) {
        if (!m_mouseInside || !Contains(b.rc, m_mouse)) continue;
        std::wstring tip = b.tip;
        D2D1_RECT_F r = D2D1::RectF(b.rc.left - 90 * s, bar.top - 66 * s, b.rc.right + 90 * s, bar.top - 38 * s);
        ComPtr<IDWriteTextLayout> lay;
        if (SUCCEEDED(m_dwrite->CreateTextLayout(tip.c_str(), UINT32(tip.size()), m_tfSmall.Get(), 400 * s, 30 * s, &lay))) {
            DWRITE_TEXT_METRICS tm;
            lay->GetMetrics(&tm);
            const float tw = tm.width + 20 * s, c = (b.rc.left + b.rc.right) / 2;
            r.left = c - tw / 2;
            r.right = c + tw / 2;
        }
        FillRound(r, 4 * s, kPanel, 0.95f * a);
        Text(tip, m_tfCenter.Get(), r, kWhite, a);
    }
}
