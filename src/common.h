#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

// Gros buffer de pixels : VirtualAlloc évite la mise à zéro et le tas fragmenté.
class PixelBuffer {
public:
    PixelBuffer() = default;
    PixelBuffer(const PixelBuffer&) = delete;
    PixelBuffer& operator=(const PixelBuffer&) = delete;
    ~PixelBuffer() { Free(); }

    bool Alloc(size_t n) {
        Free();
        m_data = static_cast<uint8_t*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        m_size = m_data ? n : 0;
        return m_data != nullptr;
    }
    void Free() {
        if (m_data) VirtualFree(m_data, 0, MEM_RELEASE);
        m_data = nullptr;
        m_size = 0;
    }
    void Swap(PixelBuffer& o) {
        std::swap(m_data, o.m_data);
        std::swap(m_size, o.m_size);
    }
    uint8_t* Data() const { return m_data; }
    size_t Size() const { return m_size; }

private:
    uint8_t* m_data = nullptr;
    size_t m_size = 0;
};

struct DecodedImage {
    uint32_t width = 0, height = 0, stride = 0;   // buffer de pixels
    uint32_t fullWidth = 0, fullHeight = 0;       // taille réelle de l'image (≠ si vignette)
    uint32_t orientation = 1;                     // EXIF 1..8
    bool hasAlpha = false;
    PixelBuffer pixels;                           // BGRA prémultiplié
    std::wstring format;
    std::vector<std::pair<std::wstring, std::wstring>> info;
    double decodeMs = 0;
    bool refinable = false;  // simple aperçu : une version complète peut être demandée
};
using ImagePtr = std::shared_ptr<DecodedImage>;

inline double NowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

// Traces de performance : actives si la variable d'environnement CYIMG_TRACE est définie.
// Écrit dans %TEMP%\cyimg_trace.txt le temps écoulé depuis la création du processus.
inline void Trace(const wchar_t* what) {
    static const bool on = GetEnvironmentVariableW(L"CYIMG_TRACE", nullptr, 0) > 0;
    if (!on) return;
    FILETIME created, unused, now;
    GetProcessTimes(GetCurrentProcess(), &created, &unused, &unused, &unused);
    GetSystemTimePreciseAsFileTime(&now);
    auto ticks = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    double ms = double(ticks(now) - ticks(created)) / 10000.0;
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"cyimg_trace.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%9.1f ms  %ls\n", ms, what);
        fclose(f);
    }
}

inline std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

inline std::wstring ExtensionOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) return {};
    return ToLower(path.substr(dot + 1));
}

inline std::wstring FileNameOf(const std::wstring& path) {
    size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

inline std::wstring ParentOf(const std::wstring& path) {
    size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring() : path.substr(0, sep);
}
