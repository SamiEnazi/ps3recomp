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
#include "pad_stick.h"                      /* dead zones, anti-dead zone, saturation, squircle */
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
  #if defined(_WIN32)
    /* The Windows host side: keys by position (GetAsyncKeyState), the mouse
     * through raw input, and SDL's joystick windows pumped on a thread of
     * their own (pad_host_thread). */
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #pragma comment(lib, "user32.lib")
    #include "gamecontrollerdb_win.h"       /* SDL_GameControllerDB, Windows lines (zlib) */
  #endif
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
    /* SIXAXIS (DualShock 3 units, 0-1023, 512 = level; ~113 per g): from a
     * controller's accelerometer when it has one (has_sensor), else made up
     * in cellPadGetData (pad_tilt_fallback). */
    int  has_sensor;
    u16  sensor_x, sensor_y, sensor_z, sensor_g;
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
static unsigned long long pad_now_us(void);
static void pad_ctl_settings_load(const PadInputMap* m);
#if defined(_WIN32) && PAD_BACKEND_SDL2
static void pad_host_thread_start(void);
#endif

/* "<the exe's folder>/<name>" (Windows), else "" -- where input.toml and an
 * updated gamecontrollerdb.txt are looked for when no variable names them. */
static void pad_exe_dir_file(const char* name, char* out, size_t n)
{
    out[0] = 0;
#ifdef _WIN32
    char exe[1024];
    DWORD k = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (!k || k >= sizeof exe) return;
    char* slash = strrchr(exe, '\\');
    if (!slash) slash = strrchr(exe, '/');
    if (!slash) return;
    snprintf(out, n, "%.*s\\%s", (int)(slash - exe), exe, name);
#else
    (void)name; (void)n;
#endif
}
static int pad_file_exists(const char* p)
{
    FILE* f = p && *p ? fopen(p, "rb") : NULL;
    if (f) fclose(f);
    return f != NULL;
}

