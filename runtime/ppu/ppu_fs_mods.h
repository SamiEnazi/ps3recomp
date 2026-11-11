/*
 * ps3recomp - game mods in the inFAMOUS Mod Manager's format, served under the
 * guest (included once, by ppu_fs.cpp, after ppu_fs_lang.h)
 *
 * The community's inFAMOUS 2 mods, made for tiffsomniac's "inFAMOUS Mod
 * Manager" 0.2 (docs/MODS.md), are folders:
 *
 *   <mod>/mod.json         {"game": "inf2", "name", "author", "version", ...}
 *   <mod>/files/ (.xpps)   whole files that replace the game's, in any
 *                          subfolder (a mod with no files/ folder keeps them
 *                          in the mod folder itself)
 *   <files>/texture_patches/<target>.xpps/ ... data<OFFSET>_ ... <W>x<H>_<DXTn>_m<mips>.dds
 *                          a texture written over the target's bytes OFFSET
 *                          past its data start (the u32 at +40), its block
 *                          rows flipped back (the exporter flips them)
 *   <files>/xpps_patches/<target>.xpps/file_<i>_<abs>[_offset_<o>].bin
 *                          packed file i of the target replaced (its size and
 *                          the later files' and chunks' offsets fixed up), or
 *                          these bytes written at o inside it
 *   <mod>/modular.json     options: "modules" (toggles) and "groups" (toggle
 *                          or dropdown, of "options") whose "paths" pick the
 *                          parts of the mod that are used
 *
 * The Manager copies a mod into the game's USRDIR, after its "Unlocker" has
 * unpacked the five big archives (infamous1/2, patch1/3/4 .psarc_s) into
 * loose files there and moved the archives away: the game reads a file out
 * of its archives when one holds it, and the loose USRDIR file when none
 * does. Here nothing is unpacked, moved or written into the game's folders:
 *
 *  - Every file a mod changes is hidden from the archives the game opens:
 *    the archive's TOC record of it gets an MD5 (the game looks a file up by
 *    the MD5 of its upper-case name) one larger, in the read buffer, never
 *    on disk (lang_apply's per-descriptor patches). The TOC is not sorted,
 *    so no other lookup moves.
 *  - The game then asks for the loose file under USRDIR, and that open/stat
 *    gets the mod's file -- or, for patches, the original (read out of the
 *    highest archive that has it: patch4 > patch3 > patch1 > infamous2 >
 *    infamous1, the order in which the Unlocker's copies overwrite each
 *    other) with the patches written in, built once into <mods>/.port_cache.
 *
 * What a mod changes is decided at start. Mods are applied in folder order
 * (case-insensitive); a mod that changes what an earlier one already
 * changes is left out whole, as the Manager refuses to enable a conflicting
 * mod: the same whole file, a texture at the same offset, a packed file that
 * overlaps, or a patch onto a file another mod replaces.
 *
 *   PS3_GAME_MODS_DIR=<dir>    the mods folder: <dir>/<mod>/mod.json, or one
 *                              folder deeper (an unzipped "<name>/<name>/").
 *                              Unset: no mods (tests, the harness).
 *   PS3_GAME_MODS=0            use none of them (the menu's "Use mods").
 *   PS3_GAME_MODS_STATE=<file> the mods the menu turned off and the options
 *                              picked (default: mods.ini beside the menu's
 *                              settings.ini). A mod not in it is on, with
 *                              its default options (the Manager's: every
 *                              toggle, a dropdown's first option).
 *   PS3_GAME_MODS_CACHE=<dir>  where patched files are built
 *                              (default <mods dir>/.port_cache).
 *
 * Log lines start "[mods]".
 */
#ifndef PPU_FS_MODS_H
#define PPU_FS_MODS_H

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <ctype.h>
#include "ps3emu/ps3_game_mods.h"
#ifdef _WIN32
#include <direct.h>
#include <shellapi.h>
#endif

namespace gmods {

typedef std::vector<uint8_t> Bytes;

/* ---- small helpers --------------------------------------------------------- */

static std::string lower(std::string s)
{
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
static std::string upper(std::string s)
{
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}
static bool ieq(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
/* The Manager's StringComparer.OrdinalIgnoreCase: upper-cased, so '_' and
 * the other characters between 'Z' and 'a' sort after the letters. */
static bool ilt(const std::string& a, const std::string& b) { return upper(a) < upper(b); }
static bool ends_i(const std::string& s, const char* suf)
{
    const size_t n = strlen(suf);
    return s.size() >= n && lang_ieq_n(s.c_str() + s.size() - n, suf, n);
}
static std::string slashes(std::string s) { for (char& c : s) if (c == '\\') c = '/'; return s; }
static std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}
static std::string base_of(const std::string& p)
{
    const size_t k = p.find_last_of('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}
static std::string dir_of(const std::string& p)
{
    const size_t k = p.find_last_of('/');
    return k == std::string::npos ? std::string() : p.substr(0, k);
}
/* The Manager's NormalizeRelativePath: trimmed, '/' separators, no leading
 * '.' or '/', no trailing '/'. */
static std::string norm_rel(const std::string& in)
{
    size_t a = 0, b = in.size();
    while (a < b && isspace((unsigned char)in[a])) a++;
    while (b > a && isspace((unsigned char)in[b - 1])) b--;
    std::string s = slashes(in.substr(a, b - a));
    size_t i = 0;
    while (i < s.size() && (s[i] == '.' || s[i] == '/')) i++;
    s.erase(0, i);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static int imax(int a, int b) { return a > b ? a : b; }
static size_t imax(size_t a, size_t b) { return a > b ? a : b; }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

struct FInfo { bool ok = false, dir = false; uint64_t size = 0; int64_t mtime = 0; };
static FInfo finfo(const std::string& p)
{
    FInfo f;
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p.c_str(), &st) != 0) return f;
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return f;
#endif
    f.ok = true;
    f.dir = (st.st_mode & S_IFMT) == S_IFDIR;
    f.size = (uint64_t)st.st_size;
    f.mtime = (int64_t)st.st_mtime;
    return f;
}
static bool is_dir(const std::string& p) { const FInfo f = finfo(p); return f.ok && f.dir; }
/* A junction or symbolic link (on Windows: a reparse point). A mod's folders
 * are walked without them: a link in a downloaded mod could lead the scan
 * anywhere on the PC, or round in a loop. */
static bool is_link(const std::string& p)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

/* Entries of a folder (no "." / ".."), sorted case-insensitively. */
static std::vector<std::string> list_dir(const std::string& dir)
{
    std::vector<std::string> v;
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) v.push_back(e->d_name);
        closedir(d);
    }
    std::stable_sort(v.begin(), v.end(), ilt);
    return v;
}
/* Every file below dir whose name ends in ext (case-insensitive), full paths.
 * no_links: junctions and symbolic links are skipped (a mod's own folders). */
static void walk(const std::string& dir, const char* ext, std::vector<std::string>& out, int depth = 0, bool no_links = false)
{
    if (depth > 16) return;
    for (const std::string& n : list_dir(dir)) {
        const std::string p = join(dir, n);
        if (no_links && is_link(p)) continue;
        const FInfo f = finfo(p);
        if (!f.ok) continue;
        if (f.dir) walk(p, ext, out, depth + 1, no_links);
        else if (ends_i(n, ext)) out.push_back(p);
    }
}
/* p relative to root (both '/'-separated; p below root), else "". */
static std::string rel_to(const std::string& root, const std::string& p)
{
    if (p.size() > root.size() && lang_ieq_n(p.c_str(), root.c_str(), root.size()) && p[root.size()] == '/')
        return p.substr(root.size() + 1);
    return std::string();
}
static std::vector<std::string> split_parts(const std::string& rel)
{
    std::vector<std::string> v;
    size_t s = 0;
    for (size_t i = 0; i <= rel.size(); i++)
        if (i == rel.size() || rel[i] == '/') { if (i > s) v.push_back(rel.substr(s, i - s)); s = i + 1; }
    return v;
}

/* A mod's file bigger than this is refused, not read: a huge (or broken)
 * download must not take the game's memory at every start. The game's own
 * largest .xpps is far below it. */
static const uint64_t kMaxModFile = 512ull << 20;
static const uint64_t kMaxModJson = 1ull << 20;
/* The file from `at` on (at most `max` bytes); false when it cannot be read or
 * what would be read is more than `limit`. */
static bool read_all(const std::string& p, Bytes& out, uint64_t at = 0, uint64_t max = ~0ull, uint64_t limit = kMaxModFile)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    if (LANG_FSEEK(f, 0) == 0) {
#ifdef _WIN32
        _fseeki64(f, 0, SEEK_END); const int64_t n = _ftelli64(f);
#else
        fseeko(f, 0, SEEK_END); const int64_t n = (int64_t)ftello(f);
#endif
        if (n >= 0 && (uint64_t)n >= at) {
            uint64_t len = (uint64_t)n - at;
            if (len > max) len = max;
            if (len <= limit) {
                out.resize((size_t)len);
                ok = LANG_FSEEK(f, at) == 0 && fread(out.data(), 1, out.size(), f) == out.size();
            }
        }
    }
    fclose(f);
    return ok;
}
static void mkdirs(const std::string& dir)
{
    std::string d;
    for (const std::string& part : split_parts(dir)) {
        d = d.empty() ? (dir[0] == '/' ? "/" + part : part) : join(d, part);
        if (d.size() == 2 && d[1] == ':') continue;   /* "C:" */
        if (!is_dir(d)) mkdir(d.c_str(), 0777);
    }
}
static bool write_all(const std::string& p, const Bytes& d)
{
    mkdirs(dir_of(p));
    const std::string tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return false; }
    remove(p.c_str());
    return rename(tmp.c_str(), p.c_str()) == 0;
}

