/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * Trophy management with persistent storage.  Unlocked trophies are saved
 * to a JSON file at gamedata/trophies/{commId}.json.
 *
 * JSON format (hand-written, no external dependency):
 * { "trophies": [ { "id": N, "timestamp": T }, ... ] }
 */

#include "sceNpTrophy.h"
#include "../system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include "ps3emu/endian.h"                   /* ps3_bswap64 -- guest is big-endian */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
/* HLE args arrive as guest effective addresses; translate before deref. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_trophy_initialized = 0;
static char s_storage_path[512] = "gamedata/trophies";
static int  s_storage_path_set = 0;

/* $PS3_GAMEDATA_ROOT (see cellSaveData.c) moves the default next to the saves;
 * an explicit sceNpTrophySetStoragePath still wins. */
static void trophy_adopt_gamedata_root(void)
{
    static int s_done = 0;
    if (s_done || s_storage_path_set) return;
    s_done = 1;
    const char* env = getenv("PS3_GAMEDATA_ROOT");
    if (env && *env)
        snprintf(s_storage_path, sizeof(s_storage_path), "%s/trophies", env);
}

/* Per-context data */
typedef struct {
    int                      in_use;
    int                      registered;
    SceNpCommunicationId     commId;
    u8                       unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u64                      unlock_time[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u32                      total_trophies;
} TrophyContext;

typedef struct {
    int in_use;
} TrophyHandle;

/* ID zero is the firmware invalid sentinel; retain all four usable slots. */
static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

/* ---------------------------------------------------------------------------
 * Persistent storage helpers
 * -----------------------------------------------------------------------*/

static void trophy_ensure_dir(void)
{
    char tmp[512];
    char* p;

    trophy_adopt_gamedata_root();
    strncpy(tmp, s_storage_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Create directories recursively */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = PATH_SEP;
        }
    }
    mkdir_p(tmp);
}

static void trophy_get_filepath(const SceNpCommunicationId* commId,
                                 char* buf, size_t bufsize)
{
    trophy_adopt_gamedata_root();
    snprintf(buf, bufsize, "%s/%s.json", s_storage_path, commId->data);
}

static void trophy_save(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    int first = 1;

    trophy_ensure_dir();
    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "w");
    if (!fp) {
        printf("[sceNpTrophy] WARNING: Could not save trophies to %s\n",
               filepath);
        return;
    }

    fprintf(fp, "{\n  \"trophies\": [");
    for (u32 i = 0; i < ctx->total_trophies; i++) {
        if (ctx->unlocked[i]) {
            if (!first) fprintf(fp, ",");
            fprintf(fp, "\n    { \"id\": %u, \"timestamp\": %llu }",
                    i, (unsigned long long)ctx->unlock_time[i]);
            first = 0;
        }
    }
    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
}

static void trophy_load(TrophyContext* ctx)
{
    char filepath[1024];
    FILE* fp;
    char line[256];

    trophy_get_filepath(&ctx->commId, filepath, sizeof(filepath));

    fp = fopen(filepath, "r");
    if (!fp)
        return; /* no saved data, all locked */

    /*
     * Simple JSON parser -- look for "id": N and "timestamp": T patterns.
     * This avoids any external JSON library dependency.
     */
    while (fgets(line, sizeof(line), fp)) {
        const char* id_pos = strstr(line, "\"id\":");
        if (id_pos) {
            u32 tid = 0;
            u64 ts = 0;
            const char* ts_pos;

            if (sscanf(id_pos, "\"id\": %u", &tid) == 1 &&
                tid < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
                ts_pos = strstr(line, "\"timestamp\":");
                if (ts_pos)
                    sscanf(ts_pos, "\"timestamp\": %llu",
                           (unsigned long long*)&ts);
                ctx->unlocked[tid] = 1;
                ctx->unlock_time[tid] = ts;
            }
        }
    }
    fclose(fp);

    printf("[sceNpTrophy] Loaded trophy data from %s\n", filepath);
}

