// Décompression deflate (RFC 1951) et zlib (RFC 1950), sans dépendance.
// Utilisé par l'application (archives zip des mises à jour) et par le plugin uasset.

#include "inflate.h"

#include <cstring>
#include <mutex>

namespace {

struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool err = false;
    int Need(int k) {
        while (cnt < k) {
            if (pos >= n) {
                err = true;
                return 0;
            }
            buf |= uint32_t(p[pos++]) << cnt;
            cnt += 8;
        }
        int v = int(buf & ((1u << k) - 1));
        buf >>= k;
        cnt -= k;
        return v;
    }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[320];
};

int Decode(Bits& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= s.Need(1);
        if (s.err) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool Build(Huffman& h, const uint8_t* length, int n) {
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) h.count[length[i]]++;
    h.count[0] = 0;
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (length[i]) h.symbol[offs[length[i]]++] = uint16_t(i);
    return true;
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Bits& s, uint8_t* out, size_t outSize, size_t& op, const Huffman& lc, const Huffman& dc) {
    for (;;) {
        int sym = Decode(s, lc);
        if (sym < 0) return false;
        if (sym < 256) {
            if (op >= outSize) return false;
            out[op++] = uint8_t(sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            size_t len = kLenBase[sym] + size_t(s.Need(kLenExtra[sym]));
            int d = Decode(s, dc);
            if (d < 0 || d >= 30) return false;
            size_t dist = kDistBase[d] + size_t(s.Need(kDistExtra[d]));
            if (s.err || dist > op || op + len > outSize) return false;
            for (size_t i = 0; i < len; ++i, ++op) out[op] = out[op - dist];
        }
    }
}

}  // namespace

bool ZlibDecompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize) {
    if (srcSize < 2 || (src[0] & 0x0F) != 8) return false;
    return InflateRaw(src + 2, srcSize - 2, dst, rawSize);
}

bool InflateRaw(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t rawSize) {
    Bits s{src, srcSize};
    size_t op = 0;
    int last;
    do {
        last = s.Need(1);
        int type = s.Need(2);
        if (s.err) return false;
        if (type == 0) {  // bloc stocké
            s.buf = 0;
            s.cnt = 0;
            if (s.pos + 4 > s.n) return false;
            size_t len = size_t(s.p[s.pos]) | size_t(s.p[s.pos + 1]) << 8;
            s.pos += 4;
            if (s.pos + len > s.n || op + len > rawSize) return false;
            memcpy(dst + op, s.p + s.pos, len);
            s.pos += len;
            op += len;
        } else if (type == 1) {  // codes fixes
            static Huffman lc, dc;
            static std::once_flag once;
            std::call_once(once, [] {
                uint8_t l[288];
                int i = 0;
                for (; i < 144; ++i) l[i] = 8;
                for (; i < 256; ++i) l[i] = 9;
                for (; i < 280; ++i) l[i] = 7;
                for (; i < 288; ++i) l[i] = 8;
                Build(lc, l, 288);
                for (i = 0; i < 30; ++i) l[i] = 5;
                Build(dc, l, 30);
            });
            if (!Codes(s, dst, rawSize, op, lc, dc)) return false;
        } else if (type == 2) {  // codes dynamiques
            int nlen = s.Need(5) + 257, ndist = s.Need(5) + 1, ncode = s.Need(4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(s.Need(3));
            Huffman lencode, lc, dc;
            Build(lencode, lengths, 19);
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = Decode(s, lencode);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[idx++] = uint8_t(sym);
                } else {
                    int len = 0, rep;
                    if (sym == 16) {
                        if (idx == 0) return false;
                        len = lengths[idx - 1];
                        rep = 3 + s.Need(2);
                    } else if (sym == 17) {
                        rep = 3 + s.Need(3);
                    } else {
                        rep = 11 + s.Need(7);
                    }
                    if (idx + rep > nlen + ndist) return false;
                    while (rep--) lengths[idx++] = uint8_t(len);
                }
            }
            Build(lc, lengths, nlen);
            Build(dc, lengths + nlen, ndist);
            if (!Codes(s, dst, rawSize, op, lc, dc)) return false;
        } else {
            return false;
        }
    } while (!last);
    return op == rawSize;
}

