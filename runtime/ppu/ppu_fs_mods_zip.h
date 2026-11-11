/*
 * ps3recomp - the Mods page's "Add a mod from a zip" (included by
 * ppu_fs_mods.h inside namespace gmods, after the mods code it uses)
 *
 * A small zip reader: the central directory, stored and deflated entries
 * (deflate through stb_image's zlib decoder, raw: stbi_zlib_decode_noheader_buffer,
 * which never writes past the size the directory gives), each file's CRC-32
 * checked. Refused: ZIP64, archives split into parts, encrypted entries,
 * other compression methods.
 *
 * zip_check reads a zip and decides everything before anything is written:
 *  - every entry name passes safe_rel (no absolute path, "..", drive letter,
 *    ':' stream, device name; '\' counts as '/' and is checked the same) and
 *    no two entries are the same file (case-insensitively);
 *  - no program anywhere in the zip (exe, dll, bat, cmd, ps1, vbs, js, msi,
 *    scr, com, lnk, py, jar, sys, ...: kProgram): the whole zip is refused,
 *    "contains a program, not a mod";
 *  - at most 20,000 entries, 512 MB per file, 2 GB in all;
 *  - exactly one mod.json at the top or one folder down, a JSON object of at
 *    most 1 MB that names inFAMOUS 2 (as the scan reads it);
 *  - every entry's local header agrees with the directory, and its data lies
 *    before the directory;
 *  - the paths fit the mods folder (narrow paths below MAX_PATH).
 * Only mod files are extracted (kModExt: json xpps xpp dds bin png jpg jpeg
 * txt md); other files, and files outside the mod's folder, are left out and
 * counted. The mod's folder is the folder holding mod.json (the zip's own
 * name for a mod.json at the top). If the mods folder has it already, only
 * Replace installs it.
 *
 * zip_install extracts into <mods>/.installing-<pid>-<n> (a dot folder: the
 * scan never lists it), then renames that to <mods>/<folder>. Replace first
 * moves the old folder to <mods>/.replaced-<pid>-<n>, puts the new one in its
 * place, and only then deletes the old one (it goes back if the rename fails).
 * Nothing else is ever deleted: remove_ours takes only those two kinds of
 * folder, directly inside the mods folder, and never follows a link.
 */
#ifndef PPU_FS_MODS_ZIP_H
#define PPU_FS_MODS_ZIP_H

/* stb_image's raw-deflate decoder (libs/codec/cellPngDec.c owns the implementation) */
extern "C" int stbi_zlib_decode_noheader_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

static const uint32_t kZipMaxEntries = 20000;
static const uint64_t kZipMaxFile = 512ull << 20;
static const uint64_t kZipMaxTotal = 2ull << 30;

static const char* const kModExt[] = { "json", "xpps", "xpp", "dds", "bin", "png", "jpg", "jpeg", "txt", "md" };
static const char* const kProgram[] = {
    "exe", "dll", "bat", "cmd", "ps1", "psm1", "psd1", "ps1xml", "vbs", "vbe", "vb", "js", "jse", "wsf", "wsh", "ws",
    "msi", "msp", "mst", "scr", "com", "pif", "cpl", "hta", "lnk", "url", "scf", "inf", "reg", "chm", "msc",
    "py", "pyw", "pyc", "pyo", "pyd", "pyz", "jar", "class", "sys", "drv", "ocx", "efi", "xll", "gadget",
    "application", "appx", "appxbundle", "msix", "msixbundle", "sh", "so", "dylib", "elf", "apk", "app",
};

/* the extension of a path's last part, lower case ("" = none) */
static std::string ext_of(const std::string& rel)
{
    const std::string b = base_of(rel);
    const size_t d = b.find_last_of('.');
    return d == std::string::npos || d == 0 ? std::string() : lower(b.substr(d + 1));
}
static bool in_list(const std::string& e, const char* const* v, size_t n)
{
    for (size_t i = 0; i < n; i++) if (e == v[i]) return true;
    return false;
}
static bool is_program(const std::string& e) { return in_list(e, kProgram, sizeof kProgram / sizeof kProgram[0]); }
static bool is_mod_ext(const std::string& e) { return in_list(e, kModExt, sizeof kModExt / sizeof kModExt[0]); }

