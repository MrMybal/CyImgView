// Panneau « Paramètres » (réglages persistants) et mode « une seule fenêtre ».

#include "version.h"
#include "viewer.h"
#include "viewer_util.h"
#include "i18n.h"

#include <shlobj.h>

#include <algorithm>

using namespace ui;

namespace {

enum SettingId { kSingleWindow = 1, kStrip, kAutoUpdate, kCheckNow, kSlideShuffle, kSlideLess, kSlideMore, kFilterPrev, kFilterNext, kLangPrev, kLangNext };

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
    case kLangPrev:
    case kLangNext: {
        // Liste : Automatique, puis les langues disponibles
        auto langs = i18n::Languages();
        wchar_t cur[32] = {};
        GetPrivateProfileStringW(L"settings", L"language", L"", cur, ARRAYSIZE(cur), m_iniPath.c_str());
        int idx = 0;
        for (size_t i = 0; i < langs.size(); ++i)
            if (langs[i].code == cur) idx = int(i) + 1;
        const int n = int(langs.size()) + 1;
        idx = (idx + (id == kLangNext ? 1 : n - 1)) % n;
        const std::wstring code = idx == 0 ? std::wstring() : langs[size_t(idx - 1)].code;
        if (!m_iniPath.empty()) WritePrivateProfileStringW(L"settings", L"language", code.c_str(), m_iniPath.c_str());
        i18n::SetLanguage(code);
        UpdateTitle();
        break;
    }
    case kFilterPrev: SetFilter((m_filter + 5) % 6); break;
    case kFilterNext: SetFilter((m_filter + 1) % 6); break;
    case kSlideLess: ChangeSlideDelay(-1); break;
    case kSlideMore: ChangeSlideDelay(1); break;
    case kCheckNow:
        m_showSettings = false;
        if (m_updateAvailable) ShowUpdateDialog();
        else StartUpdateCheck(true);
        break;
    default:
        if (id >= 1000) OnOpenWithSetting(id);  // section « Ouvrir avec » (viewer_openwith.cpp)
        break;
    }
    Invalidate();
}

