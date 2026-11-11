/*
 * ps3recomp - input mapping file for the host pad (cellPad.c)
 *
 * PS3_INPUT_MAP=<path> names a small text file (a TOML subset; an .ini
 * written the same way reads the same) that rebinds the keyboard, the mouse
 * buttons and a controller's buttons to PS3 controls, and holds the mouse
 * camera's settings. Every control or setting the file leaves out keeps its
 * built-in default, so an empty or missing file is exactly the old
 * hard-coded layout. Names are case-insensitive; an unknown section, control
 * or input name is reported once on stderr and ignored.
 *
 *   # comments start with '#' or ';'
 *   [keyboard]                 # PS3 control = keys
 *   cross    = "Space"
 *   start    = "Return, KeypadEnter"     # several keys: a list ...
 *   select   = ["Tab", "Backspace"]      # ... or a TOML array
 *   l3       = ""                        # "" or "none": unbound
 *   ls_up    = "W"
 *
 *   [mouse_buttons]            # PS3 control = mouse buttons
 *   r1 = "Left"
 *   r2 = "Right"
 *   l1 = "Middle"
 *   up = "WheelUp"
 *
 *   [mouse]                    # the mouse camera (right stick)
 *   camera      = true         # PS3_MOUSE
 *   sensitivity = 1.0          # PS3_MOUSE_SENS
 *   invert_x    = false        # PS3_MOUSE_INVERT_X
 *   invert_y    = false        # PS3_MOUSE_INVERT_Y
 *   deadzone    = 30           # PS3_MOUSE_DEADZONE (0-120)
 *   capture     = true         # PS3_MOUSE_CAPTURE: a click captures the cursor
 *
 *   [controller]               # PS3 control = SDL GameController inputs
 *   cross = "a"                # (Cross on a DualSense, A on an Xbox pad)
 *   r2    = "righttrigger"
 *
 *   [controller.xbox]          # replaces [controller] lines for one family
 *   select = "back"            # of pads only: .playstation .xbox .nintendo
 *                              # .other (the controls it lists; the rest
 *                              # follow [controller])
 *
 *   [controller_settings]      # sticks and triggers (docs/INPUT.md)
 *   deadzone        = 0.08     # radial, both sticks (or deadzone_left/_right)
 *   anti_deadzone   = 0.0      # (or anti_deadzone_left/_right)
 *   outer           = 0.97     # deflection that counts as full (outer_left/_right)
 *   squircle        = 0.5      # 0 round .. 1 square diagonals (squircle_left/_right)
 *   trigger_threshold = 0.12   # digital L2/R2 press point
 *   trigger_deadzone  = 0.02
 *   left_stick      = "left"   # which physical stick drives the PS3 left stick
 *   right_stick     = "right"  # ("left", "right", "none"): swap = southpaw
 *   invert_right_y  = false    # invert_left_x/_left_y/_right_x/_right_y
 *   nintendo_layout = "position"  # or "label": A (east) = Cross on Nintendo pads
 *   tilt            = "sensor" # SIXAXIS tilt (Ionic Storm steering): the pad's
 *                              # motion sensor, else the left stick; "left_stick"; "none"
 *   tilt_invert     = false
 *
 * The environment variables in the comments win over the file. An input the
 * file binds leaves the controls the file does not mention: `cross = "Q"`
 * takes Q off its default, circle (list it under both to keep both). A key
 * or button bound to SEVERAL controls presses them together (a chord).
 *
 * PS3 controls: up down left right (D-pad), cross circle square triangle,
 * l1 r1 l2 r2 l3 r3, start select, ls_up ls_down ls_left ls_right (left
 * stick), rs_up rs_down rs_left rs_right (right stick).
 *
 * Keys (macOS virtual key codes, US layout positions): A-Z, 0-9, F1-F20,
 * Space Return Tab Escape Backspace ForwardDelete Up Down Left Right Home
 * End PageUp PageDown Help, Minus Equal LeftBracket RightBracket Backslash
 * Semicolon Quote Comma Period Slash Grave Section, Keypad0-Keypad9
 * KeypadEnter KeypadPlus KeypadMinus KeypadMultiply KeypadDivide
 * KeypadDecimal KeypadEquals KeypadClear, Shift RightShift Control
 * RightControl Option RightOption, or a raw code as 0x00-0x7F. Cmd is the
 * window's (it releases the mouse) and cannot be bound.
 *
 * Mouse buttons: Left Right Middle Button4-Button8 WheelUp WheelDown.
 *
 * Controller inputs (SDL names): a b x y back guide start leftstick
 * rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright misc1
 * paddle1-paddle4 touchpad lefttrigger righttrigger, and the stick
 * half-axes as buttons (pushed past half way): leftx- leftx+ lefty- lefty+
 * rightx- rightx+ righty- righty+ (y- = up); PlayStation and Xbox
 * names are accepted as aliases (cross circle square triangle, l1 r1 l2 r2
 * l3 r3, share create options ps, view menu home). The names are SDL's
 * POSITIONS: a = the bottom face button on every pad (Cross, Xbox A,
 * Nintendo B). The physical sticks drive the PS3 sticks ([controller_settings]
 * left_stick/right_stick choose which); a stick direction bound to a button
 * pushes that axis to the end while it is held.
 *
 * Keys on Windows are read by POSITION too, with the US names: "W" is the
 * key left of E whatever the layout prints on it. Windows names are accepted
 * as aliases (Ctrl, Alt, LeftShift, Delete = the Del key, Insert, PrintScreen,
 * ScrollLock, Pause, CapsLock, Numpad0-9, ...).
 *
 * The in-game menu (F2 > Controls; its rules are pad_remap.h) rebinds
 * controls live and writes them back with pim_save_file: the file stays as it
 * was, comments and other sections included; only binding values change and
 * every control ends up listed in each of the three binding sections.
 *
 * This header is the parser and writer only (no SDL, no AppKit), so it
 * builds into a standalone test: libs/input/tests/test_pad_input_map.c.
 */
#ifndef PS3RECOMP_PAD_INPUT_MAP_H
#define PS3RECOMP_PAD_INPUT_MAP_H

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PS3 controls. 0-15 are the CELL_PAD_CTRL_* bit numbers, so a mask of
 * pressed controls is the cellPad button word in its low 16 bits. */
enum {
    PIM_SELECT, PIM_L3, PIM_R3, PIM_START, PIM_UP, PIM_RIGHT, PIM_DOWN, PIM_LEFT,
    PIM_L2, PIM_R2, PIM_L1, PIM_R1, PIM_TRIANGLE, PIM_CIRCLE, PIM_CROSS, PIM_SQUARE,
    PIM_LS_UP, PIM_LS_DOWN, PIM_LS_LEFT, PIM_LS_RIGHT,
    PIM_RS_UP, PIM_RS_DOWN, PIM_RS_LEFT, PIM_RS_RIGHT,
    PIM_N_CONTROLS
};

/* Mouse inputs: bit n = NSEvent buttonNumber n (1 left, 2 right, 4 middle,
 * 8 button 4, ...), as the window keeps them in g_pad_mouse_buttons; the
 * wheel's two directions are synthetic presses above them. */
#define PIM_MOUSE_WHEEL_UP    (1u << 24)
#define PIM_MOUSE_WHEEL_DOWN  (1u << 25)

/* Controller inputs: bit n = SDL_GameControllerButton n (stable values since
 * SDL 2.0; 15-20 need SDL 2.0.14+), plus the two triggers read as buttons,
 * plus the sticks' half-axes read as buttons: 23 leftx-, 24 leftx+,
 * 25 lefty- (up), 26 lefty+ (down), 27-30 the same for the right stick. */
#define PIM_PAD_TRIGGER_LEFT   21
#define PIM_PAD_TRIGGER_RIGHT  22
#define PIM_PAD_AXIS_FIRST     23
#define PIM_PAD_N_INPUTS       31
#define PIM_PAD_AXIS_MASK      (((1u << 8) - 1u) << PIM_PAD_AXIS_FIRST)
#define PIM_PAD_TRIGGER_MASK   ((1u << PIM_PAD_TRIGGER_LEFT) | (1u << PIM_PAD_TRIGGER_RIGHT))

