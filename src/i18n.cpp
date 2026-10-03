#include "i18n.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

struct Table {
    std::wstring code, name;
    std::unordered_map<std::string, std::wstring> strings;
};

std::vector<std::unique_ptr<Table>> g_tables;  // construit par Init(), ensuite en lecture seule
std::atomic<int> g_current{-1};
int g_en = -1, g_fr = -1;

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::wstring Unescape(std::wstring v) {
    std::wstring out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == L'\\' && i + 1 < v.size()) {
            wchar_t c = v[++i];
            out += c == L'n' ? L'\n' : c == L't' ? L'\t' : c;
        } else {
            out += v[i];
        }
    }
    return out;
}

Table& TableFor(const std::wstring& code) {
    for (auto& t : g_tables)
        if (t->code == code) return *t;
    g_tables.push_back(std::make_unique<Table>());
    g_tables.back()->code = code;
    return *g_tables.back();
}

// Un fichier .lang : en-tête @code (obligatoire) puis clé = valeur. Les clés déjà présentes sont
// remplacées (les fichiers du dossier « lang » passent après les ressources intégrées).
void Parse(std::string_view text) {
    if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF)
        text.remove_prefix(3);
    Table* t = nullptr;
    std::wstring pendingName;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = Trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
        if (key == "@code") {
            t = &TableFor(Utf8ToWide(value));
            if (!pendingName.empty() && t->name.empty()) t->name = pendingName;
        } else if (key == "@name") {
            pendingName = Utf8ToWide(value);
            if (t && t->name.empty()) t->name = pendingName;
        } else if (t && !key.empty()) {
            t->strings[std::string(key)] = Unescape(Utf8ToWide(value));
        }
    }
}

BOOL CALLBACK OnLangResource(HMODULE mod, LPCWSTR type, LPWSTR name, LONG_PTR) {
    HRSRC res = FindResourceW(mod, name, type);
    HGLOBAL mem = res ? LoadResource(mod, res) : nullptr;
    const char* data = mem ? static_cast<const char*>(LockResource(mem)) : nullptr;
    if (data) Parse(std::string_view(data, SizeofResource(mod, res)));
    return TRUE;
}

void LoadFolder(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.lang").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        HANDLE f = CreateFileW((dir + L"\\" + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size{};
        if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
            std::string buf(size_t(size.QuadPart), '\0');
            DWORD read = 0;
            if (ReadFile(f, buf.data(), DWORD(buf.size()), &read, nullptr)) Parse(std::string_view(buf.data(), read));
        }
        CloseHandle(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int IndexOf(const std::wstring& code) {
    for (size_t i = 0; i < g_tables.size(); ++i)
        if (g_tables[i]->code == code) return int(i);
    return -1;
}

const std::wstring* Find(int table, const char* key) {
    if (table < 0) return nullptr;
    auto& m = g_tables[size_t(table)]->strings;
    auto it = m.find(key);
    return it == m.end() ? nullptr : &it->second;
}

const std::wstring* Lookup(const char* key) {
    const int cur = g_current.load(std::memory_order_relaxed);
    if (const std::wstring* s = Find(cur, key)) return s;
    if (cur != g_en)
        if (const std::wstring* s = Find(g_en, key)) return s;
    if (cur != g_fr)
        if (const std::wstring* s = Find(g_fr, key)) return s;
    return nullptr;
}

}  // namespace

namespace i18n {

void Init(const std::wstring& exeDir, const std::wstring& preferred) {
    if (!g_tables.empty()) return;
    EnumResourceNamesW(GetModuleHandleW(nullptr), L"LANG", OnLangResource, 0);
    if (!exeDir.empty()) LoadFolder(exeDir + L"\\lang");
    for (auto& t : g_tables)
        if (t->name.empty()) t->name = t->code;
    g_en = IndexOf(L"en");
    g_fr = IndexOf(L"fr");
    SetLanguage(preferred);
}

std::vector<Language> Languages() {
    std::vector<Language> v;
    for (auto& t : g_tables) v.push_back({t->code, t->name});
    std::sort(v.begin(), v.end(), [](const Language& a, const Language& b) {
        return CompareStringEx(LOCALE_NAME_INVARIANT, LINGUISTIC_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1,
                               nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return v;
}

std::wstring CurrentCode() {
    const int cur = g_current.load();
    return cur >= 0 ? g_tables[size_t(cur)]->code : std::wstring(L"fr");
}

std::wstring SystemCode() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    // Langue de l'interface de Windows (« fr-FR », « pt-BR », « zh-CN »...)
    ULONG count = 0, size = ARRAYSIZE(name);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, name, &size) || !name[0])
        GetUserDefaultLocaleName(name, ARRAYSIZE(name));
    std::wstring full = name, primary = full.substr(0, full.find(L'-'));
    for (auto& c : full) c = towlower(c);
    for (auto& c : primary) c = towlower(c);
    if (IndexOf(full) >= 0) return full;  // variante exacte disponible (ex. « pt-br »)
    if (IndexOf(primary) >= 0) return primary;
    return IndexOf(L"en") >= 0 ? L"en" : L"fr";
}

void SetLanguage(const std::wstring& code) {
    int i = code.empty() ? -1 : IndexOf(code);
    if (i < 0) i = IndexOf(SystemCode());
    g_current.store(i);
}

}  // namespace i18n

const wchar_t* TrOr(const char* key, const wchar_t* fallback) {
    const std::wstring* s = Lookup(key);
    return s ? s->c_str() : fallback;
}

const wchar_t* T(const char* key) {
    if (const std::wstring* s = Lookup(key)) return s->c_str();
    // Clé absente partout : on affiche la clé (repérable pendant le développement)
    static std::mutex m;
    static std::unordered_set<std::wstring> missing;
    std::lock_guard<std::mutex> lock(m);
    return missing.insert(std::wstring(key, key + strlen(key))).first->c_str();
}

std::wstring Tf(const char* key, ...) {
    const wchar_t* fmt = T(key);
    va_list args;
    va_start(args, key);
    va_list copy;
    va_copy(copy, args);
    int n = _vscwprintf(fmt, copy);
    va_end(copy);
    std::wstring out;
    if (n > 0) {
        out.resize(size_t(n) + 1);
        _vsnwprintf_s(out.data(), out.size(), _TRUNCATE, fmt, args);
        out.resize(size_t(n));
    }
    va_end(args);
    return out;
}
