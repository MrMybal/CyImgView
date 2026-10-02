#include "viewer.h"

#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

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
