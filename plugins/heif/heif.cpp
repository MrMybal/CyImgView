// Plugin CyImgView : HEIF / HEIC (HEVC) et AVIF (AV1), y compris les séquences animées.
//
// libheif lit le conteneur et applique les transformations (irot/imir/clap) : l'image rendue est
// déjà orientée. Décodeurs intégrés : libde265 (HEVC), dav1d (AV1), codec « non compressé ».
// - Vignette : une vignette intégrée (HEIC d'iPhone : 320x240 environ) assez grande pour la taille
//   demandée est décodée à la place de l'image complète (grille de 48 tuiles 512x512).
// - Grilles de tuiles décodées en parallèle par libheif ; image unique : threads du codec.
// - 10/12 bits (HDR) : réduits à 8 bits par libheif (simple réduction de précision).
// - Séquences (AVIS, HEICS) : toutes les images de la première piste vidéo.

#include "cyimg_plugin.h"
#include "cyimg_plugin_i18n.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <libheif/heif.h>
#include <libheif/heif_aux_images.h>
#include <libheif/heif_brands.h>
#include <libheif/heif_items.h>
#include <libheif/heif_sequences.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// Libération automatique des objets libheif
struct CtxDel { void operator()(heif_context* p) const { heif_context_free(p); } };
struct HandleDel { void operator()(heif_image_handle* p) const { heif_image_handle_release(p); } };
struct ImageDel { void operator()(heif_image* p) const { heif_image_release(p); } };
struct OptDel { void operator()(heif_decoding_options* p) const { heif_decoding_options_free(p); } };
struct TrackDel { void operator()(heif_track* p) const { heif_track_release(p); } };
using CtxPtr = std::unique_ptr<heif_context, CtxDel>;
using HandlePtr = std::unique_ptr<heif_image_handle, HandleDel>;
using ImagePtr = std::unique_ptr<heif_image, ImageDel>;
using OptPtr = std::unique_ptr<heif_decoding_options, OptDel>;
using TrackPtr = std::unique_ptr<heif_track, TrackDel>;

// Initialisation unique (décodeurs intégrés, tables de conversion) ; jamais libérée : le plugin
// reste chargé jusqu'à la fin du processus.
void InitLibrary() {
    static std::once_flag once;
    std::call_once(once, [] { heif_init(nullptr); });
}

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint8_t(d);
}

// Type de codage d'un élément ; pour une grille (ou une superposition), celui de la première tuile.
uint32_t CodingType(heif_context* ctx, heif_item_id id, bool* isGrid) {
    uint32_t type = heif_item_get_item_type(ctx, id);
    if (type == FourCC('g', 'r', 'i', 'd') || type == FourCC('i', 'o', 'v', 'l') || type == FourCC('i', 'd', 'e', 'n')) {
        if (isGrid) *isGrid = type == FourCC('g', 'r', 'i', 'd');
        for (int i = 0; i < 16; ++i) {
            uint32_t refType = 0;
            heif_item_id* refs = nullptr;
            size_t n = heif_context_get_item_references(ctx, id, i, &refType, &refs);
            if (!n) break;
            uint32_t sub = refType == FourCC('d', 'i', 'm', 'g') ? heif_item_get_item_type(ctx, refs[0]) : 0;
            heif_release_item_references(ctx, &refs);
            if (sub) return sub;
        }
    }
    return type;
}

const wchar_t* CodecName(uint32_t type) {
    switch (type) {
    case FourCC('h', 'v', 'c', '1'):
    case FourCC('h', 'e', 'v', '1'): return L"HEVC (H.265)";
    case FourCC('a', 'v', '0', '1'): return L"AV1";
    case FourCC('a', 'v', 'c', '1'): return L"AVC (H.264)";
    case FourCC('v', 'v', 'c', '1'): return L"VVC (H.266)";
    case FourCC('j', 'p', 'e', 'g'): return L"JPEG";
    case FourCC('j', '2', 'k', '1'): return L"JPEG 2000";
    case FourCC('u', 'n', 'c', 'i'):
    case FourCC('u', 'n', 'c', 'v'): return CYIMG_TR("plugin.heif.codec.uncompressed", L"non compressé");
    case FourCC('m', 's', 'k', ' '): return CYIMG_TR("plugin.heif.codec.mask", L"masque");
    default: return nullptr;
    }
}

