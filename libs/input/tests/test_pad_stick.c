/* Stick/trigger shaping (pad_stick.h) and the controller parts of the input
 * map (pad_input_map.h): [controller_settings], [controller.<family>], the
 * stick half-axis inputs and the built-in layout. Plain C, no POSIX, so it
 * runs on Windows too:
 *
 *   clang -std=c11 -Wall -Wextra libs/input/tests/test_pad_stick.c -o test_pad_stick.exe
 *   cc -std=c11 -Wall -Wextra libs/input/tests/test_pad_stick.c -lm -o /tmp/t && /tmp/t
 */
#include "../pad_stick.h"
#include "../pad_input_map.h"
#include <assert.h>
#include <stdio.h>

static PadStickShape shape(float dz, float anti, float outer, float sq)
{
    PadStickShape s = { dz, anti, outer, sq };
    return pad_stick_shape_fix(s);
}

static void test_bytes(void)
{
    assert(pad_unit_to_byte(0.0f) == 128);
    assert(pad_unit_to_byte(1.0f) == 255);
    assert(pad_unit_to_byte(-1.0f) == 0);
    assert(pad_unit_to_byte(2.0f) == 255 && pad_unit_to_byte(-2.0f) == 0);
    assert(pad_unit_to_byte(0.001f) == 128 && pad_unit_to_byte(-0.001f) == 128);
}

static void test_shape(void)
{
    int x, y;
    const PadStickShape def = shape(PAD_STICK_DEFAULT_DEADZONE, 0, PAD_STICK_DEFAULT_OUTER, 0);
    /* rest, and jitter inside the dead zone: exactly centred */
    pad_stick_shape_apply(0, 0, &def, &x, &y); assert(x == 128 && y == 128);
    pad_stick_shape_apply(1500, -1800, &def, &x, &y); assert(x == 128 && y == 128);
    pad_stick_shape_apply(-32768, 0, &def, &x, &y); assert(x == 0 && y == 128);
    pad_stick_shape_apply(32767, 0, &def, &x, &y); assert(x == 255 && y == 128);
    pad_stick_shape_apply(0, -32768, &def, &x, &y); assert(x == 128 && y == 0);     /* SDL up = PS3 up */
    pad_stick_shape_apply(0, 32767, &def, &x, &y); assert(x == 128 && y == 255);
    /* just past the dead zone: a small step, not a jump */
    pad_stick_shape_apply((int)(0.10f * 32767), 0, &def, &x, &y); assert(x > 128 && x < 135);
    /* the outer edge saturates */
    pad_stick_shape_apply((int)(0.975f * 32767), 0, &def, &x, &y); assert(x == 255);
    /* monotonic along an axis */
    int prev = 128;
    for (int r = 0; r <= 32767; r += 512) { pad_stick_shape_apply(r, 0, &def, &x, &y); assert(x >= prev); prev = x; }
    /* anti-dead zone: the first step past the dead zone starts at it */
    const PadStickShape anti = shape(0.1f, 0.25f, 1.0f, 0);
    pad_stick_shape_apply((int)(0.11f * 32767), 0, &anti, &x, &y);
    assert(x >= 128 + (int)(0.25f * 127) && x <= 128 + (int)(0.27f * 127) + 1);
    /* round diagonal vs squircle: full diagonal reaches the corner at 1 */
    const PadStickShape round = shape(0.05f, 0, 1.0f, 0), square = shape(0.05f, 0, 1.0f, 1.0f),
                        half = shape(0.05f, 0, 1.0f, 0.5f);
    const int d = (int)(0.7071f * 32767);
    pad_stick_shape_apply(d, -d, &round, &x, &y);  assert(x >= 216 && x <= 220 && y >= 36 && y <= 40);
    pad_stick_shape_apply(d, -d, &square, &x, &y); assert(x >= 252 && y <= 3);
    pad_stick_shape_apply(d, -d, &half, &x, &y);   assert(x > 230 && x < 245);
    /* the squircle leaves the axes alone */
    pad_stick_shape_apply(32767, 0, &square, &x, &y); assert(x == 255 && y == 128);
    pad_stick_shape_apply(0, 16384, &square, &x, &y); assert(x == 128 && y > 180 && y < 200);
    /* fix() clamps nonsense */
    const PadStickShape bad = shape(5.0f, -1.0f, 0.1f, 9.0f);
    assert(bad.deadzone <= 0.9f && bad.anti_deadzone == 0.0f && bad.outer > bad.deadzone && bad.squircle == 1.0f);
}

