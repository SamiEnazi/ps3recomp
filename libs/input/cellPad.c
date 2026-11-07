/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <unistd.h>
#include <sched.h>
#include "../../include/ps3emu/log_first.h"   /* PS3_LOG_FIRST */
#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */
#include "pad_input_map.h"                  /* PS3_INPUT_MAP: keyboard/mouse/controller bindings */
#include "../../include/ps3emu/ps3_pad_icons.h"   /* the last device used, for the prompt glyphs */

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

/* PS3_INPUT_MAP=<file>: which keys, mouse buttons and controller buttons
 * drive which PS3 controls, and the mouse camera's settings (the format is
 * in pad_input_map.h). Read once, at the first cellPadInit; with no file, or
 * for whatever it leaves out, the built-in layout below applies unchanged.
 * The in-game menu rebinds controls live (cellPad_host_set_input_map): it
 * fills the copy not in use and flips to it, so a poll reading the map never
 * sees one half-written. */
static PadInputMap s_maps[2];
static volatile int s_map_cur;
static void pad_icons_register(void);
int cellPad_host_input_mode(void);
static int s_map_state;   /* 0 not loaded, 1 loaded */
static unsigned long long pad_now_ms(void);

static void pad_map_load(void)
{
    if (s_map_state) return;
    PadInputMap* m = &s_maps[0];
    const char* path = getenv("PS3_INPUT_MAP");
    if (path && *path) {
        if (pim_load_file(m, path, stderr))
            fprintf(stderr, "[cellPad] input map %s: %d change(s) from the defaults, %d name(s) ignored\n",
                    path, m->n_set, m->n_warnings);
        else
            fprintf(stderr, "[cellPad] input map %s: cannot open, using the default layout\n", path);
    } else {
        pim_defaults(m);
    }
    s_map_cur = 0;
    s_map_state = 1;
    pad_icons_register();
}

static const PadInputMap* pad_map(void)
{
    if (!s_map_state) pad_map_load();
    return &s_maps[__atomic_load_n(&s_map_cur, __ATOMIC_ACQUIRE)];
}

/* The in-game menu (ps3_overlay.mm): the live map, and a new one. Only the
 * bindings change; the [mouse] settings stay as they were loaded. */
void cellPad_host_input_map(PadInputMap* out) { *out = *pad_map(); }
void cellPad_host_set_input_map(const PadInputMap* m)
{
    const PadInputMap* cur = pad_map();
    const int next = s_map_cur ^ 1;
    PadInputMap* d = &s_maps[next];
    *d = *cur;
    memcpy(d->key, m->key, sizeof d->key);
    memcpy(d->mouse, m->mouse, sizeof d->mouse);
    memcpy(d->pad, m->pad, sizeof d->pad);
    __atomic_store_n(&s_map_cur, next, __ATOMIC_RELEASE);
}
/* Where the menu saves the bindings: PS3_INPUT_MAP; when that is not set,
 * input.toml next to PS3_OVERLAY_INI (the folder the launcher and run.sh
 * look in). Empty = not saved (PS3_INPUT_MAP set to ""). */
static void pad_map_save_path(char* out, size_t n)
{
    out[0] = 0;
    const char* e = getenv("PS3_INPUT_MAP");
    if (e) { snprintf(out, n, "%s", e); return; }
    const char* ini = getenv("PS3_OVERLAY_INI");
    if (!ini || !*ini) return;
    const char* slash = strrchr(ini, '/');
    if (!slash) { snprintf(out, n, "input.toml"); return; }
    snprintf(out, n, "%.*s/input.toml", (int)(slash - ini), ini);
}
void cellPad_host_input_map_path(char* out, size_t n) { pad_map_save_path(out, n); }
/* 1 saved, 0 nowhere to save, -1 could not write. */
int cellPad_host_save_input_map(void)
{
    char path[1024];
    pad_map_save_path(path, sizeof path);
    if (!path[0]) return 0;
    const int ok = pim_save_file(pad_map(), path);
    fprintf(stderr, "[cellPad] input map %s: %s\n", path, ok ? "saved" : "could not write it");
    return ok ? 1 : -1;
}

/* A stick axis from its two direction controls: centre (or `base`, the
 * physical stick) when neither is held, otherwise the keyboard's full
 * deflection (both held = 127, as two opposite keys always gave). */
static int pad_dir_axis(uint32_t ctl, int neg, int pos, int base)
{
    const int n = (ctl >> neg) & 1, p = (ctl >> pos) & 1;
    if (!n && !p) return base;
    return 128 + (p ? 127 : 0) - (n ? 128 : 0);
}

#if PAD_BACKEND_SDL2
/* PS3 port i's controller, by SDL instance id (-1 = none); the port keeps the
 * GUID of the last one it had so that pad gets the same port back. */
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static SDL_JoystickID      s_sdl_ids[PAD_MAX_HOST_PORTS] = { -1, -1, -1, -1 };
static SDL_JoystickGUID    s_sdl_last_guid[PAD_MAX_HOST_PORTS];
static int                 s_sdl_had[PAD_MAX_HOST_PORTS];
static int                 s_sdl_type[PAD_MAX_HOST_PORTS];   /* 1 + SDL_GameControllerType */
static unsigned long long  s_sdl_scan_ms;   /* last device-list scan (pad_now_ms) */
static int s_sdl_inited = 0;
#endif
/* Raw controller inputs of every pad (bit n = SDL button n, 21/22 the
 * triggers, as in pad_input_map.h), before the map: what the in-game menu
 * binds when the player presses a button to rebind. */
volatile uint32_t g_pad_overlay_raw;

/* One poll at a time: the guest polls from several threads, and a poll that
 * opens or closes a controller must not race another one using it.
 * cellPadSetActDirect takes it too (it talks to the controller). */
static volatile char s_poll_lock;
static int  pad_lock_try(void) { return !__atomic_test_and_set(&s_poll_lock, __ATOMIC_ACQUIRE); }
static void pad_lock(void)     { while (!pad_lock_try()) sched_yield(); }
static void pad_unlock(void)   { __atomic_clear(&s_poll_lock, __ATOMIC_RELEASE); }

/* CELL_PAD_STATUS_ASSIGN_CHANGES: a port whose reported connection changed
 * since the title last asked (cellPadGetInfo/GetInfo2/PeriphGetInfo), as
 * libpad reports a controller plugged in or taken away. */
static int s_port_reported[PAD_MAX_HOST_PORTS];
static int s_port_changed[PAD_MAX_HOST_PORTS];
static void pad_track_assign(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        const int on = s_host_state[i].connected || i == 0;   /* port 0: the virtual pad */
        if (on != s_port_reported[i]) { s_port_reported[i] = on; s_port_changed[i] = 1; }
    }
}
static u32 pad_status_take(u32 port)
{
    if (port >= PAD_MAX_HOST_PORTS) return 0;
    const int ch = __atomic_exchange_n(&s_port_changed[port], 0, __ATOMIC_RELAXED);
    return ch ? CELL_PAD_STATUS_ASSIGN_CHANGES : 0;
}

