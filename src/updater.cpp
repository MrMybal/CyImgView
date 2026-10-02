#include "updater.h"

#include "common.h"
#include "inflate.h"
#include "zip.h"
#include "version.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string_view>

namespace {

// =============================================================================================
// HTTP (WinHTTP, HTTPS uniquement)

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

struct HInternet {
    HINTERNET h = nullptr;
    ~HInternet() {
        if (h) WinHttpCloseHandle(h);
    }
};

// sink(données, taille) ; total = Content-Length (0 si inconnu). Retourne false si erreur/annulation.
bool HttpGet(const std::wstring& url, const wchar_t* accept,
             const std::function<bool(const uint8_t*, size_t, uint64_t)>& sink, std::wstring& error) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"adresse invalide";
        return false;
    }
    std::wstring object = std::wstring(path) + extra;
    HInternet session, connect, request;
    session.h = WinHttpOpen(L"CyImgView/" CYIMG_VERSION_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) session.h = WinHttpOpen(L"CyImgView", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        error = L"WinHTTP indisponible";
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    request.h = connect.h ? WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                          : nullptr;
    if (!request.h) {
        error = L"connexion impossible";
        return false;
    }
    std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.h, headers.c_str(), DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        error = L"pas de connexion à GitHub";
        return false;
    }
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
    if (status != 200) {
        wchar_t b[64];
        swprintf_s(b, status == 404 ? L"aucune release publiée (HTTP 404)" : L"réponse HTTP %lu", status);
        error = b;
        return false;
    }
    wchar_t len[32] = {};
    DWORD lsz = sizeof(len);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &lsz, nullptr)) total = _wcstoui64(len, nullptr, 10);
    std::vector<uint8_t> buf(1 << 16);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), DWORD(buf.size()), &got)) {
            error = L"téléchargement interrompu";
            return false;
        }
        if (got == 0) break;
        if (!sink(buf.data(), got, total)) {
            if (error.empty()) error = L"annulé";
            return false;
        }
    }
    return true;
}

// =============================================================================================
// JSON minimal (lecture de la réponse de l'API GitHub)

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json& operator[](const char* key) const {
        static const Json none;
        for (auto& f : fields)
            if (f.first == key) return f.second;
        return none;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view t) : m_t(t) {}
    bool Parse(Json& out) {
        if (!Value(out, 0)) return false;
        Ws();
        return m_i == m_t.size();
    }

