// Plugin CyImgView « Unreal .uasset » : textures des projets Unreal Engine (UE 4.x à 5.8, non cookées).
//
//  - Identification : on lit les tables du package (noms, imports, exports) et la classe de l'asset
//    principal (Texture2D, TextureCube...). CyImg_Accept masque les autres .uasset d'un dossier.
//  - Vignette : la miniature enregistrée par l'éditeur (table des miniatures, PNG ou JPEG).
//  - Pleine résolution : les pixels source (FTextureSource) — FEditorBulkData + FCompressedBuffer
//    (Oodle/LZ4) en UE5, FByteBulkData (brut ou zlib) en UE4, puis PNG/JPEG/UEDELTA/brut.
// Référence : sources du moteur (PackageFileSummary.cpp, ObjectResource.cpp, PropertyTag.cpp,
// Texture.cpp, EditorBulkData.cpp, PackageTrailer.cpp, CompressedBuffer.cpp, ImageCoreDelta.cpp).

#include "cyimg_plugin.h"
#include "codecs.h"
#include "gpu_decode.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kPackageTag = 0x9E2A83C1;
constexpr uint32_t PKG_Cooked = 0x200, PKG_UnversionedProperties = 0x2000, PKG_FilterEditorOnly = 0x80000000;
const uint32_t kUE5MainGuid[4] = {0x697DD581, 0xE64F41AB, 0xAA4A51EC, 0xBEB7B628};
constexpr int kTextureSourceVirtualization = 42;  // FUE5MainStreamObjectVersion

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// --- Lecture bornée d'un tampon ---------------------------------------------------------------

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : m_p(p), m_n(n) {}
    size_t Pos() const { return m_pos; }
    size_t Size() const { return m_n; }
    const uint8_t* Data() const { return m_p; }
    void Seek(uint64_t o) {
        if (o > m_n) throw ParseError("seek");
        m_pos = size_t(o);
    }
    void Skip(uint64_t k) { Seek(m_pos + k); }
    const uint8_t* Take(size_t k) {
        Need(k);
        const uint8_t* r = m_p + m_pos;
        m_pos += k;
        return r;
    }
    uint8_t U8() { return *Take(1); }
    uint16_t U16() { uint16_t v; memcpy(&v, Take(2), 2); return v; }
    uint32_t U32() { uint32_t v; memcpy(&v, Take(4), 4); return v; }
    int32_t I32() { return int32_t(U32()); }
    uint64_t U64() { uint64_t v; memcpy(&v, Take(8), 8); return v; }
    int64_t I64() { return int64_t(U64()); }
    uint32_t BE32() { const uint8_t* b = Take(4); return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
    uint64_t BE64() { uint64_t hi = BE32(); return hi << 32 | BE32(); }
    void Guid(uint32_t g[4]) { for (int i = 0; i < 4; ++i) g[i] = U32(); }

    // FString : longueur > 0 = ANSI (avec \0), < 0 = UTF-16
    std::string FString() {
        int32_t n = I32();
        if (n == 0) return {};
        if (n > 0) {
            if (n > (1 << 20)) throw ParseError("string");
            const char* s = reinterpret_cast<const char*>(Take(size_t(n)));
            return std::string(s, strnlen(s, size_t(n)));
        }
        if (n < -(1 << 20)) throw ParseError("string");
        size_t cnt = size_t(-int64_t(n));
        const uint8_t* b = Take(cnt * 2);
        std::wstring w(cnt, L'\0');
        memcpy(w.data(), b, cnt * 2);
        w.resize(wcsnlen(w.c_str(), cnt));
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), len, nullptr, nullptr);
        return s;
    }

private:
    void Need(size_t k) const {
        if (k > m_n || m_pos > m_n - k) throw ParseError("eof");
    }
    const uint8_t* m_p;
    size_t m_n;
    size_t m_pos = 0;
};

// --- Fichier ------------------------------------------------------------------------------------

bool ReadFileRange(const wchar_t* path, uint64_t off, size_t len, std::vector<uint8_t>& out, uint64_t* fileSize = nullptr) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (fileSize) *fileSize = uint64_t(sz.QuadPart);
    if (off >= uint64_t(sz.QuadPart)) {
        CloseHandle(h);
        out.clear();
        return true;
    }
    len = size_t(std::min<uint64_t>(len, uint64_t(sz.QuadPart) - off));
    out.resize(len);
    LARGE_INTEGER o;
    o.QuadPart = int64_t(off);
    SetFilePointerEx(h, o, nullptr, FILE_BEGIN);
    size_t done = 0;
    bool ok = true;
    while (ok && done < len) {
        DWORD got = 0;
        ok = ReadFile(h, out.data() + done, DWORD(std::min<size_t>(len - done, 64u << 20)), &got, nullptr) && got;
        done += got;
    }
    CloseHandle(h);
    return ok;
}

// --- Package ------------------------------------------------------------------------------------

struct Import {
    std::string classPackage, className, objectName;
    int32_t outer = 0;
};
struct Export {
    int32_t classIndex = 0, outer = 0;
    std::string name;
    int64_t serialSize = 0, serialOffset = 0;
    int64_t scriptStart = 0, scriptEnd = 0;
    bool isAsset = false;
};

struct Package {
    int32_t legacy = 0, ue4 = 0, ue5 = 0;
    uint32_t flags = 0;
    int ue5Main = -1;  // version personnalisée FUE5MainStreamObjectVersion (-1 = absente)
    std::string packageName, engineVersion;
    int32_t thumbnailOffset = 0, dataResourceOffset = 0;
    int64_t bulkDataStart = 0, payloadToc = -1;
    bool unversioned = false;  // package cooké : versions déduites
    std::vector<std::string> names;
    std::vector<Import> imports;
    std::vector<Export> exports;
    int main = -1;  // index de l'export principal

    bool FilterEditorOnly() const { return (flags & PKG_FilterEditorOnly) != 0; }
    std::string ClassOf(const Export& e) const {
        if (e.classIndex < 0 && size_t(-e.classIndex - 1) < imports.size()) return imports[size_t(-e.classIndex - 1)].objectName;
        return {};
    }
    std::string MainClass() const { return main >= 0 ? ClassOf(exports[size_t(main)]) : std::string(); }

    std::string Name(Reader& r) const {
        int32_t idx = r.I32(), num = r.I32();
        if (idx < 0 || size_t(idx) >= names.size()) throw ParseError("name");
        return num ? names[size_t(idx)] + "_" + std::to_string(num - 1) : names[size_t(idx)];
    }
};

void SkipEngineVersion(Reader& r, Package* pkg) {
    uint16_t major = r.U16(), minor = r.U16(), patch = r.U16();
    r.U32();  // changelist
    std::string branch = r.FString();
    if (pkg && pkg->engineVersion.empty() && (major || minor)) {
        char b[96];
        snprintf(b, sizeof(b), "%u.%u.%u", major, minor, patch);
        pkg->engineVersion = b;
    }
}

