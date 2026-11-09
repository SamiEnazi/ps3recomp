/*
 * ps3_cheats -- the cheats a title offers, for the in-game menu's Cheats page
 * (libs/video/vk_overlay_cheats.cpp). The toolkit knows nothing about any game:
 * the title's own code (kept with the title) registers its cheats, once, e.g.
 * from a static constructor, and does the work in their apply callbacks (usually
 * with include/ps3emu/ppu_hooks.h):
 *
 *     static int god_apply(ps3_cheat* c) { ppu_hook_set(ADDR, c->value ? hook : NULL); return 1; }
 *     static ps3_cheat s_god = { "god", "God mode", "Health never drops.", "Player",
 *                                PS3_CHEAT_TOGGLE, god_apply };
 *     ps3_cheat_register(&s_god);
 *
 * Kinds: TOGGLE (value 0/1), CHOICE (value = index into choices[]), INT / FLOAT
 * (a slider from min to max), ACTION (a button; with max > min it has an amount
 * slider beside it, the amount in value, and `button` is its label). The menu
 * writes `value` and then calls apply(c) on its own thread (the window's): keep it
 * short -- store a flag, set a hook -- and return 1 (0 = it could not apply: the
 * menu says so). An ACTION's apply is the press.
 *
 * Everything starts at its declared value (cheats off). With the menu's "Remember
 * cheats" (default on) the toggles, choices and sliders that are on are kept in its
 * settings file and set again when the window opens (ps3_cheat_rememberable; ACTIONs
 * and PS3_CHEAT_NO_REMEMBER never). PS3_CHEAT_SAVES marks a cheat whose effect the
 * game writes into its own save (XP, unlocks); the page then shows the title's note
 * (ps3_cheats_set_note) or a default warning.
 *
 * Tests: PS3_CHEATS="id=value,id=value" applies values once the title calls
 * ps3_cheats_env() (an ACTION fires with that amount).
 *
 * Header-only shared state (as in ps3_settings.h): no new link dependency.
 */
#ifndef PS3EMU_PS3_CHEATS_H
#define PS3EMU_PS3_CHEATS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_CHEATS_SHARED __declspec(selectany)
#else
#  define PS3_CHEATS_SHARED __attribute__((weak))
#endif

enum { PS3_CHEAT_TOGGLE, PS3_CHEAT_CHOICE, PS3_CHEAT_INT, PS3_CHEAT_FLOAT, PS3_CHEAT_ACTION };
/* flags: SAVES = the game writes its effect into the save; NO_REMEMBER = never kept in the
 * settings file (ACTIONs never are: re-running them at every start would repeat them) */
enum { PS3_CHEAT_SAVES = 1, PS3_CHEAT_NO_REMEMBER = 2 };

typedef struct ps3_cheat ps3_cheat;
struct ps3_cheat {
    const char* id;              /* short and stable ("god"): PS3_CHEATS, logs */
    const char* name;            /* the menu's label */
    const char* help;            /* a line or two under it (may be NULL) */
    const char* group;           /* the section it is listed under */
    int         kind;            /* PS3_CHEAT_* */
    int       (*apply)(ps3_cheat* c);
    double      value;           /* the current value (the start value as declared) */
    double      min, max;        /* INT / FLOAT / ACTION amount */
    const char* fmt;             /* the slider's format ("%d XP"), NULL = plain */
    const char* const* choices;  /* CHOICE */
    int         n_choices;
    const char* button;          /* ACTION: the button's label (NULL = name) */
    unsigned    flags;           /* PS3_CHEAT_SAVES */
    const char* unavailable;     /* non-NULL: shown disabled, with this reason */
};

enum { PS3_CHEATS_MAX = 48 };

typedef struct {
    char        lock;
    int         n;
    ps3_cheat*  e[PS3_CHEATS_MAX];
    const char* note;            /* the title's save warning */
    void      (*refresh)(void);  /* the title's: called before the page is drawn (update
                                    `unavailable`, values changed elsewhere) */
} ps3_cheats_state;

PS3_CHEATS_SHARED ps3_cheats_state g_ps3_cheats = { 0 };

static inline void ps3_cheats_lock(void)   { while (__atomic_test_and_set(&g_ps3_cheats.lock, __ATOMIC_ACQUIRE)) { } }
static inline void ps3_cheats_unlock(void) { __atomic_clear(&g_ps3_cheats.lock, __ATOMIC_RELEASE); }

