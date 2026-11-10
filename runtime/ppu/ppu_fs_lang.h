/*
 * ps3recomp - voice and text in different languages (included once, by ppu_fs.cpp)
 *
 * A title reads the console language once at boot and loads that language's
 * files; nothing on a real PS3 lets the dub and the text differ. Sucker
 * Punch's PSARC layout (inFAMOUS 2) keeps each language apart, so the two can
 * be split here, under the guest, by serving other files than the ones it
 * asked for:
 *
 *   PS3_VOICE_LANG=<name>  the dub:
 *       cache/psarc/<lang>.psarc            dialogue (/dialogue/*.ogg)
 *       movies/.../*.bik                    the voice track of the movies
 *   PS3_TEXT_LANG=<name>   what is read on screen:
 *       cache/psarc/<lang>_text.psarc       subtitles (/subtitle/*.bsub)
 *       lang_<lang>.xpps inside the game's  menus, HUD and the font with the
 *       big archives (infamous1.psarc_s,    glyphs (the Korean/Chinese/Japanese
 *       patch4.psarc_s)                     ones are bigger), so subtitles and
 *                                           menus must change together
 *
 * <name> is the file name's language as the copy spells it: english,
 * british, brazilian, portuguese, latino, spanish, french, german, italian,
 * dutch, polish, russian, turkish, japanese, korean, chinese. A name the
 * copy does not have is ignored with a log line, never half-applied.
 *
 * Nor is the dialogue ever silent: a copy may lack the dub the game itself
 * asks for (the EU disc BCES01143 has only english.psarc; told Italian, the
 * game opens italian.psarc). Such a request is answered with the game's own
 * dub if that is not it, else english.psarc, else the folder's first dub, and
 * the movies follow (below). ps3_lang_missing() tells the menu.
 *
 * Unset, each one follows PS3_LANG: a release only picks the languages of
 * its region and plays every other one in English (the Americas inFAMOUS 2
 * answers en, fr, es and pt with english, french, latino and brazilian, and
 * ru, ja, de, ko, en-gb, pt-br, tr... all with english, though the copy has
 * them). So when PS3_LANG names one language unambiguously (ru -> russian,
 * en-gb -> british, pt-br -> brazilian, pt-pt -> portuguese, es-es -> spanish,
 * ...), that is the default for both halves. pt and es are regional ("the
 * game's own Portuguese/Spanish": brazilian and latino here) and en is the
 * game's English, so those leave the choice to the game.
 *
 * How each kind is redirected:
 *  - Files: a guest open/stat of ".../<A>.psarc" or ".../<A>_text.psarc" is
 *    answered with ".../<B>.psarc" / ".../<B>_text.psarc" from the same
 *    folder, when that file exists. "<A>.psarc" only counts as a voice
 *    archive when "<A>_text.psarc" sits beside it (so core.psarc, music.psarc
 *    and the rest are never touched).
 *  - Archive entries: the game finds lang_<A>.xpps in a PSARC by the MD5 of
 *    its name in the table of contents. When such an archive is opened, the
 *    TOC records of every lang_*.xpps are made to point at lang_<B>.xpps's
 *    data (offset, size, first block): the hash still matches, the bytes are
 *    B's. The TOC is patched in the read buffer, never on disk.
 *  - Movies: the cutscene and TV .bik files carry the 5.1 bed as tracks 1-6
 *    and one voice track per dubbed language, whose id is 10 + the language's
 *    index in the game's name table (EBOOT 0x7B21C0: the PS3 system-language
 *    order, then british, latino, brazilian, turkish); the 13 voiced ids in
 *    every movie are exactly 10-18, 26, 28-30. The game asks Bink for its own
 *    language's id, so the two ids are swapped in the header as it is read.
 *    The game's own language is learned from the voice archive it opened,
 *    and the movies follow the dub that archive was answered with (a chosen
 *    dub the copy lacks leaves the movies' voice the game's; a fallback for
 *    the game's own missing dub moves them to the fallback).
 *
 * Each redirect is logged once ("[lang] ..."). Only the cellFs layer of
 * ppu_fs.cpp does this: it is the one inFAMOUS 2 opens its data through.
 */
