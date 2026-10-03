// Plugin CyImgView « RAW » : tous les formats RAW gérés par LibRaw (~1000 boîtiers).
//
// Stratégie pour la vitesse :
//  1. Aperçu JPEG embarqué (CR2/CR3 : souvent pleine résolution) -> affichage en quelques dizaines de ms.
//  2. Si l'aperçu est plus petit que le capteur, on le signale (set_refinable) : l'hôte l'affiche
//     tout de suite puis redemande la version développée par LibRaw en arrière-plan.
//  3. Sans aperçu exploitable : décodage LibRaw en demi-taille (rapide), puis développement complet.

#include "cyimg_plugin.h"
#include "cyimg_plugin_i18n.h"
#include "rawfile.h"

#include <libraw/libraw.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>

namespace {

uint32_t FlipToExif(int flip) {
    switch (flip) {
    case 3: return 3;
    case 5: return 8;
    case 6: return 6;
    default: return 1;
    }
}

std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(std::max(n, 1) - 1), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w.data(), n))
        MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    while (!w.empty() && (w.back() == L' ' || w.back() == L'\0')) w.pop_back();
    return w;
}

std::wstring ExtUpper(const wchar_t* path) {
    const wchar_t* dot = wcsrchr(path, L'.');
    std::wstring e = dot ? dot + 1 : L"RAW";
    for (auto& c : e) c = wchar_t(towupper(c));
    return e;
}

struct CancelData {
    const CyImgHost* host;
    CyImgContext* ctx;
};

int OnProgress(void* data, enum LibRaw_progress, int, int) {
    auto* c = static_cast<CancelData*>(data);
    return c->host->is_cancelled(c->ctx) ? 1 : 0;  // non nul = abandon
}

// Ouverture LibRaw : lit seulement la structure et les métadonnées (rapide).
class Raw {
public:
    Raw(const CyImgHost* host, CyImgContext* ctx) : m_cancel{host, ctx}, m_lr(std::make_unique<LibRaw>()) {
        m_lr->set_progress_handler(OnProgress, &m_cancel);
        auto& p = m_lr->imgdata.params;
        p.use_camera_wb = 1;  // balance des blancs de l'appareil, comme le JPEG boîtier
        p.output_bps = 8;
        p.user_qual = 3;      // AHD
    }
    ~Raw() {
        if (m_open) m_lr->recycle();
    }

    bool Open(const wchar_t* path) {
        m_open = m_lr->open_file(path) == LIBRAW_SUCCESS;
        if (m_open) {  // mémorisés tout de suite : le développement (demi-taille, user_flip) les modifie
            m_w = m_lr->imgdata.sizes.width;
            m_h = m_lr->imgdata.sizes.height;
            m_flip = m_lr->imgdata.sizes.flip;
        }
        return m_open;
    }
    bool IsOpen() const { return m_open; }
    LibRaw& L() { return *m_lr; }
    uint32_t Orientation() const { return FlipToExif(m_flip); }
    uint32_t SensorLongSide() const { return std::max(m_w, m_h); }

