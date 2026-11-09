/*
 * ps3_savedlg -- the system's save-data list ("choose a save") dialog.
 *
 * On a PS3, cellSaveDataListSave2 / ListLoad2 show a list the SYSTEM draws:
 * the title's funcList only says what is listed (fixedList, its order, the
 * cursor's start, a "New Save Data" entry), then the player picks, deletes
 * (Triangle) or backs out (Circle = CELL_SAVEDATA_RET_CANCEL). cellSaveData.c
 * builds that list and hands it to a provider -- the Vulkan overlay
 * (libs/video/vk_overlay_saves.cpp) -- which shows it and blocks the calling
 * guest thread until the player acts. With no provider (headless, no window,
 * PS3_OVERLAY=0, PS3_SAVEDATA_DIALOG=0, PS3_SAVEDATA_LEGACY=1) the List calls
 * pick by themselves, as before.
 *
 * One weak object, as in ps3_pad_icons.h, so cellSaveData.c and its unit tests
 * build without the overlay.
 */
#ifndef PS3EMU_PS3_SAVEDLG_H
#define PS3EMU_PS3_SAVEDLG_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_SAVEDLG_SHARED __declspec(selectany)
#else
#  define PS3_SAVEDLG_SHARED __attribute__((weak))
#endif

typedef struct ps3_savedlg_entry {
    char dir[32];               /* the save directory ("BCES01143_INFAMOUS2_1") */
    char title[128];            /* PARAM.SFO TITLE; the new entry: the title's own text for it */
    char subtitle[128];         /* SUB_TITLE ("5. Karma's a Bitch") */
    char detail[1024];          /* DETAIL, lines split by '\n' */
    long long mtime;            /* last saved, seconds since 1970 (0 = unknown) */
    int is_new;                 /* the "New Save Data" entry (ListSave2 newData) */
    const unsigned char* icon;  /* ICON0.PNG bytes (the new entry: newData's icon), NULL = none */
    unsigned icon_size;
} ps3_savedlg_entry;

typedef struct ps3_savedlg_request {
    int is_save;                        /* 1 ListSave, 0 ListLoad */
    int n;
    const ps3_savedlg_entry* entries;   /* in the order shown; valid during the call */
    int focus;                          /* the entry the cursor starts on */
    const char* message;                /* a line to show (e.g. a failed delete), NULL = none */
} ps3_savedlg_request;

enum {
    PS3_SAVEDLG_PICK   = 0,   /* entries[index] chosen (a save over an existing one was confirmed) */
    PS3_SAVEDLG_CANCEL = 1,   /* Circle / Esc: CELL_SAVEDATA_RET_CANCEL */
    PS3_SAVEDLG_DELETE = 2    /* delete entries[index] (confirmed); the dialog stays up: the caller
                                 deletes it and calls show again with the new list (or close) */
};

typedef struct ps3_savedlg_provider {
    /* Shows the list and blocks until the player acts. 1 = *action / *index set;
     * 0 = it cannot be shown (the caller picks by itself). Any thread. */
    int  (*show)(const ps3_savedlg_request* req, int* action, int* index);
    /* After a DELETE the caller does not show the list again: take it down. */
    void (*close)(void);
} ps3_savedlg_provider;

PS3_SAVEDLG_SHARED ps3_savedlg_provider g_ps3_savedlg = { 0, 0 };

#ifdef __cplusplus
}
#endif

#endif
