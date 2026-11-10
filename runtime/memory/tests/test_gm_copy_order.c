/* (critic2) test_gm_copy_order: does a FIFO-style reader (wait for the first word to change,
 * then read upward) ever see a half-written segment? gm_copy_to (ascending) vs gm_copy_to_desc.
 * Windows; build: clang-cl /O2 -Iruntime/memory runtime/memory/tests/test_gm_copy_order.c
 * Run "a" (ascending: expect some half-written reads) and "d" (top-down: expect 0). */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "guest_mem_atomic.h"
#define WORDS 96                     /* a 384-byte segment, like an Edge output segment */
static __declspec(align(128)) uint8_t g_buf[WORDS * 4 + 128];
static volatile LONG g_round, g_ack, g_stop;
static int g_desc;
static long long g_torn, g_seen;
static DWORD WINAPI reader(LPVOID p)
{
    (void)p;
    for (LONG r = 1; !g_stop; r++) {
        /* wait for round r's first word, then check every word of the segment */
        for (;;) {
            uint32_t w0 = gm_load32(g_buf);
            if (w0 == (uint32_t)r) break;
            if (g_stop) return 0;
            YieldProcessor();
        }
        int torn = 0;
        for (int k = 1; k < WORDS; k++) if (gm_load32(g_buf + 4 * k) != (uint32_t)r) { torn = 1; break; }
        g_seen++; g_torn += torn;
        InterlockedExchange(&g_ack, r);
    }
    return 0;
}
int main(int argc, char** argv)
{
    g_desc = argc > 1 && argv[1][0] == 'd';
    const LONG rounds = 200000;
    HANDLE t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    uint32_t src[WORDS];
    for (LONG r = 1; r <= rounds; r++) {
        for (int k = 0; k < WORDS; k++) src[k] = (uint32_t)r;
        if (g_desc) gm_copy_to_desc(g_buf, src, sizeof src); else gm_copy_to(g_buf, src, sizeof src);
        while (InterlockedCompareExchange(&g_ack, 0, 0) != r) YieldProcessor();
    }
    g_stop = 1; WaitForSingleObject(t, INFINITE);
    printf("%s: %lld segments, %lld read half-written\n", g_desc ? "top-down (gm_copy_to_desc)" : "ascending (gm_copy_to)", g_seen, g_torn);
    return 0;
}
