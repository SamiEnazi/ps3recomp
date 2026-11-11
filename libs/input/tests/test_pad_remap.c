/* The in-game menu's rebinding (pad_remap.h) and the writer it saves through
 * (pad_input_map.h: pim_render, pim_save_file):
 *   - the player's real file (docs/input.example.toml, the one play.ps1 makes)
 *     parsed, changed, written and parsed again: every comment and other
 *     section kept line for line, CRLF kept, the map read back the same;
 *   - a sparse hand-written file: missing controls added in place, family
 *     sections and unknown lines kept;
 *   - conflicts and Swap, Enter = keypad Enter, no double binding after any
 *     sequence of changes, Reset, the key cells, what a press is, the keys the
 *     window keeps, every bindable key readable by cellPad's Windows reader;
 *   - the save: temporary file moved over the old one, nothing left behind,
 *     a file that cannot be replaced stays as it was.
 *
 *   cc -std=gnu17 -Wall -Wextra libs/input/tests/test_pad_remap.c -o /tmp/test_pad_remap
 *   /tmp/test_pad_remap [docs/input.example.toml] [libs/input/cellPad.c]
 * (run from the toolkit's root; MinGW's gcc on Windows.)
 */
#pragma GCC diagnostic ignored "-Wunused-function"   /* the header's other helpers */
#include "../pad_remap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char* read_all(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = (char*)malloc((size_t)n + 1);
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) { free(t); t = NULL; }
    if (t) t[n] = 0;
    fclose(f);
    return t;
}
static int write_all(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const size_t n = strlen(text);
    const int ok = fwrite(text, 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}
static int key_on(const PadInputMap* m, int c, unsigned vk) { return (m->key[c][vk >> 5] >> (vk & 31)) & 1; }
static int n_keys(const PadInputMap* m, int c) { int n = 0; for (unsigned v = 0; v < 128; v++) n += key_on(m, c, v); return n; }
static PrmInput K(unsigned vk) { PrmInput i = { PRM_KEY, vk }; return i; }
static PrmInput MB(uint32_t bit) { PrmInput i = { PRM_MOUSE, bit }; return i; }
static PrmInput PB(uint32_t n) { PrmInput i = { PRM_PAD, n }; return i; }

/* No key, mouse input or controller input on two controls (Enter twins as one). */
static int no_double_bindings(const PadInputMap* m)
{
    for (unsigned vk = 0; vk < 128; vk++) {
        int n = 0;
        for (int c = 0; c < PIM_N_CONTROLS; c++) n += prm_has(m, c, K(vk));
        if (n > 1) return 0;
    }
    for (int b = 0; b < 32; b++) {
        int nm = 0, np = 0;
        for (int c = 0; c < PIM_N_CONTROLS; c++) { nm += (m->mouse[c] >> b) & 1u; np += (m->pad[c] >> b) & 1u; }
        if (nm > 1 || np > 1) return 0;
    }
    return 1;
}

/* Next line of *p: [*s, *e) without its end; 0 at the end of the text. */
static int next_line(const char** p, const char** s, const char** e)
{
    if (!**p) return 0;
    *s = *p;
    const char* nl = strchr(*p, '\n');
    *e = nl ? nl : *p + strlen(*p);
    *p = nl ? nl + 1 : *e;
    if (*e > *s && (*e)[-1] == '\r') (*e)--;
    return 1;
}
static int line_is(const char* s, const char* e, const char* want) { return (size_t)(e - s) == strlen(want) && !strncmp(s, want, (size_t)(e - s)); }

/* The player's file: change three bindings, write, compare line by line. */
static void test_real_file(const char* path)
{
    char* old = read_all(path);
    CHECK(old != NULL);
    if (!old) { fprintf(stderr, "  (cannot read %s: run from the toolkit's root)\n", path); return; }
    PadInputMap m, d;
    pim_defaults(&m);
    pim_defaults(&d);
    CHECK(pim_parse(&m, old, stderr, path) == 0);
    CHECK(prm_bindings_equal(&m, &d));                      /* the example = the defaults */
    CHECK(no_double_bindings(&m));

    CHECK(prm_owners(&m, PIM_CROSS, K(0x05)) == 0);         /* G: free */
    CHECK(prm_assign(&m, PIM_CROSS, K(0x05), 0x31) == 0);   /* Cross: Space -> G */
    CHECK(prm_owners(&m, PIM_R1, MB(4)) == 1u << PIM_R3);   /* middle mouse is R3's */
    CHECK(prm_assign(&m, PIM_R1, MB(4), -1) == 1u << PIM_R3);   /* Swap: R3 gets R1's Left */
    CHECK(m.mouse[PIM_R1] == 4 && m.mouse[PIM_R3] == 1);
    CHECK(no_double_bindings(&m));

    char* text = pim_render(&m, old);
    CHECK(text != NULL);
    if (!text) { free(old); return; }
    /* same lines, in order: only the three binding lines differ */
    const char *po = old, *pn = text, *os, *oe, *ns, *ne;
    int lines = 0, differ = 0, sec = -1;
    for (;;) {
        const int a = next_line(&po, &os, &oe), b = next_line(&pn, &ns, &ne);
        CHECK(a == b);
        if (!a || !b) break;
        lines++;
        if (os < oe && *os == '[') sec = (int)(oe - os);   /* (just "in some section") */
        if ((size_t)(oe - os) != (size_t)(ne - ns) || strncmp(os, ns, (size_t)(oe - os))) {
            differ++;
            CHECK(line_is(ns, ne, "cross    = \"G\"") || line_is(ns, ne, "r1       = \"Middle\"") ||
                  line_is(ns, ne, "r3       = \"Left\""));
        }
    }
    (void)sec;
    CHECK(lines > 100 && differ == 3);
    CHECK(strstr(text, "# input.example.toml -- every built-in binding") == text);   /* the first comment */
    CHECK(strstr(text, "[mouse]\n# camera      = true     # the mouse turns the camera (right stick)\n"));
    CHECK(strstr(text, "# [controller.xbox]          # lines for one family only"));
    PadInputMap back;
    pim_defaults(&back);
    CHECK(pim_parse(&back, text, stderr, "rendered") == 0);
    CHECK(prm_bindings_equal(&m, &back));
    char* again = pim_render(&back, text);
    CHECK(again && !strcmp(again, text));                  /* saving again changes nothing */
    free(again);

    /* the same file with Windows line ends: written back with them */
    size_t n = strlen(old), k = 0;
    char* crlf = (char*)malloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++) { if (old[i] == '\n') crlf[k++] = '\r'; crlf[k++] = old[i]; }
    crlf[k] = 0;
    char* t2 = pim_render(&m, crlf);
    CHECK(t2 != NULL);
    if (t2) {
        int bare = 0;
        for (size_t i = 0; t2[i]; i++) if (t2[i] == '\n' && (i == 0 || t2[i - 1] != '\r')) bare++;
        CHECK(bare == 0);
        size_t j = 0;
        for (size_t i = 0; t2[i]; i++) if (t2[i] != '\r') t2[j++] = t2[i];
        t2[j] = 0;
        CHECK(!strcmp(t2, text));
    }
    free(t2); free(crlf); free(text); free(old);
}