// Lit le résumé, les noms, les imports et les exports. `r` couvre au moins l'en-tête du package.
// forcedUe4/forcedUe5 : versions supposées pour un package « non versionné » (cooké).
void ParsePackageImpl(Reader& r, Package& p, int32_t forcedUe4, int32_t forcedUe5) {
    if (r.U32() != kPackageTag) throw ParseError("tag");
    p.legacy = r.I32();
    if (p.legacy < -9 || p.legacy > -1) throw ParseError("version");
    if (p.legacy != -4) r.I32();  // LegacyUE3Version
    p.ue4 = r.I32();
    if (p.legacy <= -8) p.ue5 = r.I32();
    r.I32();  // licensee
    if (p.ue4 == 0 && p.ue5 == 0) {
        if (!forcedUe4) throw ParseError("unversioned");
        p.unversioned = true;
        p.ue4 = forcedUe4;
        p.ue5 = forcedUe5;
    }
    if (p.ue5 >= 1016) {
        r.Skip(20);  // SavedHash
        r.I32();     // TotalHeaderSize
    }
    if (p.legacy <= -2) {  // versions personnalisées
        int32_t n = r.I32();
        if (n < 0 || n > 100000) throw ParseError("customver");
        for (int32_t i = 0; i < n; ++i) {
            if (p.legacy == -2) {
                r.U32();
                r.I32();
            } else {
                uint32_t g[4];
                r.Guid(g);
                int32_t v = r.I32();
                if (p.legacy >= -5) r.FString();
                if (!memcmp(g, kUE5MainGuid, 16)) p.ue5Main = v;
            }
        }
    }
    if (p.ue5 < 1016) r.I32();  // TotalHeaderSize
    p.packageName = r.FString();
    p.flags = r.U32();
    int32_t nameCount = r.I32(), nameOffset = r.I32();
    if (p.ue5 >= 1008) {
        r.I32();
        r.I32();
    }
    if (!p.FilterEditorOnly() && p.ue4 >= 516) r.FString();  // LocalizationId
    if (p.ue4 >= 459) {
        r.I32();
        r.I32();
    }
    int32_t exportCount = r.I32(), exportOffset = r.I32(), importCount = r.I32(), importOffset = r.I32();
    if (p.ue5 >= 1015) r.Skip(16);  // cellules Verse
    if (p.ue5 >= 1014) r.I32();     // MetaDataOffset
    r.I32();                        // DependsOffset
    if (p.ue4 >= 384) {
        r.I32();
        r.I32();
    }
    if (p.ue4 >= 510) r.I32();  // SearchableNamesOffset
    p.thumbnailOffset = r.I32();
    if (p.ue5 >= 1018) {
        r.I32();
        r.I32();
    }
    if (p.ue5 < 1016) r.Skip(16);  // Guid
    if (!p.FilterEditorOnly()) {
        if (p.ue4 >= 518) r.Skip(16);
        if (p.ue4 >= 518 && p.ue4 < 520) r.Skip(16);
    }
    int32_t gen = r.I32();
    if (gen < 0 || gen > 100000) throw ParseError("generations");
    r.Skip(uint64_t(gen) * 8);
    if (p.ue4 >= 336) SkipEngineVersion(r, &p);
    else r.I32();
    if (p.ue4 >= 444) SkipEngineVersion(r, nullptr);
    r.U32();  // CompressionFlags
    if (r.I32() != 0) throw ParseError("compressed package");
    r.U32();  // PackageSource
    int32_t add = r.I32();
    for (int32_t i = 0; i < add; ++i) r.FString();
    if (p.legacy > -7) r.I32();
    r.I32();  // AssetRegistryDataOffset
    p.bulkDataStart = r.I64();
    if (p.ue4 >= 224) r.I32();
    if (p.ue4 >= 326) {
        int32_t c = r.I32();
        r.Skip(uint64_t(std::max(0, c)) * 4);
    } else if (p.ue4 >= 278) {
        r.I32();
    }
    if (p.ue4 >= 507) {
        r.I32();
        r.I32();
    }
    if (p.ue5 >= 1001) r.I32();
    if (p.ue5 >= 1002) p.payloadToc = r.I64();
    if (p.ue5 >= 1009) p.dataResourceOffset = r.I32();
    // Package non versionné : la bonne hypothèse de version est celle où le résumé se termine
    // exactement au début de la table des noms.
    if (p.unversioned && r.Pos() != size_t(nameOffset)) throw ParseError("version guess");

    // Noms
    if (nameCount < 0 || nameCount > 10000000) throw ParseError("names");
    r.Seek(uint64_t(nameOffset));
    p.names.resize(size_t(nameCount));
    for (auto& n : p.names) {
        n = r.FString();
        if (p.ue4 >= 504) r.Skip(4);  // hachages
    }
    // Imports
    if (importCount < 0 || importCount > 10000000) throw ParseError("imports");
    r.Seek(uint64_t(importOffset));
    p.imports.resize(size_t(importCount));
    // PackageName des imports : absent des packages cookés jusqu'en 5.7, présent en 5.8 sans changement
    // de version -> on le détecte à la taille d'une entrée.
    bool importPackageName = p.ue4 >= 520 && !p.FilterEditorOnly();
    if (p.ue4 >= 520 && p.FilterEditorOnly() && importCount > 0 && exportOffset > importOffset) {
        const int64_t base = 28 + (p.ue5 >= 1003 ? 4 : 0);
        importPackageName = (int64_t(exportOffset) - importOffset) / importCount == base + 8;
    }
    for (auto& im : p.imports) {
        im.classPackage = p.Name(r);
        im.className = p.Name(r);
        im.outer = r.I32();
        im.objectName = p.Name(r);
        if (importPackageName) p.Name(r);
        if (p.ue5 >= 1003) r.U32();
    }
    // Exports
    if (exportCount < 0 || exportCount > 10000000) throw ParseError("exports");
    r.Seek(uint64_t(exportOffset));
    p.exports.resize(size_t(exportCount));
    for (auto& e : p.exports) {
        e.classIndex = r.I32();
        r.I32();                      // SuperIndex
        if (p.ue4 >= 508) r.I32();    // TemplateIndex
        e.outer = r.I32();
        e.name = p.Name(r);
        r.U32();                      // ObjectFlags
        if (p.ue4 >= 511) {
            e.serialSize = r.I64();
            e.serialOffset = r.I64();
        } else {
            e.serialSize = r.I32();
            e.serialOffset = r.I32();
        }
        r.Skip(12);                   // bForcedExport, bNotForClient, bNotForServer
        if (p.ue5 < 1005) r.Skip(16); // PackageGuid
        if (p.ue5 >= 1006) r.U32();   // bIsInheritedInstance
        r.U32();                      // PackageFlags
        if (p.ue4 >= 365) r.U32();
        if (p.ue4 >= 485) e.isAsset = r.U32() != 0;
        if (p.ue5 >= 1003) r.U32();
        if (p.ue4 >= 507) r.Skip(20);
        if (p.ue5 >= 1010 && !(p.flags & PKG_UnversionedProperties)) {
            e.scriptStart = r.I64();
            e.scriptEnd = r.I64();
        }
    }
    // Asset principal : bIsAsset, sinon l'export racine qui porte le nom court du package
    std::string shortName = p.packageName.substr(p.packageName.find_last_of('/') + 1);
    for (size_t i = 0; i < p.exports.size() && p.main < 0; ++i)
        if (p.exports[i].isAsset) p.main = int(i);
    for (size_t i = 0; i < p.exports.size() && p.main < 0; ++i)
        if (p.exports[i].outer == 0 && p.exports[i].name == shortName) p.main = int(i);
    if (p.main < 0 && !p.exports.empty()) p.main = 0;
}

void ParsePackage(Reader& r, Package& p) {
    if (r.Size() < 24) throw ParseError("eof");
    int32_t legacy, ue4 = 0, ue5 = 0;
    memcpy(&legacy, r.Data() + 4, 4);
    memcpy(&ue4, r.Data() + 12, 4);
    if (legacy <= -8) memcpy(&ue5, r.Data() + 16, 4);
    if (ue4 != 0 || ue5 != 0) {
        r.Seek(0);
        ParsePackageImpl(r, p, 0, 0);
        return;
    }
    // Non versionné (cooké) : versions candidates selon la famille (UE 4.x / 5.0-5.5 / 5.6+)
    std::vector<std::pair<int32_t, int32_t>> candidates;
    if (legacy == -7) candidates = {{522, 0}, {518, 0}};
    else if (legacy == -8) candidates = {{522, 1013}, {522, 1008}, {522, 1004}};
    else if (legacy <= -9) candidates = {{522, 1018}, {522, 1017}};
    for (auto& c : candidates) {
        try {
            Package q;
            r.Seek(0);
            ParsePackageImpl(r, q, c.first, c.second);
            p = std::move(q);
            return;
        } catch (const ParseError&) {
        }
    }
    throw ParseError("unversioned");
}

bool IsTextureClass(const std::string& c) {
    static const char* kinds[] = {"Texture2D", "TextureCube", "Texture2DArray", "TextureCubeArray", "VolumeTexture",
                                  "TextureLightProfile", "LightMapTexture2D", "ShadowMapTexture2D", "LightMapVirtualTexture2D"};
    for (const char* k : kinds)
        if (c == k) return true;
    return false;
}

// Lit l'en-tête du package (sans les données) depuis le fichier.
bool LoadHeader(const wchar_t* path, std::vector<uint8_t>& buf, Package& pkg) {
    if (!ReadFileRange(path, 0, 65536, buf)) return false;
    try {
        Reader r(buf.data(), buf.size());
        ParsePackage(r, pkg);
        return true;
    } catch (const ParseError&) {
    }
    // En-tête plus grand que 64 Ko : on relit jusqu'à 8 Mo
    if (buf.size() < 65536 || !ReadFileRange(path, 0, 8u << 20, buf)) return false;
    pkg = Package();
    try {
        Reader r(buf.data(), buf.size());
        ParsePackage(r, pkg);
        return true;
    } catch (const ParseError&) {
        return false;
    }
}

// --- Propriétés « taggées » -------------------------------------------------------------------

struct Tag {
    std::string name, type, inner;  // inner : StructName / EnumName / type des éléments
    int32_t size = 0;
    bool boolVal = false;
};

