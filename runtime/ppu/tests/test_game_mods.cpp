/* The Mods page's logic (runtime/ppu/ppu_fs_mods.h + ppu_fs_mods_zip.h,
 * libs/video/mod_preview.h), without the game:
 *  - the zip reader and every refusal, on zips written here: zip-slip,
 *    absolute paths, drive letters, streams, device names, backslash tricks,
 *    a program inside, ZIP64, multi-disk, encryption, unknown compression,
 *    oversized files and totals, too many entries, no / two-deep / two
 *    mod.json, not for inFAMOUS 2, broken JSON, duplicate names, damaged
 *    data (CRC), a local header that disagrees; install, Replace, leftovers;
 *  - the clash plan with priorities, "Use this one instead" and its text,
 *    restart-needed, the build-failure memory;
 *  - mods.ini, old and new format, read and written back;
 *  - the preview picture's path rules and decoding.
 * With a downloads folder (argv[1]) the real community zips are checked and
 * installed too, and with the game (argv[2] = the port's game folder) the
 * texture version check of Sasha's Black Tar runs on the real archives
 * (read only).
 *
 * Windows (clang-cl, as the build): scratchpad build_test_mods.ps1, or
 *   clang-cl /EHsc /std:c++20 /O1 /FIruntime/platform/win32_posix_min.h -Iinclude -Iruntime/ppu
 *     runtime/ppu/tests/test_game_mods.cpp shell32.lib /Fe:test_game_mods.exe
 *   test_game_mods.exe <work folder> [<downloads> [<game folder>]]
 */
#include "../../platform/win32_dirent.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#include "../../../libs/codec/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../../libs/codec/stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

extern "C" const char* ppu_vfs_root = ".";
static int g_restarts;
extern "C" void ps3_host_restart(const char*) { g_restarts++; }

#include "../ppu_fs_lang.h"
#include "../ppu_fs_mods.h"
#include "../../../libs/video/mod_preview.h"

using namespace gmods;

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
#define CHECK_HAS(s, sub) do { const std::string s_ = (s); if (has(s_, sub)) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: \"%s\" lacks \"%s\"\n", __FILE__, __LINE__, s_.c_str(), sub); } } while (0)

static std::string W;   /* the work folder ('/' separated) */

static void put_file(const std::string& p, const std::string& data)
{
    mkdirs(dir_of(p));
    FILE* f = fopen(p.c_str(), "wb");
    if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); }
}
static std::string get_file(const std::string& p)
{
    Bytes b;
    if (!read_all(p, b)) return std::string("<missing>");
    return std::string(b.begin(), b.end());
}
static void wipe(const std::string& p)   /* the test's own folders only */
{
    if (rel_to(W, p).empty()) return;
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryA(p.c_str()); DeleteFileA(p.c_str()); return; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { DeleteFileA(p.c_str()); return; }
    for (const std::string& n : list_dir(p)) wipe(join(p, n));
    RemoveDirectoryA(p.c_str());
#endif
}

