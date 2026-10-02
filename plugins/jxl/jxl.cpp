// Plugin CyImgView : JPEG XL (.jxl), décodé par libjxl (third_party/libjxl, BSD-3).
//
// Fichier lu par mapping mémoire, décodage multithreads (JxlResizableParallelRunner), sortie RGBA
// 8 bits convertie en BGRA prémultiplié. Les images avec perte (XYB) sont converties en sRGB par
// libjxl ; les images sans perte dans un autre espace (Display P3, linéaire, ICC...) sont décodées
// en 16 bits puis converties en sRGB par WIC (libjxl est compilé sans moteur de gestion des couleurs).
// Animations : une image composée par trame (vignette = trame 0). Fichier tronqué : on affiche ce
// qui a pu être décodé (rendu progressif partiel).
// Vignette : décodage complet. L'arrêt au passage DC (JXL_DEC_FRAME_PROGRESSION + JxlDecoderFlushImage)
// a été mesuré plus lent : libjxl ne l'atteint qu'avec des données fournies par tranches (moins de
// parallélisme) et le rendu du DC se fait quand même en pleine résolution (4K : 40 ms contre 33 ms).

#include "cyimg_plugin.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <jxl/decode.h>
#include <jxl/resizable_parallel_runner.h>

#include <cstring>
#include <cwchar>
#include <vector>

namespace {

// --- Fichier projeté en mémoire ----------------------------------------------------------------
struct MappedFile {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool Open(const wchar_t* path) {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0 || uint64_t(sz.QuadPart) > SIZE_MAX) return false;
        map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map) return false;
        data = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
        size = size_t(sz.QuadPart);
        return data != nullptr;
    }
    ~MappedFile() {
        if (data) UnmapViewOfFile(data);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

struct DecoderHolder {
    JxlDecoder* dec = JxlDecoderCreate(nullptr);
    void* runner = JxlResizableParallelRunnerCreate(nullptr);
    ~DecoderHolder() {
        if (dec) JxlDecoderDestroy(dec);
        if (runner) JxlResizableParallelRunnerDestroy(runner);
    }
};

// Conteneur ISOBMFF : présence d'une boîte « jbrd » (reconstruction JPEG) au premier niveau.
// Les boîtes sont parcourues sans lire leur contenu (seules les pages des en-têtes sont touchées).
bool HasJpegReconstruction(const uint8_t* p, size_t n) {
    static const uint8_t kSig[12] = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};
    if (n < 12 || memcmp(p, kSig, 12) != 0) return false;
    size_t pos = 0;
    for (int boxes = 0; pos + 8 <= n && boxes < 4096; ++boxes) {
        uint64_t len = uint64_t(p[pos]) << 24 | uint64_t(p[pos + 1]) << 16 | uint64_t(p[pos + 2]) << 8 | p[pos + 3];
        const uint8_t* type = p + pos + 4;
        if (memcmp(type, "jbrd", 4) == 0) return true;
        if (memcmp(type, "jxlc", 4) == 0) return false;  // jbrd précède le codestream (ou suit le 1er jxlp)
        if (len == 1) {
            if (pos + 16 > n) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = len << 8 | p[pos + 8 + i];
            if (len < 16) return false;
        } else if (len == 0 || len < 8) {
            return false;
        }
        if (len > n - pos) return false;
        pos += size_t(len);
    }
    return false;
}

bool IsSrgb(const JxlColorEncoding& c) {
    return (c.color_space == JXL_COLOR_SPACE_RGB || c.color_space == JXL_COLOR_SPACE_GRAY) &&
           c.white_point == JXL_WHITE_POINT_D65 &&
           (c.color_space == JXL_COLOR_SPACE_GRAY || c.primaries == JXL_PRIMARIES_SRGB) &&
           c.transfer_function == JXL_TRANSFER_FUNCTION_SRGB;
}