// Retourne false sur la balise de fin « None ».
bool ReadTag(Reader& r, const Package& p, Tag& t) {
    t = Tag();
    t.name = p.Name(r);
    if (t.name == "None") return false;
    if (p.ue5 >= 1012) {  // FPropertyTypeName : liste préfixe de nœuds {nom, nombre d'enfants}
        int remaining = 1, node = 0;
        while (remaining > 0) {
            std::string n = p.Name(r);
            int32_t inner = r.I32();
            if (node == 0) t.type = n;
            else if (node == 1) t.inner = n;
            remaining += inner - 1;
            if (++node > 256) throw ParseError("typename");
        }
        t.size = r.I32();
        uint8_t flags = r.U8();
        if (flags & 0x01) r.I32();
        if (flags & 0x02) r.Skip(16);
        if (flags & 0x04) {
            uint8_t ext = r.U8();
            if (ext & 0x02) {
                r.U8();
                r.U32();
            }
            if (ext & 0x04) r.U32();
        }
        t.boolVal = (flags & 0x10) != 0;
        return true;
    }
    t.type = p.Name(r);
    t.size = r.I32();
    r.I32();  // ArrayIndex
    if (t.type == "StructProperty") {
        t.inner = p.Name(r);
        if (p.ue4 >= 441) r.Skip(16);
    } else if (t.type == "BoolProperty") {
        t.boolVal = r.U8() != 0;
    } else if (t.type == "ByteProperty" || t.type == "EnumProperty") {
        t.inner = p.Name(r);
    } else if (t.type == "ArrayProperty" || t.type == "OptionalProperty") {
        if (p.ue4 >= 282) t.inner = p.Name(r);
    } else if (t.type == "SetProperty") {
        if (p.ue4 >= 509) t.inner = p.Name(r);
    } else if (t.type == "MapProperty") {
        if (p.ue4 >= 509) {
            t.inner = p.Name(r);
            p.Name(r);
        }
    }
    if (p.ue4 >= 503 && r.U8()) r.Skip(16);
    if (p.ue5 >= 1011) {
        uint8_t ext = r.U8();
        if (ext & 0x02) {
            r.U8();
            r.U32();
        }
        if (ext & 0x04) r.U32();
    }
    return true;
}

// Valeur d'une énumération : FName (8 octets) ou octet brut
std::string EnumValue(Reader& r, const Package& p, const Tag& t) {
    if (t.size == 8) return p.Name(r);
    if (t.size == 1) return std::to_string(r.U8());
    if (t.size == 4) return std::to_string(r.I32());
    return {};
}

struct SourceInfo {
    int32_t sizeX = 0, sizeY = 0, numSlices = 1, numMips = 1, numLayers = 1;
    std::string format = "TSF_Invalid", compression = "TSCF_None";
    bool pngCompressed = false;
    bool found = false;
};

struct TextureProps {
    SourceInfo src;
    std::string compressionSettings, lodGroup;
    bool srgb = true;
};

void ReadSourceStruct(Reader& r, const Package& p, size_t end, SourceInfo& s) {
    Tag t;
    while (r.Pos() < end && ReadTag(r, p, t)) {
        size_t start = r.Pos();
        if (t.name == "SizeX") s.sizeX = r.I32();
        else if (t.name == "SizeY") s.sizeY = r.I32();
        else if (t.name == "NumSlices") s.numSlices = r.I32();
        else if (t.name == "NumMips") s.numMips = r.I32();
        else if (t.name == "NumLayers") s.numLayers = r.I32();
        else if (t.name == "Format") s.format = EnumValue(r, p, t);
        else if (t.name == "CompressionFormat") s.compression = EnumValue(r, p, t);
        else if (t.name.rfind("bPNGCompressed", 0) == 0) s.pngCompressed = t.boolVal;
        r.Seek(start + size_t(std::max(0, t.size)));
    }
    s.found = true;
}

void ReadTextureProps(Reader& r, const Package& p, const Export& e, TextureProps& tp) {
    r.Seek(uint64_t(e.serialOffset));
    if (p.ue5 >= 1011) {
        uint8_t ext = r.U8();
        if (ext & 0x02) r.U8();
    }
    const size_t end = size_t(e.serialOffset + e.serialSize);
    Tag t;
    while (r.Pos() < end && ReadTag(r, p, t)) {
        size_t start = r.Pos();
        if (t.name == "Source" && t.type == "StructProperty") ReadSourceStruct(r, p, start + size_t(t.size), tp.src);
        else if (t.name == "CompressionSettings") tp.compressionSettings = EnumValue(r, p, t);
        else if (t.name == "LODGroup") tp.lodGroup = EnumValue(r, p, t);
        else if (t.name == "SRGB") tp.srgb = t.boolVal;
        r.Seek(start + size_t(std::max(0, t.size)));
    }
    if (p.ue5 >= 1010 && e.scriptEnd > 0) r.Seek(uint64_t(e.serialOffset + e.scriptEnd));  // fin officielle des propriétés
}

// --- Données source (bulk data) -----------------------------------------------------------------

// FCompressedBuffer (en-tête big-endian de 64 octets)
bool DecodeCompressedBuffer(const uint8_t* data, size_t avail, std::vector<uint8_t>& out, std::wstring& err) {
    Reader r(data, avail);
    if (r.BE32() != 0xB7756362) {
        err = L"en-tête FCompressedBuffer invalide";
        return false;
    }
    r.BE32();  // CRC
    uint8_t method = r.U8();
    r.U8();
    r.U8();
    uint8_t blockExp = r.U8();
    uint32_t blocks = r.BE32();
    uint64_t rawSize = r.BE64(), compSize = r.BE64();
    r.Skip(32);  // BLAKE3
    if (rawSize > (4ull << 30) || compSize > avail) {
        err = L"taille de données incohérente";
        return false;
    }
    out.resize(size_t(rawSize));
    if (method == 0) {
        memcpy(out.data(), r.Take(size_t(rawSize)), size_t(rawSize));
        return true;
    }
    if (method != 3 && method != 4) {
        err = L"méthode de compression inconnue";
        return false;
    }
    if (method == 3 && !OodleAvailable()) {
        err = L"décompresseur Oodle introuvable (installez Unreal Engine ou copiez oo2core_9_win64.dll dans le dossier plugins)";
        return false;
    }
    std::vector<uint32_t> sizes(blocks);
    for (auto& s : sizes) s = r.BE32();
    const uint64_t blockSize = 1ull << blockExp;
    uint64_t done = 0;
    for (uint32_t i = 0; i < blocks; ++i) {
        size_t raw = size_t(std::min<uint64_t>(blockSize, rawSize - done));
        const uint8_t* src = r.Take(sizes[i]);
        bool ok;
        if (sizes[i] >= raw) {
            memcpy(out.data() + done, src, raw);
            ok = true;
        } else if (method == 3) {
            ok = OodleDecompress(src, sizes[i], out.data() + done, raw);
        } else {
            ok = Lz4Decompress(src, sizes[i], out.data() + done, raw);
        }
        if (!ok) {
            err = L"échec de décompression";
            return false;
        }
        done += raw;
    }
    return done == rawSize;
}

// FArchive::SerializeCompressed (zlib par morceaux) — anciennes données UE4
bool DecodeZlibChunks(const uint8_t* data, size_t avail, std::vector<uint8_t>& out) {
    Reader r(data, avail);
    uint64_t tag = r.U64(), chunk = r.U64();
    if (uint32_t(tag) != kPackageTag) return false;
    if (chunk == kPackageTag) chunk = 131072;
    r.U64();
    uint64_t total = r.U64();
    if (total > (4ull << 30) || chunk == 0) return false;
    size_t n = size_t((total + chunk - 1) / chunk);
    std::vector<std::pair<uint64_t, uint64_t>> sizes(n);
    for (auto& s : sizes) {
        s.first = r.U64();
        s.second = r.U64();
    }
    out.resize(size_t(total));
    size_t op = 0;
    for (auto& s : sizes) {
        if (op + s.second > out.size()) return false;
        if (!ZlibDecompress(r.Take(size_t(s.first)), size_t(s.first), out.data() + op, size_t(s.second))) return false;
        op += size_t(s.second);
    }
    return op == total;
}