/* Controller families, for [controller.<family>]: what SDL calls the pad. */
enum { PIM_FAM_PLAYSTATION, PIM_FAM_XBOX, PIM_FAM_NINTENDO, PIM_FAM_OTHER, PIM_N_FAM };

typedef struct {
    uint32_t key[PIM_N_CONTROLS][4];   /* 128 key codes, like g_pad_host_keys */
    uint32_t mouse[PIM_N_CONTROLS];
    uint32_t pad[PIM_N_CONTROLS];
    /* [mouse]; -1 (or 0 for sens) = not in the file */
    int   mouse_camera, mouse_invert_x, mouse_invert_y, mouse_deadzone, mouse_capture;
    float mouse_sens;
    int   mouse_mode;        /* [mouse] mode: 1 direct, 0 stick, -1 not in the file */
    float mouse_aim_sens;    /* [mouse] aim_sensitivity; 0 = not in the file */
    /* [controller.<family>]: control c of family f is pad_fam[f][c] instead
     * of pad[c] when bit c of pad_fam_set[f] is set (pim_pad_resolve). */
    uint32_t pad_fam[PIM_N_FAM][PIM_N_CONTROLS];
    uint32_t pad_fam_set[PIM_N_FAM];
    /* [controller_settings]; < 0 = not in the file. Index 0 = the PS3 left
     * stick, 1 = the right one (pad_stick.h has the meanings). */
    float stick_deadzone[2], stick_anti[2], stick_outer[2], stick_squircle[2];
    float trigger_threshold, trigger_deadzone;
    int   stick_source[2];      /* physical stick driving it: 0 left, 1 right, 2 none */
    int   stick_invert[2][2];   /* [stick][0 = x, 1 = y] */
    int   nintendo_labels;      /* 1: Nintendo pads by label (A = Cross), 0 by position */
    int   tilt;                 /* SIXAXIS tilt: 0 none, 1 motion sensor (else left stick), 2 left stick */
    int   tilt_invert;
    int   n_set;        /* controls/settings the file changed */
    int   n_warnings;   /* names it did not know */
} PadInputMap;

typedef struct { const char* name; int id; } PimName;

static const PimName pim_controls[] = {
    {"select", PIM_SELECT}, {"l3", PIM_L3}, {"r3", PIM_R3}, {"start", PIM_START},
    {"up", PIM_UP}, {"right", PIM_RIGHT}, {"down", PIM_DOWN}, {"left", PIM_LEFT},
    {"l2", PIM_L2}, {"r2", PIM_R2}, {"l1", PIM_L1}, {"r1", PIM_R1},
    {"triangle", PIM_TRIANGLE}, {"circle", PIM_CIRCLE}, {"cross", PIM_CROSS}, {"square", PIM_SQUARE},
    {"ls_up", PIM_LS_UP}, {"ls_down", PIM_LS_DOWN}, {"ls_left", PIM_LS_LEFT}, {"ls_right", PIM_LS_RIGHT},
    {"rs_up", PIM_RS_UP}, {"rs_down", PIM_RS_DOWN}, {"rs_left", PIM_RS_LEFT}, {"rs_right", PIM_RS_RIGHT},
    /* aliases */
    {"dpad_up", PIM_UP}, {"dpad_down", PIM_DOWN}, {"dpad_left", PIM_LEFT}, {"dpad_right", PIM_RIGHT},
    {"left_stick_up", PIM_LS_UP}, {"left_stick_down", PIM_LS_DOWN},
    {"left_stick_left", PIM_LS_LEFT}, {"left_stick_right", PIM_LS_RIGHT},
    {"right_stick_up", PIM_RS_UP}, {"right_stick_down", PIM_RS_DOWN},
    {"right_stick_left", PIM_RS_LEFT}, {"right_stick_right", PIM_RS_RIGHT},
};

static const PimName pim_keys[] = {
    {"a", 0x00}, {"s", 0x01}, {"d", 0x02}, {"f", 0x03}, {"h", 0x04}, {"g", 0x05},
    {"z", 0x06}, {"x", 0x07}, {"c", 0x08}, {"v", 0x09}, {"section", 0x0A}, {"b", 0x0B},
    {"q", 0x0C}, {"w", 0x0D}, {"e", 0x0E}, {"r", 0x0F}, {"y", 0x10}, {"t", 0x11},
    {"1", 0x12}, {"2", 0x13}, {"3", 0x14}, {"4", 0x15}, {"6", 0x16}, {"5", 0x17},
    {"equal", 0x18}, {"9", 0x19}, {"7", 0x1A}, {"minus", 0x1B}, {"8", 0x1C}, {"0", 0x1D},
    {"rightbracket", 0x1E}, {"o", 0x1F}, {"u", 0x20}, {"leftbracket", 0x21}, {"i", 0x22},
    {"p", 0x23}, {"return", 0x24}, {"l", 0x25}, {"j", 0x26}, {"quote", 0x27}, {"k", 0x28},
    {"semicolon", 0x29}, {"backslash", 0x2A}, {"comma", 0x2B}, {"slash", 0x2C},
    {"n", 0x2D}, {"m", 0x2E}, {"period", 0x2F}, {"tab", 0x30}, {"space", 0x31},
    {"grave", 0x32}, {"backspace", 0x33}, {"escape", 0x35},
    {"shift", 0x38}, {"option", 0x3A}, {"control", 0x3B},
    {"rightshift", 0x3C}, {"rightoption", 0x3D}, {"rightcontrol", 0x3E},
    {"f17", 0x40}, {"keypaddecimal", 0x41}, {"keypadmultiply", 0x43}, {"keypadplus", 0x45},
    {"keypadclear", 0x47}, {"keypaddivide", 0x4B}, {"keypadenter", 0x4C},
    {"keypadminus", 0x4E}, {"f18", 0x4F}, {"f19", 0x50}, {"keypadequals", 0x51},
    {"keypad0", 0x52}, {"keypad1", 0x53}, {"keypad2", 0x54}, {"keypad3", 0x55},
    {"keypad4", 0x56}, {"keypad5", 0x57}, {"keypad6", 0x58}, {"keypad7", 0x59},
    {"f20", 0x5A}, {"keypad8", 0x5B}, {"keypad9", 0x5C},
    {"f5", 0x60}, {"f6", 0x61}, {"f7", 0x62}, {"f3", 0x63}, {"f8", 0x64}, {"f9", 0x65},
    {"f11", 0x67}, {"f13", 0x69}, {"f16", 0x6A}, {"f14", 0x6B}, {"f10", 0x6D},
    {"f12", 0x6F}, {"f15", 0x71}, {"help", 0x72}, {"home", 0x73}, {"pageup", 0x74},
    {"forwarddelete", 0x75}, {"f4", 0x76}, {"end", 0x77}, {"f2", 0x78},
    {"pagedown", 0x79}, {"f1", 0x7A}, {"left", 0x7B}, {"right", 0x7C},
    {"down", 0x7D}, {"up", 0x7E},
    /* aliases */
    {"enter", 0x24}, {"esc", 0x35}, {"backquote", 0x32},
    {"leftshift", 0x38}, {"leftcontrol", 0x3B}, {"leftoption", 0x3A},
    {"ctrl", 0x3B}, {"alt", 0x3A}, {"rightctrl", 0x3E}, {"rightalt", 0x3D},
    {"insert", 0x72}, {"numlock", 0x47},
    {"arrowup", 0x7E}, {"arrowdown", 0x7D}, {"arrowleft", 0x7B}, {"arrowright", 0x7C},
    /* Windows names (a PC keyboard's key at the same position). "Delete" is
     * the Del key here (it was the Mac's Backspace, which is "Backspace"). */
    {"delete", 0x75}, {"del", 0x75}, {"ins", 0x72},
    {"lshift", 0x38}, {"rshift", 0x3C}, {"lctrl", 0x3B}, {"rctrl", 0x3E},
    {"leftctrl", 0x3B}, {"lalt", 0x3A}, {"ralt", 0x3D}, {"leftalt", 0x3A}, {"altgr", 0x3D},
    {"capslock", 0x39}, {"caps", 0x39},
    {"printscreen", 0x69}, {"prtsc", 0x69}, {"scrolllock", 0x6B}, {"pause", 0x71}, {"break", 0x71},
    {"pgup", 0x74}, {"pgdn", 0x79}, {"apps", 0x6E}, {"contextmenu", 0x6E},
    {"tilde", 0x32}, {"backtick", 0x32}, {"lbracket", 0x21}, {"rbracket", 0x1E},
    {"numpad0", 0x52}, {"numpad1", 0x53}, {"numpad2", 0x54}, {"numpad3", 0x55},
    {"numpad4", 0x56}, {"numpad5", 0x57}, {"numpad6", 0x58}, {"numpad7", 0x59},
    {"numpad8", 0x5B}, {"numpad9", 0x5C}, {"numpadenter", 0x4C}, {"numpadplus", 0x45},
    {"numpadminus", 0x4E}, {"numpadmultiply", 0x43}, {"numpaddivide", 0x4B},
    {"numpaddecimal", 0x41}, {"oem102", 0x0A},
};

