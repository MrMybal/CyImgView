// Installeur / désinstalleur de CyImgView.
//
//  CyImgView-<version>-Setup.exe = ce programme + archive de release (zip) collée à la fin,
//  suivie d'un pied de 16 octets : "CYIMGPK1" + taille du zip (uint64).
//
//  - Installation par utilisateur, sans droits administrateur : %LOCALAPPDATA%\Programs\CyImgView
//    (les mises à jour automatiques peuvent donc remplacer les fichiers).
//  - Raccourcis Menu Démarrer (+ Bureau en option), « Ouvrir avec » et Applications par défaut
//    pour les formats d'image, entrée « Applications » de Windows pour la désinstallation.
//  - Désinstallation : uninstall.exe --uninstall (se recopie dans %TEMP% pour pouvoir effacer
//    le dossier d'installation).
//  Options : /S (installation silencieuse), --uninstall [/S].

#include "../src/common.h"
#include "../src/updater.h"
#include "../src/version.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <vector>

namespace {

const wchar_t kTitle[] = L"CyImgView";
const wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CyImgView";
const wchar_t kProgId[] = L"CyImgView.Image";
const char kMagic[8] = {'C', 'Y', 'I', 'M', 'G', 'P', 'K', '1'};

// Extensions proposées dans « Ouvrir avec » / Applications par défaut
const wchar_t* kExts[] = {L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"gif", L"bmp", L"dib", L"tif", L"tiff", L"ico",
                          L"webp", L"heic", L"heif", L"avif", L"jxr", L"wdp", L"dds",
                          L"cr2", L"cr3", L"crw", L"nef", L"nrw", L"arw", L"srf", L"sr2", L"dng", L"raf", L"rw2",
                          L"orf", L"pef", L"srw", L"x3f", L"3fr", L"iiq", L"erf", L"kdc", L"mrw", L"uasset", L"tga",
                          // formats ajoutés par les plugins de la 1.4 (« Ouvrir avec » seulement, sans imposer
                          // CyImgView comme application par défaut)
                          L"avifs", L"heics", L"heifs", L"hif", L"jxl", L"jp2", L"j2k", L"j2c", L"jpc", L"jpf", L"jpx",
                          L"jph", L"jhc", L"exr", L"hdr", L"rgbe", L"pfm", L"pbm", L"pgm", L"ppm", L"pnm", L"pam",
                          L"pcx", L"xbm", L"xpm", L"iff", L"lbm", L"ilbm", L"fits", L"fit", L"fts", L"icns", L"pct",
                          L"pict", L"dcm", L"dicom", L"psd", L"psb", L"kra", L"ora", L"xcf", L"clip", L"indd", L"eps",
                          L"epsf", L"cdr", L"afphoto", L"afdesign", L"afpub", L"svg", L"svgz", L"pdf", L"ai", L"wmf",
                          L"emf", L"emz", L"wmz", L"dxf"};

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring InstallDir() { return KnownFolder(FOLDERID_UserProgramFiles) + L"\\CyImgView"; }
std::wstring SelfPath() {
    wchar_t b[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, b, ARRAYSIZE(b));
    return b;
}

void SetString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t)));
}
void SetDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD v) {
    RegSetKeyValueW(root, key.c_str(), name, REG_DWORD, &v, sizeof(v));
}

bool CreateShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir) {
    ComPtr<IShellLinkW> sl;
    ComPtr<IPersistFile> pf;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetDescription(L"Visionneuse d'images rapide");
    sl->SetIconLocation(target.c_str(), 0);
    return SUCCEEDED(sl.As(&pf)) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
}

uint64_t DirSizeKb(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += DirSizeKb(dir + L"\\" + n) * 1024;
        else total += (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total / 1024;
}

void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// --- Charge utile (zip collé à la fin de l'exe) -------------------------------------------------

bool ExtractPayload(const std::wstring& self, const std::wstring& zipOut, uint64_t& stubSize, std::wstring& error) {
    HANDLE f = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = L"lecture de l'installeur impossible";
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    char footer[16] = {};
    DWORD got = 0;
    LARGE_INTEGER pos;
    pos.QuadPart = sz.QuadPart - 16;
    bool ok = sz.QuadPart > 16 && SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) && ReadFile(f, footer, 16, &got, nullptr) &&
              got == 16 && memcmp(footer, kMagic, 8) == 0;
    uint64_t zipSize = 0;
    if (ok) memcpy(&zipSize, footer + 8, 8);
    ok = ok && zipSize > 0 && zipSize < uint64_t(sz.QuadPart) - 16;
    if (!ok) {
        CloseHandle(f);
        error = L"installeur incomplet (archive absente)";
        return false;
    }
    stubSize = uint64_t(sz.QuadPart) - 16 - zipSize;
    pos.QuadPart = int64_t(stubSize);
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    std::vector<uint8_t> zip(static_cast<size_t>(zipSize));
    ok = ReadFile(f, zip.data(), DWORD(zip.size()), &got, nullptr) && got == zip.size();
    CloseHandle(f);
    HANDLE o = ok ? CreateFileW(zipOut.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr)
                  : INVALID_HANDLE_VALUE;
    DWORD w = 0;
    ok = o != INVALID_HANDLE_VALUE && WriteFile(o, zip.data(), DWORD(zip.size()), &w, nullptr) && w == zip.size();
    if (o != INVALID_HANDLE_VALUE) CloseHandle(o);
    if (!ok) error = L"écriture du fichier temporaire impossible";
    return ok;
}

