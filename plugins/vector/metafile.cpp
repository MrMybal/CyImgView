// EMF / WMF (et versions gzip EMZ / WMZ).
//
// Rendu Direct2D (ID2D1GdiMetafile : anticrénelage, transparence, EMF+) ; en secours GDI
// (PlayEnhMetaFile) rendu sur fond blanc et sur fond noir pour en déduire l'alpha.
// Les WMF sont convertis en EMF par SetWinMetaFileBits (en-tête « placeable » géré).

#include "vector.h"

#include <shlwapi.h>

#include <algorithm>
#include <cstring>


namespace vec {

namespace {

struct EmfHandle {
    HENHMETAFILE h = nullptr;
    ~EmfHandle() {
        if (h) DeleteEnhMetaFile(h);
    }
};

// WMF -> EMF. Retourne le handle et l'étendue (0,01 mm) déduite de l'en-tête placeable.
HENHMETAFILE WmfToEmf(const std::vector<uint8_t>& d, bool& placeable) {
    const uint8_t* p = d.data();
    size_t n = d.size();
    METAFILEPICT mp{MM_ANISOTROPIC, 0, 0, nullptr};
    placeable = n >= 22 && p[0] == 0xD7 && p[1] == 0xCD && p[2] == 0xC6 && p[3] == 0x9A;
    if (placeable) {
        const int16_t l = int16_t(p[6] | p[7] << 8), t = int16_t(p[8] | p[9] << 8);
        const int16_t r = int16_t(p[10] | p[11] << 8), b = int16_t(p[12] | p[13] << 8);
        uint16_t inch = uint16_t(p[14] | p[15] << 8);
        if (!inch) inch = 1440;
        mp.xExt = int(std::abs(r - l)) * 2540 / inch;
        mp.yExt = int(std::abs(b - t)) * 2540 / inch;
        p += 22;
        n -= 22;
    } else if (n >= 18) {
        // WMF simple : étendue de la fenêtre (META_SETWINDOWEXT), sinon taille de l'écran de référence
        const size_t hdrWords = size_t(p[2] | p[3] << 8);
        size_t pos = hdrWords * 2;
        for (int guard = 0; guard < 4096 && pos + 6 <= n; ++guard) {
            const uint32_t words = uint32_t(p[pos] | p[pos + 1] << 8 | p[pos + 2] << 16 | uint32_t(p[pos + 3]) << 24);
            const uint16_t fn = uint16_t(p[pos + 4] | p[pos + 5] << 8);
            if (words < 3 || fn == 0) break;
            if (fn == 0x020C && pos + 10 <= n) {  // paramètres : hauteur, largeur
                const int16_t hh = int16_t(p[pos + 6] | p[pos + 7] << 8), ww = int16_t(p[pos + 8] | p[pos + 9] << 8);
                if (ww && hh) {
                    mp.xExt = std::abs(int(ww)) * 2540 / 96;
                    mp.yExt = std::abs(int(hh)) * 2540 / 96;
                }
                break;
            }
            pos += size_t(words) * 2;
        }
    }
    if (n < 18 || n > 0x7FFFFFFF) return nullptr;
    HDC ref = GetDC(nullptr);
    HENHMETAFILE h = SetWinMetaFileBits(UINT(n), p, ref, (mp.xExt > 0 && mp.yExt > 0) ? &mp : nullptr);
    ReleaseDC(nullptr, ref);
    return h;
}

// Rendu GDI (secours) : blanc + noir -> alpha
int RenderGdi(const Call& c, HENHMETAFILE emf, uint32_t rw, uint32_t rh) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = LONG(rw);
    bi.bmiHeader.biHeight = -LONG(rh);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    const size_t count = size_t(rw) * rh;
    std::vector<uint8_t> white;
    for (int pass = 0; pass < 2; ++pass) {
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            DeleteDC(dc);
            return CYIMG_E_OUTOFMEMORY;
        }
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, pass == 0 ? 0xFF : 0x00, count * 4);
        SetStretchBltMode(dc, HALFTONE);
        RECT r{0, 0, LONG(rw), LONG(rh)};
        PlayEnhMetaFile(dc, emf, &r);
        GdiFlush();
        const uint8_t* px = static_cast<const uint8_t*>(bits);
        int ret = CYIMG_OK;
        if (pass == 0) {
            white.assign(px, px + count * 4);
        } else {
            // noir = couleur prémultipliée ; alpha = 255 - (blanc - noir)
            std::vector<uint8_t> out(count * 4);
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* w = &white[i * 4];
                const uint8_t* b = &px[i * 4];
                int diff = 0;
                for (int k = 0; k < 3; ++k) diff = std::max(diff, int(w[k]) - int(b[k]));
                const int a = 255 - std::clamp(diff, 0, 255);
                uint8_t* o = &out[i * 4];
                for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::min<int>(b[k], a));
                o[3] = uint8_t(a);
            }
            ret = DeliverPixels(c, out.data(), rw * 4, rw, rh, false);
        }
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        if (ret != CYIMG_OK) return ret;
    }
    return CYIMG_OK;
}

}  // namespace