/* A short hand-written file: the rest added in place, odd lines kept. */
static void test_sparse_file(void)
{
    const char* old =
        "# my controls\n"
        "[keyboard]\n"
        "cross = 'Q'   # jump on Q\n"
        "dpad_up = \"T\"\n"
        "unknown_thing = \"x\"\n"
        "\n"
        "# camera\n"
        "[mouse]\n"
        "sensitivity = 2.0\n"
        "[controller.xbox]\n"
        "select = \"back\"";                        /* no line end at the end */
    PadInputMap m;
    pim_defaults(&m);
    CHECK(pim_parse(&m, old, NULL, "sparse") == 1);         /* unknown_thing */
    CHECK(key_on(&m, PIM_CROSS, 0x0C) && n_keys(&m, PIM_TRIANGLE) == 0);
    CHECK(n_keys(&m, PIM_UP) == 1 && key_on(&m, PIM_UP, 0x11));
    /* the menu: Circle's Shift cell -> Q, which Cross has: Swap gives Cross the Shift */
    CHECK(prm_assign(&m, PIM_CIRCLE, K(0x0C), 0x38) == 1u << PIM_CROSS);
    CHECK(n_keys(&m, PIM_CIRCLE) == 1 && key_on(&m, PIM_CIRCLE, 0x0C));
    CHECK(n_keys(&m, PIM_CROSS) == 1 && key_on(&m, PIM_CROSS, 0x38));
    char* text = pim_render(&m, old);
    CHECK(text != NULL);
    if (!text) return;
    CHECK(strstr(text, "# my controls\n[keyboard]\ncross = \"Shift\"   # jump on Q\ndpad_up = \"T\"\n"
                       "unknown_thing = \"x\"\ncircle   = \"Q\"\nsquare   = \"F\"\n") == text);
    CHECK(strstr(text, "\nrs_right = \"L\"\n\n# camera\n[mouse]\nsensitivity = 2.0\n[controller.xbox]\n"
                       "select = \"back\"\n\n[mouse_buttons]\ncross    = \"\"\n"));
    CHECK(strstr(text, "\n[controller]\ncross    = \"a\"\n"));
    { const char* kb = strstr(text, "[keyboard]");
      const char* cam = strstr(text, "# camera");
      const char* up = strstr(text, "\nup       =");
      CHECK(kb && cam && up && up > cam); }               /* dpad_up is D-pad up: [keyboard] gets no second line */
    PadInputMap back;
    pim_defaults(&back);
    CHECK(pim_parse(&back, text, NULL, "rendered") == 1);
    CHECK(prm_bindings_equal(&m, &back));
    CHECK(back.mouse_sens == 2.0f);
    CHECK((back.pad_fam_set[PIM_FAM_XBOX] >> PIM_SELECT & 1u) && back.pad_fam[PIM_FAM_XBOX][PIM_SELECT] == 1u << 4);
    char* again = pim_render(&back, text);
    CHECK(again && !strcmp(again, text));
    free(again); free(text);

    /* no file yet: a header and the three sections */
    PadInputMap d;
    pim_defaults(&d);
    char* fresh = pim_render(&d, NULL);
    CHECK(fresh && strstr(fresh, "# Controls, read by the game") == fresh);
    CHECK(fresh && strstr(fresh, "\n\n[keyboard]\ncross    = \"Space\"\n"));
    CHECK(fresh && strstr(fresh, "\n\n[mouse_buttons]\n") && strstr(fresh, "\n\n[controller]\n"));
    pim_defaults(&back);
    CHECK(fresh && pim_parse(&back, fresh, NULL, "fresh") == 0 && prm_bindings_equal(&back, &d));
    char* blank = pim_render(&d, "  \r\n\n");
    CHECK(blank && fresh && !strcmp(blank, fresh));        /* a blank file = no file */
    free(blank); free(fresh);
}

