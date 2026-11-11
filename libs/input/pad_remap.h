/*
 * ps3recomp - rebinding from the in-game menu (F2 > Controls, vk_overlay_remap.inc)
 *
 * The rules the menu follows when the player changes a binding, kept apart from
 * the UI so they build into a standalone test (libs/input/tests/test_pad_remap.c):
 *
 *   - What a pressed input is: SDL's numbers in (scancodes = USB HID usage ids,
 *     SDL mouse buttons, SDL controller button bits as cellPad keeps them in
 *     g_pad_overlay_raw), PadInputMap's codes out (macOS key codes by US
 *     position, the mouse bits, the controller bit numbers of pad_input_map.h).
 *     Only keys cellPad's Windows reader (pad_win_keys) can see map to a code.
 *   - The keys the window and the menu keep: F1 (HUD), F2 (menu), F3 (lighting
 *     A/B), F7 (the Cheats page's quick key: go to the saved spot), F9 (play-log
 *     mark), F11 and Alt+Enter (fullscreen), Esc (cancels
 *     here, opens the menu in play), Alt (Alt+Tab, Alt+F4), the Windows keys and
 *     Print Screen. The PS/Xbox/Home button is the system's.
 *   - One input, one PS3 control: an input another control has is a conflict
 *     the menu asks about (Swap / Cancel). A control shows two keys, one mouse
 *     input and one controller input (the file may hold more; they are kept).
 *     Swap: the other control gets what the changed slot had (nothing to give:
 *     it just loses the input).
 *   - Return and keypad Enter are ONE key on Windows (cellPad reads both from
 *     VK_RETURN), so they bind, unbind and conflict together.
 *   - Names for the menu: Windows key names, PlayStation button names, what each
 *     control does in inFAMOUS 2.
 *
 * The map itself (bindings, file format, writer) is pad_input_map.h; this
 * header only edits a PadInputMap. No SDL, no Windows headers.
 */
#ifndef PS3RECOMP_PAD_REMAP_H
#define PS3RECOMP_PAD_REMAP_H

#include "pad_input_map.h"

enum { PRM_KEY, PRM_MOUSE, PRM_PAD };
/* KEY: a macOS key code (0-127); MOUSE: one bit of PadInputMap.mouse; PAD: a
 * bit NUMBER of PadInputMap.pad. kind < 0: nothing. */
typedef struct { int kind; uint32_t code; } PrmInput;

#define PRM_ENTER         0x24u   /* Return */
#define PRM_KEYPAD_ENTER  0x4Cu
/* Controller inputs a press can bind: the buttons and the two triggers, not
 * the stick half-axes (a resting stick must not bind) nor the PS/Home button. */
#define PRM_PAD_CAPTURE_MASK  ((((uint32_t)1 << 23) - 1u) & ~((uint32_t)1 << 5))

/* ---- the PS3 controls, in the menu's order, with what they do -------------- */
typedef struct { int ctl; const char* name; const char* does; } PrmRow;
static const PrmRow prm_rows[PIM_N_CONTROLS] = {
    { PIM_CROSS, "Cross", "Jump" },               { PIM_CIRCLE, "Circle", "Dive roll, drop" },
    { PIM_SQUARE, "Square", "Melee" },            { PIM_TRIANGLE, "Triangle", "Finisher" },
    { PIM_L1, "L1", "Aim" },                      { PIM_R1, "R1", "Bolt, use" },
    { PIM_L2, "L2", "Drain, radar pulse" },       { PIM_R2, "R2", "Kinetic pulse" },
    { PIM_L3, "L3", "Ping" },                     { PIM_R3, "R3", "Switch shoulder" },
    { PIM_START, "START", "Pause" },              { PIM_SELECT, "SELECT", "Map" },
    { PIM_UP, "D-pad up", "Precision" },          { PIM_DOWN, "D-pad down", "Ionic Vortex" },
    { PIM_LEFT, "D-pad left", "Buy powers" },     { PIM_RIGHT, "D-pad right", "Switch powers" },
    { PIM_LS_UP, "Left stick up", "Move forward" },     { PIM_LS_DOWN, "Left stick down", "Move back" },
    { PIM_LS_LEFT, "Left stick left", "Move left" },    { PIM_LS_RIGHT, "Left stick right", "Move right" },
    { PIM_RS_UP, "Right stick up", "Look up" },         { PIM_RS_DOWN, "Right stick down", "Look down" },
    { PIM_RS_LEFT, "Right stick left", "Look left" },   { PIM_RS_RIGHT, "Right stick right", "Look right" },
};
static inline const PrmRow* prm_row_of(int ctl)
{
    for (int i = 0; i < PIM_N_CONTROLS; i++) if (prm_rows[i].ctl == ctl) return &prm_rows[i];
    return &prm_rows[0];
}