/* `c` and its strings must outlive the program (static storage). */
static inline void ps3_cheat_register(ps3_cheat* c)
{
    ps3_cheats_lock();
    if (g_ps3_cheats.n < PS3_CHEATS_MAX) g_ps3_cheats.e[g_ps3_cheats.n++] = c;
    ps3_cheats_unlock();
}

static inline void ps3_cheats_set_note(const char* note) { g_ps3_cheats.note = note; }
static inline void ps3_cheats_set_refresh(void (*fn)(void)) { g_ps3_cheats.refresh = fn; }

static inline int ps3_cheats_count(void) { return __atomic_load_n(&g_ps3_cheats.n, __ATOMIC_ACQUIRE); }

static inline ps3_cheat* ps3_cheat_find(const char* id)
{
    for (int i = 0; i < ps3_cheats_count(); i++)
        if (!strcmp(g_ps3_cheats.e[i]->id, id)) return g_ps3_cheats.e[i];
    return NULL;
}

/* Set a cheat's value and apply it (any thread the title allows; the menu's is
 * the window's). 1 = applied. */
static inline int ps3_cheat_set(ps3_cheat* c, double v)
{
    if (!c || c->unavailable) return 0;
    if (c->kind == PS3_CHEAT_TOGGLE) v = v != 0.0;
    else if (c->kind == PS3_CHEAT_CHOICE) { int i = (int)v; v = i < 0 ? 0 : i >= c->n_choices ? c->n_choices - 1 : i; }
    else if (c->max > c->min) v = v < c->min ? c->min : v > c->max ? c->max : v;
    c->value = v;
    const int ok = c->apply ? c->apply(c) : 0;
    fprintf(stderr, "[cheats] %s = %g%s\n", c->id, v, ok ? "" : " (not applied)");
    return ok;
}

/* A cheat that is doing something: a TOGGLE/CHOICE not at 0 (off / the first choice), a
 * slider not at its bottom (a 1x..10x multiplier at 1x is off). ACTIONs never are. */
static inline int ps3_cheat_is_on(const ps3_cheat* c)
{
    if (c->kind == PS3_CHEAT_ACTION) return 0;
    if ((c->kind == PS3_CHEAT_INT || c->kind == PS3_CHEAT_FLOAT) && c->max > c->min) return c->value != c->min;
    return c->value != 0.0;
}

/* Any cheat on? For a HUD mark. */
static inline int ps3_cheats_any_on(void)
{
    for (int i = 0; i < ps3_cheats_count(); i++)
        if (ps3_cheat_is_on(g_ps3_cheats.e[i])) return 1;
    return 0;
}

/* "Remember cheats" (the in-game menu, libs/video/vk_overlay_cheats.cpp): a cheat that can be
 * remembered is kept in the menu's settings file as PS3_CHEAT_<ID>=<value> and set again when
 * the window opens (before the game reaches play: an apply callback must only switch things on,
 * never touch the game's memory before the game is ready -- the title's code does that from
 * its own hooks). */
static inline int ps3_cheat_rememberable(const ps3_cheat* c)
{
    return c->kind != PS3_CHEAT_ACTION && !(c->flags & PS3_CHEAT_NO_REMEMBER);
}
static inline void ps3_cheat_env_name(const ps3_cheat* c, char* out, size_t n)
{
    size_t k = (size_t)snprintf(out, n, "PS3_CHEAT_");
    for (const char* p = c->id; *p && k + 1 < n; p++)
        out[k++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    out[k < n ? k : n - 1] = 0;
}

/* PS3_CHEATS="id=value,..." (tests). */
static inline void ps3_cheats_env(void)
{
    const char* e = getenv("PS3_CHEATS");
    if (!e || !*e) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char* tok = strtok(buf, ",; "); tok; tok = strtok(NULL, ",; ")) {
        char* eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        ps3_cheat* c = ps3_cheat_find(tok);
        if (c) ps3_cheat_set(c, atof(eq + 1));
        else fprintf(stderr, "[cheats] PS3_CHEATS: no cheat '%s'\n", tok);
    }
}

#ifdef __cplusplus
}
#endif

#endif