private:
    void Ws() {
        while (m_i < m_t.size() && (m_t[m_i] == ' ' || m_t[m_i] == '\n' || m_t[m_i] == '\r' || m_t[m_i] == '\t')) ++m_i;
    }
    bool Lit(const char* w) {
        size_t n = strlen(w);
        if (m_t.compare(m_i, n, w) != 0) return false;
        m_i += n;
        return true;
    }
    static void Utf8(std::string& s, uint32_t c) {
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | c >> 6); s += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += char(0xE0 | c >> 12); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
        else { s += char(0xF0 | c >> 18); s += char(0x80 | ((c >> 12) & 63)); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
    }
    bool Hex4(uint32_t& v) {
        if (m_i + 4 > m_t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = m_t[m_i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool Str(std::string& s) {
        if (m_t[m_i] != '"') return false;
        ++m_i;
        while (m_i < m_t.size()) {
            char c = m_t[m_i++];
            if (c == '"') return true;
            if (c != '\\') {
                s += c;
                continue;
            }
            if (m_i >= m_t.size()) return false;
            char e = m_t[m_i++];
            switch (e) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u': {
                uint32_t c1;
                if (!Hex4(c1)) return false;
                if (c1 >= 0xD800 && c1 < 0xDC00 && m_t.compare(m_i, 2, "\\u") == 0) {
                    m_i += 2;
                    uint32_t c2;
                    if (!Hex4(c2)) return false;
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                Utf8(s, c1);
                break;
            }
            default: s += e; break;
            }
        }
        return false;
    }
    bool Value(Json& v, int depth) {
        if (depth > 64) return false;
        Ws();
        if (m_i >= m_t.size()) return false;
        char c = m_t[m_i];
        if (c == '{') {
            v.type = Json::Object;
            ++m_i;
            Ws();
            if (m_i < m_t.size() && m_t[m_i] == '}') return ++m_i, true;
            for (;;) {
                Ws();
                std::string key;
                if (m_i >= m_t.size() || !Str(key)) return false;
                Ws();
                if (m_i >= m_t.size() || m_t[m_i++] != ':') return false;
                Json child;
                if (!Value(child, depth + 1)) return false;
                v.fields.emplace_back(std::move(key), std::move(child));
                Ws();
                if (m_i >= m_t.size()) return false;
                if (m_t[m_i] == ',') { ++m_i; continue; }
                if (m_t[m_i] == '}') return ++m_i, true;
                return false;
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            ++m_i;
            Ws();
            if (m_i < m_t.size() && m_t[m_i] == ']') return ++m_i, true;
            for (;;) {
                Json child;
                if (!Value(child, depth + 1)) return false;
                v.items.push_back(std::move(child));
                Ws();
                if (m_i >= m_t.size()) return false;
                if (m_t[m_i] == ',') { ++m_i; continue; }
                if (m_t[m_i] == ']') return ++m_i, true;
                return false;
            }
        }
        if (c == '"') {
            v.type = Json::String;
            return Str(v.s);
        }
        if (Lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (Lit("false")) { v.type = Json::Bool; return true; }
        if (Lit("null")) return true;
        size_t start = m_i;
        while (m_i < m_t.size() && strchr("+-0123456789.eE", m_t[m_i])) ++m_i;
        if (start == m_i) return false;
        v.type = Json::Number;
        v.n = strtod(std::string(m_t.substr(start, m_i - start)).c_str(), nullptr);
        return true;
    }
    std::string_view m_t;
    size_t m_i = 0;
};

// =============================================================================================
// Divers

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

bool Sha256File(const std::wstring& path, std::wstring& hex) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
    std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), DWORD(buf.size()), &got, nullptr) && got)
        ok = BCRYPT_SUCCESS(BCryptHashData(h, buf.data(), got, 0));
    uint8_t digest[32];
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(h, digest, 32, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;
    hex.clear();
    wchar_t b[3];
    for (uint8_t d : digest) {
        swprintf_s(b, L"%02x", d);
        hex += b;
    }
    return true;
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return true;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!EnsureDir(ParentOf(dir))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteAll(const std::wstring& path, const uint8_t* data, size_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = size == 0 || (WriteFile(f, data, DWORD(size), &w, nullptr) && w == size);
    CloseHandle(f);
    return ok;
}

// =============================================================================================
// Archive zip (méthodes « stocké » et deflate)

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
};

bool ReadZip(const std::vector<uint8_t>& z, std::vector<ZipEntry>& out, std::wstring& error) {
    error = L"archive de mise à jour invalide";
    std::vector<ZipEntryInfo> list;
    if (!ZipList(z.data(), z.size(), list)) return false;
    for (auto& info : list) {
        const std::string& name = info.name;
        if (name.find("..") != std::string::npos || name[0] == '/' || name.find(':') != std::string::npos) {
            error = L"archive suspecte (chemin hors du dossier)";
            return false;
        }
        if (info.size > (512u << 20)) return false;
        ZipEntry e;
        e.name = name;
        if (!ZipExtract(z.data(), z.size(), info, e.data)) return false;
        out.push_back(std::move(e));
    }
    error.clear();
    return true;
}

}  // namespace

// =============================================================================================

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    auto parts = [](const std::wstring& v) {
        std::vector<int> p;
        size_t i = 0;
        while (i < v.size() && !iswdigit(v[i])) ++i;  // « v1.2.3 »
        while (i < v.size() && p.size() < 4) {
            int n = 0;
            while (i < v.size() && iswdigit(v[i])) n = n * 10 + (v[i++] - L'0');
            p.push_back(n);
            if (i < v.size() && v[i] == L'.') ++i;
            else break;
        }
        p.resize(4, 0);
        return p;
    };
    auto pa = parts(a), pb = parts(b);
    for (int i = 0; i < 4; ++i)
        if (pa[size_t(i)] != pb[size_t(i)]) return pa[size_t(i)] < pb[size_t(i)] ? -1 : 1;
    return 0;
}

