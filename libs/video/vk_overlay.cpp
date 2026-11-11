/*
 * vk_overlay -- the Vulkan backend's in-game overlay (see vk_overlay.h).
 *
 * Dear ImGui (third_party/imgui v1.92.9b, MIT) with its own Vulkan renderer
 * (imgui_impl_vulkan, entry points loaded through the backend's
 * vkGetInstanceProcAddr: the backend loads libvulkan at run time) and SDL2
 * platform backend (imgui_impl_sdl2, started the first time the menu opens:
 * the HUD alone needs no input and leaves SDL's cursor and capture alone).
 *
 * It draws into the swapchain image after the game's frame was blitted into
 * it: a barrier to COLOR_ATTACHMENT_OPTIMAL, then its own render pass (load,
 * store, final layout PRESENT_SRC_KHR), so the image leaves ready to present.
 *
 * The settings, the HUD's numbers and the menu's pages follow the Metal
 * backend's ps3_overlay.mm (same variable-file format, same "env" marks,
 * same pad navigation from cellPad's g_pad_overlay_nav).
 */
#include "vk_overlay.h"

#include <SDL2/SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_sdl2.h"

#include "../../include/ps3emu/ps3_settings.h"
#include "../../include/ps3emu/ps3_lightfx.h"   /* [light] the Lighting page (vk_overlay_light.inc) */
#include "../../include/ps3emu/ps3_game_mods.h" /* [mods] the Mods page: runtime/ppu/ppu_fs_mods.h */
#include "vk_postfx.h"
#include "vk_overlay_theme.h"   /* the inFAMOUS look: Hero (blue) / Infamous (red) */
#include "../../include/ps3emu/ps3_pad_icons.h"
#include "../../runtime/spu/spu_patch.h"   /* [trim] the SPU instruction alternatives (Mesh trimming) */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"   /* the parser half is cellPad's */
#endif
#include "../input/pad_input_map.h"                     /* PadInputMap, labels */
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sys/resource.h>
#include <sys/stat.h>
#endif

/* ---- what the menu changes: the toolkit's live setters ------------------- */
extern "C" {
int   cellAudio_host_volume(void);
void  cellAudio_host_set_volume(int pct);
int   cellPad_host_rumble_strength(void);
void  cellPad_host_set_rumble_strength(int pct);
int   cellPad_host_input_mode(void);
void  cellPad_host_set_input_mode(int mode);
void  cellPad_host_input_map(PadInputMap* out);
void  ps3_button_icons_set_mode(int mode);      /* rsx_button_icons.h */
int   ps3_button_icons_mode_setting(void);
int   ps3_button_icons_effective(void);
void  ps3_host_quit(const char* why);           /* libs/system/cellSysutil.c */
void  ps3_sysutil_host_language(char* out, size_t n);   /* cellSysutil.c: "Automatic"'s PS3_LANG code */
int   ps3_sysutil_language_in_use(void);                /* CELL_SYSUTIL_LANG_* the game was told, -1 */
void  ps3_lang_in_use(int kind, char* asked, char* given, size_t n);   /* runtime/ppu/ppu_fs_lang.h */
int   ps3_lang_available(int kind, char (*out)[24], int max);
void  ps3_lang_missing(int kind, char* out, size_t n);   /* the dub wanted but not on the disc */
int   vk_overlay_cheats_count(void);            /* vk_overlay_cheats.cpp: the title's cheats (ps3_cheats.h) */
void  vk_overlay_cheats_page(float w);
int   ps3_overlay_cheat_set(const char* id, double v);   /* below: "cheat=id=value" in the timed script */
int   vk_overlay_cheats_owns_key(const char* k);         /* its PS3_CHEAT_* lines of the settings file */
void  vk_overlay_cheats_write(FILE* f);
void  vk_overlay_cheats_attach(void);                    /* the remembered cheats, at window open */
int   vk_overlay_cheats_remember(void);
/* [saves] the system's save-data list (vk_overlay_saves.cpp, include/ps3emu/ps3_savedlg.h): it
 * takes the menu's window while up -- the same input hold, cursor and RSX-thread drawing */
int   vk_overlay_saves_active(void);
int   vk_overlay_saves_poll(void);                 /* 1: open the window for it, 2: close it */
void  vk_overlay_saves_draw(float scale);
void  vk_overlay_saves_pad(uint32_t buttons, int lx, int ly);
void  vk_overlay_saves_back(void);                 /* Esc */
void  vk_overlay_saves_attach(void);
void  vk_overlay_saves_shutdown(void);
extern volatile uint32_t g_pad_overlay_inject;   /* cellPad: scripted buttons while a system dialog holds the pad */
extern volatile int      g_pad_overlay_open;   /* libs/input/cellPad.c */
extern volatile int      g_pad_overlay_present; /* cellPad: a menu exists, Esc keeps the mouse capture for after it */
extern volatile uint32_t g_pad_overlay_nav, g_pad_overlay_nav2, g_pad_overlay_raw;
}

volatile int g_vk_overlay_active;
volatile int g_vk_overlay_on_worker;   /* PS3RECOMP_VK_PRESENT_ASYNC: the recording thread is drawing */
/* ImGui's context: the HUD may be built on the recording thread while the
 * RSX thread's event pump opens or closes the menu (set_open touches io). */
static std::recursive_mutex s_imgui_lock;

/* CELL_PAD_CTRL_* (libs/input/cellPad.h) */
enum {
    PAD_SELECT = 1 << 0, PAD_L3 = 1 << 1, PAD_R3 = 1 << 2, PAD_START = 1 << 3,
    PAD_UP = 1 << 4, PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8, PAD_R2 = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13, PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15,
};

static double now_s(void)
{
    static double inv;
    if (!inv) inv = 1.0 / (double)SDL_GetPerformanceFrequency();
    return (double)SDL_GetPerformanceCounter() * inv;
}

static bool env_on(const char* v)
{
    const char* e = getenv(v);
    return e && *e && strcmp(e, "0") != 0;
}

/* ---- settings and their file ---------------------------------------------
 * The file is a list of VARIABLE=value lines with the same names and values
 * as the environment variables. Before main, each line whose variable is not
 * set is exported, so every module reads it the way it always read its
 * variable; one that is set wins. Lines for variables the menu does not
 * manage are applied the same way and kept when the file is rewritten. */
enum {
    V_FPS_CAP, V_PRESENT, V_FULLSCREEN,
    V_AA, V_SHARPEN, V_RENDER_SCALE, V_ANISO, V_UPSCALE,
    V_DOF, V_EDGE_BLUR, V_MESH_TRIM, V_EDGE_VP,
    V_VOLUME,
    V_INPUT_MODE, V_BUTTON_ICONS, V_RUMBLE, V_RUMBLE_STRENGTH,
    V_HUD, V_HUD_CORNER, V_HUD_OPACITY, V_HUD_GRAPH, V_HUD_CPU,
    V_THEME,
    V_LANG, V_VOICE_LANG, V_TEXT_LANG,
    V_ASPECT,                 /* [wide] */
    V_GAME_MODS,              /* [mods] */
    V_N
};
static const char* const kVar[V_N] = {
    "PS3RECOMP_FPS_CAP", "PS3RECOMP_VK_PRESENT", "PS3RECOMP_VK_FULLSCREEN",
    "PS3RECOMP_AA", "PS3RECOMP_SHARPEN", "PS3RECOMP_RENDER_SCALE", "PS3RECOMP_ANISO", "PS3RECOMP_UPSCALE",
    "PS3RECOMP_DOF", "PS3RECOMP_EDGE_BLUR", "PS3RECOMP_MESH_TRIM", "PS3RECOMP_EDGE_VP",
    "PS3_AUDIO_VOLUME",
    "PS3_INPUT_MODE", "PS3_BUTTON_ICONS", "PS3_RUMBLE", "PS3_RUMBLE_STRENGTH",
    "PS3_OVERLAY_HUD", "PS3_OVERLAY_HUD_CORNER", "PS3_OVERLAY_HUD_OPACITY",
    "PS3_OVERLAY_HUD_GRAPH", "PS3_OVERLAY_HUD_CPU",
    "PS3_OVERLAY_THEME",
    "PS3_LANG", "PS3_VOICE_LANG", "PS3_TEXT_LANG",   /* the game reads them once, at start */
    "PS3RECOMP_ASPECT",                               /* [wide] read once, at start */
    "PS3_GAME_MODS",                                  /* [mods] read once, at start */
};
/* Plain arrays: filled during this file's dynamic initialisation, before
 * main (zero-initialised storage exists before any of it runs). */
static bool s_env_set[V_N];          /* in the environment when the game started */
static char s_saved[V_N][48];        /* the file's value ("" = not in it) */
static char s_value[V_N][48];        /* what the menu set this session ("" = untouched) */
static char s_start[V_N][48];        /* the value in effect at start (env, file or default) */
enum { EXTRA_N = 32 };
static char s_extra[EXTRA_N][2][160];   /* other VAR=value lines of the file, kept */
static int  s_n_extra;
static char s_ini_path[1024];
static bool s_dirty;
static double s_dirty_t;
static std::mutex s_set_lock;

static bool overlay_off(void)
{
    const char* e = getenv("PS3_OVERLAY");
    return e && e[0] == '0' && e[1] == 0;
}

static void trim(char*& s)
{
    while (*s == ' ' || *s == '\t' || *s == '"') s++;
    for (char* e = s + strlen(s) - 1; e >= s && (*e == '\n' || *e == '\r' || *e == ' ' || *e == '\t' || *e == '"'); e--) *e = 0;
}

static int var_index(const char* k)
{
    for (int i = 0; i < V_N; i++) if (!strcmp(k, kVar[i])) return i;
    return -1;
}

static void default_ini_path(char* out, size_t n)
{
    out[0] = 0;
#ifdef _WIN32
    const char* base = getenv("APPDATA");
    if (base && *base) snprintf(out, n, "%s\\inFAMOUS2-port\\settings.ini", base);
#else
    const char* x = getenv("XDG_CONFIG_HOME");
    const char* h = getenv("HOME");
    if (x && *x) snprintf(out, n, "%s/infamous2-port/settings.ini", x);
    else if (h && *h) snprintf(out, n, "%s/.config/infamous2-port/settings.ini", h);
#endif
}

static void preload(void)
{
    if (overlay_off()) return;
    for (int i = 0; i < V_N; i++) s_env_set[i] = getenv(kVar[i]) != NULL;
    /* Headless runs never read the player's file. */
    if (!env_on("PS3RECOMP_VK_WINDOW")) return;
    const char* p = getenv("PS3_OVERLAY_INI");
    if (p) snprintf(s_ini_path, sizeof s_ini_path, "%s", p);   /* "" = none */
    else default_ini_path(s_ini_path, sizeof s_ini_path);
    int applied = 0, kept = 0;
    bool in_file[V_N] = {};
    if (s_ini_path[0]) {
        FILE* f = fopen(s_ini_path, "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                char* s = line;
                while (*s == ' ' || *s == '\t') s++;
                if (*s == '#' || *s == ';' || *s == '[' || !*s || *s == '\n' || *s == '\r') continue;
                char* eq = strchr(s, '=');
                if (!eq) continue;
                *eq = 0;
                char* k = s; char* v = eq + 1;
                for (char* e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
                trim(v);
                if (!*k) continue;
                const int i = var_index(k);
                if (i >= 0) {
                    snprintf(s_saved[i], sizeof s_saved[i], "%s", v);
                    in_file[i] = true;
                } else if (vk_overlay_cheats_owns_key(k)) {
                    /* the Cheats page writes its own lines (vk_overlay_cheats_write) */
                } else if (s_n_extra < EXTRA_N) {
                    snprintf(s_extra[s_n_extra][0], sizeof s_extra[0][0], "%s", k);
                    snprintf(s_extra[s_n_extra][1], sizeof s_extra[0][1], "%s", v);
                    s_n_extra++;
                }
                if (getenv(k)) { kept++; continue; }
                ps3_setting_setenv(k, v);
                applied++;
            }
            fclose(f);
        }
    }
    /* PS3_OVERLAY_DEFAULTS="K=V;K=V": a launcher's defaults, below the file. */
    if (const char* d = getenv("PS3_OVERLAY_DEFAULTS")) {
        char buf[1024];
        int n = 0;
        snprintf(buf, sizeof buf, "%s", d);
        for (char* tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
            char* eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            char* k = tok; char* v = eq + 1;
            trim(k); trim(v);
            const int i = var_index(k);
            if (!*k || getenv(k) || (i >= 0 && in_file[i])) continue;
            ps3_setting_setenv(k, v);
            n++;
        }
        fprintf(stderr, "[overlay] PS3_OVERLAY_DEFAULTS: %d default(s) applied\n", n);
    }
    for (int i = 0; i < V_N; i++) {
        const char* e = getenv(kVar[i]);
        snprintf(s_start[i], sizeof s_start[i], "%s", e ? e : "");
    }
    if (s_ini_path[0])
        fprintf(stderr, "[overlay] %s: %d setting(s) applied, %d left to the environment\n",
                s_ini_path, applied, kept);
}
static struct Preload { Preload() { preload(); } } s_preload_run;

static void set_value(int v, const char* fmt, ...)
{
    char buf[48];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> g(s_set_lock);
    if (strcmp(s_value[v], buf)) {
        snprintf(s_value[v], sizeof s_value[v], "%s", buf);
        s_dirty = true;
        s_dirty_t = now_s();
    }
}

/* A setting that is not in kVar (a page with its own list, e.g. the Lighting
 * page, vk_overlay_light.inc): kept with the file's other lines and written
 * at the next save. */
static void extra_set(const char* name, const char* value)
{
    std::lock_guard<std::mutex> g(s_set_lock);
    int i = 0;
    while (i < s_n_extra && strcmp(s_extra[i][0], name)) i++;
    if (i == s_n_extra) {
        if (s_n_extra >= EXTRA_N) return;
        snprintf(s_extra[i][0], sizeof s_extra[i][0], "%s", name);
        s_extra[i][1][0] = 0;
        s_n_extra++;
    }
    if (strcmp(s_extra[i][1], value)) {
        snprintf(s_extra[i][1], sizeof s_extra[i][1], "%s", value);
        s_dirty = true;
        s_dirty_t = now_s();
    }
}

/* A setting a module may take live (ps3_settings.h): environment, applier,
 * and the file. 1 = live now. */