/* ---- a zip writer for the crafted cases --------------------------------------- */
struct ZE {
    std::string name, data;
    int method = 8;          /* 0 stored, 8 deflate, else as given (data stored) */
    uint16_t flags = 0;
    bool descriptor = false; /* sizes in a data descriptor (bit 3), as Windows zips */
    bool zip64extra = false;
    uint32_t usize_override = 0, crc_xor = 0;
    std::string local_name;  /* "" = name */
};
struct ZOpts { bool multidisk = false, zip64eocd = false, zip64loc = false; uint16_t count_override = 0; };
static void p16(std::string& s, uint32_t v) { s += (char)(v & 0xFF); s += (char)((v >> 8) & 0xFF); }
static void p32(std::string& s, uint32_t v) { p16(s, v & 0xFFFF); p16(s, v >> 16); }
static std::string deflate_raw(const std::string& d)
{
    int n = 0;
    unsigned char* z = stbi_zlib_compress((unsigned char*)d.data(), (int)d.size(), &n, 8);
    std::string r = n > 6 ? std::string((const char*)z + 2, (size_t)n - 6) : std::string();
    free(z);
    return r;
}
static void write_zip(const std::string& path, const std::vector<ZE>& es, const ZOpts& o = ZOpts())
{
    std::string out, cd;
    for (const ZE& e : es) {
        const uint32_t crc = crc32_of((const uint8_t*)e.data.data(), e.data.size()) ^ e.crc_xor;
        const std::string comp = e.method == 8 ? deflate_raw(e.data) : e.data;
        const uint32_t usize = e.usize_override ? e.usize_override : (uint32_t)e.data.size();
        const uint16_t flags = (uint16_t)(e.flags | (e.descriptor ? 8 : 0));
        const uint32_t lho = (uint32_t)out.size();
        const std::string ln = e.local_name.empty() ? e.name : e.local_name;
        p32(out, 0x04034b50); p16(out, 20); p16(out, flags); p16(out, (uint32_t)e.method); p16(out, 0); p16(out, 0);
        p32(out, e.descriptor ? 0 : crc); p32(out, e.descriptor ? 0 : (uint32_t)comp.size()); p32(out, e.descriptor ? 0 : usize);
        p16(out, (uint32_t)ln.size()); p16(out, 0);
        out += ln;
        out += comp;
        if (e.descriptor) { p32(out, 0x08074b50); p32(out, crc); p32(out, (uint32_t)comp.size()); p32(out, usize); }
        std::string extra;
        if (e.zip64extra) { p16(extra, 0x0001); p16(extra, 8); p32(extra, usize); p32(extra, 0); }
        p32(cd, 0x02014b50); p16(cd, 20); p16(cd, 20); p16(cd, flags); p16(cd, (uint32_t)e.method); p16(cd, 0); p16(cd, 0);
        p32(cd, crc); p32(cd, (uint32_t)comp.size()); p32(cd, usize);
        p16(cd, (uint32_t)e.name.size()); p16(cd, (uint32_t)extra.size()); p16(cd, 0); p16(cd, 0); p16(cd, 0); p32(cd, 0); p32(cd, lho);
        cd += e.name;
        cd += extra;
    }
    const uint32_t cd_off = (uint32_t)out.size();
    out += cd;
    if (o.zip64loc) { p32(out, 0x07064b50); p32(out, 0); p32(out, 0); p32(out, 0); p32(out, 1); }
    p32(out, 0x06054b50); p16(out, o.multidisk ? 1 : 0); p16(out, o.multidisk ? 1 : 0);
    const uint16_t n = o.count_override ? o.count_override : (uint16_t)es.size();
    p16(out, o.zip64eocd ? 0xFFFF : n); p16(out, o.zip64eocd ? 0xFFFF : n);
    p32(out, (uint32_t)cd.size()); p32(out, cd_off); p16(out, 0);
    put_file(path, out);
}
static const char* kMJ = "{\"game\": \"inf2\", \"name\": \"Red Jacket\", \"author\": \"t\", \"version\": \"1.0\", \"image\": \"pic.png\"}";

static std::string check_err(const std::vector<ZE>& es, const ZOpts& o = ZOpts(), const char* name = "c.zip")
{
    const std::string p = join(W, std::string("zips/") + name);
    write_zip(p, es, o);
    ZipPlan z;
    zip_check(p, z);
    return z.err;
}

/* a valid tiny DDS: DXT1 4x4, 1 mip (128-byte header + 8 bytes) */
static std::string tiny_dds()
{
    std::string h(128, '\0');
    memcpy(&h[0], "DDS ", 4);
    h[12] = 4; h[16] = 4; h[28] = 1;
    memcpy(&h[84], "DXT1", 4);
    return h + std::string(8, 'x');
}