// Lit l'en-tête de bulk data après les propriétés et retourne le contenu source décompressé.
bool LoadSourcePayload(Reader& r, const Package& p, std::vector<uint8_t>& payload, std::wstring& err) {
    if (r.U32()) r.Skip(16);  // UObject : GUID optionnel
    uint8_t globalStrip = r.U8();
    r.U8();
    if (globalStrip & 1) {
        err = L"données d'édition retirées (package cooké)";
        return false;
    }
    const uint8_t* file = r.Data();
    const size_t fileSize = r.Size();

    if (p.ue5Main >= kTextureSourceVirtualization) {  // UE5 : FEditorBulkData
        uint32_t flags = r.U32();
        r.Skip(16);  // BulkDataId
        uint8_t hash[20];
        memcpy(hash, r.Take(20), 20);
        int64_t size = r.I64();
        int64_t offset = -1;
        if (!(flags & 0x200) && !(flags & 0x1) && !(flags & 0x400)) offset = r.I64();
        if (size == 0) {
            err = L"texture sans données source";
            return false;
        }
        if (flags & 0x1) {
            err = L"données source virtualisées (stockées hors du fichier)";
            return false;
        }
        if (flags & 0x200) {  // dans la remorque du package (FPackageTrailer)
            if (p.payloadToc <= 0) {
                err = L"remorque de package introuvable";
                return false;
            }
            Reader t(file, fileSize);
            t.Seek(uint64_t(p.payloadToc));
            if (t.U64() != 0xD1C43B2E80A5F697ull) {
                err = L"remorque de package invalide";
                return false;
            }
            uint32_t ver = t.U32(), headerLen = t.U32();
            t.U64();
            if (ver < 1) t.U8();
            int32_t count = t.I32();
            for (int32_t i = 0; i < count; ++i) {
                const uint8_t* id = t.Take(20);
                int64_t off = t.I64();
                uint64_t comp = t.U64();
                t.U64();
                if (ver >= 2) t.Skip(4);
                uint8_t access = ver >= 1 ? t.U8() : 0;
                if (memcmp(id, hash, 20)) continue;
                if (access != 0) {
                    err = access == 2 ? L"données source virtualisées" : L"données source dans un autre fichier";
                    return false;
                }
                uint64_t abs = uint64_t(p.payloadToc) + headerLen + uint64_t(off);
                if (abs + comp > fileSize) {
                    err = L"données source tronquées";
                    return false;
                }
                return DecodeCompressedBuffer(file + abs, size_t(comp), payload, err);
            }
            err = L"données source absentes de la remorque";
            return false;
        }
        if (offset < 0 || uint64_t(offset) >= fileSize) {
            err = L"position des données source invalide";
            return false;
        }
        if (flags & 0x4) {  // ancien bulk data référencé tel quel
            if (flags & 0x8) return DecodeZlibChunks(file + offset, fileSize - size_t(offset), payload);
            if (uint64_t(offset) + uint64_t(size) > fileSize) return false;
            payload.assign(file + offset, file + offset + size);
            return true;
        }
        return DecodeCompressedBuffer(file + offset, fileSize - size_t(offset), payload, err);
    }

    // UE4 : FByteBulkData
    uint32_t flags = r.U32();
    int64_t count, sizeOnDisk;
    if (flags & 0x2000) {
        count = r.I64();
        sizeOnDisk = r.I64();
    } else {
        count = r.I32();
        sizeOnDisk = r.I32();
    }
    int64_t offset = r.I64();
    if (flags & 0x8000) r.U16();
    if (count <= 0) {
        err = L"texture sans données source";
        return false;
    }
    if (flags & 0x100) {
        err = L"données source dans un fichier séparé (.ubulk)";
        return false;
    }
    uint64_t pos = (flags & 0x1) ? uint64_t(offset) + ((flags & 0x10000) ? 0 : uint64_t(p.bulkDataStart)) : r.Pos();
    if (pos >= fileSize || pos + uint64_t(sizeOnDisk) > fileSize) {
        err = L"données source tronquées";
        return false;
    }
    if (flags & 0x2) {
        if (!DecodeZlibChunks(file + pos, size_t(sizeOnDisk), payload)) {
            err = L"échec de décompression zlib";
            return false;
        }
        return true;
    }
    payload.assign(file + pos, file + pos + count);
    return true;
}

// --- UEDELTA (UE 5.5+) --------------------------------------------------------------------------

// Défait le codage différentiel en place pour une image W x H (une tranche), découpée en
// portions exactement comme ImageCoreDelta.cpp (ce découpage fait partie du format).
void UndoDelta(uint8_t* img, int64_t W, int64_t H, int bpp, int elem) {
    const int64_t stride = W * bpp;
    auto cutsForRows = [](int64_t sizeX, int64_t sizeY, int64_t& rowsPerCut) {
        int64_t px = sizeX * sizeY, cuts = 1;
        if (px > 32768) {
            cuts = px / 32768;
            while (cuts > 512) cuts >>= 1;
        }
        rowsPerCut = (sizeY + cuts - 1) / cuts;
        return (sizeY + rowsPerCut - 1) / rowsPerCut;
    };
    auto portion = [&](int64_t x0, int64_t w, int64_t y0, int64_t h) {
        const int64_t n = w * bpp / elem;  // éléments par ligne
        for (int64_t y = 1; y < h; ++y) {
            uint8_t* row = img + (y0 + y) * stride + x0 * bpp;
            const uint8_t* up = row - stride;
            if (elem == 1) {
                for (int64_t i = 0; i < n; ++i) row[i] = uint8_t(row[i] + up[i]);
            } else if (elem == 2) {
                auto* d = reinterpret_cast<uint16_t*>(row);
                auto* u = reinterpret_cast<const uint16_t*>(up);
                for (int64_t i = 0; i < n; ++i) d[i] = uint16_t(d[i] + u[i] - 0x8080u);
            } else {
                auto* d = reinterpret_cast<uint32_t*>(row);
                auto* u = reinterpret_cast<const uint32_t*>(up);
                for (int64_t i = 0; i < n; ++i) d[i] = d[i] + u[i] - 0x80808080u;
            }
        }
    };
    if (W * H <= 136 * 136) {
        portion(0, W, 0, H);
        return;
    }
    if (stride <= 4096) {
        int64_t rows, cuts = cutsForRows(W, H, rows);
        for (int64_t c = 0; c < cuts; ++c) portion(0, W, c * rows, std::min(rows, H - c * rows));
        return;
    }
    int64_t parts = (stride + 4095) / 4096;
    int64_t partBytes = (stride + parts / 2) / parts;
    partBytes = (partBytes + 63) & ~int64_t(63);
    int64_t partPx = partBytes / bpp;
    parts = (W + partPx - 1) / partPx;
    for (int64_t s = 0; s < parts; ++s) {
        int64_t x0 = s * partPx, w = std::min(partPx, W - x0);
        int64_t rows, cuts = cutsForRows(w, H, rows);
        for (int64_t c = 0; c < cuts; ++c) portion(x0, w, c * rows, std::min(rows, H - c * rows));
    }
}

// --- Conversion des pixels ----------------------------------------------------------------------

struct FormatDesc {
    const char* name;
    int bpp, elem;
};
const FormatDesc* Describe(const std::string& f) {
    static const FormatDesc d[] = {
        {"TSF_G8", 1, 1},       {"TSF_BGRA8", 4, 1},  {"TSF_BGRE8", 4, 1},  {"TSF_RGBA16", 8, 2},
        {"TSF_RGBA16F", 8, 2},  {"TSF_RGBA8", 4, 1},  {"TSF_RGBE8", 4, 1},  {"TSF_G16", 2, 2},
        {"TSF_RGBA32F", 16, 4}, {"TSF_R16F", 2, 2},   {"TSF_R32F", 4, 4},
    };
    std::string n = f;
    if (n.size() > 11 && n.compare(n.size() - 11, 11, "_DEPRECATED") == 0) n.resize(n.size() - 11);
    static const char* byIndex[] = {"TSF_Invalid", "TSF_G8",    "TSF_BGRA8",   "TSF_BGRE8", "TSF_RGBA16", "TSF_RGBA16F",
                                    "TSF_RGBA8",   "TSF_RGBE8", "TSF_G16",     "TSF_RGBA32F", "TSF_R16F", "TSF_R32F"};
    if (!n.empty() && isdigit(uint8_t(n[0]))) {  // valeur numérique (ancien ByteProperty sans énumération)
        int i = atoi(n.c_str());
        if (i >= 0 && i < 12) n = byIndex[i];
    }
    for (auto& x : d)
        if (n == x.name) return &x;
    return nullptr;
}

inline uint8_t LinearToSrgb8(float v) {
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    return uint8_t(s * 255.0f + 0.5f);
}

struct SrgbTable {  // linéaire -> sRGB sur 4096 niveaux (évite pow() par pixel)
    uint8_t t[4097];
    SrgbTable() {
        for (int i = 0; i <= 4096; ++i) t[i] = LinearToSrgb8(i / 4096.0f);
    }
    uint8_t operator()(float v) const {
        if (!(v > 0)) return 0;
        if (v >= 1) return 255;
        return t[int(v * 4096.0f + 0.5f)];
    }
};

