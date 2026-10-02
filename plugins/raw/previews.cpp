// Recherche des aperçus JPEG embarqués dans les RAW, sans dépendance :
//  - structure TIFF : CR2, NEF, ARW, DNG, PEF, ORF, RW2, SRW, ERF... (IFD, SubIFD, JpgFromRaw)
//  - CR3 : conteneur ISO-BMFF (boîtes THMB, PRVW, et piste 1 = JPEG pleine résolution)
//  - RAF (Fujifilm) : en-tête avec position du JPEG
//  - CRW (anciens Canon) : structure CIFF, enregistrement 0x2007

#include "rawfile.h"

#include <cstdio>
#include <cstring>

namespace {

// --- JPEG --------------------------------------------------------------------------------------

// Vérifie qu'il s'agit d'un JPEG classique (pas du JPEG sans perte des données RAW) et lit sa taille.
// On saute de segment en segment dans le fichier : les blocs EXIF (avec miniature) des aperçus
// dépassent souvent 64 Ko avant l'en-tête SOF.
bool ProbeJpeg(const RawFile& f, uint64_t off, uint32_t len, uint32_t& w, uint32_t& h) {
    if (len < 64) return false;
    const uint64_t end = off + len;
    uint8_t b[9];
    if (!f.Read(off, b, 2) || b[0] != 0xFF || b[1] != 0xD8) return false;
    uint64_t p = off + 2;
    for (int i = 0; i < 64 && p + 9 <= end; ++i) {
        if (!f.Read(p, b, 9) || b[0] != 0xFF) return false;
        const uint8_t m = b[1];
        if (m == 0xFF) {  // octet de bourrage
            ++p;
            continue;
        }
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            h = Be16(b + 5);
            w = Be16(b + 7);
            return w > 0 && h > 0;
        }
        if ((m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) || m == 0xDA) return false;
        p += 2 + uint64_t(Be16(b + 2));
    }
    return false;
}

void AddPreview(const RawFile& f, RawScan& out, uint64_t off, uint64_t len) {
    if (off == 0 || len < 64 || off > f.Size() || len > f.Size() - off || len > 0x7FFFFFFF) return;
    for (auto& p : out.previews)
        if (p.off == off) return;
    Preview p;
    p.off = off;
    p.len = uint32_t(len);
    if (ProbeJpeg(f, off, p.len, p.w, p.h)) out.previews.push_back(p);
}

// Le JPEG commence parfois après quelques octets d'en-tête dans la boîte : on cherche FF D8 FF.
void AddPreviewInBox(const RawFile& f, RawScan& out, uint64_t begin, uint64_t end) {
    uint8_t head[64];
    size_t n = size_t(std::min<uint64_t>(sizeof(head), end - begin));
    if (n < 4 || !f.Read(begin, head, n)) return;
    for (size_t i = 0; i + 3 <= n; ++i) {
        if (head[i] == 0xFF && head[i + 1] == 0xD8 && head[i + 2] == 0xFF) {
            AddPreview(f, out, begin + i, end - begin - i);
            return;
        }
    }
}

// --- TIFF --------------------------------------------------------------------------------------

struct Entry {
    uint16_t tag, type;
    uint32_t count;
    uint8_t raw[4];
};

class Tiff {
public:
    Tiff(const RawFile& f, uint64_t base = 0) : m_f(f), m_base(base) {}

    bool ParseHeader() {
        uint8_t h[8];
        if (!m_f.Read(m_base, h, 8)) return false;
        if (h[0] == 'I' && h[1] == 'I') m_le = true;
        else if (h[0] == 'M' && h[1] == 'M') m_le = false;
        else return false;
        uint16_t magic = U16(h + 2);
        // 42 = TIFF, 0x4F52/0x5352 = Olympus ORF, 0x55 = Panasonic RW2
        if (magic != 42 && magic != 0x4F52 && magic != 0x5352 && magic != 0x55) return false;
        m_ifd0 = U32(h + 4);
        return true;
    }
    uint32_t Ifd0() const { return m_ifd0; }
    uint64_t Abs(uint32_t off) const { return m_base + off; }

    bool ReadIfd(uint32_t off, std::vector<Entry>& out, uint32_t& next) const {
        uint8_t c[2];
        if (!m_f.Read(Abs(off), c, 2)) return false;
        uint16_t n = U16(c);
        if (n == 0 || n > 1000) return false;
        std::vector<uint8_t> buf(size_t(n) * 12 + 4);
        if (!m_f.Read(Abs(off) + 2, buf.data(), buf.size())) return false;
        out.resize(n);
        for (uint16_t i = 0; i < n; ++i) {
            const uint8_t* e = buf.data() + size_t(i) * 12;
            out[i].tag = U16(e);
            out[i].type = U16(e + 2);
            out[i].count = U32(e + 4);
            memcpy(out[i].raw, e + 8, 4);
        }
        next = U32(buf.data() + size_t(n) * 12);
        return true;
    }