static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t crc = 0)
{
    static uint32_t t[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    });
    crc = ~crc;
    while (n--) crc = t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- paths in UTF-8 (a zip may sit in a folder whose name is not ASCII) ---- */
#ifdef _WIN32
static std::wstring widen(const std::string& s, UINT cp = CP_UTF8)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string utf8_of(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
/* a narrow (ANSI) path as the C library takes it -> UTF-8 */
static std::string utf8_of_ansi(const std::string& s) { return utf8_of(widen(s, CP_ACP)); }
static FILE* fopen_u8(const std::string& p, const char* mode) { return _wfopen(widen(p).c_str(), widen(mode).c_str()); }
static FInfo finfo_u8(const std::string& p)
{
    FInfo f;
    struct _stat64 st;
    if (_wstat64(widen(p).c_str(), &st) != 0) return f;
    f.ok = true; f.dir = (st.st_mode & S_IFMT) == S_IFDIR; f.size = (uint64_t)st.st_size; f.mtime = (int64_t)st.st_mtime;
    return f;
}
#else
static FInfo finfo_u8(const std::string& p) { return finfo(p); }
static std::string utf8_of_ansi(const std::string& s) { return s; }
static FILE* fopen_u8(const std::string& p, const char* mode) { return fopen(p.c_str(), mode); }
#endif

/* ---- the reader ------------------------------------------------------------------ */

struct ZipEnt { std::string name; uint32_t crc = 0, csize = 0, usize = 0, lho = 0; uint16_t method = 0, flags = 0; bool dir = false; };
struct ZipFile {
    FILE* f = nullptr;
    uint64_t size = 0, cd_off = 0;
    std::vector<ZipEnt> ents;
    ZipFile() = default;
    ZipFile(const ZipFile&) = delete;
    ZipFile& operator=(const ZipFile&) = delete;
    ~ZipFile() { if (f) fclose(f); }
    bool read_at(uint64_t at, void* out, size_t n) { return LANG_FSEEK(f, at) == 0 && fread(out, 1, n, f) == n; }
};

/* The zip's central directory. err says why not, in words for the menu. */
static bool zip_open(const std::string& path_u8, ZipFile& z, std::string& err)
{
    z.f = fopen_u8(path_u8, "rb");
    if (!z.f) { err = "it cannot be opened"; return false; }
#ifdef _WIN32
    _fseeki64(z.f, 0, SEEK_END); const int64_t n = _ftelli64(z.f);
#else
    fseeko(z.f, 0, SEEK_END); const int64_t n = (int64_t)ftello(z.f);
#endif
    z.size = n > 0 ? (uint64_t)n : 0;
    if (z.size < 22) { err = "it is not a zip file"; return false; }
    if (z.size >= (4ull << 30)) { err = "it is 4 GB or bigger (ZIP64), which this reader does not take"; return false; }
    const size_t tail = (size_t)(z.size < 65557 ? z.size : 65557);
    Bytes t(tail);
    if (!z.read_at(z.size - tail, t.data(), tail)) { err = "it cannot be read"; return false; }
    size_t e = (size_t)-1;
    for (size_t i = tail - 22 + 1; i-- > 0; )
        if (le32(&t[i]) == 0x06054b50u && i + 22 + le16(&t[i + 20]) <= tail) { e = i; break; }
    if (e == (size_t)-1) { err = "it is not a zip file (no central directory)"; return false; }
    if (e >= 20 && le32(&t[e - 20]) == 0x07064b50u) { err = "it is a ZIP64 archive, which this reader does not take"; return false; }
    const uint8_t* r = &t[e];
    const uint64_t eocd = z.size - tail + e;
    const uint16_t disk = le16(r + 4), cd_disk = le16(r + 6), n_disk = le16(r + 8), cnt = le16(r + 10);
    const uint32_t cd_size = le32(r + 12), cd_off = le32(r + 16);
    if (cnt == 0xFFFF || n_disk == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        err = "it is a ZIP64 archive, which this reader does not take"; return false;
    }
    if (disk || cd_disk || n_disk != cnt) { err = "it is split into several parts (a multi-disk zip)"; return false; }
    if (cnt > kZipMaxEntries) { err = "it has more than 20,000 entries"; return false; }
    if ((uint64_t)cd_off + cd_size > eocd) { err = "it is damaged (its central directory is out of place)"; return false; }
    Bytes cd(cd_size);
    if (cd_size && !z.read_at(cd_off, cd.data(), cd_size)) { err = "it cannot be read"; return false; }
    size_t p = 0;
    for (uint32_t k = 0; k < cnt; k++) {
        if (p + 46 > cd.size() || le32(&cd[p]) != 0x02014b50u) { err = "it is damaged (its central directory is broken)"; return false; }
        const uint8_t* c = &cd[p];
        ZipEnt x;
        x.flags = le16(c + 8); x.method = le16(c + 10); x.crc = le32(c + 16);
        x.csize = le32(c + 20); x.usize = le32(c + 24); x.lho = le32(c + 42);
        const size_t nl = le16(c + 28), xl = le16(c + 30), cl = le16(c + 32);
        const uint16_t dstart = le16(c + 34);
        if (p + 46 + nl + xl + cl > cd.size()) { err = "it is damaged (its central directory is broken)"; return false; }
        x.name.assign((const char*)c + 46, nl);
        for (size_t q = 46 + nl; q + 4 <= 46 + nl + xl; ) {
            if (le16(c + q) == 0x0001) { err = "it is a ZIP64 archive, which this reader does not take"; return false; }
            q += 4 + (size_t)le16(c + q + 2);
        }
        if (x.csize == 0xFFFFFFFFu || x.usize == 0xFFFFFFFFu || x.lho == 0xFFFFFFFFu) {
            err = "it is a ZIP64 archive, which this reader does not take"; return false;
        }
        if (dstart) { err = "it is split into several parts (a multi-disk zip)"; return false; }
        x.dir = !x.name.empty() && (x.name.back() == '/' || x.name.back() == '\\');
        z.ents.push_back(x);
        p += 46 + nl + xl + cl;
    }
    z.cd_off = cd_off;
    return true;
}

/* Where entry x's data starts (its local header checked against the directory). */
static bool zip_data_at(ZipFile& z, const ZipEnt& x, uint64_t& at, std::string& err)
{
    uint8_t h[30];
    if (!z.read_at(x.lho, h, 30) || le32(h) != 0x04034b50u) { err = "it is damaged (" + x.name + " has no local header)"; return false; }
    const size_t nl = le16(h + 26), xl = le16(h + 28);
    std::string nm(nl, '\0');
    if (nl && !z.read_at(x.lho + 30, &nm[0], nl)) { err = "it cannot be read"; return false; }
    if (nm != x.name || le16(h + 8) != x.method || ((le16(h + 6) ^ x.flags) & 1)) {
        err = "it is damaged (the entry " + x.name + " does not match the zip's own list)"; return false;
    }
    at = (uint64_t)x.lho + 30 + nl + xl;
    if (at + x.csize > z.cd_off) { err = "it is damaged (" + x.name + " runs into the zip's list)"; return false; }
    return true;
}

/* Entry x's bytes, inflated and CRC-checked. */
static bool zip_read(ZipFile& z, const ZipEnt& x, uint64_t at, Bytes& out, std::string& err)
{
    Bytes in(x.csize);
    if (x.csize && !z.read_at(at, in.data(), x.csize)) { err = "cannot read " + x.name + " out of the zip"; return false; }
    if (x.method == 0) out.swap(in);
    else {
        out.assign(x.usize, 0);
        if (x.usize) {
            const int got = stbi_zlib_decode_noheader_buffer((char*)out.data(), (int)x.usize, (const char*)in.data(), (int)x.csize);
            if (got != (int)x.usize) { err = x.name + " is damaged in the zip (it does not unpack to its size)"; return false; }
        }
    }
    if (out.size() != x.usize || crc32_of(out.data(), out.size()) != x.crc) {
        err = x.name + " is damaged in the zip (its CRC does not match)"; return false;
    }
    return true;
}

/* ---- the check ------------------------------------------------------------------- */

struct ZipItem { std::string rel; size_t ent = 0; uint64_t at = 0; };
struct ZipPlan {
    std::string zip;                     /* the zip (UTF-8) */
    std::string err;                     /* "" = it can be installed */
    bool not_mod = false;                /* no mod.json anywhere, or no readable zip: the list counts, not shows, these */
    std::string folder;                  /* the mod's folder in the mods folder */
    std::string prefix;                  /* the mod's folder inside the zip ("X/", or "") */
    std::string name, version, author;   /* mod.json */
    std::vector<ZipItem> files;          /* what is extracted, below <folder> */
    size_t left_out = 0;                 /* files not extracted */
    std::string left_kinds;              /* ".psd, .gif, outside the mod's folder" */
    uint64_t total = 0;                  /* bytes extracted */
    bool exists = false;                 /* <mods>/<folder> is there already: only Replace */
};

/* A folder name from the zip's own name (a zip with mod.json at the top). */
static std::string folder_from_zip(const std::string& zip)
{
    std::string b = base_of(slashes(zip));
    if (ends_i(b, ".zip")) b.resize(b.size() - 4);
    for (char& c : b) {
        const unsigned char u = (unsigned char)c;
        if (u < 0x20 || u > 0x7E || strchr("<>:\"|?*/\\", c)) c = '_';
    }
    while (!b.empty() && (b.back() == '.' || b.back() == ' ')) b.pop_back();
    while (!b.empty() && (b[0] == '.' || b[0] == ' ')) b.erase(0, 1);
    return b.empty() || !part_name_ok(b, nullptr) ? std::string("mod") : b;
}

static bool zip_check(const std::string& zip_u8, ZipPlan& z)
{
    z = ZipPlan();
    z.zip = zip_u8;
    ZipFile f;
    if (!zip_open(zip_u8, f, z.err)) { z.not_mod = true; return false; }
    auto fail = [&](const std::string& e) { z.err = e; z.files.clear(); return false; };
    /* a zip with no mod.json anywhere is no mod at all, whatever else it holds (a
     * Downloads folder is full of program zips): the list counts it, it does not
     * show it -- a file chosen in the dialog still shows why */
    z.not_mod = true;
    for (const ZipEnt& x : f.ents) {
        const size_t s = x.name.find_last_of("/\\");
        if (!x.dir && ieq(s == std::string::npos ? x.name : x.name.substr(s + 1), "mod.json")) { z.not_mod = false; break; }
    }

    /* 1. names */
    struct N { size_t k; std::string rel; };
    std::vector<N> names;
    std::set<std::string> files_l, dirs_l;
    for (size_t k = 0; k < f.ents.size(); k++) {
        const ZipEnt& x = f.ents[k];
        std::string nm = x.name, rel, why;
        if (x.dir) nm.pop_back();
        if (!safe_rel(nm, rel, &why)) {
            std::string shown;
            for (char c : x.name) shown += ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) ? '?' : c;
            return fail("it has an entry named \"" + shown + "\": " + why);
        }
        const std::string l = lower(rel);
        if (x.dir) { dirs_l.insert(l); continue; }
        if (!files_l.insert(l).second) return fail("it has two files named " + rel);
        for (size_t q = l.find('/'); q != std::string::npos; q = l.find('/', q + 1)) dirs_l.insert(l.substr(0, q));
        names.push_back({ k, rel });
    }
    for (const std::string& l : files_l) if (dirs_l.count(l)) return fail("it has a file and a folder both named " + l);

    /* 2. programs, anywhere in it */
    for (const N& n : names) if (is_program(ext_of(n.rel))) return fail("contains a program, not a mod (" + n.rel + ")");

    /* 3. encryption, compression, sizes */
    uint64_t total = 0;
    for (const N& n : names) {
        const ZipEnt& x = f.ents[n.k];
        if ((x.flags & 0x41) || x.method == 99) return fail("it is password-protected (encrypted)");
        if (x.method != 0 && x.method != 8) return fail("it uses a compression method this reader does not know (" + n.rel + ")");
        if (x.usize > kZipMaxFile) return fail(n.rel + " is bigger than 512 MB");
        if ((x.method == 0 && x.csize != x.usize) || (x.method == 8 && (uint64_t)x.csize > (uint64_t)x.usize + x.usize / 64 + 65536))
            return fail("it is damaged (" + n.rel + " does not add up)");
        total += x.usize;
    }
    if (total > kZipMaxTotal) return fail("it unpacks to more than 2 GB");

    /* 4. one mod.json, at the top or one folder down */
    std::vector<size_t> mj;
    for (size_t i = 0; i < names.size(); i++)
        if (ieq(base_of(names[i].rel), "mod.json") && split_parts(names[i].rel).size() <= 2) mj.push_back(i);
    if (mj.empty()) return fail("it has no mod.json at the top or one folder down: not a mod in the Mod Manager's format");
    if (mj.size() > 1) {
        std::string s;
        for (size_t i : mj) s += (s.empty() ? "" : ", ") + names[i].rel;
        return fail("it holds more than one mod (" + s + "): unzip it into the mods folder yourself");
    }
    const std::string mjrel = names[mj[0]].rel;
    z.prefix = mjrel.size() > 8 ? mjrel.substr(0, mjrel.size() - 8) : std::string();
    z.folder = z.prefix.empty() ? folder_from_zip(zip_u8) : z.prefix.substr(0, z.prefix.size() - 1);
    if (z.folder[0] == '.' || ieq(z.folder, ".port_cache")) return fail("its folder " + z.folder + " starts with a dot (the mods folder skips those)");

    /* 5. mod.json: the scan's reading of it */
    {
        const ZipEnt& je = f.ents[names[mj[0]].k];
        if (je.usize > kMaxModJson) return fail("its mod.json is bigger than 1 MB");
        uint64_t at = 0;
        std::string e;
        Bytes jb;
        if (!zip_data_at(f, je, at, e)) return fail(e);
        if ((je.flags & 0x41) == 0 && !zip_read(f, je, at, jb, e)) return fail(e);
        J j;
        if (!parse_json(jb, j)) return fail("its mod.json is not valid JSON");
        const std::string game = game_key(j.str("game"));
        if (game.empty()) return fail("its mod.json does not name the game (\"game\": \"inf2\")");
        if (game != "inf2") return fail(std::string("it is made for ") + game_label(game) + ", not inFAMOUS 2");
        z.name = j.str("name", z.folder);
        if (z.name.empty()) z.name = z.folder;
        z.version = j.str("version");
        z.author = j.str("author");
    }

    /* 6. what is extracted: the mod's folder, mod files only */
    std::set<std::string> kinds;
    for (const N& n : names) {
        if (!z.prefix.empty() && (n.rel.size() <= z.prefix.size() || !lang_ieq_n(n.rel.c_str(), z.prefix.c_str(), z.prefix.size()))) {
            z.left_out++; kinds.insert("files outside the mod's folder"); continue;
        }
        const std::string rel = n.rel.substr(z.prefix.size());
        const std::string e = ext_of(rel);
        if (!is_mod_ext(e)) { z.left_out++; kinds.insert(e.empty() ? std::string("files with no extension") : "." + e); continue; }
        ZipItem it;
        it.rel = rel; it.ent = n.k;
        std::string err;
        if (!zip_data_at(f, f.ents[n.k], it.at, err)) return fail(err);
        z.total += f.ents[n.k].usize;
        z.files.push_back(it);
    }
    for (const std::string& k : kinds) z.left_kinds += (z.left_kinds.empty() ? "" : ", ") + k;

    /* 7. the paths fit (the file layer's paths are narrow, below MAX_PATH) */
    if (!g.dir.empty()) {
        const size_t room = g.dir.size() + 1 + (z.folder.size() > 40 ? z.folder.size() : 40) + 1;
        for (const ZipItem& it : z.files)
            if (room + it.rel.size() > 250) return fail("a path would be too long in the mods folder (" + it.rel + ")");
        z.exists = finfo(join(g.dir, z.folder)).ok;
    }
    return true;
}

