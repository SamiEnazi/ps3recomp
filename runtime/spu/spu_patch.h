/*
 * spu_patch -- SPU instruction alternatives switched while the game runs (trim, 2026-11-10).
 *
 * The SPU twin of the PPU's g_ppu_patch (runtime/ppu/ppu_loader.cpp): a lift with
 * SPU_PATCH_FILE (tools/spu_lifter.py load_spu_patches) gives each listed instruction its
 * alternatives,
 *
 *     if (g_spu_patch[slot] == 1) { <alternative 1> } else ... else { <the original> }
 *
 * so slot value 0 (the default of the storage) runs the code as the console does, and an
 * RPCS3-style SPU patch is a run-time choice instead of a code change. Each lifted image
 * registers the switches it tests from a constructor (spu_patch_register): name, the
 * setting that picks the alternative (an environment variable, also a settings.ini line and
 * an in-game menu setting through ps3_settings.h), its default and its value map.
 *
 * spu_patch_env_init (once, from spu_run_with_halt: after main, so settings.ini's values are
 * in the environment) sets every switch from its setting. The switches are live: the
 * setting's applier (registered with ps3_setting_register) changes the byte at once; the
 * lifted code tests it at the instruction, and a job in flight sees the new value at the
 * next pass there.
 *
 * Job record / replay (spu_jobrec.c): a recording directory gets spu_patches.txt with the
 * alternatives in effect while it was recorded, and a replay uses them, so a replay checks
 * the code that was recorded. A setting in the replay's environment wins (to see what an
 * alternative changes); a recording without the file predates SPU patches: every switch 0.
 */
#ifndef PS3RECOMP_SPU_PATCH_H
#define PS3RECOMP_SPU_PATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_PATCH_MAX 16

/* the alternative each switch runs (0 = the original instruction); the lifted code reads it */
extern volatile uint8_t g_spu_patch[SPU_PATCH_MAX];
/* nonzero once spu_patch_env_init has run */
extern volatile int g_spu_patch_ready;

/* A lifted image's switch (static initialisation, one thread). `sites` = the instructions of
 * that image with its alternatives. Strings must outlive the program (literals). */
int  spu_patch_register(int slot, const char* name, const char* setting, const char* def,
                        const char* map, int sites);
/* The slot of a switch by name; -1 = no lifted image of this build tests it. */
int  spu_patch_find(const char* name);
int  spu_patch_sites(int slot);            /* its lifted instructions, all images */
int  spu_patch_alt(int slot);              /* the alternative it runs now (0 = the original) */
const char* spu_patch_setting(int slot);   /* the setting that picks it ("" = none) */
void spu_patch_env_init(void);

#ifdef __cplusplus
}
#endif

#endif
