/*
 * ps3_overlay -- the in-game menu and performance HUD of the Metal backend
 * (Dear ImGui, third_party/imgui), drawn on the drawable after the game's
 * image and the present pass (gamma, sharpening), at the drawable's size.
 *
 * Opening it: F1 (fn+F1 on a Mac keyboard), Insert/Help or Cmd+, on the
 * keyboard, L3+R3 together on a controller. Esc, the same keys or Circle
 * close it. While it is open the game's pad, keyboard and mouse input is held
 * neutral and the mouse is released. The menu changes the picture, sound and
 * controller options live and saves them to PS3_OVERLAY_INI.
 *
 * Its Controls tab also switches the input mode (PS3_INPUT_MODE: controller,
 * keyboard and mouse, or both) and the mouse camera (PS3_MOUSE) live, and
 * rebinds every PS3 control on the keyboard, the mouse and the controller:
 * click a cell, press the new input (Esc cancels; the press never reaches the
 * game). A rebinding applies at once (cellPad_host_set_input_map) and is
 * written to the controls file the launcher's Controls screen edits --
 * PS3_INPUT_MAP, or input.toml next to PS3_OVERLAY_INI when that is not set
 * (pad_input_map.h, pim_save_file).
 *
 * Settings, from strongest to weakest:
 *   1. an environment variable that is set when the game starts
 *      (PS3_SHARPEN, PS3_AUDIO_VOLUME, ... -- the launcher sets the ones it
 *      changed from their defaults);
 *   2. the menu's saved file, PS3_OVERLAY_INI=<path> (run.sh points it at
 *      ~/Library/Application Support/infamous2-recomp/overlay.ini for
 *      windowed play; unset or empty = nothing is loaded or saved). Only the
 *      settings changed in the menu are written to it;
 *   3. the built-in defaults.
 * A change made in the menu applies at once for the session either way; the
 * menu marks a setting whose variable was set ("env"), because that variable
 * wins again at the next start.
 *
 * Knobs: PS3_OVERLAY=0 (no menu, no HUD, nothing loaded), PS3_OVERLAY_PAD=0
 * (no L3+R3), PS3_OVERLAY_HUD=1 (HUD on, also headless), PS3_OVERLAY_LANG=en|pt
 * (default from PS3_LANG), and for a headless test PS3_OVERLAY_FORCE_OPEN=1
 * (the menu drawn open; the frame dump then reads the composed image) with
 * PS3_OVERLAY_TAB=0..4 (Image, Audio, Controls, Performance, Game) to pick its tab. A
 * headless run without these two is untouched: nothing is created and no
 * file is read.
 *
 * Threads: ps3_overlay_render runs on the thread that presents; the event
 * hook on the main thread (the AppKit pump); the pad hooks live in cellPad.c
 * as plain globals so that file keeps building on its own.
 */
#ifndef PS3RECOMP_PS3_OVERLAY_H
#define PS3RECOMP_PS3_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while anything is to be drawn (menu open, HUD on, a notice up):
 * the present pass's one branch. */
extern volatile int g_ps3_overlay_active;

/* Render thread, at backend init: device is the MTLDevice (bridged). */
void ps3_overlay_init(void* mtl_device, int headless);
void ps3_overlay_shutdown(void);

/* Render thread, after the present pass: draws into the drawable / headless
 * target `dst` (an id<MTLTexture>) with its own render pass on `cb` (an
 * id<MTLCommandBuffer>). Call only while g_ps3_overlay_active. */
void ps3_overlay_render(void* cb, void* dst);

/* Main thread, for each NSEvent from the backend's pump before its own
 * handling; `view_w/h` the content view's size in points, `drawable_w/h` the
 * drawable's in pixels (for the letterbox). Returns 1 when the menu took the
 * event (the pump must not give it to the game; a mouse event still goes to
 * AppKit). */
int ps3_overlay_handle_event(void* nsevent, double view_w, double view_h,
                             double drawable_w, double drawable_h);

/* Main thread, from the pump: menu open (the game's input is held neutral).
 * Also takes a pending L3+R3 toggle from cellPad. */
int ps3_overlay_menu_open(void);

/* A notice in a corner for a few seconds (any thread). */
void ps3_overlay_notice(const char* text);

/* "Quitting the game..." in the menu's language (Cmd+Q, rsx_metal_backend.m). */
void ps3_overlay_quit_notice(void);

/* Headless + PS3_OVERLAY_FORCE_OPEN/HUD: the frame dump should read the
 * composed target instead of the game's surface. */
int ps3_overlay_composes_headless(void);

#ifdef __cplusplus
}
#endif

#endif