/* [theme] the menu's look (vk_overlay_theme.h: Hero blue / Infamous red). A new
 * one is put in at the start of the next frame, never inside one: a colour
 * pushed before the switch would be popped back to the old look. -1 = none. */
static volatile int s_theme_want = -1;
static int apply_setting(int v, const char* value)
{
    set_value(v, "%s", value);
    if (v == V_THEME) s_theme_want = vk_ov_theme_parse(value);   /* [theme] the look is the overlay's own */
    return ps3_setting_apply(kVar[v], value);
}

static void mkdirs_for(const char* path)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    for (char* q = dir + 1; *q; q++) {
        if (*q != '/' && *q != '\\') continue;
        const char c = *q; *q = 0;
        if (!(q == dir + 2 && dir[1] == ':')) {   /* not "C:" itself */
#ifdef _WIN32
            CreateDirectoryA(dir, NULL);
#else
            mkdir(dir, 0755);
#endif
        }
        *q = c;
    }
}

/* Write the file: what it had plus what the menu changed. 1 = written. */
static int settings_save(void)
{
    if (!s_ini_path[0]) return 0;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_ini_path);
    mkdirs_for(s_ini_path);
    FILE* f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "# inFAMOUS 2 port -- settings of the in-game menu (F2, or hold L3+R3).\n"
               "# Same names and values as the environment variables; a variable that is\n"
               "# set when the game starts wins over its line here.\n");
    {
        std::lock_guard<std::mutex> g(s_set_lock);
        for (int i = 0; i < V_N; i++) {
            const char* v = s_value[i][0] ? s_value[i] : s_saved[i];
            if (*v) fprintf(f, "%s=%s\n", kVar[i], v);
            if (s_value[i][0]) snprintf(s_saved[i], sizeof s_saved[i], "%s", s_value[i]);
        }
        for (int i = 0; i < s_n_extra; i++) fprintf(f, "%s=%s\n", s_extra[i][0], s_extra[i][1]);
        if (vk_overlay_cheats_count() > 0) vk_overlay_cheats_write(f);
        s_dirty = false;
    }
    bool ok = fclose(f) == 0;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, s_ini_path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileA(tmp);
#else
    ok = ok && rename(tmp, s_ini_path) == 0;
    if (!ok) remove(tmp);
#endif
    return ok ? 1 : 0;
}

/* ---- state ---------------------------------------------------------------- */
static vk_overlay_attach_info s_vk;
static bool s_attached, s_enabled, s_imgui_ready, s_sdl_ready, s_vk_ready, s_failed;
static volatile int s_open;
static volatile int s_hud = 1;
static int   s_hud_corner = 0;            /* 0 top left, 1 top right, 2 bottom left, 3 bottom right */
static float s_hud_opacity = 0.55f;
static bool  s_hud_graph = true;
static bool  s_hud_cpu = true;
static int   s_tab, s_tab_want = -1;     /* the menu's page; one to switch to */
static int   s_ntabs = 5;                /* pages in draw_menu's list (L1/R1 wrap round it) */
static volatile int  s_quit_ask;         /* 1 open the confirmation, 2 showing */
static volatile bool s_quit_cancel;
static double s_t_attach;
static bool  s_ui_stale;                 /* a setting changed outside the menu: re-read its view */
static bool  s_pad_chord = true;
static bool  s_rel_mouse_saved;
static char  s_gpu_name[256];
static int   s_refresh_hz;

static VkFormat s_sc_format = VK_FORMAT_UNDEFINED;
static bool s_color_ok;

/* Vulkan entry points the overlay itself uses (ImGui loads its own). */
static PFN_vkCreateRenderPass    p_vkCreateRenderPass;
static PFN_vkDestroyRenderPass   p_vkDestroyRenderPass;
static PFN_vkCreateFramebuffer   p_vkCreateFramebuffer;
static PFN_vkDestroyFramebuffer  p_vkDestroyFramebuffer;
static PFN_vkCreateImageView     p_vkCreateImageView;
static PFN_vkDestroyImageView    p_vkDestroyImageView;
static PFN_vkCmdPipelineBarrier  p_vkCmdPipelineBarrier;
static PFN_vkCmdBeginRenderPass  p_vkCmdBeginRenderPass;
static PFN_vkCmdEndRenderPass    p_vkCmdEndRenderPass;
static PFN_vkDeviceWaitIdle      p_vkDeviceWaitIdle;
static PFN_vkGetPhysicalDeviceProperties p_vkGetPhysicalDeviceProperties;

static VkRenderPass s_rp;
static VkFormat     s_rp_format = VK_FORMAT_UNDEFINED;
enum { SC_MAX = 8 };
static struct { VkImage img; VkImageView view; VkFramebuffer fb; VkExtent2D ext; } s_fb[SC_MAX];

/* Notices (any thread). */
struct Notice { char text[160]; double until; };
static Notice s_notice[3];
static std::mutex s_notice_lock;
static volatile double s_notice_until;

static void update_active(void)
{
    g_vk_overlay_active = (s_enabled && s_color_ok && !s_failed &&
                           (s_open || s_hud || s_notice_until > now_s())) ? 1 : 0;
}

/* [saves] a system dialog can be drawn (vk_overlay_saves.cpp waits for the player) */
extern "C" int vk_overlay_can_show(void) { return s_enabled && s_color_ok && !s_failed; }

static void notice_for(const char* text, double secs)
{
    if (!s_enabled || !text) return;
    {
        std::lock_guard<std::mutex> g(s_notice_lock);
        memmove(&s_notice[1], &s_notice[0], sizeof s_notice[0] * 2);
        snprintf(s_notice[0].text, sizeof s_notice[0].text, "%s", text);
        s_notice[0].until = now_s() + secs;
        if (s_notice[0].until > s_notice_until) s_notice_until = s_notice[0].until;
    }
    update_active();
}

extern "C" void vk_overlay_notice(const char* text) { notice_for(text, 3.0); }
extern "C" void vk_overlay_notice_for(const char* text, double secs) { notice_for(text, secs); }
/* Something outside this file's settings changed what the file holds (the Cheats page). */
extern "C" void vk_overlay_settings_changed(void)
{
    std::lock_guard<std::mutex> g(s_set_lock);
    s_dirty = true;
    s_dirty_t = now_s();
}
static void set_open(int o);
/* Esc with the menu closed (the backend's pump, after cellPad took it as the
 * window's key): open the menu. 1 = opened (an overlay exists). */
extern "C" int vk_overlay_open_menu(void) { if (!s_enabled) return 0; set_open(1); return 1; }

/* ---- the menu opening and closing ------------------------------------------ */
extern "C" int vk_overlay_menu_open(void) { return s_enabled && s_open; }
static void set_open(int o)
{
    if (!s_enabled || (o != 0) == (s_open != 0)) return;
    if (!o && vk_overlay_saves_active()) return;   /* [saves] the save-data list closes itself */
    std::lock_guard<std::recursive_mutex> il(s_imgui_lock);
    s_open = o;
    g_pad_overlay_open = o;          /* cellPad: every port reads neutral while open */
    if (o) {
        /* The cursor back for the menu, whatever the game had done with it. */
        s_rel_mouse_saved = SDL_GetRelativeMouseMode() == SDL_TRUE;
        if (s_rel_mouse_saved) SDL_SetRelativeMouseMode(SDL_FALSE);
        if (s_imgui_ready) {
            ImGuiIO& io = ImGui::GetIO();
            io.ClearInputKeys();
            io.ClearInputMouse();
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        }
    } else {
        s_quit_ask = 0;
        if (s_imgui_ready) {
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            io.ClearInputKeys();
            io.ClearInputMouse();
        }
        if (s_sdl_ready) SDL_CaptureMouse(SDL_FALSE);
        if (s_rel_mouse_saved) SDL_SetRelativeMouseMode(SDL_TRUE);
        s_rel_mouse_saved = false;
        if (s_dirty) {
            if (settings_save()) vk_overlay_notice("Settings saved");
            else if (!s_ini_path[0]) vk_overlay_notice("Settings kept for this session (PS3_OVERLAY_INI is empty)");
            else vk_overlay_notice("Could not save the settings");
        }
    }
    update_active();
}

static void set_hud(bool on)
{
    s_hud = on ? 1 : 0;
    set_value(V_HUD, "%d", on ? 1 : 0);
    update_active();
}

/* [theme] the applier (so the menu marks the setting "live" and ps3_setting_apply takes it) */
static int theme_apply(const char* value) { s_theme_want = vk_ov_theme_parse(value); return 1; }

static void toggle_fullscreen(void)
{
    SDL_Window* w = (SDL_Window*)s_vk.sdl_window;
    const bool fs = w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!apply_setting(V_FULLSCREEN, fs ? "0" : "1"))
        vk_overlay_notice("Fullscreen: applies when the game restarts");
    s_ui_stale = true;
}

/* The menu's pages: the settings pages, then Cheats (always the last one) when the title
 * has any. A new settings page goes before it: TAB_CHEATS is the number of settings pages
 * ([light] Lighting = 5, [fx] Effects = 6). */
enum { TAB_MODS = 7, TAB_CHEATS = 8, MENU_TABS = TAB_CHEATS };   /* [mods] Mods before Cheats */
static int tab_count(void) { return TAB_CHEATS + (vk_overlay_cheats_count() > 0 ? 1 : 0); }

static int tab_from_name(const char* e)
{
    static const char* const names[TAB_CHEATS + 1] = { "display", "graphics", "audio", "controls", "hud", "lighting", "effects", "mods", "cheats" };
    if (!e || !*e) return -1;
    if (e[0] >= '0' && e[0] <= '9') { const int t = atoi(e); return t >= 0 && t < tab_count() ? t : 0; }
    for (int i = 0; i < tab_count(); i++) if (!SDL_strcasecmp(e, names[i])) return i;
    if (!SDL_strcasecmp(e, "performance") || !SDL_strcasecmp(e, "perf")) return 4;
    if (!SDL_strcasecmp(e, "language") || !SDL_strcasecmp(e, "lang")) return 2;   /* Audio holds Language */
    return -1;
}

/* PS3RECOMP_OVERLAY_MENU_AT="40:display,50:audio,60:close" (tests). */
enum { SCRIPT_N = 16 };
static struct { double t; char what[64]; bool done; } s_script[SCRIPT_N];
static int s_n_script;
static void script_load(void)
{
    const char* e = getenv("PS3RECOMP_OVERLAY_MENU_AT");
    if (!e || !*e) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", e);
    for (char* tok = strtok(buf, ",; "); tok && s_n_script < SCRIPT_N; tok = strtok(NULL, ",; ")) {
        char* c = strchr(tok, ':');
        s_script[s_n_script].t = atof(tok);
        snprintf(s_script[s_n_script].what, sizeof s_script[0].what, "%s", c ? c + 1 : "open");
        s_n_script++;
    }
}
static void script_poll(double now)
{
    for (int i = 0; i < s_n_script; i++) {
        if (s_script[i].done || now - s_t_attach < s_script[i].t) continue;
        s_script[i].done = true;
        const char* w = s_script[i].what;
        fprintf(stderr, "[overlay] script t=%.0f s: %s\n", s_script[i].t, w);
        if (!SDL_strcasecmp(w, "close")) set_open(0);
        else if (!SDL_strcasecmp(w, "hud0")) set_hud(false);
        else if (!SDL_strcasecmp(w, "hud1")) set_hud(true);
        else if (!SDL_strncasecmp(w, "set=", 4)) {   /* "set=VAR=value": as the menu would set it */
            char k[64]; snprintf(k, sizeof k, "%s", w + 4);
            char* eq = strchr(k, '=');
            const int v = eq ? (*eq = 0, var_index(k)) : -1;
            if (v == V_VOLUME) { cellAudio_host_set_volume(atoi(eq + 1)); set_value(v, "%d", atoi(eq + 1)); }
            else if (v >= 0) fprintf(stderr, "[overlay] %s=%s: %s\n", k, eq + 1, apply_setting(v, eq + 1) ? "live" : "at the next start");
            s_ui_stale = true;
        }
        else if (!SDL_strncasecmp(w, "cheat=", 6)) {   /* "cheat=id=value": as the Cheats page would set it */
            char k[64]; snprintf(k, sizeof k, "%s", w + 6);
            char* eq = strchr(k, '=');
            if (eq) { *eq = 0; ps3_overlay_cheat_set(k, atof(eq + 1)); }
        }
        else { const int t = tab_from_name(w); if (t >= 0) s_tab_want = t; set_open(1); }
    }
}

/* ---- events from the pump -------------------------------------------------- */
static std::mutex s_ev_lock;
static SDL_Event s_ev[128];
static int s_ev_n;
static void ev_push(const SDL_Event* e)
{
    std::lock_guard<std::mutex> g(s_ev_lock);
    if (s_ev_n < 128) s_ev[s_ev_n++] = *e;
}

static void lfx_compare_toggle(void);   /* [light] F3, vk_overlay_light.inc */

extern "C" int vk_overlay_event(const void* ev)
{
    if (!s_enabled || !ev) return 0;
    const SDL_Event* e = (const SDL_Event*)ev;
    switch (e->type) {
    case SDL_KEYDOWN: case SDL_KEYUP: {
        const SDL_Keycode k = e->key.keysym.sym;
        const bool press = e->type == SDL_KEYDOWN && !e->key.repeat;
        if (k == SDLK_F1) { if (press) { set_hud(!s_hud); if (!s_hud) vk_overlay_notice("Performance HUD off (F1 shows it)"); } return 1; }
        if (k == SDLK_F2) { if (press && !vk_overlay_saves_active()) set_open(!s_open); return 1; }
        if (k == SDLK_F3) { if (press) lfx_compare_toggle(); return 1; }   /* [light] lighting A/B */
        if (k == SDLK_F11 || (k == SDLK_RETURN && (e->key.keysym.mod & KMOD_ALT))) {
            if (press) toggle_fullscreen();
            return 1;
        }
        if (!s_open) return 0;
        if (k == SDLK_ESCAPE) {
            if (press) { if (vk_overlay_saves_active()) vk_overlay_saves_back(); else if (s_quit_ask) s_quit_cancel = true; else set_open(0); }
            return 1;
        }
        ev_push(e);
        return 1;
    }
    case SDL_TEXTINPUT: case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
        if (!s_open) return 0;
        ev_push(e);
        return 1;
    case SDL_WINDOWEVENT:
        if (s_open) ev_push(e);      /* focus and leave events: the backend acts on them too */
        return 0;
    default:
        return 0;
    }
}