// Copie de la partie « programme » de l'installeur (sans l'archive) : sert de désinstalleur
bool CopyStub(const std::wstring& self, uint64_t stubSize, const std::wstring& dest) {
    HANDLE f = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> buf(static_cast<size_t>(stubSize));
    DWORD got = 0;
    bool ok = ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got == buf.size();
    CloseHandle(f);
    if (!ok) return false;
    std::wstring tmp = dest + L".new";
    HANDLE o = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    ok = o != INVALID_HANDLE_VALUE && WriteFile(o, buf.data(), DWORD(buf.size()), &w, nullptr) && w == buf.size();
    if (o != INVALID_HANDLE_VALUE) CloseHandle(o);
    if (ok) {
        if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) MoveFileExW(dest.c_str(), (dest + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
        ok = MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return ok;
}

// --- Intégration Windows ------------------------------------------------------------------------

void RegisterApp(const std::wstring& dir, bool desktop) {
    const std::wstring exe = dir + L"\\CyImgView.exe";
    const std::wstring open = L"\"" + exe + L"\" \"%1\"";
    const std::wstring classes = L"Software\\Classes\\";

    // ProgID + « Ouvrir avec »
    SetString(HKEY_CURRENT_USER, classes + kProgId, nullptr, L"Image (CyImgView)");
    SetString(HKEY_CURRENT_USER, classes + kProgId + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
    SetString(HKEY_CURRENT_USER, classes + kProgId + L"\\shell\\open\\command", nullptr, open);
    const std::wstring app = classes + L"Applications\\CyImgView.exe";
    SetString(HKEY_CURRENT_USER, app, L"FriendlyAppName", L"CyImgView");
    SetString(HKEY_CURRENT_USER, app + L"\\shell\\open\\command", nullptr, open);
    const std::wstring caps = L"Software\\CyImgView\\Capabilities";
    SetString(HKEY_CURRENT_USER, caps, L"ApplicationName", L"CyImgView");
    SetString(HKEY_CURRENT_USER, caps, L"ApplicationDescription", L"Visionneuse d'images rapide (photos, RAW, textures Unreal)");
    for (const wchar_t* e : kExts) {
        std::wstring ext = std::wstring(L".") + e;
        SetString(HKEY_CURRENT_USER, app + L"\\SupportedTypes", ext.c_str(), L"");
        SetString(HKEY_CURRENT_USER, classes + ext + L"\\OpenWithProgids", kProgId, L"");
        SetString(HKEY_CURRENT_USER, caps + L"\\FileAssociations", ext.c_str(), kProgId);
    }
    SetString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"CyImgView", caps);

    // Raccourcis
    std::wstring programs = KnownFolder(FOLDERID_Programs);
    CreateShortcut(programs + L"\\CyImgView.lnk", exe, dir);
    if (desktop) CreateShortcut(KnownFolder(FOLDERID_Desktop) + L"\\CyImgView.lnk", exe, dir);

    // Entrée « Applications installées »
    SetString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", L"CyImgView");
    SetString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", CYIMG_VERSION_WSTR);
    SetString(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", L"MrMybal");
    SetString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", exe + L",0");
    SetString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir);
    SetString(HKEY_CURRENT_USER, kUninstallKey, L"URLInfoAbout", L"https://github.com/" CYIMG_UPDATE_REPO);
    SetString(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString", L"\"" + dir + L"\\uninstall.exe\" --uninstall");
    SetString(HKEY_CURRENT_USER, kUninstallKey, L"QuietUninstallString", L"\"" + dir + L"\\uninstall.exe\" --uninstall /S");
    SetDword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1);
    SetDword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1);
    SetDword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize", DWORD(DirSizeKb(dir)));
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void UnregisterApp() {
    const std::wstring classes = L"Software\\Classes\\";
    RegDeleteTreeW(HKEY_CURRENT_USER, (classes + kProgId).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (classes + L"Applications\\CyImgView.exe").c_str());
    for (const wchar_t* e : kExts) {
        HKEY k;
        std::wstring key = classes + L"." + e + L"\\OpenWithProgids";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, kProgId);
            RegCloseKey(k);
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\CyImgView");
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, L"CyImgView");
        RegCloseKey(k);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    DeleteFileW((KnownFolder(FOLDERID_Programs) + L"\\CyImgView.lnk").c_str());
    DeleteFileW((KnownFolder(FOLDERID_Desktop) + L"\\CyImgView.lnk").c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// --- Dialogues ------------------------------------------------------------------------------------

int Dialog(const wchar_t* main, const std::wstring& content, const TASKDIALOG_BUTTON* buttons, UINT count,
           TASKDIALOG_COMMON_BUTTON_FLAGS common, PCWSTR icon, const wchar_t* verify = nullptr, BOOL* verified = nullptr,
           bool useLinks = true) {
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | (useLinks ? TDF_USE_COMMAND_LINKS : 0) |
                 (verified && *verified ? TDF_VERIFICATION_FLAG_CHECKED : 0);
    tc.pszWindowTitle = kTitle;
    tc.hMainIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (tc.hMainIcon && !icon) tc.dwFlags |= TDF_USE_HICON_MAIN;
    else tc.pszMainIcon = icon;
    tc.pszMainInstruction = main;
    tc.pszContent = content.c_str();
    tc.pButtons = buttons;
    tc.cButtons = count;
    tc.dwCommonButtons = common;
    tc.pszVerificationText = verify;
    int choice = IDCANCEL;
    TaskDialogIndirect(&tc, &choice, nullptr, verified);
    return choice;
}

bool AppRunningFrom(const std::wstring& dir) {
    // Une fenêtre CyImgView ouverte depuis ce dossier ?
    struct Ctx {
        std::wstring dir;
        bool found = false;
    } c{dir};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[64];
            if (!GetClassNameW(h, cls, 64) || wcscmp(cls, L"CyImgViewWindow") != 0) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!p) return TRUE;
            wchar_t path[MAX_PATH * 2];
            DWORD n = ARRAYSIZE(path);
            if (QueryFullProcessImageNameW(p, 0, path, &n) && _wcsnicmp(path, c->dir.c_str(), c->dir.size()) == 0) c->found = true;
            CloseHandle(p);
            return !c->found;
        },
        reinterpret_cast<LPARAM>(&c));
    return c.found;
}

