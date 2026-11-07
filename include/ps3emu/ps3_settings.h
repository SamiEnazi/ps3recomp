/*
 * ps3_settings -- live setters for the PC settings the in-game menu changes
 * (libs/video/vk_overlay.cpp, the Vulkan backend's overlay).
 *
 * Every setting is an environment variable, with the names and values the
 * launcher (tools/play.ps1) and the menu's settings.ini use. A module that
 * can take a new value while the game runs registers an applier for its
 * variable, once, e.g. at init:
 *
 *     static int aa_apply(const char* value) { s_aa_mode = parse(value); return 1; }
 *     ...
 *     ps3_setting_register("PS3RECOMP_AA", aa_apply);
 *
 * The menu calls ps3_setting_apply(name, value): the variable is set in the
 * environment first (so a getenv() after it sees the new value), then the
 * applier runs, on the menu's thread (the Vulkan backend's RSX/present
 * thread). Keep it short -- store a value, set a dirty flag that the hot path
 * checks -- and return 1 when the value took effect now, 0 when it still
 * needs a restart. A variable nobody registered applies at the next start
 * (the menu says so next to it).
 *
 * Names are stored by pointer: pass string literals. Registering the same
 * name again replaces its applier.
 *
 * The input block while the menu is open is cellPad's own flag,
 * g_pad_overlay_open (libs/input/cellPad.c): while it is non-zero every pad
 * port reads neutral (connected, nothing pressed), which is what the macOS
 * menu (ps3_overlay.mm) has always used.
 *
 * Header-only state (one shared object, as in ps3_toggles.h), so a module
 * that includes this needs no new link dependency.
 */
#ifndef PS3EMU_PS3_SETTINGS_H
#define PS3EMU_PS3_SETTINGS_H

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_SETTINGS_SHARED __declspec(selectany)
#else
#  define PS3_SETTINGS_SHARED __attribute__((weak))
#endif

typedef int (*ps3_setting_fn)(const char* value);

enum { PS3_SETTINGS_MAX = 48 };

typedef struct {
    char lock;
    int  n;
    struct { const char* name; ps3_setting_fn fn; } e[PS3_SETTINGS_MAX];
} ps3_settings_state;

PS3_SETTINGS_SHARED ps3_settings_state g_ps3_settings = { 0 };

static inline void ps3_settings_lock(void)
{
    while (__atomic_test_and_set(&g_ps3_settings.lock, __ATOMIC_ACQUIRE)) { }
}
static inline void ps3_settings_unlock(void)
{
    __atomic_clear(&g_ps3_settings.lock, __ATOMIC_RELEASE);
}

/* `name` must outlive the program (a string literal). */
static inline void ps3_setting_register(const char* name, ps3_setting_fn fn)
{
    ps3_settings_lock();
    int i = 0;
    for (; i < g_ps3_settings.n; i++)
        if (!strcmp(g_ps3_settings.e[i].name, name)) break;
    if (i < PS3_SETTINGS_MAX) {
        g_ps3_settings.e[i].name = name;
        g_ps3_settings.e[i].fn = fn;
        if (i == g_ps3_settings.n) g_ps3_settings.n++;
    }
    ps3_settings_unlock();
}

static inline ps3_setting_fn ps3_setting_applier(const char* name)
{
    ps3_setting_fn fn = 0;
    ps3_settings_lock();
    for (int i = 0; i < g_ps3_settings.n; i++)
        if (!strcmp(g_ps3_settings.e[i].name, name)) { fn = g_ps3_settings.e[i].fn; break; }
    ps3_settings_unlock();
    return fn;
}

/* Set (value non-empty) or clear (NULL or "") an environment variable. */
static inline void ps3_setting_setenv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");   /* "" removes it */
#else
    if (value && *value) setenv(name, value, 1); else unsetenv(name);
#endif
}

/* The environment first, then the module's applier. 1 = live now, 0 = at
 * the next start. */
static inline int ps3_setting_apply(const char* name, const char* value)
{
    ps3_setting_setenv(name, value);
    const ps3_setting_fn fn = ps3_setting_applier(name);
    return fn ? fn(value ? value : "") : 0;
}

#ifdef __cplusplus
}
#endif

#endif
