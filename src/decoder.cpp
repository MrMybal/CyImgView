#include "decoder.h"
#include "i18n.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

thread_local ComPtr<IWICImagingFactory> t_wic;

IWICImagingFactory* ThreadWic() {
    if (!t_wic) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t_wic));
    }
    return t_wic.Get();
}

struct FileData {
    std::unique_ptr<uint8_t[]> data;
    size_t size = 0;
};

bool ReadWholeFile(const std::wstring& path, FileData& out, std::wstring& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = T("decoder.err.file_access");
        return false;
    }
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 0xFFFFFFF0ll;
    if (ok) {
        out.size = size_t(sz.QuadPart);
        out.data.reset(new (std::nothrow) uint8_t[out.size]);
        ok = out.data != nullptr;
        size_t done = 0;
        while (ok && done < out.size) {
            DWORD chunk = DWORD(std::min<size_t>(out.size - done, 64u << 20));
            DWORD got = 0;
            ok = ReadFile(h, out.data.get() + done, chunk, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(h);
    if (!ok) error = T("decoder.err.read");
    return ok;
}

std::wstring ContainerName(const GUID& g) {
    if (g == GUID_ContainerFormatJpeg) return L"JPEG";
    if (g == GUID_ContainerFormatPng) return L"PNG";
    if (g == GUID_ContainerFormatGif) return L"GIF";
    if (g == GUID_ContainerFormatBmp) return L"BMP";
    if (g == GUID_ContainerFormatTiff) return L"TIFF";
    if (g == GUID_ContainerFormatIco) return L"ICO";
    if (g == GUID_ContainerFormatWmp) return L"JPEG XR";
    if (g == GUID_ContainerFormatDds) return L"DDS";
    if (g == GUID_ContainerFormatHeif) return L"HEIF";
    if (g == GUID_ContainerFormatWebp) return L"WebP";
    if (g == GUID_ContainerFormatRaw) return L"RAW";
    return T("decoder.format.image");
}

bool PixelFormatHasAlpha(IWICImagingFactory* f, REFWICPixelFormatGUID pf) {
    ComPtr<IWICComponentInfo> ci;
    if (FAILED(f->CreateComponentInfo(pf, &ci))) return false;
    ComPtr<IWICPixelFormatInfo2> pi;
    if (FAILED(ci.As(&pi))) return false;
    BOOL t = FALSE;
    pi->SupportsTransparency(&t);
    return t != FALSE;
}

UINT PixelFormatBpp(IWICImagingFactory* f, REFWICPixelFormatGUID pf) {
    ComPtr<IWICComponentInfo> ci;
    if (FAILED(f->CreateComponentInfo(pf, &ci))) return 0;
    ComPtr<IWICPixelFormatInfo> pi;
    if (FAILED(ci.As(&pi))) return 0;
    UINT bpp = 0;
    pi->GetBitsPerPixel(&bpp);
    return bpp;
}

std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// Les rationnels WIC sont des VT_UI8 : numérateur dans les 32 bits bas.
bool Rational(const PROPVARIANT& pv, double& v, uint32_t& num, uint32_t& den) {
    if (pv.vt != VT_UI8 && pv.vt != VT_I8) return false;
    num = pv.uhVal.LowPart;
    den = pv.uhVal.HighPart;
    if (den == 0) return false;
    v = double(num) / double(den);
    return true;
}

std::wstring PropString(const PROPVARIANT& pv) {
    switch (pv.vt) {
    case VT_LPWSTR: return pv.pwszVal ? pv.pwszVal : L"";
    case VT_LPSTR: return pv.pszVal ? Widen(pv.pszVal) : L"";
    case VT_UI2: return std::to_wstring(pv.uiVal);
    case VT_UI4: return std::to_wstring(pv.ulVal);
    default: return {};
    }
}

void Trim(std::wstring& s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\0')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == L' ') ++i;
    s.erase(0, i);
}

// "2024:05:17 14:03:22" -> "17/05/2024 14:03:22"
std::wstring FormatExifDate(const std::wstring& s) {
    if (s.size() >= 19 && s[4] == L':' && s[7] == L':') {
        return s.substr(8, 2) + L"/" + s.substr(5, 2) + L"/" + s.substr(0, 4) + L" " + s.substr(11, 8);
    }
    return s;
}

void ReadPhotoMetadata(IWICMetadataQueryReader* qr, DecodedImage& out) {
    auto get = [&](const wchar_t* name, PROPVARIANT& pv) {
        PropVariantInit(&pv);
        return SUCCEEDED(qr->GetMetadataByName(name, &pv));
    };
    PROPVARIANT pv;
    wchar_t buf[64];

    std::wstring make, model;
    if (get(L"System.Photo.CameraManufacturer", pv)) { make = PropString(pv); PropVariantClear(&pv); }
    if (get(L"System.Photo.CameraModel", pv)) { model = PropString(pv); PropVariantClear(&pv); }
    Trim(make);
    Trim(model);
    if (!model.empty()) {
        // Beaucoup de modèles répètent déjà la marque ("Canon EOS 5D").
        std::wstring full = (!make.empty() && ToLower(model).find(ToLower(make.substr(0, make.find(L' ')))) == std::wstring::npos)
                                ? make + L" " + model : model;
        out.info.emplace_back(T("decoder.info.camera"), full);
    }
    if (get(L"System.Photo.LensModel", pv)) {
        std::wstring lens = PropString(pv);
        Trim(lens);
        if (!lens.empty()) out.info.emplace_back(T("decoder.info.lens"), lens);
        PropVariantClear(&pv);
    }
    double v;
    uint32_t n, d;
    if (get(L"System.Photo.ExposureTime", pv)) {
        if (Rational(pv, v, n, d) && v > 0) {
            if (v < 0.5) swprintf_s(buf, L"1/%.0f s", 1.0 / v);
            else swprintf_s(buf, L"%.1f s", v);
            out.info.emplace_back(T("decoder.info.exposure"), buf);
        }
        PropVariantClear(&pv);
    }
    if (get(L"System.Photo.FNumber", pv)) {
        if (Rational(pv, v, n, d) && v > 0) {
            swprintf_s(buf, L"f/%.1f", v);
            out.info.emplace_back(T("decoder.info.aperture"), buf);
        }
        PropVariantClear(&pv);
    }
    if (get(L"System.Photo.ISOSpeed", pv)) {
        std::wstring s = PropString(pv);
        if (!s.empty()) out.info.emplace_back(L"ISO", s);
        PropVariantClear(&pv);
    }
    if (get(L"System.Photo.FocalLength", pv)) {
        if (Rational(pv, v, n, d) && v > 0) {
            swprintf_s(buf, L"%.0f mm", v);
            out.info.emplace_back(T("decoder.info.focal"), buf);
        }
        PropVariantClear(&pv);
    }
    if (get(L"System.Photo.DateTaken", pv)) {
        std::wstring s;
        if (pv.vt == VT_FILETIME) {
            SYSTEMTIME st;
            FILETIME local;
            FileTimeToLocalFileTime(&pv.filetime, &local);
            FileTimeToSystemTime(&local, &st);
            swprintf_s(buf, L"%02u/%02u/%04u %02u:%02u:%02u", st.wDay, st.wMonth, st.wYear, st.wHour,
                       st.wMinute, st.wSecond);
            s = buf;
        } else {
            s = FormatExifDate(PropString(pv));
        }
        if (!s.empty()) out.info.emplace_back(T("decoder.info.date_taken"), s);
        PropVariantClear(&pv);
    }
}

uint32_t ReadOrientation(IWICMetadataQueryReader* qr) {
    static const wchar_t* names[] = {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
    for (const wchar_t* name : names) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(qr->GetMetadataByName(name, &pv))) {
            uint32_t o = 0;
            if (pv.vt == VT_UI2) o = pv.uiVal;
            else if (pv.vt == VT_UI4) o = pv.ulVal;
            PropVariantClear(&pv);
            if (o >= 1 && o <= 8) return o;
        }
    }
    return 1;
}

bool DecodeGifAnimation(IWICImagingFactory* f, IWICBitmapDecoder* dec, UINT count, DecodedImage& out, std::wstring& error);

// Cœur du décodage WIC. maxSize > 0 : vignette rapide (miniature EXIF, réduction DCT du JPEG).
bool DecodeWicDecoder(IWICImagingFactory* f, IWICBitmapDecoder* dec, uint32_t maxSize, DecodedImage& out,
                      std::wstring& error) {
    GUID cf{};
    if (SUCCEEDED(dec->GetContainerFormat(&cf))) out.format = ContainerName(cf);
    if (cf == GUID_ContainerFormatGif && maxSize == 0) {  // GIF animé : toutes les images
        UINT n = 0;
        if (SUCCEEDED(dec->GetFrameCount(&n)) && n > 1 && DecodeGifAnimation(f, dec, n, out, error)) return true;
        out.frames.clear();
        out.delays.clear();
        out.pixels.Free();
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) {
        error = T("decoder.err.unreadable");
        return false;
    }
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) {
        error = T("decoder.err.dimensions");
        return false;
    }
    out.fullWidth = w;
    out.fullHeight = h;

    WICPixelFormatGUID pf{};
    frame->GetPixelFormat(&pf);
    out.hasAlpha = PixelFormatHasAlpha(f, pf);

    ComPtr<IWICMetadataQueryReader> qr;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&qr))) {
        out.orientation = ReadOrientation(qr.Get());
        if (maxSize == 0) ReadPhotoMetadata(qr.Get(), out);
    }

    ComPtr<IWICBitmapSource> src = frame;
    if (maxSize && std::max(w, h) > maxSize) {
        double scale = double(maxSize) / double(std::max(w, h));
        UINT tw = std::max(1u, UINT(std::lround(w * scale)));
        UINT th = std::max(1u, UINT(std::lround(h * scale)));
        bool done = false;

        // 1) miniature embarquée (EXIF), si assez grande et sans bandes noires
        ComPtr<IWICBitmapSource> thumb;
        if (SUCCEEDED(frame->GetThumbnail(&thumb))) {
            UINT a = 0, b = 0;
            thumb->GetSize(&a, &b);
            double ar1 = double(w) / h, ar2 = b ? double(a) / b : 0;
            if (std::max(a, b) >= maxSize * 0.6 && std::fabs(ar1 - ar2) < 0.03 * ar1) {
                src = thumb;
                done = true;
            }
        }
        // 2) réduction native du décodeur (JPEG : échelle DCT 1/2, 1/4, 1/8 — très rapide)
        if (!done) {
            ComPtr<IWICBitmapSourceTransform> st;
            if (SUCCEEDED(frame.As(&st))) {
                UINT sw = tw, sh = th;
                WICPixelFormatGUID spf = GUID_WICPixelFormat32bppPBGRA;
                if (SUCCEEDED(st->GetClosestSize(&sw, &sh)) && sw < w && sw >= tw && sh >= th &&
                    SUCCEEDED(st->GetClosestPixelFormat(&spf))) {
                    UINT bpp = PixelFormatBpp(f, spf);
                    if (bpp) {
                        UINT stride = (sw * bpp + 31) / 32 * 4;
                        UINT size = stride * sh;
                        std::unique_ptr<BYTE[]> tmp(new (std::nothrow) BYTE[size]);
                        ComPtr<IWICBitmap> bmp;
                        if (tmp && SUCCEEDED(st->CopyPixels(nullptr, sw, sh, &spf, WICBitmapTransformRotate0,
                                                            stride, size, tmp.get())) &&
                            SUCCEEDED(f->CreateBitmapFromMemory(sw, sh, spf, stride, size, tmp.get(), &bmp))) {
                            src = bmp;
                        }
                    }
                }
            }
        }
        // 3) mise à l'échelle finale de qualité
        UINT sw = 0, sh = 0;
        src->GetSize(&sw, &sh);
        if (sw > tw || sh > th) {
            ComPtr<IWICBitmapScaler> sc;
            if (SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
                SUCCEEDED(sc->Initialize(src.Get(), tw, th, WICBitmapInterpolationModeFant))) {
                src = sc;
            }
        }
    }

    ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeMedianCut))) {
        error = T("decoder.err.conversion");
        return false;
    }
    UINT ow = 0, oh = 0;
    cv->GetSize(&ow, &oh);
    size_t stride = size_t(ow) * 4;
    size_t bytes = stride * oh;
    if (bytes > 0xFFFFFFFFull || !out.pixels.Alloc(bytes)) {
        error = T("decoder.err.memory");
        return false;
    }
    if (FAILED(cv->CopyPixels(nullptr, UINT(stride), UINT(bytes), out.pixels.Data()))) {
        out.pixels.Free();
        error = T("decoder.err.decode");
        return false;
    }
    out.width = ow;
    out.height = oh;
    out.stride = UINT(stride);
    return true;
}

