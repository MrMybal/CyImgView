// All registry operations are redirected to a disposable per-process hive. No real associations change.
#include "file_associations.h"
#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <stdexcept>
#include <string>

static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    std::cout << "PASS " << message << std::endl;
}

struct TestRegistry {
    HKEY key = nullptr;
    std::wstring path = L"Software\\CyImgViewShellTests\\ProjectIcons-" + std::to_wstring(GetCurrentProcessId());
    TestRegistry() {
        const LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                                              nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) throw std::runtime_error("create isolated registry hive: " + std::to_string(status));
        Check(true, "create isolated registry hive");
        if (RegOverridePredefKey(HKEY_CURRENT_USER, key) != ERROR_SUCCESS) {
            RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
            throw std::runtime_error("redirect registry");
        }
    }
    ~TestRegistry() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegCloseKey(key);
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\CyImgViewShellTests"); // only succeeds when empty
    }
};

static void Set(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY handle;
    const LSTATUS create = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr);
    if (create != ERROR_SUCCESS) throw std::runtime_error("create registry fixture key: " + std::to_string(create));
    const LSTATUS status = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                        DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    Check(status == ERROR_SUCCESS, "seed registry fixture");
}
static std::wstring Get(const std::wstring& key, const wchar_t* name = nullptr) {
    wchar_t value[4096] = {};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS ? value : L"";
}

int wmain(int argc, wchar_t** argv) {
    try {
        const HMODULE module = GetModuleHandleW(nullptr);
        Check(FindResourceW(module, MAKEINTRESOURCEW(1), RT_GROUP_ICON) != nullptr, "application icon still embedded as resource 1");
        Check(FindResourceW(module, MAKEINTRESOURCEW(IDI_CYIMG_DOCUMENT), RT_GROUP_ICON) != nullptr, "separate document icon embedded as resource 101");
        for (int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
            HICON icon = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(IDI_CYIMG_DOCUMENT), IMAGE_ICON, size, size, 0));
            Check(icon != nullptr, "Windows loads document icon at shell size");
            DestroyIcon(icon);
        }
        if (argc > 1) {
            HICON icon = nullptr;
            Check(ExtractIconExW(argv[1], -IDI_CYIMG_DOCUMENT, &icon, nullptr, 1) == 1 && icon,
                  "Windows extracts stable document icon ID from application binary");
            DestroyIcon(icon);
        }
        TestRegistry registry;
        const std::wstring classes = L"Software\\Classes\\";
        const std::wstring project = classes + cyimg::shell::kProjectProgId;
        const std::wstring exe = L"C:\\Example Folder\\CyImgView.exe";
        const std::wstring choice = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.cyimg\\UserChoice";
        Set(classes + L".png", nullptr, L"Other.Image");
        Set(classes + L"CyImgView.Image\\DefaultIcon", nullptr, L"app-icon,0");
        Set(choice, L"ProgId", L"Other.Editor");
        Check(cyimg::shell::RegisterProjectAssociation(exe), "register project handler");
        Check(Get(classes + L".cyimg") == cyimg::shell::kProjectProgId, "unclaimed native extension uses project handler");
        Check(Get(project + L"\\DefaultIcon") == L"\"" + exe + L"\",-101", "document icon uses quoted path and stable resource ID");
        Check(Get(project + L"\\shell\\open\\command") == L"\"" + exe + L"\" \"%1\"", "opening command quotes executable and document");
        Check(Get(L"Software\\CyImgView\\Capabilities\\FileAssociations", L".cyimg") == cyimg::shell::kProjectProgId,
              "native format exposed to Windows default-app settings");
        Check(cyimg::shell::RegisterProjectAssociation(exe), "repeated registration succeeds");
        Check(Get(classes + L".png") == L"Other.Image" && Get(classes + L"CyImgView.Image\\DefaultIcon") == L"app-icon,0",
              "ordinary image associations and application icon stay unchanged");
        Check(Get(choice, L"ProgId") == L"Other.Editor", "explicit Windows UserChoice is untouched");
        Set(classes + L".cyimg", nullptr, L"Other.Editor");
        Check(cyimg::shell::RegisterProjectAssociation(exe) && Get(classes + L".cyimg") == L"Other.Editor",
              "existing third-party default is preserved");
        Set(classes + L".cyimg", nullptr, L"CyImgView.Image");
        Check(cyimg::shell::RegisterProjectAssociation(exe) && Get(classes + L".cyimg") == cyimg::shell::kProjectProgId,
              "old shared CyImgView association migrates to document handler");
        Set(classes + L".cyimg\\OpenWithProgids", L"Other.Editor", L"keep");
        Set(classes + L".cyimg", L"PrivateMetadata", L"keep");
        cyimg::shell::UnregisterProjectAssociation();
        Check(Get(classes + L".cyimg").empty() && Get(project + L"\\DefaultIcon").empty(), "uninstall removes owned handler and default");
        Check(Get(classes + L".cyimg\\OpenWithProgids", L"Other.Editor") == L"keep" &&
              Get(classes + L".cyimg", L"PrivateMetadata") == L"keep", "uninstall preserves other handlers and extension data");
        Set(classes + L".cyimg", nullptr, L"Other.Editor");
        Set(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CyImgView", L"InstallLocation", L"C:\\Example Folder");
        cyimg::shell::RefreshInstalledProjectAssociation(L"D:\\Portable\\CyImgView.exe");
        Check(Get(project + L"\\DefaultIcon").empty(), "portable launch never redirects installed association");
        cyimg::shell::RefreshInstalledProjectAssociation(exe);
        Check(Get(project + L"\\DefaultIcon") == L"\"" + exe + L"\",-101", "installed launch repairs handler after in-app update");
        cyimg::shell::UnregisterProjectAssociation();
        Check(Get(classes + L".cyimg") == L"Other.Editor" && Get(choice, L"ProgId") == L"Other.Editor",
              "uninstall preserves third-party default and Windows choice");
        Check(!cyimg::shell::RegisterProjectAssociation(L"bad\"path.exe"), "invalid executable cannot break command quoting");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << std::endl;
        return 1;
    }
}