static void test_conflicts(void)
{
    PadInputMap m, d;
    pim_defaults(&d);
    /* a key another control has, put in a cell that had a key: Swap */
    m = d;
    CHECK(prm_owners(&m, PIM_CROSS, K(0x0E)) == 1u << PIM_R1);
    CHECK(prm_assign(&m, PIM_CROSS, K(0x0E), 0x31) == 1u << PIM_R1);
    CHECK(n_keys(&m, PIM_CROSS) == 1 && key_on(&m, PIM_CROSS, 0x0E));
    CHECK(n_keys(&m, PIM_R1) == 1 && key_on(&m, PIM_R1, 0x31));
    /* ... into an empty cell: the other control just loses it */
    m = d;
    CHECK(prm_assign(&m, PIM_CROSS, K(0x0E), -1) == 1u << PIM_R1);
    CHECK(n_keys(&m, PIM_CROSS) == 2 && key_on(&m, PIM_CROSS, 0x31) && key_on(&m, PIM_CROSS, 0x0E));
    CHECK(n_keys(&m, PIM_R1) == 0 && m.mouse[PIM_R1] == 1);   /* its mouse button stays */
    /* the same key again: nothing to take */
    CHECK(prm_has(&m, PIM_CROSS, K(0x0E)) && prm_owners(&m, PIM_CROSS, K(0x0E)) == 0);

    /* Enter and keypad Enter are one key */
    m = d;
    CHECK(prm_owners(&m, PIM_CROSS, K(PRM_KEYPAD_ENTER)) == 1u << PIM_START);
    CHECK(prm_owners(&m, PIM_CROSS, K(PRM_ENTER)) == 1u << PIM_START);
    CHECK(prm_assign(&m, PIM_CROSS, K(PRM_KEYPAD_ENTER), 0x31) == 1u << PIM_START);
    CHECK(key_on(&m, PIM_CROSS, 0x24) && key_on(&m, PIM_CROSS, 0x4C) && n_keys(&m, PIM_CROSS) == 2);
    CHECK(n_keys(&m, PIM_START) == 1 && key_on(&m, PIM_START, 0x31));
    { char list[128]; pim_inputs_text(&m, PIM_SEC_KEYBOARD, PIM_CROSS, list, sizeof list); CHECK(!strcmp(list, "Return, KeypadEnter")); }
    prm_clear(&m, PIM_CROSS, PRM_KEY, 0x24);
    CHECK(n_keys(&m, PIM_CROSS) == 0);
    /* a file that split them: either one counts as Enter */
    m = d;
    CHECK(pim_parse(&m, "[keyboard]\nstart = \"KeypadEnter\"\nselect = \"Return\"\n", NULL, "split") == 0);
    CHECK(prm_owners(&m, PIM_CROSS, K(0x24)) == ((1u << PIM_START) | (1u << PIM_SELECT)));
    CHECK(prm_assign(&m, PIM_CROSS, K(0x24), -1) == ((1u << PIM_START) | (1u << PIM_SELECT)));
    CHECK(n_keys(&m, PIM_START) == 0 && n_keys(&m, PIM_SELECT) == 0);

    /* mouse: R2 <- Left (R1's): R1 gets R2's button 4 */
    m = d;
    CHECK(prm_assign(&m, PIM_R2, MB(1), -1) == 1u << PIM_R1);
    CHECK(m.mouse[PIM_R2] == 1 && m.mouse[PIM_R1] == 8);
    /* a free input replaces the column: L2's button 5 goes */
    CHECK(prm_assign(&m, PIM_L2, MB(PIM_MOUSE_WHEEL_DOWN), -1) == 0);
    CHECK(m.mouse[PIM_L2] == PIM_MOUSE_WHEEL_DOWN);
    /* controller: SELECT <- Triangle's y: Triangle gets Share + touchpad */
    m = d;
    CHECK(prm_assign(&m, PIM_SELECT, PB(3), -1) == 1u << PIM_TRIANGLE);
    CHECK(m.pad[PIM_SELECT] == 1u << 3 && m.pad[PIM_TRIANGLE] == ((1u << 4) | (1u << 20)));
    CHECK(prm_assign(&m, PIM_CROSS, PB(PIM_PAD_TRIGGER_LEFT), -1) == 1u << PIM_L2);
    CHECK(m.pad[PIM_CROSS] == 1u << PIM_PAD_TRIGGER_LEFT && m.pad[PIM_L2] == 1u << 0);
    CHECK(no_double_bindings(&m));

    /* any sequence of menu changes leaves no input on two controls */
    m = d;
    unsigned seed = 12345;
    int order[PIM_N_CONTROLS][2];
    for (int c = 0; c < PIM_N_CONTROLS; c++) order[c][0] = order[c][1] = -1;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1103515245u + 12345u;
        const int ctl = (int)((seed >> 16) % PIM_N_CONTROLS);
        const int kind = (int)((seed >> 8) % 3);
        seed = seed * 1103515245u + 12345u;
        PrmInput in;
        if (kind == PRM_KEY) in = K((seed >> 16) % 128);
        else if (kind == PRM_MOUSE) { static const uint32_t mb[] = { 1, 2, 4, 8, 16, PIM_MOUSE_WHEEL_UP, PIM_MOUSE_WHEEL_DOWN }; in = MB(mb[(seed >> 16) % 7]); }
        else in = PB((seed >> 16) % 23);
        int slots[2];
        prm_key_slots(&m, ctl, order[ctl], slots);
        const int cell = (int)((seed >> 4) & 1);
        if (prm_has(&m, ctl, in)) continue;                 /* the menu does nothing then */
        if ((seed >> 5) % 7 == 0) prm_clear(&m, ctl, kind, slots[cell]);
        else prm_assign(&m, ctl, in, kind == PRM_KEY ? slots[cell] : -1);
        if (!no_double_bindings(&m)) { CHECK(!"a double binding"); break; }
    }

    /* Reset: every binding back, the settings kept */
    m.mouse_sens = 1.5f;
    prm_reset(&m);
    CHECK(prm_bindings_equal(&m, &d) && m.mouse_sens == 1.5f);
}