#ifndef PPU_FS_LANG_H
#define PPU_FS_LANG_H

#include <atomic>
#include <mutex>
#include <set>
#include <stdarg.h>
#include <string>
#include <vector>

/* stb_image's zlib (libs/codec/cellPngDec.c owns the implementation): the
 * PSARC name list is a zlib stream. */
extern "C" int stbi_zlib_decode_buffer(char* obuffer, int olen, const char* ibuffer, int ilen);

#define LANG_FD_MAX 256          /* == FS_MAX in ppu_fs.cpp (static_assert there) */
#define LANG_NAME_MAX 24

/* inFAMOUS 2's language-name table (EBOOT 0x7B21C0); index + 10 is the
 * language's voice track id in the movies. */
static const char* const kLangGameNames[] = {
    "japanese", "english", "french", "spanish", "german", "italian", "dutch",
    "portuguese", "russian", "korean", "chinese", "chinese_s", "finnish",
    "swedish", "danish", "norwegian", "polish", "british", "latino",
    "brazilian", "turkish",
};
#ifdef _WIN32
#  define LANG_FSEEK(f, off) _fseeki64((f), (__int64)(off), SEEK_SET)
#else
#  define LANG_FSEEK(f, off) fseeko((f), (off_t)(off), SEEK_SET)
#endif

/* ASCII case-insensitive compare of n bytes (no strncasecmp on MSVC). */
static bool lang_ieq_n(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        if (!x) return true;
    }
    return true;
}

static int lang_game_index(const char* name)
{
    for (int i = 0; i < (int)(sizeof kLangGameNames / sizeof kLangGameNames[0]); i++)
        if (!strcmp(name, kLangGameNames[i])) return i;
    return -1;
}

static struct {
    std::once_flag once;
    char voice[LANG_NAME_MAX];       /* PS3_VOICE_LANG ("" = the game's) */
    char text[LANG_NAME_MAX];        /* PS3_TEXT_LANG */
    std::mutex mx;                   /* everything below */
    char game_voice[LANG_NAME_MAX];  /* what the game asked for */
    char game_text[LANG_NAME_MAX];
    char used_voice[LANG_NAME_MAX];  /* what it was given */
    char used_text[LANG_NAME_MAX];
    char missing_voice[LANG_NAME_MAX]; /* the dub it should have had, not in this copy ("" = none) */
    char voice_dir[1100];            /* host folder of the language archives */
    std::set<std::string> logged;
} g_lang;

struct LangPatch { uint64_t off; uint32_t len; uint8_t bytes[16]; };
static std::vector<LangPatch> g_lang_patch[LANG_FD_MAX];
static std::atomic<uint8_t>   g_lang_has_patch[LANG_FD_MAX];
static std::mutex             g_lang_patch_mx;

static void lang_log_once(const char* key, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void lang_log_once(const char* key, const char* fmt, ...)
{
    {
        std::lock_guard<std::mutex> lk(g_lang.mx);
        if (!g_lang.logged.insert(key).second) return;
    }
    char line[512];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    fprintf(stderr, "[lang] %s\n", line);
}

/* "English" -> "english"; "", "same", "auto", "default" or anything that is
 * not a plain name -> "" (no redirect). */
static void lang_norm(const char* in, char* out, const char* var)
{
    out[0] = 0;
    if (!in || !*in) return;
    size_t n = 0;
    for (const char* p = in; *p && n < LANG_NAME_MAX - 1; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || c == '_')) {
            fprintf(stderr, "[lang] %s=%s is not a language name (english, brazilian, latino, ...): ignored\n", var, in);
            out[0] = 0; return;
        }
        out[n++] = c;
    }
    out[n] = 0;
    if (!strcmp(out, "same") || !strcmp(out, "auto") || !strcmp(out, "default")) out[0] = 0;
}

/* The one language a PS3_LANG value names, as the files spell it; "" for
 * the regional ones (pt, es, en: the game's own choice). Codes and numbers
 * as libs/system/cellSysutil.c reads them. */