/* ---- names ------------------------------------------------------------------ */
/* Windows names of the keys, by macOS key code (what the menu shows; the file
 * keeps pad_input_map.h's canonical names). */
static const PimName prm_key_names[] = {
    {"A", 0x00}, {"S", 0x01}, {"D", 0x02}, {"F", 0x03}, {"H", 0x04}, {"G", 0x05},
    {"Z", 0x06}, {"X", 0x07}, {"C", 0x08}, {"V", 0x09}, {"Left of Z", 0x0A}, {"B", 0x0B},
    {"Q", 0x0C}, {"W", 0x0D}, {"E", 0x0E}, {"R", 0x0F}, {"Y", 0x10}, {"T", 0x11},
    {"1", 0x12}, {"2", 0x13}, {"3", 0x14}, {"4", 0x15}, {"6", 0x16}, {"5", 0x17},
    {"=", 0x18}, {"9", 0x19}, {"7", 0x1A}, {"-", 0x1B}, {"8", 0x1C}, {"0", 0x1D},
    {"]", 0x1E}, {"O", 0x1F}, {"U", 0x20}, {"[", 0x21}, {"I", 0x22}, {"P", 0x23},
    {"Enter", 0x24}, {"L", 0x25}, {"J", 0x26}, {"'", 0x27}, {"K", 0x28}, {";", 0x29},
    {"\\", 0x2A}, {",", 0x2B}, {"/", 0x2C}, {"N", 0x2D}, {"M", 0x2E}, {".", 0x2F},
    {"Tab", 0x30}, {"Space", 0x31}, {"`", 0x32}, {"Backspace", 0x33}, {"Esc", 0x35},
    {"Left Shift", 0x38}, {"Caps Lock", 0x39}, {"Left Alt", 0x3A}, {"Left Ctrl", 0x3B},
    {"Right Shift", 0x3C}, {"Right Alt", 0x3D}, {"Right Ctrl", 0x3E},
    {"F17", 0x40}, {"Num .", 0x41}, {"Num *", 0x43}, {"Num +", 0x45}, {"Num Lock", 0x47},
    {"Num /", 0x4B}, {"Enter", 0x4C}, {"Num -", 0x4E}, {"F18", 0x4F}, {"F19", 0x50}, {"Num =", 0x51},
    {"Num 0", 0x52}, {"Num 1", 0x53}, {"Num 2", 0x54}, {"Num 3", 0x55}, {"Num 4", 0x56},
    {"Num 5", 0x57}, {"Num 6", 0x58}, {"Num 7", 0x59}, {"F20", 0x5A}, {"Num 8", 0x5B}, {"Num 9", 0x5C},
    {"F5", 0x60}, {"F6", 0x61}, {"F7", 0x62}, {"F3", 0x63}, {"F8", 0x64}, {"F9", 0x65},
    {"F11", 0x67}, {"F13", 0x69}, {"F16", 0x6A}, {"Scroll Lock", 0x6B}, {"F10", 0x6D},
    {"Menu key", 0x6E}, {"F12", 0x6F}, {"Pause", 0x71}, {"Insert", 0x72}, {"Home", 0x73},
    {"Page Up", 0x74}, {"Delete", 0x75}, {"F4", 0x76}, {"End", 0x77}, {"F2", 0x78},
    {"Page Down", 0x79}, {"F1", 0x7A}, {"Left arrow", 0x7B}, {"Right arrow", 0x7C},
    {"Down arrow", 0x7D}, {"Up arrow", 0x7E},
};
/* A key's name for the menu ("Left Shift", "Enter"); buf for a code without one. */
static inline const char* prm_key_name(uint32_t vk, char* buf, size_t n)
{
    for (int i = 0; i < PIM_COUNT(prm_key_names); i++)
        if (prm_key_names[i].id == (int)vk) return prm_key_names[i].name;
    snprintf(buf, n, "Key 0x%02X", (unsigned)(vk & 0x7Fu));
    return buf;
}
static inline const char* prm_mouse_name(uint32_t bit, char* buf, size_t n)
{
    switch (bit) {
    case 1u: return "Left mouse";
    case 2u: return "Right mouse";
    case 4u: return "Middle mouse";
    case PIM_MOUSE_WHEEL_UP: return "Wheel up";
    case PIM_MOUSE_WHEEL_DOWN: return "Wheel down";
    default: break;
    }
    for (int i = 3; i < 8; i++) if (bit == (uint32_t)1 << i) { snprintf(buf, n, "Mouse %d", i + 1); return buf; }
    snprintf(buf, n, "Mouse ?");
    return buf;
}
/* A controller input by position, with PlayStation names (bottom = Cross). */
static inline const char* prm_pad_name(uint32_t bit)
{
    static const char* const names[PIM_PAD_N_INPUTS] = {
        "Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1",
        "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Mic", "Paddle 1", "Paddle 2",
        "Paddle 3", "Paddle 4", "Touchpad", "L2", "R2",
        "Left stick left", "Left stick right", "Left stick up", "Left stick down",
        "Right stick left", "Right stick right", "Right stick up", "Right stick down",
    };
    return bit < PIM_PAD_N_INPUTS ? names[bit] : "?";
}
static inline const char* prm_input_name(PrmInput in, char* buf, size_t n)
{
    if (in.kind == PRM_KEY) return prm_key_name(in.code, buf, n);
    if (in.kind == PRM_MOUSE) return prm_mouse_name(in.code, buf, n);
    if (in.kind == PRM_PAD) return prm_pad_name(in.code);
    return "-";
}