/* ---- the install ------------------------------------------------------------------- */

/* Deletes p, which must be a ".installing-" or ".replaced-" folder directly in
 * the mods folder, and everything in it -- a junction or link inside is
 * removed itself, never what it points at. */
static bool remove_ours(const std::string& p)
{
    const std::string b = base_of(p);
    if (g.dir.empty() || !ieq(dir_of(p), g.dir) || (b.compare(0, 12, ".installing-") && b.compare(0, 10, ".replaced-"))) return false;
    struct R {
        static bool rm(const std::string& q, int depth)
        {
            if (depth > 40) return false;
#ifdef _WIN32
            const DWORD a = GetFileAttributesA(q.c_str());
            if (a == INVALID_FILE_ATTRIBUTES) return true;
            if (a & FILE_ATTRIBUTE_REPARSE_POINT)
                return (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryA(q.c_str()) != 0 : DeleteFileA(q.c_str()) != 0;
            if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
                if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(q.c_str(), a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
                return DeleteFileA(q.c_str()) != 0;
            }
            for (const std::string& n : list_dir(q)) rm(join(q, n), depth + 1);
            return RemoveDirectoryA(q.c_str()) != 0;
#else
            struct stat st;
            if (lstat(q.c_str(), &st) != 0) return true;
            if (!S_ISDIR(st.st_mode)) return unlink(q.c_str()) == 0;
            for (const std::string& n : list_dir(q)) rm(join(q, n), depth + 1);
            return rmdir(q.c_str()) == 0;
#endif
        }
    };
    return R::rm(p, 0);
}

static std::atomic<int> s_zip_seq{0};
static std::string our_name(const char* kind)
{
#ifdef _WIN32
    const unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    return std::string(kind) + std::to_string(pid) + "-" + std::to_string(++s_zip_seq);
}

/* Extracts z (zip_check'ed) into the mods folder. done: bytes written so far. */
static bool zip_install(const ZipPlan& z, bool replace, std::string& err, std::atomic<uint64_t>* done = nullptr)
{
    if (g.dir.empty()) { err = "there is no mods folder"; return false; }
    if (!z.err.empty() || z.folder.empty()) { err = z.err.empty() ? "not checked" : z.err; return false; }
    mkdirs(g.dir);
    for (const std::string& n : list_dir(g.dir))   /* what an interrupted install left */
        if (!n.compare(0, 12, ".installing-")) remove_ours(join(g.dir, n));
    ZipFile f;
    if (!zip_open(z.zip, f, err)) return false;
    const std::string tmp = join(g.dir, our_name(".installing-"));
    mkdirs(tmp);
    if (!is_dir(tmp)) { err = "cannot make a folder in the mods folder"; return false; }
    for (const ZipItem& it : z.files) {
        if (it.ent >= f.ents.size()) { remove_ours(tmp); err = "the zip changed since it was checked"; return false; }
        const ZipEnt& x = f.ents[it.ent];
        Bytes d;
        uint64_t at = 0;
        if (!zip_data_at(f, x, at, err) || at != it.at || !zip_read(f, x, at, d, err)) { remove_ours(tmp); return false; }
        const std::string p = join(tmp, it.rel);
        mkdirs(dir_of(p));
        FILE* o = fopen(p.c_str(), "wb");
        const bool ok = o && fwrite(d.data(), 1, d.size(), o) == d.size();
        if (o && fclose(o) != 0) { remove_ours(tmp); err = "cannot write " + it.rel + " (is the disk full?)"; return false; }
        if (!ok) { remove_ours(tmp); err = "cannot write " + it.rel; return false; }
        if (done) done->fetch_add(d.size());
    }
    const std::string dst = join(g.dir, z.folder);
    if (finfo(dst).ok || is_link(dst)) {
        if (!replace) { remove_ours(tmp); err = "mods/" + z.folder + " is there already (use Replace)"; return false; }
        const std::string old = join(g.dir, our_name(".replaced-"));
        if (rename(dst.c_str(), old.c_str()) != 0) {
            remove_ours(tmp);
            err = "the old " + z.folder + " could not be moved aside (a file of it is open: restart the game, then replace it)";
            return false;
        }
        if (rename(tmp.c_str(), dst.c_str()) != 0) {
            rename(old.c_str(), dst.c_str());
            remove_ours(tmp);
            err = "the new " + z.folder + " could not be put in place; the old one is kept";
            return false;
        }
        remove_ours(old);
        return true;
    }
    if (rename(tmp.c_str(), dst.c_str()) != 0) { remove_ours(tmp); err = "could not make mods/" + z.folder; return false; }
    return true;
}

#endif /* PPU_FS_MODS_ZIP_H */