static void test_zips()
{
    const std::string mods = join(W, "mods");
    wipe(mods); mkdirs(mods);
    g.dir = mods; g.state_path = join(W, "state.ini"); g.cache = join(mods, ".port_cache");
    remove(g.state_path.c_str());

    /* a good one: mod files extracted, the rest left out */
    {
        std::vector<ZE> es;
        ZE d; d.name = "Red Jacket/"; d.method = 0; es.push_back(d);
        ZE a; a.name = "Red Jacket/mod.json"; a.data = kMJ; es.push_back(a);
        ZE b; b.name = "Red Jacket/texture_patches/cole_default.xpps/data00000000_x_4x4_DXT1_m1.dds"; b.data = tiny_dds(); b.method = 0; es.push_back(b);
        ZE c; c.name = "Red Jacket\\readme.txt"; c.data = std::string(5000, 'r'); c.descriptor = true; es.push_back(c);
        ZE e; e.name = "Red Jacket/source.psd"; e.data = "psd"; es.push_back(e);
        ZE f; f.name = "other.txt"; f.data = "o"; es.push_back(f);
        ZE pic; pic.name = "Red Jacket/pic.png"; pic.data = "not really a png"; es.push_back(pic);
        const std::string p = join(W, "zips/Red Jacket v1.zip");
        write_zip(p, es);
        ZipPlan z;
        CHECK(zip_check(p, z));
        CHECK(z.err.empty());
        CHECK(z.folder == "Red Jacket");
        CHECK(z.name == "Red Jacket" && z.version == "1.0" && z.author == "t");
        CHECK(z.files.size() == 4);   /* mod.json, the dds, readme.txt (backslashes), pic.png */
        CHECK(z.left_out == 2);
        CHECK_HAS(z.left_kinds, ".psd");
        CHECK_HAS(z.left_kinds, "outside the mod's folder");
        CHECK(!z.exists);
        std::string err;
        CHECK(zip_install(z, false, err));
        CHECK(get_file(join(mods, "Red Jacket/readme.txt")) == std::string(5000, 'r'));
        CHECK(get_file(join(mods, "Red Jacket/mod.json")) == kMJ);
        CHECK(!finfo(join(mods, "Red Jacket/source.psd")).ok);
        CHECK(!finfo(join(mods, "other.txt")).ok);
        /* again: it is there now -- only Replace */
        ZipPlan z2;
        CHECK(zip_check(p, z2) && z2.exists);
        CHECK(!zip_install(z2, false, err));
        CHECK_HAS(err, "there already");
        /* Replace with v2 */
        es[3].data = "v2 readme";
        const std::string p2 = join(W, "zips/Red Jacket v2.zip");
        write_zip(p2, es);
        ZipPlan z3;
        CHECK(zip_check(p2, z3) && z3.exists);
        CHECK(zip_install(z3, true, err));
        CHECK(get_file(join(mods, "Red Jacket/readme.txt")) == "v2 readme");
        bool left = false;
        for (const std::string& n : list_dir(mods)) if (n[0] == '.') left = true;
        CHECK(!left);   /* no .installing- / .replaced- folder stays */
    }
    /* mod.json at the top: the folder is the zip's name */
    {
        ZE a; a.name = "mod.json"; a.data = kMJ;
        ZE b; b.name = "files/zeke.xpps"; b.data = "PACK";
        const std::string p = join(W, "zips/Top Level.zip");
        write_zip(p, { a, b });
        ZipPlan z;
        CHECK(zip_check(p, z));
        CHECK(z.folder == "Top Level");
        CHECK(z.files.size() == 2);
    }
    ZE mj; mj.name = "X/mod.json"; mj.data = kMJ;
    auto one = [&](const char* name) { ZE e; e.name = name; e.data = "d"; return std::vector<ZE>{ mj, e }; };
    /* names */
    CHECK_HAS(check_err(one("X/../../evil.txt")), "\"..\"");
    CHECK_HAS(check_err(one("../evil.txt")), "\"..\"");
    CHECK_HAS(check_err(one("X/./a.txt")), "\"..\" or \".\"");
    CHECK_HAS(check_err(one("/etc/passwd.txt")), "absolute");
    CHECK_HAS(check_err(one("\\\\server\\share\\a.txt")), "absolute");
    CHECK_HAS(check_err(one("C:/Windows/a.txt")), "drive letter");
    CHECK_HAS(check_err(one("C:a.txt")), "drive letter");
    CHECK_HAS(check_err(one("X\\..\\..\\evil.txt")), "\"..\"");
    CHECK_HAS(check_err(one("X/mod.json:evil")), "':'");
    CHECK_HAS(check_err(one("X/CON.txt")), "device");
    CHECK_HAS(check_err(one("X/nul")), "device");
    CHECK_HAS(check_err(one("X/com1.json")), "device");
    CHECK_HAS(check_err(one("X/a.txt.")), "dot or a space");
    CHECK_HAS(check_err(one("X/a.txt ")), "dot or a space");
    CHECK_HAS(check_err(one("X//a.txt")), "empty");
    CHECK_HAS(check_err(one("X/a?.txt")), "does not allow");
    CHECK_HAS(check_err(one("X/caf\xC3\xA9.txt")), "ASCII");
    {
        std::vector<ZE> v = one("X/a.dds");
        ZE d; d.name = "X/A.DDS"; d.data = "e"; v.push_back(d);
        CHECK_HAS(check_err(v), "two files named");
    }
    {
        std::vector<ZE> v = one("X/a");
        ZE d; d.name = "X/a/b.txt"; d.data = "e"; v.push_back(d);
        CHECK_HAS(check_err(v), "a file and a folder");
    }
    /* programs */
    CHECK_HAS(check_err(one("X/tool.exe")), "contains a program, not a mod");
    CHECK_HAS(check_err(one("X/sub/run.BAT")), "contains a program, not a mod");
    CHECK_HAS(check_err(one("elsewhere/x.dll")), "contains a program, not a mod");
    CHECK_HAS(check_err(one("X/a.ps1")), "contains a program");
    CHECK_HAS(check_err(one("X/a.lnk")), "contains a program");
    CHECK_HAS(check_err(one("X/a.py")), "contains a program");
    {   /* the list shows a program zip only when it has a mod.json (else: "not a mod", counted) */
        ZE ex; ex.name = "Tool/tool.exe"; ex.data = "MZ";
        const std::string p = join(W, "zips/tool.zip");
        write_zip(p, { ex });
        ZipPlan z;
        CHECK(!zip_check(p, z) && z.not_mod);
        CHECK_HAS(z.err, "contains a program, not a mod");
        write_zip(p, one("X/tool.exe"));
        CHECK(!zip_check(p, z) && !z.not_mod);
    }
    /* the container */
    {
        ZOpts o; o.zip64eocd = true;
        std::string e = check_err(one("X/a.txt"), o);
        CHECK_HAS(e, "ZIP64");
        ZOpts o2; o2.zip64loc = true;
        CHECK_HAS(check_err(one("X/a.txt"), o2), "ZIP64");
        std::vector<ZE> v = one("X/a.txt"); v[1].zip64extra = true;
        CHECK_HAS(check_err(v), "ZIP64");
        ZOpts o3; o3.multidisk = true;
        CHECK_HAS(check_err(one("X/a.txt"), o3), "several parts");
    }
    {
        std::vector<ZE> v = one("X/a.txt"); v[1].flags = 1;
        CHECK_HAS(check_err(v), "password");
        v = one("X/a.txt"); v[1].method = 12;
        CHECK_HAS(check_err(v), "compression method");
        v = one("X/a.dds"); v[1].usize_override = 600u << 20;
        CHECK_HAS(check_err(v), "bigger than 512 MB");
        v = { mj };
        for (int k = 0; k < 5; k++) { ZE e; e.name = "X/big" + std::to_string(k) + ".bin"; e.data = "z"; e.usize_override = 450u << 20; v.push_back(e); }
        CHECK_HAS(check_err(v), "more than 2 GB");
        v = { mj };
        for (int k = 0; k < 20001; k++) { ZE e; e.name = "X/" + std::to_string(k) + ".txt"; e.method = 0; v.push_back(e); }
        CHECK_HAS(check_err(v), "more than 20,000");
    }
    /* mod.json */
    {
        ZE e; e.name = "X/a.txt"; e.data = "d";
        const std::string p = join(W, "zips/nomod.zip");
        write_zip(p, { e });
        ZipPlan z;
        CHECK(!zip_check(p, z) && z.not_mod);
        CHECK_HAS(z.err, "no mod.json");
        ZE deep; deep.name = "a/b/mod.json"; deep.data = kMJ;
        write_zip(p, { deep });
        CHECK(!zip_check(p, z) && !z.not_mod);
        CHECK_HAS(z.err, "no mod.json at the top or one folder down");
        ZE m2; m2.name = "Y/mod.json"; m2.data = kMJ;
        CHECK_HAS(check_err({ mj, m2 }), "more than one mod");
        ZE i1; i1.name = "X/mod.json"; i1.data = "{\"game\": \"inf1\"}";
        CHECK_HAS(check_err({ i1 }), "made for inFAMOUS, not inFAMOUS 2");
        ZE ng; ng.name = "X/mod.json"; ng.data = "{\"name\": \"x\"}";
        CHECK_HAS(check_err({ ng }), "does not name the game");
        ZE bj; bj.name = "X/mod.json"; bj.data = "{\"game\": \"inf2\",";
        CHECK_HAS(check_err({ bj }), "not valid JSON");
        ZE big; big.name = "X/mod.json"; big.data = std::string(1100000, ' ') + "{}";
        CHECK_HAS(check_err({ big }), "bigger than 1 MB");
        ZE dot; dot.name = ".hidden/mod.json"; dot.data = kMJ;
        CHECK_HAS(check_err({ dot }), "starts with a dot");
    }
    /* damaged */
    {
        std::vector<ZE> v = one("X/a.txt"); v[1].local_name = "X/b.txt";
        CHECK_HAS(check_err(v), "does not match");
        v = one("X/a.txt"); v[1].data = std::string(3000, 'q'); v[1].crc_xor = 1;
        const std::string p = join(W, "zips/crc.zip");
        write_zip(p, v);
        ZipPlan z;
        CHECK(zip_check(p, z));   /* the CRC is only known by unpacking */
        std::string err;
        CHECK(!zip_install(z, false, err));
        CHECK_HAS(err, "CRC");
        CHECK(!finfo(join(mods, "X")).ok);
        bool left = false;
        for (const std::string& n : list_dir(mods)) if (n[0] == '.') left = true;
        CHECK(!left);
    }
    /* remove_ours takes nothing but its own folders */
    CHECK(!remove_ours(join(mods, "Red Jacket")));
    CHECK(!remove_ours(join(W, ".installing-1")));
    CHECK(finfo(join(mods, "Red Jacket/mod.json")).ok);
}

