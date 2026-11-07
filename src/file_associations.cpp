#include "file_associations.h"
#include "resource.h"

#include <windows.h>
#include <shlobj.h>
#include <vector>

namespace cyimg::shell {
namespace {
const std::wstring kClasses = L"Software\\Classes\\";
const std::wstring kCapabilities = L"Software\\CyImgView\\Capabilities\\FileAssociations";
const std::wstring kSupportedTypes = kClasses + L"Applications\\CyImgView.exe\\SupportedTypes";

LSTATUS ReadString(const std::wstring& key, const wchar_t* name, std::wstring& value) {
    value.clear();
    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS) return status;
    if (bytes > 65536 || bytes % sizeof(wchar_t)) return ERROR_INVALID_DATA;
    std::vector<wchar_t> text(bytes / sizeof(wchar_t) + 1, L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, text.data(), &bytes);
    if (status == ERROR_SUCCESS) value = text.data();
    return status;
}

bool SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value, bool& changed) {
    std::wstring previous;
    if (ReadString(key, name, previous) == ERROR_SUCCESS && previous == value) return true;
    HKEY handle;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS status = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                        DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    if (status != ERROR_SUCCESS) return false;
    changed = true;
    return true;
}

bool OwnedProgId(const std::wstring& value) {
    return _wcsicmp(value.c_str(), kProjectProgId) == 0 || _wcsicmp(value.c_str(), L"CyImgView.Image") == 0 ||
           _wcsicmp(value.c_str(), L"Applications\\CyImgView.exe") == 0;
}

void DeleteValue(const std::wstring& key, const wchar_t* name) {
    HKEY handle;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &handle) == ERROR_SUCCESS) {
        RegDeleteValueW(handle, name);
        RegCloseKey(handle);
    }
}
}

bool RegisterProjectAssociation(const std::wstring& executable) {
    if (executable.empty() || executable.find(L'"') != std::wstring::npos) return false;
    const std::wstring project = kClasses + kProjectProgId;
    const std::wstring icon = L"\"" + executable + L"\",-" + std::to_wstring(IDI_CYIMG_DOCUMENT);
    const std::wstring command = L"\"" + executable + L"\" \"%1\"";
    bool changed = false;
    bool ok = SetString(project, nullptr, L"CyImg", changed);
    ok = SetString(project + L"\\DefaultIcon", nullptr, icon, changed) && ok;
    ok = SetString(project + L"\\shell\\open\\command", nullptr, command, changed) && ok;
    // Publish the handler only once its icon and opening command are present.
    if (ok) {
        ok = SetString(kClasses + L".cyimg\\OpenWithProgids", kProjectProgId, L"", changed) && ok;
        ok = SetString(kSupportedTypes, L".cyimg", L"", changed) && ok;
        ok = SetString(kCapabilities, L".cyimg", kProjectProgId, changed) && ok;
        std::wstring current;
        const LSTATUS status = ReadString(kClasses + L".cyimg", nullptr, current);
        if ((status == ERROR_SUCCESS && (current.empty() || OwnedProgId(current))) || status == ERROR_FILE_NOT_FOUND)
            ok = SetString(kClasses + L".cyimg", nullptr, kProjectProgId, changed) && ok;
    }
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

void UnregisterProjectAssociation() {
    std::wstring current;
    if (ReadString(kClasses + L".cyimg", nullptr, current) == ERROR_SUCCESS && current == kProjectProgId)
        DeleteValue(kClasses + L".cyimg", nullptr);
    DeleteValue(kClasses + L".cyimg\\OpenWithProgids", kProjectProgId);
    DeleteValue(kSupportedTypes, L".cyimg");
    if (ReadString(kCapabilities, L".cyimg", current) == ERROR_SUCCESS && current == kProjectProgId)
        DeleteValue(kCapabilities, L".cyimg");
    RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + kProjectProgId).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

void RefreshInstalledProjectAssociation(const std::wstring& executable) {
    std::wstring installed;
    if (ReadString(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CyImgView", L"InstallLocation", installed) != ERROR_SUCCESS)
        return;
    if (!installed.empty() && installed.back() != L'\\') installed += L'\\';
    if (_wcsicmp(executable.c_str(), (installed + L"CyImgView.exe").c_str()) == 0)
        RegisterProjectAssociation(executable);
}
}