static void test_triggers(void)
{
    assert(pad_trigger_shape(0, 0.02f) == 0);
    assert(pad_trigger_shape(500, 0.02f) == 0);          /* resting a hair off zero */
    assert(pad_trigger_shape(32767, 0.02f) == 255);
    int prev = 0;
    for (int r = 0; r <= 32767; r += 256) { const int v = pad_trigger_shape(r, 0.02f); assert(v >= prev); prev = v; }
}

static void test_defaults(void)
{
    PadInputMap m;
    pim_defaults(&m);
    /* controller: by position, touchpad = SELECT too */
    assert(m.pad[PIM_CROSS] == 1u << 0 && m.pad[PIM_CIRCLE] == 1u << 1);
    assert(m.pad[PIM_SELECT] == ((1u << 4) | (1u << 20)));
    assert(m.pad[PIM_L2] == 1u << PIM_PAD_TRIGGER_LEFT && m.pad[PIM_R2] == 1u << PIM_PAD_TRIGGER_RIGHT);
    /* mouse: left R1, right L1 (aim), middle R3 */
    assert(m.mouse[PIM_R1] == 1 && m.mouse[PIM_L1] == 2 && m.mouse[PIM_R3] == 4);
    assert(m.mouse[PIM_UP] == PIM_MOUSE_WHEEL_UP);
    /* keys: Space Cross, F Square, Q Triangle, Shift Circle, E R1, WASD */
    assert(m.key[PIM_CROSS][0x31 >> 5] >> (0x31 & 31) & 1);
    assert(m.key[PIM_SQUARE][0] >> 0x03 & 1);
    assert(m.key[PIM_TRIANGLE][0] >> 0x0C & 1);
    assert(m.key[PIM_CIRCLE][0x38 >> 5] >> (0x38 & 31) & 1);
    assert(m.key[PIM_R1][0] >> 0x0E & 1);
    assert((m.key[PIM_LS_UP][0] >> 0x0D & 1) && (m.key[PIM_LS_LEFT][0] & 1));
    /* Escape is the window's: not bound to anything */
    for (int c = 0; c < PIM_N_CONTROLS; c++) assert(!(m.key[c][0x35 >> 5] >> (0x35 & 31) & 1));
    /* every setting unset */
    assert(m.stick_deadzone[0] < 0 && m.stick_source[1] < 0 && m.nintendo_labels < 0 && m.trigger_threshold < 0);
    assert(m.pad_fam_set[0] == 0 && m.pad_fam_set[1] == 0);
}

static void test_settings_and_families(void)
{
    PadInputMap m;
    pim_defaults(&m);
    const char* text =
        "[controller_settings]\n"
        "deadzone = 0.15\n"
        "deadzone_right = 5%\n"
        "anti_deadzone_right = 20\n"          /* percent */
        "outer = 0.9\n"
        "squircle_left = 0\n"
        "trigger_threshold = 0.3\n"
        "left_stick = \"right\"\n"
        "right_stick = left\n"
        "invert_right_y = true\n"
        "nintendo_layout = \"label\"\n"
        "tilt = left_stick\n"
        "tilt_invert = yes\n"
        "[controller]\n"
        "cross = \"a, rightx+\"\n"            /* a stick half-axis as a button */
        "[controller.xbox]\n"
        "select = \"back\"\n"                 /* Xbox pads: View only */
        "circle = \"x\"\n"                    /* and x is circle, taken off square */
        "[controller.nintendo]\n"
        "start = \"start, guide\"\n";
    const int w = pim_parse(&m, text, stderr, "test");
    assert(w == 0);
    assert(m.stick_deadzone[0] > 0.149f && m.stick_deadzone[0] < 0.151f);
    assert(m.stick_deadzone[1] > 0.049f && m.stick_deadzone[1] < 0.051f);
    assert(m.stick_anti[1] > 0.199f && m.stick_anti[1] < 0.201f && m.stick_anti[0] < 0);
    assert(m.stick_outer[0] > 0.89f && m.stick_outer[1] > 0.89f);
    assert(m.stick_squircle[0] == 0.0f && m.stick_squircle[1] < 0);
    assert(m.trigger_threshold > 0.29f && m.trigger_threshold < 0.31f);
    assert(m.stick_source[0] == 1 && m.stick_source[1] == 0);
    assert(m.stick_invert[1][1] == 1 && m.stick_invert[1][0] < 0);
    assert(m.nintendo_labels == 1);
    assert(m.tilt == 2 && m.tilt_invert == 1);
    assert(m.pad[PIM_CROSS] == ((1u << 0) | (1u << 28)));
    /* resolve: generic pads keep [controller] */
    uint32_t r[PIM_N_CONTROLS];
    pim_pad_resolve(&m, PIM_FAM_OTHER, r);
    assert(r[PIM_SELECT] == ((1u << 4) | (1u << 20)) && r[PIM_SQUARE] == 1u << 2);
    /* Xbox: select = back alone, circle = x, and x taken off square */
    pim_pad_resolve(&m, PIM_FAM_XBOX, r);
    assert(r[PIM_SELECT] == 1u << 4 && r[PIM_CIRCLE] == 1u << 2 && r[PIM_SQUARE] == 0);
    assert(r[PIM_CROSS] == m.pad[PIM_CROSS]);
    /* Nintendo: start = start + guide */
    pim_pad_resolve(&m, PIM_FAM_NINTENDO, r);
    assert(r[PIM_START] == ((1u << 6) | (1u << 5)));
    assert(pim_family_of_sdl_type(5) == PIM_FAM_NINTENDO && pim_family_of_sdl_type(4) == PIM_FAM_PLAYSTATION &&
           pim_family_of_sdl_type(2) == PIM_FAM_XBOX && pim_family_of_sdl_type(0) == PIM_FAM_OTHER);
    /* bad values warn and leave the setting alone */
    PadInputMap b;
    pim_defaults(&b);
    assert(pim_parse(&b, "[controller_settings]\ndeadzone = lots\nleft_stick = up\nwobble = 1\n[controller.gamecube]\n", NULL, "t") == 4);
    assert(b.stick_deadzone[0] < 0 && b.stick_source[0] < 0);
}

static void test_render_keeps_sections(void)
{
    PadInputMap m;
    pim_defaults(&m);
    const char* old = "[controller_settings]\ndeadzone = 0.2\n\n[controller.xbox]\nselect = \"back\"\n";
    pim_parse(&m, old, NULL, "t");
    char* text = pim_render(&m, old);
    assert(text && strstr(text, "[controller_settings]") && strstr(text, "deadzone = 0.2"));
    assert(strstr(text, "[controller.xbox]"));
    PadInputMap again;
    pim_defaults(&again);
    assert(pim_parse(&again, text, stderr, "rendered") == 0);
    assert(again.stick_deadzone[0] > 0.19f && again.pad_fam_set[PIM_FAM_XBOX] == 1u << PIM_SELECT);
    for (int c = 0; c < PIM_N_CONTROLS; c++) {
        assert(again.pad[c] == m.pad[c] && again.mouse[c] == m.mouse[c]);
        for (int k = 0; k < 4; k++) assert(again.key[c][k] == m.key[c][k]);
    }
    free(text);
}

static void test_windows_key_names(void)
{
    PadInputMap m;
    pim_defaults(&m);
    assert(pim_parse(&m, "[keyboard]\ncross = \"Delete, LCtrl, Numpad5, PrintScreen, CapsLock\"\n", stderr, "t") == 0);
    const unsigned want[] = { 0x75, 0x3B, 0x57, 0x69, 0x39 };
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
        assert(m.key[PIM_CROSS][want[i] >> 5] >> (want[i] & 31) & 1);
}

int main(void)
{
    test_bytes();
    test_shape();
    test_triggers();
    test_defaults();
    test_settings_and_families();
    test_render_keeps_sections();
    test_windows_key_names();
    printf("test_pad_stick: ok\n");
    return 0;
}
