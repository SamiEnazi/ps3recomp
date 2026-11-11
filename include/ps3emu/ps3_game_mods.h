/*
 * ps3_game_mods -- the game mods the file layer serves (runtime/ppu/ppu_fs_mods.h),
 * for the in-game menu's Mods page (libs/video/vk_overlay.cpp).
 *
 * The list is read once at start and again on ps3_game_mods_rescan(); what a
 * mod changes is decided at start, so a choice made in the menu (on/off, its
 * options) is saved at once and takes effect at the next start.
 */
#ifndef PS3EMU_PS3_GAME_MODS_H
#define PS3EMU_PS3_GAME_MODS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char folder[128];     /* the mod's folder, below the mods folder ("Red Jacket", "pack/Red Jacket") */
    char name[96];        /* mod.json "name" (else the folder) */
    char author[64];
    char version[32];
    int  on;              /* the saved choice: used at the next start */
    int  active;          /* in use in this session */
    int  for_game;        /* mod.json names inFAMOUS 2 */
    int  n_files;         /* whole files it replaces */
    int  n_tex;           /* texture patches */
    int  n_xpps;          /* packed-file patches */
    int  n_modules;       /* options (modular.json) */
    char status[240];     /* one line for the menu: what it changes, or why it is not used */
} ps3_game_mod;

typedef struct {
    char id[64];
    char name[96];
    char group[96];       /* "" = a toggle of its own */
    char group_id[64];
    int  dropdown;        /* one of a group's options (else a toggle) */
    int  picked;          /* chosen (saved) */
} ps3_game_mod_option;

const char* ps3_game_mods_dir(void);          /* the mods folder ("" = none) */
int  ps3_game_mods_count(void);
int  ps3_game_mods_get(int i, ps3_game_mod* out);
int  ps3_game_mods_option(int i, int k, ps3_game_mod_option* out);
void ps3_game_mods_set_on(int i, int on);    /* saved; next start */
void ps3_game_mods_pick(int i, int k, int on); /* an option on/off (a dropdown's: chosen) */
void ps3_game_mods_rescan(void);             /* re-read the folder for the menu */
int  ps3_game_mods_in_use(void);             /* files served from mods this session */
void ps3_game_mods_open_folder(void);        /* the mods folder in Explorer (Windows) */

#ifdef __cplusplus
}
#endif

#endif
