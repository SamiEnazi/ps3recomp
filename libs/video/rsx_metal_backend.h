/*
 * ps3recomp - RSX -> Metal backend (macOS / Apple Silicon)
 *
 * Mirrors the entry-point shape of rsx_d3d12_backend.h so a host can select a
 * backend at build time without any other change. Unlike the D3D12 backend this
 * file is NOT gated on a platform macro: the implementation lives in
 * rsx_metal_backend.m and is only compiled into the library on Apple targets
 * (see CMakeLists.txt), so there is nothing to stub out elsewhere.
 *
 * Headless mode: set PS3RECOMP_METAL_HEADLESS=1 to render to an offscreen
 * MTLTexture with no window or NSApplication. Everything below behaves
 * identically, which is what makes the backend testable in CI on a machine
 * with no display.
 */
#ifndef PS3RECOMP_RSX_METAL_BACKEND_H
#define PS3RECOMP_RSX_METAL_BACKEND_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the device, surface and command queue, and register the backend with
 * the RSX command processor. Returns 0 on success, -1 on failure. */
int  rsx_metal_backend_init(u32 width, u32 height, const char* title);

/* Unregister, then tear down the surface and device. Safe if init failed. */
void rsx_metal_backend_shutdown(void);

/* Pump the native event queue. Returns -1 when the window has been closed,
 * 0 otherwise. Always returns 0 in headless mode. */
int  rsx_metal_backend_pump_messages(void);

/* Present one frame: clear the drawable to the colour last received through the
 * RSX clear method, then hand it to the compositor. */
void rsx_metal_backend_present(void);

/* Optional diagnostics: PS3RECOMP_METAL_FPS=1 adds display FPS and frame times
 * to the native window title and stderr once per second. Call this exactly once
 * per guest FLIP submission, excluding boot redraws. It never drives rendering.
 * Without calls the separate guest submission rate stays zero. Headless mode
 * does not claim any display presentations and leaves the tracker disabled. */
void rsx_metal_backend_note_guest_frame(void);

/* The window (windowed runs only), set up from the environment at init:
 *   PS3_WINDOW_MODE=windowed|fullscreen|borderless   start mode (windowed);
 *       fullscreen is macOS's own (a Space of its own), borderless covers the
 *       screen in place with the menu bar and Dock hidden.
 *   PS3_WINDOW_SIZE=<w>x<h>   windowed content size in points (the guest's
 *       resolution by default), shrunk to fit the screen.
 *   PS3_WINDOW_ICON=<file>|game|none   the Dock / app switcher icon: an image
 *       file, or "game" for $PS3_VFS_ROOT/PS3_GAME/ICON0.PNG (none by default).
 *   PS3_DISPLAY=<n>|<name>    the screen it opens on (and fullscreen or
 *       borderless covers): 1 = the one with the menu bar, 2 = the next in the
 *       system's order, or a screen's name (exact, any case, or a part of
 *       it). The main screen when unset or not found.
 *   PS3_VSYNC=0               present without waiting for the screen's
 *       refresh (CAMetalLayer displaySyncEnabled off); the game's frame pacing
 *       comes from the host's vblank clock and does not change.
 *   PS3_MOUSE_CAPTURE=0       the mouse camera works without capturing the
 *       cursor (the old behaviour); there is no capture with PS3_MOUSE=0.
 *       [mouse] capture / camera in PS3_INPUT_MAP do the same when the
 *       variables are not set (libs/input/pad_input_map.h).
 * The window is resizable and the game's image keeps its aspect ratio,
 * letterboxed in black. Shortcuts: Cmd+F or Option+Return toggles fullscreen
 * (borderless when PS3_WINDOW_MODE=borderless, macOS fullscreen otherwise),
 * Cmd+Ctrl+F toggles macOS fullscreen. A click in the game captures the
 * mouse (hidden cursor, motion drives the camera); Esc, Cmd or leaving the
 * window releases it. */

/* --- test hooks ---------------------------------------------------------- */

/* The clear colour most recently set via NV4097_SET_COLOR_CLEAR_VALUE, in the
 * RSX's native ARGB8888. Lets a host assert the guest command stream actually
 * reached the backend. */
u32  rsx_metal_backend_debug_color(void);

/* Read the centre pixel of the last presented frame as BGRA8888, or 0 if
 * nothing has been presented. Headless mode only -- returns 0 with a live
 * drawable, whose contents belong to the compositor once presented. */
u32  rsx_metal_backend_readback_center(void);

/* How many draws of the last presented frame ran the guest's own vertex and
 * fragment programs rather than the built-in shader. A host mode that loads
 * programs asserts this is non-zero, because the built-in path would draw
 * the same test triangle and a pixel check alone could not tell them apart. */
u32  rsx_metal_backend_guest_draws(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_METAL_BACKEND_H */