const wchar_t* DescribeColor(const JxlColorEncoding& c) {
    if (c.color_space == JXL_COLOR_SPACE_RGB && c.white_point == JXL_WHITE_POINT_D65) {
        const bool srgbTf = c.transfer_function == JXL_TRANSFER_FUNCTION_SRGB;
        if (c.primaries == JXL_PRIMARIES_SRGB)
            return srgbTf ? L"sRGB" : c.transfer_function == JXL_TRANSFER_FUNCTION_LINEAR ? L"sRGB linéaire" : L"sRGB (autre courbe)";
        if (c.primaries == JXL_PRIMARIES_P3) return srgbTf ? L"Display P3" : L"P3";
        if (c.primaries == JXL_PRIMARIES_2100) {
            if (c.transfer_function == JXL_TRANSFER_FUNCTION_PQ) return L"Rec. 2100 PQ (HDR)";
            if (c.transfer_function == JXL_TRANSFER_FUNCTION_HLG) return L"Rec. 2100 HLG (HDR)";
            return L"Rec. 2020";
        }
    }
    if (c.color_space == JXL_COLOR_SPACE_GRAY) return L"Niveaux de gris";
    return nullptr;
}

// Conversion couleur par WIC : RGBA 16 bits (profil ICC source) -> BGRA 8 bits sRGB non prémultiplié.
bool WicToSrgb(const std::vector<uint8_t>& icc, uint16_t* rgba16, uint32_t w, uint32_t h, uint8_t* dst,
               uint32_t dstStride) {
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        IWICImagingFactory* factory = nullptr;
        IWICBitmap* bmp = nullptr;
        IWICColorContext *src = nullptr, *dstCtx = nullptr;
        IWICColorTransform* xf = nullptr;
        const UINT srcStride = w * 8;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat64bppRGBA, srcStride, srcStride * h,
                                                      reinterpret_cast<BYTE*>(rgba16), &bmp)) &&
            SUCCEEDED(factory->CreateColorContext(&src)) && SUCCEEDED(factory->CreateColorContext(&dstCtx)) &&
            SUCCEEDED(src->InitializeFromMemory(icc.data(), UINT(icc.size()))) &&
            SUCCEEDED(dstCtx->InitializeFromExifColorSpace(1)) &&
            SUCCEEDED(factory->CreateColorTransformer(&xf)) &&
            SUCCEEDED(xf->Initialize(bmp, src, dstCtx, GUID_WICPixelFormat32bppBGRA))) {
            ok = SUCCEEDED(xf->CopyPixels(nullptr, dstStride, dstStride * h, dst));
        }
        if (xf) xf->Release();
        if (dstCtx) dstCtx->Release();
        if (src) src->Release();
        if (bmp) bmp->Release();
        if (factory) factory->Release();
    }
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return ok;
}

// RGBA 8 bits (non prémultiplié) -> BGRA prémultiplié, sur place.
void RgbaToBgraPremul(uint8_t* buf, uint32_t w, uint32_t h, uint32_t stride, bool alpha) {
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* p = buf + size_t(y) * stride;
        for (uint32_t x = 0; x < w; ++x, p += 4) {
            const uint8_t r = p[0], b = p[2], a = alpha ? p[3] : 255;
            if (a == 255) {
                p[0] = b, p[2] = r, p[3] = 255;
            } else {
                p[0] = uint8_t((b * a + 127) / 255);
                p[1] = uint8_t((p[1] * a + 127) / 255);
                p[2] = uint8_t((r * a + 127) / 255);
            }
        }
    }
}

// BGRA (non prémultiplié) -> prémultiplié, sur place.
void Premultiply(uint8_t* buf, uint32_t w, uint32_t h, uint32_t stride) {
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* p = buf + size_t(y) * stride;
        for (uint32_t x = 0; x < w; ++x, p += 4) {
            const uint8_t a = p[3];
            if (a != 255) {
                p[0] = uint8_t((p[0] * a + 127) / 255);
                p[1] = uint8_t((p[1] * a + 127) / 255);
                p[2] = uint8_t((p[2] * a + 127) / 255);
            }
        }
    }
}

