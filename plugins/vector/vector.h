// Plugin CyImgView « vector » : déclarations communes aux différents formats vectoriels.
#pragma once

#include "cyimg_plugin_i18n.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vec {

using Microsoft::WRL::ComPtr;

// Tailles de rendu : un format vectoriel est rendu net, au moins kMinFull px sur le plus grand côté.
constexpr double kMinFull = 2048, kMaxFull = 8192;

// Fabriques partagées (créées une seule fois, utilisables depuis plusieurs threads). nullptr si indisponible.
ID2D1Factory1* D2DFactory();
IWICImagingFactory* WicFactory();
IDWriteFactory* DWriteFactory();

// Contexte d'appel : hôte + gestion des erreurs.
struct Call {
    const CyImgHost* host;
    CyImgContext* ctx;
    int Fail(const wchar_t* msg) const {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg);
        return CYIMG_E_FAIL;
    }
    bool Cancelled() const { return host->is_cancelled && host->is_cancelled(ctx); }
    uint32_t Target() const { return host->target_size ? host->target_size(ctx) : 0; }
    void Info(const wchar_t* k, const std::wstring& v) const { host->set_info(ctx, k, v.c_str()); }
};

// Taille de rendu (px) d'une image de taille naturelle w x h (unités quelconques, > 0).
void RenderSize(const Call& c, double w, double h, uint32_t& outW, uint32_t& outH);

// Cible de rendu logicielle (bitmap WIC 32 bits BGRA prémultiplié) avec contexte Direct2D.
struct WicTarget {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> rt;
    ComPtr<ID2D1DeviceContext> dc;  // nullptr si l'interface n'est pas disponible
    uint32_t w = 0, h = 0;
    bool Create(uint32_t width, uint32_t height);
    // Copie le résultat vers l'hôte (alpha détecté si forceOpaque est faux).
    int Deliver(const Call& c, bool forceOpaque);
};

// Copie des pixels BGRA prémultipliés vers l'hôte. has_alpha déduit du contenu.
int DeliverPixels(const Call& c, const uint8_t* px, uint32_t stride, uint32_t w, uint32_t h, bool forceOpaque);

// Lecture et décompression gzip (svgz, emz, wmz).
bool ReadAll(const wchar_t* path, std::vector<uint8_t>& out, size_t maxSize = size_t(1) << 30);
bool IsGzip(const std::vector<uint8_t>& d);
bool Gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);

std::wstring Fmt(const wchar_t* f, ...);

// Décodeurs (data = contenu du fichier, déjà décompressé si gzip)
int DecodeSvg(const Call& c, const std::vector<uint8_t>& data, bool gz);
int DecodeMetafile(const Call& c, const std::vector<uint8_t>& data, bool gz);  // EMF / WMF
int DecodePdf(const Call& c, const wchar_t* path, bool ai);
int DecodeDxf(const Call& c, const std::vector<uint8_t>& data);

bool IsEmf(const uint8_t* p, size_t n);
bool IsWmf(const uint8_t* p, size_t n);

}  // namespace vec
