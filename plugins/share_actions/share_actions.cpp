// Plugin CyImgView : actions « Ouvrir avec » (modèle d'un plugin qui ajoute des actions au menu).
//
//  - Imprimer… : verbe « print » de l'Explorateur (assistant d'impression de Windows).
//  - Ouvrir dans le navigateur : le navigateur par défaut (association .html) ouvre l'image.
//
// En visualisation, run() reçoit le fichier affiché ; en mode édition, un PNG temporaire de l'image
// modifiée. Une action peut aussi simplement décrire un programme externe (champs program et args)
// au lieu d'une fonction run().

#include "cyimg_edit_plugin.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <string>

namespace {

wchar_t g_browser[MAX_PATH * 2];  // exécutable du navigateur par défaut (icône et lancement)

bool FindBrowser() {
    for (const wchar_t* ext : {L".html", L".htm"}) {
        DWORD n = ARRAYSIZE(g_browser);
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, ext, L"open", g_browser, &n)) && g_browser[0]) return true;
    }
    g_browser[0] = 0;
    return false;
}

int Print(const CyImgEditHost*, CyImgEditCtx*, const wchar_t* path, void* owner) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = static_cast<HWND>(owner);
    sei.lpVerb = L"print";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) ? 1 : 0;
}

int OpenInBrowser(const CyImgEditHost*, CyImgEditCtx*, const wchar_t* path, void* owner) {
    if (!g_browser[0] && !FindBrowser()) return 0;
    wchar_t url[4096];
    DWORD n = ARRAYSIZE(url);
    if (FAILED(UrlCreateFromPathW(path, url, &n, 0))) return 0;  // file:///C:/...
    const std::wstring args = L"\"" + std::wstring(url) + L"\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = static_cast<HWND>(owner);
    sei.lpFile = g_browser;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) ? 1 : 0;
}

CyImgAction g_print = {
    sizeof(CyImgAction), "print", "plugin.share_actions.print", L"Imprimer…", nullptr, 0xE749, nullptr, Print, nullptr, nullptr,
};
CyImgAction g_browserAction = {
    sizeof(CyImgAction), "browser", "plugin.share_actions.browser", L"Ouvrir dans le navigateur", nullptr, 0xE774,
    L"jpg;jpeg;jfif;png;gif;webp;avif;svg;bmp;ico", OpenInBrowser, nullptr, nullptr,
};
const CyImgAction* const kActions[] = {&g_print, &g_browserAction};

const CyImgEditPlugin kPlugin = {
    CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "share_actions", "plugin.share_actions.name", L"Partage", L"1.0",
    nullptr, 0, nullptr, 0, nullptr, 0, kActions, 2,
};

}  // namespace

// Appelée une fois, au chargement : l'icône de l'action est celle du navigateur installé.
extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) {
    if (FindBrowser()) g_browserAction.icon = g_browser;
    return &kPlugin;
}