/* ---- the clash plan ---------------------------------------------------------- */
static Mod mk(const char* folder, std::vector<std::pair<std::string, uint32_t>> tex, const char* whole = nullptr)
{
    Mod m;
    m.folder = folder; m.name = folder; m.path = join(W, std::string("plan/") + folder); m.for_game = true;
    for (auto& t : tex) { Tex x; x.target = t.first; x.off = t.second; x.path = m.path + "/t" + std::to_string(t.second) + ".dds"; m.tex.push_back(x); m.targets.insert(t.first); }
    if (whole) { Part p; p.target = whole; p.path = m.path + "/" + whole; m.files.push_back(p); m.targets.insert(whole); }
    return m;
}
static int idx(const char* folder) { for (size_t i = 0; i < g.list.size(); i++) if (g.list[i].folder == folder) return (int)i; return -1; }
static std::string status_of(const char* folder) { ps3_game_mod m; ps3_game_mods_get(idx(folder), &m); return m.status; }

static void test_plan()
{
    g.dir = join(W, "plan"); g.state_path = join(W, "plan_state.ini");
    remove(g.state_path.c_str());
    g.start.clear(); g.start_sig.clear(); g.use = true;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list = { mk("A", { { "cole_default.xpps", 0x100 } }),
                   mk("B", { { "cole_default.xpps", 0x100 }, { "cole_default.xpps", 0x200 } }),
                   mk("C", { { "cole_default.xpps", 0x200 } }),
                   mk("D", {}, "zeke.xpps"),
                   mk("E", { { "zeke.xpps", 0x10 } }) };   /* E patches what D replaces: clash */
        g.gen++;
    }
    CHECK(mods_clash(g.list[0], g.list[1]) && mods_clash(g.list[1], g.list[0]));
    CHECK(!mods_clash(g.list[0], g.list[2]));
    CHECK(mods_clash(g.list[3], g.list[4]) && mods_clash(g.list[4], g.list[3]));
    {   /* folder order: A, C, D used; B lost to A; E lost to D */
        std::lock_guard<std::mutex> lk(g.mx);
        plan_locked();
        CHECK(g.plan.next[0] && !g.plan.next[1] && g.plan.next[2] && g.plan.next[3] && !g.plan.next[4]);
        CHECK_HAS(g.plan.why[1], "same files as A");
        CHECK(g.plan.prefer[1] == 1);
        CHECK(g.plan.prefer_text[1] == "A and C will be left out");
        CHECK(g.plan.prefer_text[4] == "D will be left out");
    }
    CHECK_HAS(status_of("B"), "Left out from the next start: it changes the same files as A");
    CHECK_HAS(status_of("A"), "Used from the next start");
    CHECK(ps3_game_mods_restart_needed() == 1);   /* nothing used now, A C D next */
    ps3_game_mod b;
    ps3_game_mods_get(1, &b);
    CHECK(b.clash == 1 && !strcmp(b.prefer, "A and C will be left out"));
    /* "Use this one instead" on B */
    CHECK(ps3_game_mods_prefer(1) == 1);
    CHECK(g.list[1].have_prio && g.list[1].prio == 1);
    {
        std::lock_guard<std::mutex> lk(g.mx);
        plan_locked();
        CHECK(!g.plan.next[0] && g.plan.next[1] && !g.plan.next[2] && g.plan.next[3]);
        CHECK(g.plan.prefer[0] == 2);                                   /* above B's 1 */
        CHECK(g.plan.prefer_text[0] == "B will be left out; C will be used");
    }
    CHECK_HAS(get_file(g.state_path), "[B]\npriority = 1\n");
    {   /* C now loses to B: its button would put it above B (2), and A comes back with it */
        std::lock_guard<std::mutex> lk(g.mx);
        plan_locked();
        CHECK(g.plan.prefer[2] == 2);
        CHECK(g.plan.prefer_text[2] == "B will be left out; A will be used");
    }
    CHECK(ps3_game_mods_prefer(3) == 0);   /* D is used: no button, nothing changes */
    /* the order is the saved priorities, then folder order -- deterministic */
    {
        std::vector<Prio> p = { { false, 0 }, { true, 1 }, { false, 0 }, { true, 5 }, { true, 1 } };
        std::vector<size_t> o = mod_order(p);
        CHECK((o == std::vector<size_t>{ 3, 1, 4, 0, 2 }));
    }
    /* restart needed: compare with what "this session" used */
    {
        std::lock_guard<std::mutex> lk(g.mx);
        plan_locked();
        g.start_sig = g.plan.sig;
    }
    CHECK(ps3_game_mods_restart_needed() == 0);
    ps3_game_mods_set_on(3, 0);
    CHECK(ps3_game_mods_restart_needed() == 1);
    ps3_game_mods_set_on(3, 1);
    CHECK(ps3_game_mods_restart_needed() == 0);