    static uint32_t TypeSize(uint16_t t) {
        switch (t) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
        }
    }

    bool Data(const Entry& e, std::vector<uint8_t>& out, size_t maxBytes = 1 << 16) const {
        uint64_t total = uint64_t(TypeSize(e.type)) * e.count;
        if (total == 0 || total > maxBytes) return false;
        out.resize(size_t(total));
        if (total <= 4) {
            memcpy(out.data(), e.raw, size_t(total));
            return true;
        }
        return m_f.Read(Abs(U32(e.raw)), out.data(), out.size());
    }

    uint32_t Scalar(const Entry& e, uint32_t i = 0) const {
        std::vector<uint8_t> d;
        if (i >= e.count || !Data(e, d)) return 0;
        switch (e.type) {
        case 1: case 7: return d[i];
        case 3: return U16(d.data() + i * 2);
        case 4: case 13: return U32(d.data() + i * 4);
        default: return 0;
        }
    }

    bool Rational(const Entry& e, double& v) const {
        std::vector<uint8_t> d;
        if ((e.type != 5 && e.type != 10) || !Data(e, d)) return false;
        uint32_t n = U32(d.data()), dn = U32(d.data() + 4);
        if (!dn) return false;
        v = e.type == 10 ? double(int32_t(n)) / double(int32_t(dn)) : double(n) / double(dn);
        return true;
    }

    std::wstring Ascii(const Entry& e) const {
        std::vector<uint8_t> d;
        if (e.type != 2 || !Data(e, d)) return {};
        std::string s(d.begin(), d.end());
        s.resize(strnlen(s.c_str(), s.size()));
        while (!s.empty() && s.back() == ' ') s.pop_back();
        std::wstring w;
        for (unsigned char ch : s) w += wchar_t(ch);
        return w;
    }

    uint16_t U16(const uint8_t* p) const { return m_le ? Le16(p) : Be16(p); }
    uint32_t U32(const uint8_t* p) const { return m_le ? Le32(p) : Be32(p); }

private:
    const RawFile& m_f;
    uint64_t m_base;
    bool m_le = true;
    uint32_t m_ifd0 = 0;
};

const Entry* Find(const std::vector<Entry>& v, uint16_t tag) {
    for (auto& e : v)
        if (e.tag == tag) return &e;
    return nullptr;
}

std::wstring FormatDate(const std::wstring& s) {  // "2024:05:17 14:03:22" -> "17/05/2024 14:03:22"
    if (s.size() >= 19 && s[4] == L':') return s.substr(8, 2) + L"/" + s.substr(5, 2) + L"/" + s.substr(0, 4) + L" " + s.substr(11, 8);
    return s;
}

void AddExif(const Tiff& t0, const std::vector<Entry>& ifd0, const Tiff& te, const std::vector<Entry>& exif, RawScan& out) {
    wchar_t buf[64];
    auto add = [&](const wchar_t* k, const std::wstring& v) {
        if (!v.empty()) out.info.emplace_back(k, v);
    };
    std::wstring make, model;
    if (auto e = Find(ifd0, 0x010F)) make = t0.Ascii(*e);
    if (auto e = Find(ifd0, 0x0110)) model = t0.Ascii(*e);
    if (!model.empty()) {
        std::wstring brand = make.substr(0, make.find(L' '));
        add(L"Appareil", (!brand.empty() && model.find(brand) == std::wstring::npos) ? make + L" " + model : model);
    }
    if (auto e = Find(exif, 0xA434)) add(L"Objectif", te.Ascii(*e));
    double v;
    if (auto e = Find(exif, 0x829A); e && te.Rational(*e, v) && v > 0) {
        if (v < 0.5) swprintf_s(buf, L"1/%.0f s", 1.0 / v);
        else swprintf_s(buf, L"%.1f s", v);
        add(L"Exposition", buf);
    }
    if (auto e = Find(exif, 0x829D); e && te.Rational(*e, v) && v > 0) {
        swprintf_s(buf, L"f/%.1f", v);
        add(L"Ouverture", buf);
    }
    if (auto e = Find(exif, 0x8827)) {
        if (uint32_t iso = te.Scalar(*e)) add(L"ISO", std::to_wstring(iso));
    }
    if (auto e = Find(exif, 0x920A); e && te.Rational(*e, v) && v > 0) {
        swprintf_s(buf, L"%.0f mm", v);
        add(L"Focale", buf);
    }
    if (auto e = Find(exif, 0x9003)) add(L"Date de prise", FormatDate(te.Ascii(*e)));
}