/* ---- what a press is ---------------------------------------------------------- */
/* SDL scancode (USB HID usage id) -> macOS key code, for the keys cellPad's
 * Windows reader sees (scan-code keys by position, the rest by virtual key).
 * Keypad Enter is Return here: on Windows both are VK_RETURN. */
static const unsigned short prm_sdl_scan_mac[][2] = {
    {4, 0x00}, {5, 0x0B}, {6, 0x08}, {7, 0x02}, {8, 0x0E}, {9, 0x03}, {10, 0x05}, {11, 0x04},
    {12, 0x22}, {13, 0x26}, {14, 0x28}, {15, 0x25}, {16, 0x2E}, {17, 0x2D}, {18, 0x1F}, {19, 0x23},
    {20, 0x0C}, {21, 0x0F}, {22, 0x01}, {23, 0x11}, {24, 0x20}, {25, 0x09}, {26, 0x0D}, {27, 0x07},
    {28, 0x10}, {29, 0x06},                                         /* A-Z */
    {30, 0x12}, {31, 0x13}, {32, 0x14}, {33, 0x15}, {34, 0x17}, {35, 0x16}, {36, 0x1A}, {37, 0x1C},
    {38, 0x19}, {39, 0x1D},                                         /* 1-9, 0 */
    {40, 0x24}, {42, 0x33}, {43, 0x30}, {44, 0x31},                 /* Return, Backspace, Tab, Space */
    {45, 0x1B}, {46, 0x18}, {47, 0x21}, {48, 0x1E}, {49, 0x2A}, {50, 0x2A},   /* - = [ ] \ (ISO #: same key) */
    {51, 0x29}, {52, 0x27}, {53, 0x32}, {54, 0x2B}, {55, 0x2F}, {56, 0x2C},   /* ; ' ` , . / */
    {57, 0x39},                                                     /* Caps Lock */
    {61, 0x76}, {62, 0x60}, {63, 0x61}, {64, 0x62}, {65, 0x64}, {67, 0x6D}, {69, 0x6F},   /* F4-F8 F10 F12 */
    {71, 0x6B}, {72, 0x71},                                         /* Scroll Lock, Pause */
    {73, 0x72}, {74, 0x73}, {75, 0x74}, {76, 0x75}, {77, 0x77}, {78, 0x79},   /* Ins Home PgUp Del End PgDn */
    {79, 0x7C}, {80, 0x7B}, {81, 0x7D}, {82, 0x7E},                 /* arrows */
    {83, 0x47}, {84, 0x4B}, {85, 0x43}, {86, 0x4E}, {87, 0x45}, {88, 0x24},   /* Num Lock / * - + Enter */
    {89, 0x53}, {90, 0x54}, {91, 0x55}, {92, 0x56}, {93, 0x57}, {94, 0x58}, {95, 0x59}, {96, 0x5B},
    {97, 0x5C}, {98, 0x52}, {99, 0x41},                             /* keypad 1-9 0 . */
    {100, 0x0A}, {101, 0x6E},                                       /* ISO key left of Z, Menu */
    {104, 0x69}, {105, 0x6B}, {106, 0x71}, {107, 0x6A}, {108, 0x40}, {109, 0x4F}, {110, 0x50}, {111, 0x5A},   /* F13-F20 */
    {224, 0x3B}, {225, 0x38}, {228, 0x3E}, {229, 0x3C},             /* Ctrl, Shift (left, right) */
};

