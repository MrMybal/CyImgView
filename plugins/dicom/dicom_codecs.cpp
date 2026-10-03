// Décompresseurs propres au DICOM : deflate de taille inconnue, RLE Lossless, JPEG sans perte (SOF3).
#include "dicom_codecs.h"

#include "cyimg_plugin_i18n.h"

#include <algorithm>
#include <cstring>

// --- Inflate (RFC 1951), sortie extensible --------------------------------------------------------

namespace {

struct Huff {
    short count[16];
    short symbol[288];
};

struct Inflater {
    const uint8_t* in;
    size_t inLen, inPos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool err = false;
    std::vector<uint8_t>& out;
    size_t maxOut;

    Inflater(const uint8_t* p, size_t n, std::vector<uint8_t>& o, size_t m) : in(p), inLen(n), out(o), maxOut(m) {}

    int Bits(int need) {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (inPos >= inLen) {
                err = true;
                return 0;
            }
            val |= uint32_t(in[inPos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return int(val & ((1u << need) - 1));
    }

    int Decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= Bits(1);
            if (err) return -1;
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

    static bool Build(Huff& h, const short* length, int n) {
        memset(h.count, 0, sizeof(h.count));
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return true;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return false;
        }
        short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = short(offs[len] + h.count[len]);
        for (int s = 0; s < n; ++s)
            if (length[s]) h.symbol[offs[length[s]]++] = short(s);
        return true;
    }

    bool Put(uint8_t b) {
        if (out.size() >= maxOut) return false;
        out.push_back(b);
        return true;
    }

    bool Codes(const Huff& lc, const Huff& dc) {
        static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lc);
            if (sym < 0) return false;
            if (sym < 256) {
                if (!Put(uint8_t(sym))) return false;
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                const int len = lbase[sym] + Bits(lext[sym]);
                const int ds = Decode(dc);
                if (ds < 0 || ds >= 30) return false;
                const size_t dist = size_t(dbase[ds] + Bits(dext[ds]));
                if (err || dist > out.size() || out.size() + size_t(len) > maxOut) return false;
                for (int i = 0; i < len; ++i) out.push_back(out[out.size() - dist]);
            }
        }
    }

    bool Run() {
        int last;
        do {
            last = Bits(1);
            const int type = Bits(2);
            if (err) return false;
            if (type == 0) {
                bitbuf = 0, bitcnt = 0;
                if (inPos + 4 > inLen) return false;
                const size_t len = size_t(in[inPos]) | size_t(in[inPos + 1]) << 8;
                inPos += 4;
                if (inPos + len > inLen || out.size() + len > maxOut) return false;
                out.insert(out.end(), in + inPos, in + inPos + len);
                inPos += len;
            } else if (type == 1) {
                short l[288];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                Huff lc, dc;
                Build(lc, l, 288);
                for (i = 0; i < 30; ++i) l[i] = 5;
                Build(dc, l, 30);
                if (!Codes(lc, dc)) return false;
            } else if (type == 2) {
                const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
                if (err || nlen > 286 || ndist > 30) return false;
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                short lengths[320] = {};
                for (int i = 0; i < ncode; ++i) lengths[order[i]] = short(Bits(3));
                Huff lencode, lc, dc;
                if (!Build(lencode, lengths, 19)) return false;
                int idx = 0;
                while (idx < nlen + ndist) {
                    int sym = Decode(lencode);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        lengths[idx++] = short(sym);
                    } else {
                        short len = 0;
                        int rep;
                        if (sym == 16) {
                            if (!idx) return false;
                            len = lengths[idx - 1];
                            rep = 3 + Bits(2);
                        } else if (sym == 17) {
                            rep = 3 + Bits(3);
                        } else {
                            rep = 11 + Bits(7);
                        }
                        if (err || idx + rep > nlen + ndist) return false;
                        while (rep--) lengths[idx++] = len;
                    }
                }
                if (!Build(lc, lengths, nlen) || !Build(dc, lengths + nlen, ndist)) return false;
                if (!Codes(lc, dc)) return false;
            } else {
                return false;
            }
        } while (!last);
        return true;
    }
};

}  // namespace

