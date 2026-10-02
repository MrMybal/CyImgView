#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Lecture positionnée d'un fichier (sans état partagé : utilisable depuis plusieurs threads).
class RawFile {
public:
    explicit RawFile(const wchar_t* path) {
        m_h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        LARGE_INTEGER sz{};
        if (m_h != INVALID_HANDLE_VALUE && GetFileSizeEx(m_h, &sz)) m_size = uint64_t(sz.QuadPart);
    }
    ~RawFile() {
        if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h);
    }
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;

    bool Ok() const { return m_h != INVALID_HANDLE_VALUE && m_size > 16; }
    uint64_t Size() const { return m_size; }

    bool Read(uint64_t off, void* dst, size_t n) const {
        if (off > m_size || n > m_size - off) return false;
        auto* p = static_cast<uint8_t*>(dst);
        while (n) {
            OVERLAPPED ov{};
            ov.Offset = DWORD(off);
            ov.OffsetHigh = DWORD(off >> 32);
            DWORD chunk = DWORD(std::min<size_t>(n, 64u << 20)), got = 0;
            if (!ReadFile(m_h, p, chunk, &got, &ov) || got == 0) return false;
            p += got;
            off += got;
            n -= got;
        }
        return true;
    }

private:
    HANDLE m_h = INVALID_HANDLE_VALUE;
    uint64_t m_size = 0;
};

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// Aperçu JPEG embarqué dans un RAW
struct Preview {
    uint64_t off = 0;
    uint32_t len = 0;
    uint32_t w = 0, h = 0;
};

struct RawScan {
    const wchar_t* container = nullptr;  // L"TIFF", L"CR3", L"RAF", L"CRW" ; nullptr si non reconnu
    std::vector<Preview> previews;
    uint32_t orientation = 0;            // EXIF 1..8, 0 = inconnue
    std::vector<std::pair<std::wstring, std::wstring>> info;  // EXIF lu dans la structure TIFF
};

// Analyse la structure du fichier et liste les aperçus JPEG exploitables (rapide : quelques lectures).
void ScanRaw(const RawFile& f, RawScan& out);