// Interruption du décodage quand l'hôte n'a plus besoin du résultat
struct CancelInfo {
    const CyImgHost* host;
    CyImgContext* ctx;
};
int CancelCallback(void* user) {
    auto* c = static_cast<const CancelInfo*>(user);
    return c->host->is_cancelled(c->ctx) ? 1 : 0;
}

// Copie d'une image RGB/RGBA entrelacée (8 bits, ou 16 bits petit-boutiste si la réduction n'a pas
// eu lieu) vers le tampon BGRA prémultiplié de l'hôte. Les zones hors image restent transparentes.
bool CopyToBgra(const heif_image* img, bool rgba, uint8_t* out, uint32_t stride, uint32_t W, uint32_t H) {
    const heif_channel ch = heif_channel_interleaved;
    size_t srcStride = 0;
    const uint8_t* src = heif_image_get_plane_readonly2(img, ch, &srcStride);
    if (!src) return false;
    const int iw = heif_image_get_width(img, ch), ih = heif_image_get_height(img, ch);
    if (iw <= 0 || ih <= 0) return false;
    const int bits = heif_image_get_bits_per_pixel_range(img, ch);
    const bool wide = bits > 8;
    const int shift = wide ? std::clamp(bits - 8, 0, 8) : 0;
    const bool premul = rgba && heif_image_is_premultiplied_alpha(const_cast<heif_image*>(img));
    const uint32_t cw = std::min<uint32_t>(W, uint32_t(iw)), chh = std::min<uint32_t>(H, uint32_t(ih));
    const size_t comps = rgba ? 4 : 3;
    if (cw < W || chh < H)
        for (uint32_t y = 0; y < H; ++y) memset(out + size_t(y) * stride, 0, size_t(W) * 4);

    for (uint32_t y = 0; y < chh; ++y) {
        const uint8_t* s = src + size_t(y) * srcStride;
        uint8_t* d = out + size_t(y) * stride;
        for (uint32_t x = 0; x < cw; ++x, d += 4) {
            uint32_t r, g, b, a;
            if (!wide) {
                const uint8_t* p = s + x * comps;
                r = p[0], g = p[1], b = p[2], a = rgba ? p[3] : 255;
            } else {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(s) + x * comps;
                auto v = [&](uint16_t c) { return std::min<uint32_t>(uint32_t(c) >> shift, 255); };
                r = v(p[0]), g = v(p[1]), b = v(p[2]), a = rgba ? v(p[3]) : 255;
            }
            if (a < 255 && !premul) {
                // division arrondie par 255
                r = (r * a + 128) * 257 >> 16, g = (g * a + 128) * 257 >> 16, b = (b * a + 128) * 257 >> 16;
            }
            d[0] = uint8_t(b), d[1] = uint8_t(g), d[2] = uint8_t(r), d[3] = uint8_t(a);
        }
    }
    return true;
}

// Au moins un pixel non opaque dans une image RGBA 8 bits entrelacée
bool HasTransparency(const heif_image* img) {
    size_t stride = 0;
    const uint8_t* p = heif_image_get_plane_readonly2(img, heif_channel_interleaved, &stride);
    const int w = heif_image_get_width(img, heif_channel_interleaved), h = heif_image_get_height(img, heif_channel_interleaved);
    if (!p || w <= 0 || h <= 0 || heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved) != 8) return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (p[size_t(y) * stride + size_t(x) * 4 + 3] != 255) return true;
    return false;
}

