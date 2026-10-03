// Plugin de TEST (jamais livré) : chaque fonction fait un accès mémoire invalide, pour vérifier que
// l'hôte désactive la fonction fautive avec un message, sans fermer l'application ni perdre le
// document. Compilé seulement avec -DCYIMG_TEST_PLUGINS=ON, dans bin/testplugins.

#include "cyimg_edit_plugin.h"

namespace {

void Crash() {
    volatile int* p = nullptr;
    *p = 42;  // accès mémoire invalide volontaire
}

int CrashProcess(const CyImgEditHost*, CyImgEditCtx*, CyImgImage*, const double*) {
    Crash();
    return CYIMG_OK;
}
int CrashDown(const CyImgEditHost*, CyImgEditCtx*, const CyImgToolEvent*) {
    Crash();
    return 0;
}
int CrashRun(const CyImgEditHost*, CyImgEditCtx*, const wchar_t*, void*) {
    Crash();
    return 1;
}

const CyImgParam kParams[] = {CYIMG_SLIDER(nullptr, L"Valeur", 0, 100, 50, 1, nullptr)};

const CyImgEffect kEffect = {sizeof(CyImgEffect), "crash", nullptr, L"Planter (test)", nullptr, L"Test", kParams, 1, CrashProcess};
const CyImgEffect* const kEffects[] = {&kEffect};

const CyImgTool kTool = {sizeof(CyImgTool), "crash", nullptr, L"Outil qui plante (test)", nullptr, L"Test", 0xE7BA, nullptr, 0, 0,
                         nullptr, 0, CrashDown, nullptr, nullptr, nullptr, nullptr, nullptr};
const CyImgTool* const kTools[] = {&kTool};

const CyImgMode kMode = {sizeof(CyImgMode), "crash", nullptr, L"Plante (test)", nullptr, nullptr, 0xE7BA, kParams, 1,
                         CrashProcess, nullptr, nullptr, nullptr};
const CyImgMode* const kModes[] = {&kMode};

const CyImgAction kActionCrash = {sizeof(CyImgAction), "crash", nullptr, L"Action qui plante (test)", nullptr, 0xE7BA, nullptr, CrashRun,
                                  nullptr, nullptr};
const CyImgAction kActionProgram = {sizeof(CyImgAction), "notepad", nullptr, L"Bloc-notes (test)", L"%SystemRoot%\\System32\\notepad.exe",
                                    0, nullptr, nullptr, L"%SystemRoot%\\System32\\notepad.exe", L"\"%1\""};
const CyImgAction* const kActions[] = {&kActionCrash, &kActionProgram};

const CyImgEditPlugin kPlugin = {
    CYIMG_EDIT_API_VERSION, sizeof(CyImgEditPlugin), "crashtest", nullptr, L"Plugin de test (plante)", L"0",
    kEffects, 1, kTools, 1, kModes, 1, kActions, 2,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgEditPlugin* CyImg_GetEditPlugin(const CyImgEditHost*) { return &kPlugin; }