UpdateStatus CheckForUpdate(const std::wstring& repo, const std::wstring& currentVersion, ReleaseInfo& out,
                            std::wstring& error) {
    if (repo.empty() || repo.find(L'/') == std::wstring::npos) {
        error = L"dépôt de mise à jour non configuré";
        return UpdateStatus::Error;
    }
    std::string body;
    if (!HttpGet(L"https://api.github.com/repos/" + repo + L"/releases/latest", L"application/vnd.github+json",
                 [&](const uint8_t* d, size_t n, uint64_t) {
                     body.append(reinterpret_cast<const char*>(d), n);
                     return body.size() < (8u << 20);
                 },
                 error))
        return UpdateStatus::Error;
    Json j;
    if (!JsonParser(body).Parse(j) || j.type != Json::Object) {
        error = L"réponse de GitHub illisible";
        return UpdateStatus::Error;
    }
    out = ReleaseInfo();
    out.tag = Utf8ToWide(j["tag_name"].s);
    out.version = out.tag;
    if (!out.version.empty() && (out.version[0] == L'v' || out.version[0] == L'V')) out.version.erase(0, 1);
    out.name = Utf8ToWide(j["name"].s);
    out.notes = Utf8ToWide(j["body"].s);
    out.page = Utf8ToWide(j["html_url"].s);
    // Archive Windows x64 : « *win64*.zip » de préférence, puis « *x64*.zip », jamais ARM
    auto score = [](const std::wstring& n) {
        if (n.find(L"arm") != std::wstring::npos) return -1;
        return n.find(L"win64") != std::wstring::npos ? 3 : n.find(L"x64") != std::wstring::npos ? 2 : 1;
    };
    int bestScore = 0;
    for (const Json& a : j["assets"].items) {
        std::wstring name = Utf8ToWide(a["name"].s), lname = Lower(name);
        if (lname.size() < 4 || lname.compare(lname.size() - 4, 4, L".zip") != 0) continue;
        int sc = score(lname);
        if (sc <= bestScore) continue;
        bestScore = sc;
        out.assetName = name;
        out.assetUrl = Utf8ToWide(a["browser_download_url"].s);
        out.assetSize = uint64_t(a["size"].n);
        std::string digest = a["digest"].s;  // « sha256:… » (fourni par GitHub pour les nouveaux fichiers)
        out.sha256 = digest.rfind("sha256:", 0) == 0 ? Lower(Utf8ToWide(digest.substr(7))) : L"";
    }
    if (out.version.empty()) {
        error = L"release sans numéro de version";
        return UpdateStatus::Error;
    }
    if (CompareVersions(out.version, currentVersion) <= 0) return UpdateStatus::UpToDate;
    if (out.assetUrl.empty()) {
        error = L"la release " + out.tag + L" ne contient pas d'archive .zip";
        return UpdateStatus::Error;
    }
    // Seules les adresses de téléchargement de GitHub sont acceptées
    if (out.assetUrl.rfind(L"https://github.com/", 0) != 0) {
        error = L"adresse de téléchargement inattendue";
        return UpdateStatus::Error;
    }
    return UpdateStatus::Available;
}

bool DownloadUpdate(const ReleaseInfo& rel, const std::wstring& destFile,
                    const std::function<bool(uint64_t, uint64_t)>& progress, std::wstring& error) {
    HANDLE f = CreateFileW(destFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = L"impossible de créer le fichier temporaire";
        return false;
    }
    uint64_t done = 0;
    bool ok = HttpGet(rel.assetUrl, L"application/octet-stream",
                      [&](const uint8_t* d, size_t n, uint64_t total) {
                          DWORD w = 0;
                          if (!WriteFile(f, d, DWORD(n), &w, nullptr) || w != n) {
                              error = L"écriture impossible (disque plein ?)";
                              return false;
                          }
                          done += n;
                          return progress(done, total ? total : rel.assetSize);
                      },
                      error);
    CloseHandle(f);
    if (ok && rel.assetSize && done != rel.assetSize) {
        error = L"téléchargement incomplet";
        ok = false;
    }
    if (ok && !rel.sha256.empty()) {
        std::wstring hex;
        if (!Sha256File(destFile, hex) || hex != rel.sha256) {
            error = L"somme de contrôle SHA-256 incorrecte : fichier corrompu ou modifié";
            ok = false;
        }
    }
    if (!ok) DeleteFileW(destFile.c_str());
    return ok;
}