/* ---------------------------------------------------------------------------
 * Rumble (cellPadSetActDirect)
 *
 * A DualShock 3 has two motors: the small one (motor[0]) is on or off, the
 * large one (motor[1]) takes 0-255. A title sets them and they stay that way
 * until it sets them again -- inFAMOUS 2 only calls when the value changes.
 * SDL's rumble runs for a duration instead, so the last request per port is
 * kept and renewed from the pad poll while it is non-zero; if the title stops
 * polling (a hang, a closed window), the motors stop within PAD_RUMBLE_HOLD_MS.
 * Large -> SDL's low-frequency motor, small -> the high-frequency one, which
 * is how DualShock 4/DualSense and Xbox pads are laid out too.
 *
 *   PS3_RUMBLE=0                 no vibration at all
 *   PS3_RUMBLE_STRENGTH=0..200   percent applied to both motors (100); above
 *                                100 boosts a weak large-motor value, capped
 *                                at the motor's maximum
 *   PS3_RUMBLE_LOG=1             print every request the title makes
 * -------------------------------------------------------------------------*/

#define PAD_RUMBLE_HOLD_MS     1000   /* one SDL request lasts this long ... */
#define PAD_RUMBLE_RENEW_MS     400   /* ... and is renewed this often */

typedef struct {
    u16 lo, hi;        /* what the host motors were last asked for */
    u64 sent_ms;       /* when (GetTickCount64), 0 = nothing pending */
} PadRumble;
static PadRumble s_rumble[PAD_MAX_HOST_PORTS];

/* Strength in percent from PS3_RUMBLE / PS3_RUMBLE_STRENGTH: 0 = off. */
static int pad_rumble_strength_from(const char* on, const char* strength)
{
    if (on && on[0] == '0' && on[1] == 0) return 0;
    if (!strength || !*strength) return 100;
    char* end = NULL;
    long v = strtol(strength, &end, 10);
    if (end == strength) return 100;
    if (v < 0) v = 0;
    if (v > 200) v = 200;
    return (int)v;
}

static volatile int s_rumble_pct = -1;   /* file scope: the in-game menu sets it live */
static int pad_rumble_strength(void)
{
    int s = s_rumble_pct;
    if (s < 0) {
        s = pad_rumble_strength_from(getenv("PS3_RUMBLE"), getenv("PS3_RUMBLE_STRENGTH"));
        fprintf(stderr, "[cellPad] rumble %s", s ? "on" : "off (PS3_RUMBLE=0)");
        if (s && s != 100) fprintf(stderr, ", strength %d%%", s);
        fputc('\n', stderr);
        s_rumble_pct = s;
    }
    return s;
}
/* The in-game menu (ps3_overlay.mm): rumble strength in percent, 0 = off. */
int  cellPad_host_rumble_strength(void)       { return pad_rumble_strength(); }
void cellPad_host_set_rumble_strength(int pct) { s_rumble_pct = pct < 0 ? 0 : pct > 200 ? 200 : pct; }

/* The two host motor levels (0-65535) for a PS3 actuator request. */
static void pad_rumble_levels(u8 small_on, u8 large, int pct, u16* lo, u16* hi)
{
    unsigned long l = (unsigned long)large * 257ul * (unsigned long)pct / 100ul;
    unsigned long h = small_on ? 65535ul * (unsigned long)pct / 100ul : 0;
    *lo = (u16)(l > 65535ul ? 65535ul : l);
    *hi = (u16)(h > 65535ul ? 65535ul : h);
}

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Keyboard fallback for port 0.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Only fills in for port 0, only when XInput found nothing there, so a real
 * controller always wins and nothing changes for a port that has one. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too. */
#ifdef _WIN32
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void pad_poll_keyboard(void)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;

    PadHostState* hs = &s_host_state[0];
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        hs->buttons = 0;
        hs->analog_lx = hs->analog_ly = 128;
        hs->analog_rx = hs->analog_ry = 128;
        hs->connected = 1;
        return;
    }

    static const struct { int vk; u16 btn; } map[] = {
        { VK_UP,     CELL_PAD_CTRL_UP },      { VK_DOWN,  CELL_PAD_CTRL_DOWN },
        { VK_LEFT,   CELL_PAD_CTRL_LEFT },    { VK_RIGHT, CELL_PAD_CTRL_RIGHT },
        { 'Z',       CELL_PAD_CTRL_CROSS },   { 'X',      CELL_PAD_CTRL_CIRCLE },
        { 'A',       CELL_PAD_CTRL_SQUARE },  { 'S',      CELL_PAD_CTRL_TRIANGLE },
        { 'Q',       CELL_PAD_CTRL_L1 },      { 'W',      CELL_PAD_CTRL_R1 },
        { '1',       CELL_PAD_CTRL_L2 },      { '2',      CELL_PAD_CTRL_R2 },
        { VK_RETURN, CELL_PAD_CTRL_START },   { VK_TAB,   CELL_PAD_CTRL_SELECT },
    };

    u16 btns = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (GetAsyncKeyState(map[i].vk) & 0x8000) btns |= map[i].btn;

    hs->buttons   = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)((btns & CELL_PAD_CTRL_LEFT) ? 0 :
                         (btns & CELL_PAD_CTRL_RIGHT) ? 255 : 128);
    hs->analog_ly = (u8)((btns & CELL_PAD_CTRL_UP) ? 0 :
                         (btns & CELL_PAD_CTRL_DOWN) ? 255 : 128);
    hs->analog_rx = hs->analog_ry = 128;
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard fallback active on port 0 (no XInput device)\n");
          fflush(stdout); } }
}
#endif

static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* PS3_PAD_SDL=0: ignore physical controllers. SDL reads a pad whatever window
 * has focus, so a headless capture running beside a windowed game took the
 * player's START too and sat in the pause menu (tools/boot.sh sets it). */
static int pad_sdl_off(void)
{
    static int off = -1;
    if (off < 0) { const char* e = getenv("PS3_PAD_SDL"); off = (e && e[0] == '0') ? 1 : 0; }
    return off;
}

/* The analog value of a PS3 trigger from the SDL inputs bound to it. */
static u8 pad_sdl_analog(uint32_t bound, uint32_t in, u8 lt, u8 rt)
{
    u8 v = 0;
    if (bound & (1u << PIM_PAD_TRIGGER_LEFT)) v = lt;
    if ((bound & (1u << PIM_PAD_TRIGGER_RIGHT)) && rt > v) v = rt;
    if (bound & in & ((1u << PIM_PAD_TRIGGER_LEFT) - 1u)) v = 255;
    return v;
}

/* Send port i's rumble state to its controller (with a fresh duration). */
static void pad_sdl_rumble_send(int i)
{
    SDL_GameController* gc = s_sdl_controllers[i];
    if (!gc) return;
    PadRumble* r = &s_rumble[i];
    const int on = r->lo || r->hi;
    SDL_GameControllerRumble(gc, r->lo, r->hi, on ? PAD_RUMBLE_HOLD_MS : 0);
    r->sent_ms = on ? GetTickCount64() : 0;
}

