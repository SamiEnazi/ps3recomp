/* SPU instruction alternatives switched while the game runs: the switches, their settings and
 * the job record / replay bookkeeping. See spu_patch.h. */
#include "spu_patch.h"
#include "spu_jobrec.h"                          /* g_spu_jobrec_mode, spu_jobrec_dir */
#include "../../include/ps3emu/ps3_settings.h"   /* the live appliers */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile uint8_t g_spu_patch[SPU_PATCH_MAX];
volatile int g_spu_patch_ready;

typedef struct {
    const char* name;      /* NULL = no image registered the slot */
    const char* setting;
    const char* def;
    const char* map;       /* "<value>=<alternative> ..." */
    int sites;
} spu_patch_sw;
static spu_patch_sw s_sw[SPU_PATCH_MAX];
static int s_claim;

/* the alternative `map` gives value v; -1 = none */
static int sp_map(const char* map, const char* v)
{
    if (!map || !v) return -1;
    const size_t vn = strlen(v);
    const char* p = map;
    for (;;) {
        while (*p == ' ') p++;
        const char* eq = strchr(p, '=');
        if (!*p || !eq) return -1;
        const char* end = eq;
        while (*end && *end != ' ') end++;
        if ((size_t)(eq - p) == vn && !strncmp(p, v, vn)) {
            const int a = atoi(eq + 1);
            return a >= 0 && a < 256 ? a : -1;
        }
        p = end;
    }
}

static void sp_record_write(void)
{
    const char* dir = spu_jobrec_dir();
    if (!dir || !*dir) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/spu_patches.txt", dir);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# SPU instruction alternatives in effect while these jobs were recorded (runtime/spu/spu_patch.c);\n"
               "# a replay runs the same ones unless a switch's setting is in its environment.\n");
    for (int i = 0; i < SPU_PATCH_MAX; i++)
        if (s_sw[i].name) fprintf(f, "%s %d\n", s_sw[i].name, (int)g_spu_patch[i]);
    fclose(f);
}

static int sp_apply(int slot, const char* v)
{
    if (slot < 0 || slot >= SPU_PATCH_MAX || !s_sw[slot].name) return 0;
    const spu_patch_sw* w = &s_sw[slot];
    const char* val = (v && *v) ? v : w->def;
    const int alt = sp_map(w->map, val);
    if (alt < 0) {
        fprintf(stderr, "[spu-patch] %s=%s not understood (values %s): %s unchanged\n", w->setting, val, w->map, w->name);
        return 0;
    }
    __atomic_store_n(&g_spu_patch[slot], (uint8_t)alt, __ATOMIC_RELEASE);
    fprintf(stderr, "[spu-patch] %s: %s=%s -> alternative %d\n", w->name, w->setting, val, alt);
    if (g_spu_jobrec_mode & SPU_JOBREC_RECORD) {
        fprintf(stderr, "[spu-patch] %s changed during a job recording: its replays use the last value\n", w->name);
        sp_record_write();
    }
    return 1;
}

/* ps3_settings.h appliers take only the value: one function per slot */
#define SP_APPLIER(k) static int sp_apply_##k(const char* v) { return sp_apply(k, v); }
SP_APPLIER(0) SP_APPLIER(1) SP_APPLIER(2) SP_APPLIER(3) SP_APPLIER(4) SP_APPLIER(5) SP_APPLIER(6) SP_APPLIER(7)
SP_APPLIER(8) SP_APPLIER(9) SP_APPLIER(10) SP_APPLIER(11) SP_APPLIER(12) SP_APPLIER(13) SP_APPLIER(14) SP_APPLIER(15)
static ps3_setting_fn const s_applier[SPU_PATCH_MAX] = {
    sp_apply_0, sp_apply_1, sp_apply_2, sp_apply_3, sp_apply_4, sp_apply_5, sp_apply_6, sp_apply_7,
    sp_apply_8, sp_apply_9, sp_apply_10, sp_apply_11, sp_apply_12, sp_apply_13, sp_apply_14, sp_apply_15,
};