unsigned CpuCount() {
    unsigned n = std::thread::hardware_concurrency();
    return std::clamp(n, 1u, 16u);
}

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    auto fail = [&](const std::wstring& msg) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg.c_str());
        return CYIMG_E_FAIL;
    };
    auto heifFail = [&](const wchar_t* what, const heif_error& err) {
        if (err.code == heif_error_Canceled) return CYIMG_E_CANCELLED;
        std::wstring m = what;
        std::wstring detail = Widen(err.message);
        while (!detail.empty() && iswspace(detail.back())) detail.pop_back();
        if (!detail.empty() && detail != L"Success") {  // message libheif (anglais) en complément
            wchar_t buf[512];
            swprintf_s(buf, CYIMG_TR("plugin.heif.err.detail", L"%ls : %ls"), what, detail.c_str());
            m = buf;
        }
        fail(m);
        // Limites de sécurité de libheif (dimensions ou mémoire déraisonnables) : message conservé
        return err.code == heif_error_Memory_allocation_error ? CYIMG_E_OUTOFMEMORY : CYIMG_E_FAIL;
    };

    cyimg_tr_begin(host, ctx);
    std::vector<uint8_t> file;
    if (!ReadAll(path, file)) return fail(CYIMG_TR("plugin.heif.err.read", L"Lecture du fichier HEIF impossible"));
    if (heif_check_filetype(file.data(), int(std::min<size_t>(file.size(), 1 << 20))) == heif_filetype_no)
        return fail(CYIMG_TR("plugin.heif.err.not_heif", L"Ce fichier n'est pas au format HEIF/AVIF"));

    InitLibrary();
    CtxPtr hctx(heif_context_alloc());
    if (!hctx) return CYIMG_E_OUTOFMEMORY;
    heif_error err = heif_context_read_from_memory_without_copy(hctx.get(), file.data(), file.size(), nullptr);
    if (err.code != heif_error_Ok) return heifFail(CYIMG_TR("plugin.heif.err.unreadable", L"Fichier HEIF illisible"), err);

    const uint32_t target = host->target_size(ctx);
    const unsigned cpus = CpuCount();
    // Vignettes : l'hôte en décode déjà plusieurs en parallèle
    const int threads = target ? std::min(4u, cpus) : int(cpus);
    heif_context_set_max_decoding_threads(hctx.get(), threads);

    OptPtr opt(heif_decoding_options_alloc());
    if (!opt) return CYIMG_E_OUTOFMEMORY;
    CancelInfo cancel{host, ctx};
    opt->convert_hdr_to_8bit = 1;
    opt->progress_user_data = &cancel;
    opt->cancel_decoding = CancelCallback;
    opt->ignore_sequence_editlist = 1;  // une seule lecture (sinon boucle infinie possible)

    // --- Image primaire et informations
    HandlePtr primary;
    {
        heif_image_handle* h = nullptr;
        if (heif_context_get_primary_image_handle(hctx.get(), &h).code == heif_error_Ok) primary.reset(h);
    }
    const bool hasSequence = heif_context_has_sequence(hctx.get()) != 0;
    if (!primary && !hasSequence) return fail(CYIMG_TR("plugin.heif.err.no_image", L"Aucune image dans ce fichier HEIF"));

    bool isGrid = false;
    uint32_t codingType = 0;
    if (primary) codingType = CodingType(hctx.get(), heif_image_handle_get_item_id(primary.get()), &isGrid);
    TrackPtr track;
    if (hasSequence) {
        track.reset(heif_context_get_track(hctx.get(), 0));
        if (!track && !primary) return fail(CYIMG_TR("plugin.heif.err.no_track", L"Aucune piste vidéo dans cette séquence HEIF"));
        if (track && !codingType) codingType = heif_track_get_sample_entry_type_of_first_cluster(track.get());
    }
    const bool av1 = codingType == FourCC('a', 'v', '0', '1') ||
                     heif_read_main_brand(file.data(), int(std::min<size_t>(file.size(), 64))) == heif_brand2_avif ||
                     heif_read_main_brand(file.data(), int(std::min<size_t>(file.size(), 64))) == heif_brand2_avis;
    const bool hevc = codingType == FourCC('h', 'v', 'c', '1');
    host->set_format(ctx, av1 ? L"AVIF" : hevc ? L"HEIC" : L"HEIF");
    if (const wchar_t* cn = CodecName(codingType)) host->set_info(ctx, CYIMG_TR("plugin.heif.info.codec", L"Codec"), cn);
    // Grille : plusieurs tuiles décodées en parallèle, un thread par tuile. Image unique : threads du codec.
    opt->num_codec_threads = isGrid ? 1 : threads;

    wchar_t buf[96];
    if (primary) {
        const int bits = heif_image_handle_get_luma_bits_per_pixel(primary.get());
        if (bits > 8) swprintf_s(buf, CYIMG_TR("plugin.heif.depth.reduced", L"%d bits (affiché en 8 bits)"), bits);
        else if (bits > 0) swprintf_s(buf, CYIMG_TR("plugin.heif.depth.bits", L"%d bits"), bits);
        else buf[0] = 0;
        if (buf[0]) host->set_info(ctx, CYIMG_TR("plugin.heif.info.depth", L"Profondeur"), buf);
        if (isGrid) host->set_info(ctx, CYIMG_TR("plugin.heif.info.structure", L"Structure"), CYIMG_TR("plugin.heif.structure.grid", L"grille de tuiles"));
    }
    const int topLevel = heif_context_get_number_of_top_level_images(hctx.get());
    if (topLevel > 1) {
        swprintf_s(buf, L"%d", topLevel);
        host->set_info(ctx, CYIMG_TR("plugin.heif.info.images", L"Images dans le fichier"), buf);
    }

    // Vignette intégrée la plus petite couvrant la taille demandée
    HandlePtr thumb;
    if (primary) {
        const int nThumbs = heif_image_handle_get_number_of_thumbnails(primary.get());
        if (nThumbs > 0) {
            std::vector<heif_item_id> thumbIds(static_cast<size_t>(nThumbs));
            const int got = heif_image_handle_get_list_of_thumbnail_IDs(primary.get(), thumbIds.data(), nThumbs);
            int bestSide = 0, maxW = 0, maxH = 0;
            for (int i = 0; i < got; ++i) {
                heif_image_handle* t = nullptr;
                if (heif_image_handle_get_thumbnail(primary.get(), thumbIds[size_t(i)], &t).code != heif_error_Ok) continue;
                HandlePtr th(t);
                const int tw = heif_image_handle_get_width(t), tht = heif_image_handle_get_height(t);
                if (tw * tht > maxW * maxH) maxW = tw, maxH = tht;
                const int side = std::max(tw, tht);
                if (target && side >= int(target) && (!thumb || side < bestSide)) {
                    bestSide = side;
                    thumb = std::move(th);
                }
            }
            swprintf_s(buf, CYIMG_TR("plugin.heif.thumb.yes", L"oui (%dx%d)"), maxW, maxH);
            host->set_info(ctx, CYIMG_TR("plugin.heif.info.thumbnail", L"Vignette intégrée"), buf);
        } else {
            host->set_info(ctx, CYIMG_TR("plugin.heif.info.thumbnail", L"Vignette intégrée"), CYIMG_TR("plugin.heif.no", L"non"));
        }
    }

    // Orientation déjà appliquée par libheif (irot/imir)
    host->set_orientation(ctx, 1);

    // --- Séquence animée : toutes les images de la piste (sauf vignette demandée avec image primaire)
    const bool canAnimate = CYIMG_HOST_HAS(host, add_frame) && CYIMG_HOST_HAS(host, set_frame_delay);
    if (track && (!primary || (!target && canAnimate))) {
        bool alpha = heif_track_has_alpha_channel(track.get()) != 0;
        const uint32_t timescale = std::max<uint32_t>(heif_track_get_timescale(track.get()), 1);
        uint8_t* out = nullptr;
        uint32_t W = 0, H = 0, stride = 0, frames = 0;
        for (;;) {
            if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
            heif_image* raw = nullptr;
            err = heif_track_decode_next_image(track.get(), &raw, heif_colorspace_RGB, heif_chroma_interleaved_RGBA,
                                               opt.get());
            ImagePtr img(raw);
            if (err.code == heif_error_End_of_sequence) break;
            if (err.code != heif_error_Ok || !img) {
                if (frames) break;  // séquence tronquée : on garde les images déjà décodées
                if (primary) goto still;
                return heifFail(CYIMG_TR("plugin.heif.err.sequence_decode", L"Décodage de la séquence HEIF impossible"), err);
            }
            if (!frames) {
                W = uint32_t(heif_image_get_primary_width(img.get()));
                H = uint32_t(heif_image_get_primary_height(img.get()));
                if (!W || !H) return fail(CYIMG_TR("plugin.heif.err.dimensions", L"Dimensions d'image HEIF invalides"));
                if (!alpha) alpha = HasTransparency(img.get());  // RGBA non compressé : pas de piste auxiliaire
                out = host->alloc_bgra(ctx, W, H, alpha ? 1 : 0, &stride);
                if (!out) return CYIMG_E_OUTOFMEMORY;
            } else {
                if (target || !canAnimate) break;
                out = host->add_frame(ctx, &stride);
                if (!out) break;
            }
            if (!CopyToBgra(img.get(), true, out, stride, W, H)) return fail(CYIMG_TR("plugin.heif.err.unusable", L"Image HEIF décodée inutilisable"));
            if (canAnimate) {
                const uint64_t ms = uint64_t(heif_image_get_duration(img.get())) * 1000 / timescale;
                host->set_frame_delay(ctx, frames, uint32_t(std::clamp<uint64_t>(ms, 10, 60000)));
            }
            ++frames;
            if (!canAnimate || target) break;
        }
        if (!frames) {
            if (primary) goto still;
            return fail(CYIMG_TR("plugin.heif.err.empty_sequence", L"Séquence HEIF vide"));
        }
        if (frames > 1) {
            swprintf_s(buf, L"%u", frames);
            host->set_info(ctx, CYIMG_TR("plugin.heif.info.frames", L"Images animées"), buf);
        }
        return CYIMG_OK;
    }

