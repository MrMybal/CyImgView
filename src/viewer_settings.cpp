// Panneau « Paramètres » (réglages persistants) et mode « une seule fenêtre ».

#include "version.h"
#include "viewer.h"
#include "viewer_util.h"

#include <shlobj.h>

using namespace ui;

namespace {

enum SettingId { kSingleWindow = 1, kStrip, kAutoUpdate, kCheckNow, kSlideShuffle, kSlideLess, kSlideMore };

}  // namespace

std::wstring SettingsIniPath() {
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        std::wstring dir = std::wstring(appData) + L"\\CyImgView";
        CreateDirectoryW(dir.c_str(), nullptr);
        path = dir + L"\\settings.ini";
    }
    CoTaskMemFree(appData);
    return path;
}

bool ForwardToRunningInstance(const std::wstring& relPath) {
    wchar_t full[MAX_PATH * 4];  // l'autre processus n'a pas le même dossier courant
    DWORD n = GetFullPathNameW(relPath.c_str(), ARRAYSIZE(full), full, nullptr);
    const std::wstring path = (n && n < ARRAYSIZE(full)) ? std::wstring(full) : relPath;
    // Le mutex sert à détecter un lancement simultané (plusieurs fichiers ouverts d'un coup depuis
    // l'Explorateur) : on attend alors que la première fenêtre existe.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\CyImgView.SingleWindow");
    const bool other = GetLastError() == ERROR_ALREADY_EXISTS;
    (void)mutex;  // gardé ouvert jusqu'à la fin du processus
    HWND target = FindWindowW(L"CyImgViewWindow", nullptr);
    for (int i = 0; !target && other && i < 40; ++i) {
        Sleep(100);
        target = FindWindowW(L"CyImgViewWindow", nullptr);
    }
    if (!target) return false;
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataOpen;
    cds.cbData = DWORD((path.size() + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(path.c_str());
    AllowSetForegroundWindow(ASFW_ANY);  // la fenêtre existante pourra passer au premier plan
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 3000, &result) &&
           result == 1;
}

void Viewer::SaveSetting(const wchar_t* section, const wchar_t* key, int value) {
    if (!m_iniPath.empty()) WritePrivateProfileStringW(section, key, std::to_wstring(value).c_str(), m_iniPath.c_str());
}

void Viewer::OnSettingClicked(int id) {
    switch (id) {
    case kSingleWindow:
        m_singleInstance = !m_singleInstance;
        SaveSetting(L"settings", L"single_instance", m_singleInstance);
        break;
    case kStrip:
        m_showStrip = !m_showStrip;
        SaveSetting(L"view", L"strip", m_showStrip);
        m_thumbReqLo = -1;
        RequestThumbs();
        break;
    case kAutoUpdate:
        m_autoUpdate = !m_autoUpdate;
        SaveSetting(L"update", L"auto", m_autoUpdate);
        break;
    case kSlideShuffle:
        m_slideShuffle = !m_slideShuffle;
        SaveSetting(L"slideshow", L"shuffle", m_slideShuffle);
        break;
    case kSlideLess: ChangeSlideDelay(-1); break;
    case kSlideMore: ChangeSlideDelay(1); break;
    case kCheckNow:
        m_showSettings = false;
        if (m_updateAvailable) ShowUpdateDialog();
        else StartUpdateCheck(true);
        break;
    default: break;
    }
    Invalidate();
}

void Viewer::DrawSettings(float cw, float ch) {
    const float s = m_scale;
    m_settingRows.clear();
    struct Row {
        int id;
        const wchar_t* title;
        const wchar_t* desc;
        bool value;
    };
    const Row rows[] = {
        {kSingleWindow, L"Ouvrir les fichiers dans la fenêtre existante",
         L"Sinon, chaque fichier ouvert crée une nouvelle fenêtre.", m_singleInstance},
        {kStrip, L"Afficher la pellicule", L"Vignettes des images du dossier en bas de l'écran (touche T).", m_showStrip},
        {kAutoUpdate, L"Rechercher les mises à jour automatiquement",
         L"Au plus une fois par jour, sur les releases GitHub de CyImgView.", m_autoUpdate},
        {kSlideShuffle, L"Diaporama : ordre aléatoire", L"Sinon, les images défilent dans l'ordre du dossier (touche D).",
         m_slideShuffle},
    };
    const float w = std::min(520 * s, cw - 24 * s), pad = 18 * s, rowH = 62 * s;
    const float h = pad * 2 + 34 * s + rowH * (ARRAYSIZE(rows) + 1) + 52 * s + 26 * s;
    m_settingsPanel = D2D1::RectF(cw - w - 12 * s, 58 * s, cw - 12 * s, std::min(ch - 12 * s, 58 * s + h));
    const D2D1_RECT_F& p = m_settingsPanel;
    FillRound(p, 8 * s, kPanel, 0.97f);
    SetColor(kWhite, 0.08f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(p, 8 * s, 8 * s), m_brush.Get(), 1);
    m_rt->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_ALIASED);
    Text(L"Paramètres", m_tfTitle.Get(), D2D1::RectF(p.left + pad, p.top + pad - 4 * s, p.right - pad, p.top + pad + 22 * s), kWhite, 1);

    float y = p.top + pad + 34 * s;
    for (const Row& r : rows) {
        D2D1_RECT_F rc = D2D1::RectF(p.left + 6 * s, y, p.right - 6 * s, y + rowH - 4 * s);
        if (m_mouseInside && Contains(rc, m_mouse)) FillRound(rc, 6 * s, kWhite, 0.06f);
        m_settingRows.push_back({r.id, rc});
        // Interrupteur
        const float tw = 40 * s, th = 22 * s;
        D2D1_RECT_F sw = D2D1::RectF(rc.right - tw - 12 * s, rc.top + 10 * s, rc.right - 12 * s, rc.top + 10 * s + th);
        FillRound(sw, th / 2, r.value ? kAccent : 0x4A4C52, 1);
        float kx = r.value ? sw.right - th / 2 : sw.left + th / 2;
        SetColor(kWhite, 1);
        m_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, (sw.top + sw.bottom) / 2), th / 2 - 3 * s, th / 2 - 3 * s), m_brush.Get());
        Text(r.title, m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 6 * s, sw.left - 10 * s, rc.top + 28 * s), kWhite, 1);
        Text(r.desc, m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 28 * s, sw.left - 10 * s, rc.top + 50 * s), kGrey, 0.9f);
        y += rowH;
    }
    // Diaporama : durée par image, boutons − et +
    {
        D2D1_RECT_F rc = D2D1::RectF(p.left + 6 * s, y, p.right - 6 * s, y + rowH - 4 * s);
        const float bs = 28 * s, vw = 56 * s;
        D2D1_RECT_F plus = D2D1::RectF(rc.right - 12 * s - bs, rc.top + 7 * s, rc.right - 12 * s, rc.top + 7 * s + bs);
        D2D1_RECT_F val = D2D1::RectF(plus.left - vw, plus.top, plus.left, plus.bottom);
        D2D1_RECT_F minus = D2D1::RectF(val.left - bs, plus.top, val.left, plus.bottom);
        for (auto [id, r] : {std::pair{kSlideLess, minus}, std::pair{kSlideMore, plus}}) {
            const bool hov = m_mouseInside && Contains(r, m_mouse);
            FillRound(r, bs / 2, kWhite, hov ? 0.18f : 0.09f);
            Text(id == kSlideLess ? L"\uE738" : L"\uE710", m_tfIcon.Get(), r, kWhite, 1);
            m_settingRows.push_back({id, r});
        }
        Text(SlideDelayLabel(), m_tfLabel.Get(), val, kAccent, 1);
        Text(L"Diaporama : durée par image", m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 6 * s, minus.left - 10 * s, rc.top + 28 * s), kWhite, 1);
        Text(L"Aussi réglable pendant le diaporama avec ↑ et ↓.", m_tfSmall.Get(),
             D2D1::RectF(rc.left + 12 * s, rc.top + 28 * s, minus.left - 10 * s, rc.top + 50 * s), kGrey, 0.9f);
        y += rowH;
    }
    // Bouton « Rechercher une mise à jour »
    D2D1_RECT_F btn = D2D1::RectF(p.left + pad, y + 8 * s, p.left + pad + 260 * s, y + 42 * s);
    bool hov = m_mouseInside && Contains(btn, m_mouse);
    FillRound(btn, 6 * s, kWhite, hov ? 0.16f : 0.09f);
    IconLabel(btn, L"", m_updateAvailable ? L"Installer la mise à jour " + m_release.version : std::wstring(L"Rechercher une mise à jour"),
              kWhite, 1);
    m_settingRows.push_back({kCheckNow, btn});
    Text(L"CyImgView " CYIMG_VERSION_WSTR L"   ·   réglages : %APPDATA%\\CyImgView\\settings.ini", m_tfSmall.Get(),
         D2D1::RectF(p.left + pad, btn.bottom + 6 * s, p.right - pad, btn.bottom + 30 * s), kGrey, 0.8f);
    m_rt->PopAxisAlignedClip();
}