static const PimName pim_mouse_buttons[] = {
    {"left", 1}, {"right", 2}, {"middle", 4}, {"button4", 8}, {"button5", 16},
    {"button6", 32}, {"button7", 64}, {"button8", 128},
    {"wheelup", (int)PIM_MOUSE_WHEEL_UP}, {"wheeldown", (int)PIM_MOUSE_WHEEL_DOWN},
    {"back", 8}, {"forward", 16},
};

static const PimName pim_pad_inputs[] = {
    {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"guide", 5}, {"start", 6},
    {"leftstick", 7}, {"rightstick", 8}, {"leftshoulder", 9}, {"rightshoulder", 10},
    {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14}, {"misc1", 15},
    {"paddle1", 16}, {"paddle2", 17}, {"paddle3", 18}, {"paddle4", 19}, {"touchpad", 20},
    {"lefttrigger", PIM_PAD_TRIGGER_LEFT}, {"righttrigger", PIM_PAD_TRIGGER_RIGHT},
    /* PlayStation / Xbox names */
    {"cross", 0}, {"circle", 1}, {"square", 2}, {"triangle", 3},
    {"share", 4}, {"create", 4}, {"select", 4}, {"view", 4},
    {"ps", 5}, {"home", 5}, {"options", 6}, {"menu", 6},
    {"l3", 7}, {"r3", 8}, {"l1", 9}, {"r1", 10}, {"lb", 9}, {"rb", 10},
    {"up", 11}, {"down", 12}, {"left", 13}, {"right", 14},
    {"l2", PIM_PAD_TRIGGER_LEFT}, {"r2", PIM_PAD_TRIGGER_RIGHT},
    {"lt", PIM_PAD_TRIGGER_LEFT}, {"rt", PIM_PAD_TRIGGER_RIGHT},
    /* the sticks' half-axes, read as buttons */
    {"leftx-", 23}, {"leftx+", 24}, {"lefty-", 25}, {"lefty+", 26},
    {"rightx-", 27}, {"rightx+", 28}, {"righty-", 29}, {"righty+", 30},
    {"ls_left", 23}, {"ls_right", 24}, {"ls_up", 25}, {"ls_down", 26},
    {"rs_left", 27}, {"rs_right", 28}, {"rs_up", 29}, {"rs_down", 30},
};

/* The family of a controller from SDL_GameControllerType (SDL 2.30 values:
 * 1/2 Xbox 360/One, 3/4/7 PS3/PS4/PS5, 5 Switch Pro, 11-13 Joy-Cons). */
static inline int pim_family_of_sdl_type(int sdl_type)
{
    switch (sdl_type) {
    case 3: case 4: case 7: return PIM_FAM_PLAYSTATION;
    case 1: case 2: return PIM_FAM_XBOX;
    case 5: case 11: case 12: case 13: return PIM_FAM_NINTENDO;
    default: return PIM_FAM_OTHER;
    }
}
static const char* const pim_family_names[PIM_N_FAM] = { "playstation", "xbox", "nintendo", "other" };

/* The bindings of family `fam` (-1: [controller] alone): each control its
 * [controller.<fam>] line if the file has one, else its [controller] line
 * minus the inputs the family section took for other controls. */
static inline void pim_pad_resolve(const PadInputMap* m, int fam, uint32_t out[PIM_N_CONTROLS])
{
    const uint32_t set = (fam >= 0 && fam < PIM_N_FAM) ? m->pad_fam_set[fam] : 0;
    uint32_t taken = 0;
    for (int c = 0; c < PIM_N_CONTROLS; c++) if (set >> c & 1u) taken |= m->pad_fam[fam][c];
    for (int c = 0; c < PIM_N_CONTROLS; c++)
        out[c] = (set >> c & 1u) ? m->pad_fam[fam][c] : (m->pad[c] & ~taken);
}

#define PIM_COUNT(a) (int)(sizeof(a) / sizeof((a)[0]))

static int pim_lookup(const PimName* t, int n, const char* name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].name, name)) return t[i].id;
    return -1;
}

static void pim_key_set(uint32_t m[4], unsigned vk) { m[(vk >> 5) & 3] |= 1u << (vk & 31); }

/* The built-in layout (docs/INPUT.md has it as a table).
 *
 * Keyboard and mouse follow inFAMOUS 2's own controls page (the game's
 * u_controller_help_* strings): L1 is "Aim Mode" and turns the face buttons
 * into powers (L1+R1 Bolt, L1+Square Grenade, L1+Triangle Rocket, L1+Cross
 * Blast, L1+L2 Find Electric Sources, L1+R3 Switch Reticule Side), so aim is
 * the right mouse button and the powers sit under the left hand:
 *
 *   move W A S D             camera  mouse (or I J K L)
 *   Cross    Space   Jump              (aim: Blast)
 *   Circle   Shift   Dive Roll / Drop / cover
 *   Square   F       Melee             (aim: Grenade)
 *   Triangle Q       Melee Finisher    (aim: Rocket; with R1: Pulse Heal)
 *   R1       E, left mouse    Static Thrusters / Use Object (aim: Bolt)
 *   L1       right mouse, Z   Aim Mode
 *   R2       R, mouse 4       Kinetic Pulse (hold to lift, tap to throw)
 *   L2       C, mouse 5       Electric Drain (hold) / Radar Pulse (tap)
 *   L3       V                Ping
 *   R3       middle mouse, X  (aim: Switch Reticule Side)
 *   D-pad    1 2 3 4 = up down left right (Precision Mode, Ionic Vortex,
 *            Purchase Powers, Switch Powers), also the arrow keys (menus);
 *            wheel up = up (Precision Mode)
 *   START    Enter            SELECT  M, Tab (World Map)
 *   Esc is the window's (menu / release the mouse), not the game's.
 *
 * A controller is bound by POSITION (SDL's a/b/x/y = bottom/right/left/top on
 * every pad; the runtime asks SDL for positions on Nintendo pads too), so its
 * layout is the DualShock's on any pad; the touchpad click of a DualShock
 * 4/DualSense is SELECT (World Map), as in PS4 remasters. */
