#pragma once

// Petits utilitaires partagés par viewer.cpp et viewer_edit.cpp

#include <d2d1_1.h>
#include <windows.h>

#include <cstdint>

namespace ui {

constexpr UINT kMsgUpdate = WM_APP + 4;     // résultat d'une recherche de mise à jour (lParam = UpdateResult*)
constexpr UINT_PTR kTimerUpdate = 2;         // recherche automatique, quelques secondes après le démarrage
constexpr UINT_PTR kTimerSlide = 4;          // diaporama : image suivante
constexpr UINT_PTR kTimerAnim = 3;           // image suivante d'une animation
constexpr UINT kMsgOpenPath = WM_APP + 5;    // fichier transmis par une autre instance (lParam = std::wstring*)
constexpr ULONG_PTR kCopyDataOpen = 0x43594956;  // 'CYIV' : WM_COPYDATA « ouvre ce fichier »

constexpr uint32_t kBg = 0x111214;
constexpr uint32_t kAccent = 0x4AA8FF;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kGrey = 0xA8ADB5;
constexpr uint32_t kPanel = 0x1C1D21;

inline D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// Matrice image -> écran : centre l'image, miroir, quart(s) de tour exact(s), zoom, translation.
inline D2D1::Matrix3x2F ImageTransform(float w, float h, bool flip, int rot, float zoom, float tx, float ty) {
    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Translation(-w / 2, -h / 2);
    if (flip) m = m * D2D1::Matrix3x2F::Scale(-1, 1);
    switch (rot & 3) {
    case 1: m = m * D2D1::Matrix3x2F(0, 1, -1, 0, 0, 0); break;
    case 2: m = m * D2D1::Matrix3x2F(-1, 0, 0, -1, 0, 0); break;
    case 3: m = m * D2D1::Matrix3x2F(0, -1, 1, 0, 0, 0); break;
    default: break;
    }
    return m * D2D1::Matrix3x2F::Scale(zoom, zoom) * D2D1::Matrix3x2F::Translation(tx, ty);
}

inline bool Contains(const D2D1_RECT_F& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

}  // namespace ui