    void AddInfo(const CyImgHost* host, CyImgContext* ctx) {
        const auto& d = m_lr->imgdata;
        wchar_t buf[96];
        std::wstring make = Widen(d.idata.make), model = Widen(d.idata.model);
        if (!model.empty()) host->set_info(ctx, CYIMG_TR("plugin.raw.info.camera", L"Appareil"), (make.empty() ? model : make + L" " + model).c_str());
        std::wstring lens = Widen(d.lens.Lens);
        if (lens.empty()) lens = Widen(d.lens.makernotes.Lens);
        if (!lens.empty()) host->set_info(ctx, CYIMG_TR("plugin.raw.info.lens", L"Objectif"), lens.c_str());
        if (d.other.shutter > 0) {
            if (d.other.shutter < 0.5f) swprintf_s(buf, L"1/%.0f s", 1.0 / d.other.shutter);
            else swprintf_s(buf, L"%.1f s", d.other.shutter);
            host->set_info(ctx, CYIMG_TR("plugin.raw.info.exposure", L"Exposition"), buf);
        }
        if (d.other.aperture > 0) {
            swprintf_s(buf, L"f/%.1f", d.other.aperture);
            host->set_info(ctx, CYIMG_TR("plugin.raw.info.aperture", L"Ouverture"), buf);
        }
        if (d.other.iso_speed > 0) {
            swprintf_s(buf, L"%.0f", d.other.iso_speed);
            host->set_info(ctx, L"ISO", buf);
        }
        if (d.other.focal_len > 0) {
            swprintf_s(buf, L"%.0f mm", d.other.focal_len);
            host->set_info(ctx, CYIMG_TR("plugin.raw.info.focal", L"Focale"), buf);
        }
        if (d.other.timestamp > 0) {
            tm t{};
            time_t ts = d.other.timestamp;
            if (localtime_s(&t, &ts) == 0) {
                wcsftime(buf, 96, L"%d/%m/%Y %H:%M:%S", &t);
                host->set_info(ctx, CYIMG_TR("plugin.raw.info.date_taken", L"Date de prise"), buf);
            }
        }
        AddSensorInfo(host, ctx);
    }

    void AddSensorInfo(const CyImgHost* host, CyImgContext* ctx) {
        wchar_t buf[64];
        swprintf_s(buf, L"%u × %u", m_w, m_h);
        host->set_info(ctx, CYIMG_TR("plugin.raw.info.sensor", L"Capteur"), buf);
    }

private:
    CancelData m_cancel;
    std::unique_ptr<LibRaw> m_lr;
    bool m_open = false;
    uint32_t m_w = 0, m_h = 0;
    int m_flip = 0;
};

// --- Calage de la tonalité sur l'aperçu de l'appareil ------------------------------------------
// LibRaw produit un rendu neutre, plus sombre et plus plat que le JPEG du boîtier (qui applique sa
// courbe de tons). Pour que l'image ne « change » pas quand la version développée remplace
// l'aperçu, on fait correspondre l'histogramme de luminance du développement à celui de l'aperçu.

using Microsoft::WRL::ComPtr;

inline uint8_t Luma(const uint8_t* bgr) { return uint8_t((bgr[2] * 54 + bgr[1] * 183 + bgr[0] * 19) >> 8); }