/* ---- hot-plug ------------------------------------------------------------
 * A DualSense over Bluetooth drops off when it is switched off or idles, and
 * comes back as a NEW SDL device (a new instance id, maybe another index).
 * This used to open controllers by device index == PS3 port and look at the
 * index again on every poll. With Homebrew's SDL2, which is sdl2-compat over
 * SDL3, a device index is resolved through a list that is refreshed only by
 * SDL_NumJoysticks() or by the SDL2 event filter (installed with an SDL
 * window, which the Metal build never makes): the list kept the switched-off
 * pad's instance id, SDL_IsGameController(0) stayed false, and the pad that
 * came back was never opened until the game was restarted.
 *
 * Now each poll drains SDL's joystick/controller events (nothing else here
 * reads them, and they piled up in the queue), closes a controller SDL says
 * is gone, by its own handle, and on a device event -- or every
 * PAD_SDL_RESCAN_MS while a port is free, in case someone else ate the
 * event -- walks the device list from SDL_NumJoysticks() and opens every
 * game controller that is not open yet: into the port its GUID had last (the
 * same pad reconnecting gets its port back), else the lowest free one. A new
 * controller starts with its motors off and no rumble left over; vibration
 * strength and the input map are global, so they apply to it at once. The
 * port reads connected again on the next poll (cellPadGetInfo2 flags the
 * change with CELL_PAD_STATUS_ASSIGN_CHANGES for ports 1-3; port 0 is the
 * virtual pad and never reads disconnected). */
#define PAD_SDL_RESCAN_MS 1000

static int pad_sdl_port_of(SDL_JoystickID id)
{
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++)
        if (s_sdl_controllers[p] && s_sdl_ids[p] == id) return p;
    return -1;
}

/* Close port p's controller (gone, or shutting down). */
static void pad_sdl_close(int p, int gone)
{
    SDL_GameController* gc = s_sdl_controllers[p];
    if (!gc) return;
    if (gone) fprintf(stderr, "[cellPad] SDL controller on port %d disconnected (instance %d)\n", p, (int)s_sdl_ids[p]);
    else      SDL_GameControllerRumble(gc, 0, 0, 0);
    SDL_GameControllerClose(gc);
    s_sdl_controllers[p] = NULL;
    s_sdl_ids[p] = -1;
    memset(&s_rumble[p], 0, sizeof s_rumble[p]);
    s_host_state[p].connected = 0;
    /* The prompt glyphs: the device last used is gone, so they fall back to
     * another controller that is still there, else the keyboard. */
    int con = 0;
    for (int q = 0; q < PAD_MAX_HOST_PORTS && !con; q++) if (s_sdl_controllers[q]) con = s_sdl_type[q];
    g_ps3_pad_icons.connected = con;
    if (g_ps3_pad_icons.source == s_sdl_type[p]) g_ps3_pad_icons.source = con ? con : -1;
    s_sdl_type[p] = 0;
}

/* Open every game controller SDL lists that no port has yet. */
static void pad_sdl_scan(void)
{
    const int n = SDL_NumJoysticks();   /* also refreshes sdl2-compat's index list */
    for (int idx = 0; idx < n; idx++) {
        if (!SDL_IsGameController(idx)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(idx);
        if (id < 0 || pad_sdl_port_of(id) >= 0) continue;
        const SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(idx);
        int port = -1, back = 0;
        for (int p = 0; p < PAD_MAX_HOST_PORTS && port < 0; p++)
            if (!s_sdl_controllers[p] && s_sdl_had[p] && !memcmp(&s_sdl_last_guid[p], &guid, sizeof guid)) { port = p; back = 1; }
        for (int p = 0; p < PAD_MAX_HOST_PORTS && port < 0; p++)
            if (!s_sdl_controllers[p]) port = p;
        if (port < 0) return;                 /* every port has a controller */
        SDL_GameController* gc = SDL_GameControllerOpen(idx);
        if (!gc) {
            fprintf(stderr, "[cellPad] SDL controller %d: open failed: %s\n", idx, SDL_GetError());
            continue;
        }
        s_sdl_controllers[port] = gc;
        s_sdl_ids[port] = id;
        s_sdl_last_guid[port] = guid;
        s_sdl_had[port] = 1;
        memset(&s_rumble[port], 0, sizeof s_rumble[port]);   /* nothing left over from before */
        SDL_GameControllerRumble(gc, 0, 0, 0);
        s_sdl_type[port] = 1 + (int)SDL_GameControllerGetType(gc);
        if (!g_ps3_pad_icons.connected) g_ps3_pad_icons.connected = s_sdl_type[port];
        const char* name = SDL_GameControllerName(gc);
        fprintf(stderr, "[cellPad] SDL controller %s on port %d: %s (instance %d)%s\n",
                back ? "reconnected" : "connected", port, name ? name : "?", (int)id,
                SDL_GameControllerHasRumble(gc) ? "" : " (no rumble)");
    }
}

/* Device changes since the last poll. */
static void pad_sdl_hotplug(int force)
{
    int changed = force;
    SDL_Event ev[32];
    int got;
    /* SDL_JOYAXISMOTION (0x600) up to the touch events (0x700): every
     * joystick and game controller event, which only this file uses. */
    while ((got = SDL_PeepEvents(ev, 32, SDL_GETEVENT, SDL_JOYAXISMOTION, SDL_JOYAXISMOTION + 0xFF)) > 0)
        for (int i = 0; i < got; i++)
            if (ev[i].type == SDL_JOYDEVICEADDED || ev[i].type == SDL_JOYDEVICEREMOVED ||
                ev[i].type == SDL_CONTROLLERDEVICEADDED || ev[i].type == SDL_CONTROLLERDEVICEREMOVED)
                changed = 1;
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++)
        if (s_sdl_controllers[p] && !SDL_GameControllerGetAttached(s_sdl_controllers[p])) {
            pad_sdl_close(p, 1);
            changed = 1;
        }
    int free_port = 0;
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) if (!s_sdl_controllers[p]) free_port = 1;
    const unsigned long long now = pad_now_ms();
    if (!free_port) return;
    if (!changed && s_sdl_scan_ms && now - s_sdl_scan_ms < PAD_SDL_RESCAN_MS) return;
    s_sdl_scan_ms = now;
    pad_sdl_scan();
}

