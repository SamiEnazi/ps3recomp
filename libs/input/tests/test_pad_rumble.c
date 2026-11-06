/* cellPadSetActDirect -> host rumble: PS3_RUMBLE / PS3_RUMBLE_STRENGTH, the
 * motor levels, and the per-port state the pad poll renews.
 *
 *   cc -std=gnu17 -I include -isystem /opt/homebrew/include \
 *      -isystem /opt/homebrew/include/SDL2 libs/input/tests/test_pad_rumble.c \
 *      -L /opt/homebrew/lib -lSDL2 -o /tmp/test_pad_rumble && /tmp/test_pad_rumble
 * (No controller is opened: the state is checked, nothing vibrates.)
 */
#include "../cellPad.c"
#include <assert.h>
#include <sys/mman.h>
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
/* the guest-memory writers' SPU coherence hooks (ppu_memory.h) */
uint64_t spu_lockline_mask_range(uint32_t ea, uint32_t len) { (void)ea; (void)len; return 0; }
void spu_lockline_lock_mask(uint64_t mask) { (void)mask; }
void spu_lockline_unlock_mask(uint64_t mask) { (void)mask; }
void spu_coh_notify_write_from_ppu(uint32_t addr) { (void)addr; }

static void test_strength(void)
{
    assert(pad_rumble_strength_from(NULL, NULL) == 100);
    assert(pad_rumble_strength_from("1", NULL) == 100);
    assert(pad_rumble_strength_from("0", NULL) == 0);
    assert(pad_rumble_strength_from("0", "80") == 0);    /* off wins */
    assert(pad_rumble_strength_from(NULL, "50") == 50);
    assert(pad_rumble_strength_from(NULL, "0") == 0);
    assert(pad_rumble_strength_from(NULL, "250") == 200);
    assert(pad_rumble_strength_from(NULL, "-3") == 0);
    assert(pad_rumble_strength_from(NULL, "abc") == 100);
    assert(pad_rumble_strength_from(NULL, "") == 100);
}

static void test_levels(void)
{
    u16 lo, hi;
    pad_rumble_levels(0, 0, 100, &lo, &hi);   assert(lo == 0 && hi == 0);
    pad_rumble_levels(1, 255, 100, &lo, &hi); assert(lo == 65535 && hi == 65535);
    pad_rumble_levels(1, 0, 100, &lo, &hi);   assert(lo == 0 && hi == 65535);  /* small motor is on/off */
    pad_rumble_levels(255, 0, 100, &lo, &hi); assert(hi == 65535);            /* any non-zero is on */
    pad_rumble_levels(1, 128, 50, &lo, &hi);  assert(lo == 16448 && hi == 32767);
    pad_rumble_levels(0, 100, 200, &lo, &hi); assert(lo == 51400 && hi == 0);
    pad_rumble_levels(1, 255, 200, &lo, &hi); assert(lo == 65535 && hi == 65535); /* capped */
    pad_rumble_levels(1, 255, 0, &lo, &hi);   assert(lo == 0 && hi == 0);
}

static void test_set_act_direct(void)
{
    vm_base = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
    s_pad_initialized = 1;
    CellPadActParam* p = (CellPadActParam*)(vm_base + 0x100);
    const uintptr_t ea = 0x100;

    p->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = 1;
    p->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = 200;
    assert(cellPadSetActDirect(0, (CellPadActParam*)ea) == CELL_OK);
    assert(s_rumble[0].lo == 200 * 257 && s_rumble[0].hi == 65535);
    assert(s_rumble[0].sent_ms == 0);     /* no controller open: nothing sent, nothing to renew */

    p->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] = 0;
    p->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] = 0;
    assert(cellPadSetActDirect(0, (CellPadActParam*)ea) == CELL_OK);
    assert(s_rumble[0].lo == 0 && s_rumble[0].hi == 0);

    assert(cellPadSetActDirect(5, (CellPadActParam*)ea) == CELL_OK);   /* a port with no host pad */
    assert(cellPadSetActDirect(CELL_PAD_MAX_PORT_NUM, (CellPadActParam*)ea) == (s32)CELL_PAD_ERROR_INVALID_PARAMETER);
    assert(cellPadSetActDirect(0, NULL) == (s32)CELL_PAD_ERROR_INVALID_PARAMETER);
    s_pad_initialized = 0;
    assert(cellPadSetActDirect(0, (CellPadActParam*)ea) == (s32)CELL_PAD_ERROR_NOT_OPENED);
}

int main(void)
{
    unsetenv("PS3_RUMBLE");
    unsetenv("PS3_RUMBLE_STRENGTH");
    test_strength();
    test_levels();
    test_set_act_direct();
    printf("test_pad_rumble: ok\n");
    return 0;
}
