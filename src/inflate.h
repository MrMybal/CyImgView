#pragma once

#include <cstddef>
#include <cstdint>

// Flux deflate brut (RFC 1951, ex. entrées d'une archive zip). dst doit faire exactement rawSize.
bool InflateRaw(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize);

// Flux zlib (RFC 1950) : en-tête de 2 octets + deflate.
bool ZlibDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize);
