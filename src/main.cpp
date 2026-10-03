#include "i18n.h"
#include "version.h"
#include "viewer.h"

#include <shellapi.h>

#include <cstring>

// Erreur grave (accès mémoire invalide…) : écrit le code et la pile (module + décalage) dans
// %LOCALAPPDATA%\CyImgView\crash.txt, pour retrouver la fonction fautive. Gestionnaire « vectorisé » :
// il voit l'erreur avant tout le reste, même dans une procédure de fenêtre appelée par le noyau, où
// Windows termine le processus sans passer par le filtre habituel. Pas d'allocation ni de CRT ici.
static LONG WINAPI WriteCrashLog(EXCEPTION_POINTERS* ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code < 0xC0000000 || code == 0xE06D7363 /* exception C++ */) return EXCEPTION_CONTINUE_SEARCH;
    static LONG busy = 0;
    if (InterlockedExchange(&busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    wchar_t path[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) {
        wcscat_s(path, L"\\CyImgView");
        CreateDirectoryW(path, nullptr);
        wcscat_s(path, L"\\crash.txt");
        HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char line[512];
            DWORD w = 0;
            int len = wsprintfA(line, "CyImgView %s\r\nException %08lX\r\n", CYIMG_VERSION_STR, code);
            WriteFile(f, line, DWORD(len), &w, nullptr);
            void* frames[62];
            const USHORT n = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
            for (USHORT i = 0; i < n; ++i) {
                HMODULE mod = nullptr;
                char name[MAX_PATH] = "?";
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCWSTR>(frames[i]), &mod))
                    GetModuleFileNameA(mod, name, MAX_PATH);
                const char* base = strrchr(name, '\\');
                len = wsprintfA(line, "  %s+0x%I64X\r\n", base ? base + 1 : name,
                                static_cast<unsigned long>(static_cast<char*>(frames[i]) - reinterpret_cast<char*>(mod)));
                WriteFile(f, line, DWORD(len), &w, nullptr);
            }
            CloseHandle(f);
        }
    }
    InterlockedExchange(&busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    AddVectoredExceptionHandler(1, WriteCrashLog);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    // Langue de l'interface : réglage [settings] language (vide = langue de Windows)
    {
        wchar_t exe[MAX_PATH * 2], lang[32] = {};
        DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        std::wstring dir(exe, n);
        dir = dir.substr(0, dir.find_last_of(L'\\'));
        GetPrivateProfileStringW(L"settings", L"language", L"", lang, ARRAYSIZE(lang), SettingsIniPath().c_str());
        i18n::Init(dir, lang);
    }

    std::wstring path;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) path = argv[1];
        LocalFree(argv);
    }

    // Option « Ouvrir les fichiers dans la fenêtre existante »
    if (!path.empty() && GetPrivateProfileIntW(L"settings", L"single_instance", 0, SettingsIniPath().c_str()) &&
        ForwardToRunningInstance(path)) {
        CoUninitialize();
        return 0;
    }

    StartGpuWarmup();  // seulement si cette instance affiche une fenêtre

    int code = 1;
    {
        Viewer viewer;
        if (viewer.Create(inst, show, path)) code = viewer.Run();
    }
    CoUninitialize();
    return code;
}