static u32 trophy_count_unlocked(TrophyContext* ctx)
{
    u32 count = 0;
    for (u32 i = 0; i < ctx->total_trophies; i++)
        if (ctx->unlocked[i]) count++;
    return count;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

void sceNpTrophySetStoragePath(const char* path)
{
    path = GUEST_PTR(path, const char*);
    if (path) {
        strncpy(s_storage_path, path, sizeof(s_storage_path) - 1);
        s_storage_path[sizeof(s_storage_path) - 1] = '\0';
        s_storage_path_set = 1;
    }
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    (void)poolPtr; (void)poolSize; (void)containerId; (void)options;

    printf("[sceNpTrophy] Init()\n");

    if (s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 1;
    return CELL_OK;
}

s32 sceNpTrophyTerm(void)
{
    printf("[sceNpTrophy] Term()\n");

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    /* Save all registered contexts */
    for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (s_contexts[i].in_use && s_contexts[i].registered)
            trophy_save(&s_contexts[i]);
    }

    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 0;
    return CELL_OK;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    (void)commSign; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!context || !commId)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    const SceNpCommunicationId* commId_h = GUEST_PTR(commId, const SceNpCommunicationId*);

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].in_use) {
            memset(&s_contexts[i], 0, sizeof(TrophyContext));
            s_contexts[i].in_use = 1;
            s_contexts[i].commId = *commId_h;
            s_contexts[i].total_trophies = SCE_NP_TROPHY_MAX_NUM_TROPHIES;
            vm_write32(GUEST_EA(context), (u32)i);
            printf("[sceNpTrophy] CreateContext(commId=\"%s\") -> ctx=%d\n",
                   commId_h->data, i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (s_contexts[context].registered)
        trophy_save(&s_contexts[context]);

    s_contexts[context].in_use = 0;
    printf("[sceNpTrophy] DestroyContext(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (!handle)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
        if (!s_handles[i].in_use) {
            s_handles[i].in_use = 1;
            vm_write32(GUEST_EA(handle), (u32)i);
            printf("[sceNpTrophy] CreateHandle() -> handle=%d\n", i);
            return CELL_OK;
        }
    }

    return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    s_handles[handle].in_use = 0;
    printf("[sceNpTrophy] DestroyHandle(handle=%d)\n", handle);
    return CELL_OK;
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options)
{
    (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES ||
        !s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;

    if (s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_ALREADY_REG;

    if (!statusCb) return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!g_ps3_guest_caller) return SCE_NP_TROPHY_ERROR_UNKNOWN;

    /* Load any previously saved trophy data */
    trophy_load(&s_contexts[context]);
    s_contexts[context].registered = 1;

    printf("[sceNpTrophy] RegisterContext(ctx=%d, handle=%d, statusCb=0x%08X, "
           "arg=0x%08X) -> loaded saved data\n", context, handle, statusCb, arg);

    /* Use the runner's OPD bridge, not the optional code/TOC scaffold entry
     * point. Registration status is synchronous; its terminal notification is
     * delivered through sysutil polling, after the HLE storage load completes.
     * Intermediate firmware installation progress is not simulated here. */
    g_ps3_guest_caller(statusCb, (u32)context, SCE_NP_TROPHY_STATUS_INSTALLED,
                       0, 0, arg, 0, 0, 0);
    if (!s_trophy_initialized) return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!s_contexts[context].in_use) return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    if (!s_handles[handle].in_use) return SCE_NP_TROPHY_ERROR_INVALID_HANDLE;
    const u64 completion[8] = {(u32)context, SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE,
                               0, 0, arg, 0, 0, 0};
    if (cellSysutilQueueGuestCallbackArgs(statusCb, completion) != CELL_OK) {
        s_contexts[context].registered = 0;
        return SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
    }

    return CELL_OK;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    (void)handle; (void)options;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!reqSpace)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    reqSpace = GUEST_PTR(reqSpace, u64*);

    /* Typical trophy pack size. The guest is big-endian and reads this u64
     * back in BE order, so the value MUST be byte-swapped on write -- otherwise
     * a host-LE 0x100000 is read as 0x0010000000000000 (2^44). LBP feeds this
     * "required disk space" straight into its save-data free-space accounting
     * (sub_379EC0: errB = budget - usedKB - reqSpaceKB); a garbage 2^44 makes
     * errB underflow negative -> cellGame GameData check returns ERROR-B ->
     * boot bails. (Same LE-write class as the cellUserInfo fix.) */
    *reqSpace = ps3_bswap64((u64)(1024 * 1024)); /* 1 MB, big-endian */
    printf("[sceNpTrophy] GetRequiredDiskSpace(ctx=%d) -> 1MB\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    details = GUEST_PTR(details, SceNpTrophyGameDetails*);
    data = GUEST_PTR(data, SceNpTrophyGameData*);

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyGameDetails));
        details->numTrophies = 32; /* default trophy set */
        details->numPlatinum = 1;
        details->numGold     = 2;
        details->numSilver   = 8;
        details->numBronze   = 21;
        strncpy(details->title, "PS3 Game",
                SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE - 1);
        strncpy(details->description, "Trophy set",
                SCE_NP_TROPHY_GAME_DESC_MAX_SIZE - 1);
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyGameData));
        data->unlockedTrophies = trophy_count_unlocked(&s_contexts[context]);
        /* Count by grade would require per-trophy grade data;
         * return conservative estimates */
        data->unlockedPlatinum = 0;
        data->unlockedGold     = 0;
        data->unlockedSilver   = 0;
        data->unlockedBronze   = data->unlockedTrophies;
    }

    printf("[sceNpTrophy] GetGameInfo(ctx=%d)\n", context);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    details = GUEST_PTR(details, SceNpTrophyDetails*);
    data = GUEST_PTR(data, SceNpTrophyData*);

    if (details) {
        memset(details, 0, sizeof(SceNpTrophyDetails));
        details->trophyId = (u32)trophyId;
        details->trophyGrade = SCE_NP_TROPHY_GRADE_BRONZE;
        snprintf(details->name, SCE_NP_TROPHY_NAME_MAX_SIZE,
                 "Trophy %d", trophyId);
        snprintf(details->description, SCE_NP_TROPHY_DESC_MAX_SIZE,
                 "Trophy #%d description", trophyId);
        details->hidden = 0;
    }

    if (data) {
        memset(data, 0, sizeof(SceNpTrophyData));
        data->trophyId = (u32)trophyId;
        data->unlocked = s_contexts[context].unlocked[trophyId];
        data->timestamp = s_contexts[context].unlock_time[trophyId];
    }

    return CELL_OK;
}

