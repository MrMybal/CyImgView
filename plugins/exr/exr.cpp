// Plugin CyImgView : OpenEXR (images HDR en virgule flottante) avec la bibliothèque C OpenEXRCore.
//
// - Scanlines et tuiles, toutes les compressions d'OpenEXRCore (RLE, ZIP(S), PIZ, PXR24, B44(A),
//   DWAA/DWAB, HTJ2K, ZSTD).
// - Canaux R,G,B,A (calque principal, sinon le premier calque « xxx.R »), luminance/chrominance
//   Y/RY/BY, ou un seul canal en niveaux de gris. Partie 0 uniquement pour les fichiers multiparties.
// - Affichage : exposition 0, linéaire -> sRGB avec écrêtage ; l'alpha EXR est déjà prémultiplié.
// - Vignette : une ligne et une colonne sur N, les blocs (chunks) sans ligne utile ne sont pas lus ;
//   niveau de mipmap adapté si le fichier en contient.
// - Pleine résolution : blocs répartis entre plusieurs threads (lecture thread-safe d'OpenEXRCore).

#include "cyimg_plugin.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <openexr.h>
#include <openjph/ojph_message.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// --- Fichier projeté en mémoire ----------------------------------------------------------------
struct Mapped {
    HANDLE file = INVALID_HANDLE_VALUE, map = nullptr;
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    bool Open(const wchar_t* path) {
        file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0) return false;
        size = uint64_t(sz.QuadPart);
        map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!map) return false;
        data = static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
        return data != nullptr;
    }
    ~Mapped() {
        if (data) UnmapViewOfFile(data);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

// Copie protégée : une erreur d'E/S sur une vue projetée lève une exception structurée
bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

struct Io {
    const uint8_t* data;
    uint64_t size;
    std::mutex mu;
    std::string firstError;
};

int64_t ReadFn(exr_const_context_t, void* user, void* buf, uint64_t sz, uint64_t off, exr_stream_error_func_ptr_t) {
    auto* io = static_cast<Io*>(user);
    if (off >= io->size) return 0;
    const uint64_t n = std::min(sz, io->size - off);
    return SafeCopy(buf, io->data + off, size_t(n)) ? int64_t(n) : -1;
}
int64_t SizeFn(exr_const_context_t, void* user) { return int64_t(static_cast<Io*>(user)->size); }

void ErrorFn(exr_const_context_t ctxt, exr_result_t, const char* msg) {
    void* user = nullptr;
    if (exr_get_user_data(ctxt, &user) != EXR_ERR_SUCCESS || !user || !msg) return;
    auto* io = static_cast<Io*>(user);
    std::lock_guard<std::mutex> lock(io->mu);
    if (io->firstError.empty()) io->firstError = msg;
}

int Probe(const uint8_t* h, size_t n) { return n >= 4 && h[0] == 0x76 && h[1] == 0x2F && h[2] == 0x31 && h[3] == 0x01; }

std::wstring Widen(const std::string& s) {
    std::wstring w;
    if (s.empty()) return w;
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0) {
        for (char c : s) w += wchar_t(uint8_t(c));
        return w;
    }
    w.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// --- Linéaire -> sRGB 8 bits ---------------------------------------------------------------------
constexpr int kLutSize = 16384;
struct SrgbLut {
    uint8_t v[kLutSize + 1];
    SrgbLut() {
        for (int i = 0; i <= kLutSize; ++i) {
            const double x = (i + 0.5) / kLutSize;
            const double s = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
            v[i] = uint8_t(std::clamp(int(s * 255 + 0.5), 0, 255));
        }
    }
};
const SrgbLut& Lut() {
    static const SrgbLut lut;  // initialisation thread-safe (C++11)
    return lut;
}
inline uint8_t ToSrgb(const SrgbLut& l, float v) {
    if (!(v > 0)) return 0;  // aussi NaN
    if (v >= 1) return 255;
    return l.v[int(v * kLutSize)];
}

// --- Choix des canaux -------------------------------------------------------------------------
enum class Mode { Rgb, Yc, Gray };
struct Pick {
    Mode mode = Mode::Gray;
    int idx[4] = {-1, -1, -1, -1};  // indices dans la liste des canaux : 3 couleurs (ou Y,RY,BY / gris) + alpha
    std::string layer;
};

bool IEquals(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

Pick ChooseChannels(const exr_attr_chlist_t* cl) {
    std::vector<std::string> layer(size_t(cl->num_channels)), base(size_t(cl->num_channels));
    std::vector<std::string> layers;
    for (int i = 0; i < cl->num_channels; ++i) {
        std::string n = cl->entries[i].name.str ? std::string(cl->entries[i].name.str, size_t(cl->entries[i].name.length)) : "";
        const size_t dot = n.rfind('.');
        layer[size_t(i)] = dot == std::string::npos ? "" : n.substr(0, dot);
        base[size_t(i)] = dot == std::string::npos ? n : n.substr(dot + 1);
        if (std::find(layers.begin(), layers.end(), layer[size_t(i)]) == layers.end()) layers.push_back(layer[size_t(i)]);
    }
    std::stable_partition(layers.begin(), layers.end(), [](const std::string& l) { return l.empty(); });
    auto find = [&](const std::string& l, const char* b) {
        for (int i = 0; i < cl->num_channels; ++i)
            if (layer[size_t(i)] == l && IEquals(base[size_t(i)], b)) return i;
        return -1;
    };
    Pick p;
    for (const std::string& l : layers) {
        const int r = find(l, "R"), g = find(l, "G"), b = find(l, "B"), y = find(l, "Y");
        if (r >= 0 || g >= 0 || b >= 0) {
            p.mode = Mode::Rgb, p.idx[0] = r, p.idx[1] = g, p.idx[2] = b;
        } else if (y >= 0) {
            const int ry = find(l, "RY"), by = find(l, "BY");
            if (ry >= 0 && by >= 0) p.mode = Mode::Yc, p.idx[0] = y, p.idx[1] = ry, p.idx[2] = by;
            else p.mode = Mode::Gray, p.idx[0] = y;
        } else {
            continue;
        }
        p.layer = l;
        p.idx[3] = find(l, "A");
        if (p.idx[3] < 0 && !l.empty()) p.idx[3] = find("", "A");
        return p;
    }
    // Aucun canal reconnu : premier canal en niveaux de gris (profondeur, masque...)
    p.mode = Mode::Gray;
    p.idx[0] = 0;
    p.layer = layer.empty() ? "" : layer[0];
    const int a = find(p.layer, "A");
    if (a > 0) p.idx[3] = a;
    return p;
}

const wchar_t* CompressionName(exr_compression_t c) {
    switch (c) {
    case EXR_COMPRESSION_NONE: return L"aucune";
    case EXR_COMPRESSION_RLE: return L"RLE";
    case EXR_COMPRESSION_ZIPS: return L"ZIPS (1 ligne)";
    case EXR_COMPRESSION_ZIP: return L"ZIP (16 lignes)";
    case EXR_COMPRESSION_PIZ: return L"PIZ";
    case EXR_COMPRESSION_PXR24: return L"PXR24";
    case EXR_COMPRESSION_B44: return L"B44";
    case EXR_COMPRESSION_B44A: return L"B44A";
    case EXR_COMPRESSION_DWAA: return L"DWAA";
    case EXR_COMPRESSION_DWAB: return L"DWAB";
    case EXR_COMPRESSION_HTJ2K256: return L"HTJ2K (256 lignes)";
    case EXR_COMPRESSION_HTJ2K32: return L"HTJ2K (32 lignes)";
    case EXR_COMPRESSION_LJ2K: return L"HTJ2K avec pertes";
    case EXR_COMPRESSION_ZSTD: return L"ZSTD";
    default: return L"inconnue";
    }
}

inline int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int CeilDiv(int a, int b) { return -FloorDiv(-a, b); }

// --- Décodage ---------------------------------------------------------------------------------
struct Job {
    exr_context_t exr = nullptr;
    const CyImgHost* host = nullptr;
    CyImgContext* ctx = nullptr;
    Pick pick;
    std::string names[4];  // noms complets des canaux choisis ("" = absent)
    int xs[4] = {1, 1, 1, 1}, ys[4] = {1, 1, 1, 1};
    bool tiled = false;
    int lx = 0, ly = 0;          // niveau de tuiles
    int tileW = 0, tileH = 0;
    exr_attr_box2i_t dw{};
    int N = 1;                   // pas d'échantillonnage (vignette)
    uint32_t outW = 0, outH = 0, stride = 0;
    uint8_t* out = nullptr;
    bool alpha = false;
    struct Chunk { int a, b; };  // scanline : y absolu ; tuile : tx, ty
    std::vector<Chunk> chunks;
    std::atomic<size_t> next{0};
    std::atomic<int> failed{0};
    std::atomic<bool> cancelled{false};
    std::mutex mu;
    float maxLum = 0;
};

void Worker(Job& job) {
    const SrgbLut& lut = Lut();
    exr_decode_pipeline_t dec = EXR_DECODE_PIPELINE_INITIALIZER;
    bool init = false;
    std::vector<float> buf[4];
    float maxLum = 0;
    auto reset = [&]() {
        if (init) exr_decoding_destroy(job.exr, &dec);
        dec = EXR_DECODE_PIPELINE_INITIALIZER;
        init = false;
    };
    try {
        for (;;) {
            const size_t ci = job.next.fetch_add(1);
            if (ci >= job.chunks.size()) break;
            if (job.cancelled.load() || job.host->is_cancelled(job.ctx)) {
                job.cancelled = true;
                break;
            }
            const Job::Chunk& ck = job.chunks[ci];
            exr_chunk_info_t cinfo{};
            exr_result_t rv = job.tiled ? exr_read_tile_chunk_info(job.exr, 0, ck.a, ck.b, job.lx, job.ly, &cinfo)
                                        : exr_read_scanline_chunk_info(job.exr, 0, ck.a, &cinfo);
            if (rv == EXR_ERR_SUCCESS) {
                if (!init) {
                    rv = exr_decoding_initialize(job.exr, 0, &cinfo, &dec);
                    init = rv == EXR_ERR_SUCCESS;
                } else {
                    rv = exr_decoding_update(job.exr, 0, &cinfo, &dec);
                }
            }
            int chW[4] = {}, chH[4] = {};
            if (rv == EXR_ERR_SUCCESS) {
                for (int c = 0; c < dec.channel_count; ++c) {
                    exr_coding_channel_info_t& ch = dec.channels[c];
                    ch.user_data_type = EXR_PIXEL_FLOAT;
                    ch.user_bytes_per_element = 4;
                    ch.user_pixel_stride = 4;
                    ch.user_line_stride = 4 * std::max(ch.width, 1);
                    ch.decode_to_ptr = nullptr;
                    for (int k = 0; k < 4; ++k)
                        if (!job.names[k].empty() && ch.channel_name && job.names[k] == ch.channel_name &&
                            ch.width > 0 && ch.height > 0) {
                            buf[k].resize(size_t(ch.width) * size_t(ch.height));
                            ch.decode_to_ptr = reinterpret_cast<uint8_t*>(buf[k].data());
                            chW[k] = ch.width, chH[k] = ch.height;
                        }
                }
                rv = exr_decoding_choose_default_routines(job.exr, 0, &dec);
                if (rv == EXR_ERR_SUCCESS) rv = exr_decoding_run(job.exr, 0, &dec);
            }
            if (rv != EXR_ERR_SUCCESS) {
                ++job.failed;
                reset();
                continue;
            }

            // Rectangle du bloc dans le niveau (coordonnées relatives à dataWindow)
            int rx0, ry0;
            if (job.tiled) rx0 = cinfo.start_x * job.tileW, ry0 = cinfo.start_y * job.tileH;
            else rx0 = 0, ry0 = cinfo.start_y - job.dw.min.y;
            const int cw = cinfo.width, chh = cinfo.height;
            const int N = job.N;
            const int firstY = (ry0 + N - 1) / N * N, firstX = (rx0 + N - 1) / N * N;
            for (int ry = firstY; ry < ry0 + chh; ry += N) {
                const uint32_t oy = uint32_t(ry / N);
                if (oy >= job.outH) break;
                const float* row[4] = {};
                for (int k = 0; k < 4; ++k) {
                    if (!chW[k]) continue;
                    int r = ry - ry0;
                    if (job.ys[k] > 1) {  // canal sous-échantillonné (scanlines uniquement)
                        const int ay = ry + job.dw.min.y;
                        r = FloorDiv(ay, job.ys[k]) - CeilDiv(cinfo.start_y, job.ys[k]);
                    }
                    row[k] = buf[k].data() + size_t(std::clamp(r, 0, chH[k] - 1)) * size_t(chW[k]);
                }
                uint8_t* d = job.out + size_t(oy) * job.stride;
                for (int rx = firstX; rx < rx0 + cw; rx += N) {
                    const uint32_t ox = uint32_t(rx / N);
                    if (ox >= job.outW) break;
                    float s[4] = {0, 0, 0, 1};
                    for (int k = 0; k < 4; ++k) {
                        if (!row[k]) continue;
                        int c = rx - rx0;
                        if (job.xs[k] > 1) c = FloorDiv(rx + job.dw.min.x, job.xs[k]) - CeilDiv(job.dw.min.x, job.xs[k]);
                        s[k] = row[k][std::clamp(c, 0, chW[k] - 1)];
                    }
                    float r, g, b;
                    if (job.pick.mode == Mode::Gray) {
                        r = g = b = s[0];
                    } else if (job.pick.mode == Mode::Yc) {  // luminance + chrominance (RgbaYca, Rec. 709)
                        r = (s[1] + 1) * s[0];
                        b = (s[2] + 1) * s[0];
                        g = (s[0] - r * 0.2126f - b * 0.0722f) / 0.7152f;
                    } else {
                        r = s[0], g = s[1], b = s[2];
                    }
                    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    if (lum > maxLum && std::isfinite(lum)) maxLum = lum;
                    uint8_t* p = d + size_t(ox) * 4;
                    if (!job.alpha) {
                        p[0] = ToSrgb(lut, b), p[1] = ToSrgb(lut, g), p[2] = ToSrgb(lut, r), p[3] = 255;
                        continue;
                    }
                    const float a = s[3];
                    if (!(a > 0)) {
                        p[0] = p[1] = p[2] = p[3] = 0;
                        continue;
                    }
                    const uint32_t a8 = a >= 1 ? 255u : uint32_t(a * 255 + 0.5f);
                    const float inv = a >= 1 ? 1.f : 1.f / a;  // sRGB sur la couleur non prémultipliée
                    p[0] = uint8_t(ToSrgb(lut, b * inv) * a8 / 255);
                    p[1] = uint8_t(ToSrgb(lut, g * inv) * a8 / 255);
                    p[2] = uint8_t(ToSrgb(lut, r * inv) * a8 / 255);
                    p[3] = uint8_t(a8);
                }
            }
        }
    } catch (...) {
        ++job.failed;
        job.next = job.chunks.size();  // mémoire insuffisante : on arrête tout
    }
    reset();
    std::lock_guard<std::mutex> lock(job.mu);
    job.maxLum = std::max(job.maxLum, maxLum);
}

struct ExrContext {
    exr_context_t c = nullptr;
    ~ExrContext() {
        if (c) exr_finish(&c);
    }
};

int Decode(const CyImgHost* host, CyImgContext* ctx, const wchar_t* path) {
    static std::once_flag quiet;  // OpenJPH (HTJ2K) écrit ses erreurs sur stderr : on les coupe
    std::call_once(quiet, [] {
        ojph::set_error_stream(nullptr);
        ojph::set_warning_stream(nullptr);
        ojph::set_info_stream(nullptr);
    });
    auto fail = [&](const std::wstring& msg) {
        if (CYIMG_HOST_HAS(host, set_error)) host->set_error(ctx, msg.c_str());
        return CYIMG_E_FAIL;
    };
    Mapped file;
    if (!file.Open(path)) return fail(L"Lecture du fichier EXR impossible");
    if (!Probe(file.data, size_t(std::min<uint64_t>(file.size, 4)))) return fail(L"Fichier OpenEXR non reconnu");

    Io io{file.data, file.size, {}, {}};
    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.user_data = &io;
    init.read_fn = ReadFn;
    init.size_fn = SizeFn;
    init.error_handler_fn = ErrorFn;
    ExrContext ec;
    if (exr_start_read(&ec.c, "image.exr", &init) != EXR_ERR_SUCCESS)
        return fail(L"En-tête EXR invalide" + (io.firstError.empty() ? L"" : L" (" + Widen(io.firstError) + L")"));

    int parts = 0;
    exr_get_count(ec.c, &parts);
    exr_storage_t storage = EXR_STORAGE_SCANLINE;
    if (exr_get_storage(ec.c, 0, &storage) != EXR_ERR_SUCCESS) return fail(L"Partie EXR illisible");
    if (storage == EXR_STORAGE_DEEP_SCANLINE || storage == EXR_STORAGE_DEEP_TILED)
        return fail(L"EXR « deep » (plusieurs échantillons par pixel) non géré");

    Job job;
    job.exr = ec.c;
    job.host = host;
    job.ctx = ctx;
    job.tiled = storage == EXR_STORAGE_TILED;
    exr_attr_box2i_t dispw{};
    const exr_attr_chlist_t* cl = nullptr;
    exr_compression_t comp = EXR_COMPRESSION_NONE;
    if (exr_get_data_window(ec.c, 0, &job.dw) != EXR_ERR_SUCCESS || exr_get_channels(ec.c, 0, &cl) != EXR_ERR_SUCCESS ||
        !cl || cl->num_channels <= 0)
        return fail(L"En-tête EXR incomplet (dataWindow ou canaux)");
    exr_get_display_window(ec.c, 0, &dispw);
    exr_get_compression(ec.c, 0, &comp);
    const int64_t fullW = int64_t(job.dw.max.x) - job.dw.min.x + 1, fullH = int64_t(job.dw.max.y) - job.dw.min.y + 1;
    if (fullW <= 0 || fullH <= 0 || fullW > (1 << 30) || fullH > (1 << 30)) return fail(L"Fenêtre de données EXR invalide");

    job.pick = ChooseChannels(cl);
    for (int k = 0; k < 4; ++k) {
        const int i = job.pick.idx[k];
        if (i < 0 || i >= cl->num_channels) continue;
        const exr_attr_chlist_entry_t& e = cl->entries[i];
        job.names[k].assign(e.name.str, size_t(e.name.length));
        job.xs[k] = std::max(1, e.x_sampling);
        job.ys[k] = std::max(1, e.y_sampling);
    }
    job.alpha = !job.names[3].empty();

    // Niveau à décoder et pas d'échantillonnage
    const uint32_t target = host->target_size(ctx);
    int64_t LW = fullW, LH = fullH;
    int32_t lpc = 1;
    if (job.tiled) {
        uint32_t tsx = 0, tsy = 0;
        exr_tile_level_mode_t lm = EXR_TILE_ONE_LEVEL;
        exr_tile_round_mode_t rm = EXR_TILE_ROUND_DOWN;
        if (exr_get_tile_descriptor(ec.c, 0, &tsx, &tsy, &lm, &rm) != EXR_ERR_SUCCESS || !tsx || !tsy || tsx > (1u << 20) ||
            tsy > (1u << 20))
            return fail(L"Description des tuiles EXR invalide");
        job.tileW = int(tsx), job.tileH = int(tsy);
        int32_t nlx = 1, nly = 1;
        if (target > 0 && lm != EXR_TILE_ONE_LEVEL && exr_get_tile_levels(ec.c, 0, &nlx, &nly) == EXR_ERR_SUCCESS) {
            for (int l = 1; l < std::min(nlx, nly); ++l) {  // mipmap (ou diagonale du ripmap)
                int32_t w = 0, h = 0;
                if (exr_get_level_sizes(ec.c, 0, l, l, &w, &h) != EXR_ERR_SUCCESS || std::max(w, h) < int32_t(target)) break;
                // Niveau réduit utilisable seulement si toutes ses tuiles sont présentes (fichier tronqué)
                int32_t cx = 0, cy = 0;
                bool ok = exr_get_tile_counts(ec.c, 0, l, l, &cx, &cy) == EXR_ERR_SUCCESS;
                for (int ty = 0; ok && ty < cy; ++ty)
                    for (int tx = 0; ok && tx < cx; ++tx) {
                        exr_chunk_info_t ci{};
                        ok = exr_read_tile_chunk_info(ec.c, 0, tx, ty, l, l, &ci) == EXR_ERR_SUCCESS &&
                             ci.data_offset + ci.packed_size <= file.size;
                    }
                if (!ok) break;
                job.lx = job.ly = l, LW = w, LH = h;
            }
        }
    } else if (exr_get_scanlines_per_chunk(ec.c, 0, &lpc) != EXR_ERR_SUCCESS || lpc <= 0) {
        return fail(L"Organisation des lignes EXR invalide");
    }
    const int64_t big = std::max(LW, LH);
    if (target > 0 && big > int64_t(target)) job.N = int(big / target);
    job.N = int(std::max<int64_t>(job.N, (big + 65534) / 65535));  // limite de l'hôte : 65535 px
    job.outW = uint32_t((LW + job.N - 1) / job.N);
    job.outH = uint32_t((LH + job.N - 1) / job.N);

    // Liste des blocs utiles (ceux qui contiennent au moins une ligne retenue)
    auto rowsUseful = [&](int64_t r0, int64_t h) { return (r0 + job.N - 1) / job.N * job.N < r0 + h; };
    if (job.tiled) {
        int32_t cx = 0, cy = 0;
        if (exr_get_tile_counts(ec.c, 0, job.lx, job.ly, &cx, &cy) != EXR_ERR_SUCCESS || cx <= 0 || cy <= 0)
            return fail(L"Nombre de tuiles EXR invalide");
        for (int y = 0; y < cy; ++y) {
            if (!rowsUseful(int64_t(y) * job.tileH, job.tileH)) continue;
            for (int x = 0; x < cx; ++x)
                if (rowsUseful(int64_t(x) * job.tileW, job.tileW)) job.chunks.push_back({x, y});
        }
    } else {
        for (int64_t r = 0; r < fullH; r += lpc)
            if (rowsUseful(r, lpc)) job.chunks.push_back({int(job.dw.min.y + r), 0});
    }

    job.out = host->alloc_bgra(ctx, job.outW, job.outH, job.alpha ? 1 : 0, &job.stride);
    if (!job.out) return CYIMG_E_OUTOFMEMORY;
    for (uint32_t y = 0; y < job.outH; ++y) {  // blocs illisibles : noir (ou transparent)
        uint8_t* d = job.out + size_t(y) * job.stride;
        memset(d, 0, size_t(job.outW) * 4);
        if (!job.alpha)
            for (uint32_t x = 0; x < job.outW; ++x) d[x * 4 + 3] = 255;
    }

    // Pleine résolution : tous les cœurs ; vignette : un quart (l'hôte en décode déjà plusieurs à la fois)
    const size_t cores = std::max(1u, std::thread::hardware_concurrency());
    size_t nThreads = std::clamp<size_t>(target > 0 ? cores / 4 : cores, 1, 16);
    nThreads = std::min(nThreads, job.chunks.size());
    std::vector<std::thread> threads;
    for (size_t i = 1; i < nThreads; ++i) {
        try {
            threads.emplace_back(Worker, std::ref(job));
        } catch (...) {
            break;
        }
    }
    Worker(job);
    for (auto& t : threads) t.join();
    if (job.cancelled) return CYIMG_E_CANCELLED;
    if (!job.chunks.empty() && job.failed.load() == int(job.chunks.size()))
        return fail(L"Données EXR illisibles" + (io.firstError.empty() ? L"" : L" (" + Widen(io.firstError) + L")"));

    // Format et informations
    host->set_format(ctx, job.tiled ? L"OpenEXR (tuiles)" : L"OpenEXR");
    wchar_t buf[160];
    host->set_info(ctx, L"Compression", CompressionName(comp));
    std::wstring chans;
    for (int i = 0; i < cl->num_channels && i < 16; ++i) {
        if (i) chans += L", ";
        chans += Widen(std::string(cl->entries[i].name.str, size_t(cl->entries[i].name.length)));
    }
    if (cl->num_channels > 16) {
        swprintf_s(buf, L", … (%d au total)", cl->num_channels);
        chans += buf;
    }
    host->set_info(ctx, L"Canaux", chans.c_str());
    std::wstring shown;
    for (int k = 0; k < 4; ++k)
        if (!job.names[k].empty()) shown += (shown.empty() ? L"" : L", ") + Widen(job.names[k]);
    host->set_info(ctx, L"Canaux affichés", shown.c_str());
    bool types[3] = {};
    for (int k = 0; k < 4; ++k)
        if (job.pick.idx[k] >= 0) {
            const int t = cl->entries[job.pick.idx[k]].pixel_type;
            if (t >= 0 && t < 3) types[t] = true;
        }
    std::wstring ty;
    const wchar_t* tn[3] = {L"uint 32 bits", L"half (16 bits)", L"float (32 bits)"};
    for (int t = 0; t < 3; ++t)
        if (types[t]) ty += (ty.empty() ? L"" : L" + ") + std::wstring(tn[t]);
    host->set_info(ctx, L"Type de données", ty.c_str());
    if (parts > 1) {
        const char* name = nullptr;
        exr_get_name(ec.c, 0, &name);
        swprintf_s(buf, L"%d (partie 0 affichée%ls%ls)", parts, name ? L" : " : L"", name ? Widen(name).c_str() : L"");
        host->set_info(ctx, L"Parties", buf);
    }
    swprintf_s(buf, L"(%d,%d) - (%d,%d)", job.dw.min.x, job.dw.min.y, job.dw.max.x, job.dw.max.y);
    host->set_info(ctx, L"Fenêtre de données", buf);
    if (dispw.min.x != job.dw.min.x || dispw.min.y != job.dw.min.y || dispw.max.x != job.dw.max.x ||
        dispw.max.y != job.dw.max.y) {
        swprintf_s(buf, L"(%d,%d) - (%d,%d)", dispw.min.x, dispw.min.y, dispw.max.x, dispw.max.y);
        host->set_info(ctx, L"Fenêtre d'affichage", buf);
    }
    if (job.tiled) {
        int32_t nlx = 1, nly = 1;
        exr_get_tile_levels(ec.c, 0, &nlx, &nly);
        swprintf_s(buf, L"%dx%d, %d niveau(x)%ls", job.tileW, job.tileH, std::max(nlx, nly), job.lx ? L" (vignette : niveau réduit)" : L"");
        host->set_info(ctx, L"Tuiles", buf);
    }
    swprintf_s(buf, L"%.4g%ls", double(job.maxLum), job.N > 1 ? L" (échantillonnée)" : L"");
    host->set_info(ctx, L"Luminance max", buf);
    if (job.failed > 0) {
        swprintf_s(buf, L"%d bloc(s) illisible(s) sur %zu (fichier tronqué ?)", job.failed.load(), job.chunks.size());
        host->set_info(ctx, L"Avertissement", buf);
    }
    return CYIMG_OK;
}

const CyImgPlugin g_plugin = {
    CYIMG_API_VERSION, L"OpenEXR (OpenEXRCore)", L"1.0", L"exr", Probe, Decode,
};

}  // namespace

extern "C" __declspec(dllexport) const CyImgPlugin* CyImg_GetPlugin(void) { return &g_plugin; }
