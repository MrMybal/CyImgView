// EPS : seul l'aperçu intégré est affiché (pas d'interpréteur PostScript).
//  - en-tête binaire DOS EPS (C5D0D3C6) : aperçu TIFF (décodé par l'hôte) ou WMF (rendu GDI) ;
//  - EPSI : bitmap hexadécimal entre %%BeginPreview et %%EndPreview (1 = noir).

#include "docs_common.h"

#include <algorithm>
#include <cstdlib>

namespace docs {
namespace {

// Rendu d'un métafichier Windows (WMF, éventuellement « placeable ») sur fond blanc.
int RenderWmf(const Job& job, const uint8_t* wmf, size_t len) {
    if (len < 18 || len > (256u << 20)) return CYIMG_E_FAIL;
    double w = 0, h = 0;  // taille naturelle en pixels à 96 ppp
    if (Le32(wmf) == 0x9AC6CDD7u && len > 22 + 18) {  // en-tête « placeable » : cadre et unités par pouce
        const int16_t l = int16_t(Le16(wmf + 6)), t = int16_t(Le16(wmf + 8)), r = int16_t(Le16(wmf + 10)),
                      b = int16_t(Le16(wmf + 12));
        const uint16_t inch = Le16(wmf + 14);
        if (inch) w = std::abs(r - l) * 96.0 / inch, h = std::abs(b - t) * 96.0 / inch;
        wmf += 22, len -= 22;
    }
    const uint16_t type = Le16(wmf), hdrWords = Le16(wmf + 2);
    if ((type != 1 && type != 2) || hdrWords != 9) return CYIMG_E_FAIL;
    if (!(w > 0 && h > 0)) {  // sans en-tête « placeable » : proportions de META_SETWINDOWEXT
        for (size_t pos = 18; pos + 6 <= len;) {
            const size_t words = Le32(wmf + pos);
            const uint16_t fn = Le16(wmf + pos + 4);
            if (words < 3 || words > (len - pos) / 2 || fn == 0) break;
            if (fn == 0x020C && words >= 5) {
                w = std::abs(int16_t(Le16(wmf + pos + 8))), h = std::abs(int16_t(Le16(wmf + pos + 6)));
                break;
            }
            pos += words * 2;
        }
    }
    METAFILEPICT mfp{MM_ANISOTROPIC, LONG(w * 2540 / 96), LONG(h * 2540 / 96), nullptr};
    HENHMETAFILE emf = SetWinMetaFileBits(UINT(len), wmf, nullptr, w > 0 && h > 0 ? &mfp : nullptr);
    if (!emf) return CYIMG_E_FAIL;
    if (!(w > 0 && h > 0)) {
        ENHMETAHEADER eh{};
        if (GetEnhMetaFileHeader(emf, sizeof(eh), &eh)) {
            w = (eh.rclFrame.right - eh.rclFrame.left) * 96.0 / 2540;
            h = (eh.rclFrame.bottom - eh.rclFrame.top) * 96.0 / 2540;
        }
    }
    if (!(w >= 1 && h >= 1) || w > 1e6 || h > 1e6) {
        DeleteEnhMetaFile(emf);
        return CYIMG_E_FAIL;
    }
    // Rendu vectoriel : taille plus confortable pour l'affichage (≥ 1024 px, ≤ 4096 px)
    const uint32_t target = job.Target();
    const double m = std::max(w, h);
    const double want = target ? std::min<double>(target, 4096) : std::clamp(m, 1024.0, 4096.0);
    const double s = want / m;
    const int W = std::max(1, int(w * s + 0.5)), H = std::max(1, int(h * s + 0.5));

    int rc = CYIMG_E_FAIL;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;  // de haut en bas
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0xFF, size_t(W) * H * 4);
        const RECT rect{0, 0, W, H};
        PlayEnhMetaFile(dc, emf, &rect);
        GdiFlush();
        uint32_t stride = 0;
        uint8_t* out = job.host->alloc_bgra(job.ctx, uint32_t(W), uint32_t(H), 0, &stride);
        if (out) {
            for (int y = 0; y < H; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(bits) + size_t(y) * W * 4;
                uint8_t* o = out + size_t(y) * stride;
                for (int x = 0; x < W; ++x) o[x * 4] = src[x * 4], o[x * 4 + 1] = src[x * 4 + 1], o[x * 4 + 2] = src[x * 4 + 2], o[x * 4 + 3] = 255;
            }
            rc = CYIMG_OK;
        } else {
            rc = CYIMG_E_OUTOFMEMORY;
        }
        SelectObject(dc, old);
    }
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    DeleteEnhMetaFile(emf);
    return rc;
}