static void pim_defaults(PadInputMap* m)
{
    memset(m, 0, sizeof *m);
    static const struct { int ctl; unsigned char vk; } keys[] = {
        {PIM_LS_UP, 0x0D}, {PIM_LS_LEFT, 0x00}, {PIM_LS_DOWN, 0x01}, {PIM_LS_RIGHT, 0x02},   /* WASD */
        {PIM_RS_UP, 0x22}, {PIM_RS_LEFT, 0x26}, {PIM_RS_DOWN, 0x28}, {PIM_RS_RIGHT, 0x25},   /* IJKL */
        {PIM_CROSS, 0x31},                              /* Space */
        {PIM_CIRCLE, 0x38},                             /* Shift */
        {PIM_SQUARE, 0x03},                             /* F */
        {PIM_TRIANGLE, 0x0C},                           /* Q */
        {PIM_R1, 0x0E},                                 /* E */
        {PIM_L1, 0x06},                                 /* Z */
        {PIM_R2, 0x0F},                                 /* R */
        {PIM_L2, 0x08},                                 /* C */
        {PIM_L3, 0x09},                                 /* V */
        {PIM_R3, 0x07},                                 /* X */
        {PIM_UP, 0x12}, {PIM_DOWN, 0x13}, {PIM_LEFT, 0x14}, {PIM_RIGHT, 0x15},   /* 1 2 3 4 */
        {PIM_UP, 0x7E}, {PIM_DOWN, 0x7D}, {PIM_LEFT, 0x7B}, {PIM_RIGHT, 0x7C},   /* arrows */
        {PIM_START, 0x24}, {PIM_START, 0x4C},          /* Return, keypad Enter */
        {PIM_SELECT, 0x2E}, {PIM_SELECT, 0x30},         /* M, Tab */
    };
    for (int i = 0; i < PIM_COUNT(keys); i++) pim_key_set(m->key[keys[i].ctl], keys[i].vk);
    m->mouse[PIM_R1] = 1;                   /* left: Bolt (with aim) / use */
    m->mouse[PIM_L1] = 2;                   /* right: Aim Mode */
    m->mouse[PIM_R3] = 4;                   /* middle: switch reticule side */
    m->mouse[PIM_R2] = 8;                   /* button 4 (back): Kinetic Pulse */
    m->mouse[PIM_L2] = 16;                  /* button 5 (forward): Drain */
    m->mouse[PIM_UP] = PIM_MOUSE_WHEEL_UP;  /* wheel up: Precision Mode */
    static const struct { int ctl; int in; } pad[] = {
        {PIM_SELECT, 4}, {PIM_SELECT, 20},              /* back (Share/View/-), touchpad */
        {PIM_L3, 7}, {PIM_R3, 8}, {PIM_START, 6},
        {PIM_UP, 11}, {PIM_RIGHT, 14}, {PIM_DOWN, 12}, {PIM_LEFT, 13},
        {PIM_L1, 9}, {PIM_R1, 10}, {PIM_TRIANGLE, 3}, {PIM_CIRCLE, 1},
        {PIM_CROSS, 0}, {PIM_SQUARE, 2},
        {PIM_L2, PIM_PAD_TRIGGER_LEFT}, {PIM_R2, PIM_PAD_TRIGGER_RIGHT},
    };
    for (int i = 0; i < PIM_COUNT(pad); i++) m->pad[pad[i].ctl] |= 1u << pad[i].in;
    m->mouse_camera = m->mouse_invert_x = m->mouse_invert_y = m->mouse_deadzone = m->mouse_capture = -1;
    m->mouse_sens = 0.0f;
    m->mouse_mode = -1;
    m->mouse_aim_sens = 0.0f;
    for (int s = 0; s < 2; s++) {
        m->stick_deadzone[s] = m->stick_anti[s] = m->stick_outer[s] = m->stick_squircle[s] = -1.0f;
        m->stick_source[s] = -1;
        m->stick_invert[s][0] = m->stick_invert[s][1] = -1;
    }
    m->trigger_threshold = m->trigger_deadzone = -1.0f;
    m->nintendo_labels = -1;
    m->tilt = m->tilt_invert = -1;
}

/* ---- parsing ---------------------------------------------------------- */

enum { PIM_SEC_NONE, PIM_SEC_KEYBOARD, PIM_SEC_MOUSE_BUTTONS, PIM_SEC_MOUSE,
       PIM_SEC_CONTROLLER, PIM_SEC_UNKNOWN, PIM_SEC_CONTROLLER_FAMILY, PIM_SEC_CONTROLLER_SETTINGS };

static void pim_lower(char* s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

static char* pim_trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void pim_warn(PadInputMap* m, FILE* err, const char* src, int line, const char* what, const char* name)
{
    m->n_warnings++;
    if (err) fprintf(err, "[cellPad] %s:%d: unknown %s '%s' (ignored)\n", src, line, what, name);
}

/* Cut a line's comment ('#' or ';' outside quotes). */
static void pim_strip_comment(char* s)
{
    char q = 0;
    for (; *s; s++) {
        if (q) { if (*s == q) q = 0; continue; }
        if (*s == '"' || *s == '\'') q = *s;
        else if (*s == '#' || *s == ';') { *s = 0; return; }
    }
}

/* A value is a string ("..." or '...'), an array of strings, or a bare word.
 * Every form flattens to a list split on commas; quotes and brackets go. */
static void pim_flatten(char* v)
{
    char* w = v;
    for (char* r = v; *r; r++)
        if (*r != '"' && *r != '\'' && *r != '[' && *r != ']') *w++ = *r;
    *w = 0;
}

static int pim_bool(const char* v, int* out)
{
    if (!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "on")) { *out = 1; return 1; }
    if (!strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "no") || !strcmp(v, "off")) { *out = 0; return 1; }
    return 0;
}

static int pim_parse_key_name(const char* name)
{
    int id = pim_lookup(pim_keys, PIM_COUNT(pim_keys), name);
    if (id >= 0) return id;
    if (name[0] == '0' && name[1] == 'x' && name[2]) {
        char* end; long v = strtol(name + 2, &end, 16);
        if (!*end && v >= 0 && v < 128) return (int)v;
    }
    return -1;
}

/* One "control = inputs" line of a binding section (fam: the family of a
 * [controller.<family>] section, else unused). */
static void pim_bind(PadInputMap* m, int sec, int ctl, char* list, FILE* err, const char* src, int line, int fam)
{
    uint32_t keys[4] = {0, 0, 0, 0}, bits = 0;
    /* no strtok: cellPad's PS3_PAD_FILE reader uses it from the poll thread */
    for (char *tok = list, *next; tok; tok = next) {
        next = strchr(tok, ',');
        if (next) *next++ = 0;
        char* name = pim_trim(tok);
        if (!*name || !strcmp(name, "none")) continue;
        if (sec == PIM_SEC_KEYBOARD) {
            int vk = pim_parse_key_name(name);
            if (vk < 0) { pim_warn(m, err, src, line, "key", name); continue; }
            pim_key_set(keys, (unsigned)vk);
        } else if (sec == PIM_SEC_MOUSE_BUTTONS) {
            int b = pim_lookup(pim_mouse_buttons, PIM_COUNT(pim_mouse_buttons), name);
            if (b < 0) { pim_warn(m, err, src, line, "mouse button", name); continue; }
            bits |= (uint32_t)b;
        } else {
            int b = pim_lookup(pim_pad_inputs, PIM_COUNT(pim_pad_inputs), name);
            if (b < 0) { pim_warn(m, err, src, line, "controller input", name); continue; }
            bits |= 1u << b;
        }
    }
    if (sec == PIM_SEC_KEYBOARD) memcpy(m->key[ctl], keys, sizeof keys);
    else if (sec == PIM_SEC_MOUSE_BUTTONS) m->mouse[ctl] = bits;
    else if (sec == PIM_SEC_CONTROLLER_FAMILY) { m->pad_fam[fam][ctl] = bits; m->pad_fam_set[fam] |= 1u << ctl; }
    else m->pad[ctl] = bits;
    m->n_set++;
}

/* A fraction setting: 0.08, or a percentage (8 or 8%). */
static int pim_fraction(const char* v, float* out)
{
    char* end;
    float f = strtof(v, &end);
    if (end == v) return 0;
    if (*end == '%') { end++; f /= 100.0f; }
    else if (f > 1.0f) f /= 100.0f;
    if (*end) return 0;
    if (f < 0.0f) f = 0.0f;
    *out = f;
    return 1;
}