void ScanTiff(const RawFile& f, RawScan& out) {
    Tiff t(f);
    if (!t.ParseHeader()) return;
    out.container = L"TIFF";
    std::vector<Entry> ifd0, exif;
    std::vector<uint32_t> todo{t.Ifd0()}, seen;
    while (!todo.empty() && seen.size() < 64) {
        uint32_t off = todo.back();
        todo.pop_back();
        if (off < 8 || off >= f.Size() || std::find(seen.begin(), seen.end(), off) != seen.end()) continue;
        seen.push_back(off);
        std::vector<Entry> ifd;
        uint32_t next = 0;
        if (!t.ReadIfd(off, ifd, next)) continue;
        if (seen.size() == 1) {
            ifd0 = ifd;
            if (auto e = Find(ifd, 0x0112)) out.orientation = t.Scalar(*e);
            if (auto e = Find(ifd, 0x8769)) {
                uint32_t en = 0;
                t.ReadIfd(t.Scalar(*e), exif, en);
            }
        }
        if (next) todo.push_back(next);
        if (auto e = Find(ifd, 0x014A)) {  // SubIFDs
            for (uint32_t i = 0; i < std::min<uint32_t>(e->count, 8); ++i) todo.push_back(t.Scalar(*e, i));
        }
        auto jo = Find(ifd, 0x0201), jl = Find(ifd, 0x0202);
        if (jo && jl) AddPreview(f, out, t.Scalar(*jo), t.Scalar(*jl));
        auto so = Find(ifd, 0x0111), sl = Find(ifd, 0x0117);
        if (so && sl && so->count >= 1) {
            uint64_t total = 0;
            for (uint32_t i = 0; i < std::min<uint32_t>(sl->count, 1024); ++i) total += t.Scalar(*sl, i);
            AddPreview(f, out, t.Scalar(*so), total);
        }
        if (auto e = Find(ifd, 0x002E); e && e->type == 7) AddPreview(f, out, t.U32(e->raw), e->count);  // RW2
    }
    AddExif(t, ifd0, t, exif, out);
}

// --- CR3 (ISO-BMFF) ----------------------------------------------------------------------------

const uint8_t kCanonUuid[16] = {0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0,
                                0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48};
const uint8_t kPreviewUuid[16] = {0xea, 0xf4, 0x2b, 0x5e, 0x1c, 0x98, 0x4b, 0x88,
                                  0xb9, 0xfb, 0xb7, 0xdc, 0x40, 0x6e, 0x4d, 0x16};

struct Box {
    uint64_t payload = 0, end = 0;
    char type[5] = {};
};

bool ReadBox(const RawFile& f, uint64_t off, uint64_t limit, Box& b) {
    uint8_t h[16];
    if (off + 8 > limit || !f.Read(off, h, 8)) return false;
    uint64_t size = Be32(h);
    memcpy(b.type, h + 4, 4);
    uint64_t hdr = 8;
    if (size == 1) {
        if (off + 16 > limit || !f.Read(off + 8, h + 8, 8)) return false;
        size = Be64(h + 8);
        hdr = 16;
    } else if (size == 0) {
        size = limit - off;
    }
    if (size < hdr || size > limit - off) return false;
    b.payload = off + hdr;
    b.end = off + size;
    return true;
}

struct Cr3State {
    uint64_t trackOff = 0, trackSize = 0;
    uint64_t cmt1 = 0, cmt1End = 0, cmt2 = 0, cmt2End = 0;
};