static void pad_poll_sdl2(void)
{
    if (pad_sdl_off()) {
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) s_host_state[i].connected = 0;
        return;
    }
    SDL_GameControllerUpdate();
    pad_sdl_hotplug(0);

    uint32_t raw_all = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc) {
            s_host_state[i].connected = 0;
            memset(&s_rumble[i], 0, sizeof s_rumble[i]);
            continue;
        }

        s_host_state[i].connected = 1;

        /* Keep a rumble the title set going (see PAD_RUMBLE_HOLD_MS). */
        if (s_rumble[i].sent_ms && GetTickCount64() - s_rumble[i].sent_ms >= PAD_RUMBLE_RENEW_MS)
            pad_sdl_rumble_send(i);

        /* Every SDL input, then the PS3 controls bound to them ([controller]
         * in PS3_INPUT_MAP; by default each button is its PS3 namesake and
         * the triggers are L2/R2, pressed past 3000). */
        const PadInputMap* map = pad_map();
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        uint32_t in = 0;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < PIM_PAD_TRIGGER_LEFT; b++)
            if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) in |= 1u << b;
        if (lt > 3000) in |= 1u << PIM_PAD_TRIGGER_LEFT;
        if (rt > 3000) in |= 1u << PIM_PAD_TRIGGER_RIGHT;
        raw_all |= in;
        /* The prompt glyphs follow the device last used: a button, a trigger
         * or a stick pushed well past its dead zone. */
        if (in || abs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)) > 16000 ||
            abs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)) > 16000 ||
            abs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX)) > 16000 ||
            abs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY)) > 16000)
            if (g_ps3_pad_icons.source != s_sdl_type[i] && cellPad_host_input_mode() != 2 /* keyboard only */)
                g_ps3_pad_icons.source = s_sdl_type[i];
        uint32_t ctl = 0;
        for (int c = 0; c < PIM_N_CONTROLS; c++)
            if (map->pad[c] & in) ctl |= 1u << c;
        u16 btns = (u16)(ctl & 0xFFFFu);

        s_host_state[i].buttons = btns;

        /* Analog sticks; a stick direction bound to a button pushes its axis
         * to the end while held. */
        s_host_state[i].analog_lx = (u8)pad_dir_axis(ctl, PIM_LS_LEFT, PIM_LS_RIGHT,
                                        pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)));
        s_host_state[i].analog_ly = (u8)pad_dir_axis(ctl, PIM_LS_UP, PIM_LS_DOWN,
                                        pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)));
        s_host_state[i].analog_rx = (u8)pad_dir_axis(ctl, PIM_RS_LEFT, PIM_RS_RIGHT,
                                        pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX)));
        s_host_state[i].analog_ry = (u8)pad_dir_axis(ctl, PIM_RS_UP, PIM_RS_DOWN,
                                        pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY)));

        /* Analog L2/R2: the trigger bound to it (the strongest, if both),
         * full for a bound button that is down. */
        s_host_state[i].trigger_l2 = pad_sdl_analog(map->pad[PIM_L2], in, pad_sdl_trigger_to_u8(lt), pad_sdl_trigger_to_u8(rt));
        s_host_state[i].trigger_r2 = pad_sdl_analog(map->pad[PIM_R2], in, pad_sdl_trigger_to_u8(lt), pad_sdl_trigger_to_u8(rt));

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
    g_pad_overlay_raw = raw_all;
}

static void pad_init_backend(void)
{
    if (pad_sdl_off()) return;
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));
    for (int p = 0; p < PAD_MAX_HOST_PORTS; p++) s_sdl_ids[p] = -1;

    const int num = SDL_NumJoysticks();
    for (int i = 0; i < num; i++)
        fprintf(stderr, "[cellPad] SDL joystick %d: %s (game controller: %s)\n", i,
                SDL_JoystickNameForIndex(i) ? SDL_JoystickNameForIndex(i) : "?",
                SDL_IsGameController(i) ? "yes" : "no");
    /* Open any controllers already connected */
    pad_sdl_hotplug(1);
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) pad_sdl_close(i, 0);
    memset(s_rumble, 0, sizeof s_rumble);
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* Port-0 state from a button mask and stick positions (0-255, centre 128). */
static void pad_apply(PadHostState* hs, u16 btns, int lx, int ly, int rx, int ry)
{
    hs->buttons = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)lx; hs->analog_ly = (u8)ly;
    hs->analog_rx = (u8)rx; hs->analog_ry = (u8)ry;
    hs->trigger_l2 = (btns & CELL_PAD_CTRL_L2) ? 255 : 0;
    hs->trigger_r2 = (btns & CELL_PAD_CTRL_R2) ? 255 : 0;
    const u8 full = 255;
    hs->press_up = (btns & CELL_PAD_CTRL_UP) ? full : 0;
    hs->press_down = (btns & CELL_PAD_CTRL_DOWN) ? full : 0;
    hs->press_left = (btns & CELL_PAD_CTRL_LEFT) ? full : 0;
    hs->press_right = (btns & CELL_PAD_CTRL_RIGHT) ? full : 0;
    hs->press_cross = (btns & CELL_PAD_CTRL_CROSS) ? full : 0;
    hs->press_circle = (btns & CELL_PAD_CTRL_CIRCLE) ? full : 0;
    hs->press_square = (btns & CELL_PAD_CTRL_SQUARE) ? full : 0;
    hs->press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? full : 0;
    hs->press_l1 = (btns & CELL_PAD_CTRL_L1) ? full : 0;
    hs->press_r1 = (btns & CELL_PAD_CTRL_R1) ? full : 0;
}

/* PS3_PAD_FILE=<path>: a scripted pad on port 0, for driving a title from a
 * shell or test script. The file holds whitespace-separated button names that
 * are held while they are in the file: UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE
 * TRIANGLE L1 R1 L2 R2 L3 R3 START SELECT, plus LX=n LY=n RX=n RY=n (0-255,
 * centre 128). Empty or missing file = nothing held. Re-read on every poll
 * (a few hundred bytes at most). Only used when no real pad is on port 0.
 * It ADDS to the keyboard rather than replacing it (pad_poll_keys merges the
 * two): replacing it left a person at the window unable to press anything
 * while a script drove the pad. */
static void pad_poll_file(u16* out_btns, int* out_lx, int* out_ly, int* out_rx, int* out_ry)
{
    static const char* path = (const char*)-1;
    if (path == (const char*)-1) path = getenv("PS3_PAD_FILE");
    *out_btns = 0; *out_lx = *out_ly = *out_rx = *out_ry = 128;
    if (!path) return;
    static u16 s_last_file_btns;
    char buf[512]; size_t n = 0;
    FILE* f = fopen(path, "r");
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = 0;
    static const struct { const char* name; u16 btn; } map[] = {
        {"UP", CELL_PAD_CTRL_UP}, {"DOWN", CELL_PAD_CTRL_DOWN},
        {"LEFT", CELL_PAD_CTRL_LEFT}, {"RIGHT", CELL_PAD_CTRL_RIGHT},
        {"CROSS", CELL_PAD_CTRL_CROSS}, {"CIRCLE", CELL_PAD_CTRL_CIRCLE},
        {"SQUARE", CELL_PAD_CTRL_SQUARE}, {"TRIANGLE", CELL_PAD_CTRL_TRIANGLE},
        {"L1", CELL_PAD_CTRL_L1}, {"R1", CELL_PAD_CTRL_R1},
        {"L2", CELL_PAD_CTRL_L2}, {"R2", CELL_PAD_CTRL_R2},
        {"L3", CELL_PAD_CTRL_L3}, {"R3", CELL_PAD_CTRL_R3},
        {"START", CELL_PAD_CTRL_START}, {"SELECT", CELL_PAD_CTRL_SELECT},
    };
    u16 btns = 0; int lx = 128, ly = 128, rx = 128, ry = 128;
    for (char* tok = strtok(buf, " \t\r\n,"); tok; tok = strtok(NULL, " \t\r\n,")) {
        if (!strncmp(tok, "LX=", 3)) { lx = atoi(tok + 3); continue; }
        if (!strncmp(tok, "LY=", 3)) { ly = atoi(tok + 3); continue; }
        if (!strncmp(tok, "RX=", 3)) { rx = atoi(tok + 3); continue; }
        if (!strncmp(tok, "RY=", 3)) { ry = atoi(tok + 3); continue; }
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(tok, map[i].name)) btns |= map[i].btn;
    }
    if (btns != s_last_file_btns) {
        printf("[cellPad] file pad: buttons 0x%04X\n", btns); fflush(stdout);
        s_last_file_btns = btns;
    }
    *out_btns = btns; *out_lx = lx; *out_ly = ly; *out_rx = rx; *out_ry = ry;
}

