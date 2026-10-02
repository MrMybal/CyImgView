#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Décompresseurs utilisés par les packages Unreal.

// Oodle (Kraken, Mermaid...) : chargé dynamiquement depuis une installation d'Unreal Engine
// (oo2core_*_win64.dll), ou depuis le dossier du plugin. Retourne false si indisponible.
bool OodleAvailable(std::wstring* where = nullptr);
bool OodleDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize);

// Bloc LZ4 brut (sans en-tête de trame).
bool Lz4Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize);

// zlib / deflate : voir src/inflate.h (partagé avec l'application)
#include "inflate.h"

// Demi-flottant IEEE -> float
float HalfToFloat(uint16_t h);
