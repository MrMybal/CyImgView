// Clip Studio Paint (.clip) : le fichier contient une base SQLite (bloc CHNKSQLi) dont la table
// CanvasPreview stocke un aperçu PNG (colonne ImageData). Mini-lecteur SQLite : en-tête, b-arbres
// de tables (pages intérieures 0x05 et feuilles 0x0D), enregistrements et pages de débordement.
// Repli : recherche directe d'un PNG complet dans le fichier.

#include "docs_common.h"

#include <algorithm>
#include <cctype>
#include <functional>

namespace docs {
namespace {

class Sqlite {
public:
    bool Open(const uint8_t* db, size_t n) {
        if (n < 100 || memcmp(db, "SQLite format 3", 16) != 0) return false;
        m_db = db, m_n = n;
        m_page = Be16(db + 16);
        if (m_page == 1) m_page = 65536;
        if (m_page < 512 || (m_page & (m_page - 1))) return false;
        const uint32_t reserved = db[20];
        if (reserved >= m_page - 480) return false;
        m_usable = m_page - reserved;
        return true;
    }

    // Parcourt les lignes d'une table : fn(payload) retourne false pour arrêter.
    bool Walk(uint32_t root, const std::function<bool(const std::vector<uint8_t>&)>& fn) {
        m_visited = 0;
        m_stop = false;
        return WalkPage(root, fn, 0);
    }

    // Valeur de colonne d'un enregistrement : type sériel et position.
    static bool Column(const std::vector<uint8_t>& rec, uint32_t col, uint64_t& type, size_t& off, size_t& len) {
        size_t p = 0;
        uint64_t hdr;
        if (!Varint(rec.data(), rec.size(), p, hdr) || hdr > rec.size()) return false;
        size_t data = size_t(hdr);
        for (uint32_t i = 0; p < size_t(hdr); ++i) {
            uint64_t t;
            if (!Varint(rec.data(), size_t(hdr), p, t)) return false;
            const size_t l = TypeLen(t);
            if (i == col) {
                if (l > rec.size() - data) return false;
                type = t, off = data, len = l;
                return true;
            }
            data += l;
            if (data > rec.size()) return false;
        }
        type = 0, off = data, len = 0;  // colonne absente : NULL
        return true;
    }
    static int64_t Int(const std::vector<uint8_t>& rec, uint64_t type, size_t off, size_t len) {
        if (type == 8) return 0;
        if (type == 9) return 1;
        if (type < 1 || type > 6) return 0;
        int64_t v = int8_t(rec[off]);
        for (size_t i = 1; i < len; ++i) v = v << 8 | rec[off + i];
        return v;
    }
    static size_t TypeLen(uint64_t t) {
        static const size_t lens[] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
        if (t < 12) return lens[t];
        return size_t((t - 12) / 2);
    }

private:
    static bool Varint(const uint8_t* p, size_t n, size_t& pos, uint64_t& v) {
        v = 0;
        for (int i = 0; i < 9; ++i) {
            if (pos >= n) return false;
            const uint8_t b = p[pos++];
            if (i == 8) {
                v = v << 8 | b;
                return true;
            }
            v = v << 7 | (b & 0x7F);
            if (!(b & 0x80)) return true;
        }
        return true;
    }
    const uint8_t* Page(uint32_t n) const {
        if (!n || uint64_t(n) * m_page > m_n) return nullptr;
        return m_db + size_t(n - 1) * m_page;
    }

    bool WalkPage(uint32_t pg, const std::function<bool(const std::vector<uint8_t>&)>& fn, int depth) {
        const uint8_t* p = Page(pg);
        if (m_stop) return true;
        if (!p || depth > 20 || ++m_visited > 1000000) return false;
        const size_t h = pg == 1 ? 100 : 0;  // la page 1 commence par l'en-tête du fichier
        const uint8_t kind = p[h];
        const uint32_t cells = Be16(p + h + 3);
        if (kind == 0x05) {  // page intérieure de table
            const size_t cp = h + 12;
            if (cp + size_t(cells) * 2 > m_usable) return false;
            for (uint32_t i = 0; i < cells; ++i) {
                const size_t c = Be16(p + cp + i * 2);
                if (c + 4 > m_usable) return false;
                if (!WalkPage(Be32(p + c), fn, depth + 1)) return false;
                if (m_stop) return true;
            }
            return WalkPage(Be32(p + h + 8), fn, depth + 1);
        }
        if (kind != 0x0D) return false;
        const size_t cp = h + 8;
        if (cp + size_t(cells) * 2 > m_usable) return false;
        std::vector<uint8_t> payload;
        for (uint32_t i = 0; i < cells; ++i) {
            size_t c = Be16(p + cp + i * 2);
            uint64_t plen, rowid;
            if (!Varint(p, m_usable, c, plen) || !Varint(p, m_usable, c, rowid)) return false;
            if (plen > (uint64_t(1) << 30)) return false;
            // Partie locale, puis chaîne de pages de débordement
            const uint64_t U = m_usable, X = U - 35;
            size_t local = size_t(plen);
            if (plen > X) {
                const uint64_t M = ((U - 12) * 32 / 255) - 23, K = M + ((plen - M) % (U - 4));
                local = size_t(K <= X ? K : M);
            }
            if (c + local > m_usable) return false;
            payload.assign(p + c, p + c + local);
            if (local < plen) {
                if (c + local + 4 > m_usable) return false;
                uint32_t next = Be32(p + c + local);
                payload.reserve(size_t(plen));
                for (int guard = 0; payload.size() < plen; ++guard) {
                    const uint8_t* o = Page(next);
                    if (!o || guard > 1000000) return false;
                    const size_t take = std::min<size_t>(size_t(plen) - payload.size(), m_usable - 4);
                    payload.insert(payload.end(), o + 4, o + 4 + take);
                    next = Be32(o);
                }
            }
            if (!fn(payload)) {
                m_stop = true;
                return true;
            }
        }
        return true;
    }