/* Keyboard pad (macOS window): the Metal window records key state by
 * virtual keycode in g_pad_host_keys and swallows the events (Shift, Control
 * and Option too, from their flag changes). The default layout -- one key,
 * one control, no overlaps, only keys every Mac keyboard has -- is
 *   left stick W/A/S/D      right stick I/J/K/L     d-pad: arrow keys
 *   CROSS Space  CIRCLE Q   SQUARE E   TRIANGLE R
 *   L1 U  R1 O  L2 Y  R2 P  L3 G  R3 H              START Return/Enter  SELECT Tab
 * and [keyboard] in PS3_INPUT_MAP rebinds any of it (pad_input_map.h). */
volatile uint32_t g_pad_host_keys[4];

/* Mouse camera (macOS window). The window adds motion in 1/16 px to
 * g_pad_mouse_dx/dy, wheel turns in 1/16 lines to g_pad_mouse_wheel (+ = up,
 * away from the player, whatever the scroll direction setting) and keeps
 * button bits in g_pad_mouse_buttons (1 left, 2 right, 4 middle, 8 button 4,
 * ...). A stick is a rate, a mouse a displacement, so the right stick
 * deflects in proportion to the mouse's speed since the last poll, with a
 * light smoothing so a frame without motion events does not snap it home,
 * and starts past the title's own dead zone so slow moves still turn.
 * Settings, the variable first, then [mouse] in PS3_INPUT_MAP:
 *   PS3_MOUSE=0              camera = false: off (keys I/J/K/L only; the
 *                            mouse buttons do nothing either)
 *   PS3_MOUSE_SENS=<f>       sensitivity: speed multiplier (default 1.0)
 *   PS3_MOUSE_INVERT_Y=1     invert_y: moving the mouse up looks down
 *   PS3_MOUSE_INVERT_X=1     invert_x: moving it right looks left
 *   PS3_MOUSE_DEADZONE=<n>   deadzone: first stick step past centre, 0-120 (30)
 *   PS3_MOUSE_CAPTURE=0      capture = false: no cursor capture, motion
 *                            counts whenever the window is key (by default a click in the game
 *                            captures the mouse and Esc/Cmd releases it,
 *                            see rsx_metal_backend.h)
 * Buttons by default: left = R1 (bolt), right = R2 (grenade), middle = L1
 * (aim); [mouse_buttons] rebinds them, the extra buttons and the wheel. */
volatile int32_t  g_pad_mouse_dx, g_pad_mouse_dy, g_pad_mouse_wheel;
volatile uint32_t g_pad_mouse_buttons;
static int pad_mouse_axis(float v, int dz)
{
    if (v > -0.5f && v < 0.5f) return 128;
    float m = (v < 0 ? -v : v) + (float)dz;
    if (m > 127.0f) m = 127.0f;
    return v < 0 ? 128 - (int)(m + 0.5f) - (m >= 127.0f ? 1 : 0) : 128 + (int)(m + 0.5f);
}
/* A wheel notch is a press: down for 70 ms, then up for 50 ms so the next
 * notch is a press of its own; up to 3 queued. */
typedef struct { int pending; unsigned long long down_until, up_until; } PadPulse;
static int pad_pulse(PadPulse* p, unsigned long long now)
{
    if (now < p->down_until) return 1;
    if (now < p->up_until) return 0;
    if (p->pending > 0) { p->pending--; p->down_until = now + 70; p->up_until = now + 120; return 1; }
    return 0;
}
/* An int setting: the variable, else the file's value (-1 = not there), else `def`. */
static int pad_env_int(const char* var, int file, int def)
{
    const char* e = getenv(var);
    return e ? atoi(e) : file >= 0 ? file : def;
}
/* Mouse buttons and wheel -> controls in *ctl; motion -> *mrx and *mry (-1 when
 * the mouse is not moving, so the keys keep the right stick). */
/* File scope so the in-game menu can read and change them live. */
static int s_mouse_on = -1, s_mouse_invert_x, s_mouse_invert_y, s_mouse_dz;
static float s_mouse_sens;
static void pad_mouse_settings_load(void)
{
    if (s_mouse_on >= 0) return;
    const PadInputMap* map = pad_map();
    const char* e = getenv("PS3_MOUSE_SENS");
    s_mouse_sens     = e ? (float)atof(e) : map->mouse_sens > 0.0f ? map->mouse_sens : 1.0f;
    if (!(s_mouse_sens > 0.0f)) s_mouse_sens = 1.0f;
    s_mouse_invert_x = pad_env_int("PS3_MOUSE_INVERT_X", map->mouse_invert_x, 0) != 0;
    s_mouse_invert_y = pad_env_int("PS3_MOUSE_INVERT_Y", map->mouse_invert_y, 0) != 0;
    s_mouse_dz       = pad_env_int("PS3_MOUSE_DEADZONE", map->mouse_deadzone, 30);
    if (s_mouse_dz < 0) s_mouse_dz = 0; if (s_mouse_dz > 120) s_mouse_dz = 120;
    s_mouse_on       = pad_env_int("PS3_MOUSE", map->mouse_camera, 1) != 0;
}
/* The in-game menu (ps3_overlay.mm): mouse camera sensitivity and inversion. */
void cellPad_host_mouse_settings(int* on, float* sens, int* invert_x, int* invert_y)
{
    pad_mouse_settings_load();
    *on = s_mouse_on; *sens = s_mouse_sens; *invert_x = s_mouse_invert_x; *invert_y = s_mouse_invert_y;
}
/* The prompt glyphs (ps3_pad_icons.h): the bindings and the mouse camera. */
static void pad_icons_map(void* out) { cellPad_host_input_map((PadInputMap*)out); }
static int  pad_icons_mouse_camera(void) { pad_mouse_settings_load(); return s_mouse_on; }
static void pad_icons_register(void)
{
    g_ps3_pad_icons.input_map = pad_icons_map;
    g_ps3_pad_icons.mouse_camera = pad_icons_mouse_camera;
}
void cellPad_host_set_mouse_settings(float sens, int invert_x, int invert_y)
{
    pad_mouse_settings_load();
    if (sens > 0.0f) s_mouse_sens = sens;
    s_mouse_invert_x = invert_x != 0;
    s_mouse_invert_y = invert_y != 0;
}
void cellPad_host_set_mouse_camera(int on)
{
    pad_mouse_settings_load();
    s_mouse_on = on != 0;
}