int spu_patch_register(int slot, const char* name, const char* setting, const char* def,
                       const char* map, int sites)
{
    if (slot < 0 || slot >= SPU_PATCH_MAX || !name) return -1;
    spu_patch_sw* w = &s_sw[slot];
    if (w->name) {
        if (strcmp(w->name, name))
            fprintf(stderr, "[spu-patch] slot %d is both %s and %s: two SPU_PATCH_FILEs in one build?\n",
                    slot, w->name, name);
        w->sites += sites;
        return slot;
    }
    w->name = name;
    w->setting = setting ? setting : "";
    w->def = def ? def : "";
    w->map = map ? map : "";
    w->sites = sites;
    if (w->setting[0]) ps3_setting_register(w->setting, s_applier[slot]);
    return slot;
}

int spu_patch_find(const char* name)
{
    for (int i = 0; name && i < SPU_PATCH_MAX; i++)
        if (s_sw[i].name && !strcmp(s_sw[i].name, name)) return i;
    return -1;
}
int spu_patch_sites(int slot) { return slot >= 0 && slot < SPU_PATCH_MAX ? s_sw[slot].sites : 0; }
int spu_patch_alt(int slot)
{
    return slot >= 0 && slot < SPU_PATCH_MAX ? (int)__atomic_load_n(&g_spu_patch[slot], __ATOMIC_ACQUIRE) : 0;
}
const char* spu_patch_setting(int slot)
{
    return slot >= 0 && slot < SPU_PATCH_MAX && s_sw[slot].name ? s_sw[slot].setting : "";
}

void spu_patch_env_init(void)
{
    if (__atomic_load_n(&g_spu_patch_ready, __ATOMIC_ACQUIRE)) return;
    int z = 0;
    if (!__atomic_compare_exchange_n(&s_claim, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (!__atomic_load_n(&g_spu_patch_ready, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
        return;
    }
    const int replay = (g_spu_jobrec_mode & SPU_JOBREC_REPLAY) != 0;
    int rec[SPU_PATCH_MAX], have_rec = 0;
    for (int i = 0; i < SPU_PATCH_MAX; i++) rec[i] = -1;
    if (replay) {   /* the alternatives the recording ran (spu_patches.txt next to the jobs) */
        const char* dir = spu_jobrec_dir();
        char path[1200], line[256], name[128];
        snprintf(path, sizeof path, "%s/spu_patches.txt", dir ? dir : ".");
        FILE* f = fopen(path, "r");
        if (f) {
            have_rec = 1;
            int a;
            while (fgets(line, sizeof line, f))
                if (line[0] != '#' && sscanf(line, "%127s %d", name, &a) == 2) {
                    const int s = spu_patch_find(name);
                    if (s >= 0 && a >= 0 && a < 256) rec[s] = a;
                    else if (s < 0) printf("[jobreplay] SPU patch %s of the recording: not in this build\n", name);
                }
            fclose(f);
        }
    }
    for (int i = 0; i < SPU_PATCH_MAX; i++) {
        const spu_patch_sw* w = &s_sw[i];
        if (!w->name) continue;
        const char* v = w->setting[0] ? getenv(w->setting) : NULL;
        int alt;
        const char* why;
        if (replay && !(v && *v)) {
            alt = rec[i] >= 0 ? rec[i] : 0;
            why = rec[i] >= 0 ? "as recorded" : have_rec ? "not in the recording: the original"
                                                          : "the recording predates SPU patches: the original";
        } else {
            alt = sp_map(w->map, (v && *v) ? v : w->def);
            why = (v && *v) ? "set" : "default";
            if (alt < 0) {
                fprintf(stderr, "[spu-patch] %s=%s not understood (values %s): the default %s\n", w->setting, v, w->map, w->def);
                alt = sp_map(w->map, w->def);
                if (alt < 0) alt = 0;
                why = "default";
            }
        }
        __atomic_store_n(&g_spu_patch[i], (uint8_t)alt, __ATOMIC_RELEASE);
        if (replay) {
            if (v && *v) printf("[jobreplay] SPU patch %s: alternative %d (%s=%s in the environment), %d lifted instruction(s)\n",
                                w->name, alt, w->setting, v, w->sites);
            else printf("[jobreplay] SPU patch %s: alternative %d (%s), %d lifted instruction(s)\n", w->name, alt, why, w->sites);
        }
        fprintf(stderr, "[spu-patch] %s: %s=%s (%s) -> alternative %d, %d lifted instruction(s)\n", w->name,
                w->setting, (v && *v) ? v : w->def, why, alt, w->sites);
    }
    if (g_spu_jobrec_mode & SPU_JOBREC_RECORD) sp_record_write();
    fflush(stdout);
    __atomic_store_n(&g_spu_patch_ready, 1, __ATOMIC_RELEASE);
}