/* ---- trophy names for an unlock notice ------------------------------------
 * The names live in the game's own TROPHY.TRP (<vfs>/dev_hdd0/game/<title>/
 * TROPDIR/<commId>_00/, or PS3_GAME/TROPDIR): a flat archive whose TROP*.SFM
 * entries are XML per language. Read once, on the first unlock, in the
 * language PS3_LANG asks for (TROP_<id>.SFM) or English (TROP.SFM). The
 * in-game menu shows "Trophy unlocked: <name>" (ps3_overlay_notice, weak, so
 * a build without the overlay just logs). */
#if !defined(_WIN32)
#include <dirent.h>
extern const char* ppu_vfs_root;
void ps3_overlay_notice(const char* text) __attribute__((weak));

static char* trp_read_entry(const char* trp, const char* want, u32* out_size)
{
    FILE* f = fopen(trp, "rb");
    if (!f) return NULL;
    unsigned char h[0x40];
    char* res = NULL;
    if (fread(h, 1, sizeof h, f) == sizeof h &&
        h[0] == 0xDC && h[1] == 0xA2 && h[2] == 0x4D && h[3] == 0x00) {
        const u32 count = (u32)h[16] << 24 | (u32)h[17] << 16 | (u32)h[18] << 8 | h[19];
        const u32 esz   = (u32)h[20] << 24 | (u32)h[21] << 16 | (u32)h[22] << 8 | h[23];
        for (u32 i = 0; i < count && i < 4096 && esz >= 48; i++) {
            unsigned char e[64];
            if (fseek(f, 0x40 + (long)i * esz, SEEK_SET) || fread(e, 1, 48, f) != 48) break;
            if (strncmp((const char*)e, want, 32)) continue;
            u64 off = 0, sz = 0;
            for (int k = 0; k < 8; k++) { off = off << 8 | e[32 + k]; sz = sz << 8 | e[40 + k]; }
            if (sz > (8u << 20) || fseek(f, (long)off, SEEK_SET)) break;
            res = (char*)malloc((size_t)sz + 1);
            if (res && fread(res, 1, (size_t)sz, f) == sz) { res[sz] = 0; *out_size = (u32)sz; }
            else { free(res); res = NULL; }
            break;
        }
    }
    fclose(f);
    return res;
}

static int trp_find(const char* commId, char* out, size_t cap)
{
    snprintf(out, cap, "%s/PS3_GAME/TROPDIR/%s_00/TROPHY.TRP", ppu_vfs_root, commId);
    FILE* f = fopen(out, "rb");
    if (f) { fclose(f); return 1; }
    char games[1024];
    snprintf(games, sizeof games, "%s/dev_hdd0/game", ppu_vfs_root);
    DIR* d = opendir(games);
    if (!d) return 0;
    struct dirent* de;
    int found = 0;
    while (!found && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(out, cap, "%s/%s/TROPDIR/%s_00/TROPHY.TRP", games, de->d_name, commId);
        f = fopen(out, "rb");
        if (f) { fclose(f); found = 1; }
    }
    closedir(d);
    return found;
}