void WalkCr3(const RawFile& f, uint64_t begin, uint64_t end, int depth, Cr3State& st, RawScan& out) {
    if (depth > 8) return;
    uint64_t off = begin;
    for (int n = 0; n < 1024 && off + 8 <= end; ++n) {
        Box b;
        if (!ReadBox(f, off, end, b)) break;
        const std::string type(b.type, 4);
        if (type == "moov" || type == "mdia" || type == "minf" || type == "stbl") {
            WalkCr3(f, b.payload, b.end, depth + 1, st, out);
        } else if (type == "trak") {
            st.trackOff = st.trackSize = 0;
            WalkCr3(f, b.payload, b.end, depth + 1, st, out);
            AddPreview(f, out, st.trackOff, st.trackSize);  // ne retient que les pistes JPEG
        } else if (type == "uuid") {
            uint8_t id[16];
            if (b.payload + 16 <= b.end && f.Read(b.payload, id, 16)) {
                if (!memcmp(id, kCanonUuid, 16)) WalkCr3(f, b.payload + 16, b.end, depth + 1, st, out);
                else if (!memcmp(id, kPreviewUuid, 16)) WalkCr3(f, b.payload + 24, b.end, depth + 1, st, out);
            }
        } else if (type == "THMB" || type == "PRVW") {
            AddPreviewInBox(f, out, b.payload, b.end);
        } else if (type == "CMT1") {
            st.cmt1 = b.payload;
            st.cmt1End = b.end;
        } else if (type == "CMT2") {
            st.cmt2 = b.payload;
            st.cmt2End = b.end;
        } else if (type == "stsz") {  // version/flags, taille fixe, nombre, [table si taille fixe = 0]
            uint8_t d[16];
            if (b.payload + 12 <= b.end && f.Read(b.payload, d, 12)) {
                uint32_t fixed = Be32(d + 4), count = Be32(d + 8);
                if (fixed) st.trackSize = fixed;
                else if (count && b.payload + 16 <= b.end && f.Read(b.payload + 12, d + 12, 4)) st.trackSize = Be32(d + 12);
            }
        } else if (type == "co64") {
            uint8_t d[16];
            if (b.payload + 16 <= b.end && f.Read(b.payload, d, 16) && Be32(d + 4)) st.trackOff = Be64(d + 8);
        } else if (type == "stco") {
            uint8_t d[12];
            if (b.payload + 12 <= b.end && f.Read(b.payload, d, 12) && Be32(d + 4)) st.trackOff = Be32(d + 8);
        }
        off = b.end;
    }
}

void ScanCr3(const RawFile& f, RawScan& out) {
    out.container = L"CR3";
    Cr3State st;
    WalkCr3(f, 0, f.Size(), 0, st, out);
    // CMT1 = IFD0 TIFF (marque, modèle, orientation), CMT2 = IFD EXIF
    Tiff t1(f, st.cmt1), t2(f, st.cmt2);
    std::vector<Entry> ifd0, exif;
    uint32_t next = 0;
    if (st.cmt1 && t1.ParseHeader() && t1.ReadIfd(t1.Ifd0(), ifd0, next)) {
        if (auto e = Find(ifd0, 0x0112)) out.orientation = t1.Scalar(*e);
    }
    if (st.cmt2 && t2.ParseHeader()) t2.ReadIfd(t2.Ifd0(), exif, next);
    AddExif(t1, ifd0, t2, exif, out);
}

// --- RAF (Fujifilm) ----------------------------------------------------------------------------

void ScanRaf(const RawFile& f, RawScan& out) {
    uint8_t h[96];
    if (!f.Read(0, h, sizeof(h))) return;
    out.container = L"RAF";
    AddPreview(f, out, Be32(h + 84), Be32(h + 88));
}

// --- CRW (Canon CIFF) --------------------------------------------------------------------------

void WalkCiff(const RawFile& f, uint64_t start, uint64_t end, int depth, RawScan& out) {
    if (depth > 4 || end < start + 6) return;
    uint8_t b[4];
    if (!f.Read(end - 4, b, 4)) return;
    uint64_t table = start + Le32(b);
    if (table + 2 > end || !f.Read(table, b, 2)) return;
    uint16_t count = Le16(b);
    if (count > 512) return;
    std::vector<uint8_t> entries(size_t(count) * 10);
    if (!f.Read(table + 2, entries.data(), entries.size())) return;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* e = entries.data() + size_t(i) * 10;
        uint16_t tag = Le16(e);
        uint64_t size = Le32(e + 2), off = start + Le32(e + 6);
        uint16_t type = tag >> 8;
        if (type == 0x28 || type == 0x30) WalkCiff(f, off, off + size, depth + 1, out);
        else if ((tag & 0x3FFF) == 0x2007) AddPreview(f, out, off, size);
    }
}

void ScanCrw(const RawFile& f, RawScan& out) {
    uint8_t h[14];
    if (!f.Read(0, h, sizeof(h))) return;
    out.container = L"CRW";
    WalkCiff(f, Le32(h + 2), f.Size(), 0, out);
}

}  // namespace

void ScanRaw(const RawFile& f, RawScan& out) {
    uint8_t h[16];
    if (!f.Read(0, h, sizeof(h))) return;
    if (!memcmp(h + 4, "ftypcrx ", 8)) ScanCr3(f, out);
    else if (!memcmp(h, "FUJIFILMCCD-RAW", 15)) ScanRaf(f, out);
    else if (h[0] == 'I' && h[1] == 'I' && !memcmp(h + 6, "HEAPCCDR", 8)) ScanCrw(f, out);
    else ScanTiff(f, out);
}