extern "C" void vk_overlay_poll(void)
{
    if (!s_enabled) return;
    const double now = now_s();
    if (s_n_script) script_poll(now);
    switch (vk_overlay_saves_poll()) {   /* [saves] the save-data list opens / closes the window */
    case 1: set_open(1); if (s_open) g_pad_overlay_open = 2; break;   /* 2: scripted input goes to it (cellPad) */
    case 2: set_open(0); break;
    default: break;
    }
    /* Hold L3+R3 for a second: port 0 as cellPad saw it before the menu's
     * neutral copy, or the two stick clicks on any controller (SDL buttons 7
     * and 8 in g_pad_overlay_raw, whatever they are bound to). cellPad keeps
     * L3 and R3 from the game while they are held together. */
    if (s_pad_chord) {
        static double t0;
        static bool fired;
        const uint32_t sticks = (1u << 7) | (1u << 8);
        const bool both = (g_pad_overlay_nav & (PAD_L3 | PAD_R3)) == (PAD_L3 | PAD_R3) ||
                          (g_pad_overlay_raw & sticks) == sticks;
        if (!both) { t0 = 0; fired = false; }
        else if (!t0) t0 = now;
        else if (!fired && now - t0 >= 1.0) { fired = true; set_open(!s_open); }
    }
}

/* ---- performance numbers ---------------------------------------------------- */
enum { FT_N = 240 };
static float  s_ft[FT_N];               /* ms between presents */
static int    s_ft_i, s_ft_count;
static double s_last_present;
struct Stats { double fps, avg_ms, max_ms, low1_fps; bool valid; };
static Stats  s_stats;

struct CpuSample { float proc_pct, cores, rsx_pct, top_pct; int busy, ncpu; bool valid; };
static std::mutex s_cpu_lock;
static CpuSample s_cpu;

#ifdef _WIN32
/* Per-thread CPU on a helper thread once a second (a thread snapshot of the
 * whole system can take a millisecond or two: not on the present thread). */
static volatile LONG s_cpu_want;
static HANDLE s_cpu_thread;
static DWORD  s_rsx_tid;
static unsigned long long ft64(const FILETIME& f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static DWORD WINAPI cpu_thread_main(LPVOID)
{
    enum { TN = 512 };
    struct Th { DWORD tid; HANDLE h; unsigned long long last; bool seen; };
    static Th th[TN];
    int n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    const int ncpu = (int)si.dwNumberOfProcessors;
    const DWORD pid = GetCurrentProcessId();
    unsigned long long p_last = 0, wall_last = 0;
    int ticks = 0;
    for (;;) {
        Sleep(1000);
        if (!s_cpu_want) { p_last = 0; continue; }
        FILETIME c, x, k, u, nowf;
        GetSystemTimeAsFileTime(&nowf);
        const unsigned long long wall = ft64(nowf);
        if ((ticks++ % 5) == 0) {        /* new threads every 5 s */
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te; te.dwSize = sizeof te;
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != pid) continue;
                    bool have = false;
                    for (int i = 0; i < n; i++) if (th[i].tid == te.th32ThreadID) { have = true; break; }
                    if (have || n >= TN) continue;
                    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (!h) continue;
                    th[n].tid = te.th32ThreadID; th[n].h = h; th[n].seen = false;
                    th[n].last = (GetThreadTimes(h, &c, &x, &k, &u)) ? ft64(k) + ft64(u) : 0;
                    n++;
                }
                CloseHandle(snap);
            }
        }
        CpuSample cs = {};
        cs.ncpu = ncpu;
        if (GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) {
            const unsigned long long t = ft64(k) + ft64(u);
            if (p_last && wall > wall_last) {
                cs.cores = (float)((double)(t - p_last) / (double)(wall - wall_last));
                cs.proc_pct = cs.cores * 100.0f / (float)(ncpu > 0 ? ncpu : 1);
                cs.valid = true;
            }
            p_last = t;
        }
        const double span = wall > wall_last && wall_last ? (double)(wall - wall_last) : 0.0;
        for (int i = 0; i < n; ) {
            DWORD code = 0;
            if (!GetThreadTimes(th[i].h, &c, &x, &k, &u) || (GetExitCodeThread(th[i].h, &code) && code != STILL_ACTIVE)) {
                CloseHandle(th[i].h);
                th[i] = th[--n];
                continue;
            }
            const unsigned long long t = ft64(k) + ft64(u);
            if (span > 0 && th[i].seen) {
                const float pct = (float)((double)(t - th[i].last) * 100.0 / span);
                if (th[i].tid == s_rsx_tid) cs.rsx_pct = pct;
                else if (pct > cs.top_pct) cs.top_pct = pct;
                if (pct >= 80.0f) cs.busy++;
            }
            th[i].last = t; th[i].seen = true;
            i++;
        }
        wall_last = wall;
        std::lock_guard<std::mutex> g(s_cpu_lock);
        s_cpu = cs;
    }
}
#endif

static void cpu_want(bool on)
{
#ifdef _WIN32
    s_cpu_want = on ? 1 : 0;
    if (on && !s_cpu_thread) {
        s_cpu_thread = CreateThread(NULL, 64 * 1024, cpu_thread_main, NULL, 0, NULL);
        if (s_cpu_thread) SetThreadPriority(s_cpu_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
#else
    (void)on;
#endif
}

static int cmp_float(const void* a, const void* b)
{
    const float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

static double s_fps_target = 60.0;      /* the cap, or 60 uncapped: the HUD's colours */
static double fps_target(void) { return s_fps_target; }
static void fps_target_read(void)
{
    const char* e = getenv("PS3RECOMP_FPS_CAP");
    const double f = e ? atof(e) : 0.0;
    s_fps_target = f > 0.0 ? f : 60.0;
}

static double s_stats_t0;               /* 0: start a new measuring window */
static void stats_tick(double now)
{
    double& t0 = s_stats_t0;
    static int frames;
    frames++;
    if (!t0) { t0 = now; frames = 0; return; }
    const double dt = now - t0;
    if (dt < 0.5) return;
    Stats st = {};
    st.fps = frames / dt;
    const int n = s_ft_count < FT_N ? s_ft_count : FT_N;
    double sum = 0, mx = 0; int k = 0;
    for (int i = 0; i < n && i < frames; i++) {
        const float v = s_ft[(s_ft_i - 1 - i + FT_N) % FT_N];
        sum += v; if (v > mx) mx = v; k++;
    }
    st.avg_ms = k ? sum / k : 0; st.max_ms = mx;
    /* 1% low over the whole ring (the last ~2-4 s): the 99th-percentile frame. */
    if (n >= 20) {
        float tmp[FT_N];
        for (int i = 0; i < n; i++) tmp[i] = s_ft[(s_ft_i - 1 - i + FT_N) % FT_N];
        qsort(tmp, (size_t)n, sizeof tmp[0], cmp_float);
        const float p99 = tmp[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1];
        st.low1_fps = p99 > 0.0f ? 1000.0 / p99 : 0.0;
    }
    st.valid = true;
    s_stats = st;
    t0 = now; frames = 0;
    fps_target_read();
#ifndef _WIN32
    { static double c0, w0;
      struct rusage ru; getrusage(RUSAGE_SELF, &ru);
      const double c = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
      if (w0 > 0 && now > w0) { std::lock_guard<std::mutex> g(s_cpu_lock); s_cpu.cores = (float)((c - c0) / (now - w0)); s_cpu.valid = true; }
      c0 = c; w0 = now; }
#endif
}


/* ---- ImGui ---------------------------------------------------------------------- */
static ImGuiStyle s_base_style;
static float s_scale_applied = -1.0f;

static void check_vk(VkResult r)
{
    if (r < 0) fprintf(stderr, "[overlay] Vulkan error %d in the ImGui renderer\n", (int)r);
}

static PFN_vkVoidFunction load_vk(const char* name, void*)
{
    PFN_vkVoidFunction f = s_vk.get_instance_proc_addr(s_vk.instance, name);
    return f;
}

static bool load_own_vk(void)
{
#define OV_LOAD(n) if (!(p_##n = (PFN_##n)s_vk.get_instance_proc_addr(s_vk.instance, #n))) return false;
    OV_LOAD(vkCreateRenderPass) OV_LOAD(vkDestroyRenderPass)
    OV_LOAD(vkCreateFramebuffer) OV_LOAD(vkDestroyFramebuffer)
    OV_LOAD(vkCreateImageView) OV_LOAD(vkDestroyImageView)
    OV_LOAD(vkCmdPipelineBarrier) OV_LOAD(vkCmdBeginRenderPass) OV_LOAD(vkCmdEndRenderPass)
    OV_LOAD(vkDeviceWaitIdle) OV_LOAD(vkGetPhysicalDeviceProperties)
#undef OV_LOAD
    return true;
}

static void fb_destroy_all(void)
{
    for (int i = 0; i < SC_MAX; i++) {
        if (s_fb[i].fb) p_vkDestroyFramebuffer(s_vk.device, s_fb[i].fb, NULL);
        if (s_fb[i].view) p_vkDestroyImageView(s_vk.device, s_fb[i].view, NULL);
        s_fb[i].fb = VK_NULL_HANDLE; s_fb[i].view = VK_NULL_HANDLE; s_fb[i].img = VK_NULL_HANDLE;
    }
}

static bool render_pass_create(VkFormat fmt)
{
    VkAttachmentDescription att = {};
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;          /* the game's frame is in it */
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = 0;
    VkRenderPassCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1; ci.pAttachments = &att;
    ci.subpassCount = 1; ci.pSubpasses = &sub;
    ci.dependencyCount = 2; ci.pDependencies = dep;
    if (p_vkCreateRenderPass(s_vk.device, &ci, NULL, &s_rp) != VK_SUCCESS) { s_rp = VK_NULL_HANDLE; return false; }
    s_rp_format = fmt;
    return true;
}

static bool vk_backend_init(void)
{
    static bool loaded;
    if (!loaded && !(loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, load_vk, NULL))) {
        fprintf(stderr, "[overlay] Vulkan entry points for ImGui missing\n");
        return false;
    }
    if (!render_pass_create(s_sc_format)) { fprintf(stderr, "[overlay] render pass creation failed\n"); return false; }
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion = VK_API_VERSION_1_0;
    ii.Instance = s_vk.instance;
    ii.PhysicalDevice = s_vk.phys;
    ii.Device = s_vk.device;
    ii.QueueFamily = s_vk.queue_family;
    ii.Queue = s_vk.queue;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE * 8;   /* [saves] + up to 32 save icons */
    ii.MinImageCount = 2;
    ii.ImageCount = 4;                 /* vertex buffers rotated: > frames in flight */
    ii.PipelineInfoMain.RenderPass = s_rp;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&ii)) { fprintf(stderr, "[overlay] ImGui_ImplVulkan_Init failed\n"); return false; }
    s_vk_ready = true;
    return true;
}

static void vk_backend_shutdown(void)
{
    if (s_vk_ready) { ImGui_ImplVulkan_Shutdown(); s_vk_ready = false; }
    if (s_rp) { p_vkDestroyRenderPass(s_vk.device, s_rp, NULL); s_rp = VK_NULL_HANDLE; }
    s_rp_format = VK_FORMAT_UNDEFINED;
}

static bool file_exists(const char* p)
{
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool imgui_setup(void)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;     /* the menu keeps its own file */
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (!s_open) io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   /* the cursor is the game's */
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImFont* font = NULL;
#ifdef _WIN32
    { char win[MAX_PATH] = "C:\\Windows";
      GetWindowsDirectoryA(win, sizeof win);
      static const char* const fonts[] = { "segoeui.ttf", "arial.ttf", "verdana.ttf" };
      for (const char* f : fonts) {
          char p[MAX_PATH + 32];
          snprintf(p, sizeof p, "%s\\Fonts\\%s", win, f);
          if (file_exists(p) && (font = io.Fonts->AddFontFromFileTTF(p, 17.0f)) != NULL) break;
      } }
#else
    { static const char* const fonts[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                           "/usr/share/fonts/TTF/DejaVuSans.ttf" };
      for (const char* f : fonts) if (file_exists(f) && (font = io.Fonts->AddFontFromFileTTF(f, 17.0f)) != NULL) break; }
#endif
    if (!font) io.Fonts->AddFontDefaultVector();
    /* the bold face for headings, titles and tabs (vk_overlay_theme): the PC's own, never a game asset */
    { ImFont* bold = NULL;
#ifdef _WIN32
      char win[MAX_PATH] = "C:\\Windows";
      GetWindowsDirectoryA(win, sizeof win);
      static const char* const bolds[] = { "segoeuib.ttf", "arialbd.ttf" };
      for (const char* f : bolds) {
          char p[MAX_PATH + 32];
          snprintf(p, sizeof p, "%s\\Fonts\\%s", win, f);
          if (file_exists(p) && (bold = io.Fonts->AddFontFromFileTTF(p, 17.0f)) != NULL) break;
      }
#else
      static const char* const bolds[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                           "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf" };
      for (const char* f : bolds) if (file_exists(f) && (bold = io.Fonts->AddFontFromFileTTF(f, 17.0f)) != NULL) break;
#endif
      vk_ov_theme_set_bold(bold); }

    ImGuiStyle& st = ImGui::GetStyle();
    vk_ov_theme_style(st);
    s_base_style = st;
    s_imgui_ready = true;
    return true;
}


/* The UI is laid out for 720 lines: everything scales with the window. */
static float s_scale = 1.0f;
static float S(float v) { return v * s_scale; }
static void apply_scale(float h)
{
    float k = h / 720.0f;
    k = k < 0.5f ? 0.5f : k > 4.0f ? 4.0f : k;
    if (fabsf(k - s_scale_applied) < 0.01f) return;
    ImGuiStyle& st = ImGui::GetStyle();
    st = s_base_style;
    st.ScaleAllSizes(k);
    st.FontScaleDpi = k;
    s_scale_applied = k;
    s_scale = k;
}

static bool sdl_setup(void)
{
    if (s_sdl_ready) return true;
    if (!ImGui_ImplSDL2_InitForVulkan((SDL_Window*)s_vk.sdl_window)) return false;
    /* Controllers come from cellPad (g_pad_overlay_nav), with its mapping and
     * its port order; the backend's own gamepad reading stays off. */
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, NULL, 0);
    s_sdl_ready = true;
    return true;
}

/* ---- the menu's view of the settings, read when it opens -------------------- */
static const int kCap[] = { 30, 60, 72, 120, 144, 0 };
static const char* const kCapName[] = { "30 fps", "60 fps (recommended)", "72 fps (smooth on 144 Hz)",
                                        "120 fps", "144 fps", "Unlimited" };
static const char* const kPresent[] = { "fifo", "mailbox", "immediate" };
static const char* const kPresentName[] = { "Vsync (FIFO)", "Low latency, no tearing (Mailbox)",
                                            "Immediate (lowest latency, may tear)" };
static const char* const kAA[] = { "off", "fxaa", "smaa" };
static const char* const kAAName[] = { "Off", "FXAA (fast)", "SMAA (sharper)" };
/* [res] PS3RECOMP_RENDER_SCALE (rsx_vulkan_backend.c's [res] block) */
static const char* const kScale[] = { "1", "1.5", "2", "3" };
static const char* const kScaleName[] = { "720p  (1280x720, the PS3's)", "1080p  (1920x1080)",
                                          "1440p  (2560x1440, untested)", "4K  (3840x2160, untested)" };
static const char* const kAniso[] = { "1", "2", "4", "8", "16" };
static const char* const kAnisoName[] = { "Off (1x)", "2x", "4x", "8x", "16x" };
/* [wide] PS3RECOMP_ASPECT (vk_postfx.c ps3_aspect_wanted, docs/WIDESCREEN.md) */
static const char* const kAspect[] = { "16:9", "auto", "21:9", "32:9", "16:10" };
static const char* const kAspectName[] = { "16:9  (the game's)", "Auto  (the screen's or window's shape)",
                                           "21:9  ultrawide", "32:9  super ultrawide", "16:10" };
static const char* const kUp[] = { "fsr", "cas", "bilinear" };   /* VK_POSTFX_UP_* order */
static const char* const kUpName[] = { "AMD FSR 1 (sharpest)", "AMD CAS scaling", "Bilinear (soft)" };

struct Ui {
    int cap, cap_custom;      /* index in kCap; -1 = cap_custom fps */
    int present;
    bool fullscreen;
    int aa, scale, aniso, upscale; float sharpen;
    bool dof, edge_blur;      /* the game's own effects (rsx_draw_engine.c ENG_DOF_BLEND_FP) */
    int mesh_trim;            /* [trim]/[edgevp] 0 = the game's culling at the render resolution (default),
                               * 1 = off (whole meshes), 2 = the PS3's 720p culling */
    int aspect;               /* [wide] index in kAspect; -1 = another value (env / file) */
    int volume;
    int input_mode, button_icons;
    bool rumble; int rumble_strength;
};
static Ui s_ui;

static int pick(const char* v, const char* const* list, int n, int def)
{
    if (!v || !*v) return def;
    for (int i = 0; i < n; i++) if (!SDL_strcasecmp(v, list[i])) return i;
    return def;
}

static void lang_ui_load(void);   /* the Language section's lists (below, by tab_audio) */
static void ui_load(void)
{
    lang_ui_load();
    { const char* e = getenv(kVar[V_FPS_CAP]);
      const int f = e ? atoi(e) : 0;
      s_ui.cap = -1; s_ui.cap_custom = f;
      for (int i = 0; i < 6; i++) if (kCap[i] == f) s_ui.cap = i; }
    s_ui.present = pick(getenv(kVar[V_PRESENT]), kPresent, 3, 1);
    { SDL_Window* w = (SDL_Window*)s_vk.sdl_window;
      s_ui.fullscreen = w && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0; }
    /* image quality: the values in effect (vk_postfx.c owns the defaults) */
    { const int a = vk_postfx_aa();
      s_ui.aa = a == VK_POSTFX_AA_SMAA ? 2 : a == VK_POSTFX_AA_FXAA ? 1 : 0; }
    s_ui.sharpen = vk_postfx_sharpen();
    s_ui.upscale = vk_postfx_upscale();
    { const char* e = getenv(kVar[V_DOF]); s_ui.dof = !(e && !strcmp(e, "0")); }
    { const char* e = getenv(kVar[V_EDGE_BLUR]); s_ui.edge_blur = !(e && !strcmp(e, "0")); }
    { const int t = spu_patch_find("mesh_trim");   /* [trim] what the SPU code runs now */
      const bool culls = t < 0 || spu_patch_alt(t) == 0;
      const char* e = getenv(kVar[V_EDGE_VP]);       /* [edgevp] culling at the render resolution */
      s_ui.mesh_trim = !culls ? 1 : (e && e[0] == '0') ? 2 : 0; }
    { const char* e = getenv(kVar[V_RENDER_SCALE]);   /* [res] numbers or 720p/1080p/1440p/4k */
      double v = e ? atof(e) : 1.0;
      if (e && !SDL_strcasecmp(e, "1080p")) v = 1.5;
      else if (e && !SDL_strcasecmp(e, "1440p")) v = 2.0;
      else if (e && (!SDL_strcasecmp(e, "4k") || !SDL_strcasecmp(e, "2160p"))) v = 3.0;
      s_ui.scale = v >= 2.5 ? 3 : v >= 1.75 ? 2 : v >= 1.25 ? 1 : 0; }
    { const int v = vk_postfx_aniso();
      s_ui.aniso = v >= 16 ? 4 : v >= 8 ? 3 : v >= 4 ? 2 : v >= 2 ? 1 : 0; }
    { const char* e = getenv(kVar[V_ASPECT]);   /* [wide] what the next start reads */
      s_ui.aspect = (!e || !*e) ? 0 : pick(e, kAspect, 5, -1); }
    s_ui.volume = cellAudio_host_volume();
    s_ui.input_mode = cellPad_host_input_mode();
    s_ui.button_icons = ps3_button_icons_mode_setting();
    const int r = cellPad_host_rumble_strength();
    s_ui.rumble = r > 0;
    if (r > 0) s_ui.rumble_strength = r; else if (!s_ui.rumble_strength) s_ui.rumble_strength = 100;
}

static const ImVec4 kOrange(1.0f, 0.60f, 0.20f, 1.0f);
static const ImVec4 kGrey(0.62f, 0.66f, 0.72f, 1.0f);

/* "env" after a setting whose variable was set at start: it wins next time.
 * live = false: a setting the game only reads at start (nothing applies now). */
static void env_mark(int v, bool live = true)
{
    if (!s_env_set[v]) return;
    ImGui::SameLine();
    ImGui::TextColored(kOrange, "env");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(live ? "%s was set when the game started (the launcher or the shell).\n"
                                 "The change applies now, but that variable wins again next time."
                               : "%s was set when the game started (the launcher or the shell).\n"
                                 "The choice is saved, but that variable wins over it at the next start.", kVar[v]);
}

static void hint(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void section(const char* text) { vk_ov_heading(text); }   /* [theme] bold, accent, a fading rule */

/* "on restart", or "restart needed" once changed, after a setting read at start. */
static void restart_mark(int v, bool changed)
{
    ImGui::SameLine();
    ImGui::TextColored(changed ? kOrange : kGrey, changed ? "restart needed" : "on restart");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s is read when the game starts: a change applies the next time.", kVar[v]);
}

/* "live" or "restart" after a setting a module may or may not take live. */
static void live_mark(int v)
{
    const bool live = ps3_setting_applier(kVar[v]) != NULL;
    const char* now = getenv(kVar[v]);
    const bool changed = strcmp(now ? now : "", s_start[v]) != 0;
    if (live) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "live");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s changes while the game runs.", kVar[v]);
    } else {
        restart_mark(v, changed);
    }
    env_mark(v);
}

static float combo_w(void) { return S(330.0f); }

static void tab_display(void)
{
    section("Frame rate");
    { char preview[64];
      if (s_ui.cap >= 0) snprintf(preview, sizeof preview, "%s", kCapName[s_ui.cap]);
      else snprintf(preview, sizeof preview, "%d fps (custom)", s_ui.cap_custom);
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::BeginCombo("Frame rate cap", preview)) {
          for (int i = 0; i < 6; i++)
              if (ImGui::Selectable(kCapName[i], s_ui.cap == i)) {
                  s_ui.cap = i;
                  char b[16]; snprintf(b, sizeof b, "%d", kCap[i]);
                  apply_setting(V_FPS_CAP, b);
              }
          ImGui::EndCombo();
      } }
    live_mark(V_FPS_CAP);
    hint("Frames are shown on an even beat. inFAMOUS 2 was made for 30/60: 60 is the target, higher caps "
         "are for a high-refresh screen where the scene allows it.");
    if (s_refresh_hz > 0) { char b[64]; snprintf(b, sizeof b, "This display: %d Hz.", s_refresh_hz); hint(b); }

    section("Presentation");
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::Combo("Present mode", &s_ui.present, kPresentName, 3))
        apply_setting(V_PRESENT, kPresent[s_ui.present]);
    live_mark(V_PRESENT);
    hint("Mailbox shows the newest finished frame at each refresh: no tearing and no queue (least input lag "
         "without tearing). Vsync queues frames behind the display. Immediate may tear. A mode the driver "
         "lacks falls back to vsync.");
    if (ImGui::Checkbox("Fullscreen (borderless)", &s_ui.fullscreen))
        apply_setting(V_FULLSCREEN, s_ui.fullscreen ? "1" : "0");
    live_mark(V_FULLSCREEN);
    hint("Also Alt+Enter or F11.");
    /* [wide] PS3RECOMP_ASPECT: read at start (the game's cameras and every target) */
    { char preview[64];
      if (s_ui.aspect >= 0) snprintf(preview, sizeof preview, "%s", kAspectName[s_ui.aspect]);
      else { const char* e = getenv(kVar[V_ASPECT]); snprintf(preview, sizeof preview, "%s (custom)", e ? e : ""); }
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::BeginCombo("Aspect ratio", preview)) {
          for (int i = 0; i < 5; i++)
              if (ImGui::Selectable(kAspectName[i], s_ui.aspect == i)) { s_ui.aspect = i; apply_setting(V_ASPECT, kAspect[i]); }
          ImGui::EndCombo();
      } }
    live_mark(V_ASPECT);
    { const float k = vk_postfx_aspect_k();
      char b[400];
      snprintf(b, sizeof b, "Wider screens see more at the sides (hor+: the same vertical view as 16:9); the HUD, menus and "
               "movies stay 16:9 in the middle. Auto takes the fullscreen screen's shape (or PS3RECOMP_VK_WINDOW_SIZE's). "
               "16:10 shows a little less at the sides than 16:9. Now: %s.",
               k > 1.001f || k < 0.999f ? "wide" : "16:9");
      hint(b); }

    section("Menu");   /* [theme] */
    { static const char* const looks[] = { "Hero (blue)", "Infamous (red)" };
      int look = vk_ov_theme_red();
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::Combo("Menu theme", &look, looks, 2)) apply_setting(V_THEME, look ? "infamous" : "hero"); }
    live_mark(V_THEME);
    hint("The colours of this menu, the save list and the system's questions: Cole's blue lightning, or the red of an infamous Cole.");

    section("Now");
    { SDL_Window* w = (SDL_Window*)s_vk.sdl_window;
      int ww = 0, wh = 0; if (w) SDL_GetWindowSize(w, &ww, &wh);
      const float rs = vk_postfx_render_scale();   /* [res] the internal resolution in effect */
      const float ka = vk_postfx_aspect_k();       /* [wide] the frame's width vs 16:9 */
      ImGui::Text("Window %d x %d, game image %u x %u", ww, wh,
                  (unsigned)((float)s_vk.render_width * rs * ka + 0.5f), (unsigned)((float)s_vk.render_height * rs + 0.5f)); }
}

static void tab_graphics(void)
{
    section("Image quality");
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::Combo("Anti-aliasing", &s_ui.aa, kAAName, 3)) apply_setting(V_AA, kAA[s_ui.aa]);
    live_mark(V_AA);
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::SliderFloat("Sharpening", &s_ui.sharpen, 0.0f, 1.0f, s_ui.sharpen > 0.0f ? "%.2f" : "off")) {
        char b[16]; snprintf(b, sizeof b, "%.2f", s_ui.sharpen);
        apply_setting(V_SHARPEN, b);
    }
    live_mark(V_SHARPEN);
    ImGui::SetNextItemWidth(combo_w());
    /* [res] internal resolution: the targets are made at start, so "restart" */
    if (ImGui::Combo("Internal resolution", &s_ui.scale, kScaleName, 4)) apply_setting(V_RENDER_SCALE, kScale[s_ui.scale]);
    live_mark(V_RENDER_SCALE);
    { const float rs = vk_postfx_render_scale();
      char b[320];
      snprintf(b, sizeof b, "The game draws its 3D scene, effects and HUD at this size (now %ux%u); the window then "
               "gets it scaled with FSR or shrunk. Each step costs GPU time: 1080p draws 2.25x the pixels of 720p.",
               (unsigned)(1280.0f * rs * vk_postfx_aspect_k() + 0.5f), (unsigned)(720.0f * rs + 0.5f));   /* [wide] */
      hint(b); }
    /* [trim] Edge's SPU triangle culling (tools/spu_patches.txt switch mesh_trim, PS3RECOMP_MESH_TRIM) */
    { static const char* const kTrimName[] = { "At the render resolution (default)", "Off: whole meshes", "The PS3's 720p culling" };
      const int slot = spu_patch_find("mesh_trim");
      if (slot < 0) ImGui::BeginDisabled();
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::Combo("Triangle culling (mesh trimming)", &s_ui.mesh_trim, kTrimName, 3)) {
          apply_setting(V_MESH_TRIM, s_ui.mesh_trim == 1 ? "0" : "1");
          apply_setting(V_EDGE_VP, s_ui.mesh_trim == 2 ? "0" : "1");
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("The game's SPU geometry code (Edge) drops back-facing and off-screen triangles, and ones\n"
                            "that cover no pixel of the PS3's 720p picture. Above 720p that last test cuts thin and\n"
                            "small parts away: holes in faces.\n"
                            "At the render resolution: Edge gets the picture's real size, so it keeps every triangle\n"
                            "that covers a pixel (the idea from ThiagoFraga761/infamous2-pc).\n"
                            "Off: \"Disable Mesh Trimming\" by Yahfz (RPCS3 patch database), every triangle drawn.");
      if (slot < 0) ImGui::EndDisabled();
      live_mark(V_MESH_TRIM);
      hint(slot < 0 ? "Not in this build: its SPU lift had no tools/spu_patches.txt."
                    : "At the render resolution: no holes, and the culling saves GPU and graphics-thread time "
                      "(the default). Off draws whole meshes. The PS3's 720p culling leaves holes above 720p."); }
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::Combo("Anisotropic filtering", &s_ui.aniso, kAnisoName, 5)) apply_setting(V_ANISO, kAniso[s_ui.aniso]);
    live_mark(V_ANISO);
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::Combo("Upscaling (window larger than the game image)", &s_ui.upscale, kUpName, 3)) apply_setting(V_UPSCALE, kUp[s_ui.upscale]);
    live_mark(V_UPSCALE);
    hint("\"live\" settings change at once; \"on restart\" ones are saved now and used the next time the "
         "game starts. A higher internal resolution costs GPU time (see the HUD).");

    section("Game effects");
    if (ImGui::Checkbox("Depth of field", &s_ui.dof)) apply_setting(V_DOF, s_ui.dof ? "1" : "0");
    live_mark(V_DOF);
    hint("The game blurs the scene by distance, strongly in cutscenes. This is the softness you see on "
         "distant things when the camera turns: inFAMOUS 2 has no camera motion blur. Off shows "
         "everything sharp. No frame-rate cost.");
    if (ImGui::Checkbox("Edge blur", &s_ui.edge_blur)) apply_setting(V_EDGE_BLUR, s_ui.edge_blur ? "1" : "0");
    live_mark(V_EDGE_BLUR);
    hint("The game can soften the borders of the picture where its lighting settings ask for it. Off "
         "keeps the borders sharp. No frame-rate cost.");
}