#ifdef _WIN32
    _putenv_s("PS3_GAME_MODS", "0");   /* "Use mods" off in the menu */
    CHECK(ps3_game_mods_restart_needed() == 1);
    CHECK_HAS(status_of("A"), "Not used: mods are off");
    _putenv_s("PS3_GAME_MODS", "");
#endif
    /* a build that failed at start stays out while nothing changed */
    {
        std::lock_guard<std::mutex> lk(g.mx);
        StartInfo si; si.fail = true; si.alone = true; si.key = mod_key(g.list[3]); si.why = "could not build zeke.xpps: x";
        g.start["d"] = si;
        g.gen++;
        plan_locked();
        CHECK(!g.plan.next[3]);
        CHECK(g.plan.why[3] == si.why);
        CHECK(g.plan.next[4]);   /* E no longer loses to D */
    }
    CHECK_HAS(status_of("D"), "Not used: could not build zeke.xpps");
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list[3].stamp ^= 1;    /* its files changed: tried again */
        g.gen++;
        plan_locked();
        CHECK(g.plan.next[3]);
    }
}

/* ---- mods.ini ------------------------------------------------------------------- */
static void test_state()
{
    g.state_path = join(W, "state_rt.ini");
    put_file(g.state_path, "# old\n[Sasha's Black Tar]\npick = default,jbintro\n\n[Young Kessler]\non = 0\n[Gone]\non = 0\n");
    std::vector<std::string> zd;
    std::map<std::string, Saved> st = state_read(&zd);
    CHECK(st.size() == 3 && zd.empty());
    CHECK(st["sasha's black tar"].have_pick && st["sasha's black tar"].pick == (std::vector<std::string>{ "default", "jbintro" }));
    CHECK(st["sasha's black tar"].on && !st["sasha's black tar"].have_prio);
    CHECK(st["young kessler"].have_on && !st["young kessler"].on);
    {
        std::lock_guard<std::mutex> lk(g.mx);
        Mod s; s.folder = "Sasha's Black Tar"; s.have_pick = true; s.pick = st["sasha's black tar"].pick;
        Mod y; y.folder = "Young Kessler"; y.on = false;
        Mod d; d.folder = "Doom Kessler";
        g.list = { d, s, y };
        g.zip_dirs.clear();
        state_write_locked();
    }
    const std::string old_out = get_file(g.state_path);
    CHECK_HAS(old_out, "[Sasha's Black Tar]\npick = default,jbintro\n[Young Kessler]\non = 0\n");
    CHECK(!has(old_out, "priority") && !has(old_out, "[Doom Kessler]") && !has(old_out, ".zips"));
    /* new keys: priority and the zip folders, round trip */
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list[0].have_prio = true; g.list[0].prio = 3;
        g.zip_dirs = { "S:/Downloads", "C:/Users/x/Mods zips" };
        state_write_locked();
    }
    const std::string new_out = get_file(g.state_path);
    CHECK_HAS(new_out, "[Doom Kessler]\npriority = 3\n");
    CHECK_HAS(new_out, "[.zips]\nfolder = S:/Downloads\nfolder = C:/Users/x/Mods zips\n");
    zd.clear();
    st = state_read(&zd);
    CHECK(st["doom kessler"].have_prio && st["doom kessler"].prio == 3 && st["doom kessler"].on);
    CHECK(!st.count(".zips"));
    CHECK((zd == std::vector<std::string>{ "S:/Downloads", "C:/Users/x/Mods zips" }));
    CHECK(st["young kessler"].have_on && !st["young kessler"].on);
    /* bad priorities are ignored */
    put_file(g.state_path, "[A]\npriority = 0\n[B]\npriority = -4\n[C]\npriority = x\n[D]\npriority = 7\n");
    st = state_read();
    CHECK(!st["a"].have_prio && !st["b"].have_prio && !st["c"].have_prio && st["d"].have_prio && st["d"].prio == 7);
}

