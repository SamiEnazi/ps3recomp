/* cellPad's keyboard/mouse pad with PS3_INPUT_MAP: with no file every key
 * does exactly what the hard-coded table did; with one, the keys, mouse
 * buttons, wheel and capture setting follow it.
 *
 *   cc -std=gnu17 -I include -isystem /opt/homebrew/include \
 *      -isystem /opt/homebrew/include/SDL2 libs/input/tests/test_pad_keymap.c \
 *      -Wl,-dead_strip -o /tmp/test_pad_keymap && /tmp/test_pad_keymap
 * (SDL's headers only: -dead_strip drops the SDL backend, nothing links SDL.)
 */
#include "../cellPad.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static void press_only(const unsigned* vks, int n)
{
    for (int i = 0; i < 4; i++) g_pad_host_keys[i] = 0;
    for (int i = 0; i < n; i++) g_pad_host_keys[vks[i] >> 5] |= 1u << (vks[i] & 31);
}

/* The built-in layout (pim_defaults: inFAMOUS 2's PC keys, docs/INPUT.md). */
static void old_layout(const unsigned* vks, int n, u16* btns, int* lx, int* ly, int* rx, int* ry)
{
    static const struct { unsigned vk; u16 btn; } map[] = {
        {0x7E, CELL_PAD_CTRL_UP}, {0x7D, CELL_PAD_CTRL_DOWN},
        {0x7B, CELL_PAD_CTRL_LEFT}, {0x7C, CELL_PAD_CTRL_RIGHT},
        {0x12, CELL_PAD_CTRL_UP}, {0x13, CELL_PAD_CTRL_DOWN},
        {0x14, CELL_PAD_CTRL_LEFT}, {0x15, CELL_PAD_CTRL_RIGHT},
        {0x31, CELL_PAD_CTRL_CROSS}, {0x38, CELL_PAD_CTRL_CIRCLE},
        {0x03, CELL_PAD_CTRL_SQUARE}, {0x0C, CELL_PAD_CTRL_TRIANGLE},
        {0x06, CELL_PAD_CTRL_L1}, {0x0E, CELL_PAD_CTRL_R1},
        {0x08, CELL_PAD_CTRL_L2}, {0x0F, CELL_PAD_CTRL_R2},
        {0x09, CELL_PAD_CTRL_L3}, {0x07, CELL_PAD_CTRL_R3},
        {0x24, CELL_PAD_CTRL_START}, {0x4C, CELL_PAD_CTRL_START},
        {0x2E, CELL_PAD_CTRL_SELECT}, {0x30, CELL_PAD_CTRL_SELECT},
    };
    int down[128] = {0};
    for (int i = 0; i < n; i++) down[vks[i]] = 1;
    *btns = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (down[map[i].vk]) *btns |= map[i].btn;
    *lx = 128 + (down[0x02] ? 127 : 0) - (down[0x00] ? 128 : 0);
    *ly = 128 + (down[0x01] ? 127 : 0) - (down[0x0D] ? 128 : 0);
    *rx = 128 + (down[0x25] ? 127 : 0) - (down[0x26] ? 128 : 0);
    *ry = 128 + (down[0x28] ? 127 : 0) - (down[0x22] ? 128 : 0);
}

static void expect_like_old(const unsigned* vks, int n)
{
    u16 b; int lx, ly, rx, ry;
    old_layout(vks, n, &b, &lx, &ly, &rx, &ry);
    press_only(vks, n);
    pad_poll_keys(0);
    const PadHostState* hs = &s_host_state[0];
    assert(hs->buttons == b);
    assert(hs->analog_lx == lx && hs->analog_ly == ly && hs->analog_rx == rx && hs->analog_ry == ry);
    assert(hs->trigger_l2 == ((b & CELL_PAD_CTRL_L2) ? 255 : 0));
}

static void test_default_layout(void)
{
    unsetenv("PS3_INPUT_MAP");
    s_map_state = 0;
    pad_map_load();
    for (unsigned vk = 0; vk < 128; vk++) expect_like_old(&vk, 1);
    const unsigned combos[][4] = {
        {0x00, 0x02, 0, 0}, {0x0D, 0x01, 0, 0}, {0x0D, 0x02, 0x31, 0x22},
        {0x26, 0x25, 0x28, 0x24}, {0x08, 0x0F, 0x06, 0x0E},
    };
    expect_like_old(combos[0], 2);
    expect_like_old(combos[1], 2);
    for (int i = 2; i < 5; i++) expect_like_old(combos[i], 4);
    press_only(NULL, 0);
    /* mouse buttons: left R1, right L1 (aim), middle R3, 4 R2, 5 L2, others nothing */
    g_pad_mouse_buttons = 1; pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_R1);
    g_pad_mouse_buttons = 2; pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_L1);
    g_pad_mouse_buttons = 4; pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_R3);
    g_pad_mouse_buttons = 8;  pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_R2);
    g_pad_mouse_buttons = 16; pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_L2);
    g_pad_mouse_buttons = 64; pad_poll_keys(0); assert(s_host_state[0].buttons == 0);
    g_pad_mouse_buttons = 0;
    g_pad_mouse_wheel = 16; pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_UP);   /* wheel up: D-pad up */
    for (int i = 0; i < 8; i++) { usleep(100 * 1000); pad_poll_keys(0); }   /* let the queued notches pass */
    g_pad_mouse_wheel = -16; pad_poll_keys(0); assert(s_host_state[0].buttons == 0);              /* wheel down: nothing */
    assert(cellPad_host_mouse_capture_wanted() == 1);
}

