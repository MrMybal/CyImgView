#include "zip.h"

#include "inflate.h"

#include <algorithm>
#include <cstring>

namespace {
uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace

bool ZipList(const uint8_t* z, size_t size, std::vector<ZipEntryInfo>& out) {
    out.clear();
    if (size < 22) return false;
    size_t eocd = size_t(-1);
    for (size_t i = size - 22 + 1; i-- > 0 && size - i < 65557;)
        if (Rd32(z + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == size_t(-1)) return false;
    const uint16_t count = Rd16(z + eocd + 10);
    size_t cd = Rd32(z + eocd + 16);
    for (uint16_t k = 0; k < count; ++k) {
        if (cd + 46 > size || Rd32(z + cd) != 0x02014b50) return false;
        ZipEntryInfo e;
        e.method = Rd16(z + cd + 10);
        const uint16_t flags = Rd16(z + cd + 8);
        e.compressedSize = Rd32(z + cd + 20);
        e.size = Rd32(z + cd + 24);
        const uint16_t nlen = Rd16(z + cd + 28), xlen = Rd16(z + cd + 30), clen = Rd16(z + cd + 32);
        e.localHeader = Rd32(z + cd + 42);
        if (cd + 46 + nlen > size) return false;
        e.name.assign(reinterpret_cast<const char*>(z + cd + 46), nlen);
        cd += 46 + size_t(nlen) + xlen + clen;
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        if (e.name.empty() || e.name.back() == '/' || (flags & 1)) continue;  // dossier ou chiffré
        out.push_back(std::move(e));
    }
    return true;
}

bool ZipExtract(const uint8_t* z, size_t size, const ZipEntryInfo& e, std::vector<uint8_t>& out) {
    if (e.localHeader + 30 > size || Rd32(z + e.localHeader) != 0x04034b50 || e.size > (1u << 30)) return false;
    const size_t data = e.localHeader + 30 + size_t(Rd16(z + e.localHeader + 26)) + Rd16(z + e.localHeader + 28);
    if (data + e.compressedSize > size) return false;
    out.resize(e.size);
    if (e.method == 0) {
        if (e.compressedSize != e.size) return false;
        memcpy(out.data(), z + data, e.size);
        return true;
    }
    if (e.method == 8) return InflateRaw(z + data, e.compressedSize, out.data(), e.size);
    return false;
}

bool ZipExtractByName(const uint8_t* z, size_t size, const std::string& name, std::vector<uint8_t>& out) {
    std::vector<ZipEntryInfo> list;
    if (!ZipList(z, size, list)) return false;
    for (auto& e : list)
        if (_stricmp(e.name.c_str(), name.c_str()) == 0) return ZipExtract(z, size, e, out);
    return false;
}