/* ---- preview pictures --------------------------------------------------------- */
static void test_images(const std::string& sample_png, const std::string& sample_jpg)
{
    const std::string mp = join(W, "imgmod");
    wipe(mp);
    mkdirs(join(mp, "sub")); mkdirs(join(mp, "dirpic.png"));
    put_file(join(mp, "pic.png"), get_file(sample_png));
    put_file(join(mp, "sub/pic.jpg"), get_file(sample_jpg));
    put_file(join(W, "outside.png"), get_file(sample_png));
    put_file(join(mp, "empty.png"), "");
    put_file(join(mp, "big.png"), std::string(9u << 20, 'b'));
    put_file(join(mp, "fake.png"), "GIF89a not a png at all....");
#ifdef _WIN32
    {   /* a junction inside the mod, to a folder outside it */
        mkdirs(join(W, "elsewhere"));
        put_file(join(W, "elsewhere/pic.png"), get_file(sample_png));
        std::string a = join(mp, "link"), b = join(W, "elsewhere");
        for (char& c : a) if (c == '/') c = '\\';
        for (char& c : b) if (c == '/') c = '\\';
        const std::string cmd = "cmd /c mklink /J \"" + a + "\" \"" + b + "\" >nul";
        system(cmd.c_str());
    }
#endif
    Mod m;
    m.path = mp;
    auto ok = [&](const char* img) { m.image = img; std::string out, why; return image_path(m, out, &why); };
    auto why_of = [&](const char* img) { m.image = img; std::string out, why; image_path(m, out, &why); return why; };
    CHECK(ok("pic.png"));
    CHECK(ok("sub/pic.jpg"));
    CHECK(ok("sub\\pic.jpg"));
    CHECK(ok("PIC.PNG"));
    CHECK_HAS(why_of("../outside.png"), "\"..\"");
    CHECK_HAS(why_of("sub/../../outside.png"), "\"..\"");
    CHECK_HAS(why_of("/outside.png"), "absolute");
    CHECK_HAS(why_of("\\\\server\\share\\x.png"), "absolute");
    CHECK_HAS(why_of("//server/share/x.png"), "absolute");
    CHECK_HAS(why_of("C:/x.png"), "drive");
    CHECK_HAS(why_of("C:x.png"), "drive");
    CHECK_HAS(why_of("pic.png:stream"), "':'");
    CHECK_HAS(why_of("CON.png"), "device");
    CHECK_HAS(why_of("sub/nul.jpg"), "device");
    CHECK_HAS(why_of("pic.png "), "dot or a space");
    CHECK_HAS(why_of("pic.gif"), "not a .png or .jpg");
    CHECK_HAS(why_of("pic.exe"), "not a .png or .jpg");
    CHECK_HAS(why_of("big.png"), "8 MB");
    CHECK_HAS(why_of("empty.png"), "8 MB");
    CHECK_HAS(why_of("missing.png"), "no such file");
    CHECK_HAS(why_of("dirpic.png"), "no such file");
    CHECK_HAS(why_of(""), "names no picture");
    {
        m.image = std::string("pic.png\0x", 9);
        std::string out, why;
        CHECK(!image_path(m, out, &why));
    }
#ifdef _WIN32
    CHECK(is_link(join(mp, "link")));
    CHECK_HAS(why_of("link/pic.png"), "junction or link");
#endif
    /* decoding: shrunk to 512 at most, aspect kept; refusals */
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    std::string why;
    CHECK(mod_preview_load(sample_png.c_str(), 512, px, w, h, &why) == 1);
    CHECK(w > 0 && h > 0 && w <= 512 && h <= 512 && px.size() == (size_t)w * h * 4);
    CHECK(mod_preview_load(sample_jpg.c_str(), 512, px, w, h, &why) == 1);
    CHECK(w <= 512 && h <= 512);
    CHECK(mod_preview_load(sample_png.c_str(), 64, px, w, h, &why) == 1 && (w == 64 || h == 64));
    CHECK(mod_preview_load(join(mp, "fake.png").c_str(), 512, px, w, h, &why) == 0);
    CHECK_HAS(why, "not a PNG or JPEG");
    CHECK(mod_preview_load(join(mp, "big.png").c_str(), 512, px, w, h, &why) == 0);
    CHECK_HAS(why, "8 MB");
    {   /* a PNG whose header says 10000 x 10000 (100 M pixels): refused before decoding */
        std::string png("\x89PNG\r\n\x1a\n", 8);
        std::string ihdr;
        p32(ihdr, 0); ihdr = std::string("\0\0\0\x0d", 4) + "IHDR";
        std::string body;
        auto be = [](std::string& s, uint32_t v) { s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v; };
        be(body, 10000); be(body, 10000); body += std::string("\x08\x06\x00\x00\x00", 5);
        const uint32_t crc = crc32_of((const uint8_t*)("IHDR" + body).data(), 4 + body.size());
        std::string c; be(c, crc);
        std::string idat = std::string("\0\0\0\0", 4) + "IDAT", ic;
        be(ic, crc32_of((const uint8_t*)"IDAT", 4));
        png += ihdr + body + c + idat + ic;
        put_file(join(mp, "huge.png"), png);
        CHECK(mod_preview_load(join(mp, "huge.png").c_str(), 512, px, w, h, &why) == 0);
        CHECK_HAS(why, "too big");
    }
}

