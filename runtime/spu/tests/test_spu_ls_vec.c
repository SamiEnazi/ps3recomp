/* SPU_LS_VEC (spu_context.h): the quadword local-store access as one 16-byte load/store
 * and a word byte-swap shuffle must give the same per-lane _u32 values as the scalar form
 * (each lane = the big-endian word at p + 4*i), in both directions.
 *
 *   clang-cl /O2 /arch:AVX2 -I runtime/spu -I include test_spu_ls_vec.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "spu_context.h"

int main(void)
{
#if !SPU_LS_VEC
    puts("test_spu_ls_vec: SPU_LS_VEC is off in this build (needs SSSE3 on x86-64)");
    return 0;
#else
    uint64_t s = 0x9E3779B97F4A7C15ull;
    long bad = 0;
    for (long it = 0; it < 2000000; it++) {
        uint8_t mem[16], back[16];
        for (int i = 0; i < 16; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; mem[i] = (uint8_t)(s >> 24); }
        /* read: the vector path */
        u128 v;
        _mm_storeu_si128((__m128i*)v._u8, spu_ls_bswap_words(_mm_loadu_si128((const __m128i*)mem)));
        for (int i = 0; i < 4; i++) {
            const uint32_t want = (uint32_t)mem[4*i] << 24 | (uint32_t)mem[4*i+1] << 16 |
                                  (uint32_t)mem[4*i+2] << 8 | mem[4*i+3];
            if (v._u32[i] != want && ++bad <= 4)
                printf("read lane %d: got %08X want %08X\n", i, v._u32[i], want);
        }
        /* write: back to memory order */
        _mm_storeu_si128((__m128i*)back, spu_ls_bswap_words(_mm_loadu_si128((const __m128i*)v._u8)));
        if (memcmp(back, mem, 16) && ++bad <= 4) puts("write: bytes differ");
    }
    printf("test_spu_ls_vec: 2000000 quadwords, %ld mismatches\n", bad);
    return bad ? 1 : 0;
#endif
}