static void test_mapped_layout(void)
{
    char path[] = "/tmp/pad_keymap_XXXXXX";
    int fd = mkstemp(path);
    FILE* f = fdopen(fd, "w");
    fputs("[keyboard]\n"
          "cross  = \"F, Shift\"\n"
          "circle = \"\"\n"            /* Q no longer does anything */
          "ls_up  = \"Up\"\n"          /* the arrow walks forward ... */
          "up     = \"T\"\n"           /* ... and the D-pad up moved to T */
          "rs_left = \"Q\"\n"
          "[mouse_buttons]\n"
          "r1 = \"Button4\"\n"
          "up = \"WheelUp\"\n"
          "[mouse]\n"
          "capture = false\n", f);
    fclose(f);
    setenv("PS3_INPUT_MAP", path, 1);
    s_map_state = 0;
    pad_map_load();
    remove(path);

    unsigned k;
    k = 0x03; press_only(&k, 1); pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS);
    k = 0x38; press_only(&k, 1); pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_CROSS);
    k = 0x31; press_only(&k, 1); pad_poll_keys(0); assert(s_host_state[0].buttons == 0);   /* Space unbound now */
    k = 0x0C; press_only(&k, 1); pad_poll_keys(0);
    assert(s_host_state[0].buttons == 0 && s_host_state[0].analog_rx == 0);                /* Q: camera left */
    k = 0x7E; press_only(&k, 1); pad_poll_keys(0);
    assert(s_host_state[0].buttons == 0 && s_host_state[0].analog_ly == 0);
    k = 0x11; press_only(&k, 1); pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_UP);
    k = 0x0E; press_only(&k, 1); pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_R1);   /* E keeps R1 */
    press_only(NULL, 0);
    g_pad_mouse_buttons = 1; pad_poll_keys(0); assert(s_host_state[0].buttons == 0);
    g_pad_mouse_buttons = 8; pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_R1);
    g_pad_mouse_buttons = 2; pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_L1);
    g_pad_mouse_buttons = 0;

    /* two wheel notches up: two separate presses of UP */
    g_pad_mouse_wheel = 32;
    pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_UP);
    usleep(85 * 1000);
    pad_poll_keys(0); assert(s_host_state[0].buttons == 0);
    usleep(50 * 1000);
    pad_poll_keys(0); assert(s_host_state[0].buttons == CELL_PAD_CTRL_UP);
    usleep(130 * 1000);
    pad_poll_keys(0); assert(s_host_state[0].buttons == 0);
    /* a turn down is not bound */
    g_pad_mouse_wheel = -16;
    pad_poll_keys(0); assert(s_host_state[0].buttons == 0);

    /* capture: the file says no, the variable wins */
    unsetenv("PS3_MOUSE_CAPTURE");
    unsetenv("PS3_MOUSE");
    assert(cellPad_host_mouse_capture_wanted() == 0);
    setenv("PS3_MOUSE_CAPTURE", "1", 1);
    assert(cellPad_host_mouse_capture_wanted() == 1);
    setenv("PS3_MOUSE", "0", 1);
    assert(cellPad_host_mouse_capture_wanted() == 0);
}

static void test_sdl_triggers(void)
{
    const uint32_t lt = 1u << PIM_PAD_TRIGGER_LEFT, rt = 1u << PIM_PAD_TRIGGER_RIGHT;
    assert(pad_sdl_analog(lt, 0, 77, 9) == 77);            /* default L2: the left trigger's own value */
    assert(pad_sdl_analog(lt | rt, 0, 77, 99) == 99);      /* both triggers: the stronger */
    assert(pad_sdl_analog(1u << 9, 0, 77, 99) == 0);       /* L1 bound, up */
    assert(pad_sdl_analog(1u << 9, 1u << 9, 0, 0) == 255); /* L1 bound, down */
}

int main(void)
{
    test_default_layout();
    test_mapped_layout();
    test_sdl_triggers();
    printf("test_pad_keymap: ok\n");
    return 0;
}