static const char* lang_implied(const char* code)
{
    if (!code || !*code) return "";
    static const struct { const char* code; int num; const char* name; } k[] = {
        {"ja", 0, "japanese"}, {"fr", 2, "french"}, {"de", 4, "german"}, {"it", 5, "italian"},
        {"nl", 6, "dutch"}, {"ru", 8, "russian"}, {"ko", 9, "korean"}, {"zh-t", 10, "chinese"},
        {"zh-s", 11, "chinese_s"}, {"fi", 12, "finnish"}, {"sv", 13, "swedish"}, {"da", 14, "danish"},
        {"no", 15, "norwegian"}, {"pl", 16, "polish"}, {"pt-br", 17, "brazilian"}, {"en-gb", 18, "british"},
        {"tr", 19, "turkish"}, {"pt-pt", -1, "portuguese"}, {"es-es", -1, "spanish"},
        {"es-419", -1, "latino"}, {"en-us", -1, "english"},
    };
    char* end = NULL;
    const long n = strtol(code, &end, 10);
    const bool num = end && end != code && *end == '\0';
    for (const auto& e : k) {
        if (num ? (e.num >= 0 && n == e.num)
                : (strlen(code) == strlen(e.code) && lang_ieq_n(code, e.code, strlen(e.code))))
            return e.name;
    }
    return "";
}

static void lang_init(void)
{
    std::call_once(g_lang.once, [] {
        const char* sys = getenv("PS3_LANG");
        const char* implied = lang_implied(sys);
        lang_norm(getenv("PS3_VOICE_LANG"), g_lang.voice, "PS3_VOICE_LANG");
        lang_norm(getenv("PS3_TEXT_LANG"), g_lang.text, "PS3_TEXT_LANG");
        const bool vset = g_lang.voice[0] != 0, tset = g_lang.text[0] != 0;
        if (!vset) snprintf(g_lang.voice, sizeof g_lang.voice, "%s", implied);
        if (!tset) snprintf(g_lang.text, sizeof g_lang.text, "%s", implied);
        if (g_lang.voice[0] || g_lang.text[0])
            fprintf(stderr, "[lang] voice: %s%s, text and subtitles: %s%s\n",
                    g_lang.voice[0] ? g_lang.voice : "the game's", vset ? " (PS3_VOICE_LANG)" : " (PS3_LANG)",
                    g_lang.text[0] ? g_lang.text : "the game's", tset ? " (PS3_TEXT_LANG)" : " (PS3_LANG)");
    });
}

static bool lang_file_exists(const char* p)
{
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

/* The dubs in a folder (dir ends in '/'): each <name>.psarc with its
 * <name>_text.psarc beside it, sorted. */
static void lang_dubs_in(const char* dir, std::set<std::string>& out)
{
    DIR* d = opendir(dir);
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        const char* nm = e->d_name;
        const size_t l = strlen(nm);
        if (l <= 11 || strcmp(nm + l - 11, "_text.psarc") || l - 11 >= LANG_NAME_MAX) continue;
        char p[1200];
        snprintf(p, sizeof p, "%s%.*s.psarc", dir, (int)(l - 11), nm);
        if (lang_file_exists(p)) out.insert(std::string(nm, l - 11));
    }
    closedir(d);
}

/* A voice archive request: the dub it gets is the chosen one (PS3_VOICE_LANG,
 * else PS3_LANG's), else the game's own, as far as this folder has them. A
 * dub the copy lacks -- the EU disc BCES01143 carries only english.psarc, and
 * the game told Italian asks for italian.psarc -- would leave the dialogue
 * silent, so the game's request then falls back to english.psarc, else the
 * folder's first dub. A folder with no dub at all (patch/psarc holds only
 * text archives) is left alone: the game goes on to the disc's. */
