/*
 * vk_overlay -- the Vulkan backend's in-game overlay: a performance HUD and
 * the PC settings menu, drawn with Dear ImGui (third_party/imgui v1.92.9b:
 * imgui_impl_vulkan + imgui_impl_sdl2) on the swapchain image after the
 * game's frame is blitted into it. The design and most of the menu follow
 * the Metal backend's ps3_overlay.mm.
 *
 * Keys (game window):
 *   F1           performance HUD on/off (default on, top left)
 *   F2           settings menu open/close (Esc also closes it)
 *   Alt+Enter, F11  fullscreen on/off
 * Controller: hold L3+R3 together for 1 s (any controller) to open/close the
 * menu; in it the D-pad / left stick move, Cross picks, Circle closes, L1/R1
 * switch pages.
 * While the menu is open the game reads neutral input (cellPad's
 * g_pad_overlay_open) and gets no keyboard or mouse events.
 * The system's save-data list (cellSaveDataListSave2 / ListLoad2: pick, new
 * save, overwrite, delete, back) takes the menu's window while it is up:
 * vk_overlay_saves.cpp, include/ps3emu/ps3_savedlg.h; PS3_SAVEDATA_DIALOG=0 = none.
 * So does the system's message dialog (cellMsgDialogOpen2 Yes/No and OK: the
 * player answers; include/ps3emu/ps3_msgdlg.h; PS3_MSGDIALOG=0 = answered by itself).
 *
 * Settings (environment variable names; menu changes apply at once where the
 * module supports it, see include/ps3emu/ps3_settings.h, and are saved):
 *   PS3_OVERLAY_INI=<path>   the settings file; unset = %APPDATA%\inFAMOUS2-port\settings.ini
 *                            (~/.config/infamous2-port/settings.ini off Windows); set but
 *                            empty = nothing is read or saved (test runs)
 *   PS3_OVERLAY_DEFAULTS="K=V;K=V"  defaults below the file (a launcher's own defaults)
 *   Priority: a variable set when the game starts > the file > PS3_OVERLAY_DEFAULTS
 *   > built-in. The file is read before main() and each line whose variable
 *   is not set is exported, so every module reads it as it always read its
 *   variable. Headless runs (no PS3RECOMP_VK_WINDOW) read no file.
 *   PS3_OVERLAY=0            no overlay at all (no HUD, no menu, no file)
 *   PS3_OVERLAY_HUD=0|1      HUD at start (default 1); _CORNER 0..3, _OPACITY, _GRAPH, _CPU
 *   PS3_OVERLAY_PAD=0        no L3+R3 chord
 *   PS3_LANG / PS3_VOICE_LANG / PS3_TEXT_LANG   Audio page > Language, saved for the next
 *                            start (the game reads them once): auto = the Windows display
 *                            language, same = follow the game's (cellSysutil.c, ppu_fs_lang.h)
 * Tests / screenshots:
 *   PS3RECOMP_OVERLAY_MENU_OPEN=1        menu open from the start
 *   PS3RECOMP_OVERLAY_TAB=<page>         0..5 or display|graphics|audio|controls|hud|cheats
 *                                        (language = audio)
 *                                        (cheats: only when the title registered some,
 *                                        include/ps3emu/ps3_cheats.h, vk_overlay_cheats.cpp)
 *   PS3RECOMP_OVERLAY_MENU_AT="40:display,50:audio,60:close"  a timed script
 *                                        (seconds after the window opened); also
 *                                        hud0 / hud1, set=VAR=value (as the menu sets it)
 *                                        and cheat=id=value (as the Cheats page sets it)
 *
 * Threads: everything runs on the thread that owns the window and presents
 * (the port's frame-clock / RSX thread): attach, draw, poll and the event
 * hook. ps3_overlay-style notices can come from any thread.
 */
#ifndef PS3RECOMP_VK_OVERLAY_H
#define PS3RECOMP_VK_OVERLAY_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while anything is to be drawn (HUD on, menu open, a notice up):
 * the present path's one branch -- with it 0 the overlay costs nothing. */
extern volatile int g_vk_overlay_active;

typedef struct vk_overlay_attach_info {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    uint32_t         queue_family;
    VkQueue          queue;
    void*            sdl_window;      /* SDL_Window* */
    uint32_t         render_width, render_height;   /* the game's image (shown in the menu) */
} vk_overlay_attach_info;

/* Once the device and the window's swapchain exist. */
void vk_overlay_attach(const vk_overlay_attach_info* info);

/* vk_create_swapchain: a new swapchain was made and the old one is about to
 * be destroyed (the device is idle). `color_ok`: its images can be colour
 * attachments (the overlay draws nothing otherwise). */
void vk_overlay_swapchain_changed(VkFormat format, int color_ok);

/* Record the overlay into `cmd` on swapchain image `image` (index `index`,
 * size `extent`), which is in `layout` after work in `stage` / `access`.
 * Returns 1 when it drew: the image is then in PRESENT_SRC_KHR; 0 = nothing
 * recorded, the caller does its own transition. Call only while
 * g_vk_overlay_active. */
int vk_overlay_draw(VkCommandBuffer cmd, uint32_t index, VkImage image, VkExtent2D extent,
                    VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access);

/* The event pump: once per pump (the chord and the timed script), and each
 * SDL_Event before the backend's own handling. Returns 1 when the overlay
 * took the event (F1/F2/Alt+Enter, and every key and mouse event while the
 * menu is open): the pump must not act on it. */
void vk_overlay_poll(void);
int  vk_overlay_event(const void* sdl_event);

/* A notice in a corner for a few seconds (any thread). */
void vk_overlay_notice(const char* text);

/* Open the menu (Esc with the menu closed). 1 = opened, 0 = no overlay. */
int  vk_overlay_open_menu(void);

/* PS3RECOMP_VK_PRESENT_ASYNC (rsx3): the present -- and the HUD with it -- may
 * run on the backend's recording thread. The menu (whose settings appliers may
 * touch the window) is drawn only on the RSX thread: while it is open the
 * backend presents there; set while the worker draws, vk_overlay_draw then
 * skips a menu that opened meanwhile (one frame). ImGui itself is guarded. */
int  vk_overlay_menu_open(void);
extern volatile int g_vk_overlay_on_worker;

/* Window teardown (device alive and idle): frees everything. */
void vk_overlay_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