// Écrit une tranche (W x H) au format source dans un tampon BGRA (alpha prémultiplié).
// Retourne vrai si l'image contient de la transparence.
bool ConvertSlice(const uint8_t* src, int64_t W, int64_t H, const std::string& fmt, uint8_t* dst, uint32_t stride) {
    static const SrgbTable srgb;
    const FormatDesc* d = Describe(fmt);
    std::string n = d->name;
    bool alpha = false;
    for (int64_t y = 0; y < H; ++y) {
        uint8_t* o = dst + size_t(y) * stride;
        const uint8_t* s = src + size_t(y) * size_t(W) * size_t(d->bpp);
        for (int64_t x = 0; x < W; ++x, o += 4) {
            float r = 0, g = 0, b = 0;
            int a = 255;
            bool lin = false;  // valeurs linéaires (flottantes) à convertir en sRGB
            if (n == "TSF_G8") {
                o[0] = o[1] = o[2] = s[x];
                o[3] = 255;
                continue;
            } else if (n == "TSF_BGRA8" || n == "TSF_RGBA8") {
                const uint8_t* p = s + x * 4;
                uint8_t B = n == "TSF_BGRA8" ? p[0] : p[2], R = n == "TSF_BGRA8" ? p[2] : p[0];
                a = p[3];
                o[0] = uint8_t(B * a / 255);
                o[1] = uint8_t(p[1] * a / 255);
                o[2] = uint8_t(R * a / 255);
                o[3] = uint8_t(a);
                alpha |= a < 255;
                continue;
            } else if (n == "TSF_BGRE8" || n == "TSF_RGBE8") {
                const uint8_t* p = s + x * 4;
                float f = p[3] ? std::ldexp(1.0f, int(p[3]) - (128 + 8)) : 0.0f;
                if (n == "TSF_BGRE8") { b = p[0] * f; g = p[1] * f; r = p[2] * f; }
                else { r = p[0] * f; g = p[1] * f; b = p[2] * f; }
                lin = true;
            } else if (n == "TSF_RGBA16") {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(s) + x * 4;
                o[2] = uint8_t(p[0] >> 8);
                o[1] = uint8_t(p[1] >> 8);
                o[0] = uint8_t(p[2] >> 8);
                a = p[3] >> 8;
                if (a < 255) {
                    for (int c = 0; c < 3; ++c) o[c] = uint8_t(o[c] * a / 255);
                    alpha = true;
                }
                o[3] = uint8_t(a);
                continue;
            } else if (n == "TSF_G16") {
                o[0] = o[1] = o[2] = uint8_t(reinterpret_cast<const uint16_t*>(s)[x] >> 8);
                o[3] = 255;
                continue;
            } else if (n == "TSF_RGBA16F") {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(s) + x * 4;
                r = HalfToFloat(p[0]);
                g = HalfToFloat(p[1]);
                b = HalfToFloat(p[2]);
                a = int(std::clamp(HalfToFloat(p[3]), 0.0f, 1.0f) * 255 + 0.5f);
                lin = true;
            } else if (n == "TSF_RGBA32F") {
                const float* p = reinterpret_cast<const float*>(s) + x * 4;
                r = p[0];
                g = p[1];
                b = p[2];
                a = int(std::clamp(p[3], 0.0f, 1.0f) * 255 + 0.5f);
                lin = true;
            } else if (n == "TSF_R16F") {
                r = g = b = HalfToFloat(reinterpret_cast<const uint16_t*>(s)[x]);
                lin = true;
            } else if (n == "TSF_R32F") {
                r = g = b = reinterpret_cast<const float*>(s)[x];
                lin = true;
            }
            if (lin) {
                o[2] = srgb(r);
                o[1] = srgb(g);
                o[0] = srgb(b);
            }
            if (a < 255) {
                for (int c = 0; c < 3; ++c) o[c] = uint8_t(o[c] * a / 255);
                alpha = true;
            }
            o[3] = uint8_t(a);
        }
    }
    return alpha;
}