bool PreviewHistogram(const uint8_t* jpg, size_t size, double hist[256]) {
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) ||
        FAILED(f->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(jpg), DWORD(size))) ||
        FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return false;
    double s = std::min(1.0, 640.0 / std::max(w, h));
    UINT tw = std::max(1u, UINT(w * s)), th = std::max(1u, UINT(h * s));
    ComPtr<IWICBitmapScaler> sc;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(frame.Get(), tw, th, WICBitmapInterpolationModeLinear)) ||
        FAILED(f->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(sc.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    std::vector<uint8_t> px(size_t(tw) * th * 4);
    if (FAILED(cv->CopyPixels(nullptr, tw * 4, UINT(px.size()), px.data()))) return false;
    std::fill(hist, hist + 256, 0.0);
    for (size_t i = 0; i < px.size(); i += 4) hist[Luma(&px[i])] += 1.0;
    return true;
}

void ToneMatch(uint8_t* px, uint32_t w, uint32_t h, uint32_t stride, const double ref[256]) {
    double dev[256] = {};
    const uint32_t step = std::max(1u, uint32_t(std::sqrt(double(w) * h / 300000.0)));
    for (uint32_t y = 0; y < h; y += step)
        for (uint32_t x = 0; x < w; x += step) dev[Luma(px + size_t(y) * stride + size_t(x) * 4)] += 1.0;
    double cdfRef[256], cdfDev[256], tr = 0, td = 0;
    for (int i = 0; i < 256; ++i) {
        tr += ref[i];
        td += dev[i];
    }
    if (tr <= 0 || td <= 0) return;
    double ar = 0, ad = 0;
    for (int i = 0; i < 256; ++i) {
        ar += ref[i];
        ad += dev[i];
        cdfRef[i] = ar / tr;
        cdfDev[i] = (ad - dev[i] / 2) / td;  // milieu de la classe
    }
    uint8_t lut[256];
    int u = 0;
    for (int v = 0; v < 256; ++v) {
        while (u < 255 && cdfRef[u] < cdfDev[v]) ++u;
        // garde-fou : jamais plus de 2x plus clair ou 2x plus sombre
        lut[v] = uint8_t(std::clamp(u, v / 2, std::min(255, v * 2 + 8)));
    }
    for (int v = 1; v < 256; ++v) lut[v] = std::max(lut[v], lut[v - 1]);  // monotone

    for (int y = 0; y < int(h); ++y) {
        uint8_t* p = px + size_t(y) * stride;
        for (uint32_t x = 0; x < w; ++x, p += 4) {
            p[0] = lut[p[0]];
            p[1] = lut[p[1]];
            p[2] = lut[p[2]];
        }
    }
}

// Image RGB/gris 8 bits de LibRaw -> BGRA de l'hôte
int EmitBitmap(const CyImgHost* host, CyImgContext* ctx, const libraw_processed_image_t* img,
               uint8_t** outPixels = nullptr, uint32_t* outStride = nullptr) {
    if (img->type != LIBRAW_IMAGE_BITMAP || img->bits != 8 || (img->colors != 3 && img->colors != 1)) return CYIMG_E_FAIL;
    uint32_t stride = 0;
    uint8_t* dst = host->alloc_bgra(ctx, img->width, img->height, 0, &stride);
    if (!dst) return CYIMG_E_OUTOFMEMORY;
    if (outPixels) *outPixels = dst;
    if (outStride) *outStride = stride;
    const uint8_t* src = img->data;
    for (uint32_t y = 0; y < img->height; ++y) {
        uint8_t* d = dst + size_t(y) * stride;
        if (img->colors == 3) {
            for (uint32_t x = 0; x < img->width; ++x, src += 3, d += 4) {
                d[0] = src[2];
                d[1] = src[1];
                d[2] = src[0];
                d[3] = 255;
            }
        } else {
            for (uint32_t x = 0; x < img->width; ++x, ++src, d += 4) {
                d[0] = d[1] = d[2] = *src;
                d[3] = 255;
            }
        }
    }
    return CYIMG_OK;
}

// Développement par LibRaw (demi-taille = sans dématriçage, 4x plus rapide).
// refHist : histogramme de l'aperçu de l'appareil, pour caler la tonalité (peut être nul).
int Develop(const CyImgHost* host, CyImgContext* ctx, Raw& raw, bool half, const double* refHist = nullptr) {
    LibRaw& lr = raw.L();
    const uint32_t orientation = raw.Orientation();
    lr.imgdata.params.half_size = half ? 1 : 0;
    lr.imgdata.params.user_flip = 0;  // l'hôte applique l'orientation à l'affichage (pas de copie)
    if (lr.unpack() != LIBRAW_SUCCESS) return host->is_cancelled(ctx) ? CYIMG_E_CANCELLED : CYIMG_E_FAIL;
    if (lr.dcraw_process() != LIBRAW_SUCCESS) return host->is_cancelled(ctx) ? CYIMG_E_CANCELLED : CYIMG_E_FAIL;
    int err = 0;
    libraw_processed_image_t* img = lr.dcraw_make_mem_image(&err);
    if (!img) return CYIMG_E_FAIL;
    uint8_t* px = nullptr;
    uint32_t stride = 0;
    int r = EmitBitmap(host, ctx, img, &px, &stride);
    if (r == CYIMG_OK && refHist) ToneMatch(px, img->width, img->height, stride, refHist);
    LibRaw::dcraw_clear_mem(img);
    if (r == CYIMG_OK) host->set_orientation(ctx, orientation);
    return r;
}

// Miniature fournie par LibRaw (formats dont on ne connaît pas la structure).
int LibRawThumbnail(const CyImgHost* host, CyImgContext* ctx, Raw& raw, uint32_t minSide) {
    LibRaw& lr = raw.L();
    if (lr.imgdata.thumbnail.tlength == 0 || lr.unpack_thumb() != LIBRAW_SUCCESS) return CYIMG_E_FAIL;
    const auto& t = lr.imgdata.thumbnail;
    if (std::max<uint32_t>(t.twidth, t.theight) < minSide) return CYIMG_E_FAIL;
    int err = 0;
    libraw_processed_image_t* img = lr.dcraw_make_mem_thumb(&err);
    if (!img) return CYIMG_E_FAIL;
    int r = img->type == LIBRAW_IMAGE_JPEG ? host->decode_encoded(ctx, img->data, img->data_size)
                                            : EmitBitmap(host, ctx, img);
    LibRaw::dcraw_clear_mem(img);
    if (r == CYIMG_OK) host->set_orientation(ctx, raw.Orientation());
    return r;
}

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    const uint32_t target = host->target_size(ctx);
    const bool best = CYIMG_HOST_HAS(host, quality) && host->quality(ctx) == 1;
    const bool canRefine = CYIMG_HOST_HAS(host, set_refinable);
    cyimg_tr_begin(host, ctx);
    const std::wstring ext = ExtUpper(path);
    wchar_t fmt[128];

    RawScan scan;
    double refHist[256];
    bool haveRef = false;
    {
        RawFile f(path);
        if (!f.Ok()) return CYIMG_E_FAIL;
        ScanRaw(f, scan);

        // Développement complet : histogramme d'un aperçu moyen (≥ 400 px) comme référence de tonalité.
        if (best) {
            const Preview* ref = nullptr;
            for (auto& p : scan.previews) {
                uint32_t side = std::max(p.w, p.h);
                if (side >= 400 && (!ref || side < std::max(ref->w, ref->h))) ref = &p;
            }
            if (ref) {
                std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[ref->len]);
                haveRef = data && f.Read(ref->off, data.get(), ref->len) && PreviewHistogram(data.get(), ref->len, refHist);
            }
        }

        // --- 1. aperçu JPEG embarqué ---
        if (!best && !scan.previews.empty()) {
            auto area = [](const Preview& p) { return uint64_t(p.w) * p.h; };
            const Preview* largest = &scan.previews[0];
            for (auto& p : scan.previews)
                if (area(p) > area(*largest) || (area(p) == area(*largest) && p.len > largest->len)) largest = &p;
            const Preview* pick = largest;
            if (target) {  // vignette : le plus petit aperçu suffisant, au bon ratio (pas de bandes noires)
                const double ar = double(largest->w) / largest->h;
                for (auto& p : scan.previews) {
                    bool sameRatio = std::fabs(double(p.w) / p.h - ar) < 0.03 * ar;
                    if (sameRatio && std::max(p.w, p.h) >= target && area(p) < area(*pick)) pick = &p;
                }
            }

            // Taille réelle du capteur et métadonnées : ouverture LibRaw (structure seulement).
            Raw raw(host, ctx);
            const bool needMeta = target == 0;
            if (needMeta) raw.Open(path);

            std::unique_ptr<uint8_t[]> data(new (std::nothrow) uint8_t[pick->len]);
            if (!data) return CYIMG_E_OUTOFMEMORY;
            if (!f.Read(pick->off, data.get(), pick->len)) return CYIMG_E_FAIL;
            if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
            int r = host->decode_encoded(ctx, data.get(), pick->len);
            if (r == CYIMG_OK) {
                if (scan.orientation >= 1 && scan.orientation <= 8) host->set_orientation(ctx, scan.orientation);
                else if (raw.IsOpen()) host->set_orientation(ctx, raw.Orientation());
                swprintf_s(fmt, CYIMG_TR("plugin.raw.format.preview", L"%ls (aperçu %u×%u)"), ext.c_str(), largest->w, largest->h);
                host->set_format(ctx, fmt);
                if (needMeta) {
                    if (!scan.info.empty()) {
                        for (auto& [k, v] : scan.info) host->set_info(ctx, k.c_str(), v.c_str());
                        if (raw.IsOpen()) raw.AddSensorInfo(host, ctx);
                    } else if (raw.IsOpen()) {
                        raw.AddInfo(host, ctx);
                    }
                    // Aperçu sensiblement plus petit que le capteur : développement complet ensuite.
                    if (canRefine && raw.IsOpen() &&
                        std::max(largest->w, largest->h) < raw.SensorLongSide() * 9 / 10)
                        host->set_refinable(ctx);
                }
                return CYIMG_OK;
            }
            // JPEG illisible : on continue avec LibRaw
        }
    }

    // --- 2. LibRaw ---
    Raw raw(host, ctx);
    if (!raw.Open(path)) return CYIMG_E_UNSUPPORTED;
    if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;

    if (target) {  // vignette : miniature LibRaw, sinon développement demi-taille
        if (LibRawThumbnail(host, ctx, raw, std::min<uint32_t>(target, 160)) == CYIMG_OK) return CYIMG_OK;
        Raw again(host, ctx);  // unpack_thumb a pu consommer l'état : on repart d'une instance propre
        if (!again.Open(path)) return CYIMG_E_FAIL;
        return Develop(host, ctx, again, true);
    }

    // Premier affichage sans aperçu : demi-taille d'abord si l'hôte sait affiner ensuite.
    const bool half = !best && canRefine;
    int r = Develop(host, ctx, raw, half, haveRef ? refHist : nullptr);
    if (r != CYIMG_OK) return r;
    swprintf_s(fmt, half ? CYIMG_TR("plugin.raw.format.half", L"%ls (RAW, demi-taille)") : CYIMG_TR("plugin.raw.format.developed", L"%ls (RAW développé)"), ext.c_str());
    host->set_format(ctx, fmt);
    raw.AddInfo(host, ctx);
    if (half) host->set_refinable(ctx);
    return CYIMG_OK;
}