/* ---- the real community zips (read only), and the version check on the game's files ---- */
static void test_real(const std::string& dl, const std::string& game)
{
    const std::string mods = join(W, "realmods");
    wipe(mods); mkdirs(mods);
    g.dir = mods; g.state_path = join(W, "real_state.ini"); g.cache = join(mods, ".port_cache");
    remove(g.state_path.c_str());
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.list.clear(); g.start.clear(); g.start_sig.clear(); g.zip_dirs.clear(); g.gen++;
    }
    const char* zips[] = { "Karmic Logo TShirt.zip", "White Coat Reaper.zip", "Climbing Gloves.zip", "Sasha's Black Tar.zip",
                           "Mixed Electricity.zip", "Doom Kessler.zip", "Young Kessler.zip" };
    for (const char* zn : zips) {
        ZipPlan z;
        const bool ok = zip_check(join(dl, zn), z);
        fprintf(stderr, "  real %s: %s -> %s, %zu files, %zu left out (%s)\n", zn, ok ? "ok" : z.err.c_str(), z.folder.c_str(), z.files.size(), z.left_out, z.left_kinds.c_str());
        CHECK(ok);
        std::string err;
        CHECK(zip_install(z, false, err));
        if (!err.empty()) fprintf(stderr, "    %s\n", err.c_str());
    }
    {
        ZipPlan z;
        CHECK(!zip_check(join(dl, "inFAMOUS 2 Ability Manager Latest.zip"), z));
        CHECK_HAS(z.err, "contains a program, not a mod");
        fprintf(stderr, "  real Ability Manager: %s\n", z.err.c_str());
    }
    rescan_now();
    CHECK(ps3_game_mods_count() == 7);
    if (idx("Sasha's Black Tar") < 0 || idx("Doom Kessler") < 0) { CHECK(!"the real mods are installed"); return; }
    for (int i = 0; i < ps3_game_mods_count(); i++) {
        ps3_game_mod m;
        ps3_game_mods_get(i, &m);
        char img[600] = "";
        const int hi = ps3_game_mods_image(i, img, sizeof img);
        fprintf(stderr, "  %s: %s | picture: %s\n", m.name, m.status, hi ? img : "-");
        CHECK(m.for_game);
        if (m.has_image) {
            CHECK(hi == 1);
            std::vector<unsigned char> px; int w = 0, h = 0; std::string why;
            CHECK(mod_preview_load(img, 512, px, w, h, &why) == 1 && w <= 512 && h <= 512);
        }
    }
    CHECK_HAS(status_of("Young Kessler"), "same files as Doom Kessler");
    CHECK_HAS(status_of("Sasha's Black Tar"), "same files as Climbing Gloves, Karmic Logo Tshirt");
    ps3_game_mod s;
    ps3_game_mods_get(idx("Sasha's Black Tar"), &s);
    CHECK(s.clash && !strcmp(s.prefer, "Climbing Gloves and Karmic Logo Tshirt will be left out"));
    fprintf(stderr, "  Sasha's Use this one instead: %s\n", s.prefer);
    if (game.empty()) return;