static void pad_map_load(void)
{
    if (s_map_state) return;
    PadInputMap* m = &s_maps[0];
    const char* path = getenv("PS3_INPUT_MAP");
    char exe_map[1100];
    /* No variable: input.toml next to the exe, if there is one. (Set to ""
     * = no file at all.) */
    if (!path) { pad_exe_dir_file("input.toml", exe_map, sizeof exe_map); if (pad_file_exists(exe_map)) path = exe_map; }
    if (path && *path) {
        if (pim_load_file(m, path, stderr))
            fprintf(stderr, "[cellPad] input map %s: %d change(s) from the defaults, %d name(s) ignored\n",
                    path, m->n_set, m->n_warnings);
        else
            fprintf(stderr, "[cellPad] input map %s: cannot open, using the default layout\n", path);
    } else {
        pim_defaults(m);
        fprintf(stderr, "[cellPad] input map: built-in layout (docs/INPUT.md; PS3_INPUT_MAP=<file> or input.toml next to the exe changes it)\n");
    }
    s_map_cur = 0;
    s_map_state = 1;
    pad_ctl_settings_load(m);
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
#ifdef _WIN32
    /* Windows: next to the exe, where pad_map_load looks for it. */
    if (!ini || !*ini) { pad_exe_dir_file("input.toml", out, n); return; }
#endif
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

/* ---- sticks and triggers ([controller_settings], pad_stick.h) -------------
 * Each setting: the variable, else the file, else the default.
 *   PS3_PAD_DEADZONE=<f>        radial dead zone of both sticks (0.08), or
 *   PS3_PAD_DEADZONE_L / _R     one stick; 0.08 and 8 (percent) are the same
 *   PS3_PAD_ANTI_DEADZONE[_L|_R]   output start past the dead zone (0)
 *   PS3_PAD_OUTER[_L|_R]        deflection that is already full (0.97)
 *   PS3_PAD_SQUIRCLE[_L|_R]     0 round .. 1 square diagonals (0.5)
 *   PS3_PAD_TRIGGER_THRESHOLD   digital L2/R2 press point (0.12)
 *   PS3_PAD_TRIGGER_DEADZONE    analog L2/R2 dead zone (0.02)
 *   PS3_PAD_SWAP_STICKS=1       left stick = camera, right stick = move
 *   PS3_PAD_INVERT=<list>       any of lx,ly,rx,ry: invert that PS3 axis */
typedef struct {
    PadStickShape shape[2];        /* 0 = PS3 left stick, 1 = right */
    float trig_threshold, trig_deadzone;
    int   source[2];               /* physical stick: 0 left, 1 right, 2 none */
    int   invert[2][2];
    int   tilt, tilt_invert;       /* SIXAXIS: 0 none, 1 motion sensor else left stick, 2 left stick */
} PadCtlSettings;
static PadCtlSettings s_ctl;

static float pad_env_frac(const char* var, const char* var2, float file, float def)
{
    const char* e = getenv(var);
    if (!(e && *e) && var2) e = getenv(var2);
    if (e && *e) {
        float f = (float)atof(e);
        if (f > 1.0f) f /= 100.0f;
        return f < 0.0f ? 0.0f : f;
    }
    return file >= 0.0f ? file : def;
}
static void pad_ctl_settings_load(const PadInputMap* m)
{
    static const char* const dz[2]   = { "PS3_PAD_DEADZONE_L", "PS3_PAD_DEADZONE_R" };
    static const char* const anti[2] = { "PS3_PAD_ANTI_DEADZONE_L", "PS3_PAD_ANTI_DEADZONE_R" };
    static const char* const out[2]  = { "PS3_PAD_OUTER_L", "PS3_PAD_OUTER_R" };
    static const char* const sq[2]   = { "PS3_PAD_SQUIRCLE_L", "PS3_PAD_SQUIRCLE_R" };
    PadCtlSettings c;
    memset(&c, 0, sizeof c);
    for (int s = 0; s < 2; s++) {
        PadStickShape sh;
        sh.deadzone      = pad_env_frac(dz[s], "PS3_PAD_DEADZONE", m->stick_deadzone[s], PAD_STICK_DEFAULT_DEADZONE);
        sh.anti_deadzone = pad_env_frac(anti[s], "PS3_PAD_ANTI_DEADZONE", m->stick_anti[s], PAD_STICK_DEFAULT_ANTI);
        sh.outer         = pad_env_frac(out[s], "PS3_PAD_OUTER", m->stick_outer[s], PAD_STICK_DEFAULT_OUTER);
        sh.squircle      = pad_env_frac(sq[s], "PS3_PAD_SQUIRCLE", m->stick_squircle[s], PAD_STICK_DEFAULT_SQUIRCLE);
        c.shape[s] = pad_stick_shape_fix(sh);
        c.source[s] = m->stick_source[s] >= 0 ? m->stick_source[s] : s;
        c.invert[s][0] = m->stick_invert[s][0] > 0;
        c.invert[s][1] = m->stick_invert[s][1] > 0;
    }
    { const char* e = getenv("PS3_PAD_SWAP_STICKS");
      if (e && *e && e[0] != '0') { c.source[0] = 1; c.source[1] = 0; } }
    { const char* e = getenv("PS3_PAD_INVERT");
      if (e) {
          static const char* const nm[4] = { "lx", "ly", "rx", "ry" };
          for (int k = 0; k < 4; k++) c.invert[k >> 1][k & 1] = strstr(e, nm[k]) != NULL;
      } }
    c.trig_threshold = pad_clampf(pad_env_frac("PS3_PAD_TRIGGER_THRESHOLD", NULL, m->trigger_threshold,
                                               PAD_TRIGGER_DEFAULT_THRESHOLD), 0.01f, 0.95f);
    c.trig_deadzone  = pad_clampf(pad_env_frac("PS3_PAD_TRIGGER_DEADZONE", NULL, m->trigger_deadzone,
                                               PAD_TRIGGER_DEFAULT_DEADZONE), 0.0f, 0.9f);
    c.tilt = m->tilt >= 0 ? m->tilt : 1;
    { const char* e = getenv("PS3_PAD_TILT");
      if (e && *e) c.tilt = !strcmp(e, "none") || !strcmp(e, "0") ? 0 : !strcmp(e, "left_stick") || !strcmp(e, "stick") ? 2 : 1; }
    c.tilt_invert = m->tilt_invert > 0;
    { const char* e = getenv("PS3_PAD_TILT_INVERT"); if (e && *e) c.tilt_invert = e[0] != '0'; }
    s_ctl = c;
    static const char* const src_name[3] = { "left", "right", "none" };
    fprintf(stderr, "[cellPad] sticks: left = physical %s (dead zone %.2f, anti %.2f, outer %.2f, squircle %.2f%s%s), "
                    "right = physical %s (dead zone %.2f, anti %.2f, outer %.2f, squircle %.2f%s%s); "
                    "L2/R2 press at %.2f, dead zone %.2f; tilt: %s%s\n",
            src_name[c.source[0]], c.shape[0].deadzone, c.shape[0].anti_deadzone, c.shape[0].outer, c.shape[0].squircle,
            c.invert[0][0] ? ", x inverted" : "", c.invert[0][1] ? ", y inverted" : "",
            src_name[c.source[1]], c.shape[1].deadzone, c.shape[1].anti_deadzone, c.shape[1].outer, c.shape[1].squircle,
            c.invert[1][0] ? ", x inverted" : "", c.invert[1][1] ? ", y inverted" : "",
            c.trig_threshold, c.trig_deadzone,
            c.tilt == 0 ? "none" : c.tilt == 2 ? "left stick" : "motion sensor, else left stick",
            c.tilt_invert ? " (inverted)" : "");
}

/* SIXAXIS in DualShock 3 units: 10 bits, 512 = level, ~113 counts per g
 * (a DS3 at rest reads X 512, Y 399, Z 512, gyro 512 -- the values RPCS3
 * uses too). */
#define PAD_SIXAXIS_ONE_G 113.0f
static u16 pad_sixaxis(float g)
{
    int v = 512 + (int)lrintf(g * PAD_SIXAXIS_ONE_G);
    return (u16)(v < 0 ? 0 : v > 1023 ? 1023 : v);
}
/* No motion sensor: tilt left/right from the PS3 left stick's X (Ionic
 * Storm steering: "hold ... then tilt the controller left and right"), a
 * full deflection = a 90-degree tilt; Y/Z/gyro level. */
static void pad_tilt_fallback(const PadHostState* hs, u16* x, u16* y, u16* z, u16* g)
{
    float t = 0.0f;
    if (s_ctl.tilt) t = ((float)hs->analog_lx - 128.0f) / 127.0f;
    if (s_ctl.tilt_invert) t = -t;
    *x = pad_sixaxis(pad_clampf(t, -1.0f, 1.0f));
    *y = pad_sixaxis(-1.0f);
    *z = 512; *g = 512;
}

/* One PS3 stick from the six SDL axes (lx ly rx ry lt rt). */
static void pad_stick_from_axes(int s, const int ax[6], int* out_x, int* out_y)
{
    const int src = s_ctl.source[s];
    if (src < 0 || src > 1) { *out_x = *out_y = 128; return; }
    int x = ax[src * 2], y = ax[src * 2 + 1];
    if (s_ctl.invert[s][0]) x = x <= -32767 ? 32767 : -x;
    if (s_ctl.invert[s][1]) y = y <= -32767 ? 32767 : -y;
    pad_stick_shape_apply(x, y, &s_ctl.shape[s], out_x, out_y);
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
static void pad_virtual_parse(void);
static void pad_virtual_attach(void);
static void pad_virtual_drive(void);
static int  s_virt_on;
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
        /* Through the same shaping as the SDL path ([controller_settings]):
         * XInput's +y is up, the PS3's (and SDL's) down. */
        (void)pad_xinput_stick_to_u8;
        { int ax[6], x, y;
          ax[0] = gp->sThumbLX; ax[1] = gp->sThumbLY <= -32767 ? 32767 : -gp->sThumbLY;
          ax[2] = gp->sThumbRX; ax[3] = gp->sThumbRY <= -32767 ? 32767 : -gp->sThumbRY;
          ax[4] = ax[5] = 0;
          pad_stick_from_axes(0, ax, &x, &y); s_host_state[i].analog_lx = (u8)x; s_host_state[i].analog_ly = (u8)y;
          pad_stick_from_axes(1, ax, &x, &y); s_host_state[i].analog_rx = (u8)x; s_host_state[i].analog_ry = (u8)y; }

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

/* PS3_PAD_SDL=0: ignore physical controllers. SDL reads a pad whatever window
 * has focus, so a headless capture running beside a windowed game took the
 * player's START too and sat in the pause menu (tools/boot.sh sets it). */
static int pad_sdl_off(void)
{
    static int off = -1;
    if (off < 0) { const char* e = getenv("PS3_PAD_SDL"); off = (e && e[0] == '0') ? 1 : 0; }
    return off;
}

/* The analog value of a PS3 trigger from the SDL inputs bound to it: a
 * trigger's own travel, full for a bound button or stick direction. */
static u8 pad_sdl_analog(uint32_t bound, uint32_t in, u8 lt, u8 rt)
{
    u8 v = 0;
    if (bound & (1u << PIM_PAD_TRIGGER_LEFT)) v = lt;
    if ((bound & (1u << PIM_PAD_TRIGGER_RIGHT)) && rt > v) v = rt;
    if (bound & in & ~PIM_PAD_TRIGGER_MASK) v = 255;
    return v;
}

static const char* pad_sdl_type_name(int t)
{
    static const char* const n[] = { "unknown", "Xbox 360", "Xbox One/Series", "PS3", "PS4", "Switch Pro",
                                     "virtual", "PS5", "Amazon Luna", "Google Stadia", "NVIDIA Shield",
                                     "Joy-Con (L)", "Joy-Con (R)", "Joy-Con pair" };
    return t >= 0 && t < (int)(sizeof n / sizeof n[0]) ? n[t] : "?";
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

/* One line per controller as it is opened: what it is and how SDL reads it
 * (the mapping SDL chose: built-in, SDL_GameControllerDB, or its HIDAPI
 * driver's own -- a GUID ending in 68xx is a HIDAPI device). */
static void pad_sdl_describe(SDL_GameController* gc, int port, int back, int id, SDL_JoystickGUID guid)
{
    const char* name = SDL_GameControllerName(gc);
    const int type = (int)SDL_GameControllerGetType(gc);
    char gs[40];
    SDL_JoystickGetGUIDString(guid, gs, (int)sizeof gs);
    const int hidapi = guid.data[14] == 'h';
    fprintf(stderr, "[cellPad] SDL controller %s on port %d: %s -- type %s (%s layout), USB id %04x:%04x, "
                    "guid %s%s, rumble %s, motion sensor %s (instance %d)\n",
            back ? "reconnected" : "connected", port, name ? name : "?", pad_sdl_type_name(type),
            pim_family_names[pim_family_of_sdl_type(type)],
            (unsigned)SDL_GameControllerGetVendor(gc), (unsigned)SDL_GameControllerGetProduct(gc),
            gs, hidapi ? " (SDL HIDAPI driver)" : "",
            SDL_GameControllerHasRumble(gc) ? "yes" : "no",
            SDL_GameControllerIsSensorEnabled(gc, SDL_SENSOR_ACCEL) ? "yes (tilt)" :
            SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL) ? "yes (unused)" : "no", id);
    char* map = SDL_GameControllerMapping(gc);
    if (map) {
        /* "<guid>,<name>,a:b0,b:b1,..." -- the bindings part tells which pad
         * layout SDL applied (a = bottom face button). */
        const char* p = strchr(map, ',');
        p = p ? strchr(p + 1, ',') : NULL;
        fprintf(stderr, "[cellPad]   mapping: %.200s\n", p ? p + 1 : map);
        SDL_free(map);
    }
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
        if (s_virt_on && guid.data[14] != 'v') continue;   /* PS3_PAD_VIRTUAL: the virtual pad only */
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
        /* SIXAXIS tilt from the pad's own accelerometer (DualShock 4,
         * DualSense, Switch Pro and most 8BitDo pads in Switch mode). */
        if (s_ctl.tilt == 1 && SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL))
            SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, SDL_TRUE);
        pad_sdl_describe(gc, port, back, (int)id, guid);
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
    pad_virtual_drive();          /* PS3_PAD_VIRTUAL: this poll's scripted inputs */
    SDL_GameControllerUpdate();   /* the pads' newest reports, read now */
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
         * in PS3_INPUT_MAP, with [controller.<family>] on top for this pad's
         * family; by default each button is its PS3 namesake by position and
         * the triggers are L2/R2, pressed past the trigger threshold). */
        const PadInputMap* map = pad_map();
        uint32_t bind[PIM_N_CONTROLS];
        pim_pad_resolve(map, pim_family_of_sdl_type(s_sdl_type[i] - 1), bind);
        int ax[SDL_CONTROLLER_AXIS_MAX];
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++)
            ax[a] = SDL_GameControllerGetAxis(gc, (SDL_GameControllerAxis)a);
        const int lt = ax[SDL_CONTROLLER_AXIS_TRIGGERLEFT], rt = ax[SDL_CONTROLLER_AXIS_TRIGGERRIGHT];
        uint32_t in = 0;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < PIM_PAD_TRIGGER_LEFT; b++)
            if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) in |= 1u << b;
        const int trig_on = (int)(s_ctl.trig_threshold * 32767.0f);
        if (lt > trig_on) in |= 1u << PIM_PAD_TRIGGER_LEFT;
        if (rt > trig_on) in |= 1u << PIM_PAD_TRIGGER_RIGHT;
        /* stick half-axes as buttons: past half way */
        for (int a = 0; a < 4; a++) {
            if (ax[a] < -16384) in |= 1u << (PIM_PAD_AXIS_FIRST + a * 2);
            if (ax[a] >  16384) in |= 1u << (PIM_PAD_AXIS_FIRST + a * 2 + 1);
        }
        raw_all |= in;
        /* The prompt glyphs follow the device last used: a button, a trigger
         * or a stick pushed well past its dead zone (the half-axis bits). */
        if (in)
            if (g_ps3_pad_icons.source != s_sdl_type[i] && cellPad_host_input_mode() != 2 /* keyboard only */)
                g_ps3_pad_icons.source = s_sdl_type[i];
        uint32_t ctl = 0;
        for (int c = 0; c < PIM_N_CONTROLS; c++)
            if (bind[c] & in) ctl |= 1u << c;
        u16 btns = (u16)(ctl & 0xFFFFu);

        s_host_state[i].buttons = btns;

        /* Analog sticks through the dead zone / saturation / squircle shaping
         * ([controller_settings]); a stick direction bound to a button pushes
         * its axis to the end while held. */
        { int x, y;
          pad_stick_from_axes(0, ax, &x, &y);
          s_host_state[i].analog_lx = (u8)pad_dir_axis(ctl, PIM_LS_LEFT, PIM_LS_RIGHT, x);
          s_host_state[i].analog_ly = (u8)pad_dir_axis(ctl, PIM_LS_UP, PIM_LS_DOWN, y);
          pad_stick_from_axes(1, ax, &x, &y);
          s_host_state[i].analog_rx = (u8)pad_dir_axis(ctl, PIM_RS_LEFT, PIM_RS_RIGHT, x);
          s_host_state[i].analog_ry = (u8)pad_dir_axis(ctl, PIM_RS_UP, PIM_RS_DOWN, y); }

        /* Analog L2/R2: the trigger bound to it (the strongest, if both),
         * full for a bound button that is down. */
        { const u8 l8 = (u8)pad_trigger_shape(lt, s_ctl.trig_deadzone), r8 = (u8)pad_trigger_shape(rt, s_ctl.trig_deadzone);
          s_host_state[i].trigger_l2 = pad_sdl_analog(bind[PIM_L2], in, l8, r8);
          s_host_state[i].trigger_r2 = pad_sdl_analog(bind[PIM_R2], in, l8, r8); }

        /* SIXAXIS from the accelerometer (SDL: m/s^2, +x right, +y up, +z
         * toward the player; at rest +1 g on y). DS3: 512 level, Y 399 at
         * rest, X rising as the pad rolls right. */
        s_host_state[i].has_sensor = 0;
        if (s_ctl.tilt == 1 && SDL_GameControllerIsSensorEnabled(gc, SDL_SENSOR_ACCEL)) {
            float a[3];
            if (SDL_GameControllerGetSensorData(gc, SDL_SENSOR_ACCEL, a, 3) == 0) {
                const float gx = a[0] / SDL_STANDARD_GRAVITY, gy = a[1] / SDL_STANDARD_GRAVITY, gz = a[2] / SDL_STANDARD_GRAVITY;
                s_host_state[i].sensor_x = pad_sixaxis(s_ctl.tilt_invert ? gx : -gx);
                s_host_state[i].sensor_y = pad_sixaxis(-gy);
                s_host_state[i].sensor_z = pad_sixaxis(-gz);
                s_host_state[i].sensor_g = 512;
                s_host_state[i].has_sensor = 1;
            }
        }

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

