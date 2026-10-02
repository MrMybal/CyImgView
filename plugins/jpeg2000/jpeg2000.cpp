// Plugin CyImgView : JPEG 2000 (JP2, JPX, codestream J2K, HTJ2K) avec OpenJPEG.
//
// - Conteneur JP2 ou codestream brut détectés par signature (l'extension n'est pas fiable).
// - Vignette : décodage à résolution réduite (opj_set_decoded_resolution_factor) — on saute les
//   derniers niveaux d'ondelettes, donc beaucoup moins de calcul (atout du JPEG 2000).
// - Pleine résolution : décodage multi-threads (moitié des cœurs).
// - Couleurs : sRGB, gris, sYCC/e-YCC, CMYK, composantes sous-échantillonnées, précisions 1..31
//   bits, signées, alpha (cdef), palette (pclr appliquée par OpenJPEG).

#include "cyimg_plugin.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <openjpeg.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

bool ReadAll(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 31);
    if (ok) {
        out.resize(size_t(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(h);
    return ok;
}

const uint8_t kJp2Sig[12] = {0, 0, 0, 0x0C, 'j', 'P', ' ', ' ', 0x0D, 0x0A, 0x87, 0x0A};
const uint8_t kJ2kSig[4] = {0xFF, 0x4F, 0xFF, 0x51};  // SOC + SIZ

// -1 = inconnu
int DetectCodec(const uint8_t* p, size_t n) {
    if (n >= 12 && !memcmp(p, kJp2Sig, 12)) return OPJ_CODEC_JP2;
    if (n >= 4 && !memcmp(p, kJ2kSig, 4)) return OPJ_CODEC_J2K;
    return -1;
}

int Probe(const uint8_t* header, size_t size) { return DetectCodec(header, size) >= 0; }

// --- Flux mémoire pour OpenJPEG ----------------------------------------------------------------
struct MemStream {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

OPJ_SIZE_T MemRead(void* buf, OPJ_SIZE_T n, void* user) {
    auto* m = static_cast<MemStream*>(user);
    if (m->pos >= m->size) return OPJ_SIZE_T(-1);  // fin de flux
    const size_t k = std::min(size_t(n), m->size - m->pos);
    memcpy(buf, m->data + m->pos, k);
    m->pos += k;
    return k;
}
OPJ_OFF_T MemSkip(OPJ_OFF_T n, void* user) {
    auto* m = static_cast<MemStream*>(user);
    if (n < 0) {
        if (size_t(-n) > m->pos) return -1;
    } else if (size_t(n) > m->size - m->pos) {
        n = OPJ_OFF_T(m->size - m->pos);
    }
    m->pos = size_t(OPJ_OFF_T(m->pos) + n);
    return n;
}
OPJ_BOOL MemSeek(OPJ_OFF_T n, void* user) {
    auto* m = static_cast<MemStream*>(user);
    if (n < 0 || size_t(n) > m->size) return OPJ_FALSE;
    m->pos = size_t(n);
    return OPJ_TRUE;
}

// Premier message d'erreur d'OpenJPEG (anglais), ajouté au message affiché
void OnError(const char* msg, void* user) {
    auto* s = static_cast<std::string*>(user);
    if (s->empty() && msg) {
        *s = msg;
        while (!s->empty() && (s->back() == '\n' || s->back() == '\r')) s->pop_back();
    }
}
void OnQuiet(const char*, void*) {}

// Libération automatique des objets OpenJPEG
struct Opj {
    opj_codec_t* codec = nullptr;
    opj_stream_t* stream = nullptr;
    opj_image_t* image = nullptr;
    opj_codestream_info_v2_t* info = nullptr;
    ~Opj() {
        if (info) opj_destroy_cstr_info(&info);
        if (image) opj_image_destroy(image);
        if (stream) opj_stream_destroy(stream);
        if (codec) opj_destroy_codec(codec);
    }
};

// Lecture d'une composante, ramenée sur 8 bits, avec rééchantillonnage au plus proche
struct Plane {
    const OPJ_INT32* data = nullptr;
    uint32_t w = 0, h = 0;
    int offset = 0;     // + 2^(prec-1) si signée
    int maxv = 255;     // 2^prec - 1
    int shift = 0;      // prec > 8 : décalage
    uint8_t lut[256];   // prec < 8 : table d'expansion
    bool small = false;
    std::vector<uint32_t> xmap;

    bool Init(const opj_image_comp_t& c, uint32_t W) {
        if (!c.data || !c.w || !c.h) return false;
        data = c.data;
        w = c.w;
        h = c.h;
        const int prec = std::clamp(int(c.prec), 1, 31);
        offset = c.sgnd ? int(1u << (prec - 1)) : 0;
        maxv = int((1ull << prec) - 1);
        shift = prec > 8 ? prec - 8 : 0;
        small = prec < 8;
        if (small)
            for (int v = 0; v <= maxv; ++v) lut[v] = uint8_t((v * 255 + maxv / 2) / maxv);
        xmap.resize(W);
        for (uint32_t x = 0; x < W; ++x) xmap[x] = uint32_t(uint64_t(x) * w / W);
        return true;
    }
    const OPJ_INT32* Row(uint32_t y, uint32_t H) const { return data + size_t(uint64_t(y) * h / H) * w; }
    uint8_t At(const OPJ_INT32* row, uint32_t x) const {
        int v = row[xmap[x]] + offset;
        v = v < 0 ? 0 : v > maxv ? maxv : v;
        return small ? lut[v] : uint8_t(v >> shift);
    }
};

inline uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    auto fail = [&](const wchar_t* msg, const std::string& detail = {}) {
        if (CYIMG_HOST_HAS(host, set_error)) {
            std::wstring m = msg;
            if (!detail.empty()) {
                m += L" (";
                for (char ch : detail) m += wchar_t(uint8_t(ch));
                m += L")";
            }
            host->set_error(ctx, m.c_str());
        }
        return CYIMG_E_FAIL;
    };

    std::vector<uint8_t> file;
    if (!ReadAll(path, file)) return fail(L"Lecture du fichier JPEG 2000 impossible");
    const int codecId = DetectCodec(file.data(), file.size());
    if (codecId < 0) return fail(L"Fichier JPEG 2000 non reconnu (signature JP2 ou J2K absente)");

    Opj o;
    std::string err, warn;
    o.codec = opj_create_decompress(OPJ_CODEC_FORMAT(codecId));
    if (!o.codec) return CYIMG_E_OUTOFMEMORY;
    opj_set_error_handler(o.codec, OnError, &err);
    opj_set_warning_handler(o.codec, OnError, &warn);
    opj_set_info_handler(o.codec, OnQuiet, nullptr);

    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);
    if (!opj_setup_decoder(o.codec, &params)) return fail(L"Initialisation du décodeur JPEG 2000 impossible", err);
    // Fichiers tronqués : décoder ce qui est disponible plutôt qu'échouer
    opj_decoder_set_strict_mode(o.codec, OPJ_FALSE);

    const uint32_t target = host->target_size(ctx);
    if (target == 0 && opj_has_thread_support()) {
        const int n = std::max(1, int(std::thread::hardware_concurrency() / 2));
        if (n > 1) opj_codec_set_threads(o.codec, n);
    }

    MemStream ms{file.data(), file.size(), 0};
    o.stream = opj_stream_create(1 << 20, OPJ_TRUE);
    if (!o.stream) return CYIMG_E_OUTOFMEMORY;
    opj_stream_set_user_data(o.stream, &ms, nullptr);
    opj_stream_set_user_data_length(o.stream, file.size());
    opj_stream_set_read_function(o.stream, MemRead);
    opj_stream_set_skip_function(o.stream, MemSkip);
    opj_stream_set_seek_function(o.stream, MemSeek);

    if (!opj_read_header(o.stream, o.codec, &o.image) || !o.image)
        return fail(L"En-tête JPEG 2000 invalide", err);
    const opj_image_t* img = o.image;
    if (img->numcomps == 0 || !img->comps || img->x1 <= img->x0 || img->y1 <= img->y0)
        return fail(L"Image JPEG 2000 vide ou invalide");
    const uint32_t fullW = img->x1 - img->x0, fullH = img->y1 - img->y0;

    // Structure du codestream : niveaux de résolution, couches, tuiles
    o.info = opj_get_cstr_info(o.codec);
    uint32_t numRes = 0, numLayers = 0;
    bool ht = false;
    if (o.info && o.info->m_default_tile_info.tccp_info) {
        numLayers = o.info->m_default_tile_info.numlayers;
        numRes = 33;
        for (uint32_t c = 0; c < o.info->nbcomps; ++c) {
            const opj_tccp_info_t& t = o.info->m_default_tile_info.tccp_info[c];
            numRes = std::min(numRes, t.numresolutions);
            ht |= (t.cblksty & 0x40) != 0;  // J2K_CCP_CBLKSTY_HT : blocs HTJ2K (Part 15)
        }
        if (numRes > 32) numRes = 0;
    }

    // Vignette : on retire des niveaux de résolution tant que le grand côté reste >= cible.
    // Image géante (> 65535 px, limite de l'hôte) : réduite de la même façon.
    uint32_t reduce = 0;
    if (numRes > 1) {
        const uint64_t big = std::max(fullW, fullH);
        auto side = [&](uint32_t r) { return (big + (1ull << r) - 1) >> r; };
        while (reduce + 1 < numRes && reduce < 31 &&
               ((target > 0 && side(reduce + 1) >= target) || side(reduce) > 65535))
            ++reduce;
        if (reduce && !opj_set_decoded_resolution_factor(o.codec, reduce)) reduce = 0;
    }

    if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
    if (!opj_decode(o.codec, o.stream, o.image)) return fail(L"Décodage JPEG 2000 impossible", err);
    opj_end_decompress(o.codec, o.stream);
    if (host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;

    // Rôle des composantes
    const uint32_t nc = img->numcomps;
    int alphaIdx = -1;
    bool premul = false;
    for (uint32_t i = 0; i < nc; ++i)
        if (img->comps[i].alpha) {
            alphaIdx = int(i);
            premul = img->comps[i].alpha == 2;
            break;
        }
    std::vector<uint32_t> color;  // composantes de couleur, dans l'ordre
    for (uint32_t i = 0; i < nc; ++i)
        if (int(i) != alphaIdx) color.push_back(i);
    if (color.empty()) return fail(L"Aucune composante de couleur dans l'image JPEG 2000");

    OPJ_COLOR_SPACE cs = img->color_space;
    if (cs == OPJ_CLRSPC_CMYK && color.size() < 4) cs = OPJ_CLRSPC_UNSPECIFIED;
    if ((cs == OPJ_CLRSPC_SYCC || cs == OPJ_CLRSPC_EYCC || cs == OPJ_CLRSPC_SRGB) && color.size() < 3)
        cs = OPJ_CLRSPC_GRAY;
    if (cs != OPJ_CLRSPC_SRGB && cs != OPJ_CLRSPC_GRAY && cs != OPJ_CLRSPC_SYCC && cs != OPJ_CLRSPC_EYCC &&
        cs != OPJ_CLRSPC_CMYK) {
        // Non précisé (codestream brut) : 3 composantes dont la chrominance est sous-échantillonnée = YCbCr
        if (color.size() >= 3 && img->comps[color[0]].dx == 1 && img->comps[color[0]].dy == 1 &&
            (img->comps[color[1]].dx > 1 || img->comps[color[1]].dy > 1))
            cs = OPJ_CLRSPC_SYCC;
        else
            cs = color.size() >= 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;
    }
    // Sans cdef : une composante de plus que nécessaire = alpha (RGBA, gris + alpha)
    const size_t needed = cs == OPJ_CLRSPC_GRAY ? 1 : cs == OPJ_CLRSPC_CMYK ? 4 : 3;
    if (alphaIdx < 0 && color.size() == needed + 1) {
        alphaIdx = int(color.back());
        color.pop_back();
    }

    // Taille de sortie : celle de la composante la plus fine (la plupart du temps la n°0)
    uint32_t W = 0, H = 0;
    for (uint32_t i : color) {
        W = std::max(W, img->comps[i].w);
        H = std::max(H, img->comps[i].h);
    }
    if (!W || !H || uint64_t(W) * H > (1ull << 30)) return fail(L"Dimensions JPEG 2000 non gérées");

    Plane planes[4], alpha;
    const size_t used = std::min(needed, color.size());
    for (size_t k = 0; k < used; ++k)
        if (!planes[k].Init(img->comps[color[k]], W)) return fail(L"Données JPEG 2000 incomplètes", err);
    const bool hasAlpha = alphaIdx >= 0 && alpha.Init(img->comps[alphaIdx], W);

    uint32_t stride = 0;
    uint8_t* out = host->alloc_bgra(ctx, W, H, hasAlpha ? 1 : 0, &stride);
    if (!out) return CYIMG_E_OUTOFMEMORY;
    const bool ycc = cs == OPJ_CLRSPC_SYCC || cs == OPJ_CLRSPC_EYCC;
    for (uint32_t y = 0; y < H; ++y) {
        if ((y & 255) == 0 && host->is_cancelled(ctx)) return CYIMG_E_CANCELLED;
        const OPJ_INT32* r[4] = {};
        for (size_t k = 0; k < used; ++k) r[k] = planes[k].Row(y, H);
        const OPJ_INT32* ra = hasAlpha ? alpha.Row(y, H) : nullptr;
        uint8_t* d = out + size_t(y) * stride;
        for (uint32_t x = 0; x < W; ++x, d += 4) {
            uint8_t R, G, B;
            if (cs == OPJ_CLRSPC_GRAY) {
                R = G = B = planes[0].At(r[0], x);
            } else if (ycc) {  // YCbCr (Rec. 601, plein écart) -> RGB, coefficients en 16.16
                const int Y = planes[0].At(r[0], x), cb = planes[1].At(r[1], x) - 128, cr = planes[2].At(r[2], x) - 128;
                R = Clamp8(Y + ((91881 * cr + 32768) >> 16));
                G = Clamp8(Y - ((22554 * cb + 46802 * cr + 32768) >> 16));
                B = Clamp8(Y + ((116130 * cb + 32768) >> 16));
            } else if (cs == OPJ_CLRSPC_CMYK) {
                const int k = 255 - planes[3].At(r[3], x);
                R = uint8_t((255 - planes[0].At(r[0], x)) * k / 255);
                G = uint8_t((255 - planes[1].At(r[1], x)) * k / 255);
                B = uint8_t((255 - planes[2].At(r[2], x)) * k / 255);
            } else {
                R = planes[0].At(r[0], x);
                G = planes[1].At(r[1], x);
                B = planes[2].At(r[2], x);
            }
            const uint8_t A = hasAlpha ? alpha.At(ra, x) : 255;
            if (A != 255 && !premul) {
                R = uint8_t((R * A + 127) / 255);
                G = uint8_t((G * A + 127) / 255);
                B = uint8_t((B * A + 127) / 255);
            } else if (premul) {  // déjà prémultiplié : garantir couleur <= alpha
                R = std::min(R, A), G = std::min(G, A), B = std::min(B, A);
            }
            d[0] = B, d[1] = G, d[2] = R, d[3] = A;
        }
    }

    // Format et informations
    host->set_format(ctx, ht ? (codecId == OPJ_CODEC_JP2 ? L"HTJ2K (JPH)" : L"HTJ2K (codestream)")
                             : (codecId == OPJ_CODEC_JP2 ? L"JPEG 2000 (JP2)" : L"JPEG 2000 (J2K)"));
    wchar_t buf[128];
    uint32_t prec = 0;
    bool sgnd = false;
    for (uint32_t i = 0; i < nc; ++i) prec = std::max(prec, img->comps[i].prec), sgnd |= img->comps[i].sgnd != 0;
    swprintf_s(buf, L"%ux%u", fullW, fullH);
    host->set_info(ctx, L"Dimensions d'origine", buf);
    swprintf_s(buf, L"%u", nc);
    host->set_info(ctx, L"Composantes", buf);
    swprintf_s(buf, L"%u bits%ls", prec, sgnd ? L" signés" : L"");
    host->set_info(ctx, L"Profondeur", buf);
    const wchar_t* csName = cs == OPJ_CLRSPC_GRAY ? L"Niveaux de gris" : cs == OPJ_CLRSPC_SYCC ? L"sYCC"
                          : cs == OPJ_CLRSPC_EYCC ? L"e-YCC" : cs == OPJ_CLRSPC_CMYK ? L"CMJN" : L"RVB";
    host->set_info(ctx, L"Espace couleur", csName);
    if (img->icc_profile_len) host->set_info(ctx, L"Profil ICC", L"présent (non appliqué)");
    if (numRes) {
        swprintf_s(buf, L"%u", numRes);
        host->set_info(ctx, L"Niveaux de résolution", buf);
    }
    if (numLayers) {
        swprintf_s(buf, L"%u", numLayers);
        host->set_info(ctx, L"Couches de qualité", buf);
    }
    if (o.info && (o.info->tw > 1 || o.info->th > 1)) {
        swprintf_s(buf, L"%u x %u (%ux%u px)", o.info->tw, o.info->th, o.info->tdx, o.info->tdy);
        host->set_info(ctx, L"Tuiles", buf);
    }
    if (o.info && o.info->m_default_tile_info.tccp_info)
        host->set_info(ctx, L"Ondelette", o.info->m_default_tile_info.tccp_info[0].qmfbid == 1
                                              ? L"5/3 réversible (sans perte)" : L"9/7 irréversible");
    if (!warn.empty()) {  // ex. flux tronqué : image décodée partiellement
        std::wstring w;
        for (char ch : warn) w += wchar_t(uint8_t(ch));
        host->set_info(ctx, L"Avertissement", w.c_str());
    }
    if (reduce) {
        swprintf_s(buf, L"1/%u", 1u << reduce);
        host->set_info(ctx, L"Résolution décodée", buf);
    }
    return CYIMG_OK;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"JPEG 2000 (OpenJPEG)", L"1.0", L"jp2;j2k;j2c;jpc;jpf;jpx;jph;jhc", Probe, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }
