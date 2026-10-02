#pragma once

#include <dxgiformat.h>

#include <cstdint>
#include <string>
#include <vector>

// Décompression des formats GPU par blocs (BC1 à BC7) par Direct3D 11 (WARP, rendu logiciel de
// Windows) : un shader lit chaque texel de la texture compressée et l'écrit dans une cible non
// compressée. Résultat identique à celui d'un GPU, sans tables de décodage recopiées à la main.
//
// out : RGBA8 (4 octets/pixel) ou RGBA32F (16 octets/pixel) si floatOut, dense (w * taille pixel).
// data : blocs de la mip, lignes de blocs consécutives (largeur et hauteur arrondies à 4).
bool GpuDecodeBlocks(DXGI_FORMAT format, uint32_t width, uint32_t height, const uint8_t* data, size_t size,
                     bool floatOut, std::vector<uint8_t>& out, std::wstring& error);

// Octets par bloc 4x4 (0 si le format n'est pas compressé par blocs)
uint32_t BlockBytes(DXGI_FORMAT format);