bool InflateUnknownSize(const uint8_t* src, size_t size, size_t maxOut, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(std::min<size_t>(maxOut, size * 4));
    Inflater inf(src, size, out, maxOut);
    return inf.Run();
}

// --- RLE Lossless ---------------------------------------------------------------------------------

bool DecodeDicomRle(const uint8_t* src, size_t size, uint32_t rows, uint32_t cols, uint32_t samples, uint32_t bps,
                    std::vector<uint8_t>& out, std::wstring& why) {
    if (size < 64) {
        why = CYIMG_TR("plugin.dicom.why.rle_short", L"fragment RLE trop court");
        return false;
    }
    auto U32 = [&](size_t i) { return uint32_t(src[i]) | uint32_t(src[i + 1]) << 8 | uint32_t(src[i + 2]) << 16 | uint32_t(src[i + 3]) << 24; };
    const uint32_t nseg = U32(0);
    if (nseg != samples * bps || nseg > 15) {
        why = CYIMG_TR("plugin.dicom.why.rle_segments", L"nombre de segments RLE incohérent");
        return false;
    }
    const size_t plane = size_t(rows) * cols;
    if (plane * samples * bps / 64 > size + 4096) {  // RLE : au mieux ~64:1
        why = CYIMG_TR("plugin.dicom.why.rle_size", L"dimensions incohérentes avec les données RLE");
        return false;
    }
    out.assign(plane * samples * bps, 0);
    for (uint32_t s = 0; s < nseg; ++s) {
        const size_t beg = U32(4 + 4 * s), end = s + 1 < nseg ? U32(8 + 4 * s) : size;
        if (beg < 64 || beg > end || end > size) {
            why = CYIMG_TR("plugin.dicom.why.rle_segment", L"segment RLE hors du fragment");
            return false;
        }
        // Segment s : échantillon s / bps, octet de poids fort d'abord
        const uint32_t sample = s / bps, byteIdx = bps - 1 - s % bps;
        uint8_t* dst = out.data() + size_t(sample) * bps + byteIdx;
        const size_t step = size_t(samples) * bps;
        size_t px = 0, i = beg;
        while (i < end && px < plane) {
            const int n = int8_t(src[i++]);
            if (n >= 0) {
                for (int k = 0; k <= n && i < end && px < plane; ++k) dst[step * px++] = src[i++];
            } else if (n != -128) {
                if (i >= end) break;
                const uint8_t v = src[i++];
                for (int k = 0; k < 1 - n && px < plane; ++k) dst[step * px++] = v;
            }
        }
    }
    return true;
}

// --- JPEG sans perte (processus 14) ---------------------------------------------------------------