#ifdef _WIN32
    _putenv_s("PS3_HDD0_ROOT", join(game, "dev_hdd0").c_str());
#endif
    static std::string vfs;
    vfs = join(game, "disc");
    ppu_vfs_root = vfs.c_str();
    Mod m;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        m = g.list[(size_t)idx("Sasha's Black Tar")];
    }
    Arcs arcs;
    const std::vector<std::string> r = check_alone(m, arcs);
    for (const std::string& x : r) fprintf(stderr, "  Sasha alone: %s\n", x.c_str());
    CHECK(r.size() == 1);
    if (!r.empty()) CHECK(r[0] == "15 textures were made for another version of cole_gold_amp.xpps: untick Solid Gold Amp");
    Mod d;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        d = g.list[(size_t)idx("Doom Kessler")];
    }
    CHECK(check_alone(d, arcs).empty());
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: test_game_mods <work folder> [<downloads> [<game folder>] [<png> <jpg>]]\n"); return 2; }
    W = slashes(argv[1]);
    mkdirs(W);
    init();   /* PS3_GAME_MODS_DIR unset: nothing read; the tests set g themselves */
    test_zips();
    fprintf(stderr, "zips: %d ok, %d failed\n", g_pass, g_fail);
    test_plan();
    fprintf(stderr, "+plan: %d ok, %d failed\n", g_pass, g_fail);
    test_state();
    fprintf(stderr, "+mods.ini: %d ok, %d failed\n", g_pass, g_fail);
    if (argc >= 6) test_images(slashes(argv[4]), slashes(argv[5]));
    fprintf(stderr, "+images: %d ok, %d failed\n", g_pass, g_fail);
    if (argc >= 3) test_real(slashes(argv[2]), argc >= 4 ? slashes(argv[3]) : std::string());
    printf("test_game_mods: %d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
