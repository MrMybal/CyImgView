// Plugin CyImgView « docs » : outils partagés entre les lecteurs de formats.
#pragma once

#include "cyimg_plugin_i18n.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace docs {

// Fichier projeté en mémoire en lecture seule.
struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool Open(const wchar_t* path);
    ~MappedFile();
};

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// Contexte d'un décodage : hôte + messages.
struct Job {
    const CyImgHost* host;
    CyImgContext* ctx;
    int Fail(const wchar_t* msg) const {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg);
        return CYIMG_E_FAIL;
    }
    void Info(const wchar_t* k, const wchar_t* v) const { host->set_info(ctx, k, v); }
    uint32_t Target() const { return host->target_size(ctx); }
    bool Cancelled() const { return host->is_cancelled(ctx) != 0; }
    // Confie un aperçu encodé (PNG, JPEG, BMP...) à l'hôte et le signale dans le panneau d'infos.
    int Preview(const uint8_t* data, size_t size, const wchar_t* what) const;
};

// Taille d'un PNG commençant en p (parcours des blocs jusqu'à IEND), 0 si invalide.
size_t PngLength(const uint8_t* p, size_t avail);
// Dimensions d'un PNG (bloc IHDR) ; false si ce n'en est pas un.
bool PngSize(const uint8_t* p, size_t avail, uint32_t& w, uint32_t& h);
// Plus grand PNG complet trouvé dans [p, p+n) (signature puis blocs) ; false si aucun.
bool FindLargestPng(const uint8_t* p, size_t n, size_t& off, size_t& len);

// Formats
int DecodeXcf(const Job& job, const uint8_t* f, size_t n);
int DecodeClip(const Job& job, const uint8_t* f, size_t n);
int DecodeEps(const Job& job, const uint8_t* f, size_t n);
int DecodeIndd(const Job& job, const uint8_t* f, size_t n);
int DecodeCdr(const Job& job, const uint8_t* f, size_t n);
int DecodeAffinity(const Job& job, const uint8_t* f, size_t n);
int DecodeZipDoc(const Job& job, const uint8_t* f, size_t n, const std::wstring& ext);

}  // namespace docs