/* ---- Language (on the Audio page) -------------------------------------------
 * PS3_LANG is the console language the game is told (libs/system/cellSysutil.c;
 * unset or "auto" = the Windows display language, English when the game has
 * none of it). PS3_VOICE_LANG / PS3_TEXT_LANG split the dub from what is read
 * on screen (runtime/ppu/ppu_fs_lang.h serves that language's files; "same" =
 * follow the game). The game reads all three once, at start, so the menu only
 * saves them (set_value): the environment is left alone and nothing changes
 * halfway through this session. The lists are what this disc has
 * (cache/psarc: <name>_text.psarc, and <name>.psarc beside it for a dub). */
#ifdef _WIN32
static const char* const kHostOs = "Windows";
#else
static const char* const kHostOs = "system";
#endif
/* The game's languages: the files' spelling (ppu_fs_lang.h), the PS3_LANG code
 * the menu writes for one (cellSysutil.c), the label, and a short name for
 * sentences ("no Italian dub"). Endonyms only in scripts the menu's font has
 * (Segoe UI: Latin and Cyrillic). */
static const struct { const char* name; const char* code; const char* label; const char* brief; } kLang[] = {
    { "english",    "en",     "English (US)",                              "English" },
    { "british",    "en-gb",  "English (UK)",                              "UK English" },
    { "french",     "fr",     "French (Fran\u00e7ais)",                    "French" },
    { "german",     "de",     "German (Deutsch)",                          "German" },
    { "italian",    "it",     "Italian (Italiano)",                        "Italian" },
    { "spanish",    "es-es",  "Spanish, Spain (Espa\u00f1ol)",             "Spanish (Spain)" },
    { "latino",     "es-419", "Spanish, Latin America (Espa\u00f1ol)",     "Latin-American Spanish" },
    { "portuguese", "pt-pt",  "Portuguese, Portugal (Portugu\u00eas)",     "Portuguese (Portugal)" },
    { "brazilian",  "pt-br",  "Portuguese, Brazil (Portugu\u00eas)",       "Brazilian Portuguese" },
    { "dutch",      "nl",     "Dutch (Nederlands)",                        "Dutch" },
    { "polish",     "pl",     "Polish (Polski)",                           "Polish" },
    { "russian",    "ru",     "Russian (\u0420\u0443\u0441\u0441\u043a\u0438\u0439)", "Russian" },
    { "turkish",    "tr",     "Turkish (T\u00fcrk\u00e7e)",                "Turkish" },
    { "finnish",    "fi",     "Finnish (Suomi)",                           "Finnish" },
    { "swedish",    "sv",     "Swedish (Svenska)",                         "Swedish" },
    { "danish",     "da",     "Danish (Dansk)",                            "Danish" },
    { "norwegian",  "no",     "Norwegian (Norsk)",                         "Norwegian" },
    { "japanese",   "ja",     "Japanese",                                  "Japanese" },
    { "korean",     "ko",     "Korean",                                    "Korean" },
    { "chinese",    "zh-t",   "Chinese, traditional",                      "Chinese (traditional)" },
    { "chinese_s",  "zh-s",   "Chinese, simplified",                       "Chinese (simplified)" },
};

struct LangUi {
    char voice[32][24], text[32][24];   /* this disc's dubs and text languages */
    int  nvoice, ntext;
    char host[8];                       /* the PS3_LANG code "Automatic" gives ("" = none: English) */
    char win[64];                       /* the display language's name */
};
static LangUi s_lang;

static const char* lang_label(const char* name)
{
    for (const auto& k : kLang) if (!SDL_strcasecmp(k.name, name)) return k.label;
    return name;
}
static const char* lang_brief(const char* name)
{
    for (const auto& k : kLang) if (!SDL_strcasecmp(k.name, name)) return k.brief;
    return name;
}

/* A CELL_SYSUTIL_LANG_* number (es and pt: the disc's own Spanish / Portuguese). */
static const char* lang_id_name(int id)
{
    static const char* const ids[20] = { "japanese", "english", "french", NULL, "german", "italian", "dutch", NULL,
                                         "russian", "korean", "chinese", "chinese_s", "finnish", "swedish", "danish",
                                         "norwegian", "polish", "brazilian", "british", "turkish" };
    return id >= 0 && id < 20 ? ids[id] : NULL;
}
static const char* lang_id_label(int id)
{
    if (id == 3) return "Spanish";
    if (id == 7) return "Portuguese";
    const char* n = lang_id_name(id);
    return n ? lang_label(n) : "?";
}

/* The one file-name language a PS3_LANG code names in the table, else NULL. */
static const char* lang_code_name(const char* code)
{
    for (const auto& k : kLang) if (!SDL_strcasecmp(code, k.code)) return k.name;
    return NULL;
}

/* A PS3_LANG value (a code from the list, another code cellSysutil takes, or a number). */
static const char* lang_code_label(const char* code)
{
    for (const auto& k : kLang) if (!SDL_strcasecmp(code, k.code)) return k.label;
    if (!SDL_strcasecmp(code, "en-us")) return lang_label("english");
    if (!SDL_strcasecmp(code, "es")) return lang_id_label(3);
    if (!SDL_strcasecmp(code, "pt")) return lang_id_label(7);
    char* end = NULL;
    const long n = strtol(code, &end, 10);
    if (end && end != code && !*end) return lang_id_label((int)n);
    return code;
}

/* "" and the words that mean "not chosen" ("auto" for PS3_LANG, "same" for the other two). */
static bool lang_unset(int v, const char* s)
{
    if (!*s) return true;
    if (v == V_LANG) return !SDL_strcasecmp(s, "auto") || !SDL_strcasecmp(s, "system");
    return !SDL_strcasecmp(s, "same") || !SDL_strcasecmp(s, "auto") || !SDL_strcasecmp(s, "default");
}
static bool lang_same(int v, const char* a, const char* b)
{
    const bool ua = lang_unset(v, a), ub = lang_unset(v, b);
    return (ua || ub) ? ua == ub : !SDL_strcasecmp(a, b);
}

/* The menu's choice: this session's, else the one in effect at start. */
static void lang_choice(int v, char* out, size_t n)
{
    std::lock_guard<std::mutex> g(s_set_lock);
    snprintf(out, n, "%s", s_value[v][0] ? s_value[v] : s_start[v]);
}
static bool lang_changed(int v)
{
    char cur[48];
    lang_choice(v, cur, sizeof cur);
    return !lang_same(v, cur, s_start[v]);
}
static void lang_set(int v, const char* value)
{
    char cur[48];
    lang_choice(v, cur, sizeof cur);
    if (!lang_same(v, cur, value)) set_value(v, "%s", value);
}

static bool lang_has(char (*list)[24], int n, const char* name)
{
    for (int i = 0; i < n; i++) if (!SDL_strcasecmp(list[i], name)) return true;
    return false;
}

/* When the menu opens (ui_load). The disc's lists are read until found (a disc
 * not laid out as <root>/PS3_GAME/USRDIR is found once the game opened its
 * voice archive) and then kept: a listing on a slow drive is not repeated. */
static void lang_ui_load(void)
{
    if (!s_lang.ntext) {
        s_lang.nvoice = ps3_lang_available(0, s_lang.voice, 32);
        s_lang.ntext = ps3_lang_available(1, s_lang.text, 32);
    }
    if (s_lang.win[0]) return;
    ps3_sysutil_host_language(s_lang.host, sizeof s_lang.host);
#ifdef _WIN32
    if (!GetLocaleInfoA(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), LOCALE_SENGLISHLANGUAGENAME,
                        s_lang.win, (int)sizeof s_lang.win))
        s_lang.win[0] = 0;
#endif
    if (!s_lang.win[0])
        snprintf(s_lang.win, sizeof s_lang.win, "%s", s_lang.host[0] ? lang_code_label(s_lang.host) : "unknown");
}

/* The dub this disc plays for a language it has no dub of (ppu_fs_lang.h's
 * fallback: english, else its first), NULL when it has that one or no list. */
static const char* lang_dub_fallback(const char* name)
{
    if (!name || !s_lang.nvoice || lang_has(s_lang.voice, s_lang.nvoice, name)) return NULL;
    return lang_has(s_lang.voice, s_lang.nvoice, "english") ? "english" : s_lang.voice[0];
}

/* "Same as the game" or one of this disc's languages (PS3_VOICE_LANG / PS3_TEXT_LANG). */
static void lang_pick(int v, const char* label, char (*list)[24], int n)
{
    char cur[48];
    lang_choice(v, cur, sizeof cur);
    const bool same = lang_unset(v, cur);
    char preview[128];
    if (same) snprintf(preview, sizeof preview, "Same as the game");
    else snprintf(preview, sizeof preview, "%s%s", lang_label(cur), n && !lang_has(list, n, cur) ? " (not on this disc)" : "");
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable("Same as the game", same)) lang_set(v, "same");
        for (const auto& k : kLang)   /* the table's order, then any name it does not know */
            if (lang_has(list, n, k.name) && ImGui::Selectable(k.label, !same && !SDL_strcasecmp(cur, k.name)))
                lang_set(v, k.name);
        for (int i = 0; i < n; i++) {
            bool known = false;
            for (const auto& k : kLang) known = known || !SDL_strcasecmp(k.name, list[i]);
            if (!known && ImGui::Selectable(list[i], !same && !SDL_strcasecmp(cur, list[i]))) lang_set(v, list[i]);
        }
        ImGui::EndCombo();
    }
    restart_mark(v, lang_changed(v));
    env_mark(v, false);
}

static void language_section(void)
{
    section("Language");
    char b[320];
    {   /* PS3_LANG */
        char cur[48];
        lang_choice(V_LANG, cur, sizeof cur);
        const bool autom = lang_unset(V_LANG, cur);
        char autolabel[128];
        snprintf(autolabel, sizeof autolabel, "Automatic (%s: %s)", kHostOs, s_lang.win);
        ImGui::SetNextItemWidth(combo_w());
        if (ImGui::BeginCombo("Game language", autom ? autolabel : lang_code_label(cur))) {
            if (ImGui::Selectable(autolabel, autom)) lang_set(V_LANG, "auto");
            for (const auto& k : kLang) {
                if (s_lang.ntext && !lang_has(s_lang.text, s_lang.ntext, k.name)) continue;   /* not on this disc */
                if (ImGui::Selectable(k.label, !autom && !SDL_strcasecmp(cur, k.code))) lang_set(V_LANG, k.code);
            }
            ImGui::EndCombo();
        }
        restart_mark(V_LANG, lang_changed(V_LANG));
        env_mark(V_LANG, false);
        if (autom && s_lang.host[0]) {
            const char* hn = lang_code_name(s_lang.host);
            const bool missing = hn && s_lang.ntext && !lang_has(s_lang.text, s_lang.ntext, hn);
            snprintf(b, sizeof b, "The console language the game is told: its menus, subtitles and dub follow it. "
                     "Automatic follows the %s display language: %s%s.", kHostOs, lang_code_label(s_lang.host),
                     missing ? " (this disc has no text in it)" : "");
        } else if (autom)
            snprintf(b, sizeof b, "The console language the game is told: its menus, subtitles and dub follow it. "
                     "Automatic follows the %s display language; the game has no %s, so it is told English (US).",
                     kHostOs, s_lang.win);
        else
            snprintf(b, sizeof b, "The console language the game is told: its menus, subtitles and dub follow it, "
                     "unless Voice or Text below says otherwise.");
        {   /* Voice on "Same as the game" and no dub of that language here: the dialogue's fallback */
            char vcur[48];
            lang_choice(V_VOICE_LANG, vcur, sizeof vcur);
            const char* gname = autom ? (s_lang.host[0] ? lang_code_name(s_lang.host) : "english") : lang_code_name(cur);
            const char* fb = lang_unset(V_VOICE_LANG, vcur) ? lang_dub_fallback(gname) : NULL;
            if (fb) {
                const size_t l = strlen(b);
                snprintf(b + l, sizeof b - l, " This disc has no %s dub: the voices stay %s.", lang_brief(gname), lang_brief(fb));
            }
        }
        hint(b);
    }
    lang_pick(V_VOICE_LANG, "Voice", s_lang.voice, s_lang.nvoice);
    if (s_lang.nvoice == 1)
        snprintf(b, sizeof b, "The dub: dialogue and the movies' voice track. This disc has one dub: %s.", lang_label(s_lang.voice[0]));
    else
        snprintf(b, sizeof b, "The dub: dialogue and the movies' voice track. Only the dubs on this disc are listed.");
    hint(b);
    lang_pick(V_TEXT_LANG, "Text", s_lang.text, s_lang.ntext);
    hint("Menus, HUD and subtitles: they change together (the font with the letters comes with the menus).");
    if (!s_lang.ntext) hint("No language files found on this disc (PS3_GAME\\USRDIR\\cache\\psarc).");

    if (lang_changed(V_LANG) || lang_changed(V_VOICE_LANG) || lang_changed(V_TEXT_LANG)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted("Applies the next time the game starts: the game loads its language once, when it starts.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    {   /* what this session runs with */
        const int told = ps3_sysutil_language_in_use();
        char va[24], vg[24], ta[24], tg[24];
        ps3_lang_in_use(0, va, vg, sizeof va);
        ps3_lang_in_use(1, ta, tg, sizeof ta);
        char miss[24];
        ps3_lang_missing(0, miss, sizeof miss);
        char vs[96] = "", ts[96] = "";   /* a dub the disc lacks, or a file the game asked for that was swapped */
        if (miss[0]) snprintf(vs, sizeof vs, " (no %s dub on this disc)", lang_brief(miss));
        else if (vg[0] && strcmp(va, vg)) snprintf(vs, sizeof vs, " instead of %s", lang_brief(va));
        if (tg[0] && strcmp(ta, tg)) snprintf(ts, sizeof ts, " instead of %s", lang_brief(ta));
        const char* tn = lang_id_name(told);
        snprintf(b, sizeof b, "Now: the game was told %s; voice: %s%s; text: %s%s.",
                 told == 3 ? "Spanish" : told == 7 ? "Portuguese" : tn ? lang_brief(tn) : "-",
                 vg[0] ? lang_brief(vg) : "-", vs, tg[0] ? lang_brief(tg) : "-", ts);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(b);
        ImGui::PopTextWrapPos();
    }
}

static void tab_audio(void)
{
    section("Sound");
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::SliderInt("Master volume", &s_ui.volume, 0, 100, "%d%%")) {
        cellAudio_host_set_volume(s_ui.volume);
        set_value(V_VOLUME, "%d", s_ui.volume);
    }
    env_mark(V_VOLUME);
    hint("What the PC plays; the game's own mix and its volume options are untouched.");
    language_section();
}

/* PS3 controls in the table's order, with their labels. */
static const struct { int ctl; const char* name; } kRows[] = {
    { PIM_CROSS, "Cross" }, { PIM_CIRCLE, "Circle" }, { PIM_SQUARE, "Square" }, { PIM_TRIANGLE, "Triangle" },
    { PIM_L1, "L1" }, { PIM_R1, "R1" }, { PIM_L2, "L2" }, { PIM_R2, "R2" },
    { PIM_L3, "L3" }, { PIM_R3, "R3" }, { PIM_START, "START" }, { PIM_SELECT, "SELECT" },
    { PIM_UP, "D-pad up" }, { PIM_DOWN, "D-pad down" }, { PIM_LEFT, "D-pad left" }, { PIM_RIGHT, "D-pad right" },
    { PIM_LS_UP, "Left stick up" }, { PIM_LS_DOWN, "Left stick down" },
    { PIM_LS_LEFT, "Left stick left" }, { PIM_LS_RIGHT, "Left stick right" },
    { PIM_RS_UP, "Right stick up" }, { PIM_RS_DOWN, "Right stick down" },
    { PIM_RS_LEFT, "Right stick left" }, { PIM_RS_RIGHT, "Right stick right" },
};
/* SDL controller input -> what it is on a PlayStation pad. */
static const char* pad_input_label(int bit)
{
    switch (bit) {
    case 0: return "Cross";   case 1: return "Circle"; case 2: return "Square"; case 3: return "Triangle";
    case 4: return "Share";   case 5: return "PS";     case 6: return "Options";
    case 7: return "L3";      case 8: return "R3";     case 9: return "L1";     case 10: return "R1";
    case 11: return "D-pad up"; case 12: return "D-pad down"; case 13: return "D-pad left"; case 14: return "D-pad right";
    case 15: return "Mic";
    case 16: return "Paddle 1"; case 17: return "Paddle 2"; case 18: return "Paddle 3"; case 19: return "Paddle 4";
    case 20: return "Touchpad";
    case PIM_PAD_TRIGGER_LEFT: return "L2"; case PIM_PAD_TRIGGER_RIGHT: return "R2";
    default: return "?";
    }
}
static const char* mouse_input_label(uint32_t bit)
{
    switch (bit) {
    case 1u: return "Left"; case 2u: return "Right"; case 4u: return "Middle";
    case PIM_MOUSE_WHEEL_UP: return "Wheel up"; case PIM_MOUSE_WHEEL_DOWN: return "Wheel down";
    default: break;
    }
    static char b[8][16];
    for (int i = 3; i < 8; i++) if (bit == 1u << i) { snprintf(b[i], sizeof b[i], "Button %d", i + 1); return b[i]; }
    return "?";
}
static void cell_text(const PadInputMap& m, int dev, int ctl, char* out, size_t n)
{
    size_t len = 0; out[0] = 0;
    auto add = [&](const char* name) {
        int w = snprintf(out + len, n - len, "%s%s", len ? ", " : "", name);
        if (w > 0) len += (size_t)w;
        if (len >= n) len = n - 1;
    };
    char tmp[16];
    if (dev == 0) {
        for (int b = 0; b < 32; b++) if (m.pad[ctl] >> b & 1u) add(pad_input_label(b));
    } else if (dev == 1) {
        for (unsigned vk = 0; vk < 128; vk++)
            if (m.key[ctl][vk >> 5] >> (vk & 31) & 1u) add(pim_key_label(vk, tmp, sizeof tmp));
    } else {
        for (int b = 0; b < 32; b++) if (m.mouse[ctl] >> b & 1u) add(mouse_input_label(1u << b));
    }
    if (!len) snprintf(out, n, "-");
}

static char   s_pads[4][128];
static int    s_n_pads;
static double s_pads_t;
static void pads_refresh(double now)
{
    if (now - s_pads_t < 1.0 && s_pads_t > 0) return;
    s_pads_t = now;
    s_n_pads = 0;
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) return;
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n && s_n_pads < 4; i++) {
        if (!SDL_IsGameController(i)) continue;
        const char* name = SDL_GameControllerNameForIndex(i);
        snprintf(s_pads[s_n_pads++], sizeof s_pads[0], "%s", name ? name : "Game controller");
    }
}

static void tab_controls(double now)
{
    section("Controllers");
    pads_refresh(now);
    if (!s_n_pads) {
        const char* e = getenv("PS3_PAD_SDL");
        hint(e && e[0] == '0' ? "Controller input is off for this run (PS3_PAD_SDL=0)."
                              : "No controller connected. Switch one on: it is picked up while the game runs.");
    }
    for (int i = 0; i < s_n_pads; i++) ImGui::BulletText("%s%s", s_pads[i], i == 0 ? "  (port 1)" : "");
    { const int eff = ps3_button_icons_effective();
      ImGui::Text("Button prompts show: %s", eff == 2 ? "Xbox" : eff == 3 ? "keyboard and mouse" : "PlayStation"); }

    section("Play with");
    { static const char* const modes[] = { "Controller, keyboard and mouse", "Controller only", "Keyboard and mouse only" };
      static const char* const names[] = { "both", "controller", "keyboard" };
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::Combo("Input", &s_ui.input_mode, modes, 3)) {
          cellPad_host_set_input_mode(s_ui.input_mode);
          set_value(V_INPUT_MODE, "%s", names[s_ui.input_mode]);
      } }
    env_mark(V_INPUT_MODE);
    { static const char* const icons[] = { "Automatic (the device in use)", "PlayStation", "Xbox", "Keyboard and mouse" };
      static const char* const names[] = { "auto", "playstation", "xbox", "keyboard" };
      ImGui::SetNextItemWidth(combo_w());
      if (ImGui::Combo("Button icons", &s_ui.button_icons, icons, 4)) {
          ps3_button_icons_set_mode(s_ui.button_icons);
          set_value(V_BUTTON_ICONS, "%s", names[s_ui.button_icons]);
      } }
    env_mark(V_BUTTON_ICONS);
    if (ImGui::Checkbox("Vibration", &s_ui.rumble)) {
        cellPad_host_set_rumble_strength(s_ui.rumble ? s_ui.rumble_strength : 0);
        set_value(V_RUMBLE, "%d", s_ui.rumble ? 1 : 0);
        if (s_ui.rumble) set_value(V_RUMBLE_STRENGTH, "%d", s_ui.rumble_strength);
    }
    env_mark(V_RUMBLE);
    ImGui::BeginDisabled(!s_ui.rumble);
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::SliderInt("Vibration strength", &s_ui.rumble_strength, 10, 200, "%d%%")) {
        cellPad_host_set_rumble_strength(s_ui.rumble_strength);
        set_value(V_RUMBLE_STRENGTH, "%d", s_ui.rumble_strength);
    }
    ImGui::EndDisabled();
    env_mark(V_RUMBLE_STRENGTH);

    section("PS3 button layout");
    PadInputMap m;
    cellPad_host_input_map(&m);
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("layout", 4, tf)) {
        ImGui::TableSetupColumn("PS3", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Mouse", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableHeadersRow();
        char text[160];
        for (const auto& r : kRows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.name);
            for (int dev = 0; dev < 3; dev++) {
                ImGui::TableSetColumnIndex(dev + 1);
                cell_text(m, dev, r.ctl, text, sizeof text);
                ImGui::TextUnformatted(text);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) &&
                    ImGui::CalcTextSize(text).x > ImGui::GetContentRegionAvail().x)
                    ImGui::SetTooltip("%s", text);   /* the whole list when the cell cuts it */
            }
        }
        ImGui::EndTable();
    }
    hint("Buttons are by position, as on a PlayStation pad (bottom = Cross). The sticks always move the PS3 "
         "sticks. The layout comes from PS3_INPUT_MAP (an input.toml); rebinding from this menu comes with the "
         "new input layer.");

    section("This menu");
    hint("F2 opens and closes it; on a controller hold L3+R3 together for a second. Esc or Circle closes it, "
         "L1/R1 switch pages. While it is open the game gets no input (it keeps running: pause first with START). "
         "F1 shows or hides the performance HUD, Alt+Enter or F11 switches fullscreen.");
}

static void stats_lines(bool compact)
{
    const Stats& st = s_stats;
    if (!st.valid) { ImGui::TextUnformatted("measuring..."); return; }
    const double target = fps_target();
    const ImVec4 col = st.fps >= target * 0.95 ? ImVec4(0.45f, 0.90f, 0.45f, 1.0f)
                     : st.fps >= target * 0.75 ? ImVec4(0.95f, 0.85f, 0.30f, 1.0f)
                                               : ImVec4(1.0f, 0.40f, 0.35f, 1.0f);
    ImGui::PushFont(NULL, compact ? 22.0f : 24.0f);
    ImGui::TextColored(col, "%.0f", st.fps);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("FPS");
    ImGui::Text("%.1f ms", st.avg_ms);
    ImGui::EndGroup();
    if (s_hud_graph || !compact) {
        float buf[FT_N]; const int n = s_ft_count < FT_N ? s_ft_count : FT_N;
        for (int i = 0; i < n; i++) buf[i] = s_ft[(s_ft_i - n + i + FT_N) % FT_N];
        const float top = st.max_ms > 50.0 ? (float)st.max_ms : 50.0f;
        const ImVec2 size(compact ? S(210.0f) : S(420.0f), compact ? S(38.0f) : S(70.0f));
        ImGui::PlotLines("##ft", buf, n, 0, NULL, 0.0f, top, size);
        /* the target frame time as a faint line */
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float y = b.y - (float)(1000.0 / target) / top * (b.y - a.y);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, 60), 1.0f);
    }
    if (st.low1_fps > 0) ImGui::Text("1%% low %.0f   max %.1f ms", st.low1_fps, st.max_ms);
    else ImGui::Text("max %.1f ms", st.max_ms);
    if (s_hud_cpu || !compact) {
        CpuSample cs;
        { std::lock_guard<std::mutex> g(s_cpu_lock); cs = s_cpu; }
        if (cs.valid) {
#ifdef _WIN32
            ImGui::Text("CPU %.0f%%  RSX %.0f%%  top %.0f%%", cs.proc_pct, cs.rsx_pct, cs.top_pct);
            if (!compact) ImGui::Text("%.1f of %d logical cores in use, %d thread(s) at 80%% or more", cs.cores, cs.ncpu, cs.busy);
#else
            ImGui::Text("CPU %.1f cores", cs.cores);
#endif
        }
    }
}

static void tab_hud(void)
{
    section("Performance HUD");
    { bool h = s_hud != 0;
      if (ImGui::Checkbox("Show it while playing (F1)", &h)) set_hud(h); }
    env_mark(V_HUD);
    static const char* const corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::Combo("Corner", &s_hud_corner, corners, 4)) set_value(V_HUD_CORNER, "%d", s_hud_corner);
    ImGui::SetNextItemWidth(combo_w());
    if (ImGui::SliderFloat("Background opacity", &s_hud_opacity, 0.0f, 1.0f, "%.2f")) set_value(V_HUD_OPACITY, "%.2f", s_hud_opacity);
    if (ImGui::Checkbox("Frame time graph", &s_hud_graph)) set_value(V_HUD_GRAPH, "%d", s_hud_graph ? 1 : 0);
    ImGui::SameLine(0, S(30));
    if (ImGui::Checkbox("CPU use", &s_hud_cpu)) { set_value(V_HUD_CPU, "%d", s_hud_cpu ? 1 : 0); }
    section("Now");
    stats_lines(false);
    if (s_gpu_name[0]) ImGui::Text("GPU: %s (Vulkan)", s_gpu_name);
    hint("FPS and frame time count presented frames; \"1% low\" is the frame rate of the slowest 1% of the last "
         "few seconds. CPU: the whole process, as a share of all logical cores; RSX: the thread that turns the "
         "game's draws into Vulkan and presents; top: the busiest other thread (the game's main thread or an SPU). "
         "The faint line in the graph is the frame-rate cap's frame time.");
}

/* ---- Effects page (fx): effects resolution (texture packs: the Mods page) --
 * A page of its own. Its settings are not in the V_ table: fx_set applies
 * one live (ps3_setting_apply: rsx_draw_engine.c registers the appliers) and
 * keeps it as one of the file's other lines, which preload() exports at the
 * next start like any other. */
extern "C" const char* rsx_texpack_dir(void);
extern "C" void rsx_texpack_status(unsigned* packs, unsigned* files, unsigned* applied, int* dumping);

static void fx_set(const char* name, const char* value)
{
    ps3_setting_apply(name, value);
    std::lock_guard<std::mutex> g(s_set_lock);
    int i = 0;
    while (i < s_n_extra && strcmp(s_extra[i][0], name)) i++;
    if (i == s_n_extra) {
        if (s_n_extra >= EXTRA_N) return;
        snprintf(s_extra[i][0], sizeof s_extra[i][0], "%s", name);
        s_n_extra++;
    }
    snprintf(s_extra[i][1], sizeof s_extra[i][1], "%s", value ? value : "");
    s_dirty = true;
    s_dirty_t = now_s();
}

static void fx_live(const char* name)
{
    ImGui::SameLine();
    if (ps3_setting_applier(name)) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "live");
    else ImGui::TextColored(kGrey, "on restart");
}

/* The pack folders inside the packs folder ("_dump" and other "_" names are not packs). */
static int fx_pack_list(char names[][64], int max)
{
    const char* dir = rsx_texpack_dir();
    int n = 0;
    if (!dir || !*dir) return 0;
#ifdef _WIN32
    char pat[700];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' || fd.cFileName[0] == '_') continue;
        if (n < max) snprintf(names[n++], 64, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    return n;
}

static void tab_effects(void)
{
    section("Fire, smoke and particles");
    {   /* PS3RECOMP_FX_RES: 1 (the game's), 2, full (2.5 at 720p) */
        static const char* const kVal[] = { "1", "2", "full" };
        static const char* const kName[] = { "As the game (512 x 288)", "High (1024 x 576)", "Full (1280 x 720)" };
        const char* e = getenv("PS3RECOMP_FX_RES");
        int cur = 0;
        if (e && (!strcmp(e, "full") || atof(e) >= 2.5)) cur = 2;
        else if (e && atof(e) > 1.0) cur = 1;
        ImGui::SetNextItemWidth(combo_w());
        if (ImGui::Combo("Effects resolution", &cur, kName, 3)) fx_set("PS3RECOMP_FX_RES", kVal[cur]);
        fx_live("PS3RECOMP_FX_RES");
        hint("The game draws fire, smoke, sparks, debris and the Beast into a small picture (512 x 288) and "
             "stretches it over the frame: that is the blocky edge round flames and smoke. High and Full draw "
             "them at a higher resolution, same art, sharper. Costs some GPU time where there is a lot of smoke "
             "(see the HUD).");
    }

}

/* ---- Mods page [mods]: game mods and texture packs ------------------------
 * Game mods are the community's inFAMOUS Mod Manager mods (mod.json, files/,
 * texture_patches/, xpps_patches/, modular.json), served by the file layer
 * (runtime/ppu/ppu_fs_mods.h) from the mods folder (PS3_GAME_MODS_DIR, the
 * launcher's "mods" folder). Which mods are on and their options are saved
 * at once in mods.ini (beside settings.ini) and apply at the next start;
 * "Use mods" is PS3_GAME_MODS in settings.ini. */
static void mods_status_line(const ps3_game_mod& m)
{
    const bool bad = !strncmp(m.status, "Not used", 8) || !strncmp(m.status, "Made for", 8);
    const ImVec4 col = (m.active && m.on) ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : bad ? kOrange : kGrey;
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(m.status);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static void mods_options(int i, int n)
{
    ps3_game_mod_option o[64];
    if (n > 64) n = 64;
    int got = 0;
    for (int k = 0; k < n; k++) if (ps3_game_mods_option(i, k, &o[got])) got++;
    ImGui::Indent(S(28.0f));
    /* toggles first, as the Manager lists them (by name) */
    for (int k = 0; k < got; k++) {
        if (o[k].dropdown) continue;
        ImGui::PushID(k);
        char label[200];
        if (o[k].group[0]) snprintf(label, sizeof label, "%s: %s", o[k].group, o[k].name);
        else snprintf(label, sizeof label, "%s", o[k].name);
        bool on = o[k].picked != 0;
        if (ImGui::Checkbox(label, &on)) ps3_game_mods_pick(i, k, on ? 1 : 0);
        ImGui::PopID();
    }
    /* each dropdown group once: a combo of its options */
    for (int k = 0; k < got; k++) {
        if (!o[k].dropdown) continue;
        bool first = true;
        for (int j = 0; j < k; j++) if (o[j].dropdown && !strcmp(o[j].group_id, o[k].group_id)) first = false;
        if (!first) continue;
        int cur = k;
        for (int j = k; j < got; j++) if (o[j].dropdown && !strcmp(o[j].group_id, o[k].group_id) && o[j].picked) cur = j;
        ImGui::PushID(1000 + k);
        ImGui::SetNextItemWidth(combo_w());
        if (ImGui::BeginCombo(o[k].group[0] ? o[k].group : "Option", o[cur].name)) {
            for (int j = k; j < got; j++) {
                if (!o[j].dropdown || strcmp(o[j].group_id, o[k].group_id)) continue;
                if (ImGui::Selectable(o[j].name, j == cur)) ps3_game_mods_pick(i, j, 1);
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    ImGui::Unindent(S(28.0f));
}

static void tab_mods(void)
{
    section("Game mods");
    const char* dir = ps3_game_mods_dir();
    if (!dir || !*dir) {
        hint("No mods folder: start the game with \"Play inFAMOUS 2\" (it uses the \"mods\" folder beside it), "
             "or set PS3_GAME_MODS_DIR.");
    } else {
        const char* e = getenv(kVar[V_GAME_MODS]);
        bool use = !(e && e[0] == '0' && !e[1]);
        if (ImGui::Checkbox("Use mods", &use)) apply_setting(V_GAME_MODS, use ? "1" : "0");
        { const char* now = getenv(kVar[V_GAME_MODS]); restart_mark(V_GAME_MODS, strcmp(now ? now : "", s_start[V_GAME_MODS]) != 0); }
        env_mark(V_GAME_MODS, false);
#ifdef _WIN32
        if (ImGui::Button("Open the mods folder")) ps3_game_mods_open_folder();
        ImGui::SameLine();
#endif
        if (ImGui::Button("Look for new mods")) ps3_game_mods_rescan();
        { char b[720]; snprintf(b, sizeof b, "Mods folder: %s", dir); hint(b); }
        const int n = ps3_game_mods_count();
        if (!n)
            hint("No mods yet. Unzip a mod into the mods folder (each mod is a folder with a mod.json in it), "
                 "then \"Look for new mods\".");
        for (int i = 0; i < n; i++) {
            ps3_game_mod m;
            if (!ps3_game_mods_get(i, &m)) continue;
            ImGui::PushID(i);
            ImGui::Separator();
            bool on = m.on != 0;
            if (!m.for_game) ImGui::BeginDisabled();
            if (ImGui::Checkbox("##on", &on)) ps3_game_mods_set_on(i, on ? 1 : 0);
            if (!m.for_game) ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::PushFont(vk_ov_theme_bold(), 0.0f);
            ImGui::TextUnformatted(m.name);
            ImGui::PopFont();
            ImGui::SameLine();
            const bool ver = m.version[0] && strcmp(m.version, "unknown");
            const bool who = m.author[0] && strcmp(m.author, "unknown");
            ImGui::TextColored(kGrey, "%s%s%s%s", ver ? "v" : "", ver ? m.version : "", who ? (ver ? "  by " : "by ") : "", who ? m.author : "");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s\n%d whole file(s), %d texture patch(es), %d xpps patch(es)", m.folder, m.n_files, m.n_tex, m.n_xpps);
            mods_status_line(m);
            if (m.n_modules && m.for_game) mods_options(i, m.n_modules);
            ImGui::PopID();
        }
        if (n) ImGui::Separator();
        hint("Mods made for the inFAMOUS Mod Manager work as they are, with no Unlocker: the game's own files are "
             "never changed, the port hands the game the mod's files in their place. Turning a mod on or off, "
             "or changing its options, applies the next time the game starts. When two mods change the same "
             "file, the first one (by folder name) is used.");
    }

    section("Texture packs");
    {
        static char packs[16][64];
        static int npacks = -1;
        static double listed;
        const double now = now_s();
        if (npacks < 0 || now - listed > 2.0) { npacks = fx_pack_list(packs, 16); listed = now; }
        const char* e = getenv("PS3_MODS");
        int cur = 0;   /* 0 = off, k = packs[k - 1] */
        for (int i = 0; i < npacks; i++) if (e && !SDL_strcasecmp(e, packs[i])) cur = i + 1;
        char preview[80];
        snprintf(preview, sizeof preview, "%s", cur ? packs[cur - 1] : (e && *e ? e : "Off"));
        ImGui::SetNextItemWidth(combo_w());
        if (ImGui::BeginCombo("Texture pack", preview)) {
            if (ImGui::Selectable("Off", cur == 0)) fx_set("PS3_MODS", "");
            for (int i = 0; i < npacks; i++)
                if (ImGui::Selectable(packs[i], cur == i + 1)) fx_set("PS3_MODS", packs[i]);
            ImGui::EndCombo();
        }
        fx_live("PS3_MODS");
        if (ImGui::Button("Reload pack files")) fx_set("PS3_MODS", e ? e : "");
        unsigned np = 0, nf = 0, na = 0; int dumping = 0;
        rsx_texpack_status(&np, &nf, &na, &dumping);
        ImGui::SameLine();
        if (np) ImGui::Text("%u replacement file(s) in use, %u upload(s) replaced", nf, na);
        else ImGui::TextColored(kGrey, "no pack in use");
        const char* dir = rsx_texpack_dir();
        if (dir && *dir) { char b[720]; snprintf(b, sizeof b, "Packs folder: %s", dir); hint(b); }
        else hint("No packs folder: make a \"texpacks\" folder (work\\texpacks), or set PS3_MODS_DIR.");
        hint("A pack is a folder of PNG (or DDS) files named by the texture they replace (the dump's names). "
             "Larger pictures are fine: a higher-resolution pack. The game's own textures come back when "
             "the pack is Off. \"Reload pack files\" picks up files you changed or added.");

        bool d = dumping != 0;
        if (ImGui::Checkbox("Dump textures (to make a pack)", &d)) ps3_setting_apply("PS3_MODS_DUMP", d ? "1" : "0");
        fx_live("PS3_MODS_DUMP");
        hint("Writes each texture the game shows, once, as <name>.png into the packs folder's _dump; "
             "fx_textures.txt there lists the ones fire, smoke and sparks use. Slows the game while on; "
             "not saved (off at the next start).");
    }
}

static void quit_confirm(void)
{
    if (s_quit_ask == 1) { ImGui::OpenPopup("###quitask"); s_quit_ask = 2; }
    if (s_quit_ask != 2) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Quit the game?###quitask", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                               ImGuiWindowFlags_NoSavedSettings)) {
        s_quit_ask = 0; s_quit_cancel = false; return;
    }
    ImGui::TextUnformatted("Progress since the last autosave will be lost.");
    ImGui::Spacing();
    const bool quit = ImGui::Button("Quit");
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel");
    ImGui::SetItemDefaultFocus();   /* Cancel: a stray Cross does not quit */
    if (cancel || s_quit_cancel) {
        s_quit_ask = 0; s_quit_cancel = false;
        ImGui::CloseCurrentPopup();
    } else if (quit) {
        s_quit_ask = 0;
        ImGui::CloseCurrentPopup();
        set_open(0);                  /* saves the settings */
        vk_overlay_notice("Quitting the game...");
        ps3_host_quit("in-game menu");
    }
    ImGui::EndPopup();
}

#include "vk_overlay_light.inc"   /* [light] tab_lighting, lfx_compare_toggle */

static void draw_menu(double now)
{
    if (vk_overlay_saves_active()) { vk_overlay_saves_draw(S(1.0f)); return; }   /* [saves] */
    const ImGuiIO& io = ImGui::GetIO();
    /* the game dimmed behind the menu */
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
    const ImVec2 size(S(720.0f), S(620.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(size.x < io.DisplaySize.x - 20 ? size.x : io.DisplaySize.x - 20,
                                    size.y < io.DisplaySize.y - 20 ? size.y : io.DisplaySize.y - 20), ImGuiCond_Always);
    if (!vk_ov_begin("inFAMOUS 2  -  Settings###ps3menu", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                                              ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End(); return;
    }
    static const char* const names[TAB_CHEATS + 1] = { "Display", "Graphics", "Audio", "Controls", "HUD", "Lighting", "Effects", "Mods", "Cheats" };
    s_ntabs = tab_count();
    if (ImGui::BeginTabBar("tabs")) {
        for (int i = 0; i < tab_count(); i++) {
            const ImGuiTabItemFlags f = (s_tab_want == i) ? ImGuiTabItemFlags_SetSelected : 0;
            ImGui::PushFont(vk_ov_theme_bold(), 0.0f);   /* [theme] tab labels in the bold face */
            const bool tab_open = ImGui::BeginTabItem(names[i], NULL, f);
            ImGui::PopFont();
            if (tab_open) {
                s_tab = i;
                ImGui::BeginChild("page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f));
                switch (i) { case 0: tab_display(); break; case 1: tab_graphics(); break; case 2: tab_audio(); break;
                             case 3: tab_controls(now); break; case 4: tab_hud(); break;
                             case 5: tab_lighting(); break; case 6: tab_effects(); break;
                             case TAB_MODS: tab_mods(); break;
                             case TAB_CHEATS: vk_overlay_cheats_page(combo_w()); break; default: break; }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        s_tab_want = -1;
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Close")) set_open(0);
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) s_quit_ask = 1;
    ImGui::SameLine();
    hint(s_tab == TAB_CHEATS ? (vk_overlay_cheats_remember() ? "Cheats apply at once and are remembered."
                                                             : "Cheats apply at once and are not saved.")
         : s_ini_path[0] ? "Changes apply at once where they can and are saved."
                         : "Changes apply at once (not saved: PS3_OVERLAY_INI is empty).");
    quit_confirm();
    ImGui::End();
}

static void draw_hud(void)
{
    const ImGuiIO& io = ImGui::GetIO();
    const float pad = S(10.0f);
    const ImVec2 pos((s_hud_corner & 1) ? io.DisplaySize.x - pad : pad, (s_hud_corner & 2) ? io.DisplaySize.y - pad : pad);
    const ImVec2 pivot((s_hud_corner & 1) ? 1.0f : 0.0f, (s_hud_corner & 2) ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(s_hud_opacity);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(9), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(6), S(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##hud", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::PushFont(NULL, 14.0f);
        stats_lines(true);
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}

static void draw_notices(double now)
{
    Notice n[3];
    { std::lock_guard<std::mutex> g(s_notice_lock); memcpy(n, s_notice, sizeof n); }
    const ImGuiIO& io = ImGui::GetIO();
    /* bottom right (bottom left when the HUD is there) */
    const bool left = (s_hud_corner == 3);
    float y = io.DisplaySize.y - S(14.0f);
    for (int i = 0; i < 3; i++) {
        if (n[i].until <= now) continue;
        const float a = (float)((n[i].until - now) < 0.5 ? (n[i].until - now) / 0.5 : 1.0);
        ImGui::SetNextWindowPos(ImVec2(left ? S(14.0f) : io.DisplaySize.x - S(14.0f), y), ImGuiCond_Always,
                                ImVec2(left ? 0.0f : 1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.85f * a);
        char id[16]; snprintf(id, sizeof id, "##note%d", i);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        if (ImGui::Begin(id, NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::TextUnformatted(n[i].text);
            y -= ImGui::GetWindowHeight() + S(6.0f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

/* The controller, as cellPad saw it before holding the game's copy neutral. */
static void feed_pad(ImGuiIO& io)
{
    static uint16_t prev;
    const uint32_t nav = g_pad_overlay_nav, nav2 = g_pad_overlay_nav2;
    /* plus the buttons of every other controller, by position (raw SDL buttons) */
    static const struct { uint8_t sdl; uint16_t pad; } kRaw[] = {
        { 0, PAD_CROSS }, { 1, PAD_CIRCLE }, { 2, PAD_SQUARE }, { 3, PAD_TRIANGLE },
        { 9, PAD_L1 }, { 10, PAD_R1 }, { 11, PAD_UP }, { 12, PAD_DOWN }, { 13, PAD_LEFT }, { 14, PAD_RIGHT },
    };
    uint16_t b = (uint16_t)nav;
    const uint32_t raw = g_pad_overlay_raw;
    for (const auto& r : kRaw) if (raw >> r.sdl & 1u) b |= r.pad;
    b |= (uint16_t)g_pad_overlay_inject;   /* [saves] PAD_SCRIPT / PAD_FILE while a system dialog holds the pad */
    const int lx = (nav >> 16) & 255, ly = (nav >> 24) & 255, ry = (nav2 >> 8) & 255;
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
    const struct { ImGuiKey k; float v; } sticks[] = {
        { ImGuiKey_GamepadLStickLeft, axis(lx, -1) }, { ImGuiKey_GamepadLStickRight, axis(lx, 1) },
        { ImGuiKey_GamepadLStickUp, axis(ly, -1) },   { ImGuiKey_GamepadLStickDown, axis(ly, 1) },
        { ImGuiKey_GamepadRStickUp, axis(ry, -1) },   { ImGuiKey_GamepadRStickDown, axis(ry, 1) },
    };
    for (const auto& s : sticks) io.AddKeyAnalogEvent(s.k, s.v > 0.1f, s.v);
    const uint16_t pressed = b & (uint16_t)~prev;
    prev = b;
    if (vk_overlay_saves_active()) { vk_overlay_saves_pad(b, lx, ly); return; }   /* [saves] */
    if (s_quit_ask) { if (pressed & PAD_CIRCLE) s_quit_cancel = true; return; }
    if (pressed & PAD_L1) s_tab_want = (s_tab + tab_count() - 1) % tab_count();
    if (pressed & PAD_R1) s_tab_want = (s_tab + 1) % tab_count();
    if ((pressed & PAD_CIRCLE) && !ImGui::IsAnyItemActive()) set_open(0);
}

/* ---- the entry points ------------------------------------------------------ */
extern "C" void vk_overlay_attach(const vk_overlay_attach_info* info)
{
    s_enabled = false;
    g_vk_overlay_active = 0;
    if (!info || overlay_off() || !info->get_instance_proc_addr || !info->device || !info->sdl_window) return;
    s_vk = *info;
    if (!load_own_vk()) { fprintf(stderr, "[overlay] Vulkan entry points missing: overlay off\n"); return; }
    s_attached = true;
    s_t_attach = now_s();
    { const char* e = getenv("PS3_OVERLAY_HUD"); s_hud = (e && *e) ? (strcmp(e, "0") != 0) : 1; }
    { const char* e = getenv("PS3_OVERLAY_HUD_CORNER"); if (e && *e) s_hud_corner = atoi(e) & 3; }
    { const char* e = getenv("PS3_OVERLAY_HUD_OPACITY");
      if (e && *e) { float v = (float)atof(e); s_hud_opacity = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; } }
    { const char* e = getenv("PS3_OVERLAY_HUD_GRAPH"); if (e && *e) s_hud_graph = strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3_OVERLAY_HUD_CPU"); if (e && *e) s_hud_cpu = strcmp(e, "0") != 0; }
    { const char* e = getenv("PS3_OVERLAY_PAD"); s_pad_chord = !(e && e[0] == '0'); }
    vk_ov_theme_select(vk_ov_theme_parse(getenv(kVar[V_THEME])));   /* [theme] default Hero */
    ps3_setting_register("PS3_OVERLAY_THEME", theme_apply);
    { const int t = tab_from_name(getenv("PS3RECOMP_OVERLAY_TAB")); if (t >= 0) s_tab_want = t; }
    if (p_vkGetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties pp;
        p_vkGetPhysicalDeviceProperties(s_vk.phys, &pp);
        snprintf(s_gpu_name, sizeof s_gpu_name, "%s", pp.deviceName);
    }
    { SDL_DisplayMode dm;
      const int di = SDL_GetWindowDisplayIndex((SDL_Window*)s_vk.sdl_window);
      if (di >= 0 && SDL_GetCurrentDisplayMode(di, &dm) == 0) s_refresh_hz = dm.refresh_rate; }
#ifdef _WIN32
    s_rsx_tid = GetCurrentThreadId();   /* the thread that presents */
#endif
    script_load();
    s_enabled = true;
    g_pad_overlay_present = 1;
    if (env_on("PS3RECOMP_OVERLAY_MENU_OPEN") || env_on("PS3_OVERLAY_FORCE_OPEN")) set_open(1);
    notice_for("F2: settings    F1: performance HUD", 6.0);
    vk_overlay_cheats_attach();      /* remembered cheats on again, and a notice naming them */
    vk_overlay_saves_attach();       /* [saves] the save-data list dialog for cellSaveData */
    update_active();
    fprintf(stderr, "[overlay] in-game menu: F2 (or hold L3+R3); HUD %s (F1); fullscreen Alt+Enter / F11; settings %s\n",
            s_hud ? "on" : "off", s_ini_path[0] ? s_ini_path : "not saved (PS3_OVERLAY_INI empty)");
}

extern "C" void vk_overlay_swapchain_changed(VkFormat format, int color_ok)
{
    if (s_attached) fb_destroy_all();
    s_sc_format = format;
    s_color_ok = color_ok != 0;
    if (!s_color_ok) fprintf(stderr, "[overlay] swapchain images cannot be colour attachments: overlay off\n");
    update_active();
}

static bool fb_for(uint32_t idx, VkImage image, VkExtent2D ext)
{
    if (idx >= SC_MAX) return false;
    if (s_fb[idx].fb && s_fb[idx].img == image && s_fb[idx].ext.width == ext.width && s_fb[idx].ext.height == ext.height)
        return true;
    if (s_fb[idx].fb || s_fb[idx].view) {          /* changed without notice: make sure it is idle */
        p_vkDeviceWaitIdle(s_vk.device);
        if (s_fb[idx].fb) p_vkDestroyFramebuffer(s_vk.device, s_fb[idx].fb, NULL);
        if (s_fb[idx].view) p_vkDestroyImageView(s_vk.device, s_fb[idx].view, NULL);
        s_fb[idx].fb = VK_NULL_HANDLE; s_fb[idx].view = VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = s_rp_format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (p_vkCreateImageView(s_vk.device, &vi, NULL, &s_fb[idx].view) != VK_SUCCESS) { s_fb[idx].view = VK_NULL_HANDLE; return false; }
    VkFramebufferCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass = s_rp;
    fi.attachmentCount = 1;
    fi.pAttachments = &s_fb[idx].view;
    fi.width = ext.width; fi.height = ext.height; fi.layers = 1;
    if (p_vkCreateFramebuffer(s_vk.device, &fi, NULL, &s_fb[idx].fb) != VK_SUCCESS) { s_fb[idx].fb = VK_NULL_HANDLE; return false; }
    s_fb[idx].img = image;
    s_fb[idx].ext = ext;
    return true;
}

extern "C" int vk_overlay_draw(VkCommandBuffer cmd, uint32_t index, VkImage image, VkExtent2D ext,
                               VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{
    if (!s_enabled || !s_color_ok || s_failed || !cmd || !ext.width || !ext.height) return 0;
    if (s_open && g_vk_overlay_on_worker) return 0;   /* the menu: on the RSX thread only (next frame) */
    std::lock_guard<std::recursive_mutex> il(s_imgui_lock);
    const double now = now_s();
    if (s_last_present > 0) {
        s_ft[s_ft_i] = (float)((now - s_last_present) * 1000.0);
        s_ft_i = (s_ft_i + 1) % FT_N; s_ft_count++;
    }
    const double dt = s_last_present > 0 ? now - s_last_present : 1.0 / 60.0;
    s_last_present = now;
    if (s_hud || s_open) stats_tick(now);
    cpu_want((s_hud && s_hud_cpu) || (s_open && s_tab == 4));
    const bool notices = s_notice_until > now;
    update_active();
    if (!s_open && !s_hud && !notices) { s_last_present = 0; s_stats_t0 = 0; cpu_want(false); return 0; }

    if (!s_imgui_ready && !imgui_setup()) { s_failed = true; update_active(); return 0; }
    if (s_vk_ready && s_rp_format != s_sc_format) {   /* a new swapchain format: rebuild the renderer */
        p_vkDeviceWaitIdle(s_vk.device);
        fb_destroy_all();
        vk_backend_shutdown();
    }
    if (!s_vk_ready && !vk_backend_init()) {
        fprintf(stderr, "[overlay] Vulkan renderer setup failed: overlay off\n");
        s_failed = true; update_active(); return 0;
    }
    if (s_open && !s_sdl_ready && !sdl_setup()) fprintf(stderr, "[overlay] SDL platform backend setup failed\n");
    if (!fb_for(index, image, ext)) return 0;

    static int was_open;
    if (s_open && (!was_open || s_ui_stale)) { ui_load(); s_ui_stale = false; }
    was_open = s_open;

    ImGuiIO& io = ImGui::GetIO();
    if (s_ev_n) {
        static SDL_Event ev[128];
        int n;
        { std::lock_guard<std::mutex> g(s_ev_lock); n = s_ev_n; memcpy(ev, s_ev, sizeof ev[0] * (size_t)n); s_ev_n = 0; }
        if (s_open && s_sdl_ready) for (int i = 0; i < n; i++) ImGui_ImplSDL2_ProcessEvent(&ev[i]);
    }

    ImGui_ImplVulkan_NewFrame();
    if (s_open && s_sdl_ready) {
        ImGui_ImplSDL2_NewFrame();
    } else {
        int ww = 0, wh = 0;
        SDL_GetWindowSize((SDL_Window*)s_vk.sdl_window, &ww, &wh);
        io.DisplaySize = ww > 0 && wh > 0 ? ImVec2((float)ww, (float)wh) : ImVec2((float)ext.width, (float)ext.height);
        io.DeltaTime = (float)(dt > 0.0001 && dt < 1.0 ? dt : 1.0 / 60.0);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) io.DisplaySize = ImVec2((float)ext.width, (float)ext.height);
    /* What is drawn into is the swapchain image, whatever the window says. */
    io.DisplayFramebufferScale = ImVec2((float)ext.width / io.DisplaySize.x, (float)ext.height / io.DisplaySize.y);
    if (s_theme_want >= 0) {   /* [theme] a new look: the whole style again, then scaled below */
        vk_ov_theme_select(s_theme_want);
        s_theme_want = -1;
        ImGuiStyle st;
        vk_ov_theme_style(st);
        s_base_style = st;
        s_scale_applied = -1.0f;
    }
    apply_scale(io.DisplaySize.y);
    if (s_open) { io.BackendFlags |= ImGuiBackendFlags_HasGamepad; feed_pad(io); }

    ImGui::NewFrame();
    if (s_open) draw_menu(now);
    else if (s_hud) draw_hud();
    if (notices) draw_notices(now);
    ImGui::Render();

    /* Save a change made in the menu soon after it, not only when it closes. */
    if (s_dirty && now - s_dirty_t > 1.0) settings_save();

    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = access;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = layout;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    p_vkCmdPipelineBarrier(cmd, stage ? stage : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkRenderPassBeginInfo rb = {};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = s_rp;
    rb.framebuffer = s_fb[index].fb;
    rb.renderArea.extent = ext;
    p_vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    p_vkCmdEndRenderPass(cmd);
    return 1;
}

extern "C" void vk_overlay_shutdown(void)
{
    if (!s_attached) return;
    if (s_dirty) settings_save();
    if (s_vk.device && p_vkDeviceWaitIdle) p_vkDeviceWaitIdle(s_vk.device);
    if (s_imgui_ready) {
        vk_backend_shutdown();
        if (s_sdl_ready) { ImGui_ImplSDL2_Shutdown(); s_sdl_ready = false; }
        ImGui::DestroyContext();
        s_imgui_ready = false;
    }
    fb_destroy_all();
    cpu_want(false);
    s_enabled = false;
    s_attached = false;
    g_vk_overlay_active = 0;
    g_pad_overlay_open = 0;
    vk_overlay_saves_shutdown();     /* [saves] a waiting save/load backs out; its icons freed */
}