// PNG/JPEG -> BGRA, avec échange R/B optionnel (miniatures et sources BGRA8 compressées en PNG :
// l'éditeur écrit des octets BGRA déclarés RGBA).
int DecodeImageFile(const CyImgHost* host, CyImgContext* ctx, const uint8_t* data, size_t size, bool swapRB) {
    if (!swapRB) return host->decode_encoded(ctx, data, size);
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) ||
        FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromMemory(const_cast<BYTE*>(data), DWORD(size))) ||
        FAILED(f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom)))
        return CYIMG_E_FAIL;
    UINT w = 0, h = 0;
    cv->GetSize(&w, &h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    if (FAILED(cv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) return CYIMG_E_FAIL;
    bool alpha = false;
    for (size_t i = 0; i < px.size(); i += 4) alpha |= px[i + 3] < 255;
    uint32_t stride = 0;
    uint8_t* out = host->alloc_bgra(ctx, w, h, alpha ? 1 : 0, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* s = px.data() + size_t(y) * w * 4;
        uint8_t* d = out + size_t(y) * stride;
        for (UINT x = 0; x < w; ++x, s += 4, d += 4) {
            uint8_t a = s[3];
            d[0] = uint8_t(s[2] * a / 255);  // R/B échangés + prémultiplication
            d[1] = uint8_t(s[1] * a / 255);
            d[2] = uint8_t(s[0] * a / 255);
            d[3] = a;
        }
    }
    return CYIMG_OK;
}

// --- Miniature de l'éditeur ---------------------------------------------------------------------

// Lecture directe dans le fichier : pas besoin de charger toute la texture pour une vignette.
int DecodeThumbnail(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path, const Package& p,
                    uint32_t* tw = nullptr, uint32_t* th = nullptr) {
    if (p.thumbnailOffset <= 0) return CYIMG_E_UNSUPPORTED;
    try {
        std::vector<uint8_t> buf;
        if (!ReadFileRange(path, uint64_t(p.thumbnailOffset), 65536, buf)) return CYIMG_E_FAIL;
        Reader r(buf.data(), buf.size());
        int32_t count = r.I32();
        const std::string cls = p.MainClass();
        int32_t best = -1;
        for (int32_t i = 0; i < count && i < 4096; ++i) {
            std::string c = r.FString();
            r.FString();
            int32_t off = r.I32();
            if (best < 0 || c == cls) best = off;
        }
        if (best <= 0) return CYIMG_E_UNSUPPORTED;
        if (!ReadFileRange(path, uint64_t(best), 12, buf) || buf.size() < 12) return CYIMG_E_FAIL;
        Reader h(buf.data(), buf.size());
        int32_t w = h.I32(), hh = h.I32(), n = h.I32();
        if (!w || !hh || n <= 0 || n > (64 << 20)) return CYIMG_E_UNSUPPORTED;
        if (!ReadFileRange(path, uint64_t(best) + 12, size_t(n), buf) || buf.size() != size_t(n)) return CYIMG_E_FAIL;
        if (tw) *tw = uint32_t(std::abs(w));
        if (th) *th = uint32_t(std::abs(hh));
        return DecodeImageFile(host, ctx, buf.data(), buf.size(), true);
    } catch (const ParseError&) {
        return CYIMG_E_FAIL;
    }
}

// --- Textures cookées (.uasset + .uexp + .ubulk/.uptnl) ----------------------------------------

std::wstring Sibling(const wchar_t* path, const wchar_t* ext) {
    std::wstring p = path;
    size_t dot = p.find_last_of(L'.');
    if (dot != std::wstring::npos && p.find_last_of(L"\\/") < dot) p.resize(dot);
    return p + ext;
}

uint64_t FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

struct MipLoc {
    uint32_t w = 0, h = 0, d = 1;
    std::wstring file;   // vide : dans le .uexp
    uint64_t offset = 0, size = 0;
    bool empty = false, compressed = false;
};

struct CookedTexture {
    std::string pixelFormat;
    uint32_t sizeX = 0, sizeY = 0, slices = 1;
    bool cube = false;
    std::vector<MipLoc> mips;
};

struct DataResource {
    uint32_t flags = 0, legacyFlags = 0;
    uint8_t cookedIndex = 0;
    int64_t serialOffset = 0, serialSize = 0;
};

std::vector<DataResource> ReadDataResources(const std::vector<uint8_t>& head, const Package& p) {
    std::vector<DataResource> out;
    if (p.dataResourceOffset <= 0) return out;
    Reader r(head.data(), head.size());
    r.Seek(uint64_t(p.dataResourceOffset));
    uint32_t ver = r.U32();
    int32_t count = r.I32();
    if (count < 0 || count > 1000000) throw ParseError("resources");
    out.resize(size_t(count));
    for (auto& d : out) {
        d.flags = r.U32();
        if (ver >= 2) d.cookedIndex = r.U8();
        d.serialOffset = r.I64();
        r.I64();  // DuplicateSerialOffset
        d.serialSize = r.I64();
        r.I64();  // RawSize
        r.I32();  // OuterIndex
        d.legacyFlags = r.U32();
    }
    return out;
}

// Fichier de données d'après les drapeaux de bulk data (.ubulk, .uptnl optionnel, .m.ubulk)
std::wstring BulkFile(const wchar_t* path, uint32_t flags, uint8_t cookedIndex = 0) {
    if (flags & 0x800) return Sibling(path, L".uptnl");
    if (flags & 0x1000) return Sibling(path, L".m.ubulk");
    if (cookedIndex) {
        wchar_t b[24];
        swprintf_s(b, L".%03u.ubulk", cookedIndex);
        return Sibling(path, b);
    }
    return Sibling(path, L".ubulk");
}

// En-tête de bulk data d'une mip (ancien format, ou index de « data resource » en UE 5.2+)
void ReadMipBulk(Reader& r, const Package& p, const std::vector<DataResource>& res, const wchar_t* path,
                 uint64_t headerSize, MipLoc& m) {
    if (!res.empty()) {
        int32_t idx = r.I32();
        if (idx < 0 || size_t(idx) >= res.size()) throw ParseError("resource index");
        const DataResource& d = res[size_t(idx)];
        m.size = uint64_t(d.serialSize);
        m.empty = d.serialSize <= 0;
        m.compressed = (d.legacyFlags & 0x2) != 0;
        if (!(d.legacyFlags & 0x1)) {  // dans le .uexp, juste à la suite
            m.offset = r.Pos();
            r.Skip(m.size);
        } else if (d.legacyFlags & 0x100) {
            m.file = BulkFile(path, d.legacyFlags, d.cookedIndex);
            m.offset = uint64_t(d.serialOffset);
        } else {
            m.offset = uint64_t(d.serialOffset) - headerSize;
        }
        return;
    }
    uint32_t flags = r.U32();
    int64_t count, size;
    if (flags & 0x2000) {
        count = r.I64();
        size = r.I64();
    } else {
        count = r.I32();
        size = r.I32();
    }
    int64_t offset = r.I64();
    if (flags & 0x8000) r.U16();
    if (flags & 0x4000) {
        r.U32();
        if (flags & 0x2000) r.I64();
        else r.I32();
        r.I64();
    }
    m.size = uint64_t(std::max<int64_t>(0, size));
    m.empty = count <= 0 || size <= 0;
    m.compressed = (flags & 0x2) != 0;
    if (!(flags & 0x1)) {
        m.offset = r.Pos();
        r.Skip(m.size);
    } else if (flags & 0x100) {
        m.file = BulkFile(path, flags);
        m.offset = uint64_t(offset + ((flags & 0x10000) ? 0 : p.bulkDataStart));
    } else {
        m.offset = uint64_t(offset + p.bulkDataStart) - headerSize;
    }
}

// Lit les données de plateforme à partir de l'ancre (FName PF_xxx) trouvée à `pos` dans le .uexp.
bool ParsePlatformData(const std::vector<uint8_t>& uexp, size_t pos, const Package& p,
                       const std::vector<DataResource>& res, const wchar_t* path, uint64_t headerSize,
                       CookedTexture& t) {
    try {
        Reader r(uexp.data(), uexp.size());
        const bool ue5 = p.legacy <= -8;
        // bSerializeMipData (UTexture2D, 5.3+) : juste avant l'ancre, précédé de bCooked = 1
        bool serializeMips = true;
        if (ue5 && pos >= 8) {
            int32_t a, b;
            memcpy(&a, uexp.data() + pos - 4, 4);
            memcpy(&b, uexp.data() + pos - 8, 4);
            if (b == 1 && (a == 0 || a == 1)) serializeMips = a == 1;
        }
        r.Seek(pos);
        std::string fmt = p.Name(r);
        r.I64();  // SkipOffset
        if (ue5) {
            if (r.U8() != 0) return false;  // données dans le DDC : non géré
            r.Skip(15);
        }
        t.sizeX = r.U32();
        t.sizeY = r.U32();
        uint32_t packed = r.U32();
        t.pixelFormat = r.FString();
        if (t.pixelFormat != fmt || !t.sizeX || !t.sizeY || t.sizeX > 65536 || t.sizeY > 65536) return false;
        t.cube = (packed >> 31) != 0;
        const bool hasOpt = (packed >> 30) & 1, cpuCopy = ue5 && ((packed >> 29) & 1);
        t.slices = packed & (ue5 ? 0x1FFFFFFFu : 0x3FFFFFFFu);
        if (hasOpt) r.Skip(8);
        if (cpuCopy) {
            r.Skip(12 + 2);
            r.Skip(uint64_t(r.I64()));
        }
        r.I32();  // FirstMipToSerialize
        int32_t numMips = r.I32();
        if (numMips <= 0 || numMips > 20) return false;
        for (int32_t i = 0; i < numMips; ++i) {
            MipLoc m;
            if (!ue5) r.U32();  // bCooked (UE4)
            if (serializeMips) ReadMipBulk(r, p, res, path, headerSize, m);
            else m.empty = true;
            m.w = r.U32();
            m.h = r.U32();
            m.d = std::max(1u, r.U32());
            if (!m.w || !m.h || m.w > t.sizeX * 2u + 4 || m.h > t.sizeY * 2u + 4) return false;
            t.mips.push_back(std::move(m));
        }
        return true;
    } catch (const ParseError&) {
        return false;
    }
}

struct PfInfo {
    const char* pf;
    DXGI_FORMAT dxgi;   // compression par blocs (décodée par Direct3D)
    const char* tsf;    // ou format non compressé (conversion directe)
};
const PfInfo* PixelFormatInfo(const std::string& pf) {
    static const PfInfo t[] = {
        {"PF_DXT1", DXGI_FORMAT_BC1_UNORM, nullptr},  {"PF_DXT3", DXGI_FORMAT_BC2_UNORM, nullptr},
        {"PF_DXT5", DXGI_FORMAT_BC3_UNORM, nullptr},  {"PF_BC4", DXGI_FORMAT_BC4_UNORM, nullptr},
        {"PF_BC5", DXGI_FORMAT_BC5_UNORM, nullptr},   {"PF_BC6H", DXGI_FORMAT_BC6H_UF16, nullptr},
        {"PF_BC7", DXGI_FORMAT_BC7_UNORM, nullptr},
        {"PF_B8G8R8A8", DXGI_FORMAT_UNKNOWN, "TSF_BGRA8"},     {"PF_R8G8B8A8", DXGI_FORMAT_UNKNOWN, "TSF_RGBA8"},
        {"PF_G8", DXGI_FORMAT_UNKNOWN, "TSF_G8"},              {"PF_L8", DXGI_FORMAT_UNKNOWN, "TSF_G8"},
        {"PF_R8", DXGI_FORMAT_UNKNOWN, "TSF_G8"},              {"PF_A8", DXGI_FORMAT_UNKNOWN, "TSF_G8"},
        {"PF_G16", DXGI_FORMAT_UNKNOWN, "TSF_G16"},            {"PF_R16_UINT", DXGI_FORMAT_UNKNOWN, "TSF_G16"},
        {"PF_FloatRGBA", DXGI_FORMAT_UNKNOWN, "TSF_RGBA16F"},  {"PF_A32B32G32R32F", DXGI_FORMAT_UNKNOWN, "TSF_RGBA32F"},
        {"PF_R16F", DXGI_FORMAT_UNKNOWN, "TSF_R16F"},          {"PF_R32_FLOAT", DXGI_FORMAT_UNKNOWN, "TSF_R32F"},
        {"PF_A16B16G16R16", DXGI_FORMAT_UNKNOWN, "TSF_RGBA16"},{"PF_R16G16B16A16_UNORM", DXGI_FORMAT_UNKNOWN, "TSF_RGBA16"},
    };
    for (auto& x : t)
        if (pf == x.pf) return &x;
    return nullptr;
}

bool ReadMipBytes(const wchar_t*, const std::vector<uint8_t>& uexp, const MipLoc& m, std::vector<uint8_t>& out,
                  std::wstring& why) {
    if (m.empty) {
        why = L"mip absente";
        return false;
    }
    if (m.compressed) {
        why = L"mip compressée (zlib/Oodle) non gérée";
        return false;
    }
    if (m.file.empty()) {
        if (m.offset + m.size > uexp.size()) {
            why = L"données de mip hors du .uexp";
            return false;
        }
        out.assign(uexp.begin() + ptrdiff_t(m.offset), uexp.begin() + ptrdiff_t(m.offset + m.size));
        return true;
    }
    if (!ReadFileRange(m.file.c_str(), m.offset, size_t(m.size), out) || out.size() != m.size) {
        why = L"fichier " + m.file.substr(m.file.find_last_of(L'\\') + 1) + L" absent ou incomplet";
        return false;
    }
    return true;
}

// Convertit une mip (toutes faces/tranches) en BGRA. Cubemap : 6 faces côte à côte.
int EmitMip(const CyImgHost* host, CyImgContext* ctx, const CookedTexture& t, const MipLoc& m,
            const std::vector<uint8_t>& data, std::wstring& why) {
    const PfInfo* pf = PixelFormatInfo(t.pixelFormat);
    if (!pf) {
        why = L"format " + Widen(t.pixelFormat) + L" non pris en charge (ASTC/ETC : textures mobiles)";
        return CYIMG_E_UNSUPPORTED;
    }
    const uint32_t W = m.w, H = m.h;
    const uint32_t faces = t.cube ? 6 : 1;
    size_t faceBytes;
    if (pf->tsf) faceBytes = size_t(W) * H * size_t(Describe(pf->tsf)->bpp);
    else faceBytes = size_t((W + 3) / 4) * ((H + 3) / 4) * BlockBytes(pf->dxgi);
    if (data.size() < faceBytes * faces) {
        why = L"données de mip trop courtes";
        return CYIMG_E_FAIL;
    }
    if (size_t(W) * faces > 65535) {
        why = L"texture trop grande";
        return CYIMG_E_FAIL;
    }
    // Formats par blocs : décompression de toutes les faces avant d'allouer l'image de sortie
    const bool hdr = pf->dxgi == DXGI_FORMAT_BC6H_UF16;
    std::vector<std::vector<uint8_t>> decoded(faces);
    if (!pf->tsf)
        for (uint32_t f = 0; f < faces; ++f)
            if (!GpuDecodeBlocks(pf->dxgi, W, H, data.data() + faceBytes * f, faceBytes, hdr, decoded[f], why))
                return CYIMG_E_FAIL;
    uint32_t stride = 0;
    uint8_t* out = host->alloc_bgra(ctx, W * faces, H, 1, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    for (uint32_t f = 0; f < faces; ++f) {
        const uint8_t* src = data.data() + faceBytes * f;
        uint8_t* dst = out + size_t(f) * W * 4;
        if (pf->tsf) {
            ConvertSlice(src, W, H, pf->tsf, dst, stride);
            continue;
        }
        const std::vector<uint8_t>& px = decoded[f];
        if (hdr) {
            ConvertSlice(px.data(), W, H, "TSF_RGBA32F", dst, stride);
            continue;
        }
        for (uint32_t y = 0; y < H; ++y) {
            const uint8_t* s = px.data() + size_t(y) * W * 4;
            uint8_t* d = dst + size_t(y) * stride;
            for (uint32_t x = 0; x < W; ++x, s += 4, d += 4) {
                uint8_t r = s[0], g = s[1], b = s[2], a = s[3];
                if (pf->dxgi == DXGI_FORMAT_BC4_UNORM) {
                    g = b = r;
                    a = 255;
                } else if (pf->dxgi == DXGI_FORMAT_BC5_UNORM) {  // normal map : Z reconstruit
                    float nx = r / 127.5f - 1, ny = g / 127.5f - 1;
                    float nz = std::sqrt(std::max(0.0f, 1 - nx * nx - ny * ny));
                    b = uint8_t(nz * 127.5f + 127.5f);
                    a = 255;
                }
                d[0] = uint8_t(b * a / 255);
                d[1] = uint8_t(g * a / 255);
                d[2] = uint8_t(r * a / 255);
                d[3] = a;
            }
        }
    }
    return CYIMG_OK;
}

int DecodeCooked(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path, const std::vector<uint8_t>& head,
                 const Package& pkg, std::wstring& why) {
    const std::wstring uexpPath = Sibling(path, L".uexp");
    std::vector<uint8_t> uexp;
    if (!ReadFileRange(uexpPath.c_str(), 0, size_t(-1) >> 1, uexp) || uexp.empty()) {
        why = L"fichier .uexp introuvable à côté du .uasset";
        return CYIMG_E_FAIL;
    }
    const uint64_t headerSize = FileSizeOf(path);  // le moteur utilise la taille réelle du .uasset
    const Export& e = pkg.exports[size_t(pkg.main)];
    const int64_t start = e.serialOffset - int64_t(headerSize), end = start + e.serialSize;
    if (start < 0 || end > int64_t(uexp.size())) {
        why = L"export hors du fichier .uexp";
        return CYIMG_E_FAIL;
    }
    std::vector<DataResource> res;
    try {
        res = ReadDataResources(head, pkg);
    } catch (const ParseError&) {
        why = L"table des données illisible";
        return CYIMG_E_FAIL;
    }
    // Les propriétés « non versionnées » n'ont pas de balises : on cherche l'ancre qui les suit,
    // le nom du format de pixels (FName PF_xxx), puis on valide en lisant toute la structure.
    std::vector<int32_t> pfNames;
    for (size_t i = 0; i < pkg.names.size(); ++i)
        if (pkg.names[i].rfind("PF_", 0) == 0) pfNames.push_back(int32_t(i));
    CookedTexture tex;
    bool found = false;
    for (int64_t pos = start; pos + 16 <= end && !found; ++pos) {
        int32_t idx, num;
        memcpy(&idx, uexp.data() + pos, 4);
        memcpy(&num, uexp.data() + pos + 4, 4);
        if (num != 0 || std::find(pfNames.begin(), pfNames.end(), idx) == pfNames.end()) continue;
        tex = CookedTexture();
        found = ParsePlatformData(uexp, size_t(pos), pkg, res, path, headerSize, tex);
    }
    if (!found) {
        why = pfNames.empty() ? L"aucune donnée de plateforme (texture virtuelle ou non cookée pour PC ?)"
                              : L"structure des données cookées non reconnue";
        return CYIMG_E_FAIL;
    }

    // Mip choisie : la plus grande disponible, ou pour une vignette la plus petite suffisante
    const uint32_t target = host->target_size(ctx);
    std::vector<size_t> order;
    for (size_t i = 0; i < tex.mips.size(); ++i) order.push_back(i);
    if (target) {
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            bool okA = std::max(tex.mips[a].w, tex.mips[a].h) >= target, okB = std::max(tex.mips[b].w, tex.mips[b].h) >= target;
            if (okA != okB) return okA;
            return okA ? a > b : a < b;  // suffisantes : la plus petite ; sinon : la plus grande
        });
    }
    std::wstring lastWhy;
    for (size_t i : order) {
        const MipLoc& m = tex.mips[i];
        std::vector<uint8_t> data;
        if (!ReadMipBytes(path, uexp, m, data, lastWhy)) continue;  // ex. mip optionnelle (.uptnl) absente
        int r = EmitMip(host, ctx, tex, m, data, lastWhy);
        if (r == CYIMG_E_UNSUPPORTED || r == CYIMG_E_OUTOFMEMORY) {
            why = lastWhy;
            return r;
        }
        if (r != CYIMG_OK) continue;
        if (!target) {
            wchar_t b[160];
            swprintf_s(b, L"Unreal %ls (cookée)", Widen(pkg.MainClass()).c_str());
            host->set_format(ctx, b);
            host->set_info(ctx, L"Classe", Widen(pkg.MainClass()).c_str());
            host->set_info(ctx, L"Package", Widen(pkg.packageName).c_str());
            host->set_info(ctx, L"Format GPU", Widen(tex.pixelFormat).c_str());
            swprintf_s(b, L"%u × %u", tex.sizeX, tex.sizeY);
            host->set_info(ctx, L"Taille", b);
            swprintf_s(b, L"%zu / %zu (%u × %u)", i + 1, tex.mips.size(), m.w, m.h);
            host->set_info(ctx, L"Mip affichée", b);
            if (tex.cube) host->set_info(ctx, L"Cubemap", L"6 faces côte à côte");
            host->set_info(ctx, L"Moteur", pkg.legacy == -7 ? L"Unreal Engine 4" : pkg.legacy == -8 ? L"Unreal Engine 5.0 – 5.5" : L"Unreal Engine 5.6+");
        }
        return CYIMG_OK;
    }
    why = lastWhy.empty() ? L"aucune mip lisible" : lastWhy;
    return CYIMG_E_FAIL;
}