/* SDL hints the pad depends on, set in code before SDL's joystick subsystem
 * starts, so no launcher has to remember them. SDL_SetHint does not override
 * an environment variable of the same name: a player's own setting wins.
 *   SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1   SDL otherwise freezes controller
 *       state while none of ITS windows has keyboard focus (a game started
 *       from a console, the window behind another one)
 *   SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0  Nintendo-layout pads by position:
 *       the bottom button is Cross, as on a DualShock ([controller_settings]
 *       nintendo_layout = "label" turns it back on: A = Cross)
 *   SDL_JOYSTICK_HIDAPI_PS4_RUMBLE / _PS5_RUMBLE=1   rumble on a DualShock 4 /
 *       DualSense over Bluetooth (SDL's enhanced reports; the pad stays in
 *       that mode until it is switched off)
 *   SDL_JOYSTICK_THREAD=1   (Windows) SDL's device notifications and raw
 *       input for joysticks on a thread of SDL's own */
static void pad_sdl_hints(void)
{
    const PadInputMap* m = pad_map();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, m->nintendo_labels == 1 ? "1" : "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
#endif
}

/* Controller mappings beyond SDL's built-in list, after it (the later
 * mapping for a GUID wins over SDL's own; a mapping the player set with
 * SDL_GAMECONTROLLERCONFIG(_FILE) wins over all of these):
 *   1. SDL_GameControllerDB's Windows lines, compiled in
 *      (third_party/SDL_GameControllerDB, zlib license)
 *   2. gamecontrollerdb.txt next to the exe, if present (a newer download)
 *   3. PS3_PAD_DB=<file>[;<file>...]: the player's own mapping files */