// Aperçu EPSI dans le texte PostScript.
int DecodeEpsi(const Job& job, const uint8_t* p, size_t n) {
    static const char kBegin[] = "%%BeginPreview:";
    const char* s = reinterpret_cast<const char*>(p);
    const char* end = s + n;
    const char* b = std::search(s, end, kBegin, kBegin + sizeof(kBegin) - 1);
    if (b == end) return CYIMG_E_FAIL;
    b += sizeof(kBegin) - 1;
    long v[4] = {};
    for (int i = 0; i < 4; ++i) {
        while (b < end && (*b == ' ' || *b == '\t')) ++b;
        char* e = nullptr;
        char tmp[16] = {};
        size_t k = 0;
        while (b + k < end && k < 15 && b[k] >= '0' && b[k] <= '9') tmp[k] = b[k], ++k;
        if (!k) return CYIMG_E_FAIL;
        v[i] = strtol(tmp, &e, 10);
        b += k;
    }
    const long w = v[0], h = v[1], depth = v[2];
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (depth != 1 && depth != 2 && depth != 4 && depth != 8))
        return CYIMG_E_FAIL;
    const size_t rowBytes = (size_t(w) * depth + 7) / 8, need = rowBytes * h;
    std::vector<uint8_t> data;
    data.reserve(need);
    // Lignes suivantes : « %hexa… » jusqu'à %%EndPreview
    while (b < end && *b != '\n' && *b != '\r') ++b;
    int hi = -1;
    while (b < end && data.size() < need) {
        while (b < end && (*b == '\n' || *b == '\r')) ++b;
        if (b >= end) break;
        if (end - b >= 13 && memcmp(b, "%%EndPreview", 12) == 0) break;
        if (*b == '%') ++b;
        for (; b < end && *b != '\n' && *b != '\r'; ++b) {
            const char c = *b;
            int x;
            if (c >= '0' && c <= '9') x = c - '0';
            else if (c >= 'a' && c <= 'f') x = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') x = c - 'A' + 10;
            else continue;
            if (hi < 0) {
                hi = x;
            } else {
                data.push_back(uint8_t(hi << 4 | x));
                hi = -1;
                if (data.size() == need) break;
            }
        }
    }
    if (data.size() < need) return CYIMG_E_FAIL;
    uint32_t stride = 0;
    uint8_t* out = job.host->alloc_bgra(job.ctx, uint32_t(w), uint32_t(h), 0, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    const uint32_t maxv = (1u << depth) - 1;
    for (long y = 0; y < h; ++y) {
        const uint8_t* r = &data[size_t(y) * rowBytes];
        uint8_t* o = out + size_t(y) * stride;
        for (long x = 0; x < w; ++x, o += 4) {
            const size_t bit = size_t(x) * depth;
            const uint32_t val = (r[bit >> 3] >> (8 - depth - (bit & 7))) & maxv;
            o[0] = o[1] = o[2] = uint8_t(255 - val * 255 / maxv);  // 0 = blanc
            o[3] = 255;
        }
    }
    wchar_t buf[96];
    swprintf_s(buf, L"miniature intégrée (EPSI %ld × %ld, %ld bit%ls)", w, h, depth, depth > 1 ? L"s" : L"");
    job.Info(L"Aperçu", buf);
    return CYIMG_OK;
}

}  // namespace

int DecodeEps(const Job& job, const uint8_t* f, size_t n) {
    job.host->set_format(job.ctx, L"EPS");
    const uint8_t* ps = f;
    size_t psLen = n;  // section PostScript (tout le fichier sans en-tête DOS)
    if (n >= 30 && Le32(f) == 0xC6D3D0C5u) {  // en-tête binaire DOS EPS
        const uint32_t psOff = Le32(f + 4), psL = Le32(f + 8), wmfOff = Le32(f + 12), wmfLen = Le32(f + 16),
                       tifOff = Le32(f + 20), tifLen = Le32(f + 24);
        auto inside = [&](uint32_t o, uint32_t l) { return l && o >= 30 && o <= n && l <= n - o; };
        if (inside(tifOff, tifLen) && job.Preview(f + tifOff, tifLen, L"TIFF") == CYIMG_OK) return CYIMG_OK;
        if (inside(wmfOff, wmfLen)) {
            const int rc = RenderWmf(job, f + wmfOff, wmfLen);
            if (rc == CYIMG_OK) {
                job.Info(L"Aperçu", L"miniature intégrée (WMF)");
                return CYIMG_OK;
            }
            if (rc == CYIMG_E_OUTOFMEMORY) return rc;
        }
        const bool hadPreview = tifLen || wmfLen;
        if (inside(psOff, psL)) ps = f + psOff, psLen = psL;
        const int rc = DecodeEpsi(job, ps, psLen);
        if (rc == CYIMG_OK || rc == CYIMG_E_OUTOFMEMORY) return rc;
        return job.Fail(hadPreview ? L"EPS : aperçu intégré tronqué ou illisible" : L"EPS sans aperçu (PostScript non supporté)");
    }
    const int rc = DecodeEpsi(job, ps, psLen);
    if (rc == CYIMG_OK || rc == CYIMG_E_OUTOFMEMORY) return rc;
    return job.Fail(L"EPS sans aperçu (PostScript non supporté)");
}

}  // namespace docs