// --- Décodage -----------------------------------------------------------------------------------

std::wstring ShortEnum(const std::string& v, const char* prefix) {
    std::string s = v;
    size_t pl = strlen(prefix);
    if (s.compare(0, pl, prefix) == 0) s = s.substr(pl);
    if (s.size() > 11 && s.compare(s.size() - 11, 11, "_DEPRECATED") == 0) s.resize(s.size() - 11);
    return Widen(s);
}

int Fail(const CyImgHost* host, CyImgContext* ctx, const std::wstring& msg, int code = CYIMG_E_FAIL) {
    if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg.c_str());
    return code;
}

int DecodeSource(const CyImgHost* host, CyImgContext* ctx, const std::vector<uint8_t>& file, const Package& pkg,
                 TextureProps& tp, std::wstring& why) {
    Reader r(file.data(), file.size());
    ReadTextureProps(r, pkg, pkg.exports[size_t(pkg.main)], tp);
    const SourceInfo& s = tp.src;
    if (!s.found || s.sizeX <= 0 || s.sizeY <= 0) {
        why = L"informations source absentes";
        return CYIMG_E_FAIL;
    }
    const FormatDesc* fd = Describe(s.format);
    if (!fd) {
        why = L"format source non géré (" + Widen(s.format) + L")";
        return CYIMG_E_FAIL;
    }
    std::vector<uint8_t> payload;
    if (!LoadSourcePayload(r, pkg, payload, why)) return CYIMG_E_FAIL;
    if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;

    const std::string& c = s.compression;
    const bool png = s.pngCompressed || c == "TSCF_PNG" || c == "1";
    const bool jpeg = c == "TSCF_JPEG" || c == "2";
    if (png || jpeg) {  // fichier image complet (une seule tranche / couche / mip)
        bool swap = png && (fd->name == std::string("TSF_BGRA8"));
        return DecodeImageFile(host, ctx, payload.data(), payload.size(), swap);
    }
    if (c == "TSCF_UEJPEG" || c == "3") {
        why = L"compression UEJPEG non gérée";
        return CYIMG_E_FAIL;
    }
    const bool delta = c == "TSCF_UEDELTA" || c == "4";

    // Mip 0 de la couche 0 : tranches consécutives
    const int64_t W = s.sizeX, H = s.sizeY;
    const int64_t sliceBytes = W * H * fd->bpp;
    const std::string cls = pkg.MainClass();
    const bool cube = cls.find("Cube") != std::string::npos && s.numSlices >= 6;
    const int64_t slices = cube ? 6 : 1;  // cubemap : les 6 faces côte à côte ; sinon la 1re tranche
    if (int64_t(payload.size()) < sliceBytes * slices) {
        why = L"données source trop courtes";
        return CYIMG_E_FAIL;
    }
    if (delta)
        for (int64_t i = 0; i < slices; ++i) UndoDelta(payload.data() + i * sliceBytes, W, H, fd->bpp, fd->elem);

    if (W * slices > 65535 || H > 65535) {
        why = L"texture trop grande";
        return CYIMG_E_FAIL;
    }
    uint32_t stride = 0;
    // la transparence n'est connue qu'après conversion : on alloue avec alpha et on le signale
    uint8_t* out = host->alloc_bgra(ctx, uint32_t(W * slices), uint32_t(H), 1, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    for (int64_t i = 0; i < slices; ++i)
        ConvertSlice(payload.data() + i * sliceBytes, W, H, s.format, out + i * W * 4, stride);
    return CYIMG_OK;
}

void AddInfo(const CyImgHost* host, CyImgContext* ctx, const Package& pkg, const TextureProps& tp) {
    auto put = [&](const wchar_t* k, const std::wstring& v) {
        if (!v.empty()) host->set_info(ctx, k, v.c_str());
    };
    put(L"Classe", Widen(pkg.MainClass()));
    put(L"Package", Widen(pkg.packageName));
    if (tp.src.found) {
        wchar_t b[96];
        swprintf_s(b, L"%d × %d", tp.src.sizeX, tp.src.sizeY);
        put(L"Source", b);
        std::wstring fmt = ShortEnum(tp.src.format, "TSF_");
        std::wstring comp = tp.src.pngCompressed ? L"PNG" : ShortEnum(tp.src.compression, "TSCF_");
        put(L"Format source", fmt + (comp.empty() || comp == L"None" ? L"" : L" · " + comp));
        if (tp.src.numMips > 1) put(L"Mips", std::to_wstring(tp.src.numMips));
        if (tp.src.numSlices > 1) put(L"Tranches", std::to_wstring(tp.src.numSlices));
        if (tp.src.numLayers > 1) put(L"Couches", std::to_wstring(tp.src.numLayers) + L" (1re affichée)");
    }
    put(L"Compression", tp.compressionSettings.empty() ? L"Default" : ShortEnum(tp.compressionSettings, "TC_"));
    if (!tp.lodGroup.empty()) put(L"Groupe LOD", ShortEnum(tp.lodGroup, "TEXTUREGROUP_"));
    put(L"sRGB", tp.srgb ? L"oui" : L"non");
    if (!pkg.engineVersion.empty()) put(L"Enregistré par", L"Unreal Engine " + Widen(pkg.engineVersion));
}

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    std::vector<uint8_t> head;
    Package pkg;
    if (!LoadHeader(path, head, pkg)) return Fail(host, ctx, L"Package Unreal illisible ou version non prise en charge");
    const std::string cls = pkg.MainClass();
    if (!IsTextureClass(cls))
        return Fail(host, ctx, L"Ce .uasset n'est pas une texture (" + (cls.empty() ? L"classe inconnue" : Widen(cls)) + L")",
                    CYIMG_E_UNSUPPORTED);
    if (pkg.flags & (PKG_Cooked | PKG_UnversionedProperties)) {  // texture cookée (jeu packagé, Saved/Cooked)
        std::wstring why;
        int r = CYIMG_E_FAIL;
        try {
            r = DecodeCooked(host, ctx, path, head, pkg, why);
        } catch (const ParseError&) {
            why = L"structure non reconnue";
        }
        if (r == CYIMG_OK || r == CYIMG_E_CANCELLED) return r;
        return Fail(host, ctx, L"Texture cookée illisible : " + why, r);
    }

    const uint32_t target = host->target_size(ctx);
    wchar_t fmt[160];
    if (target) {  // vignette : la miniature de l'éditeur suffit (256 px)
        if (DecodeThumbnail(host, ctx, path, pkg) == CYIMG_OK) return CYIMG_OK;
    }

    // Fichier complet (données source à la fin du package)
    std::vector<uint8_t> file;
    if (!ReadFileRange(path, 0, size_t(-1) >> 1, file)) return Fail(host, ctx, L"Lecture du fichier impossible");
    if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;

    TextureProps tp;
    std::wstring why;
    int r = CYIMG_E_FAIL;
    try {
        r = DecodeSource(host, ctx, file, pkg, tp, why);
    } catch (const ParseError&) {
        why = L"structure de la texture non reconnue";
        r = CYIMG_E_FAIL;
    }
    if (r == CYIMG_E_CANCELLED) return r;
    if (r == CYIMG_OK) {
        swprintf_s(fmt, L"Unreal %ls", Widen(cls).c_str());
        host->set_format(ctx, fmt);
        if (!target) AddInfo(host, ctx, pkg, tp);
        return CYIMG_OK;
    }
    // Repli : la miniature de l'éditeur, avec la raison
    uint32_t tw = 0, th = 0;
    if (DecodeThumbnail(host, ctx, path, pkg, &tw, &th) == CYIMG_OK) {
        swprintf_s(fmt, L"Unreal %ls (miniature %u×%u)", Widen(cls).c_str(), tw, th);
        host->set_format(ctx, fmt);
        if (!target) {
            AddInfo(host, ctx, pkg, tp);
            host->set_info(ctx, L"Pleine résolution", (L"indisponible : " + why).c_str());
        }
        return CYIMG_OK;
    }
    return Fail(host, ctx, L"Texture illisible : " + why);
}

int Probe(const uint8_t* h, size_t n) {
    return n >= 4 && h[0] == 0xC1 && h[1] == 0x83 && h[2] == 0x2A && h[3] == 0x9E;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"Unreal Engine (.uasset)", L"1.0", L"uasset", Probe, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }

// Filtre de dossier : seules les textures (cookées ou non) sont listées.
extern "C" __declspec(dllexport) int CyImg_Accept(const wchar_t* path) {
    std::vector<uint8_t> head;
    Package pkg;
    if (!LoadHeader(path, head, pkg)) return 0;
    return IsTextureClass(pkg.MainClass()) ? 1 : 0;
}