still:
    // --- Image fixe (primaire ou vignette)
    {
        heif_image_handle* handle = thumb ? thumb.get() : primary.get();
        bool alpha = heif_image_handle_has_alpha_channel(handle) != 0;
        auto decodeHandle = [&](int codecThreads) {
            opt->num_codec_threads = codecThreads;
            heif_image* raw = nullptr;
            err = heif_decode_image(handle, &raw, heif_colorspace_RGB,
                                    alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB, opt.get());
            return ImagePtr(raw);
        };
        const int fullThreads = opt->num_codec_threads;
        ImagePtr img = decodeHandle(thumb ? 1 : fullThreads);
        if (thumb && (err.code != heif_error_Ok || !img) && err.code != heif_error_Canceled) {
            // Vignette intégrée illisible (fichier tronqué...) : image complète
            thumb.reset();
            handle = primary.get();
            alpha = heif_image_handle_has_alpha_channel(handle) != 0;
            img = decodeHandle(fullThreads);
        }
        if (err.code != heif_error_Ok || !img) {
            if (err.code == heif_error_Unsupported_feature && err.subcode == heif_suberror_Unsupported_codec) {
                const wchar_t* cn = CodecName(codingType);
                if (!cn) return fail(CYIMG_TR("plugin.heif.err.codec", L"Codec HEIF non pris en charge"));
                wchar_t msg[128];
                swprintf_s(msg, CYIMG_TR("plugin.heif.err.codec_named", L"Codec HEIF non pris en charge : %ls"), cn);
                return fail(msg);
            }
            if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
            return heifFail(CYIMG_TR("plugin.heif.err.decode", L"Décodage HEIF impossible"), err);
        }
        const int w = heif_image_get_width(img.get(), heif_channel_interleaved);
        const int h = heif_image_get_height(img.get(), heif_channel_interleaved);
        if (w <= 0 || h <= 0) return fail(CYIMG_TR("plugin.heif.err.dimensions", L"Dimensions d'image HEIF invalides"));
        uint32_t stride = 0;
        uint8_t* out = host->alloc_bgra(ctx, uint32_t(w), uint32_t(h), alpha ? 1 : 0, &stride);
        if (!out) return CYIMG_E_OUTOFMEMORY;
        if (!CopyToBgra(img.get(), alpha, out, stride, uint32_t(w), uint32_t(h)))
            return fail(CYIMG_TR("plugin.heif.err.unusable", L"Image HEIF décodée inutilisable"));
        if (alpha) host->set_info(ctx, CYIMG_TR("plugin.heif.info.alpha", L"Alpha"), CYIMG_TR("plugin.heif.yes", L"oui"));
        if (thumb) host->set_info(ctx, CYIMG_TR("plugin.heif.info.source", L"Source"), CYIMG_TR("plugin.heif.source.thumbnail", L"vignette intégrée"));
    }
    return CYIMG_OK;
}