bool InstallUpdate(const std::wstring& zipFile, const std::wstring& appDir, std::wstring& error) {
    // Lecture de l'archive
    std::vector<uint8_t> zip;
    {
        HANDLE f = CreateFileW(zipFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER sz{};
        if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &sz) || sz.QuadPart > (1ll << 30)) {
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            error = L"archive illisible";
            return false;
        }
        zip.resize(size_t(sz.QuadPart));
        DWORD got = 0;
        bool ok = ReadFile(f, zip.data(), DWORD(zip.size()), &got, nullptr) && got == zip.size();
        CloseHandle(f);
        if (!ok) {
            error = L"archive illisible";
            return false;
        }
    }
    std::vector<ZipEntry> entries;
    if (!ReadZip(zip, entries, error)) return false;

    // Dossier racine commun éventuel (« CyImgView-1.2.0-win64/... ») retiré
    std::string prefix;
    if (!entries.empty()) {
        size_t s = entries[0].name.find('/');
        if (s != std::string::npos) {
            prefix = entries[0].name.substr(0, s + 1);
            for (auto& e : entries)
                if (e.name.rfind(prefix, 0) != 0) prefix.clear();
        }
    }
    bool hasExe = false;
    for (auto& e : entries) {
        e.name.erase(0, prefix.size());
        if (_stricmp(e.name.c_str(), "CyImgView.exe") == 0) hasExe = true;
    }
    if (!hasExe) {
        error = L"l'archive ne contient pas CyImgView.exe";
        return false;
    }

    // Test d'écriture dans le dossier de l'application
    std::wstring probe = appDir + L"\\.cyimg-write-test";
    if (!WriteAll(probe, nullptr, 0)) {
        error = L"le dossier de l'application n'est pas accessible en écriture (" + appDir + L")";
        return false;
    }
    DeleteFileW(probe.c_str());

    // 1) écriture des nouveaux fichiers en .new
    std::vector<std::wstring> targets;
    for (auto& e : entries) {
        std::wstring rel = Utf8ToWide(e.name);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        std::wstring target = appDir + L"\\" + rel;
        if (!EnsureDir(ParentOf(target)) || !WriteAll(target + L".new", e.data.data(), e.data.size())) {
            for (auto& t : targets) DeleteFileW((t + L".new").c_str());
            error = L"écriture impossible : " + rel;
            return false;
        }
        targets.push_back(target);
    }
    // 2) bascule : ancien -> .old, .new -> nom définitif ; retour arrière en cas d'échec
    std::vector<std::pair<std::wstring, bool>> done;  // cible, avait un ancien fichier
    for (auto& t : targets) {
        bool existed = GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES;
        bool ok = (!existed || MoveFileExW(t.c_str(), (t + L".old").c_str(), MOVEFILE_REPLACE_EXISTING)) &&
                  MoveFileExW((t + L".new").c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!ok) {
            error = L"remplacement impossible : " + FileNameOf(t);
            if (existed && GetFileAttributesW(t.c_str()) == INVALID_FILE_ATTRIBUTES)
                MoveFileExW((t + L".old").c_str(), t.c_str(), 0);
            for (auto it = done.rbegin(); it != done.rend(); ++it) {
                DeleteFileW(it->first.c_str());
                if (it->second) MoveFileExW((it->first + L".old").c_str(), it->first.c_str(), 0);
            }
            for (auto& x : targets) DeleteFileW((x + L".new").c_str());
            return false;
        }
        done.emplace_back(t, existed);
    }
    DeleteFileW(zipFile.c_str());
    return true;
}

void CleanupUpdateLeftovers(const std::wstring& appDir) {
    for (const std::wstring& dir : {appDir, appDir + L"\\plugins"}) {
        for (const wchar_t* pat : {L"\\*.old", L"\\*.new"}) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());  // échoue sans dommage si encore utilisé
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
}