/* PS3_INPUT_MODE: what drives the game.
 *   both (default)   controllers, keyboard and mouse together
 *   controller       controllers only: keys and the mouse do nothing in the
 *                    game (the window's shortcuts and the menu still work)
 *   keyboard         keyboard and mouse only: controllers are ignored by the
 *                    game, though L3+R3 still opens the menu and moves in it
 * The in-game menu switches it live. PS3_PAD_FILE counts in every mode. */
enum { PAD_MODE_BOTH, PAD_MODE_CONTROLLER, PAD_MODE_KEYBOARD };
static volatile int s_input_mode = -1;
static int pad_input_mode_from(const char* e)
{
    if (!e || !*e) return PAD_MODE_BOTH;
    if (!strcasecmp(e, "controller") || !strcasecmp(e, "pad") || !strcasecmp(e, "gamepad")) return PAD_MODE_CONTROLLER;
    if (!strcasecmp(e, "keyboard") || !strcasecmp(e, "kbm") || !strcasecmp(e, "keyboard_mouse") ||
        !strcasecmp(e, "mouse")) return PAD_MODE_KEYBOARD;
    if (strcasecmp(e, "both") && strcasecmp(e, "all"))
        fprintf(stderr, "[cellPad] PS3_INPUT_MODE=%s: expected both, controller or keyboard; both\n", e);
    return PAD_MODE_BOTH;
}
int cellPad_host_input_mode(void)
{
    int m = s_input_mode;
    if (m < 0) { m = pad_input_mode_from(getenv("PS3_INPUT_MODE")); s_input_mode = m; }
    return m;
}
void cellPad_host_set_input_mode(int mode)
{
    s_input_mode = mode == PAD_MODE_CONTROLLER || mode == PAD_MODE_KEYBOARD ? mode : PAD_MODE_BOTH;
}
/* For the window: the mouse turns the camera right now (camera on, and the
 * input mode lets the mouse in). It captures the cursor only then. */
int cellPad_host_mouse_active(void)
{
    pad_mouse_settings_load();
    return s_mouse_on && cellPad_host_input_mode() != PAD_MODE_CONTROLLER;
}
static void pad_poll_mouse(uint32_t* ctl, int* mrx, int* mry)
{
    static float sx, sy; static unsigned long long last;
    static int32_t wheel_acc; static PadPulse wheel_up, wheel_down;
    const PadInputMap* map = pad_map();
    *mrx = *mry = -1;
    pad_mouse_settings_load();
    const int on = s_mouse_on, invert_x = s_mouse_invert_x, invert_y = s_mouse_invert_y, dz = s_mouse_dz;
    const float sens = s_mouse_sens;
    if (!on) return;
    const unsigned long long now = pad_now_ms();
    uint32_t b = g_pad_mouse_buttons;
    { const int32_t w = __atomic_exchange_n(&g_pad_mouse_wheel, 0, __ATOMIC_RELAXED);
      if ((w > 0 && wheel_acc < 0) || (w < 0 && wheel_acc > 0)) wheel_acc = 0;   /* turned back */
      wheel_acc += w;
      while (wheel_acc >= 16)  { wheel_acc -= 16; if (wheel_up.pending < 3)   wheel_up.pending++; }
      while (wheel_acc <= -16) { wheel_acc += 16; if (wheel_down.pending < 3) wheel_down.pending++; }
      if (pad_pulse(&wheel_up, now))   b |= PIM_MOUSE_WHEEL_UP;
      if (pad_pulse(&wheel_down, now)) b |= PIM_MOUSE_WHEEL_DOWN; }
    if (b)
        for (int c = 0; c < PIM_N_CONTROLS; c++)
            if (map->mouse[c] & b) *ctl |= 1u << c;
    const int32_t dx = __atomic_exchange_n(&g_pad_mouse_dx, 0, __ATOMIC_RELAXED);
    const int32_t dy = __atomic_exchange_n(&g_pad_mouse_dy, 0, __ATOMIC_RELAXED);
    unsigned long long dt = last ? now - last : 16;
    last = now;
    if (dt < 1) dt = 1;
    if (dt > 100) dt = 100;                 /* a long gap is not one slow move */
    /* px per ms; 1.5 px/ms (a brisk flick) reaches full deflection at SENS 1. */
    const float k = 127.0f / 1.5f * sens;
    const float vx = (float)dx / 16.0f / (float)dt * k * (invert_x ? -1.0f : 1.0f);
    const float vy = (float)dy / 16.0f / (float)dt * k * (invert_y ? -1.0f : 1.0f);
    sx = sx * 0.35f + vx * 0.65f;
    sy = sy * 0.35f + vy * 0.65f;
    if (sx > -0.5f && sx < 0.5f) sx = 0.0f;
    if (sy > -0.5f && sy < 0.5f) sy = 0.0f;
    if (sx != 0.0f) *mrx = pad_mouse_axis(sx, dz);
    if (sy != 0.0f) *mry = pad_mouse_axis(sy, dz);   /* NSEvent deltaY > 0 = down, as the stick */
    { static int logged; static int lrx = -1, lry = -1;
      static uint32_t lb;
      if (getenv("PS3_MOUSE_LOG") && logged < 400 && (*mrx != lrx || *mry != lry || b != lb)) {
          logged++; lrx = *mrx; lry = *mry; lb = b;
          printf("[cellPad] mouse: d=(%d,%d)/16 px in %llu ms -> rx=%d ry=%d buttons=0x%X\n",
                 dx, dy, dt, *mrx, *mry, b); fflush(stdout); } }
}
/* For the window (rsx_metal_backend.m): the mouse camera is on and wants
 * the cursor captured -- PS3_MOUSE / PS3_MOUSE_CAPTURE, else [mouse] camera
 * and capture in PS3_INPUT_MAP, else yes. Safe before cellPadInit. */
int cellPad_host_mouse_capture_wanted(void)
{
    const PadInputMap* map = pad_map();
    return pad_env_int("PS3_MOUSE", map->mouse_camera, 1) != 0 &&
           pad_env_int("PS3_MOUSE_CAPTURE", map->mouse_capture, 1) != 0;
}
/* The capture setting alone (the camera can be switched on later, from the
 * menu); the window asks cellPad_host_mouse_active before it captures. */