/* ---- JSON (mod.json, modular.json: comments and trailing commas allowed) ---- */

struct J {
    int t = 0;   /* 0 null, 1 bool, 2 number, 3 string, 4 array, 5 object */
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* get(const char* k) const
    {
        if (t != 5) return nullptr;
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    /* The Manager's ReadString: the trimmed string, else the fallback. */
    std::string str(const char* k, const std::string& fallback = std::string()) const
    {
        const J* v = get(k);
        if (!v || (v->t != 3 && v->t != 2)) return fallback;
        size_t a0 = 0, b0 = v->s.size();
        while (a0 < b0 && isspace((unsigned char)v->s[a0])) a0++;
        while (b0 > a0 && isspace((unsigned char)v->s[b0 - 1])) b0--;
        return v->s.substr(a0, b0 - a0);
    }
};

struct JParse {
    const char* p; const char* e;
    void ws()
    {
        for (;;) {
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (e - p >= 2 && p[0] == '/' && p[1] == '/') { while (p < e && *p != '\n') p++; continue; }
            if (e - p >= 2 && p[0] == '/' && p[1] == '*') {
                p += 2;
                while (e - p >= 2 && !(p[0] == '*' && p[1] == '/')) p++;
                p = (e - p >= 2) ? p + 2 : e;
                continue;
            }
            return;
        }
    }
    static void utf8(std::string& s, uint32_t c)
    {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    bool hex4(uint32_t& v)
    {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& s)
    {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p != '\\') { s += *p++; continue; }
            if (++p >= e) return false;
            const char c = *p++;
            switch (c) {
            case 'n': s += '\n'; break; case 't': s += '\t'; break; case 'r': s += '\r'; break;
            case 'b': s += '\b'; break; case 'f': s += '\f'; break;
            case 'u': {
                uint32_t u;
                if (!hex4(u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(s, u);
                break;
            }
            default: s += c; break;   /* \" \\ \/ */
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool val(J& v, int depth)
    {
        if (depth > 64) return false;
        ws();
        if (p >= e) return false;
        if (*p == '{') {
            v.t = 5; p++;
            for (;;) {
                ws();
                if (p < e && *p == '}') { p++; return true; }
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                p++;
                J x;
                if (!val(x, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(x));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.t = 4; p++;
            for (;;) {
                ws();
                if (p < e && *p == ']') { p++; return true; }
                J x;
                if (!val(x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.t = 3; return str(v.s); }
        if (e - p >= 4 && !strncmp(p, "true", 4)) { v.t = 1; v.s = "true"; p += 4; return true; }
        if (e - p >= 5 && !strncmp(p, "false", 5)) { v.t = 1; v.s = "false"; p += 5; return true; }
        if (e - p >= 4 && !strncmp(p, "null", 4)) { v.t = 0; p += 4; return true; }
        const char* s0 = p;
        while (p < e && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) p++;
        if (p == s0) return false;
        v.t = 2; v.s.assign(s0, p);
        return true;
    }
};

/* The file's object; an empty one if it is missing or not JSON (err says why). */
static J read_json(const std::string& path, std::string* err = nullptr)
{
    J j;
    Bytes b;
    if (!read_all(path, b, 0, ~0ull, kMaxModJson)) {
        if (err && finfo(path).ok) *err = base_of(slashes(path)) + " cannot be read or is bigger than 1 MB";
        return j;
    }
    const char* p = (const char*)b.data();
    const char* e = p + b.size();
    if (b.size() >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    JParse jp{ p, e };
    J v;
    if (jp.val(v, 0) && v.t == 5) return v;
    if (err) *err = base_of(slashes(path)) + " is not valid JSON";
    return j;
}

/* ---- PSARC (the game's archives) ------------------------------------------- */

struct Psarc {
    FILE* f = nullptr;
    std::string path;
    uint32_t toclen = 0, esz = 0, n = 0, bsz = 0;
    int nb = 2;
    size_t nblocks = 0;
    Bytes toc;                       /* the TOC after the 32-byte header */
    std::vector<std::string> names;  /* entry i's name, lower case, no leading '/' ("" for 0) */
    ~Psarc() { if (f) fclose(f); }
    uint64_t rd(const uint8_t* q, int k) const { uint64_t v = 0; for (int i = 0; i < k; i++) v = (v << 8) | q[i]; return v; }
    const uint8_t* rec(uint32_t i) const { return toc.data() + (size_t)i * esz; }
    bool entry(uint32_t i, Bytes& out)
    {
        if (i >= n) return false;
        const uint8_t* r = rec(i);
        uint64_t blk = rd(r + 16, 4);
        const uint64_t usz = rd(r + 20, 5), off = rd(r + 25, 5);
        if (usz > (2ull << 30)) return false;
        out.clear();
        out.reserve((size_t)usz);
        Bytes in(bsz), dec(bsz);
        const size_t tab = (size_t)n * esz;
        if (LANG_FSEEK(f, off) != 0) return false;
        while (out.size() < usz) {
            if (blk >= nblocks) return false;
            const uint32_t c = (uint32_t)rd(toc.data() + tab + blk * (size_t)nb, nb);
            blk++;
            const uint32_t take = c ? c : bsz;
            if (take > bsz || fread(in.data(), 1, take, f) != take) return false;
            const size_t want = (size_t)(usz - out.size()) < bsz ? (size_t)(usz - out.size()) : bsz;
            int got = -1;
            if (c && in[0] == 0x78)
                got = stbi_zlib_decode_buffer((char*)dec.data(), (int)bsz, (const char*)in.data(), (int)take);
            if (got > 0) out.insert(out.end(), dec.data(), dec.data() + ((size_t)got < want ? (size_t)got : want));
            else out.insert(out.end(), in.data(), in.data() + (take < want ? take : want));
        }
        return true;
    }
    bool open(const std::string& p)
    {
        path = p;
        f = fopen(p.c_str(), "rb");
        if (!f) return false;
        uint8_t h[32];
        if (fread(h, 1, 32, f) != 32 || memcmp(h, "PSAR", 4) || memcmp(h + 8, "zlib", 4)) return false;
        toclen = be32(h + 12); esz = be32(h + 16); n = be32(h + 20); bsz = be32(h + 24);
        if (esz != 30 || n < 2 || n > 1000000 || toclen < 32 + n * esz || toclen > (64u << 20) || !bsz || bsz > (16u << 20))
            return false;
        toc.resize(toclen - 32);
        if (fread(toc.data(), 1, toc.size(), f) != toc.size()) return false;
        nb = bsz <= 0x10000 ? 2 : bsz <= 0x1000000 ? 3 : 4;
        nblocks = (toc.size() - (size_t)n * esz) / (size_t)nb;
        Bytes list;
        if (!entry(0, list)) return false;
        names.assign(n, std::string());
        uint32_t idx = 1;
        for (size_t s = 0; s < list.size() && idx < n; idx++) {
            size_t q = s;
            while (q < list.size() && list[q] != '\n') q++;
            std::string nm((const char*)list.data() + s, q - s);
            if (!nm.empty() && nm.back() == '\r') nm.pop_back();
            if (!nm.empty() && nm[0] == '/') nm.erase(0, 1);
            names[idx] = lower(nm);
            s = q + 1;
        }
        return true;
    }
    int find(const std::string& lname) const
    {
        for (uint32_t i = 1; i < n; i++) if (names[i] == lname) return (int)i;
        return -1;
    }
};

/* ---- the Manager's patch formats ------------------------------------------- */

static int dxt_block(const std::string& fmt) { return fmt == "DXT1" ? 8 : 16; }
static size_t dxt_size(int w, int h, const std::string& fmt, int mips)
{
    size_t total = 0;
    for (int i = 0; i < mips; i++) {
        total += (size_t)imax(1, (w + 3) / 4) * (size_t)imax(1, (h + 3) / 4) * (size_t)dxt_block(fmt);
        w = imax(1, w / 2); h = imax(1, h / 2);
    }
    return total;
}
static void flip_dxt1(uint8_t* b) { std::swap(b[4], b[7]); std::swap(b[5], b[6]); }
static void flip_dxt3(uint8_t* b)
{
    uint8_t t[8]; memcpy(t, b, 8);
    memcpy(b + 0, t + 6, 2); memcpy(b + 2, t + 4, 2); memcpy(b + 4, t + 2, 2); memcpy(b + 6, t + 0, 2);
    flip_dxt1(b + 8);
}
static void flip_dxt5(uint8_t* b)
{
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    uint8_t px[16], fl[16];
    for (int j = 0; j < 16; j++) px[j] = (uint8_t)((bits >> (3 * j)) & 7);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) fl[r * 4 + c] = px[(3 - r) * 4 + c];
    bits = 0;
    for (int j = 0; j < 16; j++) bits |= (uint64_t)fl[j] << (3 * j);
    for (int i = 0; i < 6; i++) b[2 + i] = (uint8_t)(bits >> (8 * i));
    flip_dxt1(b + 8);
}
/* The exporter's (and the Manager's) vertical flip of every mip: block rows
 * reversed, each block's own rows reversed. Its own inverse. */
static void flip_payload(uint8_t* data, int w, int h, const std::string& fmt, int mips)
{
    const int bs = dxt_block(fmt);
    size_t at = 0;
    for (int i = 0; i < mips; i++) {
        const int bw = imax(1, (w + 3) / 4), bh = imax(1, (h + 3) / 4);
        const size_t len = (size_t)bw * bh * bs;
        Bytes tmp(len);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                uint8_t* d = tmp.data() + ((size_t)y * bw + x) * bs;
                memcpy(d, data + at + ((size_t)(bh - 1 - y) * bw + x) * bs, (size_t)bs);
                if (fmt == "DXT1") flip_dxt1(d); else if (fmt == "DXT3") flip_dxt3(d); else flip_dxt5(d);
            }
        memcpy(data + at, tmp.data(), len);
        at += len;
        w = imax(1, w / 2); h = imax(1, h / 2);
    }
}

static bool is_hex8(const std::string& s, size_t at)
{
    if (at + 8 > s.size()) return false;
    for (size_t i = at; i < at + 8; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}
/* The Manager's texture-patch name, (?:^|_)(?:data)?([0-9a-f]{8})_.*?(\d+)x(\d+)_(DXT[135])_m(\d+)\.dds$
 * case-insensitively: the offset, size, format and mip count. */
static bool parse_tex_name(const std::string& nm, uint32_t& off, int& w, int& h, std::string& fmt, int& mips)
{
    const std::string s = lower(nm);
    if (!ends_i(s, ".dds")) return false;
    size_t q = s.size() - 4, m_end = q;
    while (q > 0 && isdigit((unsigned char)s[q - 1])) q--;
    if (q == m_end || q < 2 || s.compare(q - 2, 2, "_m")) return false;
    mips = atoi(s.substr(q, m_end - q).c_str());
    q -= 2;
    if (q < 5 || s.compare(q - 4, 3, "dxt") || (s[q - 1] != '1' && s[q - 1] != '3' && s[q - 1] != '5') || s[q - 5] != '_') return false;
    fmt = std::string("DXT") + s[q - 1];
    q -= 5;
    const size_t h_end = q;
    while (q > 0 && isdigit((unsigned char)s[q - 1])) q--;
    if (q == h_end || q < 1 || s[q - 1] != 'x') return false;
    h = atoi(s.substr(q, h_end - q).c_str());
    const size_t x_at = q - 1;
    size_t wq = x_at;
    while (wq > 0 && isdigit((unsigned char)s[wq - 1])) wq--;
    if (wq == x_at) return false;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && s[i - 1] != '_') continue;
        size_t hx = std::string::npos;
        if (!s.compare(i, 4, "data") && is_hex8(s, i + 4) && i + 12 < s.size() && s[i + 12] == '_') hx = i + 4;
        else if (is_hex8(s, i) && i + 8 < s.size() && s[i + 8] == '_') hx = i;
        if (hx == std::string::npos) continue;
        const size_t region = hx + 9;
        if (region >= x_at) continue;
        const size_t ws = imax(region, wq);
        w = atoi(s.substr(ws, x_at - ws).c_str());
        off = (uint32_t)strtoul(s.substr(hx, 8).c_str(), nullptr, 16);
        return true;
    }
    return false;
}
/* ^file_(\d+)_([0-9a-f]{8})(?:_offset_([0-9a-f]{8}))?\.bin$, case-insensitively. */
static bool parse_xp_name(const std::string& nm, int& index, uint32_t& abs, bool& chunk, uint32_t& choff)
{
    const std::string s = lower(nm);
    if (s.compare(0, 5, "file_") || !ends_i(s, ".bin")) return false;
    size_t q = 5;
    const size_t d0 = q;
    while (q < s.size() && isdigit((unsigned char)s[q])) q++;
    if (q == d0 || q >= s.size() || s[q] != '_' || !is_hex8(s, q + 1)) return false;
    index = atoi(s.substr(d0, q - d0).c_str());
    abs = (uint32_t)strtoul(s.substr(q + 1, 8).c_str(), nullptr, 16);
    q += 9;
    chunk = false; choff = 0;
    if (q == s.size() - 4) return true;
    if (s.compare(q, 8, "_offset_") || !is_hex8(s, q + 8) || q + 16 != s.size() - 4) return false;
    chunk = true;
    choff = (uint32_t)strtoul(s.substr(q + 8, 8).c_str(), nullptr, 16);
    return true;
}

struct XpFile { uint32_t size, off; size_t size_at, off_at; };
struct XpChunk { uint32_t size, off, file_index, file_count; size_t size_at, off_at; };
struct XpLayout {
    uint32_t data_off = 0, data_size = 0;
    uint64_t tables_end = 48;            /* the header and both tables end here */
    std::vector<XpChunk> chunks; std::vector<XpFile> files;
};

static bool xp_layout(const Bytes& d, XpLayout& L, std::string& err)
{
    if (d.size() < 48 || memcmp(d.data(), "PACK", 4)) { err = "not an XPPS/XPP PACK file"; return false; }
    const uint32_t tab = be32(&d[24]);
    L.data_off = be32(&d[40]); L.data_size = be32(&d[44]);
    if ((uint64_t)tab + 24 > d.size()) { err = "its table header is out of range"; return false; }
    const uint32_t nc = be32(&d[tab + 4]), nf = be32(&d[tab + 12]);
    const uint64_t c0 = (uint64_t)tab + 24, f0 = c0 + (uint64_t)nc * 28;
    if (f0 + (uint64_t)nf * 16 > d.size()) { err = "its file table is out of range"; return false; }
    L.tables_end = f0 + (uint64_t)nf * 16;
    if (L.tables_end < 48) L.tables_end = 48;
    for (uint32_t i = 0; i < nc; i++) {
        const size_t r = (size_t)(c0 + (uint64_t)i * 28);
        L.chunks.push_back({ be32(&d[r + 4]), be32(&d[r + 8]), be32(&d[r + 20]), be32(&d[r + 24]), r + 4, r + 8 });
    }
    for (uint32_t i = 0; i < nf; i++) {
        const size_t r = (size_t)(f0 + (uint64_t)i * 16);
        L.files.push_back({ be32(&d[r + 4]), be32(&d[r + 8]), r + 4, r + 8 });
    }
    return true;
}

/* ---- mods ------------------------------------------------------------------ */

struct Module { std::string id, name, group_id, group_name; std::vector<std::string> paths; bool dropdown = false; };
struct Part { std::string path, target; };                         /* a mod file and the game file it changes (lower case) */
struct Tex : Part { uint32_t off = 0; int w = 0, h = 0, mips = 0; std::string fmt, name; };
struct Xp : Part { int index = 0; uint32_t abs = 0, choff = 0, len = 0; bool chunk = false; std::string name; };
struct Mod {
    std::string folder, path, name, author, version, game, err;
    bool for_game = false;
    std::vector<Module> modules;
    bool on = true;                      /* the saved choice */
    bool have_pick = false;
    std::vector<std::string> pick;       /* the saved option ids */
    std::vector<Part> files;
    std::vector<Tex> tex;
    std::vector<Xp> xp;
    std::set<std::string> targets;
};

struct StartInfo { bool active = false; std::string why; };

static struct {
    std::once_flag once;
    std::mutex mx;                                   /* list, start, state, pending */
    std::string dir, cache, state_path;
    bool use = true;                                 /* PS3_GAME_MODS */
    std::vector<Mod> list;                           /* what the menu shows */
    std::map<std::string, StartInfo> start;          /* by lower(folder): what this session uses */
    std::map<std::string, std::string> serve;        /* lower file name -> host file the game gets */
    std::map<std::string, std::string> serve_mod;    /* lower file name -> the mod's name, for the log */
    std::atomic<int> nserve{0};
    std::mutex log_mx;                               /* logged */
    std::set<std::string> logged;
} g;

static void log_once(const std::string& key, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_once(const std::string& key, const char* fmt, ...)
{
    {
        std::lock_guard<std::mutex> lk(g.log_mx);
        if (!g.logged.insert(key).second) return;
    }
    char b[1400];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "[mods] %s\n", b);
}

static std::string game_key(const std::string& v)
{
    const std::string s = lower(v);
    if (s == "inf2" || s == "infamous2" || s == "i2") return "inf2";
    if (s == "inf1" || s == "infamous1" || s == "infamous") return "inf1";
    if (s == "fob" || s == "festivalofblood" || s == "festival_of_blood") return "fob";
    return s;
}
static const char* game_label(const std::string& k)
{
    return k == "inf1" ? "inFAMOUS" : k == "fob" ? "inFAMOUS Festival of Blood" : k == "inf2" ? "inFAMOUS 2" : "another game";
}

/* modular.json, as the Manager reads it (ListModules). */
static void read_modules(Mod& m)
{
    const J j = read_json(join(m.path, "modular.json"));
    std::set<std::string> used;
    /* ids are saved in mods.ini as "pick = a,b": no separators, brackets or
     * line breaks in them (a crafted id could otherwise add lines there) */
    auto clean = [](std::string s) {
        for (char& c : s) if ((unsigned char)c < 32 || c == ',' || c == '[' || c == ']' || c == '=' || c == '#' || c == ';') c = '_';
        return s;
    };
    auto parse = [&](const J& node, const std::string& fallback, bool dropdown, const std::string& gid, const std::string& gname) {
        std::string id = clean(node.str("id", node.str("name", fallback)));
        if (id.empty()) id = fallback;
        std::string uid = id;
        for (int k = 2; !used.insert(lower(uid)).second; k++) uid = id + "_" + std::to_string(k);
        Module mod;
        mod.id = uid;
        mod.name = node.str("name", uid);
        if (mod.name.empty()) mod.name = uid;
        mod.dropdown = dropdown; mod.group_id = gid; mod.group_name = gname;
        std::set<std::string> seen;
        if (const J* ps = node.get("paths")) if (ps->t == 4)
            for (const J& x : ps->a) {
                if (x.t != 3) continue;
                const std::string r = norm_rel(x.s);
                if (!r.empty() && seen.insert(lower(r)).second) mod.paths.push_back(r);
            }
        m.modules.push_back(mod);
    };
    if (const J* ms = j.get("modules")) if (ms->t == 4) {
        int k = 0;
        for (const J& x : ms->a) { k++; if (x.t == 5) parse(x, "module_" + std::to_string(k), false, "", ""); }
    }
    if (const J* gs = j.get("groups")) if (gs->t == 4) {
        int k = 0;
        for (const J& gr : gs->a) {
            k++;
            if (gr.t != 5) continue;
            std::string gid = clean(gr.str("id", "group_" + std::to_string(k)));
            if (gid.empty()) gid = "group_" + std::to_string(k);
            const std::string gname = gr.str("name", gid);
            const bool dd = ieq(gr.str("type", "toggle"), "dropdown");
            const J* os = gr.get("options");
            if (!os || os->t != 4) continue;
            int n = 0;
            for (const J& o : os->a) { n++; if (o.t == 5) parse(o, gid + "_option_" + std::to_string(n), dd, gid, gname); }
        }
    }
    std::stable_sort(m.modules.begin(), m.modules.end(), [](const Module& a, const Module& b) { return ilt(a.name, b.name); });
}

/* The chosen options (the Manager's SelectedModuleIdsFromState): with no
 * saved choice (or an empty one) every toggle; each dropdown group's saved
 * option, else its first. */
static std::set<std::string> picked_ids(const Mod& m)
{
    std::set<std::string> out, saved;
    for (const std::string& s : m.pick) saved.insert(lower(s));
    for (const Module& x : m.modules)
        if (!x.dropdown && (!m.have_pick || m.pick.empty() || saved.count(lower(x.id)))) out.insert(lower(x.id));
    std::vector<std::string> groups;
    for (const Module& x : m.modules)
        if (x.dropdown && std::find_if(groups.begin(), groups.end(), [&](const std::string& s) { return ieq(s, x.group_id); }) == groups.end())
            groups.push_back(x.group_id);
    for (const std::string& gid : groups) {
        const Module* first = nullptr; const Module* chosen = nullptr;
        for (const Module& x : m.modules) {
            if (!x.dropdown || !ieq(x.group_id, gid)) continue;
            if (!first) first = &x;
            if (!chosen && saved.count(lower(x.id))) chosen = &x;
        }
        if (chosen || first) out.insert(lower((chosen ? chosen : first)->id));
    }
    return out;
}

/* Whether a path of the mod is in a chosen option (PathEnabledForMod).
 * sel == nullptr: the mod has no options, everything is used. */
static bool path_on(const std::vector<std::string>* sel, const std::string& rel)
{
    if (!sel) return true;
    const std::string n = lower(norm_rel(rel));
    for (const std::string& p : *sel)
        if (n == p || (n.size() > p.size() && !n.compare(0, p.size(), p) && n[p.size()] == '/')) return true;
    return false;
}

/* A path below the mod no part of which is a dot folder, backups or a patch folder. */
static bool part_ok(const std::string& rel)
{
    for (const std::string& part : split_parts(rel))
        if (part[0] == '.' || ieq(part, "backups") || ieq(part, "texture_patches") || ieq(part, "xpps_patches")) return false;
    return true;
}

/* What the mod changes with its chosen options: the Manager's ListModFiles,
 * ListTexturePatches and ListXppsPatches. A file it cannot use makes the
 * whole mod unusable (the Manager refuses to enable it). */
static void read_parts(Mod& m)
{
    /* m.err holds only this function's verdict (a mod.json error never gets
     * here), so a pick that fixes the options clears an old one */
    m.err.clear();
    m.files.clear(); m.tex.clear(); m.xp.clear(); m.targets.clear();
    std::vector<std::string> selv;
    const std::vector<std::string>* sel = nullptr;
    std::vector<std::string> roots;   /* the chosen options' folders */
    if (!m.modules.empty()) {
        const std::set<std::string> ids = picked_ids(m);
        std::set<std::string> ps;
        for (const Module& x : m.modules) if (ids.count(lower(x.id))) for (const std::string& p : x.paths) ps.insert(lower(p));
        selv.assign(ps.begin(), ps.end());
        sel = &selv;
        for (const Module& x : m.modules) {
            if (!ids.count(lower(x.id))) continue;
            for (const std::string& p : x.paths) {
                if (p.find("..") != std::string::npos) continue;
                const std::string full = join(m.path, p);
                if (is_dir(full) && !is_link(full) && std::find_if(roots.begin(), roots.end(), [&](const std::string& r) { return ieq(r, full); }) == roots.end())
                    roots.push_back(full);
            }
        }
        std::stable_sort(roots.begin(), roots.end(), ilt);
    }
    if (is_link(m.path)) { m.err = "its folder is a link (junction or symbolic link); copy the mod in instead"; return; }
    const std::string files_root = is_dir(join(m.path, "files")) && !is_link(join(m.path, "files")) ? join(m.path, "files") : m.path;
    const std::string backups = join(m.path, "backups");
    std::set<std::string> seen;

    /* whole files */
    struct F { std::string path, rel; };
    std::vector<F> fl;
    auto add_files = [&](const std::string& root, const std::string& rel_root) {
        std::vector<std::string> v;
        walk(root, ".xpps", v, 0, true);
        for (const std::string& p : v) {
            if (!ends_i(p, ".xpps") || !seen.insert(lower(p)).second) continue;
            if (!rel_to(backups, p).empty()) continue;
            const std::string rel = rel_to(rel_root, p);
            if (rel.empty() || !part_ok(rel) || !path_on(sel, rel)) continue;
            fl.push_back({ p, rel });
        }
    };
    add_files(files_root, files_root);
    for (const std::string& r : roots) add_files(r, m.path);
    std::stable_sort(fl.begin(), fl.end(), [](const F& a, const F& b) { return ilt(a.rel, b.rel); });
    for (const F& f : fl) {
        const std::string t = lower(base_of(f.rel));
        bool dup = false;
        for (const Part& q : m.files) if (q.target == t) dup = true;
        if (dup) { log_once("dup:" + m.folder + t, "%s: two files named %s; using the first (%s)", m.name.c_str(), t.c_str(), f.rel.c_str()); continue; }
        m.files.push_back({ f.path, t });
    }

    /* patches below <root>/texture_patches and <root>/xpps_patches */
    auto target_of = [](const std::string& patch_root, const std::string& p) {
        const std::vector<std::string> parts = split_parts(rel_to(patch_root, p));
        for (size_t i = 0; i + 1 < parts.size(); i++)
            if (ends_i(parts[i], ".xpps") || ends_i(parts[i], ".xpp")) return lower(parts[i]);
        return std::string();
    };
    std::set<std::string> seen_tex, seen_xp;
    auto add_patches = [&](const std::string& root, bool rel_mod) {
        const std::string rel_root = rel_mod ? m.path : root;
        const std::string tr = join(root, "texture_patches"), xr = join(root, "xpps_patches");
        std::vector<std::string> v;
        if (is_dir(tr) && !is_link(tr)) walk(tr, ".dds", v, 0, true);
        std::vector<Tex> tl;
        for (const std::string& p : v) {
            if (!seen_tex.insert(lower(p)).second || !path_on(sel, rel_to(rel_root, p))) continue;
            const std::string t = target_of(tr, p);
            if (t.empty()) continue;
            Tex x;
            x.path = p; x.target = t; x.name = base_of(p);
            if (!parse_tex_name(x.name, x.off, x.w, x.h, x.fmt, x.mips)) { m.err = x.name + " does not use the exported texture name format"; return; }
            if (x.w < 1 || x.h < 1 || x.w > 16384 || x.h > 16384 || x.mips < 1 || x.mips > 16) {
                m.err = x.name + " names a size or mip count no texture has";
                return;
            }
            Bytes hd;
            const FInfo fi = finfo(p);
            if (!read_all(p, hd, 0, 128) || hd.size() < 128 || memcmp(hd.data(), "DDS ", 4)) { m.err = x.name + " is not a DDS file"; return; }
            const int hh = (int)le32(&hd[12]), ww = (int)le32(&hd[16]), mm = (int)le32(&hd[28]);
            const std::string four((const char*)&hd[84], 4);
            if (ww != x.w || hh != x.h || upper(four) != x.fmt || mm != x.mips) {   /* the Manager upper-cases the fourcc */
                m.err = x.name + " header does not match its name";
                return;
            }
            if (fi.size - 128 != dxt_size(x.w, x.h, x.fmt, x.mips)) { m.err = x.name + " is not the size its name says"; return; }
            tl.push_back(x);
        }
        std::stable_sort(tl.begin(), tl.end(), [](const Tex& a, const Tex& b) {
            if (a.target != b.target) return a.target < b.target;
            if (a.off != b.off) return a.off < b.off;
            return ilt(a.name, b.name); });
        for (Tex& x : tl) m.tex.push_back(x);
        v.clear();
        if (is_dir(xr) && !is_link(xr)) walk(xr, ".bin", v, 0, true);
        std::vector<Xp> xl;
        for (const std::string& p : v) {
            if (!seen_xp.insert(lower(p)).second || !path_on(sel, rel_to(rel_root, p))) continue;
            const std::string t = target_of(xr, p);
            if (t.empty()) continue;
            Xp x;
            x.path = p; x.target = t; x.name = base_of(p);
            if (!parse_xp_name(x.name, x.index, x.abs, x.chunk, x.choff)) { m.err = x.name + " is not named file_<index>_<offset>[_offset_<offset>].bin"; return; }
            x.len = (uint32_t)finfo(p).size;
            xl.push_back(x);
        }
        std::stable_sort(xl.begin(), xl.end(), [](const Xp& a, const Xp& b) {
            if (a.target != b.target) return a.target < b.target;
            if (a.index != b.index) return a.index < b.index;
            if (a.abs != b.abs) return a.abs < b.abs;
            return ilt(a.name, b.name); });
        for (Xp& x : xl) m.xp.push_back(x);
    };
    add_patches(files_root, false);
    for (const std::string& r : roots) { if (!m.err.empty()) break; add_patches(r, true); }
    if (!m.err.empty()) { m.files.clear(); m.tex.clear(); m.xp.clear(); return; }
    for (const Part& p : m.files) m.targets.insert(p.target);
    for (const Tex& p : m.tex) m.targets.insert(p.target);
    for (const Xp& p : m.xp) m.targets.insert(p.target);
    if (m.targets.empty()) m.err = "it has no .xpps files, texture patches or xpps patches (with these options)";
}

/* ---- the saved choices (mods.ini) ------------------------------------------ */

struct Saved { bool have_on = false, on = true, have_pick = false; std::vector<std::string> pick; };

static std::map<std::string, Saved> state_read()
{
    std::map<std::string, Saved> st;
    FILE* f = g.state_path.empty() ? nullptr : fopen(g.state_path.c_str(), "r");
    if (!f) return st;
    char line[2048];
    std::string cur;
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) a++;
        s.erase(0, a);
        if (s.empty() || s[0] == '#' || s[0] == ';') continue;
        if (s[0] == '[' && s.back() == ']') { cur = lower(s.substr(1, s.size() - 2)); continue; }
        const size_t eq = s.find('=');
        if (cur.empty() || eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
        Saved& sv = st[cur];
        if (k == "on") { sv.have_on = true; sv.on = v != "0"; }
        else if (k == "pick") {
            sv.have_pick = true; sv.pick.clear();
            size_t s0 = 0;
            for (size_t i = 0; i <= v.size(); i++)
                if (i == v.size() || v[i] == ',') { if (i > s0) sv.pick.push_back(v.substr(s0, i - s0)); s0 = i + 1; }
        }
    }
    fclose(f);
    return st;
}

/* Called with g.mx held. */
static void state_write_locked()
{
    if (g.state_path.empty()) return;
    std::string out = "# inFAMOUS 2 port: the game mods the menu turned off and the options picked\n"
                      "# (F2 > Mods). A mod not listed here is on, with its default options.\n";
    for (const Mod& m : g.list) {
        if (m.on && !m.have_pick) continue;
        out += "[" + m.folder + "]\n";
        if (!m.on) out += "on = 0\n";
        if (m.have_pick) {
            out += "pick = ";
            for (size_t i = 0; i < m.pick.size(); i++) out += (i ? "," : "") + m.pick[i];
            out += "\n";
        }
    }
    Bytes b(out.begin(), out.end());
    if (!write_all(g.state_path, b)) fprintf(stderr, "[mods] could not save %s\n", g.state_path.c_str());
}

/* The mod folders: <dir>/<x>/mod.json, or <dir>/<x>/<y>/mod.json (a zip
 * unpacked into a folder of its own name). Dot folders are skipped. */
static std::vector<Mod> scan()
{
    std::vector<Mod> out;
    if (g.dir.empty()) return out;
    const std::map<std::string, Saved> st = state_read();
    auto add = [&](const std::string& path, const std::string& folder) {
        Mod m;
        m.path = path; m.folder = folder;
        std::string jerr;
        const J j = read_json(join(path, "mod.json"), &jerr);
        m.name = j.str("name", base_of(folder));
        if (m.name.empty()) m.name = base_of(folder);
        m.author = j.str("author", "unknown");
        m.version = j.str("version", "unknown");
        m.game = game_key(j.str("game"));
        m.for_game = m.game == "inf2";
        auto it = st.find(lower(folder));
        if (it != st.end()) {
            if (it->second.have_on) m.on = it->second.on;
            m.have_pick = it->second.have_pick; m.pick = it->second.pick;
        }
        if (!jerr.empty()) m.err = jerr;
        else if (m.for_game) { read_modules(m); read_parts(m); }
        out.push_back(std::move(m));
    };
    for (const std::string& n : list_dir(g.dir)) {
        if (n[0] == '.') continue;
        const std::string p = join(g.dir, n);
        if (!is_dir(p)) continue;
        if (finfo(join(p, "mod.json")).ok) { add(p, n); continue; }
        for (const std::string& n2 : list_dir(p)) {
            if (n2[0] == '.') continue;
            const std::string p2 = join(p, n2);
            if (is_dir(p2) && finfo(join(p2, "mod.json")).ok) add(p2, n + "/" + n2);
        }
    }
    return out;
}

/* ---- building what the game gets ------------------------------------------- */

struct Arc { std::string path; int prio; };

static int arc_prio(const std::string& name)
{
    const std::string s = lower(name);
    if (!s.compare(0, 5, "patch")) return 100 + atoi(s.c_str() + 5);
    if (s == "infamous2.psarc_s") return 2;
    if (s == "infamous1.psarc_s") return 1;
    return 0;
}

/* The game's big archives, highest first (patchN by N, then infamous2, infamous1). */
static std::vector<Arc> archives()
{
    std::vector<std::string> v;
    if (const char* h = getenv("PS3_HDD0_ROOT")) {
        /* Only the running game's folders: a shared dev_hdd0 can hold other
         * games' patchN.psarc_s, which would outrank this game's. The game's id
         * is PS3_TITLE_ID, else the first 9 characters of the folder that holds
         * infamous2.psarc_s (BCES01143_INSTALL -> BCES01143, whose patches are
         * in BCES01143); none found: every folder, as before. */
        const std::string gd = join(slashes(h), "game");
        const std::vector<std::string> titles = list_dir(gd);
        std::string id;
        if (const char* t = getenv("PS3_TITLE_ID")) id = t;
        for (size_t i = 0; id.empty() && i < titles.size(); i++) {
            std::vector<std::string> w;
            walk(join(join(gd, titles[i]), "USRDIR"), ".psarc_s", w);
            for (const std::string& p : w)
                if (ieq(base_of(p), "infamous2.psarc_s")) { id = titles[i].substr(0, 9); break; }
        }
        for (const std::string& t : titles)
            if (id.empty() || (t.size() >= id.size() && lang_ieq_n(t.c_str(), id.c_str(), id.size())))
                walk(join(join(gd, t), "USRDIR"), ".psarc_s", v);
    }
    walk(join(slashes(ppu_vfs_root ? ppu_vfs_root : "."), "PS3_GAME/USRDIR"), ".psarc_s", v);
    std::vector<Arc> a;
    std::set<std::string> seen;
    for (const std::string& p : v) if (seen.insert(lower(p)).second) a.push_back({ p, arc_prio(base_of(p)) });
    std::stable_sort(a.begin(), a.end(), [](const Arc& x, const Arc& y) { return x.prio > y.prio; });
    return a;
}

/* The game's big archives, opened on first need (highest first). */
struct Arcs {
    bool loaded = false;
    std::vector<std::unique_ptr<Psarc>> v;
    Psarc* find(const std::string& name, int& idx)
    {
        if (!loaded) {
            loaded = true;
            for (const Arc& a : archives()) {
                std::unique_ptr<Psarc> p(new Psarc());
                if (p->open(a.path)) v.push_back(std::move(p));
                else fprintf(stderr, "[mods] cannot read the archive %s\n", a.path.c_str());
            }
        }
        for (auto& p : v) if ((idx = p->find(name)) >= 0) return p.get();
        return nullptr;
    }
};

static std::string sig_line(const char* kind, const std::string& p)
{
    const FInfo f = finfo(p);
    char b[64];
    snprintf(b, sizeof b, " %llu %lld\n", (unsigned long long)f.size, (long long)f.mtime);
    return std::string(kind) + " " + p + b;
}

/* The game file `name` with the patches written in (base: a mod's whole file,
 * else the original), built into the cache unless the cache already holds
 * this exact build. dry: only whether it builds (nothing read from or
 * written to the cache). */
static bool build(const std::string& name, const Part* base, const std::vector<const Tex*>& tex,
                  const std::vector<const Xp*>& xp, Arcs& arcs, std::string& out_path, std::string& err, bool dry = false)
{
    out_path = join(g.cache, name);
    std::string sig = "gmods 1\n";
    std::string from;
    Psarc* arc = nullptr;
    int idx = -1;
    if (base) sig += sig_line("base", base->path);
    else {
        arc = arcs.find(name, idx);
        if (!arc) { err = name + " is not in the game's archives"; return false; }
        from = arc->path;
        sig += sig_line("archive", from);
    }
    for (const Tex* t : tex) sig += sig_line("tex", t->path);
    for (const Xp* x : xp) sig += sig_line("xpps", x->path);
    Bytes old;
    if (!dry && finfo(out_path).ok && read_all(out_path + ".sig", old) && std::string(old.begin(), old.end()) == sig) return true;

    Bytes d;
    if (base) { if (!read_all(base->path, d)) { err = "cannot read " + base->path; return false; } }
    else if (!arc->entry((uint32_t)idx, d)) { err = "cannot read " + name + " out of " + base_of(slashes(from)); return false; }
    if (!tex.empty()) {
        if (d.size() < 48 || memcmp(d.data(), "PACK", 4)) { err = name + " is not an XPPS/XPP PACK file"; return false; }
        const uint64_t ds = be32(&d[40]);
        for (const Tex* t : tex) {
            Bytes pl;
            if (!read_all(t->path, pl, 128)) { err = "cannot read " + t->path; return false; }
            /* the size the scan checked; the file may have changed since */
            if (pl.size() != dxt_size(t->w, t->h, t->fmt, t->mips)) { err = t->name + " is not the size its name says"; return false; }
            const uint64_t at = ds + t->off;
            if (at + pl.size() > d.size()) { err = t->name + " would patch outside " + name; return false; }
            flip_payload(pl.data(), t->w, t->h, t->fmt, t->mips);
            memcpy(d.data() + at, pl.data(), pl.size());
        }
    }
    if (!xp.empty()) {
        XpLayout L;
        if (!xp_layout(d, L, err)) { err = name + ": " + err; return false; }
        for (const Xp* x : xp) {
            Bytes pl;
            if (!read_all(x->path, pl)) { err = "cannot read " + x->path; return false; }
            int fi = -1;
            if (x->index >= 0 && x->index < (int)L.files.size()) fi = x->index;
            else {
                int hits = 0;
                for (size_t i = 0; i < L.files.size(); i++)
                    if ((uint64_t)L.data_off + L.files[i].off == x->abs) { fi = (int)i; hits++; }
                if (hits != 1) { err = x->name + " does not match any file entry in " + name; return false; }
            }
            XpFile& F = L.files[(size_t)fi];
            const uint64_t a0 = (uint64_t)L.data_off + F.off, a1 = a0 + F.size;
            if (a1 > d.size()) { err = x->name + " points outside " + name; return false; }
            if (x->chunk) {
                if ((uint64_t)x->choff + pl.size() > F.size) { err = x->name + " points outside file_" + std::to_string(fi) + " in " + name; return false; }
                memcpy(d.data() + a0 + x->choff, pl.data(), pl.size());
                continue;
            }
            /* the whole packed file: later files, the chunks after it and the data size move.
             * The table entries are rewritten at their places in the file, which
             * only stay put when the replaced bytes lie after the header and both
             * tables -- as in every one of the game's own files. A file whose
             * header puts a packed file over its tables (a mod's own copy can) is
             * refused, and every table write is bounds-checked, as the Manager's
             * WriteXppsU32 is. */
            if (a0 < L.tables_end) { err = x->name + " would replace a part of " + name + " that holds its tables"; return false; }
            const int64_t delta = (int64_t)pl.size() - (int64_t)F.size;
            Bytes nd;
            nd.reserve((size_t)((int64_t)d.size() + delta));
            nd.insert(nd.end(), d.begin(), d.begin() + (ptrdiff_t)a0);
            nd.insert(nd.end(), pl.begin(), pl.end());
            nd.insert(nd.end(), d.begin() + (ptrdiff_t)a1, d.end());
            d.swap(nd);
            bool in_range = true;
            auto put = [&](uint64_t at, uint32_t v) {
                if (at + 4 > d.size()) { in_range = false; return; }
                put_be32(&d[(size_t)at], v);
            };
            const uint32_t old_off = F.off;
            F.size = (uint32_t)pl.size();
            put(F.size_at, F.size);
            if (delta) {
                for (size_t i = (size_t)fi + 1; i < L.files.size(); i++) {
                    L.files[i].off = (uint32_t)((int64_t)L.files[i].off + delta);
                    put(L.files[i].off_at, L.files[i].off);
                }
                int ci = -1;
                for (size_t i = 0; i < L.chunks.size(); i++)
                    if ((int64_t)L.chunks[i].file_index <= fi && fi < (int64_t)L.chunks[i].file_index + L.chunks[i].file_count) { ci = (int)i; break; }
                if (ci >= 0) {
                    L.chunks[(size_t)ci].size = (uint32_t)((int64_t)L.chunks[(size_t)ci].size + delta);
                    put(L.chunks[(size_t)ci].size_at, L.chunks[(size_t)ci].size);
                }
                for (size_t i = 0; i < L.chunks.size(); i++)
                    if ((int)i != ci && L.chunks[i].off > old_off) {
                        L.chunks[i].off = (uint32_t)((int64_t)L.chunks[i].off + delta);
                        put(L.chunks[i].off_at, L.chunks[i].off);
                    }
                L.data_size = (uint32_t)((int64_t)L.data_size + delta);
                put(44, L.data_size);
            }
            if (!in_range) { err = x->name + ": a table entry of " + name + " lies outside it"; return false; }
        }
    }
    if (dry) return true;
    if (!write_all(out_path, d)) { err = "cannot write " + out_path; return false; }
    Bytes sb(sig.begin(), sig.end());
    write_all(out_path + ".sig", sb);
    fprintf(stderr, "[mods] built %s (%zu bytes: %zu texture patch(es), %zu xpps patch(es) on %s)\n", name.c_str(), d.size(),
            tex.size(), xp.size(), base ? "the mod's file" : base_of(from).c_str());
    return true;
}

static bool xp_overlap(const Xp& a, const Xp& b)
{
    if (a.target != b.target || a.index != b.index) return false;
    if (!a.chunk || !b.chunk) return true;
    return a.choff < b.choff + b.len && b.choff < a.choff + a.len;
}

/* Which mods are used, and what the game gets for each file they change. */
static void resolve(std::vector<Mod>& mods)
{
    Arcs arcs;
    std::map<std::string, std::string> why;    /* lower(folder) -> why it is left out */
    /* every failed round leaves out at least one more mod, so this ends */
    for (size_t round = 0; round <= mods.size() + 1; round++) {
        std::map<std::string, size_t> whole;                 /* target -> mod */
        std::map<std::string, size_t> texk;                  /* target:offset -> mod */
        std::map<std::string, std::vector<size_t>> patched;  /* target -> mods with patches on it */
        std::vector<std::pair<size_t, const Xp*>> xps;
        std::vector<size_t> used;
        for (size_t i = 0; i < mods.size(); i++) {
            Mod& m = mods[i];
            if (!m.on || !m.for_game || !m.err.empty() || why.count(lower(m.folder))) continue;
            std::set<size_t> clash;
            for (const Part& f : m.files) {
                if (whole.count(f.target)) clash.insert(whole[f.target]);
                for (size_t o : patched[f.target]) clash.insert(o);
            }
            for (const Tex& t : m.tex) {
                const std::string k = t.target + ":" + std::to_string(t.off);
                if (texk.count(k)) clash.insert(texk[k]);
                if (whole.count(t.target)) clash.insert(whole[t.target]);
            }
            for (const Xp& x : m.xp) {
                if (whole.count(x.target)) clash.insert(whole[x.target]);
                for (const auto& o : xps) if (xp_overlap(x, *o.second)) clash.insert(o.first);
            }
            clash.erase(i);
            if (!clash.empty()) {
                std::string s = "it changes the same files as ";
                bool first = true;
                for (size_t o : clash) { s += (first ? "" : ", ") + mods[o].name; first = false; }
                why[lower(m.folder)] = s;
                continue;
            }
            for (const Part& f : m.files) whole[f.target] = i;
            for (const Tex& t : m.tex) { texk[t.target + ":" + std::to_string(t.off)] = i; patched[t.target].push_back(i); }
            for (const Xp& x : m.xp) { xps.push_back({ i, &x }); patched[x.target].push_back(i); }
            used.push_back(i);
        }
        /* each changed file: a whole file as it is, or a build */
        std::map<std::string, std::string> serve, by;
        bool again = false;
        std::set<std::string> targets;
        for (size_t i : used) for (const std::string& t : mods[i].targets) targets.insert(t);
        for (const std::string& t : targets) {
            const Part* base = nullptr;
            size_t base_mod = 0;
            std::vector<const Tex*> tl;
            std::vector<const Xp*> xl;
            std::set<size_t> who;
            for (size_t i : used) {
                for (const Part& f : mods[i].files) if (f.target == t) { base = &f; base_mod = i; who.insert(i); }
                for (const Tex& x : mods[i].tex) if (x.target == t) { tl.push_back(&x); who.insert(i); }
                for (const Xp& x : mods[i].xp) if (x.target == t) { xl.push_back(&x); who.insert(i); }
            }
            std::string bym;
            for (size_t i : who) bym += (bym.empty() ? "" : " + ") + mods[i].name;
            if (tl.empty() && xl.empty()) { serve[t] = base->path; by[t] = bym; continue; }
            (void)base_mod;
            std::string path, err;
            if (!build(t, base, tl, xl, arcs, path, err)) {
                fprintf(stderr, "[mods] %s: %s\n", t.c_str(), err.c_str());
                /* Who is at fault: each mod on this file alone (a dry build). The
                 * ones that fail alone are left out; if each builds alone, the
                 * last one by folder name, which came after the others (as the
                 * Manager refuses the mod being turned on, not the ones before). */
                bool blamed = false;
                if (who.size() > 1)
                    for (size_t i : who) {
                        const Part* b1 = nullptr;
                        std::vector<const Tex*> t1;
                        std::vector<const Xp*> x1;
                        for (const Part& f : mods[i].files) if (f.target == t) b1 = &f;
                        for (const Tex& x : mods[i].tex) if (x.target == t) t1.push_back(&x);
                        for (const Xp& x : mods[i].xp) if (x.target == t) x1.push_back(&x);
                        std::string p1, e1;
                        if (!build(t, b1, t1, x1, arcs, p1, e1, true)) { why[lower(mods[i].folder)] = "could not build " + t + ": " + e1; blamed = true; }
                    }
                if (!blamed) {
                    const size_t last = *who.rbegin();
                    why[lower(mods[last].folder)] = (who.size() > 1 ? "could not build " + t + " together with the mods before it: "
                                                                     : "could not build " + t + ": ") + err;
                }
                again = true;
                break;
            }
            serve[t] = path; by[t] = bym;
        }
        if (again) continue;
        std::lock_guard<std::mutex> lk(g.mx);
        g.serve.swap(serve);
        g.serve_mod.swap(by);
        for (Mod& m : mods) {
            StartInfo si;
            si.active = std::find(used.begin(), used.end(), (size_t)(&m - mods.data())) != used.end();
            auto w = why.find(lower(m.folder));
            if (w != why.end()) si.why = w->second;
            g.start[lower(m.folder)] = si;
        }
        g.nserve.store((int)g.serve.size(), std::memory_order_release);
        return;
    }
    fprintf(stderr, "[mods] gave up building the mods' files; no mod is used\n");
    std::lock_guard<std::mutex> lk(g.mx);
    g.serve.clear();
    g.serve_mod.clear();
    for (const Mod& m : mods) {
        StartInfo si;
        auto w = why.find(lower(m.folder));
        si.why = w != why.end() ? w->second : "the mods' files could not be built (see the session log)";
        g.start[lower(m.folder)] = si;
    }
    g.nserve.store(0, std::memory_order_release);
}

static std::string settings_dir()
{
#ifdef _WIN32
    const char* a = getenv("APPDATA");
    return a && *a ? join(slashes(a), "inFAMOUS2-port") : std::string();
#else
    const char* x = getenv("XDG_CONFIG_HOME");
    const char* h = getenv("HOME");
    if (x && *x) return join(x, "infamous2-port");
    return h && *h ? join(join(h, ".config"), "infamous2-port") : std::string();
#endif
}

static void init_body();
static void init()
{
    std::call_once(g.once, [] {
        /* a mod the scan or a build cannot hold in memory must not crash the
         * game at every start: no mod is used then, and the log says why */
        try { init_body(); }
        catch (const std::exception& e) {
            fprintf(stderr, "[mods] could not read the mods (%s); none is used this session\n", e.what());
            std::lock_guard<std::mutex> lk(g.mx);
            g.serve.clear(); g.serve_mod.clear();
            g.nserve.store(0, std::memory_order_release);
        }
    });
}
static void init_body()
{
    {
        const char* d = getenv("PS3_GAME_MODS_DIR");
        if (!d || !*d) return;
        g.dir = slashes(d);
        while (g.dir.size() > 1 && g.dir.back() == '/') g.dir.pop_back();
        const char* u = getenv("PS3_GAME_MODS");
        g.use = !(u && u[0] == '0' && !u[1]);
        const char* s = getenv("PS3_GAME_MODS_STATE");
        g.state_path = s ? slashes(s) : (settings_dir().empty() ? std::string() : join(settings_dir(), "mods.ini"));
        const char* c = getenv("PS3_GAME_MODS_CACHE");
        g.cache = c && *c ? slashes(c) : join(g.dir, ".port_cache");
        std::vector<Mod> mods = scan();
        int n_for = 0;
        for (const Mod& m : mods) if (m.for_game) n_for++;
        fprintf(stderr, "[mods] %s: %zu mod(s), %d for inFAMOUS 2%s\n", g.dir.c_str(), mods.size(), n_for,
                g.use ? "" : "; PS3_GAME_MODS=0: none used");
        if (g.use) resolve(mods);
        for (const Mod& m : mods) {
            const StartInfo si = g.start.count(lower(m.folder)) ? g.start[lower(m.folder)] : StartInfo();
            std::string t;
            for (const std::string& x : m.targets) t += (t.empty() ? "" : ", ") + x;
            if (si.active)
                fprintf(stderr, "[mods] using \"%s\" %s by %s: %zu file(s), %zu texture patch(es), %zu xpps patch(es) -> %s\n",
                        m.name.c_str(), m.version.c_str(), m.author.c_str(), m.files.size(), m.tex.size(), m.xp.size(), t.c_str());
            else
                fprintf(stderr, "[mods] not using \"%s\": %s\n", m.name.c_str(),
                        !m.err.empty() ? m.err.c_str() : !m.for_game ? "not made for inFAMOUS 2" : !m.on ? "turned off" :
                        !g.use ? "mods are off" : si.why.c_str());
        }
        std::lock_guard<std::mutex> lk(g.mx);
        g.list.swap(mods);
    }
}

/* The name whose mod file the game gets for `k` (lower case). With a text
 * language chosen (PS3_TEXT_LANG, ppu_fs_lang.h), every lang_*.xpps entry of
 * an archive is pointed at lang_<text>.xpps: whatever name the game asks for,
 * it reads that text. So a mod's lang_<text>.xpps stands for every
 * lang_*.xpps name, and a mod's other lang_*.xpps (text the player did not
 * choose) is not used. Without a chosen text language, names are as they are. */
static std::string serve_key(const std::string& k)
{
    if (k.size() <= 10 || k.compare(0, 5, "lang_") || !ends_i(k, ".xpps")) return k;
    lang_init();
    if (!g_lang.text[0]) return k;
    return lower(std::string("lang_") + g_lang.text + ".xpps");
}

/* A guest open (read-only) or stat of a loose file under USRDIR whose name a
 * mod changes: the mod's file (or the build) instead. */
static void redirect(const char* guest, char* out, size_t cap)
{
    init();
    if (!g.nserve.load(std::memory_order_acquire)) return;
    const char* u = nullptr;
    for (const char* p = guest; *p; p++) if (lang_ieq_n(p, "/USRDIR/", 8)) { u = p; break; }
    if (!u) return;
    const char* b = strrchr(guest, '/');
    const std::string key = serve_key(lower(b + 1));
    std::string host, by;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        auto it = g.serve.find(key);
        if (it == g.serve.end()) return;
        host = it->second; by = g.serve_mod[key];
    }
    if (host.size() >= cap) {   /* cut short, it would name another file */
        log_once("long:" + host, "%s: the path is too long to hand to the game; not used: %s", guest, host.c_str());
        return;
    }
    snprintf(out, cap, "%s", host.c_str());
    log_once("serve:" + std::string(guest), "%s -> %s's %s", guest, by.c_str(), host.c_str());
}

/* An archive was opened on fd: hide the files the mods change from its TOC. */
static void on_open(int fd, const char* hpath)
{
    init();
    if (fd < 0 || fd >= LANG_FD_MAX || !g.nserve.load(std::memory_order_acquire)) return;
    const std::string hp = hpath;
    if (!ends_i(hp, ".psarc") && !ends_i(hp, ".psarc_s")) return;
    Psarc p;
    if (!p.open(hp)) return;
    std::vector<LangPatch> add;
    std::string hidden;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        for (uint32_t i = 1; i < p.n; i++) {
            if (!g.serve.count(serve_key(p.names[i]))) continue;
            uint8_t md5[16];
            memcpy(md5, p.rec(i), 16);
            for (int k = 15; k >= 0 && ++md5[k] == 0; k--) { }   /* one larger: the lookup misses */
            LangPatch lp; lp.off = 32 + (uint64_t)i * p.esz; lp.len = 16; memcpy(lp.bytes, md5, 16);
            add.push_back(lp);
            hidden += (hidden.empty() ? "" : ", ") + p.names[i];
        }
    }
    if (add.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_lang_patch_mx);
        for (const LangPatch& lp : add) g_lang_patch[fd].push_back(lp);
        g_lang_has_patch[fd].store(1, std::memory_order_release);
    }
    log_once("hide:" + hp, "%s: %zu file(s) hidden, the mods' are read instead: %s", base_of(slashes(hp)).c_str(), add.size(), hidden.c_str());
}

} /* namespace gmods */

/* ---- for the in-game menu (vk_overlay.cpp) --------------------------------- */

extern "C" const char* ps3_game_mods_dir(void) { gmods::init(); return gmods::g.dir.c_str(); }

extern "C" int ps3_game_mods_count(void)
{
    gmods::init();
    std::lock_guard<std::mutex> lk(gmods::g.mx);
    return (int)gmods::g.list.size();
}

extern "C" int ps3_game_mods_in_use(void) { gmods::init(); return gmods::g.nserve.load(std::memory_order_acquire); }

extern "C" int ps3_game_mods_get(int i, ps3_game_mod* o)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size() || !o) return 0;
    const Mod& m = g.list[(size_t)i];
    memset(o, 0, sizeof *o);
    snprintf(o->folder, sizeof o->folder, "%s", m.folder.c_str());
    snprintf(o->name, sizeof o->name, "%s", m.name.c_str());
    snprintf(o->author, sizeof o->author, "%s", m.author.c_str());
    snprintf(o->version, sizeof o->version, "%s", m.version.c_str());
    o->on = m.on; o->for_game = m.for_game;
    o->n_files = (int)m.files.size(); o->n_tex = (int)m.tex.size(); o->n_xpps = (int)m.xp.size();
    o->n_modules = (int)m.modules.size();
    auto it = g.start.find(lower(m.folder));
    o->active = it != g.start.end() && it->second.active;
    std::string t;
    int k = 0;
    for (const std::string& x : m.targets) { if (k++ < 4) t += (t.empty() ? "" : ", ") + x; }
    if (k > 4) t += ", +" + std::to_string(k - 4) + " more";
    std::string s;
    if (!m.err.empty()) s = "Not used: " + m.err;   /* a broken mod.json names no game: its error first */
    else if (!m.for_game)
        s = m.game.empty() ? "Not used: its mod.json does not name the game (\"game\": \"inf2\")"
                           : std::string("Made for ") + game_label(m.game) + ", not inFAMOUS 2";
    else if (o->active) s = (m.on ? "In use: changes " : "Off from the next start (in use now: changes ") + t + (m.on ? "" : ")");
    else if (!m.on) s = "Off";
    else if (!g.use) s = "Not used: mods are off";
    else if (it != g.start.end() && !it->second.why.empty()) s = "Not used: " + it->second.why;
    else s = "Used from the next start: changes " + t;
    snprintf(o->status, sizeof o->status, "%s", s.c_str());
    return 1;
}

extern "C" int ps3_game_mods_option(int i, int k, ps3_game_mod_option* o)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size() || !o) return 0;
    const Mod& m = g.list[(size_t)i];
    if (k < 0 || k >= (int)m.modules.size()) return 0;
    const Module& x = m.modules[(size_t)k];
    memset(o, 0, sizeof *o);
    snprintf(o->id, sizeof o->id, "%s", x.id.c_str());
    snprintf(o->name, sizeof o->name, "%s", x.name.c_str());
    snprintf(o->group, sizeof o->group, "%s", x.group_name.c_str());
    snprintf(o->group_id, sizeof o->group_id, "%s", x.group_id.c_str());
    o->dropdown = x.dropdown;
    o->picked = picked_ids(m).count(lower(x.id)) ? 1 : 0;
    return 1;
}

