// IFF ILBM (Amiga, plans de bits) et PBM (Deluxe Paint PC, octets « chunky »).
// BMHD, CMAP, CAMG, BODY (brut ou ByteRun1), masque (plan ou couleur transparente), EHB, HAM6/HAM8,
// ILBM 24/32 bits. Les autres fichiers IFF (Maya, AIFF, 8SVX...) sont refusés avec un message clair.

#include "classic.h"

#include <algorithm>

namespace classic {
namespace {

// Décompresse ByteRun1 (PackBits) jusqu'à remplir out ; retourne false si les données manquent.
bool ByteRun1(const uint8_t* s, size_t n, size_t& pos, uint8_t* out, size_t need) {
    size_t o = 0;
    while (o < need) {
        if (pos >= n) return false;
        const int8_t c = int8_t(s[pos++]);
        if (c >= 0) {
            const size_t cnt = size_t(c) + 1;
            if (pos + cnt > n) return false;
            const size_t k = std::min(cnt, need - o);
            memcpy(out + o, s + pos, k);
            pos += cnt;
            o += k;
        } else if (c != -128) {
            const size_t cnt = size_t(1 - c);
            if (pos >= n) return false;
            const size_t k = std::min(cnt, need - o);
            memset(out + o, s[pos++], k);
            o += k;
        }
    }
    return true;
}

std::wstring FourCC(const uint8_t* p) {
    std::wstring s;
    for (int i = 0; i < 4; ++i) s += (p[i] >= 32 && p[i] < 127) ? wchar_t(p[i]) : L'?';
    return s;
}

}  // namespace

int DecodeIff(Dec& d) {
    if (d.n < 12) return d.Fail(L"Fichier IFF tronqué");
    const uint8_t* f = d.d;
    if (memcmp(f, "FORM", 4) != 0) {
        if (memcmp(f, "FOR4", 4) == 0 || memcmp(f, "FOR8", 4) == 0)
            return d.Fail(L"Fichier IFF Maya (FOR4/FOR8) : non pris en charge, seules les images ILBM/PBM le sont");
        return d.Fail(L"Conteneur IFF non pris en charge (seules les images FORM ILBM/PBM le sont)");
    }
    const bool pbm = memcmp(f + 8, "PBM ", 4) == 0;
    if (!pbm && memcmp(f + 8, "ILBM", 4) != 0) {
        std::wstring msg = L"Ce fichier IFF n'est pas une image ILBM/PBM (type « " + FourCC(f + 8) + L" »)";
        return d.Fail(msg.c_str());
    }
    const size_t end = std::min<size_t>(d.n, size_t(BE32(f + 4)) + 8);

    // Parcours des blocs
    const uint8_t* bmhd = nullptr;
    const uint8_t* cmap = nullptr;
    size_t cmapLen = 0, bodyPos = 0, bodyLen = 0;
    uint32_t camg = 0;
    bool hasCamg = false;
    for (size_t pos = 12; pos + 8 <= end;) {
        const uint8_t* c = f + pos;
        const size_t len = BE32(c + 4);
        const size_t avail = std::min(len, end - pos - 8);
        if (!memcmp(c, "BMHD", 4) && avail >= 20) bmhd = c + 8;
        else if (!memcmp(c, "CMAP", 4)) cmap = c + 8, cmapLen = avail;
        else if (!memcmp(c, "CAMG", 4) && avail >= 4) camg = BE32(c + 8), hasCamg = true;
        else if (!memcmp(c, "BODY", 4)) {
            bodyPos = pos + 8, bodyLen = avail;
            break;
        }
        if (len > end - pos - 8) break;
        pos += 8 + len + (len & 1);
    }
    if (!bmhd) return d.Fail(L"Fichier IFF : en-tête BMHD absent");
    if (!bodyPos) return d.Fail(L"Fichier IFF : données BODY absentes (fichier tronqué ?)");
    const uint32_t w = BE16(bmhd), h = BE16(bmhd + 2);
    const uint32_t planes = bmhd[8], masking = bmhd[9], comp = bmhd[10];
    const uint32_t transColor = BE16(bmhd + 12);
    if (!SizeOk(w, h)) return d.Fail(L"Dimensions IFF invalides");
    if (comp > 1) return d.Fail(L"Compression IFF non gérée (seul ByteRun1 l'est)");
    if (pbm ? planes != 8 : (planes < 1 || (planes > 8 && planes != 24 && planes != 32)))
        return d.Fail(L"Nombre de plans IFF non géré");

    // Palette (CMAP) : les fichiers OCS stockent 4 bits par composante dans le quartet haut
    uint8_t pal[256][3] = {};
    int palCount = int(std::min<size_t>(256, cmapLen / 3));
    if (palCount) {
        bool low = true;
        for (int i = 0; i < palCount * 3; ++i) low &= (cmap[i] & 0x0F) == 0;
        for (int i = 0; i < palCount; ++i)
            for (int k = 0; k < 3; ++k) pal[i][k] = low ? uint8_t(cmap[i * 3 + k] | cmap[i * 3 + k] >> 4) : cmap[i * 3 + k];
    } else if (planes <= 8) {
        const int cnt = 1 << planes;
        for (int i = 0; i < cnt; ++i) pal[i][0] = pal[i][1] = pal[i][2] = uint8_t(i * 255 / (cnt - 1));
        palCount = cnt;
    }
    const bool ham = !pbm && (camg & 0x800) && (planes == 6 || planes == 8);
    const bool ehb = !pbm && !ham && planes == 6 && ((camg & 0x80) || (!hasCamg && palCount <= 32));
    if (ehb)
        for (int i = 0; i < 32; ++i)
            for (int k = 0; k < 3; ++k) pal[32 + i][k] = pal[i][k] >> 1;

    // Lignes source : ILBM = plans (+ plan de masque), PBM = octets
    const size_t rowBytes = pbm ? (size_t(w) + 1) & ~size_t(1) : ((size_t(w) + 15) / 16) * 2;
    const size_t nPlanes = pbm ? 1 : planes + (masking == 1 ? 1 : 0);
    const size_t lineBytes = rowBytes * nPlanes;
    std::vector<uint8_t> line(lineBytes);
    const uint8_t* body = f + bodyPos;
    size_t bpos = 0;
    Rgba img;
    img.Init(w, h, 255);
    const bool alpha = masking == 1 || masking == 2 || planes == 32;
    for (uint32_t y = 0; y < h; ++y) {
        if (comp == 1) {
            if (!ByteRun1(body, bodyLen, bpos, line.data(), lineBytes)) return d.Fail(L"Fichier IFF tronqué");
        } else {
            if (bpos + lineBytes > bodyLen) return d.Fail(L"Fichier IFF tronqué");
            memcpy(line.data(), body + bpos, lineBytes);
            bpos += lineBytes;
        }
        uint8_t* o = img.Row(y);
        uint8_t cur[3] = {pal[0][0], pal[0][1], pal[0][2]};  // HAM : couleur de départ = fond
        for (uint32_t x = 0; x < w; ++x, o += 4) {
            const size_t byte = x >> 3;
            const int bit = 7 - int(x & 7);
            if (pbm) {
                const uint8_t idx = line[x];
                o[0] = pal[idx][0], o[1] = pal[idx][1], o[2] = pal[idx][2];
                if (masking == 2 && idx == transColor) o[3] = 0;
                continue;
            }
            uint32_t v = 0;
            for (uint32_t p = 0; p < planes; ++p) v |= uint32_t((line[p * rowBytes + byte] >> bit) & 1) << p;
            if (planes >= 24) {
                o[0] = uint8_t(v), o[1] = uint8_t(v >> 8), o[2] = uint8_t(v >> 16);
                if (planes == 32) o[3] = uint8_t(v >> 24);
            } else if (ham) {
                const int db = planes - 2;                 // bits de donnée (4 ou 6)
                const uint32_t ctl = v >> db, val = v & ((1u << db) - 1);
                const uint8_t c8 = db == 4 ? uint8_t(val << 4 | val) : uint8_t(val << 2 | val >> 4);
                if (ctl == 0) cur[0] = pal[val][0], cur[1] = pal[val][1], cur[2] = pal[val][2];
                else if (ctl == 1) cur[2] = c8;
                else if (ctl == 2) cur[0] = c8;
                else cur[1] = c8;
                o[0] = cur[0], o[1] = cur[1], o[2] = cur[2];
            } else {
                o[0] = pal[v][0], o[1] = pal[v][1], o[2] = pal[v][2];
                if (masking == 2 && v == transColor) o[3] = 0;
            }
            if (masking == 1 && !((line[planes * rowBytes + byte] >> bit) & 1)) o[3] = 0;
        }
        if ((y & 63) == 0 && d.Cancelled()) return CYIMG_E_CANCELLED;
    }

    wchar_t fmt[80];
    if (pbm) swprintf_s(fmt, L"IFF PBM (Deluxe Paint), 256 couleurs");
    else if (ham) swprintf_s(fmt, L"IFF ILBM HAM%u", planes);
    else if (ehb) swprintf_s(fmt, L"IFF ILBM EHB (64 couleurs)");
    else if (planes >= 24) swprintf_s(fmt, L"IFF ILBM %u bits", planes);
    else swprintf_s(fmt, L"IFF ILBM %u couleurs", 1u << planes);
    d.Format(fmt);
    if (masking == 1) d.Info(L"Masque", L"plan de masque");
    else if (masking == 2) d.Info(L"Masque", L"couleur transparente");
    return Emit(d, img, alpha);
}

}  // namespace classic