static void test_slots(void)
{
    PadInputMap m;
    pim_defaults(&m);
    int s[2];
    const int none[2] = { -1, -1 };
    CHECK(prm_key_slots(&m, PIM_UP, none, s) == 0 && s[0] == 0x12 && s[1] == 0x7E);   /* 1, Up arrow */
    const int up_first[2] = { 0x7E, -1 };
    CHECK(prm_key_slots(&m, PIM_UP, up_first, s) == 0 && s[0] == 0x7E && s[1] == 0x12);   /* a key keeps its cell */
    CHECK(prm_key_slots(&m, PIM_START, none, s) == 0 && s[0] == 0x24 && s[1] == -1);     /* both Enters: one cell */
    CHECK(prm_key_slots(&m, PIM_L3, none, s) == 0 && s[0] == 0x09 && s[1] == -1);
    prm_key_put(&m, PIM_CROSS, 0x00, 1);
    prm_key_put(&m, PIM_CROSS, 0x0B, 1);
    CHECK(prm_key_slots(&m, PIM_CROSS, none, s) == 1 && s[0] == 0x00 && s[1] == 0x0B);  /* A, B shown, +1 (Space) */
    /* the first cell cleared: the second stays where it is */
    prm_clear(&m, PIM_UP, PRM_KEY, 0x12);
    const int was[2] = { -1, 0x7E };
    CHECK(prm_key_slots(&m, PIM_UP, was, s) == 0 && s[0] == -1 && s[1] == 0x7E);
    CHECK(prm_key_slots(&m, PIM_UP, none, s) == 0 && s[0] == 0x7E && s[1] == -1);
    PadInputMap e;
    pim_defaults(&e);
    prm_clear_keys(&e, PIM_CROSS);
    CHECK(prm_key_slots(&e, PIM_CROSS, none, s) == 0 && s[0] == -1 && s[1] == -1);
}

