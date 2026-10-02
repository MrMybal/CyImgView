#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Flux deflate brut de taille décompressée inconnue (DICOM « Deflated »). maxOut : garde-fou.
bool InflateUnknownSize(const uint8_t* src, size_t size, size_t maxOut, std::vector<uint8_t>& out);

// RLE Lossless DICOM (PS3.5 annexe G) : un fragment = une image. Sortie au format natif
// (échantillons entrelacés, petit-boutiste) : rows * cols * samples * bytesPerSample octets.
bool DecodeDicomRle(const uint8_t* src, size_t size, uint32_t rows, uint32_t cols, uint32_t samples,
                    uint32_t bytesPerSample, std::vector<uint8_t>& out, std::wstring& why);

// JPEG sans perte (processus 14, SOF3). Sortie native : composantes entrelacées, 1 octet par
// échantillon si bytesPerSample == 1, sinon 2 (petit-boutiste).
bool DecodeLosslessJpeg(const uint8_t* src, size_t size, uint32_t bytesPerSample, std::vector<uint8_t>& out,
                        uint32_t& width, uint32_t& height, uint32_t& components, uint32_t& precision, std::wstring& why);