/* One line of [controller_settings]. */
static void pim_controller_setting(PadInputMap* m, const char* key, const char* v, FILE* err, const char* src, int line)
{
    static const struct { const char* name; int field; int stick; } fl[] = {
        {"deadzone", 0, 2}, {"deadzone_left", 0, 0}, {"deadzone_right", 0, 1},
        {"left_deadzone", 0, 0}, {"right_deadzone", 0, 1},
        {"anti_deadzone", 1, 2}, {"anti_deadzone_left", 1, 0}, {"anti_deadzone_right", 1, 1},
        {"outer", 2, 2}, {"outer_left", 2, 0}, {"outer_right", 2, 1},
        {"saturation", 2, 2}, {"saturation_left", 2, 0}, {"saturation_right", 2, 1},
        {"squircle", 3, 2}, {"squircle_left", 3, 0}, {"squircle_right", 3, 1},
        {"trigger_threshold", 4, 0}, {"trigger_deadzone", 5, 0},
    };
    for (int i = 0; i < PIM_COUNT(fl); i++) {
        if (strcmp(key, fl[i].name)) continue;
        float f;
        if (!pim_fraction(v, &f)) { pim_warn(m, err, src, line, "value for controller setting", v); return; }
        for (int s = 0; s < 2; s++) {
            if (fl[i].stick != 2 && fl[i].stick != s) continue;
            switch (fl[i].field) {
            case 0: m->stick_deadzone[s] = f; break;
            case 1: m->stick_anti[s] = f; break;
            case 2: m->stick_outer[s] = f; break;
            case 3: m->stick_squircle[s] = f; break;
            case 4: m->trigger_threshold = f; break;
            case 5: m->trigger_deadzone = f; break;
            }
        }
        m->n_set++;
        return;
    }
    int b;
    if (!strcmp(key, "left_stick") || !strcmp(key, "right_stick")) {
        const int s = key[0] == 'r';
        int src_stick = -1;
        if (!strcmp(v, "left")) src_stick = 0;
        else if (!strcmp(v, "right")) src_stick = 1;
        else if (!strcmp(v, "none") || !*v) src_stick = 2;
        if (src_stick < 0) { pim_warn(m, err, src, line, "value for controller setting", v); return; }
        m->stick_source[s] = src_stick;
        m->n_set++;
        return;
    }
    static const struct { const char* name; int s, a; } inv[] = {
        {"invert_left_x", 0, 0}, {"invert_left_y", 0, 1}, {"invert_right_x", 1, 0}, {"invert_right_y", 1, 1},
    };
    for (int i = 0; i < PIM_COUNT(inv); i++) {
        if (strcmp(key, inv[i].name)) continue;
        if (!pim_bool(v, &b)) { pim_warn(m, err, src, line, "value for controller setting", v); return; }
        m->stick_invert[inv[i].s][inv[i].a] = b;
        m->n_set++;
        return;
    }
    if (!strcmp(key, "nintendo_layout")) {
        if (!strcmp(v, "position") || !strcmp(v, "positional") || !strcmp(v, "positions")) m->nintendo_labels = 0;
        else if (!strcmp(v, "label") || !strcmp(v, "labels") || !strcmp(v, "labeled")) m->nintendo_labels = 1;
        else { pim_warn(m, err, src, line, "value for controller setting", v); return; }
        m->n_set++;
        return;
    }
    if (!strcmp(key, "tilt")) {
        if (!strcmp(v, "sensor") || !strcmp(v, "motion") || !strcmp(v, "gyro")) m->tilt = 1;
        else if (!strcmp(v, "left_stick") || !strcmp(v, "stick")) m->tilt = 2;
        else if (!strcmp(v, "none") || !strcmp(v, "off") || !*v) m->tilt = 0;
        else { pim_warn(m, err, src, line, "value for controller setting", v); return; }
        m->n_set++;
        return;
    }
    if (!strcmp(key, "tilt_invert")) {
        if (!pim_bool(v, &b)) { pim_warn(m, err, src, line, "value for controller setting", v); return; }
        m->tilt_invert = b;
        m->n_set++;
        return;
    }
    pim_warn(m, err, src, line, "controller setting", key);
}

static void pim_mouse_setting(PadInputMap* m, const char* key, const char* v, FILE* err, const char* src, int line)
{
    int b;
    if (!strcmp(key, "camera") || !strcmp(key, "enabled")) {
        if (pim_bool(v, &b)) { m->mouse_camera = b; m->n_set++; return; }
    } else if (!strcmp(key, "capture")) {
        if (pim_bool(v, &b)) { m->mouse_capture = b; m->n_set++; return; }
    } else if (!strcmp(key, "invert_x")) {
        if (pim_bool(v, &b)) { m->mouse_invert_x = b; m->n_set++; return; }
    } else if (!strcmp(key, "invert_y")) {
        if (pim_bool(v, &b)) { m->mouse_invert_y = b; m->n_set++; return; }
    } else if (!strcmp(key, "sensitivity") || !strcmp(key, "sens")) {
        char* end; float f = strtof(v, &end);
        if (end != v && !*end && f > 0.0f) { m->mouse_sens = f; m->n_set++; return; }
    } else if (!strcmp(key, "deadzone")) {
        char* end; long n = strtol(v, &end, 10);
        if (end != v && !*end) { m->mouse_deadzone = n < 0 ? 0 : n > 120 ? 120 : (int)n; m->n_set++; return; }
    } else if (!strcmp(key, "mode")) {
        if (!strcmp(v, "direct")) { m->mouse_mode = 1; m->n_set++; return; }
        if (!strcmp(v, "stick")) { m->mouse_mode = 0; m->n_set++; return; }
    } else if (!strcmp(key, "aim_sensitivity") || !strcmp(key, "aim_sens")) {
        char* end; float f = strtof(v, &end);
        if (end != v && !*end && f > 0.0f) { m->mouse_aim_sens = f; m->n_set++; return; }
    } else {
        pim_warn(m, err, src, line, "mouse setting", key);
        return;
    }
    pim_warn(m, err, src, line, "value for mouse setting", v);
}

/* Apply a whole file's text on top of what *m holds (call pim_defaults
 * first). `src` names it in the warnings; `err` may be NULL. Returns the
 * number of warnings. */