int cellPad_host_mouse_capture_setting(void)
{
    return pad_env_int("PS3_MOUSE_CAPTURE", pad_map()->mouse_capture, 1) != 0;
}
static void pad_poll_keys(int merge)
{
    const PadInputMap* map = pad_map();
    uint32_t keys[4];
    for (int i = 0; i < 4; i++) keys[i] = g_pad_host_keys[i];
    uint32_t ctl = 0;
    if (keys[0] | keys[1] | keys[2] | keys[3])
        for (int c = 0; c < PIM_N_CONTROLS; c++)
            if (pim_keys_hit(map, c, keys)) ctl |= 1u << c;
    int mrx, mry;
    pad_poll_mouse(&ctl, &mrx, &mry);   /* drained in every mode: no jump when the mouse comes back */
    if (cellPad_host_input_mode() == PAD_MODE_CONTROLLER) { ctl = 0; mrx = mry = -1; }
    else if ((ctl || mrx >= 0 || mry >= 0) && g_ps3_pad_icons.source != 0)
        g_ps3_pad_icons.source = 0;   /* the prompt glyphs: keys and mouse in use */
    u16 btns = (u16)(ctl & 0xFFFFu);
    /* Stick axes: 0 = left/up, 255 = right/down. */
    int lx = pad_dir_axis(ctl, PIM_LS_LEFT, PIM_LS_RIGHT, 128);
    int ly = pad_dir_axis(ctl, PIM_LS_UP, PIM_LS_DOWN, 128);
    int rx = pad_dir_axis(ctl, PIM_RS_LEFT, PIM_RS_RIGHT, 128);
    int ry = pad_dir_axis(ctl, PIM_RS_UP, PIM_RS_DOWN, 128);
    if (mrx >= 0) rx = mrx;   /* mouse motion wins over I/J/K/L while it moves */
    if (mry >= 0) ry = mry;
    /* PS3_PAD_FILE: its buttons are held in addition to the keys; an
     * off-centre file axis wins over the keyboard's. */
    { u16 fb; int flx, fly, frx, fry;
      pad_poll_file(&fb, &flx, &fly, &frx, &fry);
      btns |= fb;
      if (flx != 128) lx = flx;
      if (fly != 128) ly = fly;
      if (frx != 128) rx = frx;
      if (fry != 128) ry = fry; }
    PadHostState* hs = &s_host_state[0];
    /* With a real pad on port 0 (SDL filled hs this poll), the keyboard and
     * mouse add to it instead of being switched off: buttons are OR-ed, and
     * an axis the keys or mouse move wins over the pad's. A paired DualSense
     * that nobody was holding used to make every key do nothing. */
    if (merge) {
        btns |= hs->buttons;
        if (lx == 128) lx = hs->analog_lx;
        if (ly == 128) ly = hs->analog_ly;
        if (rx == 128) rx = hs->analog_rx;
        if (ry == 128) ry = hs->analog_ry;
        const u8 l2 = hs->trigger_l2, r2 = hs->trigger_r2;
        pad_apply(hs, btns, lx, ly, rx, ry);
        if (l2 > hs->trigger_l2) hs->trigger_l2 = l2;   /* keep an analog trigger's own value */
        if (r2 > hs->trigger_r2) hs->trigger_r2 = r2;
        return;
    }
    if (btns != hs->buttons) { printf("[cellPad] keyboard: buttons 0x%04X\n", btns); fflush(stdout); }
    pad_apply(hs, btns, lx, ly, rx, ry);
}

/* The in-game menu (libs/video/ps3_overlay.mm). Plain globals rather than
 * calls, so this file still builds on its own (tests/ include it):
 *   g_pad_overlay_open     set by the menu while it is open: every port is
 *                          held neutral (connected, nothing pressed)
 *   g_pad_overlay_toggle   counted up here when L3+R3 go down together on a
 *                          controller (PS3_OVERLAY_PAD=0: never); the menu
 *                          opens or closes on each change. While both are
 *                          held neither reaches the title.
 *   g_pad_overlay_nav      port 0's controller before it is held neutral
 *                          (buttons | lx << 16 | ly << 24; rx, ry in _nav2),
 *                          which the menu navigates with */
volatile int      g_pad_overlay_open;
volatile uint32_t g_pad_overlay_toggle;
volatile uint32_t g_pad_overlay_nav, g_pad_overlay_nav2;
static void pad_overlay_gate(void)
{
    static int combo_on = -1, held;
    if (combo_on < 0) { const char* e = getenv("PS3_OVERLAY_PAD"); combo_on = !(e && e[0] == '0'); }
    const u16 both = CELL_PAD_CTRL_L3 | CELL_PAD_CTRL_R3;
    int down = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected && (s_host_state[i].buttons & both) == both) down = 1;
    if (combo_on && down && !held) __atomic_add_fetch(&g_pad_overlay_toggle, 1, __ATOMIC_RELEASE);
    if (combo_on) held = down ? 1 : (held && (s_host_state[0].buttons & both)) ? 1 : 0;
    const PadHostState* p = &s_host_state[0];
    if (p->connected) {
        g_pad_overlay_nav  = p->buttons | ((uint32_t)p->analog_lx << 16) | ((uint32_t)p->analog_ly << 24);
        g_pad_overlay_nav2 = p->analog_rx | ((uint32_t)p->analog_ry << 8);
    } else {
        g_pad_overlay_nav = 0x80800000u; g_pad_overlay_nav2 = 0x8080u;
    }
    if (held)
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) s_host_state[i].buttons &= (u16)~both;
}
static void pad_overlay_neutral(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        PadHostState* hs = &s_host_state[i];
        const int c = hs->connected;
        memset(hs, 0, sizeof *hs);
        hs->connected = c;
        hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
    }
}

