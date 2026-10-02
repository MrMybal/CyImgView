#pragma once

// Petits utilitaires partagés par viewer.cpp et viewer_edit.cpp

#include <d2d1_1.h>
#include <windows.h>

#include <cstdint>

namespace ui {

constexpr UINT kMsgUpdate = WM_APP + 4;     // résultat d'une recherche de mise à jour (lParam = UpdateResult*)
constexpr UINT_PTR kTimerUpdate = 2;         // recherche automatique, quelques secondes après le démarrage

constexpr uint32_t kBg = 0x111214;
constexpr uint32_t kAccent = 0x4AA8FF;
constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kGrey = 0xA8ADB5;
constexpr uint32_t kPanel = 0x1C1D21;

inline D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

inline bool Contains(const D2D1_RECT_F& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

}  // namespace ui
