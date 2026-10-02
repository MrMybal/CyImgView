#include "codecs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>

// =============================================================================================
// Oodle

namespace {

// OodleLZ_Decompress (oodle2.h, SDK 2.9) — 14 paramètres
using OodleDecompressFn = intptr_t(__stdcall*)(const void* comp, intptr_t compSize, void* raw, intptr_t rawLen,
                                               int fuzzSafe, int checkCrc, int verbosity, void* decBufBase,
                                               intptr_t decBufSize, void* callback, void* callbackData,
                                               void* decoderMemory, intptr_t decoderMemorySize, int threadPhase);

std::once_flag g_oodleOnce;
OodleDecompressFn g_oodle = nullptr;
std::wstring g_oodlePath;

bool TryLoad(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\oo2core_*_win64.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<std::wstring> names;
    do names.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.rbegin(), names.rend());  // version la plus récente d'abord
    for (auto& n : names) {
        std::wstring path = dir + L"\\" + n;
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) continue;
        if (auto fn = reinterpret_cast<OodleDecompressFn>(GetProcAddress(m, "OodleLZ_Decompress"))) {
            g_oodle = fn;
            g_oodlePath = path;
            return true;
        }
        FreeLibrary(m);
    }
    return false;
}

void CollectEngineDirs(HKEY root, const wchar_t* key, bool valuesArePaths, std::vector<std::wstring>& out) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return;
    wchar_t name[256], data[1024];
    if (valuesArePaths) {  // HKCU\...\Builds : valeurs = chemins des moteurs compilés depuis les sources
        for (DWORD i = 0;; ++i) {
            DWORD nl = 256, dl = sizeof(data), type = 0;
            if (RegEnumValueW(k, i, name, &nl, nullptr, &type, reinterpret_cast<BYTE*>(data), &dl) != ERROR_SUCCESS) break;
            if (type == REG_SZ) out.push_back(data);
        }
    } else {  // HKLM\...\Unreal Engine\<version>\InstalledDirectory
        std::vector<std::wstring> versions;
        for (DWORD i = 0;; ++i) {
            DWORD nl = 256;
            if (RegEnumKeyExW(k, i, name, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            versions.push_back(name);
        }
        std::sort(versions.rbegin(), versions.rend());
        for (auto& v : versions) {
            DWORD dl = sizeof(data);
            if (RegGetValueW(k, v.c_str(), L"InstalledDirectory", RRF_RT_REG_SZ, nullptr, data, &dl) == ERROR_SUCCESS)
                out.push_back(data);
        }
    }
    RegCloseKey(k);
}

void LoadOodle() {
    // 1) à côté du plugin (copie manuelle possible)
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadOodle), &self);
    wchar_t mod[MAX_PATH];
    if (self && GetModuleFileNameW(self, mod, MAX_PATH)) {
        std::wstring d = mod;
        d.resize(d.find_last_of(L'\\'));
        if (TryLoad(d)) return;
    }
    // 2) installations d'Unreal Engine (launcher, puis moteurs compilés)
    std::vector<std::wstring> engines;
    CollectEngineDirs(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EpicGames\\Unreal Engine", false, engines);
    CollectEngineDirs(HKEY_CURRENT_USER, L"SOFTWARE\\Epic Games\\Unreal Engine\\Builds", true, engines);
    static const wchar_t* rel[] = {
        L"\\Engine\\Binaries\\Win64",
        L"\\Engine\\Binaries\\ThirdParty\\Oodle\\Win64",
        L"\\Engine\\Binaries\\DotNET\\AutomationTool\\AutomationScripts\\BuildGraph",
        L"\\Engine\\Binaries\\DotNET\\AutomationTool\\AutomationScripts\\CookedEditor",
        L"\\Engine\\Binaries\\DotNET\\AutomationTool\\AutomationScripts\\Scripts",
        L"\\Engine\\Binaries\\DotNET\\AutomationTool",
    };
    for (auto& e : engines)
        for (const wchar_t* r : rel)
            if (TryLoad(e + r)) return;
}

}  // namespace

bool OodleAvailable(std::wstring* where) {
    std::call_once(g_oodleOnce, LoadOodle);
    if (where) *where = g_oodlePath;
    return g_oodle != nullptr;
}

bool OodleDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize) {
    if (!OodleAvailable()) return false;
    intptr_t r = g_oodle(src, intptr_t(srcSize), dst, intptr_t(rawSize), 1 /*FuzzSafe_Yes*/, 0 /*CheckCRC_No*/,
                         0 /*Verbosity_None*/, nullptr, 0, nullptr, nullptr, nullptr, 0, 3 /*Unthreaded*/);
    return r == intptr_t(rawSize);
}

// =============================================================================================
// LZ4 (format de bloc)

bool Lz4Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize) {
    size_t ip = 0, op = 0;
    while (ip < srcSize) {
        uint8_t token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= srcSize) return false;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (ip + lit > srcSize || op + lit > rawSize) return false;
        memcpy(dst + op, src + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= srcSize) break;  // dernière séquence : littéraux seulement
        if (ip + 2 > srcSize) return false;
        size_t off = size_t(src[ip]) | size_t(src[ip + 1]) << 8;
        ip += 2;
        if (off == 0 || off > op) return false;
        size_t len = (token & 15) + 4;
        if ((token & 15) == 15) {
            uint8_t b;
            do {
                if (ip >= srcSize) return false;
                b = src[ip++];
                len += b;
            } while (b == 255);
        }
        if (op + len > rawSize) return false;
        for (size_t i = 0; i < len; ++i, ++op) dst[op] = dst[op - off];  // recouvrement possible
    }
    return op == rawSize;
}

// =============================================================================================

float HalfToFloat(uint16_t h) {
    uint32_t sign = uint32_t(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF, bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {  // dénormalisé
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000 | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