enum { PRM_CAP_NONE, PRM_CAP_INPUT, PRM_CAP_CANCEL, PRM_CAP_RESERVED, PRM_CAP_UNKNOWN };

/* A key pressed while the menu waits for one: PRM_CAP_INPUT (*out set),
 * _CANCEL (Esc), _RESERVED (*why says whose it is) or _UNKNOWN (the game
 * cannot read it). alt_held: Alt was down (Alt+<key> is the window's). */
static inline int prm_capture_key(int scancode, int alt_held, PrmInput* out, const char** why)
{
    *why = NULL;
    switch (scancode) {
    case 41:  return PRM_CAP_CANCEL;                                               /* Esc */
    case 58:  *why = "F1 shows and hides the performance HUD."; return PRM_CAP_RESERVED;
    case 59:  *why = "F2 opens and closes this menu."; return PRM_CAP_RESERVED;
    case 60:  *why = "F3 is the lighting comparison switch."; return PRM_CAP_RESERVED;
    case 64:  *why = "F7 is the Cheats page's quick key (go to the saved spot)."; return PRM_CAP_RESERVED;
    case 66:  *why = "F9 marks a moment in the play log."; return PRM_CAP_RESERVED;
    case 68:  *why = "F11 switches fullscreen."; return PRM_CAP_RESERVED;
    case 70:  *why = "Windows takes Print Screen for screenshots."; return PRM_CAP_RESERVED;
    case 226: case 230:
        *why = "Alt belongs to the window (Alt+Enter fullscreen, Alt+Tab, Alt+F4)."; return PRM_CAP_RESERVED;
    case 227: case 231: *why = "The Windows key belongs to Windows."; return PRM_CAP_RESERVED;
    default: break;
    }
    if (alt_held) { *why = "Alt+<key> belongs to the window (Alt+Enter fullscreen, Alt+F4 quits)."; return PRM_CAP_RESERVED; }
    for (int i = 0; i < (int)(sizeof prm_sdl_scan_mac / sizeof prm_sdl_scan_mac[0]); i++)
        if (prm_sdl_scan_mac[i][0] == scancode) { out->kind = PRM_KEY; out->code = prm_sdl_scan_mac[i][1]; return PRM_CAP_INPUT; }
    *why = "The game cannot read this key.";
    return PRM_CAP_UNKNOWN;
}
/* SDL_BUTTON_LEFT/MIDDLE/RIGHT/X1/X2 (1-5) -> the mouse bit (cellPad's raw
 * mouse reads these five). 0 = not one of them. */
static inline uint32_t prm_mouse_from_sdl(int button)
{
    switch (button) {
    case 1: return 1u;  case 2: return 4u;  case 3: return 2u;
    case 4: return 8u;  case 5: return 16u;
    default: return 0u;
    }
}
static inline uint32_t prm_mouse_from_wheel(int y) { return y > 0 ? PIM_MOUSE_WHEEL_UP : y < 0 ? PIM_MOUSE_WHEEL_DOWN : 0u; }
/* The controller input of a fresh press (bits newly down in g_pad_overlay_raw),
 * the lowest bindable one; -1 none. */