int Probe(const uint8_t* h, size_t n) {
    if (n < 16) return 0;
    if (h[0] == 'I' && h[1] == 'I' && h[2] == 0x2A && h[3] == 0 && h[8] == 'C' && h[9] == 'R') return 1;  // CR2
    if (!memcmp(h + 4, "ftypcrx ", 8)) return 1;                                                          // CR3
    if (!memcmp(h, "FUJIFILMCCD-RAW", 15)) return 1;                                                       // RAF
    if (h[0] == 'I' && h[1] == 'I' && !memcmp(h + 6, "HEAPCCDR", 8)) return 1;                            // CRW
    if ((h[0] == 'I' && h[1] == 'I' && (h[2] == 'R' || h[2] == 'U')) || (h[0] == 'M' && h[1] == 'M' && h[3] == 'R'))
        return 1;  // ORF, RW2
    return 0;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION,
    L"RAW (LibRaw 0.22)",
    L"2.0",
    L"3fr;ari;arw;bay;cap;cr2;cr3;crw;cs1;dcr;dcs;dng;drf;eip;erf;fff;gpr;iiq;k25;kdc;mdc;mef;mos;mrw;nef;nrw;"
    L"orf;ori;pef;ptx;pxn;qtk;raf;raw;rw2;rwl;rwz;sr2;srf;srw;sti;x3f",
    Probe,
    Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) {
    // Le pool de threads OpenMP (vcomp140.dll) survit aux décodages : on l'épingle pour qu'il ne soit
    // jamais déchargé sous ses threads (plantage à la fermeture sinon).
    HMODULE omp = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"vcomp140.dll", &omp);
    return &g_plugin;
}
