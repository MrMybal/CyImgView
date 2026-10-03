#include "edit_plugins.h"
#include "i18n.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

namespace editplug {

namespace {

int CrashFilter(EXCEPTION_POINTERS* ep, Crash* c) {
    c->code = ep->ExceptionRecord->ExceptionCode;
    c->addr = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

// Recopie d'une structure versionnée : struct_size du plugin, champs inconnus à zéro
template <class T>
bool CopyVersioned(const T* src, T& dst) {
    dst = T{};
    if (!src) return false;
    const uint32_t n = src->struct_size;
    if (n < offsetof(T, struct_size) + sizeof(uint32_t) * 2) return false;
    memcpy(&dst, src, std::min<size_t>(n, sizeof(T)));
    dst.struct_size = uint32_t(sizeof(T));
    return true;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    while (a <= s.size()) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    return out;
}

std::vector<Param> ReadParams(const CyImgParam* arr, uint32_t count) {
    std::vector<Param> out;
    if (!arr || !count) return out;
    const uint32_t stride = arr->struct_size;
    if (stride < offsetof(CyImgParam, unit)) return out;  // tableau illisible
    for (uint32_t i = 0; i < count && i < 256; ++i) {
        const auto* src = reinterpret_cast<const CyImgParam*>(reinterpret_cast<const uint8_t*>(arr) + size_t(i) * stride);
        Param p;
        CopyVersioned(src, p.p);
        p.label = Text(p.p.key, p.p.name);
        if (p.p.unit) p.unit = p.p.unit;
        if (p.p.type == CYIMG_PARAM_CHOICE) {
            std::wstring list = p.p.choices ? p.p.choices : L"";
            if (p.p.key) list = TrOr((std::string(p.p.key) + ".choices").c_str(), list.c_str());
            p.choices = Split(list, L'|');
            p.p.min = 0;
            p.p.max = double(std::max<size_t>(1, p.choices.size()) - 1);
            p.p.step = 1;
        } else if (p.p.type == CYIMG_PARAM_CHECK) {
            p.p.min = 0;
            p.p.max = 1;
        } else if (p.p.type == CYIMG_PARAM_COLOR) {
            p.p.min = 0;
            p.p.max = 0xFFFFFF;
        }
        if (p.p.max < p.p.min) std::swap(p.p.min, p.p.max);
        p.p.def = std::clamp(p.p.def, p.p.min, p.p.max);
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<std::wstring> SplitExts(const wchar_t* s) {
    std::vector<std::wstring> r;
    if (!s) return r;
    for (std::wstring e : Split(s, L';')) {
        while (!e.empty() && (e.front() == L'.' || e.front() == L' ' || e.front() == L'*')) e.erase(0, 1);
        while (!e.empty() && e.back() == L' ') e.pop_back();
        if (!e.empty()) r.push_back(ToLower(e));
    }
    return r;
}

std::mutex g_jobsLock;
std::vector<JobPtr> g_finished;

}  // namespace

std::wstring Crash::Where() const {
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod)) {
        wchar_t full[MAX_PATH];
        if (GetModuleFileNameW(mod, full, MAX_PATH)) wcscpy_s(name, FileNameOf(full).c_str());
    }
    wchar_t buf[MAX_PATH + 64];
    swprintf_s(buf, L"%ls+0x%llX, %08lX", name,
               static_cast<unsigned long long>(static_cast<const char*>(addr) - reinterpret_cast<const char*>(mod)), code);
    return buf;
}

// Pas d'objet C++ à détruire dans cette fonction (contrainte de __try)
bool GuardedInvoke(void (*fn)(void*), void* arg, Crash& crash) {
    __try {
        fn(arg);
        return true;
    } __except (CrashFilter(GetExceptionInformation(), &crash)) {
        if (crash.code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return false;
    }
}

std::wstring Text(const char* key, const wchar_t* def) {
    if (!def) def = L"";
    return key && *key ? TrOr(key, def) : def;
}

std::wstring Plugin::Name() const {
    const std::wstring n = Text(api.key, api.name);
    return n.empty() ? file : n;
}

std::wstring GlyphString(uint32_t cp) {
    std::wstring s;
    if (!cp || cp > 0x10FFFF) return s;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        s += wchar_t(0xD800 + (cp >> 10));
        s += wchar_t(0xDC00 + (cp & 0x3FF));
    } else {
        s += wchar_t(cp);
    }
    return s;
}

std::vector<double> Defaults(const std::vector<Param>& params) {
    std::vector<double> v;
    for (const Param& p : params) v.push_back(p.p.def);
    return v;
}

Registry& Get() {
    static Registry r;
    return r;
}

void Load(const std::wstring& exeDir, const CyImgEditHost* host) {
    Registry& reg = Get();
    if (reg.loaded) return;
    reg.loaded = true;
    const double t0 = NowSeconds();
    const std::wstring dir = exeDir + L"\\plugins\\";
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileExW((dir + L"*.dll").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring file = dir + fd.cFileName;
        // Déjà chargée par le décodeur (même DLL) : simple compteur de références
        HMODULE mod = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod) continue;
        auto get = reinterpret_cast<CyImgGetEditPluginFn>(GetProcAddress(mod, CYIMG_EDIT_PLUGIN_ENTRY));
        const CyImgEditPlugin* api = nullptr;
        Crash crash;
        if (get && !Guard([&] { api = get(host); }, crash)) {
            Trace((L"plugin d'édition planté au chargement : " + std::wstring(fd.cFileName) + L" @ " + crash.Where()).c_str());
            continue;  // la DLL reste chargée : du code à elle peut encore tourner
        }
        auto p = std::make_unique<Plugin>();
        if (!api || api->api_version != CYIMG_EDIT_API_VERSION || !CopyVersioned(api, p->api)) {
            FreeLibrary(mod);
            continue;
        }
        p->module = mod;
        p->file = fd.cFileName;
        const Plugin* pl = p.get();
        const CyImgEditPlugin& a = p->api;
        for (uint32_t i = 0; a.effects && i < a.effect_count && i < 256; ++i) {
            Effect e;
            e.plugin = pl;
            if (!CopyVersioned(a.effects[i], e.api) || !e.api.process) continue;
            e.name = Text(e.api.key, e.api.name);
            e.category = e.api.category || e.api.category_key ? Text(e.api.category_key, e.api.category) : pl->Name();
            e.params = ReadParams(e.api.params, e.api.param_count);
            reg.effects.push_back(std::move(e));
        }
        for (uint32_t i = 0; a.tools && i < a.tool_count && i < 64; ++i) {
            Tool t;
            t.plugin = pl;
            if (!CopyVersioned(a.tools[i], t.api)) continue;
            t.name = Text(t.api.key, t.api.name);
            t.hint = Text(t.api.hint_key, t.api.hint);
            t.glyph = GlyphString(t.api.glyph);
            t.params = ReadParams(t.api.params, t.api.param_count);
            t.values = Defaults(t.params);
            reg.tools.push_back(std::move(t));
        }
        for (uint32_t i = 0; a.modes && i < a.mode_count && i < 16; ++i) {
            Mode m;
            m.plugin = pl;
            if (!CopyVersioned(a.modes[i], m.api) || !m.api.process) continue;
            m.name = Text(m.api.key, m.api.name);
            m.hint = Text(m.api.hint_key, m.api.hint);
            m.glyph = GlyphString(m.api.glyph);
            m.controls = ReadParams(m.api.controls, m.api.control_count);
            reg.modes.push_back(std::move(m));
        }
        for (uint32_t i = 0; a.actions && i < a.action_count && i < 64; ++i) {
            Action ac;
            ac.plugin = pl;
            if (!CopyVersioned(a.actions[i], ac.api) || (!ac.api.run && !ac.api.program)) continue;
            ac.name = Text(ac.api.key, ac.api.name);
            ac.exts = SplitExts(ac.api.extensions);
            reg.actions.push_back(std::move(ac));
        }
        reg.plugins.push_back(std::move(p));
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    wchar_t tr[96];
    swprintf_s(tr, L"plugins d'édition chargés (%zu) en %.1f ms", reg.plugins.size(), (NowSeconds() - t0) * 1000);
    Trace(tr);
}

// =============================================================================================
// Calculs

void RunJob(Job& j, const CyImgEditHost* host) {
    const double t0 = NowSeconds();
    const size_t bytes = size_t(j.stride) * j.height;
    if (j.cancel.load()) {
        j.result = CYIMG_E_CANCELLED;
    } else if (!j.src || !j.src->Data() || !j.process || !j.dst.Alloc(bytes)) {
        j.result = CYIMG_E_OUTOFMEMORY;
    } else {
        memcpy(j.dst.Data(), j.src->Data(), bytes);
        CyImgImage img{};
        img.struct_size = sizeof(img);
        img.data = j.dst.Data();
        img.source = j.src->Data();
        img.width = j.width;
        img.height = j.height;
        img.stride = j.stride;
        img.sel = j.sel;
        img.scale = j.scale;
        img.full_width = j.fullWidth;
        img.full_height = j.fullHeight;
        CyImgEditCtx ctx;  // calcul : seuls translate() et is_cancelled() sont utilisables
        ctx.job = &j;
        int r = CYIMG_E_FAIL;
        const double* vals = j.values.empty() ? nullptr : j.values.data();
        if (!Guard([&] { r = j.process(host, &ctx, &img, vals); }, j.crash)) j.crashed = true;
        j.result = j.cancel.load() && r == CYIMG_OK ? CYIMG_E_CANCELLED : r;
    }
    j.ms = (NowSeconds() - t0) * 1000;
}

void StartJob(const JobPtr& job, const CyImgEditHost* host, HWND hwnd, UINT msg) {
    std::thread([job, host, hwnd, msg] {
        RunJob(*job, host);
        {
            std::lock_guard<std::mutex> lock(g_jobsLock);
            g_finished.push_back(job);
        }
        PostMessageW(hwnd, msg, 0, 0);
    }).detach();
}

void DispatchJobs() {
    std::vector<JobPtr> done;
    {
        std::lock_guard<std::mutex> lock(g_jobsLock);
        done.swap(g_finished);
    }
    for (auto& j : done)
        if (j->done) j->done(*j);
}

}  // namespace editplug