extern "C" void ps3_game_mods_set_on(int i, int on)
{
    using namespace gmods;
    init();
    std::lock_guard<std::mutex> lk(g.mx);
    if (i < 0 || i >= (int)g.list.size()) return;
    g.list[(size_t)i].on = on != 0;
    state_write_locked();
}

extern "C" void ps3_game_mods_pick(int i, int k, int on)
{
    using namespace gmods;
    init();
    /* a copy, read without the lock (read_parts walks the mod's folders, and
     * every guest USRDIR open waits on g.mx), then put back */
    Mod m;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        if (i < 0 || i >= (int)g.list.size()) return;
        m = g.list[(size_t)i];
    }
    if (k < 0 || k >= (int)m.modules.size()) return;
    const Module x = m.modules[(size_t)k];
    std::set<std::string> ids = picked_ids(m);
    if (x.dropdown) {
        for (const Module& y : m.modules) if (y.dropdown && ieq(y.group_id, x.group_id)) ids.erase(lower(y.id));
        ids.insert(lower(x.id));
    } else if (on) ids.insert(lower(x.id));
    else ids.erase(lower(x.id));
    /* the Manager's "Select at least one module": an empty choice is never
     * saved (read back, it would mean every option) */
    if (ids.empty()) return;
    m.pick.clear();
    for (const Module& y : m.modules) if (ids.count(lower(y.id))) m.pick.push_back(y.id);
    m.have_pick = true;
    try { read_parts(m); }
    catch (const std::exception&) {
        m.files.clear(); m.tex.clear(); m.xp.clear(); m.targets.clear();
        m.err = "its files could not be read (too big?)";
    }
    std::lock_guard<std::mutex> lk(g.mx);
    if (i >= (int)g.list.size() || g.list[(size_t)i].folder != m.folder) return;   /* rescanned meanwhile */
    g.list[(size_t)i] = std::move(m);
    state_write_locked();
}

extern "C" void ps3_game_mods_rescan(void)
{
    using namespace gmods;
    init();
    if (g.dir.empty()) return;
    std::vector<Mod> mods;
    try { mods = scan(); }
    catch (const std::exception& e) { fprintf(stderr, "[mods] could not read the mods folder again (%s)\n", e.what()); return; }
    std::lock_guard<std::mutex> lk(g.mx);
    g.list.swap(mods);
}

/* Opens the mods folder in the file manager (Windows; elsewhere nothing). */
extern "C" void ps3_game_mods_open_folder(void)
{
    gmods::init();
    if (gmods::g.dir.empty()) return;
#ifdef _WIN32
    gmods::mkdirs(gmods::g.dir);
    std::string w = gmods::g.dir;
    for (char& c : w) if (c == '/') c = '\\';
    ShellExecuteA(NULL, "open", w.c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
}

#endif /* PPU_FS_MODS_H */