void Viewer::DrawSettings(float cw, float ch) {
    const float s = m_scale;
    m_settingRows.clear();
    struct Row {
        int id;
        const char* title;  // clés de traduction
        const char* desc;
        bool value;
    };
    const Row rows[] = {
        {kSingleWindow, "settings.single_window", "settings.single_window.desc", m_singleInstance},
        {kStrip, "settings.strip", "settings.strip.desc", m_showStrip},
        {kAutoUpdate, "settings.auto_update", "settings.auto_update.desc", m_autoUpdate},
        {kSlideShuffle, "settings.shuffle", "settings.shuffle.desc", m_slideShuffle},
    };
    const float w = std::min(580 * s, cw - 24 * s), pad = 18 * s, rowH = 62 * s;
    const float owH = OpenWithSettingsHeight();
    const float h = pad * 2 + 34 * s + rowH * (ARRAYSIZE(rows) + 3) + owH + 52 * s + 26 * s;
    m_settingsPanel = D2D1::RectF(cw - w - 12 * s, 58 * s, cw - 12 * s, std::min(ch - 12 * s, 58 * s + h));
    const D2D1_RECT_F& p = m_settingsPanel;
    // Défilement (molette) quand le contenu dépasse la fenêtre
    const float maxScroll = std::max(0.0f, h - (p.bottom - p.top));
    if (m_settingsScrollToOpenWith) {  // « Configurer… » du menu Ouvrir avec : section visible
        m_settingsScroll = pad + 34 * s + rowH * (ARRAYSIZE(rows) + 3) - 8 * s;
        m_settingsScrollToOpenWith = false;
    }
    m_settingsScroll = std::clamp(m_settingsScroll, 0.0f, maxScroll);
    FillRound(p, 8 * s, kPanel, 0.97f);
    SetColor(kWhite, 0.08f);
    m_rt->DrawRoundedRectangle(D2D1::RoundedRect(p, 8 * s, 8 * s), m_brush.Get(), 1);
    m_rt->PushAxisAlignedClip(p, D2D1_ANTIALIAS_MODE_ALIASED);
    const float top = p.top - m_settingsScroll;
    Text(T("settings.title"), m_tfTitle.Get(), D2D1::RectF(p.left + pad, top + pad - 4 * s, p.right - pad, top + pad + 22 * s), kWhite, 1);
    if (maxScroll > 0) {  // barre de défilement
        const float trackH = p.bottom - p.top - 16 * s, thumbH = std::max(30 * s, trackH * (p.bottom - p.top) / h);
        const float ty = p.top + 8 * s + (trackH - thumbH) * m_settingsScroll / maxScroll;
        FillRound(D2D1::RectF(p.right - 6 * s, ty, p.right - 3 * s, ty + thumbH), 1.5f * s, kWhite, 0.3f);
    }

    float y = top + pad + 34 * s;
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
        Text(T(r.title), m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 6 * s, sw.left - 10 * s, rc.top + 28 * s), kWhite, 1);
        Text(T(r.desc), m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 28 * s, sw.left - 10 * s, rc.top + 50 * s), kGrey, 0.9f);
        y += rowH;
    }
    // Langue de l'interface : sélecteur ‹ valeur ›
    {
        wchar_t cur[32] = {};
        GetPrivateProfileStringW(L"settings", L"language", L"", cur, ARRAYSIZE(cur), m_iniPath.c_str());
        std::wstring label;
        for (auto& l : i18n::Languages()) {
            if (l.code == cur) label = l.name;
            if (!cur[0] && l.code == i18n::CurrentCode()) label = Tf("settings.language.auto", l.name.c_str());
        }
        D2D1_RECT_F rc = D2D1::RectF(p.left + 6 * s, y, p.right - 6 * s, y + rowH - 4 * s);
        const float bs = 28 * s, vw = 170 * s;
        D2D1_RECT_F next = D2D1::RectF(rc.right - 12 * s - bs, rc.top + 7 * s, rc.right - 12 * s, rc.top + 7 * s + bs);
        D2D1_RECT_F val = D2D1::RectF(next.left - vw, next.top, next.left, next.bottom);
        D2D1_RECT_F prev = D2D1::RectF(val.left - bs, next.top, val.left, next.bottom);
        for (auto [id, r] : {std::pair{kLangPrev, prev}, std::pair{kLangNext, next}}) {
            const bool hov = m_mouseInside && Contains(r, m_mouse);
            FillRound(r, bs / 2, kWhite, hov ? 0.18f : 0.09f);
            Text(id == kLangPrev ? L"" : L"", m_tfIcon.Get(), r, kWhite, 1);
            m_settingRows.push_back({id, r});
        }
        Text(label, m_tfLabel.Get(), val, kAccent, 1);
        Text(T("settings.language"), m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 6 * s, prev.left - 10 * s, rc.top + 28 * s), kWhite, 1);
        Text(T("settings.language.desc"), m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 28 * s, prev.left - 10 * s, rc.top + 50 * s), kGrey, 0.9f);
        y += rowH;
    }
    // Filtrage de l'image : sélecteur ‹ valeur ›
    {
        static const char* desc[] = {  // clés : la langue peut changer en cours d'exécution
            "settings.filter.desc.auto",  "settings.filter.desc.raw",      "settings.filter.desc.linear",
            "settings.filter.desc.cubic", "settings.filter.desc.cubic_hq", "settings.filter.desc.anisotropic",
        };
        D2D1_RECT_F rc = D2D1::RectF(p.left + 6 * s, y, p.right - 6 * s, y + rowH - 4 * s);
        const float bs = 28 * s, vw = 116 * s;
        D2D1_RECT_F next = D2D1::RectF(rc.right - 12 * s - bs, rc.top + 7 * s, rc.right - 12 * s, rc.top + 7 * s + bs);
        D2D1_RECT_F val = D2D1::RectF(next.left - vw, next.top, next.left, next.bottom);
        D2D1_RECT_F prev = D2D1::RectF(val.left - bs, next.top, val.left, next.bottom);
        for (auto [id, r] : {std::pair{kFilterPrev, prev}, std::pair{kFilterNext, next}}) {
            const bool hov = m_mouseInside && Contains(r, m_mouse);
            FillRound(r, bs / 2, kWhite, hov ? 0.18f : 0.09f);
            Text(id == kFilterPrev ? L"\uE76B" : L"\uE76C", m_tfIcon.Get(), r, kWhite, 1);
            m_settingRows.push_back({id, r});
        }
        Text(FilterName(m_filter), m_tfLabel.Get(), val, kAccent, 1);
        Text(T("settings.filter"), m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 6 * s, prev.left - 10 * s, rc.top + 28 * s), kWhite, 1);
        Text(T(desc[m_filter]), m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 28 * s, prev.left - 10 * s, rc.top + 50 * s), kGrey, 0.9f);
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
        Text(T("settings.slide_delay"), m_tfSmall.Get(), D2D1::RectF(rc.left + 12 * s, rc.top + 6 * s, minus.left - 10 * s, rc.top + 28 * s), kWhite, 1);
        Text(T("settings.slide_delay.desc"), m_tfSmall.Get(),
             D2D1::RectF(rc.left + 12 * s, rc.top + 28 * s, minus.left - 10 * s, rc.top + 50 * s), kGrey, 0.9f);
        y += rowH;
    }
    // Ouvrir avec — applications (viewer_openwith.cpp)
    y += DrawOpenWithSettings(p.left + 6 * s, p.right - 6 * s, y);
    // Bouton « Rechercher une mise à jour »
    D2D1_RECT_F btn = D2D1::RectF(p.left + pad, y + 8 * s, p.left + pad + 260 * s, y + 42 * s);
    bool hov = m_mouseInside && Contains(btn, m_mouse);
    FillRound(btn, 6 * s, kWhite, hov ? 0.16f : 0.09f);
    IconLabel(btn, L"", m_updateAvailable ? Tf("settings.update.install", m_release.version.c_str()) : std::wstring(T("settings.update.check")),
              kWhite, 1);
    m_settingRows.push_back({kCheckNow, btn});
    Text(L"CyImgView " CYIMG_VERSION_WSTR L"   ·   " + Tf("settings.ini_path", L"%APPDATA%\\CyImgView\\settings.ini"), m_tfSmall.Get(),
         D2D1::RectF(p.left + pad, btn.bottom + 6 * s, p.right - pad, btn.bottom + 30 * s), kGrey, 0.8f);
    m_rt->PopAxisAlignedClip();
}