static void lang_redirect_voice(char* path, size_t cap, const char* stem, size_t dl)
{
    const char* want = g_lang.voice[0] ? g_lang.voice : stem;   /* the dub this request should get */
    char dir[1100], cand[1100];
    snprintf(dir, sizeof dir, "%.*s", (int)dl, path);
    const char* tries[3] = { want, stem, "english" };
    char got[LANG_NAME_MAX] = "";
    for (const char* t : tries) {
        snprintf(cand, sizeof cand, "%s%s.psarc", dir, t);
        if (lang_file_exists(cand)) { snprintf(got, sizeof got, "%s", t); break; }
    }
    if (!got[0]) {
        std::set<std::string> dubs;
        lang_dubs_in(dir, dubs);
        if (dubs.empty()) return;
        snprintf(got, sizeof got, "%s", dubs.begin()->c_str());
    }
    {
        std::lock_guard<std::mutex> lk(g_lang.mx);
        snprintf(g_lang.game_voice, sizeof g_lang.game_voice, "%s", stem);
        snprintf(g_lang.used_voice, sizeof g_lang.used_voice, "%s", got);
        snprintf(g_lang.missing_voice, sizeof g_lang.missing_voice, "%s", strcmp(got, want) ? want : "");
        if (dl < sizeof g_lang.voice_dir) snprintf(g_lang.voice_dir, sizeof g_lang.voice_dir, "%s", dir);
    }
    char key[160];
    snprintf(key, sizeof key, "voice:%s>%s", stem, got);
    if (strcmp(got, want))
        lang_log_once(key, "voice: no %s dub in this copy (no %s.psarc in %s): %s.psarc%s", want, want, dir, got,
                      strcmp(got, stem) ? " instead, so the dialogue is not silent" : ", the game's own");
    else if (strcmp(got, stem))
        lang_log_once(key, "voice: %s -> %s.psarc", path, got);
    if (strcmp(got, stem)) snprintf(path, cap, "%s%s.psarc", dir, got);
}

/* After the guest path is translated: swap a language archive's name for
 * the chosen one. path is a host path; it is rewritten in place. */
static void lang_redirect_host(char* path, size_t cap)
{
    lang_init();
    char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    const size_t bl = strlen(base);
    static const char kText[] = "_text.psarc", kVoice[] = ".psarc";
    const size_t tl = sizeof kText - 1, vl = sizeof kVoice - 1;
    const bool is_text = bl > tl && !strcmp(base + bl - tl, kText);
    const bool is_voice = !is_text && bl > vl && !strcmp(base + bl - vl, kVoice);
    if (!is_text && !is_voice) return;
    const size_t sl = bl - (is_text ? tl : vl);
    if (sl >= LANG_NAME_MAX) return;
    char stem[LANG_NAME_MAX];
    memcpy(stem, base, sl); stem[sl] = 0;
    for (size_t i = 0; i < sl; i++) if (!((stem[i] >= 'a' && stem[i] <= 'z') || stem[i] == '_')) return;

    const size_t dl = (size_t)(base - path);           /* folder, with its trailing '/' */
    if (dl >= 1000) return;                            /* the folder must fit the candidates' buffers */
    char cand[1100];
    if (is_voice) {
        /* A voice archive: its text archive sits beside it, or it is named
         * after one of the game's languages (a dub the copy lacks may have
         * no text archive either). core.psarc, music.psarc: neither. */
        snprintf(cand, sizeof cand, "%.*s%s_text.psarc", (int)dl, path, stem);
        if (!lang_file_exists(cand) && lang_game_index(stem) < 0) return;
        lang_redirect_voice(path, cap, stem, dl);
        return;
    }
    const char* want = g_lang.text;
    {
        std::lock_guard<std::mutex> lk(g_lang.mx);
        snprintf(g_lang.game_text, sizeof g_lang.game_text, "%s", stem);
        snprintf(g_lang.used_text, sizeof g_lang.used_text, "%s", stem);
    }
    if (!want[0] || !strcmp(want, stem)) return;
    snprintf(cand, sizeof cand, "%.*s%s%s", (int)dl, path, want, kText);
    char key[160];
    snprintf(key, sizeof key, "file:%s", cand);
    if (!lang_file_exists(cand)) {
        /* The game also probes folders that hold no language at all
         * (patch/psarc): nothing to say unless the folder has the original. */
        if (lang_file_exists(path))
            lang_log_once(key, "text: %s is not in this copy, keeping %s", cand, base);
        return;
    }
    lang_log_once(key, "text: %s -> %s%s", path, want, kText);
    snprintf(path, cap, "%s", cand);
    std::lock_guard<std::mutex> lk(g_lang.mx);
    snprintf(g_lang.used_text, sizeof g_lang.used_text, "%s", want);
}

