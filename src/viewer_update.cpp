// Mises à jour via GitHub Releases : recherche en arrière-plan, dialogue, téléchargement avec
// progression, installation et redémarrage.

#include "i18n.h"
#include "version.h"
#include "viewer.h"
#include "viewer_util.h"

#include <commctrl.h>
#include <shellapi.h>

#include <ctime>
#include <thread>

using namespace ui;

void Viewer::StartUpdateCheck(bool manual) {
    const std::wstring repo = CYIMG_UPDATE_REPO;
    if (repo.empty()) {
        if (manual) ShowToast(T("update.toast.no_repo"));
        return;
    }
    if (m_updateChecking) return;
    m_updateChecking = true;
    if (manual) ShowToast(T("update.toast.checking"));
    HWND hwnd = m_hwnd;
    std::thread([hwnd, repo, manual] {
        auto* r = new UpdateResult{};
        r->manual = manual;
        r->status = CheckForUpdate(repo, CYIMG_VERSION_WSTR, r->release, r->error);
        if (!PostMessageW(hwnd, kMsgUpdate, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void Viewer::OnUpdateResult(UpdateResult* raw) {
    std::unique_ptr<UpdateResult> r(raw);
    m_updateChecking = false;
    if (!m_iniPath.empty() && r->status != UpdateStatus::Error)
        WritePrivateProfileStringW(L"update", L"last", std::to_wstring(std::time(nullptr)).c_str(), m_iniPath.c_str());

    switch (r->status) {
    case UpdateStatus::Available: {
        wchar_t skip[64] = {};
        if (!m_iniPath.empty()) GetPrivateProfileStringW(L"update", L"skip", L"", skip, 64, m_iniPath.c_str());
        if (!r->manual && r->release.version == skip) return;  // version ignorée par l'utilisateur
        m_release = r->release;
        m_updateAvailable = true;
        Invalidate();
        if (r->manual) ShowUpdateDialog();
        else ShowToast(Tf("update.toast.available", m_release.version.c_str()));
        break;
    }
    case UpdateStatus::UpToDate:
        if (r->manual) ShowToast(Tf("update.toast.up_to_date", CYIMG_VERSION_WSTR));
        break;
    default:
        if (r->manual) MessageBoxW(m_hwnd, Tf("update.err.check", r->error.c_str()).c_str(), L"CyImgView", MB_ICONWARNING);
        break;
    }
}

void Viewer::ShowUpdateDialog() {
    if (!m_updateAvailable) return;
    std::wstring notes = m_release.notes;
    if (notes.size() > 1500) notes = notes.substr(0, 1500) + L"\n…";
    if (notes.empty()) notes = T("update.no_notes");
    std::wstring size;
    if (m_release.assetSize) size = Tf("update.size_mb", m_release.assetSize / (1024.0 * 1024.0));
    std::wstring installLabel = Tf("update.btn.install", size.c_str());
    std::wstring main = Tf("update.available", m_release.version.c_str());
    std::wstring sub = Tf("update.installed_version", CYIMG_VERSION_WSTR);

    TASKDIALOG_BUTTON buttons[] = {
        {100, installLabel.c_str()},
        {101, T("update.btn.release_page")},
        {102, T("update.btn.skip")},
    };
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = m_hwnd;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW |
                 TDF_EXPAND_FOOTER_AREA;
    tc.pszWindowTitle = T("update.title");
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszMainInstruction = main.c_str();
    tc.pszContent = sub.c_str();
    tc.pszExpandedInformation = notes.c_str();
    tc.pszExpandedControlText = T("update.notes.hide");
    tc.pszCollapsedControlText = T("update.notes.show");
    tc.cButtons = ARRAYSIZE(buttons);
    tc.pButtons = buttons;
    tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    tc.nDefaultButton = 100;
    int choice = 0;
    if (FAILED(TaskDialogIndirect(&tc, &choice, nullptr, nullptr))) return;
    if (choice == 100) {
        InstallUpdateNow();
    } else if (choice == 101 && !m_release.page.empty()) {
        ShellExecuteW(m_hwnd, L"open", m_release.page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (choice == 102) {
        if (!m_iniPath.empty()) WritePrivateProfileStringW(L"update", L"skip", m_release.version.c_str(), m_iniPath.c_str());
        m_updateAvailable = false;
        Invalidate();
    }
}

namespace {

struct InstallJob {
    ReleaseInfo rel;
    std::wstring zip, appDir, error;
    std::atomic<bool> cancel{false}, finished{false}, installing{false};
    std::atomic<uint64_t> done{0}, total{0};
    bool ok = false;
    std::thread worker;
};

HRESULT CALLBACK InstallDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR ref) {
    auto* job = reinterpret_cast<InstallJob*>(ref);
    switch (msg) {
    case TDN_CREATED:
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
        job->worker = std::thread([job] {
            job->ok = DownloadUpdate(job->rel, job->zip,
                                     [job](uint64_t d, uint64_t t) {
                                         job->done = d;
                                         job->total = t;
                                         return !job->cancel.load();
                                     },
                                     job->error);
            if (job->ok && !job->cancel) {
                job->installing = true;
                job->ok = InstallUpdate(job->zip, job->appDir, job->error);
            } else if (job->cancel) {
                job->ok = false;
            }
            job->finished = true;
        });
        break;
    case TDN_TIMER: {
        uint64_t d = job->done, t = job->total;
        if (t) SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, WPARAM(d * 1000 / t), 0);
        const std::wstring b = job->installing ? std::wstring(T("update.installing"))
                                               : Tf("update.progress", d / 1048576.0, t / 1048576.0);
        SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(b.c_str()));
        if (job->installing) SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
        if (job->finished) SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);  // ferme le dialogue
        break;
    }
    case TDN_BUTTON_CLICKED:
        if (wp == IDCANCEL) {
            if (job->installing && !job->finished) return S_FALSE;  // on ne coupe pas une installation
            job->cancel = true;
        }
        break;
    default: break;
    }
    return S_OK;
}

}  // namespace

void Viewer::InstallUpdateNow() {
    if (m_edit.active && !EndEdit(true)) return;  // modifications en cours : enregistrer d'abord

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    InstallJob job;
    job.rel = m_release;
    job.zip = std::wstring(tmp) + L"CyImgView-update-" + m_release.version + L".zip";
    job.appDir = m_exeDir;

    std::wstring main = Tf("update.updating_to", m_release.version.c_str());
    TASKDIALOGCONFIG tc{sizeof(tc)};
    tc.hwndParent = m_hwnd;
    tc.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = T("update.title");
    tc.pszMainInstruction = main.c_str();
    tc.pszContent = T("update.connecting");
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pfCallback = InstallDialogProc;
    tc.lpCallbackData = reinterpret_cast<LONG_PTR>(&job);
    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
    job.cancel = true;
    if (job.worker.joinable()) job.worker.join();

    if (!job.ok) {
        if (!job.error.empty() && job.error != T("updater.err.cancelled"))
            MessageBoxW(m_hwnd, Tf("update.err.failed", job.error.c_str()).c_str(), L"CyImgView", MB_ICONERROR);
        return;
    }

    // Version affichée dans « Applications installées » (si installé par l'installeur)
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CyImgView", 0,
                      KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, reinterpret_cast<const BYTE*>(m_release.version.c_str()),
                       DWORD((m_release.version.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }

    // Redémarrage sur la même image
    std::wstring exe = m_exeDir + L"\\CyImgView.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    if (const FileEntry* cur = Current()) cmd += L" \"" + cur->path + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, m_exeDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(m_hwnd, T("update.done_restart"), L"CyImgView", MB_ICONINFORMATION);
    }
    m_updateAvailable = false;
    PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
}
