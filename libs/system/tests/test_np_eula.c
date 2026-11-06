/* macOS: clang -std=gnu17 -I include libs/system/tests/test_np_eula.c
 * libs/system/cellSysutil.c -Wl,-dead_strip -pthread -o /tmp/test_np_eula && /tmp/test_np_eula
 *
 * The PSN terms-of-service check answers through the title's callback on the
 * next sysutil poll, never at call time and never not at all (inFAMOUS 2
 * waited forever behind "Please wait..." when it did not). */
#include "../cellSysutilNpEula.c"
#include "ps3emu/guest_call.h"
#include <assert.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
void np_psnr_pump(void) {}
void cellMsgDialog_tick(void) {}
int ps3_savedata_busy(void) { return 0; }
static int s_online;
int ps3_netctl_online(void) { return s_online; }
int np_psnr_enabled(void) { return 0; }

static unsigned calls;
static u64 got[8];
static u32 got_opd;
static void caller(u32 opd, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    calls++; got_opd = opd;
    got[0] = a; got[1] = b; got[2] = c; got[3] = d; got[4] = e; got[5] = f; got[6] = g; got[7] = h;
}

int main(void)
{
    g_ps3_guest_caller = caller;

    /* Bad arguments fail at call time, as RPCS3's. */
    assert(sceNpEulaCheckEulaStatus(0, 0, 0, 0x100, 0) == (s32)SCE_NP_EULA_ERROR_INVALID_ARGUMENT);
    assert(sceNpEulaCheckEulaStatus(0x200, 0, 0, 0, 0) == (s32)SCE_NP_EULA_ERROR_INVALID_ARGUMENT);
    assert(sceNpEulaAbort() == (s32)SCE_NP_EULA_ERROR_NOT_INITIALIZED);

    /* Offline (the default): CELL_OK now, then status ERROR on the next poll.
     * inFAMOUS 2's func_002BA9E4 maps ERROR to "give up" (state 24, spinner off),
     * the call shape is its func_002BA498: r4 from its config, r5 = 0, r7 = 0. */
    assert(sceNpEulaCheckEulaStatus(0x200, 0x10, 0, 0x100, 0) == CELL_OK);
    assert(calls == 0);                                  /* not from inside the call */
    assert(sceNpEulaCheckEulaStatus(0x200, 0x10, 0, 0x100, 0) == (s32)SCE_NP_EULA_ERROR_ALREADY_INITIALIZED);
    cellSysutilCheckCallback();
    assert(calls == 1 && got_opd == 0x100);
    assert(got[0] == SCE_NP_EULA_ERROR && got[1] == SCE_NP_EULA_ERROR_UNKNOWN && got[2] == 0 && got[3] == 0);
    cellSysutilCheckCallback(); assert(calls == 1);      /* once */

    /* Abort before the poll: the answer becomes ABORTED (RPCS3), userdata kept. */
    assert(sceNpEulaCheckEulaStatus(0x200, 0, 0, 0x100, 0x5555) == CELL_OK);
    assert(sceNpEulaAbort() == CELL_OK);
    cellSysutilCheckCallback();
    assert(calls == 2 && got[0] == SCE_NP_EULA_ABORTED && got[3] == 0x5555);

    /* PS3_NET_ONLINE=1: RPCS3's answer, already accepted, version 1. */
    s_online = 1;
    assert(sceNpEulaCheckEulaStatus(0x200, 0, 0, 0x100, 0x77) == CELL_OK);
    cellSysutilCheckCallback();
    assert(calls == 3 && got[0] == SCE_NP_EULA_ALREADY_ACCEPTED && got[1] == 0 && got[2] == 1 && got[3] == 0x77);
    s_online = 0;

    /* "Show the terms": nothing to show, so it closes on the next poll. */
    assert(sceNpEulaShowCurrentEula(0x200, 0, 0x300, 0x99) == CELL_OK);
    assert(sceNpEulaCheckEulaStatus(0x200, 0, 0, 0x100, 0) == (s32)SCE_NP_EULA_ERROR_ALREADY_INITIALIZED);
    cellSysutilCheckCallback();
    assert(calls == 4 && got_opd == 0x300 && got[0] == 0 && got[2] == 0x99);

    puts("cellSysutilNpEula checks passed");
    return 0;
}