static uint64_t lang_be(const uint8_t* p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static void lang_add_patch(std::vector<LangPatch>& v, uint64_t off, const uint8_t* bytes, uint32_t len)
{
    LangPatch p; p.off = off; p.len = len; memcpy(p.bytes, bytes, len);
    v.push_back(p);
}

/* lang_<A>.xpps -> lang_<B>.xpps in a PSARC's table of contents. */
static void lang_scan_psarc(FILE* f, const char* hpath, std::vector<LangPatch>& out)
{
    uint8_t h[32];
    if (LANG_FSEEK(f, 0) != 0 || fread(h, 1, 32, f) != 32 || memcmp(h, "PSAR", 4)) return;
    const uint32_t toclen = (uint32_t)lang_be(h + 12, 4), esz = (uint32_t)lang_be(h + 16, 4);
    const uint32_t n = (uint32_t)lang_be(h + 20, 4), bsz = (uint32_t)lang_be(h + 24, 4);
    if (memcmp(h + 8, "zlib", 4) || esz != 30 || n < 2 || n > 1000000 || toclen < 32 + n * esz ||
        toclen > (64u << 20) || bsz == 0 || bsz > (16u << 20))
        return;
    std::vector<uint8_t> toc(toclen - 32);
    if (fread(toc.data(), 1, toc.size(), f) != toc.size()) return;
    const int nb = bsz <= 0x10000 ? 2 : bsz <= 0x1000000 ? 3 : 4;
    const size_t tab = (size_t)n * esz, nblocks = (toc.size() - tab) / (size_t)nb;
    /* Entry 0 is the name list: entry i+1 is line i. */
    const uint8_t* e0 = toc.data();
    uint64_t blk = lang_be(e0 + 16, 4), usz = lang_be(e0 + 20, 5), off = lang_be(e0 + 25, 5);
    if (usz == 0 || usz > (16u << 20)) return;
    std::vector<char> names; names.reserve((size_t)usz);
    std::vector<char> in(bsz), dec(bsz);
    if (LANG_FSEEK(f, off) != 0) return;
    while (names.size() < usz) {
        if (blk >= nblocks) return;
        const uint32_t c = (uint32_t)lang_be(toc.data() + tab + blk * nb, nb);
        blk++;
        const uint32_t take = c ? c : bsz;
        if (take > bsz || fread(in.data(), 1, take, f) != take) return;
        const size_t want = (size_t)(usz - names.size()) < bsz ? (size_t)(usz - names.size()) : bsz;
        int got = -1;
        if (c && (uint8_t)in[0] == 0x78)
            got = stbi_zlib_decode_buffer(dec.data(), (int)bsz, in.data(), (int)take);
        if (got > 0) names.insert(names.end(), dec.data(), dec.data() + (got < (int)want ? got : (int)want));
        else names.insert(names.end(), in.data(), in.data() + (take < want ? take : want));
    }
    /* Find lang_<B>.xpps and every other lang_*.xpps. */
    char target[64];
    snprintf(target, sizeof target, "lang_%s.xpps", g_lang.text);
    std::vector<uint32_t> others;
    int64_t t = -1;
    uint32_t idx = 1;
    for (size_t s = 0; s < names.size() && idx < n; idx++) {
        size_t e = s;
        while (e < names.size() && names[e] != '\n') e++;
        const char* nm = names.data() + s; size_t len = e - s;
        if (len && nm[0] == '/') { nm++; len--; }
        if (len > 10 && lang_ieq_n(nm, "lang_", 5) && lang_ieq_n(nm + len - 5, ".xpps", 5)) {
            if (len == strlen(target) && lang_ieq_n(nm, target, len)) t = idx;
            else others.push_back(idx);
        }
        s = e + 1;
    }
    if (others.empty()) return;
    const char* base = strrchr(hpath, '/'); base = base ? base + 1 : hpath;
    char key[160];
    snprintf(key, sizeof key, "xpps:%s", hpath);
    if (t < 0) {
        lang_log_once(key, "text: %s has no %s, its menus stay the game's", base, target);
        return;
    }
    for (uint32_t j : others)
        lang_add_patch(out, 32 + (uint64_t)j * esz + 16, toc.data() + (size_t)t * esz + 16, 14);
    lang_log_once(key, "text: menus and font -> %s in %s (%zu lang_*.xpps entries point at it)",
                  target, base, others.size());
}

/* The voice track ids of a .bik: the game's language's and the served dub's
 * swap. The served one, not the chosen one (lang_redirect_voice): movies and
 * dialogue never speak different languages. */
static void lang_scan_bik(FILE* f, const char* hpath, std::vector<LangPatch>& out)
{
    char game[LANG_NAME_MAX], used[LANG_NAME_MAX];
    { std::lock_guard<std::mutex> lk(g_lang.mx);
      snprintf(game, sizeof game, "%s", g_lang.game_voice);
      snprintf(used, sizeof used, "%s", g_lang.used_voice); }
    const int ia = lang_game_index(game), ib = lang_game_index(used);
    if (ia < 0 || ib < 0 || ia == ib) return;
    uint8_t h[0x2C + 12 * 64];
    if (LANG_FSEEK(f, 0) != 0 || fread(h, 1, 0x2C, f) != 0x2C || memcmp(h, "BIK", 3) || h[3] == 'k') return;
    const uint32_t nt = (uint32_t)h[0x28] | ((uint32_t)h[0x29] << 8) | ((uint32_t)h[0x2A] << 16) | ((uint32_t)h[0x2B] << 24);
    if (nt == 0 || nt > 64 || fread(h + 0x2C, 1, 12 * nt, f) != 12 * nt) return;
    const uint32_t ids_at = 0x2C + 8 * nt;
    const uint32_t ida = 10u + (uint32_t)ia, idb = 10u + (uint32_t)ib;
    int ta = -1, tb = -1;
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t* p = h + ids_at + 4 * i;
        const uint32_t id = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        if (id == ida) ta = (int)i;
        if (id == idb) tb = (int)i;
    }
    if (ta < 0 || tb < 0) return;      /* no voice, or not in both languages */
    const uint8_t a[4] = { (uint8_t)ida, 0, 0, 0 }, b[4] = { (uint8_t)idb, 0, 0, 0 };
    lang_add_patch(out, ids_at + 4 * (uint64_t)tb, a, 4);   /* B's audio answers to A's id */
    lang_add_patch(out, ids_at + 4 * (uint64_t)ta, b, 4);
    const char* base = strrchr(hpath, '/'); base = base ? base + 1 : hpath;
    char key[160];
    snprintf(key, sizeof key, "bik:%s", base);
    lang_log_once(key, "voice: %s plays its %s track (id %u) for %s (id %u)",
                  base, used, idb, game, ida);
}