namespace {

struct JHuff {
    bool set = false;
    uint8_t vals[256] = {};
    int mincode[17] = {}, maxcode[18] = {}, valptr[17] = {};
};

void BuildJHuff(JHuff& h, const uint8_t counts[16], const uint8_t* vals, int nvals) {
    memcpy(h.vals, vals, size_t(nvals));
    int code = 0, k = 0;
    for (int l = 1; l <= 16; ++l) {
        h.valptr[l] = k;
        h.mincode[l] = code;
        code += counts[l - 1];
        k += counts[l - 1];
        h.maxcode[l] = counts[l - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h.maxcode[17] = 0x7FFFFFFF;
    h.set = true;
}

struct BitReader {
    const uint8_t* p;
    size_t n, pos;
    uint32_t acc = 0;
    int cnt = 0;
    bool marker = false;

    void Fill() {
        while (cnt <= 24) {
            uint32_t b = 0;
            if (!marker && pos < n) {
                b = p[pos];
                if (b == 0xFF) {
                    const uint8_t nb = pos + 1 < n ? p[pos + 1] : 0xD9;
                    if (nb == 0) pos += 2;
                    else marker = true, b = 0;
                } else {
                    ++pos;
                }
            }
            acc |= b << (24 - cnt);
            cnt += 8;
        }
    }
    int Bit() {
        if (!cnt) Fill();
        const int b = int(acc >> 31);
        acc <<= 1;
        --cnt;
        return b;
    }
    int Get(int k) {
        if (cnt < k) Fill();
        const int v = int(acc >> (32 - k));
        acc <<= k;
        cnt -= k;
        return v;
    }
    // Après un intervalle de redémarrage : saute le marqueur RSTn
    void Restart() {
        acc = 0, cnt = 0, marker = false;
        while (pos + 1 < n && !(p[pos] == 0xFF && p[pos + 1] >= 0xD0 && p[pos + 1] <= 0xD7)) ++pos;
        if (pos + 1 < n) pos += 2;
    }
};

int DecodeSym(BitReader& br, const JHuff& h) {
    int code = br.Bit(), l = 1;
    while (code > h.maxcode[l]) {
        code = (code << 1) | br.Bit();
        if (++l > 16) return -1;
    }
    const int idx = h.valptr[l] + code - h.mincode[l];
    return idx >= 0 && idx < 256 ? h.vals[idx] : -1;
}

}  // namespace

bool DecodeLosslessJpeg(const uint8_t* src, size_t size, uint32_t bytesPerSample, std::vector<uint8_t>& out, uint32_t& W,
                        uint32_t& H, uint32_t& NC, uint32_t& P, std::wstring& why) {
    if (size < 4 || src[0] != 0xFF || src[1] != 0xD8) {
        why = CYIMG_TR("plugin.dicom.why.jpeg_invalid", L"flux JPEG invalide");
        return false;
    }
    JHuff tables[4];
    struct Comp {
        int id = 0, td = 0;
    } comps[4];
    W = H = NC = P = 0;
    uint32_t restart = 0;
    std::vector<uint16_t> planes;
    bool gotFrame = false, gotScan = false;
    size_t pos = 2;
    while (pos + 4 <= size) {
        if (src[pos] != 0xFF) {
            ++pos;
            continue;
        }
        const uint8_t m = src[pos + 1];
        if (m == 0xFF || m == 0x00 || (m >= 0xD0 && m <= 0xD7)) {
            pos += m == 0xFF ? 1 : 2;
            continue;
        }
        if (m == 0xD9) break;
        const size_t len = size_t(src[pos + 2]) << 8 | src[pos + 3];
        if (len < 2 || pos + 2 + len > size) {
            why = CYIMG_TR("plugin.dicom.why.jpeg_segment", L"segment JPEG tronqué");
            return false;
        }
        const uint8_t* s = src + pos + 4;
        const size_t sl = len - 2;
        pos += 2 + len;
        if (m == 0xC4) {
            size_t i = 0;
            while (i + 17 <= sl) {
                const int th = s[i] & 15;
                int total = 0;
                for (int k = 0; k < 16; ++k) total += s[i + 1 + size_t(k)];
                if (th > 3 || total > 256 || i + 17 + size_t(total) > sl) {
                    why = CYIMG_TR("plugin.dicom.why.huffman", L"table de Huffman invalide");
                    return false;
                }
                BuildJHuff(tables[th], s + i + 1, s + i + 17, total);
                i += 17 + size_t(total);
            }
        } else if (m == 0xC3) {
            if (sl < 6) return false;
            P = s[0];
            H = uint32_t(s[1]) << 8 | s[2];
            W = uint32_t(s[3]) << 8 | s[4];
            NC = s[5];
            if (P < 2 || P > 16 || !W || !H || NC < 1 || NC > 4 || sl < 6 + size_t(NC) * 3 || W > 65535 || H > 65535) {
                why = CYIMG_TR("plugin.dicom.why.dnl", L"en-tête JPEG sans perte invalide (hauteur DNL non gérée)");
                return false;
            }
            for (uint32_t c = 0; c < NC; ++c) {
                comps[c].id = s[6 + c * 3];
                if (s[7 + c * 3] != 0x11) {
                    why = CYIMG_TR("plugin.dicom.why.subsampling", L"sous-échantillonnage JPEG sans perte non géré");
                    return false;
                }
            }
            if (size_t(W) * H * NC / 8 > size + 4096) {  // au moins 1 bit par échantillon
                why = CYIMG_TR("plugin.dicom.why.jpeg_size2", L"dimensions incohérentes avec les données JPEG");
                return false;
            }
            planes.assign(size_t(W) * H * NC, 0);
            gotFrame = true;
        } else if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            wchar_t msg[256];
            swprintf_s(msg, CYIMG_TR("plugin.dicom.why.not_lossless", L"ce n'est pas un JPEG sans perte (SOF%d)"), int(m - 0xC0));
            why = msg;
            return false;
        } else if (m == 0xDD) {
            if (sl >= 2) restart = uint32_t(s[0]) << 8 | s[1];
        } else if (m == 0xDA) {
            if (!gotFrame || sl < 1) {
                why = CYIMG_TR("plugin.dicom.why.scan_before", L"scan JPEG avant l'en-tête");
                return false;
            }
            const uint32_t ns = s[0];
            if (ns < 1 || ns > NC || sl < 1 + ns * 2 + 3) return false;
            int idx[4], td[4];
            for (uint32_t i = 0; i < ns; ++i) {
                idx[i] = -1;
                for (uint32_t c = 0; c < NC; ++c)
                    if (comps[c].id == s[1 + i * 2]) idx[i] = int(c);
                td[i] = s[2 + i * 2] >> 4;
                if (idx[i] < 0 || td[i] > 3 || !tables[td[i]].set) {
                    why = CYIMG_TR("plugin.dicom.why.component", L"composante ou table JPEG manquante");
                    return false;
                }
            }
            const int pred = s[1 + ns * 2], pt = s[3 + ns * 2] & 15;
            if (pred < 1 || pred > 7) {
                why = CYIMG_TR("plugin.dicom.why.predictor", L"prédicteur JPEG sans perte invalide");
                return false;
            }
            BitReader br{src, size, pos};
            const int def = 1 << (int(P) - pt - 1);
            uint32_t mcu = 0;
            bool resetRow = true;  // début de scan / d'intervalle : première ligne
            uint32_t resetY = 0;
            for (uint32_t y = 0; y < H; ++y) {
                for (uint32_t x = 0; x < W; ++x) {
                    bool first = false;
                    if (restart && mcu && mcu % restart == 0) {
                        br.Restart();
                        resetRow = true;
                    }
                    if (resetRow) {
                        first = true;
                        resetRow = false;
                        resetY = y;
                    }
                    for (uint32_t i = 0; i < ns; ++i) {
                        uint16_t* pl = planes.data() + size_t(idx[i]);
                        auto at = [&](uint32_t xx, uint32_t yy) { return int(pl[(size_t(yy) * W + xx) * NC]); };
                        int pv;
                        if (first) pv = def;
                        else if (y == resetY) pv = at(x - 1, y);          // première ligne de l'intervalle
                        else if (x == 0) pv = at(0, y - 1);
                        else {
                            const int ra = at(x - 1, y), rb = at(x, y - 1), rc = at(x - 1, y - 1);
                            switch (pred) {
                            case 1: pv = ra; break;
                            case 2: pv = rb; break;
                            case 3: pv = rc; break;
                            case 4: pv = ra + rb - rc; break;
                            case 5: pv = ra + ((rb - rc) >> 1); break;
                            case 6: pv = rb + ((ra - rc) >> 1); break;
                            default: pv = (ra + rb) >> 1; break;
                            }
                        }
                        const int t = DecodeSym(br, tables[td[i]]);
                        if (t < 0 || t > 16) {
                            why = CYIMG_TR("plugin.dicom.why.corrupt", L"données JPEG sans perte corrompues");
                            return false;
                        }
                        int diff = 0;
                        if (t == 16) diff = 32768;
                        else if (t) {
                            diff = br.Get(t);
                            if (diff < (1 << (t - 1))) diff -= (1 << t) - 1;
                        }
                        pl[(size_t(y) * W + x) * NC] = uint16_t(pv + diff);
                    }
                    ++mcu;
                }
            }
            if (pt)
                for (auto& v : planes) v = uint16_t(v << pt);
            gotScan = true;
            pos = br.pos;  // suite : marqueurs après les données
        }
    }
    if (!gotScan) {
        why = CYIMG_TR("plugin.dicom.why.no_data", L"aucune donnée JPEG sans perte");
        return false;
    }
    out.resize(planes.size() * bytesPerSample);
    if (bytesPerSample == 1) {
        for (size_t i = 0; i < planes.size(); ++i) out[i] = uint8_t(planes[i]);
    } else {
        for (size_t i = 0; i < planes.size(); ++i) out[2 * i] = uint8_t(planes[i]), out[2 * i + 1] = uint8_t(planes[i] >> 8);
    }
    return true;
}
