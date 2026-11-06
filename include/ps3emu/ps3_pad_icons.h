/*
 * ps3_pad_icons -- what the button-prompt glyphs (libs/video/rsx_button_icons.c)
 * need from the pad (libs/input/cellPad.c): which kind of device was used last
 * and the bindings. One weak object, as in ps3_toggles.h, so each side keeps
 * building on its own (the pad tests, the draw-engine test) with no new link
 * dependency.
 */
#ifndef PS3EMU_PS3_PAD_ICONS_H
#define PS3EMU_PS3_PAD_ICONS_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_PAD_ICONS_SHARED __declspec(selectany)
#else
#  define PS3_PAD_ICONS_SHARED __attribute__((weak))
#endif

typedef struct {
    /* The device of the last real input: -1 none yet, 0 the keyboard or
     * mouse, 1 + SDL_GameControllerType for a controller. */
    volatile int source;
    /* 1 + SDL_GameControllerType of a connected controller, 0 none. */
    volatile int connected;
    /* cellPad_host_input_map (a PadInputMap*) and whether the mouse turns the
     * camera; set by cellPad when it starts, NULL before. */
    void (*input_map)(void* out);
    int  (*mouse_camera)(void);
} ps3_pad_icon_state;

PS3_PAD_ICONS_SHARED ps3_pad_icon_state g_ps3_pad_icons = { -1, 0, 0, 0 };

#ifdef __cplusplus
}
#endif

#endif