int DecodeSafe(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    try {
        return Decode(host, ctx, path);
    } catch (const std::bad_alloc&) {
        return CYIMG_E_OUTOFMEMORY;
    } catch (...) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, cyimg_tr(host, ctx, "plugin.heif.err.internal", L"Erreur interne du décodeur HEIF"));
        return CYIMG_E_FAIL;
    }
}

// Boîte ftyp : marque principale ou compatible HEIF/AVIF (images ou séquences)
int Probe(const uint8_t* header, size_t size) {
    if (size < 16 || memcmp(header + 4, "ftyp", 4) != 0) return 0;
    const size_t boxSize = std::min<size_t>(size_t(header[0]) << 24 | size_t(header[1]) << 16 | size_t(header[2]) << 8 | header[3], size);
    static const char* const kBrands[] = {"heic", "heix", "hevc", "hevx", "heim", "heis", "hevm", "hevs",
                                          "mif1", "mif2", "msf1", "avif", "avis"};
    for (size_t off = 8; off + 4 <= boxSize; off += 4) {
        if (off == 12) continue;  // version mineure
        for (const char* b : kBrands)
            if (memcmp(header + off, b, 4) == 0) return 1;
    }
    return 0;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"HEIF / HEIC / AVIF (libheif)", L"1.0", L"avif;avifs;heic;heif;heics;heifs;hif", Probe, DecodeSafe,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }
