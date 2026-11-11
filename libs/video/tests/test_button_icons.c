/*
 * rsx_button_icons: the mode, the generation the draw engine re-uploads the
 * prompt atlas on, and the glyph drawing (no game data: the atlas itself is
 * recognised by its pixels, which only the player's copy has, so here only a
 * look-alike is checked to be left alone).
 *
 *   cc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *      -o /tmp/test_button_icons libs/video/tests/test_button_icons.c \
 *      libs/video/rsx_button_icons.c -lm && /tmp/test_button_icons
 */
#include "rsx_button_icons.h"
#pragma GCC diagnostic ignored "-Wunused-function"   /* the parser half of the header */
#include "../../input/pad_input_map.h"
#include "../../input/pad_remap.h"                 /* [remap] the menu's rebinding rules */
#include "../../../include/ps3emu/ps3_pad_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static PadInputMap s_test_map;
static int s_test_mouse_cam = 1;
static void test_map(void* out) { memcpy(out, &s_test_map, sizeof s_test_map); }
static int test_mouse_cam(void) { return s_test_mouse_cam; }

static uint8_t img[256][512][4], ref[256][512][4];

/* Alpha in a box (inclusive), summed. */
static long alpha_sum(int x0, int y0, int x1, int y1)
{
    long s = 0;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) s += img[y][x][3];
    return s;
}
static int box_differs(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        if (memcmp(&img[y][x0], &ref[y][x0], (size_t)(x1 - x0 + 1) * 4)) return 1;
    return 0;
}

int main(void)
{
    setenv("PS3RECOMP_METAL_HEADLESS", "1", 1);
    unsetenv("PS3_BUTTON_ICONS");
    pim_defaults(&s_test_map);
    g_ps3_pad_icons.input_map = test_map;
    g_ps3_pad_icons.mouse_camera = test_mouse_cam;

    /* auto, nothing used yet, headless: the original glyphs */
    uint32_t g0 = rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_PLAYSTATION);
    CHECK(rsx_button_icons_frame() == g0);                         /* nothing changed: same generation */

    /* an Xbox One pad pressed (1 + SDL_CONTROLLER_TYPE_XBOXONE) */
    g_ps3_pad_icons.source = 1 + 2;
    uint32_t g1 = rsx_button_icons_frame();
    CHECK(g1 != g0 && ps3_button_icons_effective() == PS3_ICONS_XBOX);
    /* a DualSense (PS5 = 7) */
    g_ps3_pad_icons.source = 1 + 7;
    uint32_t g2 = rsx_button_icons_frame();
    CHECK(g2 != g1 && ps3_button_icons_effective() == PS3_ICONS_PLAYSTATION);
    /* the keyboard */
    g_ps3_pad_icons.source = 0;
    rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_KEYBOARD);
    /* a rebinding changes the keyboard glyphs: a new generation */
    uint32_t g3 = rsx_button_icons_frame();
    pim_key_set(s_test_map.key[PIM_CROSS], 0x0C);   /* Q */
    CHECK(rsx_button_icons_frame() != g3);
    /* nothing used, a controller connected: that controller */
    g_ps3_pad_icons.source = -1; g_ps3_pad_icons.connected = 1 + 1;   /* Xbox 360 */
    rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_XBOX);
    /* the menu's choice wins over auto, and -1 hands it back */
    ps3_button_icons_set_mode(PS3_ICONS_KEYBOARD);
    rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_KEYBOARD);
    CHECK(ps3_button_icons_mode_setting() == PS3_ICONS_KEYBOARD);
    ps3_button_icons_set_mode(-1);
    rsx_button_icons_frame();
    CHECK(ps3_button_icons_effective() == PS3_ICONS_XBOX);

    /* Anything that is not the atlas is left alone, whatever the mode. */
    for (int y = 0; y < 256; y++) for (int x = 0; x < 512; x++) {
        img[y][x][0] = img[y][x][1] = img[y][x][2] = 240; img[y][x][3] = (uint8_t)((x ^ y) & 0xFF);
    }
    memcpy(ref, img, sizeof img);
    CHECK(rsx_button_icons_upload(0x85, 512, 256, 1, &img[0][0][0], 2048) == 0);
    CHECK(!memcmp(img, ref, sizeof img));
    CHECK(rsx_button_icons_upload(0x86, 512, 256, 1, &img[0][0][0], 2048) == 0);   /* DXT1 */
    CHECK(rsx_button_icons_upload(0x85, 256, 256, 1, &img[0][0][0], 1024) == 0);

    /* Xbox glyphs: drawn inside the button boxes only. */
    memset(img, 0, sizeof img);
    memcpy(ref, img, sizeof img);
    PadInputMap def; pim_defaults(&def);
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_XBOX, &def, 1);
    CHECK(alpha_sum(57, 178, 94, 215) > 255L * 300);      /* the cross box: a disc */
    CHECK(alpha_sum(241, 8, 290, 41) > 255L * 600);       /* L1: a bumper */
    CHECK(img[196][62][1] > img[196][62][0] + 60 && img[196][62][3] == 255);   /* A: a green disc */
    CHECK(alpha_sum(3, 34, 234, 79) == 0);                 /* the logo's area is not touched */
    CHECK(alpha_sum(468, 54, 504, 147) == 0);              /* the walking man, the flag, the check */
    CHECK(alpha_sum(105, 176, 147, 218) == 0);             /* the D-pad stays the original */

    /* Keyboard glyphs follow the bindings: cross on Q differs from Space. */
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &def, 1);
    memcpy(ref, img, sizeof img);
    PadInputMap q = def;
    memset(q.key[PIM_CROSS], 0, sizeof q.key[PIM_CROSS]);
    pim_key_set(q.key[PIM_CROSS], 0x0C);
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &q, 1);
    CHECK(box_differs(57, 178, 94, 215));
    CHECK(!box_differs(419, 57, 456, 94));                 /* circle: same key, same glyph */
    /* R1 is the left mouse button with the mouse camera on, the O key without */
    memcpy(ref, img, sizeof img);
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &q, 0);
    CHECK(box_differs(359, 8, 408, 41));

    /* A controller rebinding shows the button pressed: cross on Y. */
    memset(img, 0, sizeof img);
    PadInputMap y = def; y.pad[PIM_CROSS] = 1u << 3;
    rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_XBOX, &y, 1);
    CHECK(img[196][62][0] > img[196][62][2] + 60 && img[196][62][1] > img[196][62][2] + 60);   /* Y: yellow */

    /* [remap] After a change in F2 > Controls (pad_remap.h's rules): the prompt is the new
     * key's, exactly as for a map that had it from the start, and the swapped key moves too. */
    {
        PadInputMap r = def;
        const PrmInput e = { PRM_KEY, 0x0E };              /* Cross's Space cell -> E (R1's): Swap */
        CHECK(prm_assign(&r, PIM_CROSS, e, 0x31) == 1u << PIM_R1);
        PadInputMap direct = def;
        memset(direct.key[PIM_CROSS], 0, sizeof direct.key[PIM_CROSS]);
        memset(direct.key[PIM_R1], 0, sizeof direct.key[PIM_R1]);
        pim_key_set(direct.key[PIM_CROSS], 0x0E);
        pim_key_set(direct.key[PIM_R1], 0x31);
        static uint8_t a[256][512][4];
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &r, 0);
        memcpy(a, img, sizeof img);
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &direct, 0);
        CHECK(!memcmp(a, img, sizeof img));                 /* the same atlas */
        memcpy(ref, img, sizeof img);
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &def, 0);
        CHECK(box_differs(57, 178, 94, 215));               /* cross: E, not Space */
        CHECK(box_differs(359, 8, 408, 41));                /* R1: Space, not E (mouse camera off: keys) */
        CHECK(!box_differs(419, 57, 456, 94));              /* circle: untouched */
        /* Start rebound to keypad Enter alone still shows ENTER (Enter twins) */
        PadInputMap s = def;
        const PrmInput kpe = { PRM_KEY, 0x4C };
        prm_clear_keys(&s, PIM_START);
        prm_assign(&s, PIM_START, kpe, -1);
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &s, 0);
        memcpy(a, img, sizeof img);
        memset(img, 0, sizeof img);
        rsx_button_icons_draw(&img[0][0][0], 2048, PS3_ICONS_KEYBOARD, &def, 0);
        CHECK(!memcmp(a, img, sizeof img));
        /* the live path: the map the menu set is what the next frame hashes */
        ps3_button_icons_set_mode(PS3_ICONS_KEYBOARD);
        s_test_map = def;
        const uint32_t g4 = rsx_button_icons_frame();
        s_test_map = r;
        const uint32_t g5 = rsx_button_icons_frame();
        CHECK(g5 != g4);
        CHECK(rsx_button_icons_frame() == g5);              /* and stays until the next change */
        ps3_button_icons_set_mode(-1);
    }

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("test_button_icons: ok\n");
    return 0;
}