constexpr size_t kAnimationBudget = 1024ull << 20;  // mémoire maximale pour les images d'une animation

// GIF animé : chaque image du fichier est un morceau à poser sur le « canevas » (position,
// transparence, règle d'effacement), comme le font les navigateurs.
bool DecodeGifAnimation(IWICImagingFactory* f, IWICBitmapDecoder* dec, UINT count, DecodedImage& out, std::wstring& error) {
    auto metaU = [](IWICMetadataQueryReader* q, const wchar_t* name, uint32_t def) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        uint32_t v = def;
        if (q && SUCCEEDED(q->GetMetadataByName(name, &pv))) {
            if (pv.vt == VT_UI1) v = pv.bVal;
            else if (pv.vt == VT_UI2) v = pv.uiVal;
            else if (pv.vt == VT_UI4) v = pv.ulVal;
            PropVariantClear(&pv);
        }
        return v;
    };
    ComPtr<IWICMetadataQueryReader> gq;
    dec->GetMetadataQueryReader(&gq);
    ComPtr<IWICBitmapFrameDecode> first;
    if (FAILED(dec->GetFrame(0, &first))) return false;
    UINT fw = 0, fh = 0;
    first->GetSize(&fw, &fh);
    const uint32_t W = metaU(gq.Get(), L"/logscrdesc/Width", fw), H = metaU(gq.Get(), L"/logscrdesc/Height", fh);
    if (!W || !H || W > 16384 || H > 16384) return false;
    const size_t stride = size_t(W) * 4, bytes = stride * H;
    std::vector<uint8_t> canvas(bytes, 0), saved;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICMetadataQueryReader> q;
        ComPtr<IWICFormatConverter> cv;
        if (FAILED(dec->GetFrame(i, &frame))) break;
        frame->GetMetadataQueryReader(&q);
        const uint32_t left = metaU(q.Get(), L"/imgdesc/Left", 0), top = metaU(q.Get(), L"/imgdesc/Top", 0);
        uint32_t delay = metaU(q.Get(), L"/grctlext/Delay", 10) * 10;
        if (delay < 20) delay = 100;  // convention des navigateurs pour les délais nuls ou trop courts
        const uint32_t disposal = metaU(q.Get(), L"/grctlext/Disposal", 0);
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        std::vector<uint8_t> px(size_t(w) * h * 4);
        if (FAILED(f->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                  WICBitmapPaletteTypeCustom)) ||
            FAILED(cv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data())))
            break;
        if (disposal == 3) saved = canvas;
        // Pose du morceau (alpha prémultiplié : dst = src + dst * (1 - a))
        for (UINT y = 0; y < h && top + y < H; ++y) {
            const uint8_t* s = px.data() + size_t(y) * w * 4;
            uint8_t* d = canvas.data() + (top + y) * stride + size_t(left) * 4;
            for (UINT x = 0; x < w && left + x < W; ++x, s += 4, d += 4) {
                const uint32_t inv = 255 - s[3];
                for (int c = 0; c < 4; ++c) d[c] = uint8_t(s[c] + (d[c] * inv + 127) / 255);
            }
        }
        // Image affichée = état du canevas
        uint8_t* dst;
        if (i == 0) {
            if (!out.pixels.Alloc(bytes)) {
                error = T("decoder.err.memory");
                return false;
            }
            dst = out.pixels.Data();
        } else {
            if (bytes * (out.frames.size() + 2) > kAnimationBudget) break;  // trop d'images : on s'arrête là
            auto pb = std::make_unique<PixelBuffer>();
            if (!pb->Alloc(bytes)) break;
            dst = pb->Data();
            out.frames.push_back(std::move(pb));
        }
        memcpy(dst, canvas.data(), bytes);
        out.delays.push_back(delay);
        // Effacement avant l'image suivante
        if (disposal == 2) {
            for (UINT y = 0; y < h && top + y < H; ++y)
                memset(canvas.data() + (top + y) * stride + size_t(left) * 4, 0, size_t(std::min(w, W - left)) * 4);
        } else if (disposal == 3 && !saved.empty()) {
            canvas = saved;
        }
    }
    if (!out.pixels.Data()) return false;
    out.width = out.fullWidth = W;
    out.height = out.fullHeight = H;
    out.stride = uint32_t(stride);
    out.hasAlpha = true;
    out.format = T("decoder.format.animated_gif");
    if (out.frames.empty()) out.delays.clear();
    return true;
}