/* The macOS key codes cellPad's Windows reader sees: the second numbers of
 * kWinScanMac and kWinVkMac in cellPad.c, plus keypad Enter (read from
 * VK_RETURN). */
static int readable[128];
static int load_readable(const char* cellpad)
{
    char* src = read_all(cellpad);
    if (!src) return 0;
    int n = 0;
    static const char* const tables[] = { "kWinScanMac[][2] = {", "kWinVkMac[][2] = {" };
    for (int t = 0; t < 2; t++) {
        const char* p = strstr(src, tables[t]);
        if (!p) continue;
        p += strlen(tables[t]);
        const char* end = strstr(p, "};");
        while (p && p < end) {
            const char* o = strchr(p, '{');
            if (!o || o > end) break;
            const char* comma = strchr(o, ',');
            const char* close = strchr(o, '}');
            if (!comma || !close || comma > close) break;
            const long v = strtol(comma + 1, NULL, 0);
            if (v >= 0 && v < 128) { if (!readable[v]) n++; readable[v] = 1; }
            p = close + 1;
        }
    }
    readable[PRM_KEYPAD_ENTER] = 1;
    free(src);
    return n;
}

static void test_capture(const char* cellpad)
{
    PrmInput in;
    const char* why;
    CHECK(prm_capture_key(26, 0, &in, &why) == PRM_CAP_INPUT && in.kind == PRM_KEY && in.code == 0x0D);   /* W */
    CHECK(prm_capture_key(40, 0, &in, &why) == PRM_CAP_INPUT && in.code == 0x24);   /* Return */
    CHECK(prm_capture_key(88, 0, &in, &why) == PRM_CAP_INPUT && in.code == 0x24);   /* keypad Enter = Enter */
    CHECK(prm_capture_key(225, 0, &in, &why) == PRM_CAP_INPUT && in.code == 0x38);  /* left Shift */
    CHECK(prm_capture_key(108, 0, &in, &why) == PRM_CAP_INPUT && in.code == 0x40);  /* F17 */
    CHECK(prm_capture_key(41, 0, &in, &why) == PRM_CAP_CANCEL);                     /* Esc */
    CHECK(prm_capture_key(41, 1, &in, &why) == PRM_CAP_CANCEL);
    static const struct { int sc; const char* word; } kept[] = {
        { 58, "F1" }, { 59, "F2" }, { 60, "F3" }, { 64, "F7" }, { 66, "F9" }, { 68, "F11" }, { 70, "Print Screen" },
        { 226, "Alt" }, { 230, "Alt" }, { 227, "Windows" }, { 231, "Windows" },
    };
    for (unsigned i = 0; i < sizeof kept / sizeof kept[0]; i++)
        CHECK(prm_capture_key(kept[i].sc, 0, &in, &why) == PRM_CAP_RESERVED && why && strstr(why, kept[i].word));
    CHECK(prm_capture_key(40, 1, &in, &why) == PRM_CAP_RESERVED && strstr(why, "Alt+Enter"));   /* Alt+Enter */
    CHECK(prm_capture_key(4, 1, &in, &why) == PRM_CAP_RESERVED);                                /* Alt+A */
    CHECK(prm_capture_key(112, 0, &in, &why) == PRM_CAP_UNKNOWN && why);            /* F21 */
    CHECK(prm_capture_key(0, 0, &in, &why) == PRM_CAP_UNKNOWN);

    /* every bindable key: a Windows name, a file name that reads back, and cellPad reads it */
    const int nread = load_readable(cellpad);
    CHECK(nread > 90);
    if (!nread) fprintf(stderr, "  (cannot read %s: run from the toolkit's root)\n", cellpad);
    int bindable = 0;
    for (int sc = 0; sc < 256; sc++) {
        if (prm_capture_key(sc, 0, &in, &why) != PRM_CAP_INPUT) continue;
        bindable++;
        char buf[24], low[32], tmp[8];
        const char* nm = prm_key_name(in.code, buf, sizeof buf);
        CHECK(strncmp(nm, "Key 0x", 6) != 0);
        snprintf(low, sizeof low, "%s", pim_key_label(in.code, tmp, sizeof tmp));
        pim_lower(low);
        CHECK(pim_parse_key_name(low) == (int)in.code);
        if (nread && !readable[in.code]) { fprintf(stderr, "  scancode %d -> 0x%02X not read by cellPad\n", sc, in.code); CHECK(readable[in.code]); }
    }
    CHECK(bindable > 95);

    /* mouse, wheel, controller */
    CHECK(prm_mouse_from_sdl(1) == 1 && prm_mouse_from_sdl(2) == 4 && prm_mouse_from_sdl(3) == 2);
    CHECK(prm_mouse_from_sdl(4) == 8 && prm_mouse_from_sdl(5) == 16 && prm_mouse_from_sdl(6) == 0);
    CHECK(prm_mouse_from_wheel(1) == PIM_MOUSE_WHEEL_UP && prm_mouse_from_wheel(-2) == PIM_MOUSE_WHEEL_DOWN && !prm_mouse_from_wheel(0));
    CHECK(prm_pad_from_raw(1u << 5) == -1);                 /* PS / Home */
    CHECK(prm_pad_from_raw(1u << 23) == -1);                /* a stick half-axis */
    CHECK(prm_pad_from_raw(1u << PIM_PAD_TRIGGER_RIGHT) == PIM_PAD_TRIGGER_RIGHT);
    CHECK(prm_pad_from_raw((1u << 3) | (1u << 1)) == 1);
    /* names */
    char b[24];
    CHECK(!strcmp(prm_key_name(0x24, b, sizeof b), "Enter") && !strcmp(prm_key_name(0x4C, b, sizeof b), "Enter"));
    CHECK(!strcmp(prm_key_name(0x38, b, sizeof b), "Left Shift") && !strcmp(prm_key_name(0x75, b, sizeof b), "Delete"));
    CHECK(!strcmp(prm_mouse_name(8, b, sizeof b), "Mouse 4") && !strcmp(prm_mouse_name(2, b, sizeof b), "Right mouse"));
    CHECK(!strcmp(prm_pad_name(0), "Cross") && !strcmp(prm_pad_name(PIM_PAD_TRIGGER_LEFT), "L2"));
    for (int i = 0; i < PIM_N_CONTROLS; i++) CHECK(prm_row_of(i)->ctl == i && prm_row_of(i)->does[0]);
    /* the timed script's inputs */
    CHECK(prm_parse_input("key:F17", &in) && in.kind == PRM_KEY && in.code == 0x40);
    CHECK(prm_parse_input("F17", &in) && in.kind == PRM_KEY && in.code == 0x40);
    CHECK(prm_parse_input("Mouse:Right", &in) && in.kind == PRM_MOUSE && in.code == 2);
    CHECK(prm_parse_input("pad:triangle", &in) && in.kind == PRM_PAD && in.code == 3);
    CHECK(!prm_parse_input("key:hyper", &in) && !prm_parse_input("mouse:button9", &in));
}