/* PS3 system language ids of the TROP_<id>.SFM files, by PS3_LANG prefix. */
static const char* trp_lang_file(void)
{
    static const struct { const char* lang; const char* file; } k[] = {
        { "ja", "TROP_00.SFM" }, { "fr", "TROP_02.SFM" }, { "es", "TROP_03.SFM" },
        { "de", "TROP_04.SFM" }, { "it", "TROP_05.SFM" }, { "nl", "TROP_06.SFM" },
        { "pt", "TROP_07.SFM" }, { "ru", "TROP_08.SFM" }, { "ko", "TROP_09.SFM" },
        { "zh", "TROP_11.SFM" }, { "fi", "TROP_12.SFM" }, { "sv", "TROP_13.SFM" },
        { "da", "TROP_14.SFM" }, { "no", "TROP_15.SFM" }, { "pl", "TROP_16.SFM" },
    };
    const char* l = getenv("PS3_LANG");
    if (!l) return "TROP.SFM";
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strncmp(l, k[i].lang, 2)) return k[i].file;
    return "TROP.SFM";
}

static void trophy_notice(const char* commId, int id)
{
    static char* xml;
    static int tried;
    if (!tried) {
        tried = 1;
        char trp[1200];
        u32 n = 0;
        if (trp_find(commId, trp, sizeof trp)) {
            xml = trp_read_entry(trp, trp_lang_file(), &n);
            if (!xml) xml = trp_read_entry(trp, "TROP.SFM", &n);
        }
    }
    char name[160] = "";
    if (xml) {
        char key[32];
        snprintf(key, sizeof key, "<trophy id=\"%03d\"", id);
        const char* p = strstr(xml, key);
        const char* a = p ? strstr(p, "<name>") : NULL;
        const char* b = a ? strstr(a, "</name>") : NULL;
        if (a && b && b - a - 6 < (long)sizeof name) { memcpy(name, a + 6, (size_t)(b - a - 6)); name[b - a - 6] = 0; }
    }
    const char* l = getenv("PS3_LANG");
    const int pt = l && !strncmp(l, "pt", 2);
    char text[256];
    if (name[0]) snprintf(text, sizeof text, pt ? "Troféu desbloqueado: %s" : "Trophy unlocked: %s", name);
    else snprintf(text, sizeof text, pt ? "Troféu desbloqueado (#%d)" : "Trophy unlocked (#%d)", id);
    printf("[sceNpTrophy] %s\n", text);
    if (ps3_overlay_notice) ps3_overlay_notice(text);
}
#else
static void trophy_notice(const char* commId, int id) { (void)commId; (void)id; }
#endif

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (trophyId < 0 || (u32)trophyId >= s_contexts[context].total_trophies)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    if (s_contexts[context].unlocked[trophyId])
        return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;

    /* Unlock the trophy */
    s_contexts[context].unlocked[trophyId] = 1;
    s_contexts[context].unlock_time[trophyId] = (u64)time(NULL);

    printf("************************************************************\n");
    printf("*  TROPHY UNLOCKED!  Trophy #%d                            \n",
           trophyId);
    printf("*  Context: %d  CommId: %s                                 \n",
           context, s_contexts[context].commId.data);
    printf("************************************************************\n");

    /* Save immediately */
    trophy_save(&s_contexts[context]);
    trophy_notice(s_contexts[context].commId.data, trophyId);

    /* Check if all non-platinum trophies are unlocked -> platinum */
    if (platinumId) {
        vm_write32((u32)(uintptr_t)platinumId, (u32)SCE_NP_TROPHY_INVALID_TROPHY_ID);
        /* Simplified: don't auto-award platinum without grade info */
    }

    return CELL_OK;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!flags || !count)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 flags_ea = (u32)(uintptr_t)flags;
    for (u32 o = 0; o < sizeof(SceNpTrophyFlagArray); o += 4)
        vm_write32(flags_ea + o, 0);

    for (u32 i = 0; i < s_contexts[context].total_trophies; i++) {
        if (s_contexts[context].unlocked[i])
            {
            u32 word_ea = flags_ea + (i / 32) * 4;
            vm_write32(word_ea, vm_read32(word_ea) | (1u << (i % 32)));
        }
    }

    vm_write32((u32)(uintptr_t)count, (u32)s_contexts[context].total_trophies);
    return CELL_OK;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    (void)handle;

    if (!s_trophy_initialized)
        return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;

    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS ||
        !s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;

    if (!s_contexts[context].registered)
        return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    if (!percentage)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 total = s_contexts[context].total_trophies;
    u32 unlocked = trophy_count_unlocked(&s_contexts[context]);

    s32 pct = total > 0 ? (s32)((unlocked * 100) / total) : 0;
    vm_write32(GUEST_EA(percentage), (u32)pct);

    printf("[sceNpTrophy] GetGameProgress(ctx=%d) -> %d%%\n",
           context, pct);
    return CELL_OK;
}