void CloseRunningApp(const std::wstring& dir) {
    // Demande poliment aux fenêtres ouvertes depuis ce dossier de se fermer
    struct Ctx {
        std::wstring dir;
    } c{dir};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[64];
            if (GetClassNameW(h, cls, 64) && wcscmp(cls, L"CyImgViewWindow") == 0) {
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                wchar_t path[MAX_PATH * 2];
                DWORD n = ARRAYSIZE(path);
                if (p && QueryFullProcessImageNameW(p, 0, path, &n) && _wcsnicmp(path, c->dir.c_str(), c->dir.size()) == 0)
                    PostMessageW(h, WM_CLOSE, 0, 0);
                if (p) CloseHandle(p);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&c));
    for (int i = 0; i < 50 && AppRunningFrom(dir); ++i) Sleep(100);
}

// --- Installation ---------------------------------------------------------------------------------

int Install(bool silent) {
    const std::wstring dir = InstallDir(), self = SelfPath();
    wchar_t installed[64] = {};
    DWORD sz = sizeof(installed);
    RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, installed, &sz);
    BOOL desktop = TRUE;

    if (!silent) {
        std::wstring content = L"Dossier : " + dir + L"\n\nAucun droit administrateur n'est nécessaire.";
        if (*installed) content += std::wstring(L"\nVersion installée : ") + installed;
        std::wstring label = std::wstring(*installed ? L"Mettre à jour" : L"Installer") +
                             L"\nRaccourci dans le Menu Démarrer, « Ouvrir avec » pour les images, photos RAW et textures Unreal";
        TASKDIALOG_BUTTON b[] = {{100, label.c_str()}};
        std::wstring main = L"Installer CyImgView " CYIMG_VERSION_WSTR;
        if (Dialog(main.c_str(), content, b, 1, TDCBF_CANCEL_BUTTON, nullptr, L"Créer un raccourci sur le Bureau", &desktop) != 100)
            return 1;
        if (AppRunningFrom(dir)) CloseRunningApp(dir);  // l'ancienne version ouverte est fermée proprement
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring zip = std::wstring(tmp) + L"CyImgView-setup-" CYIMG_VERSION_WSTR L".zip";
    std::wstring error;
    uint64_t stubSize = 0;
    CreateDirectoryW(KnownFolder(FOLDERID_UserProgramFiles).c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok = ExtractPayload(self, zip, stubSize, error) && InstallUpdate(zip, dir, error);
    DeleteFileW(zip.c_str());
    if (ok && !CopyStub(self, stubSize, dir + L"\\uninstall.exe")) {
        ok = false;
        error = L"création du désinstalleur impossible";
    }
    if (ok) {
        CleanupUpdateLeftovers(dir);
        RegisterApp(dir, desktop != FALSE);
    }
    SetCursor(old);

    if (!ok) {
        if (!silent) Dialog(L"L'installation a échoué", error, nullptr, 0, TDCBF_CLOSE_BUTTON, TD_ERROR_ICON);
        return 2;
    }
    if (silent) return 0;
    TASKDIALOG_BUTTON b[] = {{100, L"Lancer CyImgView"},
                             {101, L"Choisir CyImgView comme visionneuse par défaut\nOuvre les paramètres « Applications par défaut » de Windows"}};
    int c = Dialog(L"CyImgView est installé", L"Vous le trouverez dans le Menu Démarrer. Les mises à jour se feront automatiquement.",
                   b, 2, TDCBF_CLOSE_BUTTON, nullptr);
    if (c == 101) ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
    if (c == 100 || c == 101) ShellExecuteW(nullptr, L"open", (dir + L"\\CyImgView.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    return 0;
}

// --- Désinstallation ------------------------------------------------------------------------------

int UninstallRun(const std::wstring& dir, DWORD parentPid, bool removeSettings, bool notify) {
    if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, parentPid)) {  // attend la fin de uninstall.exe
        WaitForSingleObject(p, 10000);
        CloseHandle(p);
    }
    CloseRunningApp(dir);
    UnregisterApp();
    // Sécurité : on n'efface que le dossier d'installation reconnu
    if (GetFileAttributesW((dir + L"\\CyImgView.exe").c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((dir + L"\\uninstall.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        DeleteTree(dir);
    if (removeSettings) DeleteTree(KnownFolder(FOLDERID_RoamingAppData) + L"\\CyImgView");
    if (notify) {
        bool gone = GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES;
        std::wstring msg = gone ? L"CyImgView a été désinstallé."
                                : L"CyImgView a été désinstallé, mais certains fichiers n'ont pas pu être supprimés :\n" + dir;
        MessageBoxW(nullptr, msg.c_str(), kTitle, gone ? MB_ICONINFORMATION : MB_ICONWARNING);
    }
    // Auto-suppression de cette copie temporaire
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + SelfPath() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

int Uninstall(bool silent) {
    const std::wstring self = SelfPath(), dir = ParentOf(self);
    BOOL removeSettings = FALSE;
    if (!silent) {
        TASKDIALOG_BUTTON b[] = {{100, L"Désinstaller"}};
        if (Dialog(L"Désinstaller CyImgView ?", L"Le programme et ses raccourcis seront supprimés de " + dir + L".", b, 1,
                   TDCBF_CANCEL_BUTTON, nullptr, L"Supprimer aussi mes réglages", &removeSettings, false) != 100)
            return 1;
    }
    // Copie temporaire qui pourra effacer le dossier (un exe ne peut pas se supprimer lui-même)
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring copy = std::wstring(tmp) + L"CyImgView-uninstall.exe";
    if (!CopyFileW(self.c_str(), copy.c_str(), FALSE)) return 2;
    std::wstring cmd = L"\"" + copy + L"\" --uninstall-run \"" + dir + L"\" " + std::to_wstring(GetCurrentProcessId()) +
                       (removeSettings ? L" --remove-settings" : L"") + (silent ? L"" : L" --notify");
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(copy.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, tmp, &si, &pi)) return 2;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;  // la copie temporaire attend notre fin pour effacer le dossier
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    auto has = [&](const wchar_t* a) {
        for (auto& x : args)
            if (_wcsicmp(x.c_str(), a) == 0) return true;
        return false;
    };
    const bool silent = has(L"/S") || has(L"--silent");
    int code;
    if (args.size() >= 3 && args[0] == L"--uninstall-run")
        code = UninstallRun(args[1], DWORD(_wtoi(args[2].c_str())), has(L"--remove-settings"), has(L"--notify"));
    else if (has(L"--uninstall")) code = Uninstall(silent);
    else code = Install(silent);
    CoUninitialize();
    return code;
}