    const uint8_t* m_db = nullptr;
    size_t m_n = 0;
    uint32_t m_page = 0, m_usable = 0;
    size_t m_visited = 0;
    bool m_stop = false;
};

// Index de la colonne « name » dans un CREATE TABLE (-1 si absente).
int ColumnIndex(const std::string& sql, const char* name) {
    const size_t a = sql.find('('), b = sql.rfind(')');
    if (a == std::string::npos || b == std::string::npos || b <= a) return -1;
    int depth = 0, idx = 0;
    std::string cur;
    auto check = [&](const std::string& def) -> bool {
        size_t i = 0;
        while (i < def.size() && isspace(uint8_t(def[i]))) ++i;
        std::string tok;
        const char q = i < def.size() ? def[i] : 0;
        if (q == '"' || q == '`' || q == '[') {
            const char close = q == '[' ? ']' : q;
            for (++i; i < def.size() && def[i] != close; ++i) tok += def[i];
        } else {
            while (i < def.size() && !isspace(uint8_t(def[i])) && def[i] != '(') tok += def[i++];
        }
        return _stricmp(tok.c_str(), name) == 0;
    };
    auto isConstraint = [](const std::string& def) {
        std::string t;
        for (char c : def) t += char(toupper(uint8_t(c)));
        const size_t i = t.find_first_not_of(" \t\r\n");
        if (i == std::string::npos) return true;
        for (const char* k : {"PRIMARY", "UNIQUE", "CHECK", "FOREIGN", "CONSTRAINT"})
            if (t.compare(i, strlen(k), k) == 0) return true;
        return false;
    };
    for (size_t i = a + 1; i <= b; ++i) {
        const char c = sql[i];
        if (i == b || (c == ',' && depth == 0)) {
            if (!isConstraint(cur)) {
                if (check(cur)) return idx;
                ++idx;
            }
            cur.clear();
            continue;
        }
        if (c == '(') ++depth;
        if (c == ')') --depth;
        cur += c;
    }
    return -1;
}

// Plus grand ImageData de CanvasPreview (PNG complet), false si introuvable.
bool ReadCanvasPreview(const uint8_t* db, size_t n, std::vector<uint8_t>& png) {
    Sqlite s;
    if (!s.Open(db, n)) return false;
    uint32_t root = 0;
    int col = -1;
    s.Walk(1, [&](const std::vector<uint8_t>& rec) {
        uint64_t t;
        size_t off, len;
        // sqlite_master : type, name, tbl_name, rootpage, sql
        if (!Sqlite::Column(rec, 1, t, off, len) || t < 13 || !(t & 1)) return true;
        if (std::string(reinterpret_cast<const char*>(&rec[off]), len) != "CanvasPreview") return true;
        if (!Sqlite::Column(rec, 0, t, off, len) || std::string(reinterpret_cast<const char*>(&rec[off]), len) != "table")
            return true;
        if (Sqlite::Column(rec, 3, t, off, len)) root = uint32_t(Sqlite::Int(rec, t, off, len));
        if (Sqlite::Column(rec, 4, t, off, len) && t >= 13 && (t & 1))
            col = ColumnIndex(std::string(reinterpret_cast<const char*>(&rec[off]), len), "ImageData");
        return false;
    });
    if (!root || col < 0) return false;
    s.Walk(root, [&](const std::vector<uint8_t>& rec) {
        uint64_t t;
        size_t off, len;
        if (Sqlite::Column(rec, uint32_t(col), t, off, len) && t >= 12 && !(t & 1) && len > png.size() &&
            PngLength(&rec[off], len))
            png.assign(rec.begin() + ptrdiff_t(off), rec.begin() + ptrdiff_t(off + len));
        return true;
    });
    return !png.empty();
}

}  // namespace

int DecodeClip(const Job& job, const uint8_t* f, size_t n) {
    job.host->set_format(job.ctx, L"Clip Studio Paint");
    // Bloc CHNKSQLi : "CHNK" "SQLi" + taille 64 bits, puis la base
    const uint8_t* db = nullptr;
    size_t dbLen = 0;
    static const char kTag[] = "CHNKSQLi";
    const auto it = std::search(f, f + n, kTag, kTag + 8);
    if (it != f + n && size_t(f + n - it) >= 16) {
        const size_t start = size_t(it - f) + 16;
        const uint64_t len = Be64(it + 8);
        db = f + start;
        dbLen = size_t(std::min<uint64_t>(len, n - start));
    } else {  // pas de bloc : recherche directe de l'en-tête SQLite
        static const char kSql[] = "SQLite format 3";
        const auto s = std::search(f, f + n, kSql, kSql + 16);
        if (s != f + n) db = s, dbLen = size_t(f + n - s);
    }
    std::vector<uint8_t> png;
    if (db && ReadCanvasPreview(db, dbLen, png) &&
        job.Preview(png.data(), png.size(), L"CanvasPreview") == CYIMG_OK)
        return CYIMG_OK;
    // Repli : PNG contigu dans le fichier
    size_t off = 0, len = 0;
    if (FindLargestPng(f, n, off, len) && job.Preview(f + off, len, L"PNG") == CYIMG_OK) return CYIMG_OK;
    return job.Fail(db ? L"Clip Studio Paint : aperçu CanvasPreview absent ou illisible"
                       : L"Clip Studio Paint : base de données interne introuvable (fichier tronqué ?)");
}

}  // namespace docs