/* pim_save_file: through <file>.tmp, moved over the file in one step. */
static void test_save(const char* example)
{
    const char* tmpdir = getenv("TEMP");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    char path[600], tmp[620];
    snprintf(path, sizeof path, "%s/pim_remap_test_%d.toml", tmpdir, (int)(size_t)&fails % 100000);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    char* old = read_all(example);
    CHECK(old != NULL);
    if (!old) return;
    CHECK(write_all(path, old));
    PadInputMap m;
    CHECK(pim_load_file(&m, path, stderr) == 1);
    prm_assign(&m, PIM_SELECT, K(0x40), 0x2E);              /* SELECT: M -> F17 */
    CHECK(pim_save_file(&m, path) == 1);
    char* saved = read_all(path);
    char* want = pim_render(&m, old);
    CHECK(saved && want && !strcmp(saved, want));
    CHECK(saved && strstr(saved, "\nselect   = \"Tab, F17\"\n"));
    { FILE* f = fopen(tmp, "rb"); CHECK(f == NULL); if (f) fclose(f); }   /* nothing left behind */
    PadInputMap back;
    CHECK(pim_load_file(&back, path, stderr) == 1 && prm_bindings_equal(&back, &m) && back.n_warnings == 0);
    CHECK(pim_save_file(&back, path) == 1);                 /* again: the same bytes */
    char* saved2 = read_all(path);
    CHECK(saved && saved2 && !strcmp(saved, saved2));
    /* a file that cannot be replaced: 0, and it stays whole */
    chmod(path, S_IREAD);
    prm_reset(&back);
    const int r = pim_save_file(&back, path);
    char* saved3 = read_all(path);
    chmod(path, S_IREAD | S_IWRITE);
#ifdef _WIN32
    CHECK(r == 0 && saved3 && saved && !strcmp(saved3, saved));
    { FILE* f = fopen(tmp, "rb"); CHECK(f == NULL); if (f) fclose(f); }
#else
    (void)r;   /* POSIX: rename() replaces a read-only file in a writable folder (fine) */
#endif
    remove(path);
    remove(tmp);
    free(old); free(saved); free(want); free(saved2); free(saved3);
}

int main(int argc, char** argv)
{
    const char* example = argc > 1 ? argv[1] : "docs/input.example.toml";
    const char* cellpad = argc > 2 ? argv[2] : "libs/input/cellPad.c";
    test_real_file(example);
    test_sparse_file();
    test_conflicts();
    test_slots();
    test_capture(cellpad);
    test_save(example);
    if (fails) { printf("test_pad_remap: %d failure(s)\n", fails); return 1; }
    printf("test_pad_remap: ok\n");
    return 0;
}