static int pim_parse(PadInputMap* m, const char* text, FILE* err, const char* src)
{
    int sec = PIM_SEC_NONE, line_no = 0, fam = 0;
    const int start_warnings = m->n_warnings;
    uint32_t set_key = 0, set_mouse = 0, set_pad = 0;   /* controls this text binds */
    const char* p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char buf[1024];
        line_no++;
        if (len >= sizeof buf) len = sizeof buf - 1;   /* overlong lines are cut, not fatal */
        memcpy(buf, p, len); buf[len] = 0;
        p = nl ? nl + 1 : p + strlen(p);

        pim_strip_comment(buf);
        char* s = pim_trim(buf);
        if (!*s) continue;
        if (*s == '[') {
            char* e = strchr(s, ']');
            if (e) *e = 0;
            char* name = pim_trim(s + 1);
            pim_lower(name);
            if (!strcmp(name, "keyboard") || !strcmp(name, "keys")) sec = PIM_SEC_KEYBOARD;
            else if (!strcmp(name, "mouse_buttons") || !strcmp(name, "mouse.buttons")) sec = PIM_SEC_MOUSE_BUTTONS;
            else if (!strcmp(name, "mouse")) sec = PIM_SEC_MOUSE;
            else if (!strcmp(name, "controller") || !strcmp(name, "gamepad") || !strcmp(name, "pad")) sec = PIM_SEC_CONTROLLER;
            else if (!strcmp(name, "controller_settings") || !strcmp(name, "controller.settings") ||
                     !strcmp(name, "sticks")) sec = PIM_SEC_CONTROLLER_SETTINGS;
            else if (!strncmp(name, "controller.", 11)) {
                const char* f = name + 11;
                sec = PIM_SEC_CONTROLLER_FAMILY;
                if (!strcmp(f, "playstation") || !strcmp(f, "ps") || !strcmp(f, "dualshock") || !strcmp(f, "dualsense")) fam = PIM_FAM_PLAYSTATION;
                else if (!strcmp(f, "xbox")) fam = PIM_FAM_XBOX;
                else if (!strcmp(f, "nintendo") || !strcmp(f, "switch")) fam = PIM_FAM_NINTENDO;
                else if (!strcmp(f, "other") || !strcmp(f, "generic")) fam = PIM_FAM_OTHER;
                else { sec = PIM_SEC_UNKNOWN; pim_warn(m, err, src, line_no, "section", name); }
            }
            else { sec = PIM_SEC_UNKNOWN; pim_warn(m, err, src, line_no, "section", name); }
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) { pim_warn(m, err, src, line_no, "line", s); continue; }
        *eq = 0;
        char* key = pim_trim(s);
        char* val = pim_trim(eq + 1);
        pim_flatten(key);
        pim_flatten(val);
        key = pim_trim(key);
        val = pim_trim(val);
        pim_lower(key);
        pim_lower(val);
        if (sec == PIM_SEC_UNKNOWN) continue;              /* already reported */
        if (sec == PIM_SEC_NONE) { pim_warn(m, err, src, line_no, "setting outside a section", key); continue; }
        if (sec == PIM_SEC_MOUSE) { pim_mouse_setting(m, key, val, err, src, line_no); continue; }
        if (sec == PIM_SEC_CONTROLLER_SETTINGS) { pim_controller_setting(m, key, val, err, src, line_no); continue; }
        const int ctl = pim_lookup(pim_controls, PIM_COUNT(pim_controls), key);
        if (ctl < 0) { pim_warn(m, err, src, line_no, "PS3 control", key); continue; }
        pim_bind(m, sec, ctl, val, err, src, line_no, fam);
        if (sec == PIM_SEC_KEYBOARD) set_key |= 1u << ctl;
        else if (sec == PIM_SEC_MOUSE_BUTTONS) set_mouse |= 1u << ctl;
        else if (sec == PIM_SEC_CONTROLLER) set_pad |= 1u << ctl;
        /* a family section's inputs leave the other controls in pim_pad_resolve */
    }
    /* An input bound here leaves the controls not mentioned here. */
    uint32_t keys[4] = {0, 0, 0, 0}, mouse = 0, pad = 0;
    for (int c = 0; c < PIM_N_CONTROLS; c++) {
        if (set_key >> c & 1) for (int i = 0; i < 4; i++) keys[i] |= m->key[c][i];
        if (set_mouse >> c & 1) mouse |= m->mouse[c];
        if (set_pad >> c & 1) pad |= m->pad[c];
    }
    for (int c = 0; c < PIM_N_CONTROLS; c++) {
        if (!(set_key >> c & 1)) for (int i = 0; i < 4; i++) m->key[c][i] &= ~keys[i];
        if (!(set_mouse >> c & 1)) m->mouse[c] &= ~mouse;
        if (!(set_pad >> c & 1)) m->pad[c] &= ~pad;
    }
    return m->n_warnings - start_warnings;
}

/* Defaults, then the file at `path` on top. Returns 1 if the file was read,
 * 0 if it could not be opened (the defaults stand). */
static int pim_load_file(PadInputMap* m, const char* path, FILE* err)
{
    pim_defaults(m);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    size_t cap = 4096, n = 0;
    char* text = (char*)malloc(cap);
    if (!text) { fclose(f); return 0; }
    for (;;) {
        if (n + 1024 + 1 > cap) {
            if (cap >= (1u << 20)) break;   /* a mapping is a few KB; stop at 1 MB */
            char* t = (char*)realloc(text, cap * 2);
            if (!t) break;
            text = t; cap *= 2;
        }
        size_t r = fread(text + n, 1, 1024, f);
        n += r;
        if (r < 1024) break;
    }
    fclose(f);
    text[n] = 0;
    pim_parse(m, text, err, path);
    free(text);
    return 1;
}

/* ---- writing ---------------------------------------------------------- */
/* The in-game menu saves what it rebinds to the same file. The file is kept
 * line for line -- comments, blank lines, the order, other sections ([mouse],
 * [controller.xbox], ...), its line endings -- and only the VALUES of the
 * binding lines change: each line of [keyboard], [mouse_buttons] and
 * [controller] that names a PS3 control gets that control's inputs, in the
 * canonical spelling (the launcher's, launcher/src-tauri/src/input_map.rs in
 * the inFAMOUS 2 port), its trailing comment kept. A control a section does
 * not list yet is added after the section's last line (its first block), a
 * binding section the file lacks is appended, so the file always reads back
 * as exactly the map. A new file gets a header and the three sections. */

/* PS3 controls in the order the file lists them. */
static const int pim_write_order[PIM_N_CONTROLS] = {
    PIM_CROSS, PIM_CIRCLE, PIM_SQUARE, PIM_TRIANGLE, PIM_L1, PIM_R1, PIM_L2, PIM_R2,
    PIM_L3, PIM_R3, PIM_START, PIM_SELECT, PIM_UP, PIM_DOWN, PIM_LEFT, PIM_RIGHT,
    PIM_LS_UP, PIM_LS_DOWN, PIM_LS_LEFT, PIM_LS_RIGHT, PIM_RS_UP, PIM_RS_DOWN, PIM_RS_LEFT, PIM_RS_RIGHT,
};

/* The file's name of a PS3 control ("cross", "ls_up"). */
static const char* pim_control_name(int ctl)
{
    for (int i = 0; i < PIM_COUNT(pim_controls); i++)
        if (pim_controls[i].id == ctl) return pim_controls[i].name;
    return "?";
}

/* Canonical spelling of each key (the launcher's), by macOS key code. */
static const PimName pim_key_canon[] = {
    {"A", 0x00}, {"S", 0x01}, {"D", 0x02}, {"F", 0x03}, {"H", 0x04}, {"G", 0x05},
    {"Z", 0x06}, {"X", 0x07}, {"C", 0x08}, {"V", 0x09}, {"Section", 0x0A}, {"B", 0x0B},
    {"Q", 0x0C}, {"W", 0x0D}, {"E", 0x0E}, {"R", 0x0F}, {"Y", 0x10}, {"T", 0x11},
    {"1", 0x12}, {"2", 0x13}, {"3", 0x14}, {"4", 0x15}, {"6", 0x16}, {"5", 0x17},
    {"Equal", 0x18}, {"9", 0x19}, {"7", 0x1A}, {"Minus", 0x1B}, {"8", 0x1C}, {"0", 0x1D},
    {"RightBracket", 0x1E}, {"O", 0x1F}, {"U", 0x20}, {"LeftBracket", 0x21}, {"I", 0x22},
    {"P", 0x23}, {"Return", 0x24}, {"L", 0x25}, {"J", 0x26}, {"Quote", 0x27}, {"K", 0x28},
    {"Semicolon", 0x29}, {"Backslash", 0x2A}, {"Comma", 0x2B}, {"Slash", 0x2C},
    {"N", 0x2D}, {"M", 0x2E}, {"Period", 0x2F}, {"Tab", 0x30}, {"Space", 0x31},
    {"Grave", 0x32}, {"Backspace", 0x33}, {"Escape", 0x35},
    {"Shift", 0x38}, {"Option", 0x3A}, {"Control", 0x3B},
    {"RightShift", 0x3C}, {"RightOption", 0x3D}, {"RightControl", 0x3E},
    {"F17", 0x40}, {"KeypadDecimal", 0x41}, {"KeypadMultiply", 0x43}, {"KeypadPlus", 0x45},
    {"KeypadClear", 0x47}, {"KeypadDivide", 0x4B}, {"KeypadEnter", 0x4C},
    {"KeypadMinus", 0x4E}, {"F18", 0x4F}, {"F19", 0x50}, {"KeypadEquals", 0x51},
    {"Keypad0", 0x52}, {"Keypad1", 0x53}, {"Keypad2", 0x54}, {"Keypad3", 0x55},
    {"Keypad4", 0x56}, {"Keypad5", 0x57}, {"Keypad6", 0x58}, {"Keypad7", 0x59},
    {"F20", 0x5A}, {"Keypad8", 0x5B}, {"Keypad9", 0x5C},
    {"F5", 0x60}, {"F6", 0x61}, {"F7", 0x62}, {"F3", 0x63}, {"F8", 0x64}, {"F9", 0x65},
    {"F11", 0x67}, {"F13", 0x69}, {"F16", 0x6A}, {"F14", 0x6B}, {"F10", 0x6D},
    {"F12", 0x6F}, {"F15", 0x71}, {"Help", 0x72}, {"Home", 0x73}, {"PageUp", 0x74},
    {"ForwardDelete", 0x75}, {"F4", 0x76}, {"End", 0x77}, {"F2", 0x78},
    {"PageDown", 0x79}, {"F1", 0x7A}, {"Left", 0x7B}, {"Right", 0x7C},
    {"Down", 0x7D}, {"Up", 0x7E},
};
static const PimName pim_mouse_canon[] = {
    {"Left", 1}, {"Right", 2}, {"Middle", 4}, {"Button4", 8}, {"Button5", 16},
    {"Button6", 32}, {"Button7", 64}, {"Button8", 128},
    {"WheelUp", (int)PIM_MOUSE_WHEEL_UP}, {"WheelDown", (int)PIM_MOUSE_WHEEL_DOWN},
};

