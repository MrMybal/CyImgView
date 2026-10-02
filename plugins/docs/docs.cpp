// Plugin CyImgView : fichiers de travail de logiciels graphiques (composite ou aperçu intégré).
//
//  - Krita (.kra) et OpenRaster (.ora) : mergedimage.png de l'archive zip ;
//  - GIMP (.xcf) : composition des calques visibles (xcf.cpp) ;
//  - Clip Studio Paint (.clip) : aperçu de la table SQLite CanvasPreview (clip.cpp) ;
//  - EPS : aperçu TIFF/WMF de l'en-tête DOS ou EPSI (eps.cpp) ;
//  - InDesign (.indd) : vignette JPEG des métadonnées XMP ;
//  - CorelDRAW (.cdr) : aperçu PNG/BMP de l'archive zip, ou bloc DISP des anciens fichiers RIFF ;
//  - Affinity (.afphoto, .afdesign, .afpub) : plus grand PNG intégré.

#include "docs_common.h"

#include "zip.h"

#include <algorithm>
#include <cwctype>
#include <functional>

namespace docs {

bool MappedFile::Open(const wchar_t* path) {
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0) return false;
    size = size_t(sz.QuadPart);
    map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!map) return false;
    data = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    return data != nullptr;
}

MappedFile::~MappedFile() {
    if (data) UnmapViewOfFile(data);
    if (map) CloseHandle(map);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

size_t PngLength(const uint8_t* p, size_t avail) {
    if (avail < 8 + 25 || memcmp(p, kPngSig, 8) != 0 || memcmp(p + 12, "IHDR", 4) != 0) return 0;
    size_t pos = 8;
    while (pos + 12 <= avail) {
        const uint32_t len = Be32(p + pos);
        if (len > avail - pos - 12) return 0;
        const bool end = memcmp(p + pos + 4, "IEND", 4) == 0;
        pos += 12 + size_t(len);
        if (end) return pos;
    }
    return 0;
}

bool PngSize(const uint8_t* p, size_t avail, uint32_t& w, uint32_t& h) {
    if (avail < 24 || memcmp(p, kPngSig, 8) != 0 || memcmp(p + 12, "IHDR", 4) != 0) return false;
    w = Be32(p + 16), h = Be32(p + 20);
    return w && h;
}

bool FindLargestPng(const uint8_t* p, size_t n, size_t& off, size_t& len) {
    len = 0;
    for (size_t i = 0; i + 8 <= n;) {
        const void* q = memchr(p + i, 0x89, n - i);
        if (!q) break;
        i = size_t(static_cast<const uint8_t*>(q) - p);
        const size_t l = PngLength(p + i, n - i);
        if (l > len) off = i, len = l;
        i += l ? l : 1;
    }
    return len != 0;
}

int Job::Preview(const uint8_t* data, size_t size, const wchar_t* what) const {
    if (!data || !size) return CYIMG_E_FAIL;
    const int rc = host->decode_encoded(ctx, data, size);
    if (rc != CYIMG_OK) return rc;
    wchar_t buf[160];
    uint32_t w = 0, h = 0;
    if (PngSize(data, size, w, h)) swprintf_s(buf, L"miniature intégrée (%ls, %u × %u)", what, w, h);
    else swprintf_s(buf, L"miniature intégrée (%ls)", what);
    Info(L"Aperçu", buf);
    return CYIMG_OK;
}

// ---------------------------------------------------------------------------------------------
// Archives zip : Krita, OpenRaster, CorelDRAW récents

int DecodeZipDoc(const Job& job, const uint8_t* f, size_t n, const std::wstring& ext) {
    std::vector<ZipEntryInfo> entries;
    if (!ZipList(f, n, entries)) return job.Fail(L"Archive zip illisible ou tronquée");
    auto lower = [](std::string s) {
        for (char& c : s) c = char(tolower(uint8_t(c)));
        return s;
    };
    auto find = [&](const char* name) -> const ZipEntryInfo* {
        for (const auto& e : entries)
            if (lower(e.name) == name) return &e;
        return nullptr;
    };
    std::vector<uint8_t> out;
    auto extract = [&](const ZipEntryInfo* e) { return e && e->size && ZipExtract(f, n, *e, out) && !out.empty(); };

    if (ext == L"cdr" || (!find("mergedimage.png") && !find("maindoc.xml") && !find("stack.xml"))) {
        // CorelDRAW X4+ : previews/page1.png puis metadata/thumbnails/thumbnail.(png|bmp)
        job.host->set_format(job.ctx, L"CorelDRAW");
        const ZipEntryInfo* best = nullptr;
        int bestRank = -1;
        for (const auto& e : entries) {
            const std::string s = lower(e.name);
            const bool img = s.ends_with(".png") || s.ends_with(".bmp") || s.ends_with(".jpg");
            if (!img) continue;
            int rank = -1;
            if (s == "previews/page1.png") rank = 3;
            else if (s.starts_with("previews/")) rank = 2;
            else if (s.starts_with("metadata/thumbnails/")) rank = 1;
            if (rank > bestRank) best = &e, bestRank = rank;
        }
        if (!best) return job.Fail(L"CorelDRAW : aucun aperçu intégré");
        if (!extract(best)) return job.Fail(L"CorelDRAW : aperçu intégré illisible");
        const std::wstring what(best->name.begin(), best->name.end());
        return job.Preview(out.data(), out.size(), what.c_str()) == CYIMG_OK ? CYIMG_OK
                                                                           : job.Fail(L"CorelDRAW : aperçu intégré illisible");
    }

    const bool kra = ext == L"kra" || find("maindoc.xml");
    job.host->set_format(job.ctx, kra ? L"Krita" : L"OpenRaster");
    const ZipEntryInfo* merged = find("mergedimage.png");
    const ZipEntryInfo* thumbs[] = {find("preview.png"), find("thumbnails/thumbnail.png")};
    // Vignette : l'aperçu suffit s'il est assez grand
    if (const uint32_t target = job.Target()) {
        for (auto* t : thumbs) {
            uint32_t w = 0, h = 0;
            if (extract(t) && PngSize(out.data(), out.size(), w, h) && std::max(w, h) >= target &&
                job.host->decode_encoded(job.ctx, out.data(), out.size()) == CYIMG_OK)
                return CYIMG_OK;
        }
    }
    if (extract(merged) && job.host->decode_encoded(job.ctx, out.data(), out.size()) == CYIMG_OK) return CYIMG_OK;
    for (auto* t : thumbs)
        if (extract(t) && job.Preview(out.data(), out.size(), L"image fusionnée absente") == CYIMG_OK) return CYIMG_OK;
    return job.Fail(kra ? L"Krita : image fusionnée (mergedimage.png) absente ou illisible"
                        : L"OpenRaster : image fusionnée (mergedimage.png) absente ou illisible");
}

// ---------------------------------------------------------------------------------------------
// InDesign : vignettes JPEG en base64 dans le XMP (xmpGImg:image, xapGImg:image avant CS3)

static std::vector<uint8_t> Base64(const char* s, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '&') {  // entité XML (&#xA; ...) : ignorée
            while (i < n && s[i] != ';') ++i;
            continue;
        }
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else continue;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

int DecodeIndd(const Job& job, const uint8_t* f, size_t n) {
    job.host->set_format(job.ctx, L"Adobe InDesign");
    static const char kKey[] = "GImg:image";
    const size_t kl = sizeof(kKey) - 1;
    const auto searcher = std::boyer_moore_horspool_searcher(kKey, kKey + kl);
    std::vector<uint8_t> best;
    const char* p = reinterpret_cast<const char*>(f);
    const char* end = p + n;
    for (const char* it = p;;) {
        it = std::search(it, end, searcher);
        if (it == end) break;
        it += kl;
        if (job.Cancelled()) return CYIMG_E_CANCELLED;
        // Élément <x:image>…</x:image> ou attribut x:image="…"
        char stop;
        if (it < end && *it == '>') stop = '<', ++it;
        else if (end - it >= 2 && it[0] == '=' && (it[1] == '"' || it[1] == '\'')) stop = it[1], it += 2;
        else continue;
        const char* e = static_cast<const char*>(memchr(it, stop, std::min<size_t>(size_t(end - it), 64u << 20)));
        if (!e) continue;
        auto img = Base64(it, size_t(e - it));
        if (img.size() > best.size() && img.size() > 3 && img[0] == 0xFF && img[1] == 0xD8) best.swap(img);
        it = e;
    }
    if (best.empty()) return job.Fail(L"InDesign : aucune vignette dans les métadonnées XMP");
    return job.Preview(best.data(), best.size(), L"vignette XMP") == CYIMG_OK ? CYIMG_OK
                                                                         : job.Fail(L"InDesign : vignette XMP illisible");
}

// ---------------------------------------------------------------------------------------------
// CorelDRAW ancien (RIFF) : bloc DISP = DIB d'aperçu

static int DecodeDib(const Job& job, const uint8_t* d, size_t n) {
    // Recherche de l'en-tête BITMAPINFOHEADER au début du bloc
    for (size_t o = 0; o <= 16 && o + 40 <= n; o += 2) {
        const uint8_t* b = d + o;
        if (Le32(b) != 40) continue;
        const int32_t w = int32_t(Le32(b + 4)), h = int32_t(Le32(b + 8));
        const uint16_t planes = Le16(b + 12), bpp = Le16(b + 14);
        const uint32_t comp = Le32(b + 16), used = Le32(b + 32);
        if (w <= 0 || w > 16384 || h == 0 || h > 16384 || h < -16384 || planes != 1 ||
            (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) || comp > 3)
            continue;
        const size_t colors = used ? used : bpp <= 8 ? (size_t(1) << bpp) : 0;
        const size_t hdr = 40 + colors * 4 + (comp == 3 ? 12 : 0);
        const size_t rowBytes = (size_t(w) * bpp + 31) / 32 * 4;
        const size_t bits = comp == 0 || comp == 3 ? rowBytes * size_t(h < 0 ? -h : h) : 0;
        if (hdr > n - o || bits > n - o - hdr) continue;
        const size_t dibLen = comp == 0 || comp == 3 ? hdr + bits : n - o;
        std::vector<uint8_t> bmp(14 + dibLen);
        bmp[0] = 'B', bmp[1] = 'M';
        const uint32_t total = uint32_t(bmp.size()), off = uint32_t(14 + hdr);
        memcpy(&bmp[2], &total, 4);
        memcpy(&bmp[10], &off, 4);
        memcpy(&bmp[14], b, dibLen);
        if (job.Preview(bmp.data(), bmp.size(), L"bloc DISP") == CYIMG_OK) return CYIMG_OK;
    }
    return CYIMG_E_FAIL;
}

int DecodeCdr(const Job& job, const uint8_t* f, size_t n) {
    job.host->set_format(job.ctx, L"CorelDRAW");
    if (n < 12 || memcmp(f, "RIFF", 4) != 0) return job.Fail(L"Fichier CorelDRAW non reconnu");
    // Parcours récursif des blocs RIFF/LIST à la recherche de DISP
    std::function<int(size_t, size_t, int)> walk = [&](size_t pos, size_t end, int depth) -> int {
        while (pos + 8 <= end) {
            const uint8_t* c = f + pos;
            const size_t len = Le32(c + 4);
            if (len > end - pos - 8) return CYIMG_E_FAIL;
            if (memcmp(c, "DISP", 4) == 0 && DecodeDib(job, c + 8, len) == CYIMG_OK) return CYIMG_OK;
            if ((memcmp(c, "LIST", 4) == 0 || memcmp(c, "RIFF", 4) == 0) && len >= 4 && depth < 8 &&
                walk(pos + 12, pos + 8 + len, depth + 1) == CYIMG_OK)
                return CYIMG_OK;
            pos += 8 + len + (len & 1);
        }
        return CYIMG_E_FAIL;
    };
    if (walk(12, std::min<size_t>(n, 8 + size_t(Le32(f + 4))), 0) == CYIMG_OK) return CYIMG_OK;
    return job.Fail(L"CorelDRAW : aucun aperçu intégré (bloc DISP absent)");
}

// ---------------------------------------------------------------------------------------------
// Affinity : plus grand PNG intégré

int DecodeAffinity(const Job& job, const uint8_t* f, size_t n) {
    size_t off = 0, len = 0;
    if (!FindLargestPng(f, n, off, len)) return job.Fail(L"Affinity : aucun aperçu PNG intégré");
    return job.Preview(f + off, len, L"PNG") == CYIMG_OK ? CYIMG_OK : job.Fail(L"Affinity : aperçu intégré illisible");
}

// ---------------------------------------------------------------------------------------------

static int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    const Job job{host, ctx};
    std::wstring ext;
    if (const wchar_t* dot = wcsrchr(path, L'.'); dot && !wcschr(dot, L'\\') && !wcschr(dot, L'/'))
        for (const wchar_t* q = dot + 1; *q; ++q) ext += wchar_t(towlower(*q));
    MappedFile mf;
    if (!mf.Open(path)) return job.Fail(L"Lecture du fichier impossible");
    const uint8_t* f = mf.data;
    const size_t n = mf.size;
    static const uint8_t kInddGuid[16] = {0x06, 0x06, 0xED, 0xF5, 0xD8, 0x1D, 0x46, 0xE5,
                                          0xBD, 0x31, 0xEF, 0xE7, 0xFE, 0x74, 0xB7, 0x1D};
    try {
        // Signature d'abord, extension ensuite
        if (n >= 9 && memcmp(f, "gimp xcf ", 9) == 0) return DecodeXcf(job, f, n);
        if (n >= 8 && memcmp(f, "CSFCHUNK", 8) == 0) return DecodeClip(job, f, n);
        if (n >= 4 && (Le32(f) == 0xC6D3D0C5u || memcmp(f, "%!PS", 4) == 0)) return DecodeEps(job, f, n);
        if (n >= 4 && memcmp(f, "PK\x03\x04", 4) == 0) return DecodeZipDoc(job, f, n, ext);
        if (n >= 12 && memcmp(f, "RIFF", 4) == 0) return DecodeCdr(job, f, n);
        if (n >= 16 && memcmp(f, kInddGuid, 16) == 0) return DecodeIndd(job, f, n);
        if (ext == L"afphoto" || ext == L"afdesign" || ext == L"afpub") {
            host->set_format(ctx, ext == L"afphoto" ? L"Affinity Photo" : ext == L"afdesign" ? L"Affinity Designer"
                                                                                           : L"Affinity Publisher");
            return DecodeAffinity(job, f, n);
        }
        if (ext == L"indd") return DecodeIndd(job, f, n);
        if (ext == L"clip") return DecodeClip(job, f, n);
        if (ext == L"cdr") return job.Fail(L"CorelDRAW : version de fichier non gérée (aucun aperçu trouvé)");
        if (ext == L"xcf") return job.Fail(L"Fichier GIMP non reconnu");
        if (ext == L"eps" || ext == L"epsf") return job.Fail(L"Fichier EPS non reconnu");
        return job.Fail(L"Format de fichier non reconnu");
    } catch (const std::bad_alloc&) {
        return job.Fail(L"Mémoire insuffisante");
    }
}

static int Probe(const uint8_t* h, size_t n) {
    return (n >= 9 && memcmp(h, "gimp xcf ", 9) == 0) || (n >= 8 && memcmp(h, "CSFCHUNK", 8) == 0) ||
           (n >= 4 && Le32(h) == 0xC6D3D0C5u);
}

static const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION,
    L"Documents graphiques (Krita, OpenRaster, GIMP, Clip Studio, EPS, InDesign, CorelDRAW, Affinity)",
    L"1.0",
    L"kra;ora;xcf;clip;indd;eps;epsf;cdr;afphoto;afdesign;afpub",
    Probe,
    Decode,
};

}  // namespace docs

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &docs::g_plugin; }