static void pad_sdl_load_db(void)
{
    const int before = SDL_GameControllerNumMappings();
    int embedded = 0, files = 0;
#if defined(_WIN32)
    for (int i = 0; i < PAD_DB_WINDOWS_COUNT; i++)
        if (SDL_GameControllerAddMapping(kPadDbWindows[i]) >= 0) embedded++;
#endif
    char path[1100];
    pad_exe_dir_file("gamecontrollerdb.txt", path, sizeof path);
    if (pad_file_exists(path)) {
        const int n = SDL_GameControllerAddMappingsFromFile(path);
        fprintf(stderr, "[cellPad] controller mappings from %s: %d\n", path, n);
        if (n > 0) files += n;
    }
    const char* e = getenv("PS3_PAD_DB");
    if (e && *e) {
        char list[2048];
        snprintf(list, sizeof list, "%s", e);
        for (char *p = list, *next; p && *p; p = next) {
            next = strchr(p, ';');
            if (next) *next++ = 0;
            const int n = SDL_GameControllerAddMappingsFromFile(p);
            fprintf(stderr, "[cellPad] controller mappings from %s: %s%d\n", p, n < 0 ? "cannot read it, " : "", n < 0 ? 0 : n);
            if (n > 0) files += n;
        }
    }
    fprintf(stderr, "[cellPad] controller database: %d SDL built-in, %d SDL_GameControllerDB (compiled in), %d from files; %d known\n",
            before, embedded, files, SDL_GameControllerNumMappings());
}

/* SDL's game controller subsystem with the hints and the database. On
 * Windows this runs on the host input thread (pad_host_thread), so the
 * hidden windows SDL makes for device arrival/removal belong to a thread
 * that pumps messages: HIDAPI pads (DualShock, Switch Pro, 8BitDo in
 * Switch mode) hot-plug through them. */
static int pad_sdl_init_subsystem(void)
{
    if (s_sdl_inited) return 1;
    pad_sdl_hints();
    pad_virtual_parse();          /* PS3_PAD_VIRTUAL: before init (it turns the hardware drivers off) */
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0 && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[cellPad] SDL game controller init failed: %s\n", SDL_GetError());
        return 0;
    }
    pad_sdl_load_db();
    pad_virtual_attach();
    /* No joystick/controller EVENTS: the state is read directly at read
     * time (SDL_GameControllerUpdate in cellPadGetData), so the window's
     * event pump on the RSX thread neither updates the pads nor fills its
     * queue with their events; device changes are found by the rescan. */
    SDL_GameControllerEventState(SDL_IGNORE);
    SDL_JoystickEventState(SDL_IGNORE);
    s_sdl_inited = 1;
    return 1;
}

/* ---- PS3_PAD_VIRTUAL: a scripted controller through the REAL SDL path ------
 *   PS3_PAD_VIRTUAL="<sec>:<input>=<value>,<sec>:<input>=<value>,..."
 * attaches an SDL virtual game controller (an "Xbox One" pad: family xbox)
 * and sets its inputs at the given seconds after cellPadInit; everything
 * after that -- SDL's mapping, [controller] bindings, dead zones, triggers,
 * the trace -- is the code a real pad goes through. Inputs: SDL button names
 * (a b x y back guide start leftstick rightstick leftshoulder rightshoulder
 * dpup dpdown dpleft dpright misc1 paddle1-4 touchpad; 0/1) and axes (leftx
 * lefty rightx righty lefttrigger righttrigger; -32768..32767, triggers
 * 0..32767). SDL's hardware drivers are switched off in this mode, so a real
 * pad is neither used nor touched (a test run beside someone playing). */
static struct { double t; int axis; int id; int value; } s_virt_ev[64];
static int s_virt_n, s_virt_next;
static SDL_Joystick* s_virt_js;
static unsigned long long s_virt_t0;
static void pad_virtual_parse(void)
{
    const char* e = getenv("PS3_PAD_VIRTUAL");
    if (!e || !*e) return;
    static const char* const axes[] = { "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger" };
    for (const char* p = e; *p && s_virt_n < 64; ) {
        char* end;
        const double t = strtod(p, &end);
        if (end == p || *end != ':') break;
        p = end + 1;
        char name[32]; int k = 0;
        while (*p && *p != '=' && k < 31) name[k++] = (char)tolower((unsigned char)*p++);
        name[k] = 0;
        if (*p != '=') break;
        const long v = strtol(p + 1, &end, 0);
        p = end;
        int axis = 0, id = -1;
        for (int a = 0; a < 6; a++) if (!strcmp(name, axes[a])) { axis = 1; id = a; }
        if (id < 0) { const int b = pim_lookup(pim_pad_inputs, PIM_COUNT(pim_pad_inputs), name); if (b >= 0 && b <= 20) id = b; }
        if (id < 0) fprintf(stderr, "[cellPad] PS3_PAD_VIRTUAL: unknown input '%s' (ignored)\n", name);
        else { s_virt_ev[s_virt_n].t = t; s_virt_ev[s_virt_n].axis = axis; s_virt_ev[s_virt_n].id = id;
               s_virt_ev[s_virt_n].value = (int)v; s_virt_n++; }
        while (*p == ',' || *p == ' ') p++;
    }
    s_virt_on = 1;
    /* no hardware: only the virtual pad exists for SDL */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
}
static void pad_virtual_attach(void)
{
    if (!s_virt_on) return;
    SDL_VirtualJoystickDesc d;
    memset(&d, 0, sizeof d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.vendor_id = 0x045E; d.product_id = 0x02EA;      /* Xbox One controller */
    d.name = "ps3recomp virtual pad (PS3_PAD_VIRTUAL)";
    const int idx = SDL_JoystickAttachVirtualEx(&d);
    if (idx < 0) { fprintf(stderr, "[cellPad] PS3_PAD_VIRTUAL: attach failed: %s\n", SDL_GetError()); s_virt_on = 0; return; }
    /* SDL's own mapping for a virtual pad stops at paddle4: give it every
     * button, the touchpad included, in SDL_GameControllerButton order. */
    { char gs[40], map[640];
      SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(idx), gs, (int)sizeof gs);
      snprintf(map, sizeof map, "%s,%s,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,rightstick:b8,"
               "leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,misc1:b15,paddle1:b16,"
               "paddle2:b17,paddle3:b18,paddle4:b19,touchpad:b20,leftx:a0,lefty:a1,rightx:a2,righty:a3,"
               "lefttrigger:a4,righttrigger:a5", gs, d.name);
      SDL_GameControllerAddMapping(map); }
    s_virt_js = SDL_JoystickOpen(idx);
    s_virt_t0 = pad_now_us();
    fprintf(stderr, "[cellPad] PS3_PAD_VIRTUAL: virtual pad attached (device %d), %d event(s); real controllers off\n", idx, s_virt_n);
}
static void pad_virtual_drive(void)
{
    if (!s_virt_js) return;
    const double now = (double)(pad_now_us() - s_virt_t0) / 1e6;
    while (s_virt_next < s_virt_n && s_virt_ev[s_virt_next].t <= now) {
        const int i = s_virt_next++;
        int v = s_virt_ev[i].value;
        /* a virtual trigger axis is full range (-32768 = released): the
         * script's 0..32767 is spread over it */
        if (s_virt_ev[i].axis && s_virt_ev[i].id >= SDL_CONTROLLER_AXIS_TRIGGERLEFT) v = v * 2 - 32768;
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        if (s_virt_ev[i].axis) SDL_JoystickSetVirtualAxis(s_virt_js, s_virt_ev[i].id, (Sint16)v);
        else SDL_JoystickSetVirtualButton(s_virt_js, s_virt_ev[i].id, (Uint8)(s_virt_ev[i].value != 0));
        printf("[cellPad] PS3_PAD_VIRTUAL t=%.2fs: %s %d = %d\n", now, s_virt_ev[i].axis ? "axis" : "button",
               s_virt_ev[i].id, s_virt_ev[i].value);
        fflush(stdout);
    }
}

