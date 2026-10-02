#include "viewer.h"

#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    StartGpuWarmup();
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    std::wstring path;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) path = argv[1];
        LocalFree(argv);
    }

    int code = 1;
    {
        Viewer viewer;
        if (viewer.Create(inst, show, path)) code = viewer.Run();
    }
    CoUninitialize();
    return code;
}