bool DecodeWicMemory(IWICImagingFactory* f, const void* data, size_t size, uint32_t maxSize, DecodedImage& out,
                     std::wstring& error) {
    if (size > 0xFFFFFFF0ull) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), DWORD(size))) ||
        FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
        error = T("decoder.err.unknown_format");
        return false;
    }
    return DecodeWicDecoder(f, dec.Get(), maxSize, out, error);
}

std::vector<std::wstring> SplitExts(const std::wstring& s) {
    std::vector<std::wstring> r;
    std::wstring cur;
    for (wchar_t c : s + L";") {
        if (c == L';' || c == L',' || c == L' ') {
            while (!cur.empty() && cur.front() == L'.') cur.erase(0, 1);
            if (!cur.empty()) r.push_back(ToLower(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Contexte et fonctions hôte exposés aux plugins

struct CyImgContext {
    IWICImagingFactory* wic = nullptr;
    uint32_t target = 0;
    uint32_t quality = 0;
    const std::atomic<bool>* cancel = nullptr;
    DecodedImage* out = nullptr;
    bool produced = false;
    bool refinable = false;
    uint32_t orientation = 0;
    std::wstring format;
    std::wstring error;
    std::vector<std::pair<std::wstring, std::wstring>> info;
};

namespace {

uint32_t HostTargetSize(CyImgContext* c) { return c->target; }
int HostIsCancelled(CyImgContext* c) { return c->cancel && c->cancel->load() ? 1 : 0; }

uint8_t* HostAllocBgra(CyImgContext* c, uint32_t w, uint32_t h, int hasAlpha, uint32_t* stride) {
    if (c->produced || !w || !h || w > 65535 || h > 65535) return nullptr;
    size_t s = size_t(w) * 4;
    if (!c->out->pixels.Alloc(s * h)) return nullptr;
    c->out->width = c->out->fullWidth = w;
    c->out->height = c->out->fullHeight = h;
    c->out->stride = uint32_t(s);
    c->out->hasAlpha = hasAlpha != 0;
    c->produced = true;
    if (stride) *stride = uint32_t(s);
    return c->out->pixels.Data();
}

int HostDecodeEncoded(CyImgContext* c, const void* data, size_t size) {
    if (c->produced || !data || !size) return CYIMG_E_FAIL;
    std::wstring err;
    if (!DecodeWicMemory(c->wic, data, size, c->target, *c->out, err)) return CYIMG_E_FAIL;
    c->produced = true;
    return CYIMG_OK;
}

void HostSetOrientation(CyImgContext* c, uint32_t o) {
    if (o >= 1 && o <= 8) c->orientation = o;
}
void HostSetFormat(CyImgContext* c, const wchar_t* name) {
    if (name) c->format = name;
}
void HostSetInfo(CyImgContext* c, const wchar_t* k, const wchar_t* v) {
    if (k && v && c->info.size() < 64) c->info.emplace_back(k, v);
}

uint32_t HostQuality(CyImgContext* c) { return c->quality; }
void HostSetRefinable(CyImgContext* c) { c->refinable = true; }
const wchar_t* HostTranslate(CyImgContext*, const char* key, const wchar_t* fallback) {
    return key ? TrOr(key, fallback) : fallback;
}

void HostSetError(CyImgContext* c, const wchar_t* m) {
    if (m) c->error = m;
}

uint8_t* HostAddFrame(CyImgContext* c, uint32_t* stride) {
    DecodedImage* o = c->out;
    if (!c->produced || c->target || !o->pixels.Data()) return nullptr;  // pas d'animation pour une vignette
    const size_t bytes = o->pixels.Size();
    if (bytes * (o->FrameCount() + 1) > kAnimationBudget || o->frames.size() >= 10000) return nullptr;
    auto pb = std::make_unique<PixelBuffer>();
    if (!pb->Alloc(bytes)) return nullptr;
    uint8_t* p = pb->Data();
    o->frames.push_back(std::move(pb));
    if (stride) *stride = o->stride;
    return p;
}

void HostSetFrameDelay(CyImgContext* c, uint32_t index, uint32_t ms) {
    auto& d = c->out->delays;
    if (index > 10000) return;
    if (d.size() <= index) d.resize(size_t(index) + 1, 100);
    d[index] = ms < 20 ? 100 : ms;
}

const CyImgHost g_host = {
    CYIMG_API_VERSION, sizeof(CyImgHost), HostTargetSize,     HostIsCancelled, HostAllocBgra,    HostDecodeEncoded,
    HostSetOrientation, HostSetFormat,   HostSetInfo,         HostQuality,     HostSetRefinable, HostSetError,
    HostAddFrame,       HostSetFrameDelay, HostTranslate,
};

// Vignette demandée mais le plugin a rendu une grande image : on la réduit (le cache de vignettes
// garderait sinon des mégaoctets par entrée).
void ShrinkForThumbnail(IWICImagingFactory* f, DecodedImage& img, uint32_t maxSize) {
    if (!maxSize || std::max(img.width, img.height) <= maxSize || !img.pixels.Data()) return;
    double s = double(maxSize) / std::max(img.width, img.height);
    UINT tw = std::max(1u, UINT(std::lround(img.width * s))), th = std::max(1u, UINT(std::lround(img.height * s)));
    ComPtr<IWICBitmap> src;
    ComPtr<IWICBitmapScaler> sc;
    if (FAILED(f->CreateBitmapFromMemory(img.width, img.height, GUID_WICPixelFormat32bppPBGRA, img.stride,
                                         img.stride * img.height, img.pixels.Data(), &src)) ||
        FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(src.Get(), tw, th, WICBitmapInterpolationModeFant)))
        return;
    PixelBuffer out;
    if (!out.Alloc(size_t(tw) * 4 * th) || FAILED(sc->CopyPixels(nullptr, tw * 4, tw * 4 * th, out.Data()))) return;
    if (!img.fullWidth) img.fullWidth = img.width;
    if (!img.fullHeight) img.fullHeight = img.height;
    img.pixels.Swap(out);
    img.width = tw;
    img.height = th;
    img.stride = tw * 4;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

Decoder::~Decoder() {
    // Les plugins ne sont pas déchargés : le processus se termine juste après, et un plugin peut avoir
    // laissé des threads en attente (pool OpenMP de LibRaw, thread pools des décodeurs) qui
    // planteraient si leur code disparaissait sous eux.
}

void Decoder::ReleaseThreadResources() { t_wic.Reset(); }

void Decoder::Init(const std::wstring& exeDir) {
    // Plugins
    std::wstring dir = exeDir + L"\\plugins\\";
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileExW((dir + L"*.dll").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            std::wstring file = dir + fd.cFileName;
            HMODULE mod = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!mod) continue;
            auto get = reinterpret_cast<CyImgGetPluginFn>(GetProcAddress(mod, CYIMG_PLUGIN_ENTRY));
            const CyImgPlugin* api = get ? get() : nullptr;
            if (!api || api->api_version != CYIMG_API_VERSION || !api->decode) {
                FreeLibrary(mod);
                continue;
            }
            Plugin p;
            p.module = mod;
            p.api = api;
            p.exts = SplitExts(api->extensions ? api->extensions : L"");
            p.accept = reinterpret_cast<CyImgAcceptFn>(GetProcAddress(mod, CYIMG_ACCEPT_ENTRY));
            for (auto& e : p.exts) m_allExts.insert(e);
            m_pluginInfo.push_back({api->name ? api->name : fd.cFileName, api->version ? api->version : L"",
                                    fd.cFileName, api->extensions ? api->extensions : L""});
            m_plugins.push_back(std::move(p));
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }

    // Codecs WIC installés (JPEG, PNG, TIFF, GIF, BMP, ICO, JXR, DDS + WebP/HEIF/AVIF si présents)
    if (IWICImagingFactory* f = ThreadWic()) {
        ComPtr<IEnumUnknown> en;
        if (SUCCEEDED(f->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &en))) {
            ComPtr<IUnknown> u;
            ULONG n = 0;
            while (en->Next(1, &u, &n) == S_OK) {
                ComPtr<IWICBitmapDecoderInfo> di;
                if (SUCCEEDED(u.As(&di))) {
                    UINT len = 0;
                    if (SUCCEEDED(di->GetFileExtensions(0, nullptr, &len)) && len > 1) {
                        std::wstring s(len, L'\0');
                        if (SUCCEEDED(di->GetFileExtensions(len, s.data(), &len))) {
                            s.resize(wcslen(s.c_str()));
                            for (auto& e : SplitExts(s)) {
                                m_wicExts.insert(e);
                                m_allExts.insert(e);
                            }
                        }
                    }
                }
                u.Reset();
            }
        }
    }
    for (const wchar_t* e : {L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"gif", L"bmp", L"dib", L"tif", L"tiff",
                             L"ico", L"jxr", L"wdp", L"hdp"}) {
        m_allExts.insert(e);
    }
}

bool Decoder::IsSupportedExt(const std::wstring& extLower) const { return m_allExts.count(extLower) != 0; }

namespace {
int GuardedAccept(CyImgAcceptFn fn, const wchar_t* path);  // plus bas
}  // namespace

bool Decoder::AcceptsFile(const std::wstring& path) const {
    const std::wstring ext = ExtensionOf(path);
    if (!IsSupportedExt(ext)) return false;
    for (auto& p : m_plugins) {  // filtre éventuel du plugin propriétaire de l'extension
        if (!p.accept || std::find(p.exts.begin(), p.exts.end(), ext) == p.exts.end()) continue;
        return GuardedAccept(p.accept, path.c_str()) != 0;
    }
    return true;
}

std::wstring Decoder::DialogPattern() const {
    std::vector<std::wstring> v(m_allExts.begin(), m_allExts.end());
    std::sort(v.begin(), v.end());
    std::wstring s;
    for (auto& e : v) {
        if (!s.empty()) s += L";";
        s += L"*." + e;
    }
    return s;
}

namespace {

// Filet de sécurité : une erreur d'accès mémoire dans un plugin (fichier corrompu, bug d'une
// bibliothèque tierce) fait échouer ce fichier au lieu de fermer la visionneuse. Pas d'objet C++
// à détruire dans cette fonction (contrainte de __try).
constexpr int kPluginCrashed = -1000;

int CrashFilter(EXCEPTION_POINTERS* ep, DWORD* code, void** addr) {
    *code = ep->ExceptionRecord->ExceptionCode;
    *addr = ep->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

int GuardedDecode(const CyImgPlugin* api, CyImgContext* ctx, const wchar_t* path, DWORD* code, void** addr) {
    __try {
        return api->decode(&g_host, ctx, path);
    } __except (CrashFilter(GetExceptionInformation(), code, addr)) {
        if (*code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return kPluginCrashed;
    }
}

// « raw.dll+0x1A2B3 (C0000005) » : où le plugin a planté, pour le signaler.
std::wstring CrashLocation(DWORD code, void* addr) {
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod)) {
        wchar_t full[MAX_PATH];
        if (GetModuleFileNameW(mod, full, MAX_PATH)) wcscpy_s(name, FileNameOf(full).c_str());
    }
    wchar_t buf[MAX_PATH + 64];
    swprintf_s(buf, L"%ls+0x%llX, %08lX", name,
               static_cast<unsigned long long>(static_cast<const char*>(addr) - reinterpret_cast<const char*>(mod)), code);
    return buf;
}

int GuardedAccept(CyImgAcceptFn fn, const wchar_t* path) {
    __try {
        return fn(path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;  // fichier masqué plutôt que visionneuse fermée
    }
}

int GuardedProbe(int (*fn)(const uint8_t*, size_t), const uint8_t* data, size_t size) {
    __try {
        return fn(data, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace

bool Decoder::TryPlugin(const Plugin& p, const std::wstring& path, uint32_t maxSize, bool best,
                        const std::atomic<bool>* cancel, ImagePtr& out, std::wstring& error) const {
    auto img = std::make_shared<DecodedImage>();
    CyImgContext ctx;
    ctx.wic = ThreadWic();
    ctx.target = maxSize;
    ctx.quality = best ? 1 : 0;
    ctx.cancel = cancel;
    ctx.out = img.get();
    DWORD crashCode = 0;
    void* crashAddr = nullptr;
    const int r = GuardedDecode(p.api, &ctx, path.c_str(), &crashCode, &crashAddr);
    if (r == kPluginCrashed) {
        const std::wstring where = CrashLocation(crashCode, crashAddr);
        Trace((L"plugin planté : " + path + L" @ " + where).c_str());
        error = Tf("decoder.err.plugin_crashed", p.api->name ? p.api->name : L"?", where.c_str());
        return false;
    }
    if (r != CYIMG_OK || !ctx.produced || !img->pixels.Data()) {
        if (r == CYIMG_E_CANCELLED) error = T("decoder.err.cancelled");
        else if (!ctx.error.empty()) error = ctx.error;
        else if (error.empty()) error = Tf("decoder.err.plugin_failed", p.api->name ? p.api->name : L"?");
        return false;
    }
    ShrinkForThumbnail(ctx.wic, *img, maxSize);
    if (ctx.orientation) img->orientation = ctx.orientation;
    if (!ctx.format.empty()) img->format = ctx.format;
    else if (p.api->name) img->format = p.api->name;
    if (!ctx.info.empty()) img->info = std::move(ctx.info);
    img->refinable = ctx.refinable && !best && maxSize == 0;
    if (img->frames.empty()) img->delays.clear();
    else img->delays.resize(img->FrameCount(), 100);  // durée par défaut des images sans délai
    out = std::move(img);
    return true;
}

ImagePtr Decoder::Decode(const std::wstring& path, uint32_t maxSize, bool best, const std::atomic<bool>* cancel,
                         std::wstring& error) const {
    double t0 = NowSeconds();
    IWICImagingFactory* f = ThreadWic();
    if (!f) {
        error = T("decoder.err.no_wic");
        return nullptr;
    }
    std::wstring ext = ExtensionOf(path);
    std::vector<const Plugin*> tried;
    ImagePtr out;

    auto finish = [&](ImagePtr img) {
        img->decodeMs = (NowSeconds() - t0) * 1000.0;
        error.clear();
        return img;
    };

    // 1) plugins déclarant l'extension
    for (auto& p : m_plugins) {
        if (std::find(p.exts.begin(), p.exts.end(), ext) == p.exts.end()) continue;
        tried.push_back(&p);
        if (TryPlugin(p, path, maxSize, best, cancel, out, error)) return finish(out);
        if (cancel && cancel->load()) return nullptr;
    }
    // Version complète d'un aperçu : seul un plugin peut la produire, WIC n'apporterait rien.
    if (best && !tried.empty()) return nullptr;

    // 2) WIC (détection par le contenu)
    FileData fdata;
    if (!ReadWholeFile(path, fdata, error)) return nullptr;
    if (cancel && cancel->load()) return nullptr;
    {
        auto img = std::make_shared<DecodedImage>();
        std::wstring werr;
        if (!best && DecodeWicMemory(f, fdata.data.get(), fdata.size, maxSize, *img, werr)) return finish(img);
        if (error.empty()) error = werr;
    }

    // 3) plugins qui reconnaissent l'en-tête
    size_t hsz = std::min<size_t>(fdata.size, 512);
    for (auto& p : m_plugins) {
        if (!p.api->probe || std::find(tried.begin(), tried.end(), &p) != tried.end()) continue;
        if (!GuardedProbe(p.api->probe, fdata.data.get(), hsz)) continue;
        fdata = FileData();  // libère la mémoire avant de laisser la main au plugin
        if (TryPlugin(p, path, maxSize, best, cancel, out, error)) return finish(out);
        break;
    }
    if (error.empty()) error = T("decoder.err.unknown_format");
    return nullptr;
}