static void pad_init_backend(void)
{
#if defined(_WIN32)
    pad_host_thread_start();      /* SDL's init and the raw mouse, on their own thread */
#endif
    if (pad_sdl_off()) return;
#if !defined(_WIN32)
    pad_sdl_init_subsystem();
#endif
    if (!s_sdl_inited) return;
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
 * including one sampling for button-press edges.
 *
 * Now 1 ms by default, on the performance counter: the old 4 ms gate ran on
 * GetTickCount64, which on Windows moves in 15.6 ms steps -- every read in
 * the same tick got the cached state, up to a frame old at 60 fps and more
 * above it. inFAMOUS 2 reads once a frame, so the host is sampled at that
 * read (SDL_GameControllerUpdate there), never from a stale cache.
 *   PS3_PAD_POLL_US=<us>   the minimum gap between two host polls (1000;
 *                          0 = poll on every read) */
#define PAD_POLL_INTERVAL_MS 1
static unsigned long long pad_poll_interval_us(void)
{
    static long long us = -1;
    if (us < 0) { const char* e = getenv("PS3_PAD_POLL_US"); us = e && *e ? atoll(e) : 1000; if (us < 0) us = 0; }
    return (unsigned long long)us;
}

/* Microseconds, monotonic, on the performance counter (Windows) -- not
 * GetTickCount64 (15.6 ms steps). */
static unsigned long long pad_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    const unsigned long long q = (unsigned long long)c.QuadPart, fr = (unsigned long long)f.QuadPart;
    return q / fr * 1000000ull + q % fr * 1000000ull / fr;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
#endif
}
static unsigned long long pad_now_ms(void) { return pad_now_us() / 1000ull; }

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

/* Keyboard pad. Keys are macOS virtual key codes (US positions) everywhere:
 * the Metal window records key state in g_pad_host_keys; on Windows the
 * keys are read at the poll itself, by position (pad_win_keys), while a
 * window of this process has the focus. The default layout is
 * pim_defaults' (docs/INPUT.md) and [keyboard] in PS3_INPUT_MAP rebinds any
 * of it (pad_input_map.h). */
volatile uint32_t g_pad_host_keys[4];

/* Mouse camera. The window (macOS) or the raw-input thread (Windows,
 * pad_host_thread) adds motion in 1/16 px to
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
 *                            see rsx_metal_backend.h; on Windows the
 *                            mouse counts only while captured)
 * Buttons by default: left = R1 (bolt while aiming), right = L1 (aim),
 * middle = R3, button 4 = R2 (Kinetic Pulse), button 5 = L2 (Drain), wheel
 * up = D-pad up (Precision Mode); [mouse_buttons] rebinds them. */
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
static void pad_icons_map(void* out)
{
    PadInputMap* m = (PadInputMap*)out;
    cellPad_host_input_map(m);
    /* the controller glyphs: the bindings of the family of the pad in use */
    const int src = g_ps3_pad_icons.source > 0 ? g_ps3_pad_icons.source : g_ps3_pad_icons.connected;
    if (src > 0) {
        uint32_t r[PIM_N_CONTROLS];
        pim_pad_resolve(m, pim_family_of_sdl_type(src - 1), r);
        memcpy(m->pad, r, sizeof r);
    }
}
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
    const unsigned long long now_us = pad_now_us(), now = now_us / 1000ull;
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
    /* The time since the last read, in ms from the performance counter (it
     * was GetTickCount64: 15.6 ms steps on Windows, so a 16.7 ms frame read
     * as 15 or 31 ms and the camera speed jumped by 2x from frame to frame). */
    float dt = last ? (float)(now_us - last) / 1000.0f : 16.0f;
    last = now_us;
    if (dt < 1.0f) dt = 1.0f;
    if (dt > 100.0f) dt = 100.0f;           /* a long gap is not one slow move */
    /* px per ms; 1.5 px/ms (a brisk flick) reaches full deflection at SENS 1. */
    const float k = 127.0f / 1.5f * sens;
    const float vx = (float)dx / 16.0f / dt * k * (invert_x ? -1.0f : 1.0f);
    const float vy = (float)dy / 16.0f / dt * k * (invert_y ? -1.0f : 1.0f);
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
          printf("[cellPad] mouse: d=(%d,%d)/16 px in %.1f ms -> rx=%d ry=%d buttons=0x%X\n",
                 dx, dy, (double)dt, *mrx, *mry, b); fflush(stdout); } }
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

/* ---- the real keyboard and mouse on Windows -------------------------------
 * PS3_KBM=0 keeps them out of the game. A test run (PS3_PAD_SDL=0: real
 * controllers ignored) keeps them out too unless PS3_KBM=1: the game window
 * can take the focus while someone types in another one. PAD_NO_KEYBOARD=1
 * (the XInput build's old switch) = off as well. */
int cellPad_host_overlay_open(void);
static int pad_kbm_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3_KBM");
        const char* sdl = getenv("PS3_PAD_SDL");
        int v;
        if (e && *e) v = e[0] != '0';
        else if (getenv("PAD_NO_KEYBOARD")) v = 0;
        else v = !(sdl && sdl[0] == '0');
        fprintf(stderr, "[cellPad] keyboard and mouse: %s\n",
                v ? "on" : "off (PS3_KBM=0, PAD_NO_KEYBOARD, or a test run's PS3_PAD_SDL=0; PS3_KBM=1 forces them on)");
        on = v;
    }
    return on;
}
/* Is macOS key code vk bound to any PS3 control? */
static int pad_key_bound(unsigned vk)
{
    const PadInputMap* m = pad_map();
    for (int c = 0; c < PIM_N_CONTROLS; c++)
        if (m->key[c][(vk >> 5) & 3] >> (vk & 31) & 1u) return 1;
    return 0;
}

#if defined(_WIN32) && PAD_BACKEND_SDL2
static int pad_win_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Keys by POSITION: scan code (set 1) -> the macOS key code of the US key at
 * that spot, resolved to the current layout's virtual key at the poll (so W
 * is the key left of E on AZERTY too, like the Mac's key codes). */
static const unsigned char kWinScanMac[][2] = {
    {0x29, 0x32}, {0x02, 0x12}, {0x03, 0x13}, {0x04, 0x14}, {0x05, 0x15}, {0x06, 0x17}, {0x07, 0x16},
    {0x08, 0x1A}, {0x09, 0x1C}, {0x0A, 0x19}, {0x0B, 0x1D}, {0x0C, 0x1B}, {0x0D, 0x18},
    {0x10, 0x0C}, {0x11, 0x0D}, {0x12, 0x0E}, {0x13, 0x0F}, {0x14, 0x11}, {0x15, 0x10}, {0x16, 0x20},
    {0x17, 0x22}, {0x18, 0x1F}, {0x19, 0x23}, {0x1A, 0x21}, {0x1B, 0x1E},
    {0x1E, 0x00}, {0x1F, 0x01}, {0x20, 0x02}, {0x21, 0x03}, {0x22, 0x05}, {0x23, 0x04}, {0x24, 0x26},
    {0x25, 0x28}, {0x26, 0x25}, {0x27, 0x29}, {0x28, 0x27}, {0x2B, 0x2A},
    {0x2C, 0x06}, {0x2D, 0x07}, {0x2E, 0x08}, {0x2F, 0x09}, {0x30, 0x0B}, {0x31, 0x2D}, {0x32, 0x2E},
    {0x33, 0x2B}, {0x34, 0x2F}, {0x35, 0x2C},
    {0x56, 0x0A},                                             /* ISO key left of Z */
};
/* Keys whose virtual key is the same on every layout. */
static const unsigned char kWinVkMac[][2] = {
    {VK_RETURN, 0x24}, {VK_TAB, 0x30}, {VK_SPACE, 0x31}, {VK_BACK, 0x33}, {VK_ESCAPE, 0x35},
    {VK_LSHIFT, 0x38}, {VK_RSHIFT, 0x3C}, {VK_LCONTROL, 0x3B}, {VK_RCONTROL, 0x3E},
    {VK_LMENU, 0x3A}, {VK_RMENU, 0x3D}, {VK_CAPITAL, 0x39},
    {VK_F1, 0x7A}, {VK_F2, 0x78}, {VK_F3, 0x63}, {VK_F4, 0x76}, {VK_F5, 0x60}, {VK_F6, 0x61},
    {VK_F7, 0x62}, {VK_F8, 0x64}, {VK_F9, 0x65}, {VK_F10, 0x6D}, {VK_F11, 0x67}, {VK_F12, 0x6F},
    {VK_F13, 0x69}, {VK_F14, 0x6B}, {VK_F15, 0x71}, {VK_F16, 0x6A}, {VK_F17, 0x40}, {VK_F18, 0x4F},
    {VK_F19, 0x50}, {VK_F20, 0x5A},
    {VK_SNAPSHOT, 0x69}, {VK_SCROLL, 0x6B}, {VK_PAUSE, 0x71},
    {VK_INSERT, 0x72}, {VK_HOME, 0x73}, {VK_PRIOR, 0x74}, {VK_DELETE, 0x75}, {VK_END, 0x77}, {VK_NEXT, 0x79},
    {VK_LEFT, 0x7B}, {VK_RIGHT, 0x7C}, {VK_DOWN, 0x7D}, {VK_UP, 0x7E},
    {VK_NUMPAD0, 0x52}, {VK_NUMPAD1, 0x53}, {VK_NUMPAD2, 0x54}, {VK_NUMPAD3, 0x55}, {VK_NUMPAD4, 0x56},
    {VK_NUMPAD5, 0x57}, {VK_NUMPAD6, 0x58}, {VK_NUMPAD7, 0x59}, {VK_NUMPAD8, 0x5B}, {VK_NUMPAD9, 0x5C},
    {VK_DECIMAL, 0x41}, {VK_MULTIPLY, 0x43}, {VK_ADD, 0x45}, {VK_NUMLOCK, 0x47}, {VK_DIVIDE, 0x4B},
    {VK_SUBTRACT, 0x4E}, {VK_APPS, 0x6E},
};
#define PAD_N_SCAN_KEYS ((int)(sizeof kWinScanMac / sizeof kWinScanMac[0]))
#define PAD_N_VK_KEYS   ((int)(sizeof kWinVkMac / sizeof kWinVkMac[0]))

/* The bound keys that are down, read now (GetAsyncKeyState: the keyboard's
 * state at this instant, not a queue the window thread drains every few
 * ms). Only while a window of this process has the focus and no menu is
 * open; only the keys some control uses are asked for. */
static void pad_win_keys(uint32_t keys[4], const PadInputMap* map)
{
    if (!pad_kbm_on() || cellPad_host_overlay_open() || !pad_win_focused()) return;
    uint32_t want[4] = { 0, 0, 0, 0 };
    for (int c = 0; c < PIM_N_CONTROLS; c++)
        for (int w = 0; w < 4; w++) want[w] |= map->key[c][w];
    if (!(want[0] | want[1] | want[2] | want[3])) return;
    static HKL s_hkl;
    static int s_hkl_set;
    static unsigned char s_scan_vk[PAD_N_SCAN_KEYS];
    const HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));
    if (!s_hkl_set || hkl != s_hkl) {
        for (int i = 0; i < PAD_N_SCAN_KEYS; i++)
            s_scan_vk[i] = (unsigned char)MapVirtualKeyExW(kWinScanMac[i][0], MAPVK_VSC_TO_VK_EX, hkl);
        s_hkl = hkl; s_hkl_set = 1;
    }
#define PAD_WANT(mac) (want[(mac) >> 5] >> ((mac) & 31) & 1u)
#define PAD_SET(mac)  (keys[(mac) >> 5] |= 1u << ((mac) & 31))
    for (int i = 0; i < PAD_N_SCAN_KEYS; i++) {
        const unsigned mac = kWinScanMac[i][1];
        if (PAD_WANT(mac) && s_scan_vk[i] && (GetAsyncKeyState(s_scan_vk[i]) & 0x8000)) PAD_SET(mac);
    }
    for (int i = 0; i < PAD_N_VK_KEYS; i++) {
        const unsigned mac = kWinVkMac[i][1];
        if (PAD_WANT(mac) && (GetAsyncKeyState(kWinVkMac[i][0]) & 0x8000)) PAD_SET(mac);
    }
    /* keypad Enter is VK_RETURN too: one Enter for both */
    if (PAD_WANT(0x4Cu) && (GetAsyncKeyState(VK_RETURN) & 0x8000)) PAD_SET(0x4Cu);
#undef PAD_WANT
#undef PAD_SET
}

/* ---- the host input thread -------------------------------------------------
 * One thread with a message loop, for two things that need one:
 *   1. SDL's joystick subsystem is started here, so the hidden windows SDL
 *      makes for device notifications belong to a thread that pumps them
 *      (cellPadInit runs on a game thread that never does: a HIDAPI pad
 *      switched on mid-game was never noticed).
 *   2. The mouse, through raw input (RIDEV_INPUTSINK on a message-only
 *      window): motion and buttons land in g_pad_mouse_* the moment the
 *      mouse reports them, not when the window's event pump next runs (a
 *      pass every ~4-16 ms on the RSX thread), so the camera neither lags
 *      nor judders. They count only while the game window holds the mouse
 *      (g_pad_mouse_captured, set by the window: rsx_vulkan_backend.c).
 * SDL's relative mouse mode is not used: it registers raw mouse input for
 * the process too, and only one window per process can have it. */
volatile int g_pad_mouse_captured;
static HANDLE s_host_thread, s_host_ready;
static DWORD  s_host_tid;
static int    s_abs_have, s_abs_x, s_abs_y;