static inline int prm_pad_from_raw(uint32_t newly)
{
    newly &= PRM_PAD_CAPTURE_MASK;
    for (int b = 0; b < 32; b++) if (newly >> b & 1u) return b;
    return -1;
}

/* ---- editing a map ------------------------------------------------------------ */
static inline uint32_t prm_key_canon(uint32_t vk) { return vk == PRM_KEYPAD_ENTER ? PRM_ENTER : vk; }
static inline int prm_key_on(const PadInputMap* m, int ctl, uint32_t vk)
{
    return (m->key[ctl][(vk >> 5) & 3u] >> (vk & 31u)) & 1u;
}
/* Key vk (both Enters for either) on / off control ctl. */
static inline void prm_key_put(PadInputMap* m, int ctl, uint32_t vk, int on)
{
    const uint32_t c = prm_key_canon(vk);
    const uint32_t v[2] = { c, c == PRM_ENTER ? PRM_KEYPAD_ENTER : c };
    for (int i = 0; i < 2; i++) {
        const uint32_t w = (v[i] >> 5) & 3u, b = 1u << (v[i] & 31u);
        if (on) m->key[ctl][w] |= b; else m->key[ctl][w] &= ~b;
    }
}
/* Does control ctl have input `in` (a key: either Enter for Enter)? */
static inline int prm_has(const PadInputMap* m, int ctl, PrmInput in)
{
    if (in.kind == PRM_KEY) {
        const uint32_t c = prm_key_canon(in.code);
        return prm_key_on(m, ctl, c) || (c == PRM_ENTER && prm_key_on(m, ctl, PRM_KEYPAD_ENTER));
    }
    if (in.kind == PRM_MOUSE) return (m->mouse[ctl] & in.code) != 0;
    if (in.kind == PRM_PAD) return in.code < 32 && (m->pad[ctl] >> in.code & 1u);
    return 0;
}
/* The controls other than `ctl` that have `in` (a mask of PIM_* bits). */
static inline uint32_t prm_owners(const PadInputMap* m, int ctl, PrmInput in)
{
    uint32_t o = 0;
    for (int c = 0; c < PIM_N_CONTROLS; c++) if (c != ctl && prm_has(m, c, in)) o |= 1u << c;
    return o;
}
static inline int prm_first(uint32_t mask)
{
    for (int c = 0; c < PIM_N_CONTROLS; c++) if (mask >> c & 1u) return c;
    return -1;
}

/* Bind `in` to control `ctl`. A key goes in the place of `old_key` (the key in
 * the slot being changed; -1 = an empty slot: added); a mouse or controller
 * input replaces the control's whole mouse / controller binding. Every other
 * control loses `in`, and the first of them gets what was replaced (Swap) when
 * there was something. Returns the controls `in` was taken from. */
static inline uint32_t prm_assign(PadInputMap* m, int ctl, PrmInput in, int old_key)
{
    const uint32_t owners = prm_owners(m, ctl, in);
    const int first = prm_first(owners);
    if (in.kind == PRM_KEY) {
        for (int c = 0; c < PIM_N_CONTROLS; c++) if (owners >> c & 1u) prm_key_put(m, c, in.code, 0);
        if (old_key >= 0 && prm_key_canon((uint32_t)old_key) != prm_key_canon(in.code)) {
            prm_key_put(m, ctl, (uint32_t)old_key, 0);
            if (first >= 0) prm_key_put(m, first, (uint32_t)old_key, 1);
        }
        prm_key_put(m, ctl, in.code, 1);
    } else if (in.kind == PRM_MOUSE) {
        const uint32_t prev = m->mouse[ctl] & ~in.code;
        for (int c = 0; c < PIM_N_CONTROLS; c++) if (owners >> c & 1u) m->mouse[c] &= ~in.code;
        m->mouse[ctl] = in.code;
        if (first >= 0) m->mouse[first] |= prev;
    } else if (in.kind == PRM_PAD && in.code < 32) {
        const uint32_t bit = 1u << in.code, prev = m->pad[ctl] & ~bit;
        for (int c = 0; c < PIM_N_CONTROLS; c++) if (owners >> c & 1u) m->pad[c] &= ~bit;
        m->pad[ctl] = bit;
        if (first >= 0) m->pad[first] |= prev;
    }
    return owners;
}
/* Clear one slot: a key (both Enters for either), or the whole mouse /
 * controller binding of the control. */