/* A key's name ("Space", or "0x0A" for a code without one); buf >= 8. */
static const char* pim_key_label(unsigned vk, char* buf, size_t n)
{
    for (int i = 0; i < PIM_COUNT(pim_key_canon); i++)
        if (pim_key_canon[i].id == (int)vk) return pim_key_canon[i].name;
    snprintf(buf, n, "0x%02X", vk & 0x7Fu);
    return buf;
}
/* A mouse input's name (one bit of PadInputMap.mouse), or NULL. */
static const char* pim_mouse_label(uint32_t bit)
{
    for (int i = 0; i < PIM_COUNT(pim_mouse_canon); i++)
        if ((uint32_t)pim_mouse_canon[i].id == bit) return pim_mouse_canon[i].name;
    return NULL;
}
/* A controller input's SDL name (bit number of PadInputMap.pad), or NULL:
 * the first entry with that id is the SDL one, the aliases come after. */
static const char* pim_pad_label(int bit)
{
    for (int i = 0; i < PIM_COUNT(pim_pad_inputs); i++)
        if (pim_pad_inputs[i].id == bit) return pim_pad_inputs[i].name;
    return NULL;
}

/* The inputs bound to control `ctl` of section `sec` (PIM_SEC_KEYBOARD,
 * _MOUSE_BUTTONS, _CONTROLLER), as the file writes them: "Return, KeypadEnter",
 * "" for none. */
static void pim_inputs_text(const PadInputMap* m, int sec, int ctl, char* out, size_t n)
{
    size_t len = 0;
    out[0] = 0;
    char tmp[8];
#define PIM_ADD(name) do { const char* nm_ = (name); \
        int w_ = snprintf(out + len, n > len ? n - len : 0, "%s%s", len ? ", " : "", nm_); \
        if (w_ > 0) len += (size_t)w_; if (len >= n) { len = n - 1; } } while (0)
    if (sec == PIM_SEC_KEYBOARD) {
        for (unsigned vk = 0; vk < 128; vk++)
            if (m->key[ctl][vk >> 5] >> (vk & 31) & 1u) PIM_ADD(pim_key_label(vk, tmp, sizeof tmp));
    } else if (sec == PIM_SEC_MOUSE_BUTTONS) {
        for (int b = 0; b < 32; b++)
            if (m->mouse[ctl] >> b & 1u) { const char* nm = pim_mouse_label(1u << b); if (nm) PIM_ADD(nm); }
    } else {
        for (int b = 0; b < 32; b++)
            if (m->pad[ctl] >> b & 1u) { const char* nm = pim_pad_label(b); if (nm) PIM_ADD(nm); }
    }
#undef PIM_ADD
}

/* A growing text buffer for the writer. */
typedef struct { char* p; size_t len, cap; int oom; } PimBuf;
static void pim_buf_add(PimBuf* b, const char* s, size_t n)
{
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > c) c *= 2;
        char* t = (char*)realloc(b->p, c);
        if (!t) { b->oom = 1; return; }
        b->p = t; b->cap = c;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void pim_buf_str(PimBuf* b, const char* s) { pim_buf_add(b, s, strlen(s)); }
/* `s` (not inside b) put at offset `at`. */
static void pim_buf_insert(PimBuf* b, size_t at, const char* s, size_t n)
{
    const size_t tail = b->len - at;
    pim_buf_add(b, s, n);                    /* room (and the terminator) */
    if (b->oom || !n) return;
    memmove(b->p + at + n, b->p + at, tail);
    memcpy(b->p + at, s, n);
}
/* The text ends with an empty line (or is empty). */
static int pim_buf_ends_blank(const PimBuf* b)
{
    size_t e = b->len;
    if (e && b->p[e - 1] == '\n') e--;
    if (e && b->p[e - 1] == '\r') e--;
    return e == 0 || b->p[e - 1] == '\n';
}

/* The writer's binding sections: 0 [keyboard], 1 [mouse_buttons], 2
 * [controller] (with the parser's other spellings); -1 any other. */
static int pim_binding_section(const char* name)
{
    if (!strcmp(name, "keyboard") || !strcmp(name, "keys")) return 0;
    if (!strcmp(name, "mouse_buttons") || !strcmp(name, "mouse.buttons")) return 1;
    if (!strcmp(name, "controller") || !strcmp(name, "gamepad") || !strcmp(name, "pad")) return 2;
    return -1;
}
static const int pim_binding_secs[3] = { PIM_SEC_KEYBOARD, PIM_SEC_MOUSE_BUTTONS, PIM_SEC_CONTROLLER };

/* "cross    = \"Space\"" + eol for every control not in *done, in the
 * file's order, put at offset `at`; then every control counts as done. */
static void pim_add_missing(PimBuf* b, size_t at, const PadInputMap* m, int s, uint32_t* done, const char* eol)
{
    PimBuf t = { NULL, 0, 0, 0 };
    char list[512], line[640];
    for (int i = 0; i < PIM_N_CONTROLS; i++) {
        const int c = pim_write_order[i];
        if (*done >> c & 1u) continue;
        pim_inputs_text(m, pim_binding_secs[s], c, list, sizeof list);
        snprintf(line, sizeof line, "%-8s = \"%s\"%s", pim_control_name(c), list, eol);
        pim_buf_str(&t, line);
    }
    if (t.oom) b->oom = 1;
    else if (t.len) pim_buf_insert(b, at, t.p, t.len);
    free(t.p);
    *done = (1u << PIM_N_CONTROLS) - 1u;
}

/* The file's text for map `m`, written into `old` (the file as it is, or
 * NULL): see above. malloc'd; NULL if out of memory. */