// Repli sans conversion couleur : RGBA 16 bits -> BGRA 8 bits non prémultiplié.
void Rgba16ToBgra8(const uint16_t* s, uint32_t w, uint32_t h, uint8_t* dst, uint32_t stride, bool alpha) {
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* d = dst + size_t(y) * stride;
        for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) {
            d[0] = uint8_t((s[2] * 255u + 32767u) / 65535u);
            d[1] = uint8_t((s[1] * 255u + 32767u) / 65535u);
            d[2] = uint8_t((s[0] * 255u + 32767u) / 65535u);
            d[3] = alpha ? uint8_t((s[3] * 255u + 32767u) / 65535u) : 255;
        }
    }
}

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    auto fail = [&](const wchar_t* msg) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg);
        return CYIMG_E_FAIL;
    };
    MappedFile f;
    if (!f.Open(path)) return fail(L"Lecture du fichier JPEG XL impossible");
    if (JxlSignatureCheck(f.data, f.size) < JXL_SIG_CODESTREAM) return fail(L"Fichier JPEG XL non reconnu");

    DecoderHolder d;
    if (!d.dec || !d.runner) return CYIMG_E_OUTOFMEMORY;
    JxlDecoder* dec = d.dec;

    const bool thumb = host->target_size(ctx) > 0;
    const bool canAnimate = CYIMG_HOST_HAS(host, add_frame) && CYIMG_HOST_HAS(host, set_frame_delay);
    int events = JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE;

    if (JxlDecoderSubscribeEvents(dec, events) != JXL_DEC_SUCCESS ||
        JxlDecoderSetParallelRunner(dec, JxlResizableParallelRunner, d.runner) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, f.data, f.size) != JXL_DEC_SUCCESS)
        return fail(L"Initialisation du décodeur JPEG XL impossible");
    // Pas de JxlDecoderCloseInput : un fichier tronqué donne JXL_DEC_NEED_MORE_INPUT, et on peut
    // alors afficher le rendu partiel (JxlDecoderFlushImage).

    JxlBasicInfo info{};
    uint32_t W = 0, H = 0;   // dimensions de sortie (orientation appliquée)
    bool alpha = false, animated = false;
    bool direct = true;      // sRGB 8 bits décodé directement dans le tampon de l'hôte
    std::vector<uint8_t> icc;
    std::vector<uint16_t> tmp16;
    const wchar_t* colorName = nullptr;
    bool iccConverted = true;
    uint8_t* out = nullptr;  // tampon BGRA de la trame courante (hôte)
    uint8_t* prev = nullptr; // trame précédente (animation)
    uint32_t stride = 0, prevStride = 0;
    uint32_t frames = 0;     // trames terminées
    bool frameOpen = false;  // tampon de sortie fourni pour la trame courante
    bool partial = false;
    uint32_t pendingDelay = 0;

    // Finalise la trame courante (conversion vers BGRA prémultiplié)
    auto finishFrame = [&]() {
        if (direct) {
            RgbaToBgraPremul(out, W, H, stride, alpha);
        } else {
            if (!WicToSrgb(icc, tmp16.data(), W, H, out, stride)) {
                Rgba16ToBgra8(tmp16.data(), W, H, out, stride, alpha);
                iccConverted = false;
            }
            if (alpha) Premultiply(out, W, H, stride);
        }
        if (animated && canAnimate) host->set_frame_delay(ctx, frames, pendingDelay);
        ++frames;
        frameOpen = false;
    };
    // Trame d'animation déjà ajoutée chez l'hôte mais illisible : copie de la précédente
    auto abandonFrame = [&]() {
        if (!frameOpen || frames == 0 || !prev) return;
        for (uint32_t y = 0; y < H; ++y) memcpy(out + size_t(y) * stride, prev + size_t(y) * prevStride, size_t(W) * 4);
        if (canAnimate) host->set_frame_delay(ctx, frames, pendingDelay);
        ++frames;
        frameOpen = false;
    };

    bool done = false;
    while (!done) {
        if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
        const JxlDecoderStatus st = JxlDecoderProcessInput(dec);
        switch (st) {
        case JXL_DEC_BASIC_INFO: {
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) return fail(L"En-tête JPEG XL invalide");
            W = info.xsize, H = info.ysize;
            if (info.orientation >= JXL_ORIENT_TRANSPOSE) W = info.ysize, H = info.xsize;
            if (!W || !H || uint64_t(W) * H > (1ull << 30)) return fail(L"Dimensions JPEG XL non gérées");
            alpha = info.alpha_bits > 0;
            animated = info.have_animation != 0;
            JxlResizableParallelRunnerSetThreads(d.runner, JxlResizableParallelRunnerSuggestThreads(info.xsize, info.ysize));
            break;
        }
        case JXL_DEC_COLOR_ENCODING: {
            JxlColorEncoding enc{};
            const bool haveEnc = JxlDecoderGetColorAsEncodedProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, &enc) == JXL_DEC_SUCCESS;
            if (haveEnc) colorName = DescribeColor(enc);
            if (!info.uses_original_profile) {
                // XYB : libjxl sait produire directement du sRGB
                JxlColorEncoding srgb{};
                srgb.color_space = info.num_color_channels == 1 ? JXL_COLOR_SPACE_GRAY : JXL_COLOR_SPACE_RGB;
                srgb.white_point = JXL_WHITE_POINT_D65;
                srgb.primaries = JXL_PRIMARIES_SRGB;
                srgb.transfer_function = JXL_TRANSFER_FUNCTION_SRGB;
                srgb.rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;
                if (JxlDecoderSetPreferredColorProfile(dec, &srgb) != JXL_DEC_SUCCESS) iccConverted = false;
            } else if (!haveEnc || !IsSrgb(enc)) {
                size_t iccSize = 0;
                if (JxlDecoderGetICCProfileSize(dec, JXL_COLOR_PROFILE_TARGET_DATA, &iccSize) == JXL_DEC_SUCCESS &&
                    iccSize > 0 && iccSize < (64u << 20)) {
                    icc.resize(iccSize);
                    if (JxlDecoderGetColorAsICCProfile(dec, JXL_COLOR_PROFILE_TARGET_DATA, icc.data(), iccSize) == JXL_DEC_SUCCESS)
                        direct = false;
                }
                if (!colorName) colorName = L"Profil ICC";
            }
            break;
        }
        case JXL_DEC_FRAME: {
            JxlFrameHeader fh{};
            pendingDelay = 0;
            if (animated && JxlDecoderGetFrameHeader(dec, &fh) == JXL_DEC_SUCCESS && info.animation.tps_numerator) {
                const uint64_t ms = uint64_t(fh.duration) * info.animation.tps_denominator * 1000 / info.animation.tps_numerator;
                pendingDelay = ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(ms);
            }
            break;
        }
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            if (frames == 0) {
                out = host->alloc_bgra(ctx, W, H, alpha ? 1 : 0, &stride);
            } else {
                prev = out, prevStride = stride;
                out = canAnimate ? host->add_frame(ctx, &stride) : nullptr;
                if (!out) { done = true; break; }  // l'hôte ne veut plus de trames
            }
            if (!out) return CYIMG_E_OUTOFMEMORY;
            JxlPixelFormat fmt{4, direct ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
            if (direct) {
                if (stride < W * 4) return fail(L"Tampon de sortie invalide");
                fmt.align = stride;  // lignes complétées jusqu'au pas de l'hôte
                if (JxlDecoderSetImageOutBuffer(dec, &fmt, out, size_t(stride) * H) != JXL_DEC_SUCCESS)
                    return fail(L"Format de sortie JPEG XL refusé");
            } else {
                try {
                    tmp16.assign(size_t(W) * H * 4, 0);
                } catch (...) {
                    return CYIMG_E_OUTOFMEMORY;
                }
                if (JxlDecoderSetImageOutBuffer(dec, &fmt, tmp16.data(), tmp16.size() * 2) != JXL_DEC_SUCCESS)
                    return fail(L"Format de sortie JPEG XL refusé");
            }
            frameOpen = true;
            break;
        }
        case JXL_DEC_FULL_IMAGE:
            if (frameOpen) finishFrame();
            if (thumb || !animated || !canAnimate) done = true;
            break;
        case JXL_DEC_SUCCESS:
            done = true;
            break;
        case JXL_DEC_NEED_MORE_INPUT:
            // Fichier tronqué : rendu partiel de la trame en cours si possible
            if (frameOpen && JxlDecoderFlushImage(dec) == JXL_DEC_SUCCESS) {
                finishFrame();
                partial = true;
            } else if (frames > 0) {
                abandonFrame();
                partial = true;
            } else {
                return fail(L"Fichier JPEG XL tronqué");
            }
            done = true;
            break;
        case JXL_DEC_ERROR:
        default:
            if (frames == 0) return fail(L"Fichier JPEG XL invalide ou corrompu");
            abandonFrame();
            partial = true;  // animation : on garde les trames déjà décodées
            done = true;
            break;
        }
    }
    if (frames == 0) return fail(L"Aucune image dans le fichier JPEG XL");

    // --- Informations ---
    host->set_orientation(ctx, 1);  // libjxl applique déjà l'orientation
    const bool jpeg = HasJpegReconstruction(f.data, f.size);
    host->set_format(ctx, jpeg ? L"JPEG XL (JPEG recompressé)" : L"JPEG XL");
    host->set_info(ctx, L"Compression", jpeg ? L"JPEG recompressé (reconstructible à l'identique)"
                                        : !info.uses_original_profile ? L"avec perte (VarDCT/XYB)"
                                                                       : L"sans perte");
    wchar_t buf[96];
    if (info.exponent_bits_per_sample)
        swprintf_s(buf, L"%u bits flottant par composante", info.bits_per_sample);
    else
        swprintf_s(buf, L"%u bits par composante", info.bits_per_sample);
    host->set_info(ctx, L"Profondeur", buf);
    swprintf_s(buf, L"%ls%ls", info.num_color_channels == 1 ? L"niveaux de gris" : L"couleur",
               alpha ? (info.alpha_premultiplied ? L" + alpha (prémultiplié)" : L" + alpha") : L"");
    host->set_info(ctx, L"Canaux", buf);
    if (colorName) {
        swprintf_s(buf, L"%ls%ls", colorName,
                   !iccConverted ? L" (non converti)" : (!direct || !info.uses_original_profile) && wcscmp(colorName, L"sRGB") != 0
                                                            ? L" (converti en sRGB)" : L"");
        host->set_info(ctx, L"Espace couleur", buf);
    }
    if (animated) {
        if (info.animation.num_loops)
            swprintf_s(buf, L"%u trame(s) décodée(s), %u boucle(s)", frames, info.animation.num_loops);
        else
            swprintf_s(buf, L"%u trame(s) décodée(s), en boucle", frames);
        host->set_info(ctx, L"Animation", buf);
    }
    if (partial) host->set_info(ctx, L"Avertissement", L"fichier tronqué ou corrompu : image incomplète");
    return CYIMG_OK;
}

int Probe(const uint8_t* header, size_t size) {
    return JxlSignatureCheck(header, size) >= JXL_SIG_CODESTREAM ? 1 : 0;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"JPEG XL (libjxl 0.12)", L"1.0", L"jxl", Probe, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }
