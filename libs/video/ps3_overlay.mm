/*
 * ps3_overlay -- the in-game menu and performance HUD (see ps3_overlay.h).
 *
 * Dear ImGui (third_party/imgui, MIT) draws it with its Metal renderer; the
 * platform side is done here rather than with imgui_impl_osx, because the
 * backend's pump dequeues every NSEvent itself (keys never reach AppKit) and
 * ImGui runs on the presenting thread, not the main one: the main thread
 * queues the events, the render thread hands them to ImGui.
 *
 * Built with -fobjc-arc (CMakeLists.txt).
 */
#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#include "imgui.h"
#include "backends/imgui_impl_metal.h"
#include "ps3_overlay.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"   /* the parser half is cellPad's */
#include "../input/pad_input_map.h"   /* PadInputMap, names, pim_rebind */
#pragma clang diagnostic pop

#include <float.h>
#include <math.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- what the menu changes: the toolkit's live setters ------------------- */
extern "C" {
float rsx_metal_gfx_sharpen(void);
void  rsx_metal_gfx_set_sharpen(float v);
float rsx_metal_gfx_vignette(void);
void  rsx_metal_gfx_set_vignette(float v);
float rsx_metal_gfx_grain(void);
void  rsx_metal_gfx_set_grain(float v);
float rsx_metal_gfx_lut_strength(void);
void  rsx_metal_gfx_set_lut_strength(float v);
float rsx_metal_gfx_vibrance(void);
void  rsx_metal_gfx_set_vibrance(float v);
float rsx_metal_gfx_contrast(void);
void  rsx_metal_gfx_set_contrast(float v);
float rsx_metal_gfx_curve(int zone);    /* 0 shadows, 1 midtones, 2 highlights */
void  rsx_metal_gfx_set_curve(int zone, float v);
float rsx_metal_gfx_clarity(void);
void  rsx_metal_gfx_set_clarity(float v);
int   rsx_metal_gfx_lut_preset(void);   /* 0 none, 1 warm, 2 cool, 3 film, 4 vivid */
void  rsx_metal_gfx_set_lut_preset(int p);
int   rsx_metal_gfx_lut_file(void);
int   rsx_metal_gfx_aa(void);           /* 0 off, 1 FXAA, 2 SMAA */
void  rsx_metal_gfx_set_aa(int mode);
int   rsx_metal_gfx_aniso(void);
int   rsx_metal_gfx_upscale(void);
void  rsx_metal_gfx_set_upscale(int on);
float rsx_metal_res_scale(void);        /* PS3_RES_SCALE in effect; -1 not possible here */
float rsx_metal_aspect(void);           /* PS3_ASPECT in effect (1 = 16:9); -1 not possible here */
int   rsx_metal_vsync(void);
void  rsx_metal_set_vsync(int on);
int   rsx_metal_window_mode(void);
void  rsx_metal_set_window_mode(int mode);
void  rsx_metal_present_times(unsigned long long ns[4], unsigned* n);
float cellAvconfExt_display_gamma_override(void);
void  cellAvconfExt_set_display_gamma_override(float g);
float cellAvconfExt_title_gamma(void);
int   cellAudio_host_volume(void);
void  cellAudio_host_set_volume(int pct);
int   cellPad_host_rumble_strength(void);
void  cellPad_host_set_rumble_strength(int pct);
void  cellPad_host_mouse_settings(int* on, float* sens, int* invert_x, int* invert_y);
void  cellPad_host_set_mouse_settings(float sens, int invert_x, int invert_y);
void  cellPad_host_set_mouse_camera(int on);
int   cellPad_host_input_mode(void);
void  cellPad_host_set_input_mode(int mode);
void  cellPad_host_input_map(PadInputMap* out);
void  cellPad_host_set_input_map(const PadInputMap* m);
void  cellPad_host_input_map_path(char* out, size_t n);
int   cellPad_host_save_input_map(void);
void  ps3_button_icons_set_mode(int mode);      /* rsx_button_icons.h */
int   ps3_button_icons_mode_setting(void);
int   ps3_button_icons_effective(void);
void  ps3_lang_in_use(int kind, char* asked, char* given, size_t n);   /* runtime/ppu/ppu_fs_lang.h */
int   ps3_lang_available(int kind, char (*out)[24], int max);
void  ps3_host_quit(const char* why);              /* libs/system/cellSysutil.c */
extern volatile int      g_pad_overlay_open;
extern volatile uint32_t g_pad_overlay_toggle, g_pad_overlay_nav, g_pad_overlay_nav2, g_pad_overlay_raw;
}

volatile int g_ps3_overlay_active;

/* CELL_PAD_CTRL_* (libs/input/cellPad.h) */
enum {
    PAD_SELECT = 1 << 0, PAD_L3 = 1 << 1, PAD_R3 = 1 << 2, PAD_START = 1 << 3,
    PAD_UP = 1 << 4, PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8, PAD_R2 = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13, PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15,
};

/* ---- settings and their file ---------------------------------------------
 * The file is a list of VARIABLE=value lines with the same names and values
 * as the environment variables (so it reads like the launcher's extra
 * variables). At start, before main, each one whose variable is not set is
 * exported, so every module reads it the way it always read its variable;
 * one that is set wins. Only the variables below are taken. */
enum {
    V_GAMMA, V_SHARPEN, V_AA, V_ANISO, V_UPSCALE, V_VSYNC, V_WINDOW_MODE,
    V_VOLUME, V_RUMBLE, V_RUMBLE_STRENGTH, V_MOUSE_SENS, V_MOUSE_INVERT_X, V_MOUSE_INVERT_Y,
    V_HUD, V_HUD_CORNER, V_HUD_OPACITY, V_HUD_GRAPH, V_MOUSE, V_INPUT_MODE, V_VIBRANCE, V_CONTRAST, V_VIGNETTE, V_GRAIN, V_LUT_STRENGTH, V_BUTTON_ICONS,
    V_LANG, V_VOICE_LANG, V_TEXT_LANG,
    V_SHADOWS, V_MIDTONES, V_HIGHLIGHTS, V_CLARITY, V_LUT_PRESET, V_RES_SCALE, V_ASPECT, V_N
};
static const char* const kVar[V_N] = {
    "PS3_DISPLAY_GAMMA", "PS3_SHARPEN", "PS3_AA", "PS3_ANISO", "PS3_UPSCALE", "PS3_VSYNC",
    "PS3_WINDOW_MODE", "PS3_AUDIO_VOLUME", "PS3_RUMBLE", "PS3_RUMBLE_STRENGTH", "PS3_MOUSE_SENS",
    "PS3_MOUSE_INVERT_X", "PS3_MOUSE_INVERT_Y", "PS3_OVERLAY_HUD", "PS3_OVERLAY_HUD_CORNER",
    "PS3_OVERLAY_HUD_OPACITY", "PS3_OVERLAY_HUD_GRAPH", "PS3_MOUSE", "PS3_INPUT_MODE",
    "PS3_VIBRANCE", "PS3_CONTRAST", "PS3_VIGNETTE", "PS3_GRAIN", "PS3_LUT_STRENGTH",
    "PS3_BUTTON_ICONS", "PS3_LANG", "PS3_VOICE_LANG", "PS3_TEXT_LANG",
    "PS3_SHADOWS", "PS3_MIDTONES", "PS3_HIGHLIGHTS", "PS3_CLARITY", "PS3_LUT_PRESET",
    "PS3_RES_SCALE", "PS3_ASPECT",
};
/* Plain arrays: these are filled by a constructor, before C++ globals of
 * this file would be guaranteed to exist. */
static bool s_env_set[V_N];          /* in the environment when the game started */
static char s_saved[V_N][48];        /* the file's value ("" = not in it) */
static char s_value[V_N][48];        /* what the menu set this session ("" = untouched) */
static char s_ini_path[1024];
static pthread_mutex_t s_set_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_dirty;

static bool env_on(const char* v)
{
    const char* e = getenv(v);
    return e && *e && strcmp(e, "0") != 0;
}
static bool overlay_off(void)
{
    const char* e = getenv("PS3_OVERLAY");
    return e && e[0] == '0' && e[1] == 0;
}
static bool headless_env(void)
{
    return env_on("PS3RECOMP_METAL_HEADLESS") || getenv("HEADLESS") != NULL;
}

__attribute__((constructor)) static void ps3_overlay_preload(void)
{
    if (overlay_off()) return;
    for (int i = 0; i < V_N; i++) s_env_set[i] = getenv(kVar[i]) != NULL;
    /* Headless runs never read the player's file. */
    if (headless_env()) return;
    const char* p = getenv("PS3_OVERLAY_INI");
    if (!p || !*p) return;
    snprintf(s_ini_path, sizeof s_ini_path, "%s", p);
    FILE* f = fopen(s_ini_path, "r");
    if (!f) return;
    char line[256];
    int applied = 0, kept = 0;
    while (fgets(line, sizeof line, f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || *s == '[' || !*s) continue;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = s; char* v = eq + 1;
        for (char* e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t' || *v == '"') v++;
        for (char* e = v + strlen(v) - 1; e >= v && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t' || *e == '"'); e--) *e = 0;
        for (int i = 0; i < V_N; i++) {
            if (strcmp(k, kVar[i])) continue;
            snprintf(s_saved[i], sizeof s_saved[i], "%s", v);
            if (!s_env_set[i]) { setenv(kVar[i], v, 1); applied++; } else kept++;
        }
    }
    fclose(f);
    fprintf(stderr, "[overlay] %s: %d setting(s) applied, %d left to the environment\n",
            s_ini_path, applied, kept);
}

static void set_value(int v, const char* fmt, ...)
{
    char buf[48];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    pthread_mutex_lock(&s_set_lock);
    if (strcmp(s_value[v], buf)) { snprintf(s_value[v], sizeof s_value[v], "%s", buf); s_dirty = true; }
    pthread_mutex_unlock(&s_set_lock);
}

/* Write the file: what it had plus what the menu changed. 1 = written. */
static int settings_save(void)
{
    if (!s_ini_path[0]) return 0;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_ini_path);
    { char dir[1024]; snprintf(dir, sizeof dir, "%s", s_ini_path);   /* mkdir -p of the folder */
      for (char* q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; } }
    FILE* f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "# In-game menu settings (F1 / Cmd+, / L3+R3). Same names and values as the\n"
               "# environment variables; a variable set when the game starts wins over its line.\n");
    pthread_mutex_lock(&s_set_lock);
    for (int i = 0; i < V_N; i++) {
        const char* v = s_value[i][0] ? s_value[i] : s_saved[i];
        if (*v) fprintf(f, "%s=%s\n", kVar[i], v);
        if (s_value[i][0]) snprintf(s_saved[i], sizeof s_saved[i], "%s", s_value[i]);
    }
    s_dirty = false;
    pthread_mutex_unlock(&s_set_lock);
    const int ok = fclose(f) == 0 && rename(tmp, s_ini_path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

/* ---- state ---------------------------------------------------------------- */
static id<MTLDevice> s_dev;
static bool s_enabled, s_headless, s_force_open, s_imgui_ready, s_pt;
static volatile int s_open;
static volatile int s_hud;
static int   s_hud_corner = 1;          /* 0 top left, 1 top right, 2 bottom left, 3 bottom right */
static float s_hud_opacity = 0.6f;
static bool  s_hud_graph = true;
static uint32_t s_seen_toggle;
static char  s_title[64] = "ps3recomp";
static int   s_tab, s_tab_want = -1;   /* the menu's tab; one to switch to */
/* "Quit game": 1 = open the confirmation next frame, 2 = it is showing.
 * s_quit_cancel: Esc or Circle closes it (both close the whole menu otherwise). */
static volatile int  s_quit_ask;
static volatile bool s_quit_cancel;

static const char* L(const char* en, const char* pt) { return s_pt ? pt : en; }

static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Notices (any thread). */
struct Notice { char text[128]; double until; };
static Notice s_notice[3];
static pthread_mutex_t s_notice_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile double s_notice_until;

static void update_active(void)
{
    g_ps3_overlay_active = s_enabled && (s_open || s_hud || s_notice_until > now_s());
}

extern "C" void ps3_overlay_notice(const char* text)
{
    if (!s_enabled || !text) return;
    pthread_mutex_lock(&s_notice_lock);
    memmove(&s_notice[1], &s_notice[0], sizeof s_notice[0] * 2);
    snprintf(s_notice[0].text, sizeof s_notice[0].text, "%s", text);
    s_notice[0].until = now_s() + 3.0;
    s_notice_until = s_notice[0].until;
    pthread_mutex_unlock(&s_notice_lock);
    update_active();
}

/* ---- rebinding (Controls tab) -----------------------------------------------
 * The tab starts a capture for one cell (a PS3 control on one device); the
 * next key (main thread, ps3_overlay_handle_event), mouse button or wheel
 * turn (same), or newly pressed controller button (render thread, from
 * g_pad_overlay_raw) ends it. The menu is open throughout, so the game holds
 * neutral and none of it reaches the game. */
enum { CAP_KEYS, CAP_MOUSE, CAP_PAD };                 /* the table's columns */
enum { CAP_PENDING, CAP_BIND, CAP_CANCEL, CAP_UNBIND };
static const double kCapTimeout = 8.0;                 /* seconds, then it gives up */
static pthread_mutex_t s_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static int      s_cap_dev = -1, s_cap_ctl, s_cap_res;
static uint32_t s_cap_val, s_cap_pad_base;
static double   s_cap_t0, s_cap_wheel;

static int cap_device(void)
{
    pthread_mutex_lock(&s_cap_lock);
    const int d = (s_cap_dev >= 0 && s_cap_res == CAP_PENDING) ? s_cap_dev : -1;
    pthread_mutex_unlock(&s_cap_lock);
    return d;
}
static void cap_finish(int res, uint32_t val)
{
    pthread_mutex_lock(&s_cap_lock);
    if (s_cap_dev >= 0 && s_cap_res == CAP_PENDING) { s_cap_res = res; s_cap_val = val; }
    pthread_mutex_unlock(&s_cap_lock);
}

static void set_open(int o)
{
    if (!s_enabled || (o != 0) == (s_open != 0)) return;
    if (!o) cap_finish(CAP_CANCEL, 0);
    s_open = o;
    g_pad_overlay_open = o;
    if (!o && s_dirty) {
        if (settings_save()) ps3_overlay_notice(L("Settings saved", "Ajustes salvos"));
        else if (!s_ini_path[0]) ps3_overlay_notice(L("Settings kept for this session (no PS3_OVERLAY_INI)",
                                                      "Ajustes valem só nesta sessão (sem PS3_OVERLAY_INI)"));
        else ps3_overlay_notice(L("Could not save the settings", "Não foi possível salvar os ajustes"));
    }
    update_active();
}

/* Cmd+Q (rsx_metal_backend.m): the notice in the menu's language. */
extern "C" void ps3_overlay_quit_notice(void)
{
    ps3_overlay_notice(L("Quitting the game...", "Saindo do jogo..."));
}

extern "C" int ps3_overlay_menu_open(void)
{
    if (!s_enabled) return 0;
    /* L3+R3 on a controller: cellPad counts each press of the pair. */
    const uint32_t t = __atomic_load_n(&g_pad_overlay_toggle, __ATOMIC_ACQUIRE);
    if (t != s_seen_toggle) { s_seen_toggle = t; set_open(!s_open); }
    return s_open;
}

extern "C" int ps3_overlay_composes_headless(void) { return s_enabled && s_headless; }

extern "C" void ps3_overlay_init(void* mtl_device, int headless)
{
    s_dev = (__bridge id<MTLDevice>)mtl_device;
    s_headless = headless != 0;
    s_enabled = false;
    g_ps3_overlay_active = 0;
    if (overlay_off()) return;
    s_force_open = env_on("PS3_OVERLAY_FORCE_OPEN");
    s_hud = env_on("PS3_OVERLAY_HUD");
    /* Headless: nothing at all unless a test asks for it. */
    if (s_headless && !s_force_open && !s_hud) return;
    { const char* e = getenv("PS3_OVERLAY_HUD_CORNER"); if (e && *e) s_hud_corner = atoi(e) & 3; }
    { const char* e = getenv("PS3_OVERLAY_HUD_OPACITY");
      if (e && *e) { float v = (float)atof(e); s_hud_opacity = v < 0.1f ? 0.1f : v > 1.0f ? 1.0f : v; } }
    { const char* e = getenv("PS3_OVERLAY_HUD_GRAPH"); if (e && *e) s_hud_graph = strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3_OVERLAY_LANG");
      if (!e || !*e) e = getenv("PS3_LANG");
      s_pt = e && !strncasecmp(e, "pt", 2); }
    { const char* e = getenv("PS3_TITLE"); if (e && *e) snprintf(s_title, sizeof s_title, "%s", e); }
    s_seen_toggle = __atomic_load_n(&g_pad_overlay_toggle, __ATOMIC_ACQUIRE);
    /* PS3_OVERLAY_TAB=0..3: the tab it opens on (with FORCE_OPEN, the one a
     * headless test draws). */
    { const char* e = getenv("PS3_OVERLAY_TAB"); if (e && *e) { const int t = atoi(e); s_tab_want = t >= 0 && t < 5 ? t : 0; } }
    s_enabled = true;
    if (s_force_open) set_open(1);
    /* PS3_OVERLAY_FORCE_OPEN=quit: open on the "Quit the game?" confirmation
     * (a headless frame dump shows it; nothing is pressed). */
    { const char* e = getenv("PS3_OVERLAY_FORCE_OPEN"); if (e && !strcmp(e, "quit")) s_quit_ask = 1; }
    update_active();
    fprintf(stderr, "[overlay] in-game menu: F1 (fn+F1), Insert/Help or Cmd+, ; L3+R3 on a controller%s%s\n",
            s_hud ? "; HUD on" : "", s_force_open ? "; forced open" : "");
}

extern "C" void ps3_overlay_shutdown(void)
{
    if (s_imgui_ready) {
        ImGui_ImplMetal_Shutdown();
        ImGui::DestroyContext();
        s_imgui_ready = false;
    }
    s_enabled = false;
    g_ps3_overlay_active = 0;
    g_pad_overlay_open = 0;
}

/* ---- input from the main thread --------------------------------------------- */
struct InEv { int kind; int key; int down; float x, y; char text[16]; };
enum { IN_KEY, IN_MOD, IN_POS, IN_BUTTON, IN_WHEEL, IN_TEXT };
static InEv s_in[256];
static int s_in_n;
static pthread_mutex_t s_in_lock = PTHREAD_MUTEX_INITIALIZER;

static void in_push(const InEv& e)
{
    pthread_mutex_lock(&s_in_lock);
    if (s_in_n < (int)(sizeof s_in / sizeof s_in[0])) s_in[s_in_n++] = e;
    pthread_mutex_unlock(&s_in_lock);
}

static ImGuiKey key_from_vk(unsigned vk)
{
    switch (vk) {
    case 0x7E: return ImGuiKey_UpArrow;    case 0x7D: return ImGuiKey_DownArrow;
    case 0x7B: return ImGuiKey_LeftArrow;  case 0x7C: return ImGuiKey_RightArrow;
    case 0x30: return ImGuiKey_Tab;        case 0x24: return ImGuiKey_Enter;
    case 0x4C: return ImGuiKey_KeypadEnter; case 0x31: return ImGuiKey_Space;
    case 0x33: return ImGuiKey_Backspace;  case 0x75: return ImGuiKey_Delete;
    case 0x73: return ImGuiKey_Home;       case 0x77: return ImGuiKey_End;
    case 0x74: return ImGuiKey_PageUp;     case 0x79: return ImGuiKey_PageDown;
    case 0x00: return ImGuiKey_A;          case 0x08: return ImGuiKey_C;
    case 0x09: return ImGuiKey_V;          case 0x07: return ImGuiKey_X;
    case 0x06: return ImGuiKey_Z;          case 0x10: return ImGuiKey_Y;
    case 0x2F: return ImGuiKey_Period;     case 0x1B: return ImGuiKey_Minus;
    default:   return ImGuiKey_None;
    }
}

/* An event while a cell waits for its new input: 1 = taken. */
static int cap_event(NSEvent* ev, NSEventType t)
{
    const int dev = cap_device();
    if (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp) {
        const unsigned vk = [ev keyCode] & 127u;
        const bool cmd = ([ev modifierFlags] & NSEventModifierFlagCommand) != 0;
        /* the menu's own keys still open and close it (closing cancels) */
        if ((vk == 0x7A && !cmd) || vk == 0x72 || (vk == 0x2B && cmd) || vk == 0x6F) return 0;
        if (cmd) return 0;                                 /* the window's shortcuts */
        if (t == NSEventTypeKeyUp || [ev isARepeat]) return 1;
        if (vk == 0x35) cap_finish(CAP_CANCEL, 0);                         /* Esc */
        else if (dev == CAP_KEYS) cap_finish(CAP_BIND, vk);
        else if (vk == 0x33 || vk == 0x75) cap_finish(CAP_UNBIND, 0);      /* Backspace, Delete */
        return 1;
    }
    if (t == NSEventTypeFlagsChanged) {
        /* Shift, Control and Option are keys of the keyboard pad as well (the
         * device-dependent bits tell left from right, as in the backend). */
        static const struct { unsigned short vk; NSUInteger bit; } mods[] = {
            {0x38, 0x02}, {0x3C, 0x04}, {0x3B, 0x01}, {0x3E, 0x2000}, {0x3A, 0x20}, {0x3D, 0x40},
        };
        const unsigned vk = [ev keyCode] & 127u;
        if (dev == CAP_KEYS)
            for (auto& m : mods)
                if (m.vk == vk && ([ev modifierFlags] & m.bit)) cap_finish(CAP_BIND, vk);
        return 1;
    }
    if (dev != CAP_MOUSE) return 0;                         /* the mouse works the menu */
    switch (t) {
    case NSEventTypeLeftMouseDown:  cap_finish(CAP_BIND, 1u); return 1;
    case NSEventTypeRightMouseDown: cap_finish(CAP_BIND, 2u); return 1;
    case NSEventTypeOtherMouseDown: {
        /* middle = 4, button 4 = 8, ... as the backend records them */
        const NSInteger b = [ev buttonNumber];
        cap_finish(CAP_BIND, 1u << (b < 2 ? 2 : b > 7 ? 7 : b));
        return 1;
    }
    case NSEventTypeLeftMouseUp: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseUp:
        return 1;
    case NSEventTypeScrollWheel: {
        /* + = turned away from the player whatever the scroll setting */
        const double dy = [ev isDirectionInvertedFromDevice] ? -[ev deltaY] : [ev deltaY];
        pthread_mutex_lock(&s_cap_lock);
        s_cap_wheel += dy;
        const double w = s_cap_wheel;
        pthread_mutex_unlock(&s_cap_lock);
        if (w >= 1.0) cap_finish(CAP_BIND, PIM_MOUSE_WHEEL_UP);
        else if (w <= -1.0) cap_finish(CAP_BIND, PIM_MOUSE_WHEEL_DOWN);
        return 1;
    }
    default:
        return 0;
    }
}

extern "C" int ps3_overlay_handle_event(void* nsevent, double view_w, double view_h,
                                        double drawable_w, double drawable_h)
{
    if (!s_enabled) return 0;
    NSEvent* ev = (__bridge NSEvent*)nsevent;
    const NSEventType t = [ev type];
    if (s_open && cap_device() >= 0 && cap_event(ev, t)) return 1;
    if (t == NSEventTypeKeyDown || t == NSEventTypeKeyUp) {
        const unsigned vk = [ev keyCode] & 127u;
        const NSEventModifierFlags m = [ev modifierFlags];
        const bool cmd = (m & NSEventModifierFlagCommand) != 0;
        const bool down = t == NSEventTypeKeyDown;
        /* F1, Insert (Help on older Mac keyboards) and Cmd+, (the macOS
         * Settings shortcut) open and close it; Esc closes it. */
        const bool toggle = (vk == 0x7A && !cmd) || vk == 0x72 || (vk == 0x2B && cmd);
        /* F12: capture this frame for debug mode (the launcher's "Capture"),
         * menu open or not; the game never sees the key. */
        if (vk == 0x6F && !cmd) {
            if (down && ![ev isARepeat]) {
                extern int rsx_metal_capture_available(void);
                extern void rsx_metal_request_capture(void);
                if (rsx_metal_capture_available()) {
                    rsx_metal_request_capture();
                    ps3_overlay_notice(L("Frame captured for the debug session", "Quadro capturado para a sessão de depuração"));
                } else {
                    ps3_overlay_notice(L("F12 captures in debug mode (launcher: Advanced > Debug mode)",
                                         "O F12 captura no modo de depuração (app: Avançado > Modo de depuração)"));
                }
            }
            return 1;
        }
        if (vk == 0x35 && s_open && s_quit_ask) {      /* Esc: back out of "Quit game?" */
            if (down && ![ev isARepeat]) s_quit_cancel = true;
            return 1;
        }
        if (toggle || (vk == 0x35 && s_open)) {
            if (!down) return s_open;
            if (![ev isARepeat]) set_open(toggle ? !s_open : 0);
            return 1;
        }
        if (!s_open) return 0;
        if (cmd) return 0;   /* the window's own shortcuts (Cmd+F ...) still work */
        const ImGuiKey k = key_from_vk(vk);
        if (k != ImGuiKey_None) { InEv e = {}; e.kind = IN_KEY; e.key = k; e.down = down; in_push(e); }
        if (down) {
            NSString* chars = [ev characters];
            for (NSUInteger i = 0; chars && i < [chars length]; i++) {
                const unichar c = [chars characterAtIndex:i];
                if (c < 0x20 || c == 0x7F || (c >= 0xF700 && c <= 0xF8FF)) continue;
                InEv e = {}; e.kind = IN_TEXT;
                NSString* one = [NSString stringWithCharacters:&c length:1];
                snprintf(e.text, sizeof e.text, "%s", [one UTF8String] ? [one UTF8String] : "");
                in_push(e);
            }
        }
        return 1;
    }
    if (!s_open) return 0;
    switch (t) {
    case NSEventTypeFlagsChanged: {
        const NSEventModifierFlags m = [ev modifierFlags];
        const struct { NSEventModifierFlags f; ImGuiKey k; } mods[] = {
            { NSEventModifierFlagShift, ImGuiMod_Shift }, { NSEventModifierFlagControl, ImGuiMod_Ctrl },
            { NSEventModifierFlagOption, ImGuiMod_Alt },  { NSEventModifierFlagCommand, ImGuiMod_Super },
        };
        for (auto& md : mods) { InEv e = {}; e.kind = IN_MOD; e.key = md.k; e.down = (m & md.f) != 0; in_push(e); }
        return 1;
    }
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged: case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp: case NSEventTypeOtherMouseDown:
    case NSEventTypeOtherMouseUp: case NSEventTypeScrollWheel: {
        /* The layer letterboxes the drawable into the view: undo that, then
         * the menu's own scale (720 logical lines, see ps3_overlay_render). */
        if (view_w > 0 && view_h > 0 && drawable_w > 0 && drawable_h > 0 && [ev window]) {
            const NSPoint p = [ev locationInWindow];
            const double a = drawable_w / drawable_h;
            double fw = view_w, fh = view_h, x0 = 0, y0 = 0;
            if (view_w / view_h > a) { fw = view_h * a; x0 = (view_w - fw) / 2; }
            else                     { fh = view_w / a; y0 = (view_h - fh) / 2; }
            const double k = 720.0 / drawable_h;
            InEv e = {}; e.kind = IN_POS;
            e.x = (float)((p.x - x0) / fw * drawable_w * k);
            e.y = (float)(((view_h - p.y) - y0) / fh * drawable_h * k);
            in_push(e);
        }
        if (t == NSEventTypeScrollWheel) {
            const double s = [ev hasPreciseScrollingDeltas] ? 0.1 : 1.0;
            InEv e = {}; e.kind = IN_WHEEL;
            e.x = (float)([ev scrollingDeltaX] * s); e.y = (float)([ev scrollingDeltaY] * s);
            in_push(e);
        } else {
            int b = -1, down = 0;
            if (t == NSEventTypeLeftMouseDown || t == NSEventTypeLeftMouseUp)   b = 0;
            if (t == NSEventTypeRightMouseDown || t == NSEventTypeRightMouseUp) b = 1;
            if (t == NSEventTypeOtherMouseDown || t == NSEventTypeOtherMouseUp) b = 2;
            down = t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown || t == NSEventTypeOtherMouseDown;
            if (b >= 0) { InEv e = {}; e.kind = IN_BUTTON; e.key = b; e.down = down; in_push(e); }
        }
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- performance numbers ---------------------------------------------------- */
enum { FT_N = 180 };
static float  s_ft[FT_N];               /* ms between presents */
static int    s_ft_i, s_ft_count;
static double s_last_present;
struct Stats { double fps, avg_ms, max_ms, cores, enc_ms, wait_ms; int busy; bool valid; };
static Stats  s_stats;

static void stats_tick(double now)
{
    static double t0, cpu0;
    static unsigned long long pt0[4]; static unsigned pn0;
    static int frames;
    frames++;
    if (!t0) {
        t0 = now;
        struct rusage ru; getrusage(RUSAGE_SELF, &ru);
        cpu0 = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
        rsx_metal_present_times(pt0, &pn0);
        frames = 0;
        return;
    }
    const double dt = now - t0;
    if (dt < 1.0) return;
    Stats st = {};
    st.fps = frames / dt;
    { struct rusage ru; getrusage(RUSAGE_SELF, &ru);
      const double cpu = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
      st.cores = (cpu - cpu0) / dt; cpu0 = cpu; }
    /* Threads busy at least 80% (Mach's decayed usage): with this title the
     * SPU threads, the PPU main thread and the RSX/present thread. */
    { thread_act_array_t th = NULL; mach_msg_type_number_t n = 0;
      if (task_threads(mach_task_self(), &th, &n) == KERN_SUCCESS) {
          for (mach_msg_type_number_t i = 0; i < n; i++) {
              thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
              if (thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS &&
                  !(bi.flags & TH_FLAGS_IDLE) && bi.cpu_usage >= TH_USAGE_SCALE * 8 / 10) st.busy++;
              mach_port_deallocate(mach_task_self(), th[i]);
          }
          vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(thread_t));
      } }
    { unsigned long long pt[4]; unsigned pn;
      rsx_metal_present_times(pt, &pn);
      if (pn > pn0 && pt[0] >= pt0[0] && pt[2] >= pt0[2]) {
          st.wait_ms = (pt[0] - pt0[0]) / 1e6 / (pn - pn0);
          st.enc_ms  = (pt[2] - pt0[2]) / 1e6 / (pn - pn0);
      }
      memcpy(pt0, pt, sizeof pt0); pn0 = pn; }
    { const int n = s_ft_count < FT_N ? s_ft_count : FT_N;
      double sum = 0, mx = 0; int k = 0;
      for (int i = 0; i < n && i < frames; i++) {
          const float v = s_ft[(s_ft_i - 1 - i + FT_N) % FT_N];
          sum += v; if (v > mx) mx = v; k++;
      }
      st.avg_ms = k ? sum / k : 0; st.max_ms = mx; }
    st.valid = true;
    s_stats = st;
    t0 = now; frames = 0;
}

/* ---- ImGui ---------------------------------------------------------------------- */
static void imgui_setup(void)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;     /* the menu keeps its own file */
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "ps3recomp_overlay";
    /* A system font with the accents Portuguese needs; ImGui's own otherwise. */
    static const char* const fonts[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf",
    };
    ImFont* font = NULL;
    for (const char* f : fonts) if (access(f, R_OK) == 0 && (font = io.Fonts->AddFontFromFileTTF(f, 17.0f))) break;
    if (!font) io.Fonts->AddFontDefaultVector();

    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.FontSizeBase = 17.0f;
    st.WindowRounding = 10.0f; st.FrameRounding = 5.0f; st.GrabRounding = 5.0f;
    st.TabRounding = 5.0f; st.PopupRounding = 6.0f;
    st.WindowPadding = ImVec2(18, 14); st.FramePadding = ImVec2(10, 5); st.ItemSpacing = ImVec2(10, 8);
    st.WindowBorderSize = 1.0f; st.GrabMinSize = 14.0f;
    /* near-black with an electric cyan accent, like the launcher */
    ImVec4* c = st.Colors;
    const ImVec4 cyan(0.20f, 0.80f, 0.95f, 1.0f), cyan_dim(0.12f, 0.45f, 0.55f, 1.0f);
    c[ImGuiCol_WindowBg]        = ImVec4(0.04f, 0.05f, 0.07f, 0.94f);
    c[ImGuiCol_Border]          = ImVec4(0.20f, 0.80f, 0.95f, 0.35f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.12f, 0.14f, 0.18f, 1.0f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.16f, 0.22f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.18f, 0.28f, 0.34f, 1.0f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.06f, 0.08f, 0.11f, 1.0f);
    c[ImGuiCol_CheckMark]       = cyan;
    c[ImGuiCol_SliderGrab]      = cyan_dim;
    c[ImGuiCol_SliderGrabActive]= cyan;
    c[ImGuiCol_Button]          = ImVec4(0.12f, 0.16f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonHovered]   = cyan_dim;
    c[ImGuiCol_ButtonActive]    = cyan;
    c[ImGuiCol_Header]          = ImVec4(0.12f, 0.30f, 0.38f, 1.0f);
    c[ImGuiCol_HeaderHovered]   = cyan_dim;
    c[ImGuiCol_Tab]             = ImVec4(0.10f, 0.12f, 0.16f, 1.0f);
    c[ImGuiCol_TabHovered]      = cyan_dim;
    c[ImGuiCol_TabSelected]     = ImVec4(0.12f, 0.38f, 0.48f, 1.0f);
    c[ImGuiCol_PlotLines]       = cyan;
    c[ImGuiCol_NavCursor]       = ImVec4(1.0f, 0.60f, 0.20f, 1.0f);   /* warm orange: where the pad is */
    c[ImGuiCol_Separator]       = ImVec4(0.20f, 0.80f, 0.95f, 0.25f);

    ImGui_ImplMetal_Init(s_dev);
    s_imgui_ready = true;
}

/* The menu's view of the settings, read from the modules when it opens. */
struct Ui {
    bool  gamma_on; float gamma; float title_gamma;
    float sharpen; float vibrance; float contrast; float vignette; float grain; float lut; int aa; int aniso; int upscale; int vsync; int wmode;
    float curve[3]; float clarity; int lut_preset;
    int   volume;
    bool  rumble; int rumble_strength;
    int   mouse_on; float mouse_sens; bool inv_x, inv_y;
    int   input_mode;
    int   button_icons;     /* 0 auto, 1 PlayStation, 2 Xbox, 3 keyboard */
    int   aniso_start;
    int   res, res_start;   /* PS3_RES_SCALE: index in kResScale; -1 n/a */
    int   asp, asp_start;   /* PS3_ASPECT: index in kAspect; -1 n/a */
};
/* PS3_RES_SCALE choices of the menu (the variable takes any 1..4). */
static const char* const kResScale[] = { "1", "1.5", "2", "3" };
static int res_scale_index(double v)
{
    int best = 0;
    for (int i = 1; i < 4; i++) if (fabs(atof(kResScale[i]) - v) < fabs(atof(kResScale[best]) - v)) best = i;
    return best;
}
/* PS3_ASPECT choices of the menu ("" = the game's 16:9). */
static const char* const kAspect[] = { "16:9", "21:9", "32:9" };
static int aspect_index(double k)   /* k = aspect / (16/9) */
{
    const double a = k * 16.0 / 9.0, v[3] = { 16.0 / 9.0, 21.0 / 9.0, 32.0 / 9.0 };
    int best = 0;
    for (int i = 1; i < 3; i++) if (fabs(v[i] - a) < fabs(v[best] - a)) best = i;
    return best;
}
static int aspect_index_str(const char* e)
{
    for (int i = 0; i < 3; i++) if (!strcmp(e, kAspect[i])) return i;
    return 0;
}
static Ui s_ui;

static void ui_load(void)
{
    const float g = cellAvconfExt_display_gamma_override();
    s_ui.gamma_on = g > 0.0f;
    s_ui.title_gamma = cellAvconfExt_title_gamma();
    s_ui.gamma = g > 0.0f ? g : s_ui.title_gamma;
    s_ui.sharpen = rsx_metal_gfx_sharpen();
    s_ui.aa = rsx_metal_gfx_aa();
    s_ui.vibrance = rsx_metal_gfx_vibrance();
    s_ui.contrast = rsx_metal_gfx_contrast();
    s_ui.vignette = rsx_metal_gfx_vignette();
    s_ui.grain = rsx_metal_gfx_grain();
    s_ui.lut = rsx_metal_gfx_lut_strength();
    for (int k = 0; k < 3; k++) s_ui.curve[k] = rsx_metal_gfx_curve(k);
    s_ui.clarity = rsx_metal_gfx_clarity();
    s_ui.lut_preset = rsx_metal_gfx_lut_preset();
    s_ui.aniso_start = rsx_metal_gfx_aniso();
    if (!s_ui.aniso) s_ui.aniso = s_ui.aniso_start;
    { const char* sv = s_value[V_ANISO][0] ? s_value[V_ANISO] : NULL; if (sv) s_ui.aniso = atoi(sv); }
    s_ui.upscale = rsx_metal_gfx_upscale();
    { const float rs = rsx_metal_res_scale();
      s_ui.res_start = rs < 0.0f ? -1 : res_scale_index(rs);
      s_ui.res = s_value[V_RES_SCALE][0] ? res_scale_index(atof(s_value[V_RES_SCALE])) : s_ui.res_start; }
    { const float ak = rsx_metal_aspect();
      s_ui.asp_start = ak < 0.0f ? -1 : aspect_index(ak);
      s_ui.asp = s_value[V_ASPECT][0] ? aspect_index_str(s_value[V_ASPECT]) : s_ui.asp_start; }
    s_ui.vsync = rsx_metal_vsync();
    s_ui.wmode = rsx_metal_window_mode();
    s_ui.volume = cellAudio_host_volume();
    const int r = cellPad_host_rumble_strength();
    s_ui.rumble = r > 0;
    if (r > 0) s_ui.rumble_strength = r; else if (!s_ui.rumble_strength) s_ui.rumble_strength = 100;
    int ix, iy;
    cellPad_host_mouse_settings(&s_ui.mouse_on, &s_ui.mouse_sens, &ix, &iy);
    s_ui.inv_x = ix != 0; s_ui.inv_y = iy != 0;
    s_ui.input_mode = cellPad_host_input_mode();
    s_ui.button_icons = ps3_button_icons_mode_setting();
}

/* "env" after a setting whose variable was set at start: it wins next time. */
static void env_mark(int v)
{
    if (!s_env_set[v]) return;
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.20f, 1.0f), "env");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(L("%s was set when the game started (the launcher or the shell).\n"
                            "The change applies now, but that variable wins again next time.",
                            "%s estava definida quando o jogo abriu (o launcher ou o terminal).\n"
                            "A mudança vale agora, mas essa variável vence de novo na próxima vez."), kVar[v]);
}

static void hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.66f, 0.72f, 1.0f));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void section(const char* text)
{
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.20f, 0.80f, 0.95f, 1.0f), "%s", text);
    ImGui::Separator();
}

static void tab_image(void)
{
    section(L("Picture", "Ajustes da imagem"));
    if (ImGui::Checkbox(L("Override the game's brightness", "Substituir o brilho do jogo"), &s_ui.gamma_on)) {
        cellAvconfExt_set_display_gamma_override(s_ui.gamma_on ? s_ui.gamma : 0.0f);
        if (s_ui.gamma_on) set_value(V_GAMMA, "%.2f", s_ui.gamma); else set_value(V_GAMMA, "0");
    }
    env_mark(V_GAMMA);
    ImGui::BeginDisabled(!s_ui.gamma_on);
    if (ImGui::SliderFloat(L("Gamma", "Gama"), &s_ui.gamma, 0.80f, 1.20f, "%.2f")) {
        cellAvconfExt_set_display_gamma_override(s_ui.gamma);
        set_value(V_GAMMA, "%.2f", s_ui.gamma);
    }
    ImGui::EndDisabled();
    { char b[160];
      snprintf(b, sizeof b, L("Off: the game's own brightness option (now %.2f), as on the console. Higher = darker.",
                              "Desligado: a opção de brilho do próprio jogo (agora %.2f), como no console. Maior = mais escuro."),
               (double)s_ui.title_gamma);
      hint(b); }

    if (ImGui::SliderFloat(L("Sharpening", "Nitidez"), &s_ui.sharpen, 0.0f, 1.0f, s_ui.sharpen > 0.0f ? "%.2f" : L("off", "desligada"))) {
        rsx_metal_gfx_set_sharpen(s_ui.sharpen);
        set_value(V_SHARPEN, "%.2f", s_ui.sharpen);
    }
    env_mark(V_SHARPEN);
    if (ImGui::SliderFloat(L("Vibrance", "Vibrance"), &s_ui.vibrance, -1.0f, 1.0f,
                           s_ui.vibrance != 0.0f ? "%+.2f" : L("off", "desligada"))) {
        rsx_metal_gfx_set_vibrance(s_ui.vibrance);
        set_value(V_VIBRANCE, "%.2f", s_ui.vibrance);
    }
    env_mark(V_VIBRANCE);
    if (ImGui::SliderFloat(L("Contrast", "Contraste"), &s_ui.contrast, 0.5f, 1.5f,
                           s_ui.contrast != 1.0f ? "%.2f" : L("normal", "normal"))) {
        rsx_metal_gfx_set_contrast(s_ui.contrast);
        set_value(V_CONTRAST, "%.2f", s_ui.contrast);
    }
    env_mark(V_CONTRAST);
    if (ImGui::SliderFloat(L("Vignette", "Vinheta"), &s_ui.vignette, 0.0f, 1.0f,
                           s_ui.vignette > 0.0f ? "%.2f" : L("off", "desligada"))) {
        rsx_metal_gfx_set_vignette(s_ui.vignette);
        set_value(V_VIGNETTE, "%.2f", s_ui.vignette);
    }
    env_mark(V_VIGNETTE);
    if (ImGui::SliderFloat(L("Film grain", "Granulado"), &s_ui.grain, 0.0f, 1.0f,
                           s_ui.grain > 0.0f ? "%.2f" : L("off", "desligado"))) {
        rsx_metal_gfx_set_grain(s_ui.grain);
        set_value(V_GRAIN, "%.2f", s_ui.grain);
    }
    env_mark(V_GRAIN);
    if (ImGui::SliderFloat(L("Clarity", "Claridade"), &s_ui.clarity, -1.0f, 1.0f,
                           s_ui.clarity != 0.0f ? "%+.2f" : L("off", "desligada"))) {
        rsx_metal_gfx_set_clarity(s_ui.clarity);
        set_value(V_CLARITY, "%.2f", s_ui.clarity);
    }
    env_mark(V_CLARITY);
    { static const int vars[3] = { V_SHADOWS, V_MIDTONES, V_HIGHLIGHTS };
      const char* names[3] = { L("Shadows", "Sombras"), L("Midtones", "Meios-tons"), L("Highlights", "Realces") };
      for (int k = 0; k < 3; k++) {
          if (ImGui::SliderFloat(names[k], &s_ui.curve[k], -1.0f, 1.0f,
                                 s_ui.curve[k] != 0.0f ? "%+.2f" : L("normal", "normal"))) {
              rsx_metal_gfx_set_curve(k, s_ui.curve[k]);
              set_value(vars[k], "%.2f", s_ui.curve[k]);
          }
          env_mark(vars[k]);
      } }
    { static const char* const lp_val[5] = { "off", "warm", "cool", "film", "vivid" };
      const char* lp_name[5] = { rsx_metal_gfx_lut_file() ? L("your LUT file", "seu arquivo de LUT") : L("off", "desligado"),
                                 L("Warm", "Quente"), L("Cool", "Frio"), L("Film", "Filme"), L("Vivid", "Vívido") };
      if (ImGui::Combo(L("Colour preset", "Predefinição de cor"), &s_ui.lut_preset, lp_name, 5)) {
          rsx_metal_gfx_set_lut_preset(s_ui.lut_preset);
          set_value(V_LUT_PRESET, "%s", lp_val[s_ui.lut_preset]);
          s_ui.lut = rsx_metal_gfx_lut_strength();
      } }
    env_mark(V_LUT_PRESET);
    if (s_ui.lut >= 0.0f) {
        if (ImGui::SliderFloat(L("Colour LUT", "LUT de cor"), &s_ui.lut, 0.0f, 1.0f, "%.2f")) {
            rsx_metal_gfx_set_lut_strength(s_ui.lut);
            set_value(V_LUT_STRENGTH, "%.2f", s_ui.lut);
        }
        env_mark(V_LUT_STRENGTH);
    }
    { static const char* const aa_val[3] = { "off", "fxaa", "smaa" };
      const char* aa_name[3] = { L("off", "desligado"), "FXAA", "SMAA" };
      if (ImGui::Combo(L("Anti-aliasing", "Antisserrilhamento"), &s_ui.aa, aa_name, 3)) {
          rsx_metal_gfx_set_aa(s_ui.aa);
          set_value(V_AA, "%s", aa_val[s_ui.aa]);
      } }
    env_mark(V_AA);

    ImGui::BeginDisabled(s_ui.upscale < 0);
    { bool up = s_ui.upscale > 0;
      if (ImGui::Checkbox(L("MetalFX upscaling to the window", "Escala MetalFX até a janela"), &up)) {
          rsx_metal_gfx_set_upscale(up);
          s_ui.upscale = rsx_metal_gfx_upscale();
          set_value(V_UPSCALE, "%s", up ? "metalfx" : "off");
      } }
    ImGui::EndDisabled();
    env_mark(V_UPSCALE);
    if (s_ui.upscale < 0) hint(L("Not available here (headless, or a build without MetalFX).",
                                 "Indisponível aqui (sem janela, ou build sem MetalFX)."));

    static const int an_v[] = { 0, 2, 4, 8, 16 };
    static const char* const an_en[] = { "off", "2x", "4x", "8x", "16x" };
    static const char* const an_pt[] = { "desligado", "2x", "4x", "8x", "16x" };
    int ai = 0;
    for (int i = 0; i < 5; i++) if (s_ui.aniso >= an_v[i]) ai = i;
    if (ImGui::Combo(L("Anisotropic filtering", "Filtragem anisotrópica"), &ai, s_pt ? an_pt : an_en, 5)) {
        s_ui.aniso = an_v[ai];
        set_value(V_ANISO, "%d", s_ui.aniso);
    }
    env_mark(V_ANISO);
    if (s_ui.aniso != s_ui.aniso_start)
        ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.20f, 1.0f), "%s", L("Applies when the game restarts.", "Vale quando o jogo reiniciar."));
    else
        hint(L("Applies when the game restarts.", "Vale quando o jogo reiniciar."));

    /* PS3_RES_SCALE: the game's render targets at 1.5, 2 or 3 times 720p. */
    ImGui::BeginDisabled(s_ui.res_start < 0);
    { const char* rn_en[4] = { "720p (native)", "1080p (1.5x)", "1440p (2x)", "4K (3x)" };
      const char* rn_pt[4] = { "720p (nativa)", "1080p (1,5x)", "1440p (2x)", "4K (3x)" };
      int ri = s_ui.res < 0 ? 0 : s_ui.res;
      if (ImGui::Combo(L("Internal resolution", "Resolução interna"), &ri, s_pt ? rn_pt : rn_en, 4)) {
          s_ui.res = ri;
          set_value(V_RES_SCALE, "%s", kResScale[ri]);
      } }
    ImGui::EndDisabled();
    env_mark(V_RES_SCALE);
    if (s_ui.res_start < 0)
        hint(L("Not available here (the old vtable path).", "Indisponível aqui (caminho vtable antigo)."));
    else if (s_ui.res != s_ui.res_start)
        ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.20f, 1.0f), "%s", L("Applies when the game restarts.", "Vale quando o jogo reiniciar."));
    else
        hint(L("Applies when the game restarts. Higher costs GPU time (see the HUD).",
               "Vale quando o jogo reiniciar. Mais alta custa tempo de GPU (veja o HUD)."));

    /* PS3_ASPECT: a wider picture (hor+), the HUD kept 16:9. */
    ImGui::BeginDisabled(s_ui.asp_start < 0);
    { const char* an_en[3] = { "16:9 (original)", "21:9 (ultrawide)", "32:9 (super ultrawide)" };
      const char* an_pt[3] = { "16:9 (original)", "21:9 (ultrawide)", "32:9 (super ultrawide)" };
      int ai = s_ui.asp < 0 ? 0 : s_ui.asp;
      if (ImGui::Combo(L("Aspect ratio", "Proporção da tela"), &ai, s_pt ? an_pt : an_en, 3)) {
          s_ui.asp = ai;
          set_value(V_ASPECT, "%s", kAspect[ai]);
      } }
    ImGui::EndDisabled();
    env_mark(V_ASPECT);
    if (s_ui.asp_start < 0)
        hint(L("Not available here (the old vtable path).", "Indisponível aqui (caminho vtable antigo)."));
    else if (s_ui.asp != s_ui.asp_start)
        ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.20f, 1.0f), "%s", L("Applies when the game restarts.", "Vale quando o jogo reiniciar."));
    else
        hint(L("Applies when the game restarts. Wider shows more to the sides; the HUD stays 16:9.",
               "Vale quando o jogo reiniciar. Mais larga mostra mais dos lados; o HUD fica em 16:9."));

    section(L("Display", "Tela"));
    ImGui::BeginDisabled(s_ui.vsync < 0);
    { bool v = s_ui.vsync > 0;
      if (ImGui::Checkbox(L("Vsync", "Vsync (sincronia vertical)"), &v)) {
          rsx_metal_set_vsync(v); s_ui.vsync = v;
          set_value(V_VSYNC, "%d", v ? 1 : 0);
      } }
    ImGui::EndDisabled();
    env_mark(V_VSYNC);
    ImGui::BeginDisabled(s_ui.wmode < 0);
    { const char* modes[] = { L("Window", "Janela"), L("Fullscreen (macOS)", "Tela cheia (macOS)"),
                              L("Borderless fullscreen", "Tela cheia sem bordas") };
      int m = s_ui.wmode < 0 ? 0 : s_ui.wmode;
      if (ImGui::Combo(L("Display mode", "Modo de exibição"), &m, modes, 3)) {
          rsx_metal_set_window_mode(m); s_ui.wmode = m;
          static const char* const names[] = { "windowed", "fullscreen", "borderless" };
          set_value(V_WINDOW_MODE, "%s", names[m]);
      } }
    ImGui::EndDisabled();
    env_mark(V_WINDOW_MODE);
}

static void tab_audio(void)
{
    section(L("Sound", "Som"));
    if (ImGui::SliderInt(L("Master volume", "Volume geral"), &s_ui.volume, 0, 100, "%d%%")) {
        cellAudio_host_set_volume(s_ui.volume);
        set_value(V_VOLUME, "%d", s_ui.volume);
    }
    env_mark(V_VOLUME);
    hint(L("What the Mac plays; the game's own mix and its volume options are untouched.",
           "O que o Mac toca; a mixagem do jogo e as opções de volume dele não mudam."));
}

/* ---- the Controls tab: the bindings table ----------------------------------- */
/* PS3 controls in the table's order (the launcher's), with their labels. */
static const struct { int ctl; const char* en; const char* pt; } kRows[] = {
    { PIM_CROSS, "Cross", "Xis" }, { PIM_CIRCLE, "Circle", "Bola" },
    { PIM_SQUARE, "Square", "Quadrado" }, { PIM_TRIANGLE, "Triangle", "Triângulo" },
    { PIM_L1, "L1", "L1" }, { PIM_R1, "R1", "R1" }, { PIM_L2, "L2", "L2" }, { PIM_R2, "R2", "R2" },
    { PIM_L3, "L3", "L3" }, { PIM_R3, "R3", "R3" }, { PIM_START, "START", "START" }, { PIM_SELECT, "SELECT", "SELECT" },
    { PIM_UP, "D-pad up", "Direcional cima" }, { PIM_DOWN, "D-pad down", "Direcional baixo" },
    { PIM_LEFT, "D-pad left", "Direcional esquerda" }, { PIM_RIGHT, "D-pad right", "Direcional direita" },
    { PIM_LS_UP, "Left stick up", "Analógico esq. cima" }, { PIM_LS_DOWN, "Left stick down", "Analógico esq. baixo" },
    { PIM_LS_LEFT, "Left stick left", "Analógico esq. esquerda" }, { PIM_LS_RIGHT, "Left stick right", "Analógico esq. direita" },
    { PIM_RS_UP, "Right stick up", "Analógico dir. cima" }, { PIM_RS_DOWN, "Right stick down", "Analógico dir. baixo" },
    { PIM_RS_LEFT, "Right stick left", "Analógico dir. esquerda" }, { PIM_RS_RIGHT, "Right stick right", "Analógico dir. direita" },
};
static const char* row_label(int ctl)
{
    for (auto& r : kRows) if (r.ctl == ctl) return L(r.en, r.pt);
    return "?";
}
static const char* mouse_input_label(uint32_t bit)
{
    switch (bit) {
    case 1u: return L("Left", "Esquerdo");   case 2u: return L("Right", "Direito");
    case 4u: return L("Middle", "Meio");
    case PIM_MOUSE_WHEEL_UP: return L("Wheel up", "Roda p/ cima");
    case PIM_MOUSE_WHEEL_DOWN: return L("Wheel down", "Roda p/ baixo");
    default: break;
    }
    static char b[8][24];
    for (int i = 3; i < 8; i++) if (bit == 1u << i) {
        snprintf(b[i], sizeof b[i], L("Button %d", "Botão %d"), i + 1); return b[i]; }
    return "?";
}
/* SDL controller input -> what it is on a PlayStation pad. */
static const char* pad_input_label(int bit)
{
    switch (bit) {
    case 0: return L("Cross", "Xis");     case 1: return L("Circle", "Bola");
    case 2: return L("Square", "Quadrado"); case 3: return L("Triangle", "Triângulo");
    case 4: return "Create/Share";        case 5: return "PS";
    case 6: return "Options";             case 7: return "L3";
    case 8: return "R3";                  case 9: return "L1";
    case 10: return "R1";
    case 11: return L("D-pad up", "Direcional cima");      case 12: return L("D-pad down", "Direcional baixo");
    case 13: return L("D-pad left", "Direcional esq.");    case 14: return L("D-pad right", "Direcional dir.");
    case 15: return L("Mic", "Microfone");
    case 16: return "Paddle 1"; case 17: return "Paddle 2"; case 18: return "Paddle 3"; case 19: return "Paddle 4";
    case 20: return "Touchpad";
    case PIM_PAD_TRIGGER_LEFT: return "L2"; case PIM_PAD_TRIGGER_RIGHT: return "R2";
    default: return "?";
    }
}
/* One cell's text: the inputs of control `ctl` on device `dev`. */
static void cell_text(const PadInputMap& m, int dev, int ctl, char* out, size_t n)
{
    size_t len = 0; out[0] = 0;
    auto add = [&](const char* name) {
        int w = snprintf(out + len, n - len, "%s%s", len ? ", " : "", name);
        if (w > 0) len += (size_t)w;
        if (len >= n) len = n - 1;
    };
    char tmp[8];
    if (dev == CAP_KEYS) {
        for (unsigned vk = 0; vk < 128; vk++)
            if (m.key[ctl][vk >> 5] >> (vk & 31) & 1u) add(pim_key_label(vk, tmp, sizeof tmp));
    } else if (dev == CAP_MOUSE) {
        for (int b = 0; b < 32; b++) if (m.mouse[ctl] >> b & 1u) add(mouse_input_label(1u << b));
    } else {
        for (int b = 0; b < 32; b++) if (m.pad[ctl] >> b & 1u) add(pad_input_label(b));
    }
    if (!len) snprintf(out, n, "-");
}

static char s_bind_note[256];

/* A change to the live map: on to cellPad at once, then into the file. */
static void bindings_apply(const PadInputMap& m)
{
    cellPad_host_set_input_map(&m);
    const int r = cellPad_host_save_input_map();
    char path[1024];
    cellPad_host_input_map_path(path, sizeof path);
    const char* base = strrchr(path, '/'); base = base ? base + 1 : path;
    if (r > 0) snprintf(s_bind_note, sizeof s_bind_note, L("Saved to %s (the launcher's Controls screen shows it too).",
                                                          "Salvo em %s (a tela Controles do launcher mostra também)."), base);
    else if (r == 0) snprintf(s_bind_note, sizeof s_bind_note, "%s", L("Applies now; not saved (PS3_INPUT_MAP is empty).",
                                                                       "Vale agora; não foi salvo (PS3_INPUT_MAP vazia)."));
    else snprintf(s_bind_note, sizeof s_bind_note, L("Applies now, but %s could not be written.",
                                                     "Vale agora, mas não foi possível gravar %s."), path);
}

/* Render thread, every frame: a finished capture changes the map; a
 * controller capture looks for a newly pressed button; any of them gives up
 * after kCapTimeout. */
static void cap_poll(void)
{
    int dev = -1, ctl = 0, res = CAP_PENDING; uint32_t val = 0;
    pthread_mutex_lock(&s_cap_lock);
    if (s_cap_dev >= 0) {
        if (s_cap_res == CAP_PENDING && s_cap_dev == CAP_PAD) {
            const uint32_t raw = g_pad_overlay_raw;
            const uint32_t fresh = raw & ~s_cap_pad_base;
            s_cap_pad_base &= raw;            /* let go of a held one: it counts when pressed again */
            if (fresh) { s_cap_res = CAP_BIND; s_cap_val = (uint32_t)__builtin_ctz(fresh); }
        }
        if (s_cap_res == CAP_PENDING && now_s() - s_cap_t0 > kCapTimeout) s_cap_res = CAP_CANCEL;
        if (s_cap_res != CAP_PENDING) {
            dev = s_cap_dev; ctl = s_cap_ctl; res = s_cap_res; val = s_cap_val;
            s_cap_dev = -1;
        }
    }
    pthread_mutex_unlock(&s_cap_lock);
    if (dev < 0 || res == CAP_CANCEL) return;
    static const int sec[3] = { PIM_SEC_KEYBOARD, PIM_SEC_MOUSE_BUTTONS, PIM_SEC_CONTROLLER };
    PadInputMap m;
    cellPad_host_input_map(&m);
    uint32_t taken = 0;
    if (res == CAP_BIND) taken = pim_rebind(&m, sec[dev], ctl, val);
    else pim_unbind(&m, sec[dev], ctl);
    bindings_apply(m);
    if (taken) {   /* say where it came from, as the launcher does */
        char from[160] = ""; size_t len = 0;
        for (auto& r : kRows)
            if (taken >> r.ctl & 1u) {
                int w = snprintf(from + len, sizeof from - len, "%s%s", len ? ", " : "", L(r.en, r.pt));
                if (w > 0) len += (size_t)w;
                if (len >= sizeof from) len = sizeof from - 1;
            }
        const size_t n0 = strlen(s_bind_note);
        snprintf(s_bind_note + n0, sizeof s_bind_note - n0, L(" Taken from: %s.", " Tirado de: %s."), from);
    }
}

static void cap_start(int dev, int ctl)
{
    pthread_mutex_lock(&s_cap_lock);
    s_cap_dev = dev; s_cap_ctl = ctl; s_cap_res = CAP_PENDING; s_cap_val = 0;
    s_cap_t0 = now_s(); s_cap_wheel = 0;
    s_cap_pad_base = g_pad_overlay_raw;   /* what is held now (the Cross that chose the cell) does not count */
    pthread_mutex_unlock(&s_cap_lock);
    s_bind_note[0] = 0;
}

static void bindings_table(void)
{
    PadInputMap m;
    cellPad_host_input_map(&m);
    int cap_dev, cap_ctl; double left;
    pthread_mutex_lock(&s_cap_lock);
    cap_dev = (s_cap_dev >= 0 && s_cap_res == CAP_PENDING) ? s_cap_dev : -1;
    cap_ctl = s_cap_ctl;
    left = kCapTimeout - (now_s() - s_cap_t0);
    pthread_mutex_unlock(&s_cap_lock);

    const ImVec4 orange(1.0f, 0.60f, 0.20f, 1.0f);
    if (cap_dev >= 0) {
        char what[160];
        snprintf(what, sizeof what,
                 cap_dev == CAP_KEYS  ? L("%s: press a key (Esc cancels, %.0f s)", "%s: aperte uma tecla (Esc cancela, %.0f s)") :
                 cap_dev == CAP_MOUSE ? L("%s: click a mouse button or turn the wheel (Esc cancels, Backspace unbinds, %.0f s)",
                                          "%s: clique um botão do mouse ou gire a roda (Esc cancela, Backspace limpa, %.0f s)") :
                                        L("%s: press a button on the controller (Esc cancels, %.0f s)",
                                          "%s: aperte um botão do controle (Esc cancela, %.0f s)"),
                 row_label(cap_ctl), left > 0 ? left : 0.0);
        ImGui::TextColored(orange, "%s", what);
        if (cap_dev != CAP_MOUSE) {
            if (ImGui::SmallButton(L("Unbind", "Limpar"))) cap_finish(CAP_UNBIND, 0);
            ImGui::SameLine();
            if (ImGui::SmallButton(L("Cancel", "Cancelar"))) cap_finish(CAP_CANCEL, 0);
        }
    } else {
        hint(L("Click a cell (or pick it with the controller and press Cross), then press the new key, mouse button "
               "or controller button. Right-click a cell to unbind it. The change applies at once.",
               "Clique numa célula (ou escolha com o controle e aperte Xis) e aperte a nova tecla, botão do mouse "
               "ou botão do controle. Botão direito numa célula a limpa. A mudança vale na hora."));
    }

    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("binds", 4, tf)) {
        ImGui::TableSetupColumn(L("PS3", "PS3"), ImGuiTableColumnFlags_WidthStretch, 1.25f);
        ImGui::TableSetupColumn(L("Keyboard", "Teclado"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(L("Mouse", "Mouse"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(L("Controller", "Controle"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        char text[160], id[200];
        for (auto& r : kRows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(L(r.en, r.pt));
            for (int dev = 0; dev < 3; dev++) {
                ImGui::TableSetColumnIndex(dev + 1);
                const bool here = cap_dev == dev && cap_ctl == r.ctl;
                if (here) snprintf(text, sizeof text, "%s", L("press...", "aperte..."));
                else cell_text(m, dev, r.ctl, text, sizeof text);
                snprintf(id, sizeof id, "%s##b%d_%d", text, dev, r.ctl);
                if (here) ImGui::PushStyleColor(ImGuiCol_Text, orange);
                if (ImGui::Button(id, ImVec2(-FLT_MIN, 0)) && cap_dev < 0) cap_start(dev, r.ctl);
                if (here) ImGui::PopStyleColor();
                if (!here && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) &&
                    ImGui::CalcTextSize(text).x > ImGui::GetItemRectSize().x - 8.0f)
                    ImGui::SetTooltip("%s", text);   /* the whole list when the cell cuts it */
                if (cap_dev < 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                    PadInputMap u = m;
                    static const int sec[3] = { PIM_SEC_KEYBOARD, PIM_SEC_MOUSE_BUTTONS, PIM_SEC_CONTROLLER };
                    pim_unbind(&u, sec[dev], r.ctl);
                    bindings_apply(u);
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(cap_dev >= 0);
    if (ImGui::Button(L("Restore defaults", "Restaurar padrões"))) {
        PadInputMap d;
        pim_defaults(&d);
        bindings_apply(d);
        snprintf(s_bind_note + strlen(s_bind_note), sizeof s_bind_note - strlen(s_bind_note), "%s",
                 L(" Default layout restored.", " Layout padrão restaurado."));
    }
    ImGui::EndDisabled();
    if (s_bind_note[0]) hint(s_bind_note);
    hint(L("Esc, F1, F12 and the Cmd shortcuts belong to the menu and the window and cannot be bound. "
           "The controller's sticks always move the PS3 sticks.",
           "Esc, F1, F12 e os atalhos com Cmd são do menu e da janela e não podem ser usados. "
           "Os analógicos do controle sempre movem os analógicos do PS3."));
}

static void tab_controls(void)
{
    section(L("Play with", "Jogar com"));
    { const char* modes[] = { L("Controller, keyboard and mouse", "Controle, teclado e mouse"),
                              L("Controller only", "Só o controle"),
                              L("Keyboard and mouse only", "Só teclado e mouse") };
      if (ImGui::Combo(L("Input", "Entrada"), &s_ui.input_mode, modes, 3)) {
          cellPad_host_set_input_mode(s_ui.input_mode);
          static const char* const names[] = { "both", "controller", "keyboard" };
          set_value(V_INPUT_MODE, "%s", names[s_ui.input_mode]);
      } }
    env_mark(V_INPUT_MODE);
    hint(L("Applies at once. With the keyboard and mouse only, L3+R3 on a controller still opens this menu.",
           "Vale na hora. Com só teclado e mouse, L3+R3 no controle ainda abre este menu."));

    { const char* icons[] = { L("Automatic (the controller in use)", "Automático (o controle em uso)"),
                              "PlayStation", "Xbox", L("Keyboard and mouse", "Teclado e mouse") };
      if (ImGui::Combo(L("Button icons", "Ícones dos botões"), &s_ui.button_icons, icons, 4)) {
          ps3_button_icons_set_mode(s_ui.button_icons);
          static const char* const names[] = { "auto", "playstation", "xbox", "keyboard" };
          set_value(V_BUTTON_ICONS, "%s", names[s_ui.button_icons]);
      } }
    env_mark(V_BUTTON_ICONS);
    { const int eff = ps3_button_icons_effective();
      const char* now = eff == 2 ? "Xbox" : eff == 3 ? L("keyboard and mouse", "teclado e mouse") : "PlayStation";
      char line[200];
      snprintf(line, sizeof line, L("The glyphs in the game's prompts (\"Press X to ...\"). Automatic follows the last device you "
                                    "used: an Xbox-style pad, a PlayStation one, or the keys you bound. Showing now: %s.",
                                    "Os símbolos nos avisos do jogo (\"Pressione X para ...\"). Automático segue o último aparelho "
                                    "usado: um controle estilo Xbox, um PlayStation, ou as teclas que você escolheu. Agora: %s."), now);
      hint(line); }

    section(L("Controller", "Controle"));
    if (ImGui::Checkbox(L("Vibration", "Vibração"), &s_ui.rumble)) {
        cellPad_host_set_rumble_strength(s_ui.rumble ? s_ui.rumble_strength : 0);
        set_value(V_RUMBLE, "%d", s_ui.rumble ? 1 : 0);
        if (s_ui.rumble) set_value(V_RUMBLE_STRENGTH, "%d", s_ui.rumble_strength);
    }
    env_mark(V_RUMBLE);
    ImGui::BeginDisabled(!s_ui.rumble);
    if (ImGui::SliderInt(L("Vibration strength", "Força da vibração"), &s_ui.rumble_strength, 10, 200, "%d%%")) {
        cellPad_host_set_rumble_strength(s_ui.rumble_strength);
        set_value(V_RUMBLE_STRENGTH, "%d", s_ui.rumble_strength);
    }
    ImGui::EndDisabled();
    env_mark(V_RUMBLE_STRENGTH);
    hint(L("Takes effect with the game's next vibration. Above 100% strengthens light rumble. "
           "A controller switched off and on again (or plugged in) is picked up by itself.",
           "Vale na próxima vibração do jogo. Acima de 100% reforça vibrações fracas. "
           "Um controle desligado e ligado de novo (ou conectado) é reconhecido sozinho."));

    section(L("Mouse camera", "Câmera no mouse"));
    { bool on = s_ui.mouse_on != 0;
      if (ImGui::Checkbox(L("The mouse turns the camera", "O mouse gira a câmera"), &on)) {
          s_ui.mouse_on = on;
          cellPad_host_set_mouse_camera(on);
          set_value(V_MOUSE, "%d", on ? 1 : 0);
      } }
    env_mark(V_MOUSE);
    ImGui::BeginDisabled(!s_ui.mouse_on);
    bool ch = false;
    if (ImGui::SliderFloat(L("Sensitivity", "Sensibilidade"), &s_ui.mouse_sens, 0.10f, 5.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic)) {
        ch = true; set_value(V_MOUSE_SENS, "%.2f", s_ui.mouse_sens);
    }
    env_mark(V_MOUSE_SENS);
    if (ImGui::Checkbox(L("Invert vertical", "Inverter vertical"), &s_ui.inv_y)) { ch = true; set_value(V_MOUSE_INVERT_Y, "%d", s_ui.inv_y ? 1 : 0); }
    env_mark(V_MOUSE_INVERT_Y);
    ImGui::SameLine(0, 30);
    if (ImGui::Checkbox(L("Invert horizontal", "Inverter horizontal"), &s_ui.inv_x)) { ch = true; set_value(V_MOUSE_INVERT_X, "%d", s_ui.inv_x ? 1 : 0); }
    env_mark(V_MOUSE_INVERT_X);
    if (ch) cellPad_host_set_mouse_settings(s_ui.mouse_sens, s_ui.inv_x, s_ui.inv_y);
    ImGui::EndDisabled();
    if (!s_ui.mouse_on) hint(L("Off: the mouse buttons do nothing in the game either.",
                               "Desligada: os botões do mouse também não fazem nada no jogo."));

    section(L("Buttons", "Botões"));
    bindings_table();

    section(L("This menu", "Este menu"));
    hint(L("Opens and closes with F1 (fn+F1 on a Mac keyboard), Insert or Cmd+, ; L3+R3 together on a controller. "
           "Esc or Circle closes it. While it is open the game gets no input (it keeps running: pause it first with START). "
           "L1/R1 switch tabs. F12 captures the current frame in debug mode.",
           "Abre e fecha com F1 (fn+F1 no teclado do Mac), Insert ou Cmd+, ; L3+R3 juntos no controle. "
           "Esc ou Bola fecha. Aberto, o jogo não recebe comandos (mas continua rodando: pause antes com START). "
           "L1/R1 trocam de aba. O F12 captura o quadro atual no modo de depuração."));
}


/* ---- the Game tab: language ------------------------------------------------
 * The game reads its language once, at boot, so these are saved for the next
 * start. PS3_LANG is the console language; PS3_VOICE_LANG / PS3_TEXT_LANG
 * split the dub from what is read on screen (runtime/ppu/ppu_fs_lang.h
 * serves the chosen language's files). The lists are the languages this copy
 * has (its cache/psarc folder). */
static const struct { const char* name; const char* en; const char* pt; } kLangNames[] = {
    { "brazilian", "Portuguese (Brazil)", "Português (Brasil)" },
    { "portuguese", "Portuguese (Portugal)", "Português (Portugal)" },
    { "english", "English (US)", "Inglês (EUA)" }, { "british", "English (UK)", "Inglês (Reino Unido)" },
    { "latino", "Spanish (Latin America)", "Espanhol (América Latina)" },
    { "spanish", "Spanish (Spain)", "Espanhol (Espanha)" },
    { "french", "French", "Francês" }, { "german", "German", "Alemão" }, { "italian", "Italian", "Italiano" },
    { "dutch", "Dutch", "Holandês" }, { "polish", "Polish", "Polonês" }, { "russian", "Russian", "Russo" },
    { "turkish", "Turkish", "Turco" }, { "japanese", "Japanese", "Japonês" }, { "korean", "Korean", "Coreano" },
    { "chinese", "Chinese (traditional)", "Chinês tradicional" },
    { "chinese_s", "Chinese (simplified)", "Chinês simplificado" },
    { "finnish", "Finnish", "Finlandês" }, { "swedish", "Swedish", "Sueco" },
    { "danish", "Danish", "Dinamarquês" }, { "norwegian", "Norwegian", "Norueguês" },
};
static const char* lang_label(const char* name)
{
    for (const auto& k : kLangNames) if (!strcmp(k.name, name)) return L(k.en, k.pt);
    return name;
}
/* PS3_LANG codes, as the launcher offers them (the Americas release answers
 * pt and es with brazilian and latino; the toolkit plays the other ones with
 * their own files, see ppu_fs_lang.h). */
static const struct { const char* code; const char* name; } kLangCodes[] = {
    { "pt", "brazilian" }, { "pt-pt", "portuguese" }, { "en", "english" }, { "en-gb", "british" },
    { "es", "latino" }, { "es-es", "spanish" }, { "fr", "french" }, { "de", "german" }, { "it", "italian" },
    { "nl", "dutch" }, { "pl", "polish" }, { "ru", "russian" }, { "tr", "turkish" }, { "ja", "japanese" },
    { "ko", "korean" }, { "zh-t", "chinese" },
};

struct LangUi {
    bool  loaded;
    char  voice[32][24], text[32][24];
    int   nvoice, ntext;
    char  start[3][48];      /* PS3_LANG / _VOICE / _TEXT as the game started */
    char  cur[3][48];        /* what the menu shows ("" = same as the language) */
};
static LangUi s_lang;

static void lang_ui_load(void)
{
    if (s_lang.loaded) return;
    s_lang.loaded = true;
    s_lang.nvoice = ps3_lang_available(0, s_lang.voice, 32);
    s_lang.ntext = ps3_lang_available(1, s_lang.text, 32);
    static const int vars[3] = { V_LANG, V_VOICE_LANG, V_TEXT_LANG };
    for (int i = 0; i < 3; i++) {
        const char* e = getenv(kVar[vars[i]]);
        snprintf(s_lang.start[i], sizeof s_lang.start[i], "%s", e ? e : "");
        if (i && (!strcmp(s_lang.start[i], "same") || !strcmp(s_lang.start[i], "auto"))) s_lang.start[i][0] = 0;
        snprintf(s_lang.cur[i], sizeof s_lang.cur[i], "%s", s_lang.start[i]);
    }
}

static bool lang_has(char (*list)[24], int n, const char* name)
{
    for (int i = 0; i < n; i++) if (!strcmp(list[i], name)) return true;
    return false;
}

/* One of the two "same as the language / <name>" selects. */
static void lang_pick(int which, const char* label, char (*list)[24], int n, const char* none_hint)
{
    char* cur = s_lang.cur[which];
    char preview[96];
    snprintf(preview, sizeof preview, "%s", cur[0] ? lang_label(cur) : L("Same as the language", "Igual ao idioma"));
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable(L("Same as the language", "Igual ao idioma"), !cur[0])) {
            cur[0] = 0;
            set_value(which == 1 ? V_VOICE_LANG : V_TEXT_LANG, "same");
        }
        for (int i = 0; i < n; i++) {
            char item[96];
            snprintf(item, sizeof item, "%s##%s", lang_label(list[i]), list[i]);
            if (ImGui::Selectable(item, !strcmp(cur, list[i]))) {
                snprintf(cur, sizeof s_lang.cur[which], "%s", list[i]);
                set_value(which == 1 ? V_VOICE_LANG : V_TEXT_LANG, "%s", list[i]);
            }
        }
        ImGui::EndCombo();
    }
    env_mark(which == 1 ? V_VOICE_LANG : V_TEXT_LANG);
    if (none_hint) hint(none_hint);
}

static void tab_game(void)
{
    lang_ui_load();
    section(L("Language", "Idioma"));

    /* PS3_LANG */
    {
        char* cur = s_lang.cur[0];
        const char* shown = cur;
        for (const auto& c : kLangCodes) if (!strcasecmp(c.code, cur)) { shown = lang_label(c.name); break; }
        char preview[96];
        snprintf(preview, sizeof preview, "%s", cur[0] ? shown : L("System (the Mac's)", "Do sistema (o do Mac)"));
        if (ImGui::BeginCombo(L("Game language", "Idioma do jogo"), preview)) {
            for (const auto& c : kLangCodes) {
                if (s_lang.ntext && !lang_has(s_lang.text, s_lang.ntext, c.name)) continue;   /* not in this copy */
                char item[96];
                snprintf(item, sizeof item, "%s (%s)", lang_label(c.name), c.code);
                if (ImGui::Selectable(item, !strcasecmp(c.code, cur))) {
                    snprintf(cur, sizeof s_lang.cur[0], "%s", c.code);
                    set_value(V_LANG, "%s", c.code);
                }
            }
            ImGui::EndCombo();
        }
        env_mark(V_LANG);
        hint(L("Menus, dub and subtitles, unless Voice or Text below says otherwise.",
               "Menus, dublagem e legendas, a não ser que Voz ou Texto abaixo digam outra coisa."));
    }
    lang_pick(1, L("Voice", "Voz"), s_lang.voice, s_lang.nvoice,
              L("The dub: dialogue and the movies' voice track. Korean, Chinese and UK English have no dub of their own.",
                "A dublagem: os diálogos e a faixa de voz dos filmes. Coreano, chinês e inglês do Reino Unido não têm dublagem própria."));
    lang_pick(2, L("Text (menus and subtitles)", "Texto (menus e legendas)"), s_lang.text, s_lang.ntext,
              L("Menus, HUD and subtitles change together: the font with the letters comes with the menus.",
                "Menus, HUD e legendas mudam juntos: a fonte com as letras vem com os menus."));

    bool changed = false;
    for (int i = 0; i < 3; i++) if (strcmp(s_lang.cur[i], s_lang.start[i])) changed = true;
    if (changed)
        ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.20f, 1.0f), "%s",
                           L("Applies when the game restarts: the game loads its language when it starts.",
                             "Vale quando o jogo reiniciar: o jogo carrega o idioma ao abrir."));
    else
        hint(L("Applies when the game restarts: the game loads its language when it starts.",
               "Vale quando o jogo reiniciar: o jogo carrega o idioma ao abrir."));

    section(L("Now", "Agora"));
    { char va[24], vg[24], ta[24], tg[24];
      ps3_lang_in_use(0, va, vg, sizeof va);
      ps3_lang_in_use(1, ta, tg, sizeof ta);
      ImGui::Text("%s %s", L("Voice:", "Voz:"), vg[0] ? lang_label(vg) : "-");
      ImGui::Text("%s %s", L("Text:", "Texto:"), tg[0] ? lang_label(tg) : "-");
      if ((vg[0] && strcmp(va, vg)) || (tg[0] && strcmp(ta, tg)))
          hint(L("The game asked for its own language's files; these were served instead.",
                 "O jogo pediu os arquivos do idioma dele; estes foram servidos no lugar.")); }
    if (!s_lang.ntext)
        hint(L("No language archives found in this copy (cache/psarc).",
               "Nenhum arquivo de idioma encontrado nesta cópia (cache/psarc)."));
}

static void stats_lines(bool compact)
{
    const Stats& st = s_stats;
    if (!st.valid) { ImGui::TextUnformatted(L("measuring...", "medindo...")); return; }
    ImGui::Text("%.1f fps   %.1f ms (max %.0f)", st.fps, st.avg_ms, st.max_ms);
    if (s_hud_graph || !compact) {
        float buf[FT_N]; const int n = s_ft_count < FT_N ? s_ft_count : FT_N;
        for (int i = 0; i < n; i++) buf[i] = s_ft[(s_ft_i - n + i + FT_N) % FT_N];
        char ov[32]; snprintf(ov, sizeof ov, "%.0f ms", st.max_ms);
        ImGui::PlotLines("##ft", buf, n, 0, ov, 0.0f, st.max_ms > 50.0 ? (float)st.max_ms : 50.0f,
                         ImVec2(compact ? 230.0f : 420.0f, compact ? 40.0f : 70.0f));
    }
    ImGui::Text(L("CPU %.1f cores, %d busy threads", "CPU %.1f núcleos, %d threads ocupadas"), st.cores, st.busy);
    ImGui::Text(L("RSX encode %.1f ms, GPU wait %.1f ms", "RSX codifica %.1f ms, espera GPU %.1f ms"), st.enc_ms, st.wait_ms);
}

static void tab_perf(void)
{
    section(L("Performance overlay", "Painel de desempenho"));
    { bool h = s_hud;
      if (ImGui::Checkbox(L("Show it while playing", "Mostrar durante o jogo"), &h)) {
          s_hud = h; set_value(V_HUD, "%d", h ? 1 : 0); update_active();
      } }
    env_mark(V_HUD);
    const char* corners[] = { L("Top left", "Superior esquerdo"), L("Top right", "Superior direito"),
                              L("Bottom left", "Inferior esquerdo"), L("Bottom right", "Inferior direito") };
    if (ImGui::Combo(L("Corner", "Canto"), &s_hud_corner, corners, 4)) set_value(V_HUD_CORNER, "%d", s_hud_corner);
    if (ImGui::SliderFloat(L("Opacity", "Opacidade"), &s_hud_opacity, 0.1f, 1.0f, "%.2f")) set_value(V_HUD_OPACITY, "%.2f", s_hud_opacity);
    if (ImGui::Checkbox(L("Frame time graph", "Gráfico do tempo de quadro"), &s_hud_graph)) set_value(V_HUD_GRAPH, "%d", s_hud_graph ? 1 : 0);
    section(L("Now", "Agora"));
    stats_lines(false);
    hint(L("fps and frame time count the game's presented frames. Busy threads: those using 80% or more of a core "
           "(here mostly the SPU threads). RSX encode: CPU time turning a frame's draws into Metal commands; "
           "GPU wait: time the presenter waited for the GPU. Per-SPU and FIFO timelines: PS3_FTRACE.",
           "fps e tempo de quadro contam os quadros que o jogo apresenta. Threads ocupadas: as que usam 80% ou mais de um núcleo "
           "(aqui sobretudo as das SPUs). RSX codifica: tempo de CPU para transformar os draws de um quadro em comandos Metal; "
           "espera GPU: tempo que o apresentador esperou a GPU. Linha do tempo por SPU e FIFO: PS3_FTRACE."));
}

/* "Quit game?": Quit saves the menu's settings (closing it does that), then
 * ps3_host_quit (cellSysutil.c) ends the game the way the console's Quit Game
 * does and the process exits 0. Cancel has the focus, so a stray Cross does
 * not quit. */
static void quit_confirm(void)
{
    if (s_quit_ask == 1) { ImGui::OpenPopup("###quitask"); s_quit_ask = 2; }
    if (s_quit_ask != 2) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    char title[64];
    snprintf(title, sizeof title, "%s###quitask", L("Quit the game?", "Sair do jogo?"));
    if (!ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings)) {
        s_quit_ask = 0; s_quit_cancel = false; return;
    }
    ImGui::TextUnformatted(L("Unsaved progress since the last autosave will be lost.",
                             "O progresso não salvo desde o último salvamento automático será perdido."));
    ImGui::Spacing();
    const bool quit = ImGui::Button(L("Quit", "Sair"));
    ImGui::SameLine();
    const bool cancel = ImGui::Button(L("Cancel", "Cancelar"));
    ImGui::SetItemDefaultFocus();   /* Cancel */
    if (cancel || s_quit_cancel) {
        s_quit_ask = 0; s_quit_cancel = false;
        ImGui::CloseCurrentPopup();
    } else if (quit) {
        s_quit_ask = 0;
        ImGui::CloseCurrentPopup();
        set_open(0);
        ps3_overlay_notice(L("Quitting the game...", "Saindo do jogo..."));
        ps3_host_quit("in-game menu");
    }
    ImGui::EndPopup();
}

static void draw_menu(void)
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size(700.0f, 660.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(size.x < io.DisplaySize.x - 20 ? size.x : io.DisplaySize.x - 20,
                                    size.y < io.DisplaySize.y - 20 ? size.y : io.DisplaySize.y - 20), ImGuiCond_Always);
    char title[96];
    snprintf(title, sizeof title, "%s  -  %s###ps3menu", s_title, L("Menu", "Menu"));
    if (!ImGui::Begin(title, NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End(); return;
    }
    static const int n_tabs = 5;
    const char* names[n_tabs] = { L("Image", "Imagem"), L("Audio", "Áudio"), L("Controls", "Controles"),
                                  L("Performance", "Desempenho"), L("Game", "Jogo") };
    if (ImGui::BeginTabBar("tabs")) {
        for (int i = 0; i < n_tabs; i++) {
            const ImGuiTabItemFlags f = (s_tab_want == i) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(names[i], NULL, f)) {
                s_tab = i;
                ImGui::BeginChild("page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
                switch (i) { case 0: tab_image(); break; case 1: tab_audio(); break;
                             case 2: tab_controls(); break; case 3: tab_perf(); break; default: tab_game(); break; }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        s_tab_want = -1;
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button(L("Close", "Fechar"))) set_open(0);
    ImGui::SameLine();
    if (ImGui::Button(L("Quit game", "Sair do jogo"))) s_quit_ask = 1;
    ImGui::SameLine();
    hint(s_ini_path[0] ? L("Changes apply at once and are saved when the menu closes.",
                           "As mudanças valem na hora e são salvas ao fechar o menu.")
                       : L("Changes apply at once (not saved: no PS3_OVERLAY_INI).",
                           "As mudanças valem na hora (não são salvas: sem PS3_OVERLAY_INI)."));
    quit_confirm();
    ImGui::End();
}

static void draw_hud(void)
{
    const ImGuiIO& io = ImGui::GetIO();
    const float pad = 12.0f;
    const ImVec2 pos((s_hud_corner & 1) ? io.DisplaySize.x - pad : pad, (s_hud_corner & 2) ? io.DisplaySize.y - pad : pad);
    const ImVec2 pivot((s_hud_corner & 1) ? 1.0f : 0.0f, (s_hud_corner & 2) ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(s_hud_opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));
    if (ImGui::Begin("##hud", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::PushFont(NULL, 15.0f);
        stats_lines(true);
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void draw_notices(double now)
{
    Notice n[3];
    pthread_mutex_lock(&s_notice_lock);
    memcpy(n, s_notice, sizeof n);
    pthread_mutex_unlock(&s_notice_lock);
    const ImGuiIO& io = ImGui::GetIO();
    /* Opposite the HUD's corner, at the bottom. */
    const bool left = (s_hud_corner == 3);
    float y = io.DisplaySize.y - 14.0f;
    for (int i = 0; i < 3; i++) {
        if (n[i].until <= now) continue;
        const float a = (float)((n[i].until - now) < 0.5 ? (n[i].until - now) / 0.5 : 1.0);
        ImGui::SetNextWindowPos(ImVec2(left ? 14.0f : io.DisplaySize.x - 14.0f, y), ImGuiCond_Always,
                                ImVec2(left ? 0.0f : 1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.85f * a);
        char id[16]; snprintf(id, sizeof id, "##note%d", i);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        if (ImGui::Begin(id, NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::TextUnformatted(n[i].text);
            y -= ImGui::GetWindowHeight() + 6.0f;
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

static void feed_input(ImGuiIO& io)
{
    InEv in[256]; int n;
    pthread_mutex_lock(&s_in_lock);
    n = s_in_n; memcpy(in, s_in, sizeof in[0] * (size_t)n); s_in_n = 0;
    pthread_mutex_unlock(&s_in_lock);
    if (!s_open) return;
    for (int i = 0; i < n; i++) {
        const InEv& e = in[i];
        switch (e.kind) {
        case IN_KEY: case IN_MOD: io.AddKeyEvent((ImGuiKey)e.key, e.down != 0); break;
        case IN_POS:    io.AddMousePosEvent(e.x, e.y); break;
        case IN_BUTTON: io.AddMouseButtonEvent(e.key, e.down != 0); break;
        case IN_WHEEL:  io.AddMouseWheelEvent(e.x, e.y); break;
        case IN_TEXT:   io.AddInputCharactersUTF8(e.text); break;
        }
    }
    /* The controller, as cellPad saw it before holding the game's copy neutral.
     * While a cell waits for its new input the pad works nothing in the menu
     * (its next button is the binding, see cap_poll). */
    static uint16_t prev, held_off;
    const bool capturing = cap_device() >= 0;
    const uint32_t nav = capturing ? 0x80800000u : g_pad_overlay_nav;
    const uint32_t nav2 = capturing ? 0x8080u : g_pad_overlay_nav2;
    /* ... nor does the button that ended it, until it is let go (Cross bound
     * to a cell would otherwise press that cell again at once). */
    const uint16_t b_now = (uint16_t)g_pad_overlay_nav;
    held_off = capturing ? b_now : (uint16_t)(held_off & b_now);
    const uint16_t b = (uint16_t)nav & (uint16_t)~held_off;
    const int lx = (nav >> 16) & 255, ly = (nav >> 24) & 255, rx = nav2 & 255, ry = (nav2 >> 8) & 255;
    (void)rx;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (b & PAD_UP) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (b & PAD_DOWN) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (b & PAD_LEFT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (b & PAD_RIGHT) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (b & PAD_CROSS) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (b & PAD_CIRCLE) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  (b & PAD_SQUARE) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    (b & PAD_TRIANGLE) != 0);
    auto axis = [](int v, int dir) {   /* dir -1: towards 0, +1: towards 255 */
        float f = dir < 0 ? (128 - v) / 127.0f : (v - 128) / 127.0f;
        f = (f - 0.25f) / 0.75f;
        return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    };
    struct { ImGuiKey k; float v; } sticks[] = {
        { ImGuiKey_GamepadLStickLeft, axis(lx, -1) }, { ImGuiKey_GamepadLStickRight, axis(lx, 1) },
        { ImGuiKey_GamepadLStickUp, axis(ly, -1) },   { ImGuiKey_GamepadLStickDown, axis(ly, 1) },
        { ImGuiKey_GamepadRStickUp, axis(ry, -1) },   { ImGuiKey_GamepadRStickDown, axis(ry, 1) },
    };
    for (auto& s : sticks) io.AddKeyAnalogEvent(s.k, s.v > 0.1f, s.v);
    const uint16_t pressed = b & (uint16_t)~prev;
    prev = b;
    if (s_quit_ask) { if (pressed & PAD_CIRCLE) s_quit_cancel = true; return; }
    if (pressed & PAD_L1) s_tab_want = (s_tab + 4) % 5;
    if (pressed & PAD_R1) s_tab_want = (s_tab + 1) % 5;
    if ((pressed & PAD_CIRCLE) && !ImGui::IsAnyItemActive()) set_open(0);
}

extern "C" void ps3_overlay_render(void* cbp, void* dstp)
{
    if (!s_enabled || !dstp) return;
    @autoreleasepool {
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cbp;
        id<MTLTexture> dst = (__bridge id<MTLTexture>)dstp;
        const double now = now_s();
        if (s_last_present > 0) {
            s_ft[s_ft_i] = (float)((now - s_last_present) * 1000.0);
            s_ft_i = (s_ft_i + 1) % FT_N; s_ft_count++;
        }
        const double dt = s_last_present > 0 ? now - s_last_present : 1.0 / 30.0;
        s_last_present = now;
        if (s_hud || s_open) stats_tick(now);
        const bool notices = s_notice_until > now;
        update_active();
        if (!s_open && !s_hud && !notices) { s_last_present = 0; return; }

        if (!s_imgui_ready) imgui_setup();
        static int was_open;
        if (s_open && !was_open) ui_load();
        was_open = s_open;

        ImGuiIO& io = ImGui::GetIO();
        const float W = (float)[dst width], H = (float)[dst height];
        const float scale = H / 720.0f > 0.25f ? H / 720.0f : 0.25f;   /* the UI is sized for 720 lines */
        io.DisplaySize = ImVec2(W / scale, H / scale);
        io.DisplayFramebufferScale = ImVec2(scale, scale);
        io.DeltaTime = (float)(dt > 0.0001 && dt < 1.0 ? dt : 1.0 / 30.0);
        io.MouseDrawCursor = false;
        feed_input(io);
        if (!s_open) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture     = dst;
        rp.colorAttachments[0].loadAction  = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        ImGui_ImplMetal_NewFrame(rp);
        ImGui::NewFrame();
        if (s_open) { cap_poll(); draw_menu(); }
        if (s_hud && !s_open) draw_hud();
        if (notices) draw_notices(now);
        ImGui::Render();
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (enc) {
            [enc pushDebugGroup:@"ps3 overlay"];
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            [enc popDebugGroup];
            [enc endEncoding];
        }
    }
}