static void pad_poll_backend(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    if (!pad_lock_try()) return;   /* another thread is polling: its result serves this read */
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
#if defined(_WIN32) && PAD_BACKEND_XINPUT
    /* The XInput build's own keyboard fallback; the SDL build reads keys
     * through the input map below like every other host. */
    if (!s_host_state[0].connected) pad_poll_keyboard();
#endif
    pad_overlay_gate();   /* the controllers alone: before the keys join in */
    /* PS3_INPUT_MODE=keyboard: the controllers have done their part for the
     * menu (L3+R3, navigation); the game does not see them. */
    if (cellPad_host_input_mode() == PAD_MODE_KEYBOARD)
        for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
            memset(&s_host_state[i], 0, sizeof s_host_state[i]);
            s_host_state[i].analog_lx = s_host_state[i].analog_ly = 128;
            s_host_state[i].analog_rx = s_host_state[i].analog_ry = 128;
        }
    /* Keyboard, mouse and PS3_PAD_FILE always count; with a real pad on port 0
     * they are merged into it (pad_poll_keys). */
    pad_poll_keys(s_host_state[0].connected);
    if (g_pad_overlay_open) pad_overlay_neutral();
    pad_track_assign();
    pad_unlock();
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));

    pad_map_load();   /* PS3_INPUT_MAP, before the first poll reads it */
    pad_lock();
    pad_init_backend();
    pad_unlock();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();
    /* What is there at start is the baseline; only later changes are
     * flagged CELL_PAD_STATUS_ASSIGN_CHANGES. */
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        s_port_reported[i] = s_host_state[i].connected || i == 0;
        s_port_changed[i] = 0;
    }

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_lock();
    pad_shutdown_backend();
    pad_unlock();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second, each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * An optional third field, "<sec>:<mask>:<hold>", keeps the button down
     * for <hold> seconds of wall clock instead of one press: inFAMOUS 2's
     * "Hold L1 to aim" wants L1 down while the R1 presses after it happen.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK.
     * Bits 16..19 drive the left stick while the event is down -- 0x10000 up,
     * 0x20000 down, 0x40000 left, 0x80000 right -- so a hold walks: inFAMOUS 2
     * moves on the stick only, and its lit streets are a walk from the save.
     * Bits 20..23 drive the right stick the same way (0x100000 up, 0x200000
     * down, 0x400000 left, 0x800000 right): the camera, to look at something.
     * Up to 64 events (32 did not reach inFAMOUS 2's Beast fight). */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; double hold; int world; } ev[64]; static int n_ev = 0;
      /* A time written "w<sec>" counts from the moment the file named by
       * PAD_SCRIPT_WAIT appears (the draw engine creates it on the first
       * world frame, PS3RECOMP_ENG_WORLD_MARK): a walk scripted from the
       * start of gameplay lands on the same spot whatever the loading and
       * cutscene skips took. */
      static const char* wait_path; static unsigned long long t_world;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          wait_path = getenv("PAD_SCRIPT_WAIT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 64) {
                  int world = 0;
                  if (*p == 'w') { world = 1; p++; }
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  double h = 0;
                  if (*p == ':') { p++; h = strtod(p, (char**)&p); }
                  ev[n_ev].t = t; ev[n_ev].mask = m; ev[n_ev].hold = h; ev[n_ev].world = world; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)\n", n_ev);
          }
      }
      if (s_sc && port_no == 0) {
          double now = (double)(GetTickCount64() - t0) / 1000.0;
          if (wait_path && !t_world && access(wait_path, F_OK) == 0) {
              t_world = GetTickCount64();
              printf("[cellPad] PAD_SCRIPT: world clock starts at t=%.1fs\n", now); }
          const double now_world = t_world ? (double)(GetTickCount64() - t_world) / 1000.0 : -1.0;
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[64], held[64], stick_was;
          int stick_on = 0; unsigned char lx = 128, ly = 128, rx = 128, ry = 128;
          for (int i = 0; i < n_ev; i++) {
              const double tn = ev[i].world ? now_world : now;
              if (ev[i].world && now_world < 0) continue;
              if (!fired[i] && tn >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              const int down = held[i] > 0 ||
                  (ev[i].hold > 0 && tn >= ev[i].t && tn < ev[i].t + ev[i].hold);
              if (held[i] > 0) held[i]--;
              if (down) {
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  if (ev[i].mask & 0xFF0000u) {
                      stick_on = 1;
                      if (ev[i].mask & 0x40000u)  lx = 0;   if (ev[i].mask & 0x80000u)  lx = 255;
                      if (ev[i].mask & 0x10000u)  ly = 0;   if (ev[i].mask & 0x20000u)  ly = 255;
                      if (ev[i].mask & 0x400000u) rx = 0;   if (ev[i].mask & 0x800000u) rx = 255;
                      if (ev[i].mask & 0x100000u) ry = 0;   if (ev[i].mask & 0x200000u) ry = 255;
                  }
                  static int said[64];
                  if (!said[i]) { said[i] = 1;
                      if (ev[i].hold > 0)
                          printf("[cellPad] PAD_SCRIPT t=%.1fs hold 0x%04X for %.0fs\n", ev[i].t, ev[i].mask, ev[i].hold);
                      else
                          printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
          /* The stick words are copied from the host state below; a script
           * event owns them while it is down and centres them once after. */
          if (stick_on) { hs->analog_lx = lx; hs->analog_ly = ly; hs->analog_rx = rx; hs->analog_ry = ry; stick_was = 1; }
          else if (stick_was) { hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128; stick_was = 0; }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          if (held_n <= 0) {
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[64]; buf[0] = 0;
                  if (fgets(buf, sizeof buf, f)) {
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, 0, 0);
                      if (m) { held_mask = m; held_n = n > 0 ? n : 40;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          /* Stick bits as in PAD_SCRIPT (0x10000.. left, 0x100000.. right
           * stick): "0x10000 600" walks forward for 600 polls, so a driver can
           * steer a character by looking at screenshots between steps. */
          static int pf_stick;
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF);
              if (held_mask & 0xFF0000u) {
                  hs->analog_lx = (held_mask & 0x40000u) ? 0 : (held_mask & 0x80000u) ? 255 : 128;
                  hs->analog_ly = (held_mask & 0x10000u) ? 0 : (held_mask & 0x20000u) ? 255 : 128;
                  hs->analog_rx = (held_mask & 0x400000u) ? 0 : (held_mask & 0x800000u) ? 255 : 128;
                  hs->analog_ry = (held_mask & 0x100000u) ? 0 : (held_mask & 0x200000u) ? 255 : 128;
                  pf_stick = 1; } }
          else if (pf_stick) { hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128; pf_stick = 0; }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320").
       * Several holds, separated by ';', drive a tutorial that asks for one
       * stick after another in a single run ("...;128,128,128,0,450,456"). */
      enum { STICK_MAX = 16 };
      static unsigned char s_v[STICK_MAX][4];
      static double s_from[STICK_MAX], s_until[STICK_MAX]; static int s_n = 0;
      static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        for (const char* q = e; q && *q && s_n < STICK_MAX; ) {
            int a=128,b=128,c=128,d=128; double f = 0, u = 1e30;
            if (sscanf(q, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&f,&u) < 4) break;
            s_v[s_n][0]=(unsigned char)a; s_v[s_n][1]=(unsigned char)b;
            s_v[s_n][2]=(unsigned char)c; s_v[s_n][3]=(unsigned char)d;
            s_from[s_n] = f; s_until[s_n] = u; s_n++;
            q = strchr(q, ';'); if (q) q++;
        }
        if (s_n) printf("[cellPad] PAD_STICK: %d hold(s)\n", s_n); }
      if (s_st) { const double now = (double)(GetTickCount64() - s_t0) / 1000.0;
        for (int i = 0; i < s_n; i++)
            if (now >= s_from[i] && now < s_until[i]) {
                hs->analog_lx = s_v[i][0]; hs->analog_ly = s_v[i][1];
                hs->analog_rx = s_v[i][2]; hs->analog_ry = s_v[i][3];
                hs->connected = 1; break; } }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, (u8)((on ? CELL_PAD_STATUS_CONNECTED : 0) | pad_status_take(i)));
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED | pad_status_take(i);
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED | pad_status_take(i);
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED | pad_status_take(i);
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED | pad_status_take(i);
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if (PS3_LOG_FIRST(_n, 2))
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);

    const u8 small_on = param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL];
    const u8 large    = param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE];
    { static int log = -1; if (log < 0) log = getenv("PS3_RUMBLE_LOG") ? 1 : 0;
      if (log) fprintf(stderr, "[cellPad] SetActDirect port %u: small %u, large %u\n",
                       port_no, small_on, large); }
    if (port_no >= PAD_MAX_HOST_PORTS)
        return CELL_OK;

    u16 lo, hi;
    pad_rumble_levels(small_on, large, pad_rumble_strength(), &lo, &hi);
    PadRumble* r = &s_rumble[port_no];
    const int changed = r->lo != lo || r->hi != hi;
    r->lo = lo;
    r->hi = hi;

#if PAD_BACKEND_XINPUT
    /* XInput keeps the motors at what they were last set to: no renewal. */
    if (changed && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)lo;
        vib.wRightMotorSpeed = (WORD)hi;
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (changed) { pad_lock(); pad_sdl_rumble_send((int)port_no); pad_unlock(); }
#endif
    (void)changed;
    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