static void pad_host_raw_mouse(const RAWMOUSE* m)
{
    if (!__atomic_load_n(&g_pad_mouse_captured, __ATOMIC_ACQUIRE)) { s_abs_have = 0; return; }
    if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
        /* tablets, remote desktop: absolute 0..65535 -> pixel deltas */
        const int vd = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        const int w = GetSystemMetrics(vd ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        const int h = GetSystemMetrics(vd ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        const int x = (int)((long long)m->lLastX * w / 65535), y = (int)((long long)m->lLastY * h / 65535);
        if (s_abs_have) {
            __atomic_add_fetch(&g_pad_mouse_dx, (x - s_abs_x) * 16, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_pad_mouse_dy, (y - s_abs_y) * 16, __ATOMIC_RELAXED);
        }
        s_abs_x = x; s_abs_y = y; s_abs_have = 1;
    } else if (m->lLastX || m->lLastY) {
        __atomic_add_fetch(&g_pad_mouse_dx, (int32_t)m->lLastX * 16, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_pad_mouse_dy, (int32_t)m->lLastY * 16, __ATOMIC_RELAXED);
    }
    const USHORT f = m->usButtonFlags;
    if (!f) return;
    static const USHORT down[5] = { RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
                                    RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
    static const USHORT up[5]   = { RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP,
                                    RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP };
    for (int k = 0; k < 5; k++) {   /* 1 left, 2 right, 4 middle, 8 button 4, 16 button 5 */
        if (f & down[k]) __atomic_or_fetch(&g_pad_mouse_buttons, 1u << k, __ATOMIC_RELAXED);
        if (f & up[k])   __atomic_and_fetch(&g_pad_mouse_buttons, ~(1u << k), __ATOMIC_RELAXED);
    }
    if (f & RI_MOUSE_WHEEL)
        __atomic_add_fetch(&g_pad_mouse_wheel, (int32_t)(short)m->usButtonData * 16 / WHEEL_DELTA, __ATOMIC_RELAXED);
}

static LRESULT CALLBACK pad_host_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INPUT) {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE)
            pad_host_raw_mouse(&ri.data.mouse);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI pad_host_thread_main(LPVOID arg)
{
    (void)arg;
    if (pad_kbm_on()) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = pad_host_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ps3recomp_pad_host";
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ps3recomp pad host", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, NULL, wc.hInstance, NULL);
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01; rid.usUsage = 0x02;          /* generic desktop: mouse */
        rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = hwnd;
        if (!hwnd || !RegisterRawInputDevices(&rid, 1, sizeof rid))
            fprintf(stderr, "[cellPad] raw mouse input unavailable (error %lu): no mouse camera\n", (unsigned long)GetLastError());
    }
    if (!pad_sdl_off()) pad_sdl_init_subsystem();
    SetEvent(s_host_ready);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static void pad_host_thread_start(void)
{
    if (s_host_thread) return;
    if (pad_sdl_off() && !pad_kbm_on()) return;
    s_host_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_host_thread = CreateThread(NULL, 0, pad_host_thread_main, NULL, 0, &s_host_tid);
    if (!s_host_thread) {
        fprintf(stderr, "[cellPad] host input thread: CreateThread failed (%lu); SDL on this thread, no raw mouse\n",
                (unsigned long)GetLastError());
        if (!pad_sdl_off()) pad_sdl_init_subsystem();
        return;
    }
    SetThreadPriority(s_host_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    const DWORD w = WaitForSingleObject(s_host_ready, 15000);
    fprintf(stderr, "[cellPad] host input thread %lu: %s%s\n", (unsigned long)s_host_tid,
            pad_sdl_off() ? "SDL controllers off (PS3_PAD_SDL=0)" : (s_sdl_inited ? "SDL controllers up" : "SDL init failed"),
            w == WAIT_OBJECT_0 ? "" : " (still starting after 15 s)");
}
#endif /* _WIN32 && PAD_BACKEND_SDL2 */

static void pad_poll_keys(int merge)
{
    const PadInputMap* map = pad_map();
    uint32_t keys[4];
    for (int i = 0; i < 4; i++) keys[i] = g_pad_host_keys[i];
#if defined(_WIN32) && PAD_BACKEND_SDL2
    pad_win_keys(keys, map);
#endif
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

/* An in-game menu is open: the game gets neutral input, the mouse is not
 * captured, keys are not read. THE one place that answers it -- an overlay
 * with a flag of its own adds it here. */
int cellPad_host_overlay_open(void)
{
    return g_pad_overlay_open != 0;
}

/* ---- the game window and the mouse (Windows: rsx_vulkan_backend.c) ---------
 * The window tells cellPad what happens to it and asks whether it should
 * hold the mouse; cellPad owns the decision so every window backend behaves
 * alike:
 *   - nothing is captured until the player clicks into the game window
 *     (a test run's window that takes the focus never grabs anyone's cursor);
 *     that click is the capture's own and does not reach the game
 *   - captured = cursor hidden and kept inside the window, raw motion turns
 *     the camera; only while the window has the focus, the mouse camera is
 *     on (PS3_MOUSE, the input mode) and no menu is open
 *   - Esc is the window's, not the game's (unless [keyboard] binds Escape):
 *     it counts up g_pad_overlay_toggle (the in-game menu opens/closes on
 *     each change, as for L3+R3) and, when no menu exists
 *     (g_pad_overlay_present = 0), releases the mouse. Alt+F4 or closing
 *     the window quits.
 *   - PS3_MOUSE_CAPTURE=0 / [mouse] capture = false: no capture; motion
 *     counts whenever the window has the focus, with the cursor visible */
volatile int g_pad_overlay_present;   /* set by an in-game menu that opens on g_pad_overlay_toggle */
static volatile int s_cap_engaged, s_cap_focused;
enum { PAD_WIN_CLICK = 1, PAD_WIN_ESCAPE = 2 };
void cellPad_host_window_focus(int focused) { s_cap_focused = focused != 0; }
int cellPad_host_window_event(int ev)
{
    if (ev == PAD_WIN_CLICK) {
        if (!s_cap_engaged && pad_kbm_on() && cellPad_host_mouse_active() && !cellPad_host_overlay_open() &&
            cellPad_host_mouse_capture_setting()) {
            s_cap_engaged = 1;
            fprintf(stderr, "[cellPad] mouse captured (Esc releases it)\n");
            return 1;
        }
        return 0;
    }
    if (ev == PAD_WIN_ESCAPE) {
        if (pad_key_bound(0x35)) return 0;            /* [keyboard] gives Escape to the game */
        __atomic_add_fetch(&g_pad_overlay_toggle, 1, __ATOMIC_RELEASE);
        if (!g_pad_overlay_present && s_cap_engaged) {
            s_cap_engaged = 0;
            fprintf(stderr, "[cellPad] mouse released (click in the game to capture it again)\n");
        }
        return 1;
    }
    return 0;
}
/* 0: the mouse does nothing now; 1: raw motion counts, cursor left alone
 * (capture off); 2: hold it -- hide the cursor and keep it in the window. */
int cellPad_host_capture_wanted_now(void)
{
    if (!s_cap_focused || !pad_kbm_on() || !cellPad_host_mouse_active() || cellPad_host_overlay_open()) return 0;
    if (!cellPad_host_mouse_capture_setting()) return 1;
    return s_cap_engaged ? 2 : 0;
}
/* The window applied it: motion and buttons count from now on (or stop). */
void cellPad_host_set_mouse_captured(int on)
{
    __atomic_store_n(&g_pad_mouse_buttons, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pad_mouse_dx, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pad_mouse_dy, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pad_mouse_wheel, 0, __ATOMIC_RELAXED);
#if defined(_WIN32) && PAD_BACKEND_SDL2
    __atomic_store_n(&g_pad_mouse_captured, on ? 1 : 0, __ATOMIC_RELEASE);
#else
    (void)on;
#endif
}

/* The per-port host state is final for this poll here: every source has
 * been merged (controllers, keys, mouse, PS3_PAD_FILE). Anything that must
 * hold the game's input (a menu) does it in this one function. */
static void pad_finalize_ports(void)
{
    if (cellPad_host_overlay_open()) pad_overlay_neutral();
    pad_track_assign();
}

/* PS3_PAD_STATS=1: once a second, what the game asked for and what a poll
 * cost (stdout):  reads per port, packets handed out (len > 0), host polls,
 * the controller read (SDL_GameControllerUpdate + state) and the key read. */
static struct {
    int on;
    unsigned long long t0;
    unsigned reads[PAD_MAX_HOST_PORTS], packets[PAD_MAX_HOST_PORTS], polls;
    unsigned long long sdl_us, sdl_max, keys_us, keys_max;
} s_ps;
static int pad_stats_on(void)
{
    if (!s_ps.on) { const char* e = getenv("PS3_PAD_STATS"); s_ps.on = (e && *e && *e != '0') ? 1 : -1; }
    return s_ps.on > 0;
}
static void pad_stats_tick(void)
{
    const unsigned long long now = pad_now_us();
    if (!s_ps.t0) { s_ps.t0 = now; return; }
    if (now - s_ps.t0 < 1000000ull) return;
    const double s = (double)(now - s_ps.t0) / 1e6;
    printf("[padstats] %.1fs: reads/s p0 %.0f p1 %.0f p2 %.0f p3 %.0f, packets/s p0 %.0f, polls/s %.0f, "
           "controller read avg %.1f us max %llu us, keys avg %.1f us max %llu us\n",
           s, s_ps.reads[0] / s, s_ps.reads[1] / s, s_ps.reads[2] / s, s_ps.reads[3] / s, s_ps.packets[0] / s,
           s_ps.polls / s, s_ps.polls ? (double)s_ps.sdl_us / s_ps.polls : 0.0, s_ps.sdl_max,
           s_ps.polls ? (double)s_ps.keys_us / s_ps.polls : 0.0, s_ps.keys_max);
    fflush(stdout);
    memset(s_ps.reads, 0, sizeof s_ps.reads);
    memset(s_ps.packets, 0, sizeof s_ps.packets);
    s_ps.polls = 0; s_ps.sdl_us = s_ps.sdl_max = s_ps.keys_us = s_ps.keys_max = 0;
    s_ps.t0 = now;
}

static void pad_poll_backend(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_us = 0;
    const unsigned long long now = pad_now_us();
    if (s_last_us && now - s_last_us < pad_poll_interval_us()) return;
    if (!pad_lock_try()) return;   /* another thread is polling: its result serves this read */
    s_last_us = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

    const int stats = pad_stats_on();
#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
    unsigned long long t_ctl = 0;
    if (stats) {
        t_ctl = pad_now_us();
        const unsigned long long d = t_ctl - now;
        s_ps.polls++; s_ps.sdl_us += d; if (d > s_ps.sdl_max) s_ps.sdl_max = d;
    }
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
    if (stats) {
        const unsigned long long d = pad_now_us() - t_ctl;
        s_ps.keys_us += d; if (d > s_ps.keys_max) s_ps.keys_max = d;
    }
    pad_finalize_ports();
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

/* PS3_PAD_TRACE: the pad state the game RECEIVES (after every source and
 * injection: controllers, keys, mouse, PAD_SCRIPT, PAD_FILE, ...), on stdout.
 *   1     a line whenever port 0's packet changes (len > 0 packets only)
 *   2     a line for every port-0 packet (one per game frame)
 *   all   as 1, for every port
 * "t" is seconds since the first read, "dt" ms since that port's last packet
 * -- the game's own read cadence. */
static void pad_trace_packet(u32 port, const CellPadData* d)
{
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("PS3_PAD_TRACE");
        mode = !e || !*e || *e == '0' ? 0 : !strcmp(e, "all") ? 3 : atoi(e) == 2 ? 2 : 1;
    }
    if (!mode || !d->len || port >= PAD_MAX_HOST_PORTS || (mode != 3 && port != 0)) return;
    static u16 last[PAD_MAX_HOST_PORTS][CELL_PAD_BTN_OFFSET_SENSOR_G + 1];
    static unsigned long long t0, last_us[PAD_MAX_HOST_PORTS];
    static int have[PAD_MAX_HOST_PORTS];
    const unsigned long long now = pad_now_us();
    if (!t0) t0 = now;
    /* buttons and sticks exactly; the tilt only when it moves (a real
     * accelerometer jitters by a few counts every report) */
    const int same = have[port] && !memcmp(last[port], d->button, (CELL_PAD_BTN_OFFSET_PRESS_R2 + 1) * sizeof(u16)) &&
                     abs((int)last[port][CELL_PAD_BTN_OFFSET_SENSOR_X] - (int)d->button[CELL_PAD_BTN_OFFSET_SENSOR_X]) < 12;
    const double dt = have[port] ? (double)(now - last_us[port]) / 1000.0 : 0.0;
    last_us[port] = now;
    if (same && mode != 2) return;
    memcpy(last[port], d->button, sizeof last[port]);
    have[port] = 1;
    const u16 b = (u16)(d->button[CELL_PAD_BTN_OFFSET_DIGITAL1] | (d->button[CELL_PAD_BTN_OFFSET_DIGITAL2] << 8));
    static const char* const names[16] = { "SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
                                           "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE" };
    char list[160]; size_t len = 0; list[0] = 0;
    for (int i = 0; i < 16; i++)
        if (b >> i & 1) {
            const int w = snprintf(list + len, sizeof list - len, "%s%s", len ? "+" : "", names[i]);
            if (w > 0 && (size_t)w < sizeof list - len) len += (size_t)w;
        }
    printf("[padtrace] t=%.3f dt=%.1f port%u len=%d btn=0x%04X %-24s L=(%3u,%3u) R=(%3u,%3u) L2=%3u R2=%3u tilt=(%u,%u,%u)\n",
           (double)(now - t0) / 1e6, dt, port, (int)d->len, b, len ? list : "-",
           d->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X], d->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y],
           d->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X], d->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y],
           d->button[CELL_PAD_BTN_OFFSET_PRESS_L2], d->button[CELL_PAD_BTN_OFFSET_PRESS_R2],
           d->button[CELL_PAD_BTN_OFFSET_SENSOR_X], d->button[CELL_PAD_BTN_OFFSET_SENSOR_Y],
           d->button[CELL_PAD_BTN_OFFSET_SENSOR_Z]);
    fflush(stdout);
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
     * down, 0x400000 left, 0x800000 right): the camera, to look at something. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; double hold; int world; } ev[32]; static int n_ev = 0;
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
              while (*p && n_ev < 32) {
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
          static int fired[32], held[32], stick_was;
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
                  static int said[32];
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
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF); }
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
        /* Only where the host gave no pressure at all: an analog trigger
         * pressed past its threshold keeps its own travel (this used to
         * force 255 as soon as the digital bit was set: L2/R2 lost their
         * analog range). */
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] == 0)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* SIXAXIS: the controller's accelerometer if it has one, else tilt
         * from the left stick (s_ctl.tilt), else level (X 512, Y 399 =
         * gravity, Z 512, gyro 512). inFAMOUS 2 steers the Ionic Storm
         * with it. */
        u16 sx, sy, sz, sg;
        if (hs->has_sensor) { sx = hs->sensor_x; sy = hs->sensor_y; sz = hs->sensor_z; sg = hs->sensor_g; }
        else pad_tilt_fallback(hs, &sx, &sy, &sz, &sg);
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = sx;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = sy;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = sz;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = sg;
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
    if (port_no < PAD_MAX_HOST_PORTS && pad_stats_on()) {
        s_ps.reads[port_no]++;
        if (data->len) s_ps.packets[port_no]++;
        pad_stats_tick();
    }
    pad_trace_packet(port_no, data);
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