int DecodeMetafile(const Call& c, const std::vector<uint8_t>& data, bool gz) {
    const bool wmf = !IsEmf(data.data(), data.size());
    EmfHandle emf;
    bool placeable = false;
    if (wmf) emf.h = WmfToEmf(data, placeable);
    else if (data.size() < 0x7FFFFFFF) emf.h = SetEnhMetaFileBits(UINT(data.size()), data.data());
    if (!emf.h) return c.Fail(wmf ? L"Métafichier WMF invalide" : L"Métafichier EMF invalide");

    // Octets EMF (WMF converti) pour Direct2D
    std::vector<uint8_t> bytes;
    if (wmf) {
        const UINT n = GetEnhMetaFileBits(emf.h, 0, nullptr);
        if (!n) return c.Fail(L"Conversion WMF -> EMF impossible");
        bytes.resize(n);
        GetEnhMetaFileBits(emf.h, n, bytes.data());
    }
    const std::vector<uint8_t>& eb = wmf ? bytes : data;
    ENHMETAHEADER hdr{};
    if (!GetEnhMetaFileHeader(emf.h, sizeof(hdr), &hdr)) return c.Fail(L"En-tête de métafichier invalide");

    // Taille naturelle : rclFrame (0,01 mm) -> pixels 96 ppp ; sinon rclBounds (pixels du périphérique)
    double w = (hdr.rclFrame.right - hdr.rclFrame.left) * 96.0 / 2540.0;
    double h = (hdr.rclFrame.bottom - hdr.rclFrame.top) * 96.0 / 2540.0;
    if (!(w > 0.5) || !(h > 0.5)) {
        w = double(hdr.rclBounds.right - hdr.rclBounds.left + 1);
        h = double(hdr.rclBounds.bottom - hdr.rclBounds.top + 1);
    }
    if (!(w > 0.5) || !(h > 0.5) || w > 1e6 || h > 1e6) return c.Fail(L"Dimensions du métafichier invalides");
    uint32_t rw = 0, rh = 0;
    RenderSize(c, w, h, rw, rh);
    if (c.Cancelled()) return CYIMG_E_CANCELLED;

    const wchar_t* fmt = wmf ? (gz ? L"WMZ (WMF compressé)" : placeable ? L"WMF (placeable)" : L"WMF")
                             : (gz ? L"EMZ (EMF compressé)" : L"EMF");
    bool done = false;
    WicTarget t;
    if (t.Create(rw, rh) && t.dc) {
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(eb.data(), UINT(eb.size())));
        ComPtr<ID2D1GdiMetafile> mf;
        if (stream && SUCCEEDED(D2DFactory()->CreateGdiMetafile(stream.Get(), &mf))) {
            D2D1_RECT_F b{};
            mf->GetBounds(&b);
            const float bw = b.right - b.left, bh = b.bottom - b.top;
            if (bw > 0 && bh > 0) {
                t.dc->BeginDraw();
                t.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                t.dc->SetTransform(D2D1::Matrix3x2F::Translation(-b.left, -b.top) *
                                   D2D1::Matrix3x2F::Scale(float(rw) / bw, float(rh) / bh));
                t.dc->DrawGdiMetafile(mf.Get());
                done = SUCCEEDED(t.dc->EndDraw());
            }
        }
    }
    int r;
    if (done) {
        r = t.Deliver(c, false);
    } else {
        r = RenderGdi(c, emf.h, rw, rh);
    }
    if (r != CYIMG_OK) return r;
    c.host->set_format(c.ctx, fmt);
    c.Info(L"Taille d'origine", Fmt(L"%.1f × %.1f mm", w * 25.4 / 96, h * 25.4 / 96));
    c.Info(L"Enregistrements", Fmt(L"%u", unsigned(hdr.nRecords)));
    c.Info(L"Rendu", done ? L"Direct2D" : L"GDI");
    return CYIMG_OK;
}

}  // namespace vec
