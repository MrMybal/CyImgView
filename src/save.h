#pragma once

#include "common.h"

// Format d'enregistrement : un encodeur WIC installé (JPEG, PNG, TIFF, BMP, GIF, JPEG XR, HEIF...).
struct SaveFormat {
    GUID container{};
    std::wstring name;                 // "JPEG"
    std::vector<std::wstring> exts;    // {"jpg", "jpeg", ...} en minuscules
    bool lossy = false;                // a une option de qualité
    bool alpha = false;                // conserve la transparence
};

// Encodeurs disponibles, les plus courants d'abord. Appel depuis un thread où COM est initialisé.
std::vector<SaveFormat> EnumerateSaveFormats();

struct SaveRequest {
    std::wstring path;
    int format = 0;       // index dans EnumerateSaveFormats()
    int quality = 92;     // 1..100, formats avec perte
};

// Boîte « Enregistrer sous » de Windows, avec choix de la qualité. preferred : extension proposée.
bool ShowSaveDialog(HWND owner, const std::vector<SaveFormat>& formats, const std::wstring& folder,
                    const std::wstring& baseName, const std::wstring& preferredExt, SaveRequest& out);

// Encode des pixels BGRA prémultipliés. Écrit dans un fichier temporaire puis remplace la cible :
// l'original n'est jamais laissé à moitié écrit.
bool SaveImage(const SaveRequest& req, const SaveFormat& fmt, const uint8_t* pixels, uint32_t width,
               uint32_t height, uint32_t stride, bool hasAlpha, std::wstring& error);

// Copie orientée (miroir + quarts de tour, comme l'affichage) d'une image BGRA.
bool OrientPixels(const uint8_t* src, uint32_t w, uint32_t h, uint32_t stride, bool flip, int rot,
                  PixelBuffer& dst, uint32_t& dw, uint32_t& dh, uint32_t& dstride);