static inline void prm_clear(PadInputMap* m, int ctl, int kind, int key)
{
    if (kind == PRM_KEY) { if (key >= 0) prm_key_put(m, ctl, (uint32_t)key, 0); }
    else if (kind == PRM_MOUSE) m->mouse[ctl] = 0;
    else if (kind == PRM_PAD) m->pad[ctl] = 0;
}
/* Every key of a control off. */
static inline void prm_clear_keys(PadInputMap* m, int ctl) { memset(m->key[ctl], 0, sizeof m->key[ctl]); }
/* The built-in bindings (keys, mouse, controller); the settings stay. */
static inline void prm_reset(PadInputMap* m)
{
    PadInputMap d;
    pim_defaults(&d);
    memcpy(m->key, d.key, sizeof m->key);
    memcpy(m->mouse, d.mouse, sizeof m->mouse);
    memcpy(m->pad, d.pad, sizeof m->pad);
}
static inline int prm_bindings_equal(const PadInputMap* a, const PadInputMap* b)
{
    return !memcmp(a->key, b->key, sizeof a->key) && !memcmp(a->mouse, b->mouse, sizeof a->mouse) &&
           !memcmp(a->pad, b->pad, sizeof a->pad);
}

/* The two key slots of control ctl: out[j] = a key code (Enter twins as 0x24)
 * or -1. `order` holds what the slots showed before (-1 none), so a key stays
 * in the slot it was put in; the other keys fill the empty slots by code.
 * Returns how many more keys the control has than the two shown. */
static inline int prm_key_slots(const PadInputMap* m, int ctl, const int order[2], int out[2])
{
    int keys[128], n = 0;
    for (uint32_t vk = 0; vk < 128; vk++) {
        if (!prm_key_on(m, ctl, vk)) continue;
        const uint32_t c = prm_key_canon(vk);
        int dup = 0;
        for (int i = 0; i < n; i++) if (keys[i] == (int)c) dup = 1;
        if (!dup) keys[n++] = (int)c;
    }
    /* ascending (0x24 may have come from 0x4C after higher codes: re-sort) */
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && keys[j - 1] > keys[j]; j--) { int t = keys[j]; keys[j] = keys[j - 1]; keys[j - 1] = t; }
    int used[128] = {0};
    out[0] = out[1] = -1;
    for (int s = 0; s < 2; s++) {
        if (!order || order[s] < 0) continue;
        for (int i = 0; i < n; i++)
            if (!used[i] && keys[i] == (int)prm_key_canon((uint32_t)order[s])) { out[s] = keys[i]; used[i] = 1; break; }
    }
    int extra = 0;
    for (int i = 0; i < n; i++) {
        if (used[i]) continue;
        if (out[0] < 0) out[0] = keys[i];
        else if (out[1] < 0) out[1] = keys[i];
        else extra++;
    }
    return extra;
}

/* "key:F17", "mouse:Right", "pad:b", or a bare key name (the file's names,
 * any case) -> an input. 1 = understood. (The menu's timed test script.) */
static inline int prm_parse_input(const char* spec, PrmInput* out)
{
    char s[64];
    snprintf(s, sizeof s, "%s", spec ? spec : "");
    pim_lower(s);
    const char* v = s;
    int kind = PRM_KEY;
    if (!strncmp(s, "key:", 4)) v = s + 4;
    else if (!strncmp(s, "mouse:", 6)) { v = s + 6; kind = PRM_MOUSE; }
    else if (!strncmp(s, "pad:", 4)) { v = s + 4; kind = PRM_PAD; }
    int id;
    if (kind == PRM_KEY) id = pim_parse_key_name(v);
    else if (kind == PRM_MOUSE) id = pim_lookup(pim_mouse_buttons, PIM_COUNT(pim_mouse_buttons), v);
    else id = pim_lookup(pim_pad_inputs, PIM_COUNT(pim_pad_inputs), v);
    if (id < 0) return 0;
    out->kind = kind;
    out->code = (uint32_t)id;
    return 1;
}

#endif /* PS3RECOMP_PAD_REMAP_H */