/* An fd was opened on hpath: note what its reads must change. */
static void lang_on_open(int fd, const char* hpath)
{
    if (fd < 0 || fd >= LANG_FD_MAX) return;
    lang_init();
    std::vector<LangPatch> p;
    /* The movies follow the served dub whenever it is not the game's: a chosen
     * one, or the fallback for a dub the copy lacks (also with no PS3_VOICE_LANG
     * or PS3_LANG at all: the game told Italian by the host language). */
    bool vswap;
    { std::lock_guard<std::mutex> lk(g_lang.mx);
      vswap = g_lang.game_voice[0] && g_lang.used_voice[0] && strcmp(g_lang.game_voice, g_lang.used_voice); }
    if (g_lang.text[0] || vswap) {
        const size_t l = strlen(hpath);
        const bool psarc = (l > 6 && !strcmp(hpath + l - 6, ".psarc")) || (l > 8 && !strcmp(hpath + l - 8, ".psarc_s"));
        const bool bik = l > 4 && lang_ieq_n(hpath + l - 4, ".bik", 4);
        if ((psarc && g_lang.text[0]) || (bik && vswap)) {
            /* A FILE of its own: the guest's descriptor keeps its position. */
            if (FILE* f = fopen(hpath, "rb")) {
                if (psarc) lang_scan_psarc(f, hpath, p); else lang_scan_bik(f, hpath, p);
                fclose(f);
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_lang_patch_mx);
    g_lang_patch[fd].swap(p);
    g_lang_has_patch[fd].store(g_lang_patch[fd].empty() ? 0 : 1, std::memory_order_release);
}

static void lang_on_close(int fd)
{
    if (fd < 0 || fd >= LANG_FD_MAX || !g_lang_has_patch[fd].load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_lang_patch_mx);
    g_lang_patch[fd].clear();
    g_lang_has_patch[fd].store(0, std::memory_order_release);
}

/* n bytes of fd's file from offset off were just read into buf. */
static void lang_apply(int fd, uint64_t off, uint8_t* buf, size_t n)
{
    if (fd < 0 || fd >= LANG_FD_MAX || !n || !g_lang_has_patch[fd].load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_lang_patch_mx);
    for (const LangPatch& p : g_lang_patch[fd]) {
        const uint64_t lo = p.off > off ? p.off : off;
        const uint64_t hi = (p.off + p.len) < (off + n) ? (p.off + p.len) : (off + n);
        if (lo < hi) memcpy(buf + (lo - off), p.bytes + (lo - p.off), (size_t)(hi - lo));
    }
}

/* ---- for the in-game menu (ps3_overlay.mm, vk_overlay.cpp) ---------------- */

/* The language the game asked for and the one it was given:
 * kind 0 voice, 1 text. "" until the game opened that archive. */
extern "C" void ps3_lang_in_use(int kind, char* asked, char* given, size_t n)
{
    std::lock_guard<std::mutex> lk(g_lang.mx);
    if (asked) snprintf(asked, n, "%s", kind ? g_lang.game_text : g_lang.game_voice);
    if (given) snprintf(given, n, "%s", kind ? g_lang.used_text : g_lang.used_voice);
}

/* The dub the game (or PS3_VOICE_LANG) wanted that this copy has no
 * <name>.psarc for, "" if none: the dialogue then plays ps3_lang_in_use's
 * "given" one instead. kind 0 (voice) only; text always gets its file. */
extern "C" void ps3_lang_missing(int kind, char* out, size_t n)
{
    if (!out || !n) return;
    std::lock_guard<std::mutex> lk(g_lang.mx);
    snprintf(out, n, "%s", kind ? "" : g_lang.missing_voice);
}

/* The languages this copy has: kind 0 voice (<name>.psarc beside its
 * <name>_text.psarc), 1 text (<name>_text.psarc). Read from the folder the
 * game opened its voice archive in, else <vfs root>/PS3_GAME/USRDIR/cache/psarc.
 * Fills out[max][LANG_NAME_MAX] sorted; returns the count. */
extern "C" int ps3_lang_available(int kind, char (*out)[LANG_NAME_MAX], int max)
{
    char dir[1100];
    { std::lock_guard<std::mutex> lk(g_lang.mx); snprintf(dir, sizeof dir, "%s", g_lang.voice_dir); }
    if (!dir[0]) snprintf(dir, sizeof dir, "%s/PS3_GAME/USRDIR/cache/psarc/", ppu_vfs_root ? ppu_vfs_root : ".");
    std::set<std::string> found;
    if (kind == 0) {
        lang_dubs_in(dir, found);
    } else if (DIR* d = opendir(dir)) {
        while (struct dirent* e = readdir(d)) {
            const char* nm = e->d_name;
            const size_t l = strlen(nm);
            if (l <= 11 || strcmp(nm + l - 11, "_text.psarc") || l - 11 >= LANG_NAME_MAX) continue;
            found.insert(std::string(nm, l - 11));
        }
        closedir(d);
    }
    int i = 0;
    for (const std::string& s : found) {
        if (i >= max) break;
        snprintf(out[i++], LANG_NAME_MAX, "%s", s.c_str());
    }
    return i;
}

#endif /* PPU_FS_LANG_H */