static char* pim_render(const PadInputMap* m, const char* old)
{
    static const char* const titles[3] = { "keyboard", "mouse_buttons", "controller" };
    PimBuf b = { NULL, 0, 0, 0 };
    char list[512];
    pim_buf_add(&b, "", 0);                  /* "" even when nothing follows */
    int fresh = 1;
    if (old) for (const char* q = old; *q; q++) if (!isspace((unsigned char)*q)) { fresh = 0; break; }
    const char* eol = "\n";
    if (!fresh) { const char* nl = strchr(old, '\n'); if (nl && nl > old && nl[-1] == '\r') eol = "\r\n"; }
    int seen[3] = { 0, 0, 0 };
    if (fresh) {
        pim_buf_str(&b, "# Controls, read by the game through PS3_INPUT_MAP (format: the toolkit's\n"
                        "# libs/input/pad_input_map.h). Written by the in-game menu (F2 > Controls);\n"
                        "# hand edits and comments are kept. \"\" unbinds a control.\n");
    } else {
        uint32_t done[3] = { 0, 0, 0 };
        int sec = -1, first = 0;             /* the binding section of this block; its first block */
        size_t ins_at = 0;                   /* after the block's last line (missing controls go there) */
        for (const char* p = old; *p; ) {
            const char* nl = strchr(p, '\n');
            const size_t ln = nl ? (size_t)(nl - p) : strlen(p);
            const int cr = ln && p[ln - 1] == '\r';
            const size_t n = cr ? ln - 1 : ln;          /* the line without its end */
            const char* le = cr ? "\r\n" : nl ? "\n" : eol;
            size_t i = 0;
            while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
            if (i < n && p[i] == '[') {
                if (sec >= 0 && first) pim_add_missing(&b, ins_at, m, sec, &done[sec], eol);
                char name[64]; size_t k = 0;
                for (size_t j = i + 1; j < n && p[j] != ']' && k + 1 < sizeof name; j++)
                    if (p[j] != ' ' && p[j] != '\t') name[k++] = (char)tolower((unsigned char)p[j]);
                name[k] = 0;
                sec = pim_binding_section(name);
                first = sec >= 0 && !seen[sec];
                if (sec >= 0) seen[sec] = 1;
                pim_buf_add(&b, p, n); pim_buf_str(&b, le);
                ins_at = b.len;
            } else if (sec >= 0 && i < n && p[i] != '#' && p[i] != ';') {
                /* the comment, as the parser cuts it ('#' or ';' outside quotes) */
                size_t cpos = n; char q = 0;
                for (size_t j = i; j < n; j++) {
                    if (q) { if (p[j] == q) q = 0; continue; }
                    if (p[j] == '"' || p[j] == '\'') q = p[j];
                    else if (p[j] == '#' || p[j] == ';') { cpos = j; break; }
                }
                const char* eq = (const char*)memchr(p + i, '=', cpos - i);
                int ctl = -1;
                if (eq) {                    /* the key as the parser reads it */
                    char key[64]; size_t k = 0;
                    for (const char* r = p + i; r < eq && k + 1 < sizeof key; r++)
                        if (*r != '"' && *r != '\'' && *r != '[' && *r != ']') key[k++] = (char)tolower((unsigned char)*r);
                    while (k && (key[k - 1] == ' ' || key[k - 1] == '\t')) k--;
                    key[k] = 0;
                    ctl = pim_lookup(pim_controls, PIM_COUNT(pim_controls), key);
                }
                if (ctl >= 0) {
                    const size_t pre = (size_t)(eq - p) + 1;     /* "cross    =" as written */
                    size_t ve = cpos;                            /* the old value's end */
                    while (ve > pre && (p[ve - 1] == ' ' || p[ve - 1] == '\t')) ve--;
                    pim_inputs_text(m, pim_binding_secs[sec], ctl, list, sizeof list);
                    pim_buf_add(&b, p, pre);
                    pim_buf_str(&b, " \""); pim_buf_str(&b, list); pim_buf_str(&b, "\"");
                    if (cpos < n) {
                        if (ve < cpos) pim_buf_add(&b, p + ve, cpos - ve); else pim_buf_str(&b, " ");
                        pim_buf_add(&b, p + cpos, n - cpos);
                    }
                    done[sec] |= 1u << ctl;
                } else {
                    pim_buf_add(&b, p, n);   /* not a control: as it was */
                }
                pim_buf_str(&b, le);
                ins_at = b.len;
            } else {
                pim_buf_add(&b, p, n); pim_buf_str(&b, le);
            }
            p = nl ? nl + 1 : p + ln;
        }
        if (sec >= 0 && first) pim_add_missing(&b, ins_at, m, sec, &done[sec], eol);
    }
    for (int s = 0; s < 3; s++) {
        if (seen[s]) continue;
        if (!pim_buf_ends_blank(&b)) pim_buf_str(&b, eol);
        pim_buf_str(&b, "["); pim_buf_str(&b, titles[s]); pim_buf_str(&b, "]"); pim_buf_str(&b, eol);
        uint32_t none = 0;
        pim_add_missing(&b, b.len, m, s, &none, eol);
    }
    if (b.oom) { free(b.p); return NULL; }
    return b.p;
}

#ifdef _WIN32
#include <direct.h>
#define PIM_MKDIR(d) _mkdir(d)
/* The temporary file over the old one in one step (MoveFileEx; declared here
 * so this header needs no <windows.h>; the same prototype as winbase.h's). */
#ifdef __cplusplus
extern "C"
#endif
__declspec(dllimport) int __stdcall MoveFileExA(const char* existing, const char* replacement, unsigned long flags);
#define PIM_REPLACE(tmp, path) (MoveFileExA((tmp), (path), 0x1u /* REPLACE_EXISTING */ | 0x8u /* WRITE_THROUGH */) != 0)
#else
#include <sys/stat.h>
#define PIM_MKDIR(d) mkdir(d, 0755)
#define PIM_REPLACE(tmp, path) (rename((tmp), (path)) == 0)
#endif

/* Write map `m` to `path` (a temporary file next to it, then moved over it in
 * one step, so a reader -- or a crash -- never sees half of it), keeping the
 * file's comments and other sections. 1 = written. */
static int pim_save_file(const PadInputMap* m, const char* path)
{
    char* old = NULL;
    int whole = 1;                            /* the old file read to its end */
    { FILE* f = fopen(path, "rb");
      if (f) {
          size_t cap = 4096, n = 0;
          old = (char*)malloc(cap);
          while (old) {
              if (n + 1025 > cap) {
                  char* t = cap >= (1u << 20) ? NULL : (char*)realloc(old, cap * 2);
                  if (!t) { whole = 0; break; }
                  old = t; cap *= 2;
              }
              const size_t r = fread(old + n, 1, 1024, f);
              n += r;
              if (r < 1024) break;
          }
          if (!old) whole = 0;
          else old[n] = 0;
          fclose(f);
      } }
    if (!whole) { free(old); return 0; }      /* over 1 MB, or no memory: never write half of it back */
    char* text = pim_render(m, old);
    free(old);
    if (!text) return 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    for (char* q = dir + 1; *q; q++)          /* mkdir -p of the folder */
        if (*q == '/' || *q == '\\') { const char sep = *q; *q = 0; PIM_MKDIR(dir); *q = sep; }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    int ok = 0;
    if (f) {
        const size_t n = strlen(text);
        ok = fwrite(text, 1, n, f) == n;
        ok = (fclose(f) == 0) && ok;
        ok = ok && PIM_REPLACE(tmp, path);
        if (!ok) remove(tmp);
    }
    free(text);
    return ok;
}

/* Bind `input` alone to control `ctl` of section `sec`, taking it off every
 * other control of that section (what the launcher's Controls screen does).
 * `input` is a key code, a mouse bit, or a controller bit number. Returns
 * the controls it was taken from, as a mask. */
static inline uint32_t pim_rebind(PadInputMap* m, int sec, int ctl, uint32_t input)
{
    uint32_t taken = 0;
    for (int c = 0; c < PIM_N_CONTROLS; c++) {
        if (sec == PIM_SEC_KEYBOARD) {
            const uint32_t w = input >> 5 & 3u, b = 1u << (input & 31);
            if (c != ctl && (m->key[c][w] & b)) { m->key[c][w] &= ~b; taken |= 1u << c; }
        } else if (sec == PIM_SEC_MOUSE_BUTTONS) {
            if (c != ctl && (m->mouse[c] & input)) { m->mouse[c] &= ~input; taken |= 1u << c; }
        } else {
            if (c != ctl && (m->pad[c] >> input & 1u)) { m->pad[c] &= ~(1u << input); taken |= 1u << c; }
        }
    }
    if (sec == PIM_SEC_KEYBOARD) { memset(m->key[ctl], 0, sizeof m->key[ctl]); pim_key_set(m->key[ctl], input & 127u); }
    else if (sec == PIM_SEC_MOUSE_BUTTONS) m->mouse[ctl] = input;
    else m->pad[ctl] = 1u << input;
    return taken;
}
/* No input on control `ctl` of section `sec`. */
static inline void pim_unbind(PadInputMap* m, int sec, int ctl)
{
    if (sec == PIM_SEC_KEYBOARD) memset(m->key[ctl], 0, sizeof m->key[ctl]);
    else if (sec == PIM_SEC_MOUSE_BUTTONS) m->mouse[ctl] = 0;
    else m->pad[ctl] = 0;
}

/* Any key of control `ctl` held in a 128-bit key state. */
static int pim_keys_hit(const PadInputMap* m, int ctl, const uint32_t keys[4])
{
    const uint32_t* k = m->key[ctl];
    return ((k[0] & keys[0]) | (k[1] & keys[1]) | (k[2] & keys[2]) | (k[3] & keys[3])) != 0;
}

#endif /* PS3RECOMP_PAD_INPUT_MAP_H */
