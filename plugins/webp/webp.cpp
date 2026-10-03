// Plugin CyImgView : WebP (Google), fixe ou animé, via libwebp.
//
// Image fixe : décodage direct dans le tampon de l'hôte en BGRA prémultiplié (MODE_bgrA), avec
// réduction intégrée au décodeur pour les vignettes (bien plus rapide que réduire après coup).
// Décodeur incrémental : un fichier tronqué affiche les lignes déjà disponibles.
// Animation : WebPAnimDecoder compose chaque image sur le canevas ; image 0 seulement en vignette.

#include "cyimg_plugin.h"
#include "cyimg_plugin_i18n.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {

bool ReadAll(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 12 && sz.QuadPart < (1ll << 31);
    if (ok) {
        out.resize(size_t(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

int Probe(const uint8_t* h, size_t size) {
    return size >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WEBP", 4);
}

const wchar_t* StatusMessage(VP8StatusCode s) {
    switch (s) {
    case VP8_STATUS_OUT_OF_MEMORY: return CYIMG_TR("plugin.webp.err.memory", L"Mémoire insuffisante pour décoder ce WebP");
    case VP8_STATUS_UNSUPPORTED_FEATURE: return CYIMG_TR("plugin.webp.err.unsupported", L"Fonctionnalité WebP non gérée");
    case VP8_STATUS_NOT_ENOUGH_DATA: return CYIMG_TR("plugin.webp.err.truncated", L"Fichier WebP tronqué");
    default: return CYIMG_TR("plugin.webp.err.invalid", L"Fichier WebP invalide ou corrompu");
    }
}

// Compression d'une image (données d'une trame ANMF ou du fichier) : chunks ALPH éventuel puis VP8/VP8L.
// Renvoie L"VP8" ou L"VP8L" (identifiant interne, non affiché).
const wchar_t* FrameCodec(const uint8_t* p, size_t n) {
    while (n >= 8) {
        const uint32_t len = uint32_t(p[4] | p[5] << 8 | p[6] << 16 | uint32_t(p[7]) << 24);
        if (!memcmp(p, "VP8 ", 4)) return L"VP8";
        if (!memcmp(p, "VP8L", 4)) return L"VP8L";
        const size_t step = 8 + size_t(len) + (len & 1);
        if (step > n) break;
        p += step, n -= step;
    }
    return nullptr;
}

bool AnyTransparent(const uint8_t* px, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (px[i * 4 + 3] != 255) return true;
    return false;
}

int DecodeStill(const CyImgHost* host, CyImgContext* ctx, const std::vector<uint8_t>& f,
                const WebPBitstreamFeatures& feat, const std::function<int(const wchar_t*)>& fail) {
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) return fail(CYIMG_TR("plugin.webp.err.libwebp", L"Version de libwebp incompatible"));
    const uint32_t W = uint32_t(feat.width), H = uint32_t(feat.height);
    uint32_t w = W, h = H;
    const uint32_t target = host->target_size(ctx);
    if (target && std::max(W, H) > target) {  // vignette : réduction pendant le décodage
        const double s = double(target) / std::max(W, H);
        w = std::max(1u, uint32_t(W * s + 0.5));
        h = std::max(1u, uint32_t(H * s + 0.5));
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = int(w);
        cfg.options.scaled_height = int(h);
    } else if (!target) {
        cfg.options.use_threads = 1;  // filtrage VP8 dans un second thread (image complète uniquement)
    }
    uint32_t stride = 0;
    uint8_t* out = host->alloc_bgra(ctx, w, h, feat.has_alpha, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    cfg.output.colorspace = MODE_bgrA;
    cfg.output.is_external_memory = 1;
    cfg.output.width = int(w);
    cfg.output.height = int(h);
    cfg.output.u.RGBA.rgba = out;
    cfg.output.u.RGBA.stride = int(stride);
    cfg.output.u.RGBA.size = size_t(stride) * h;

    WebPIDecoder* idec = WebPIDecode(nullptr, 0, &cfg);
    if (!idec) return fail(CYIMG_TR("plugin.webp.err.memory", L"Mémoire insuffisante pour décoder ce WebP"));
    const VP8StatusCode st = WebPIUpdate(idec, f.data(), f.size());
    int lastY = 0;
    if (st == VP8_STATUS_SUSPENDED) WebPIDecGetRGB(idec, &lastY, nullptr, nullptr, nullptr);
    WebPIDelete(idec);
    WebPFreeDecBuffer(&cfg.output);  // mémoire externe : ne libère rien de l'hôte
    if (st == VP8_STATUS_SUSPENDED) {  // fichier tronqué : on garde les lignes décodées
        if (lastY <= 0) return fail(CYIMG_TR("plugin.webp.err.truncated", L"Fichier WebP tronqué"));
        for (uint32_t y = uint32_t(lastY); y < h; ++y) memset(out + size_t(y) * stride, 0, size_t(w) * 4);
        host->set_info(ctx, CYIMG_TR("plugin.webp.info.warning", L"Avertissement"),
                       CYIMG_TR("plugin.webp.warn.truncated", L"fichier tronqué, image incomplète"));
    } else if (st != VP8_STATUS_OK) {
        return fail(StatusMessage(st));
    }
    host->set_format(ctx, L"WebP");
    const wchar_t* codec = feat.format == 1   ? CYIMG_TR("plugin.webp.codec.lossy", L"VP8 (avec perte)")
                           : feat.format == 2 ? CYIMG_TR("plugin.webp.codec.lossless", L"VP8L (sans perte)")
                                              : nullptr;
    if (codec) host->set_info(ctx, CYIMG_TR("plugin.webp.info.compression", L"Compression"), codec);
    if (feat.has_alpha) host->set_info(ctx, CYIMG_TR("plugin.webp.info.alpha", L"Alpha"), CYIMG_TR("plugin.webp.yes", L"oui"));
    return CYIMG_OK;
}

int DecodeAnimation(const CyImgHost* host, CyImgContext* ctx, const std::vector<uint8_t>& f,
                    const WebPBitstreamFeatures& feat, const std::function<int(const wchar_t*)>& fail) {
    WebPData data{f.data(), f.size()};
    // Infos (nombre d'images, durée, compression) via le demux : simple lecture des chunks
    WebPDemuxer* demux = WebPDemux(&data);
    if (!demux) return fail(CYIMG_TR("plugin.webp.err.anim_invalid_truncated", L"Animation WebP invalide ou tronquée"));
    const uint32_t frames = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
    const uint32_t loops = WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT);
    uint64_t totalMs = 0;
    bool lossy = false, lossless = false;
    WebPIterator it;
    if (WebPDemuxGetFrame(demux, 1, &it)) {
        do {
            totalMs += uint32_t(std::max(it.duration, 0));
            const wchar_t* c = FrameCodec(it.fragment.bytes, it.fragment.size);
            if (c && c[3] == L'L') lossless = true;  // L"VP8L"
            else if (c) lossy = true;
        } while (WebPDemuxNextFrame(&it));
        WebPDemuxReleaseIterator(&it);
    }
    WebPDemuxDelete(demux);

    WebPAnimDecoderOptions opt;
    if (!WebPAnimDecoderOptionsInit(&opt)) return fail(CYIMG_TR("plugin.webp.err.libwebp", L"Version de libwebp incompatible"));
    opt.color_mode = MODE_bgrA;
    opt.use_threads = host->target_size(ctx) ? 0 : 1;
    WebPAnimDecoder* dec = WebPAnimDecoderNew(&data, &opt);
    if (!dec) return fail(CYIMG_TR("plugin.webp.err.anim_invalid_truncated", L"Animation WebP invalide ou tronquée"));
    WebPAnimInfo ai;
    int rc = CYIMG_OK;
    if (!WebPAnimDecoderGetInfo(dec, &ai) || !ai.canvas_width || !ai.canvas_height) {
        WebPAnimDecoderDelete(dec);
        return fail(CYIMG_TR("plugin.webp.err.anim_invalid", L"Animation WebP invalide"));
    }
    const size_t frameBytes = size_t(ai.canvas_width) * 4;
    const bool canAnimate = CYIMG_HOST_HAS(host, add_frame) && CYIMG_HOST_HAS(host, set_frame_delay) &&
                            !host->target_size(ctx);
    uint32_t index = 0;
    int prevTs = 0;
    uint8_t* out = nullptr;
    uint32_t stride = 0;
    while (WebPAnimDecoderHasMoreFrames(dec)) {
        if (host->is_cancelled(ctx)) {
            rc = CYIMG_E_CANCELLED;
            break;
        }
        uint8_t* buf = nullptr;
        int ts = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) {
            if (index == 0) rc = fail(CYIMG_TR("plugin.webp.err.anim_corrupt", L"Animation WebP corrompue"));
            break;  // images suivantes illisibles : on garde celles déjà décodées
        }
        if (index == 0) {
            const bool alpha = feat.has_alpha || AnyTransparent(buf, size_t(ai.canvas_width) * ai.canvas_height);
            out = host->alloc_bgra(ctx, ai.canvas_width, ai.canvas_height, alpha, &stride);
        } else {
            out = host->add_frame(ctx, &stride);
        }
        if (!out) {
            if (index == 0) rc = CYIMG_E_OUTOFMEMORY;
            break;
        }
        for (uint32_t y = 0; y < ai.canvas_height; ++y)
            memcpy(out + size_t(y) * stride, buf + y * frameBytes, frameBytes);
        if (canAnimate) host->set_frame_delay(ctx, index, uint32_t(std::max(ts - prevTs, 0)));
        prevTs = ts;
        ++index;
        if (!canAnimate) break;  // vignette : image 0 seulement
    }
    WebPAnimDecoderDelete(dec);
    if (rc != CYIMG_OK) return rc;

    host->set_format(ctx, CYIMG_TR("plugin.webp.format.animated", L"WebP animé"));
    wchar_t s[64];
    host->set_info(ctx, CYIMG_TR("plugin.webp.info.compression", L"Compression"),
                   lossy && lossless ? CYIMG_TR("plugin.webp.codec.mixed", L"mixte (VP8 + VP8L)")
                   : lossless        ? CYIMG_TR("plugin.webp.codec.lossless", L"VP8L (sans perte)")
                                     : CYIMG_TR("plugin.webp.codec.lossy", L"VP8 (avec perte)"));
    swprintf_s(s, L"%u", frames);
    host->set_info(ctx, CYIMG_TR("plugin.webp.info.frames", L"Images"), s);
    swprintf_s(s, L"%.2f s", double(totalMs) / 1000.0);
    host->set_info(ctx, CYIMG_TR("plugin.webp.info.duration", L"Durée"), s);
    if (loops) swprintf_s(s, L"%u", loops);
    host->set_info(ctx, CYIMG_TR("plugin.webp.info.loops", L"Boucles"), loops ? s : CYIMG_TR("plugin.webp.loops.infinite", L"infinies"));
    if (feat.has_alpha) host->set_info(ctx, CYIMG_TR("plugin.webp.info.alpha", L"Alpha"), CYIMG_TR("plugin.webp.yes", L"oui"));
    return CYIMG_OK;
}

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    const std::function<int(const wchar_t*)> fail = [&](const wchar_t* msg) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg);
        return CYIMG_E_FAIL;
    };
    cyimg_tr_begin(host, ctx);
    std::vector<uint8_t> f;
    if (!ReadAll(path, f)) return fail(CYIMG_TR("plugin.webp.err.read", L"Lecture du fichier WebP impossible"));
    if (!Probe(f.data(), f.size())) return fail(CYIMG_TR("plugin.webp.err.unknown", L"Fichier WebP non reconnu"));
    WebPBitstreamFeatures feat;
    const VP8StatusCode st = WebPGetFeatures(f.data(), f.size(), &feat);
    if (st != VP8_STATUS_OK) return fail(StatusMessage(st));
    if (feat.width <= 0 || feat.height <= 0) return fail(CYIMG_TR("plugin.webp.err.dimensions", L"Dimensions WebP invalides"));
    return feat.has_animation ? DecodeAnimation(host, ctx, f, feat, fail) : DecodeStill(host, ctx, f, feat, fail);
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"WebP (libwebp 1.6.0)", L"1.0", L"webp", Probe, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }
