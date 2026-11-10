/* test_vmx_sse.c -- runtime/ppu/ppu_vmx_sse.h against runtime/ppu/ppu_vmx.h, bit for bit.
 *
 *   clang-cl /O2 /arch:AVX2 /I runtime/ppu tools/test_vmx_sse.c /Fe:test_vmx_sse.exe && test_vmx_sse.exe [cases]
 *
 * Every helper of the SSE header gets the same operands as its scalar namesake:
 * lanes drawn from special values (signed zeros, denormals, infinities, quiet and
 * signalling NaNs with payloads, extremes) and random bits, with VSCR[NJ] on and
 * off; any byte that differs is a failure (exit 1). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "ppu_vmx.h"
static uint8_t* vm_base;
/* ppu_recomp.h's raw load, as the lifted code has it */
#define VM_RAW_LD(T, ea) ({ const uint8_t* _p = vm_base + (uint32_t)(ea); T _v; memcpy(&_v, _p, sizeof(T)); _v; })
#define PPU_VMX_SSE_NO_REDIRECT   /* call each side by its own name */
#define PPU_VMX_FAST 1            /* and the fast forms (ppu3) */
#define PPU_VMX_CLEAN 1           /* the masked forms (ppu-vmx; -DPPU_VMX_CLEAN_VERIFY: the check build) */
#define PPU_VMX_INT 1             /* the integer ops (ppu-vmx) */
#include "ppu_vmx_sse.h"

static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 16); }
static const uint32_t SPECIAL[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu, 0x00400000u,
    0x00800000u, 0x80800000u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u,
    0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0xFF800001u, 0x7FBFFFFFu, 0x7FC12345u,
    0xFF812345u, 0x34000000u, 0xB4000000u, 0x0C000000u, 0x8C000000u, 0x4B800000u, 0x3F7FFFFFu,
};
#define NSP (sizeof SPECIAL / sizeof SPECIAL[0])
static uint32_t lane(void) {
    uint32_t k = rnd() % 8;
    if (k < 3) return SPECIAL[rnd() % NSP];
    if (k == 3) return rnd() & 0x807FFFFFu;                    /* denormal / zero */
    if (k == 4) return (rnd() & 0x80000000u) | ((rnd() % 40 + 100) << 23) | (rnd() & 0x7FFFFFu);  /* near 1 */
    if (k == 5) return SPECIAL[rnd() % NSP] ^ (rnd() & 1);    /* next to a special */
    return rnd();
}
/* big-endian register bytes, as lvx leaves them */
static void vec(uint8_t v[16]) { for (int i = 0; i < 4; i++) { uint32_t x = lane(); v[4*i] = x >> 24; v[4*i+1] = x >> 16; v[4*i+2] = x >> 8; v[4*i+3] = x; } }
static void bytes(uint8_t v[16]) { for (int i = 0; i < 16; i++) v[i] = (uint8_t)rnd(); }

static long fails;
static void check(const char* op, const uint8_t* want, const uint8_t* got, const uint8_t* a, const uint8_t* b, const uint8_t* c, uint32_t vscr) {
    if (!memcmp(want, got, 16)) return;
    if (fails++ < 12) {
        printf("FAIL %s vscr=%08X\n  a", op, vscr);
        for (int i = 0; i < 16; i++) printf("%s%02X", i % 4 ? "" : " ", a[i]);
        printf("\n  b"); for (int i = 0; i < 16; i++) printf("%s%02X", i % 4 ? "" : " ", b[i]);
        if (c) { printf("\n  c"); for (int i = 0; i < 16; i++) printf("%s%02X", i % 4 ? "" : " ", c[i]); }
        printf("\n  want"); for (int i = 0; i < 16; i++) printf("%s%02X", i % 4 ? "" : " ", want[i]);
        printf("\n  got "); for (int i = 0; i < 16; i++) printf("%s%02X", i % 4 ? "" : " ", got[i]);
        printf("\n");
    }
}

/* the scalar vperm the lifter emits inline */
static void vperm_ref(uint8_t* d, const uint8_t* a, const uint8_t* b, const uint8_t* c) {
    uint8_t tmp[16];
    for (int i = 0; i < 16; i++) { uint8_t sel = c[i] & 0x1F; tmp[i] = (sel < 16) ? a[sel] : b[sel - 16]; }
    memcpy(d, tmp, 16);
}

int main(int argc, char** argv) {
    long n = argc > 1 ? atol(argv[1]) : 4000000;
    uint8_t a[16], b[16], c[16], w[16], g[16];
    for (long it = 0; it < n; it++) {
        const uint32_t vscr = (it & 1) ? VMX_NJ : 0;
        vec(a); vec(b); vec(c);
        vmx_vaddfp(w, a, b, vscr);   vmx_vaddfp_sse(g, a, b, vscr);   check("vaddfp", w, g, a, b, 0, vscr);
        vmx_vsubfp(w, a, b, vscr);   vmx_vsubfp_sse(g, a, b, vscr);   check("vsubfp", w, g, a, b, 0, vscr);
        vmx_vmaddfp(w, a, b, c, vscr);  vmx_vmaddfp_sse(g, a, b, c, vscr);  check("vmaddfp", w, g, a, b, c, vscr);
        vmx_vnmsubfp(w, a, b, c, vscr); vmx_vnmsubfp_sse(g, a, b, c, vscr); check("vnmsubfp", w, g, a, b, c, vscr);
        /* aliasing as the lifted code does it: d == a */
        memcpy(w, a, 16); memcpy(g, a, 16);
        vmx_vmaddfp(w, w, b, c, vscr);  vmx_vmaddfp_sse(g, g, b, c, vscr);  check("vmaddfp d=a", w, g, a, b, c, vscr);
        vmx_vrefp(w, b, vscr);        vmx_vrefp_sse(g, b, vscr);        check("vrefp", w, g, a, b, 0, vscr);
        vmx_vrsqrtefp(w, b, vscr);    vmx_vrsqrtefp_sse(g, b, vscr);    check("vrsqrtefp", w, g, a, b, 0, vscr);
        vmx_vmaxfp(w, a, b, vscr);    vmx_vmaxfp_sse(g, a, b, vscr);    check("vmaxfp", w, g, a, b, 0, vscr);
        vmx_vminfp(w, a, b, vscr);    vmx_vminfp_sse(g, a, b, vscr);    check("vminfp", w, g, a, b, 0, vscr);
        if ((it & 7) == 2) memcpy(b, a, 16);   /* equal operands, zeros of both signs */
        { uint32_t r1 = vmx_vcmpeqfp(w, a, b, vscr), r2 = vmx_vcmpeqfp_sse(g, a, b, vscr); check("vcmpeqfp", w, g, a, b, 0, vscr);
          if (r1 != r2) { fails++; printf("FAIL vcmpeqfp cr6 %u %u\n", r1, r2); } }
        { uint32_t r1 = vmx_vcmpgefp(w, a, b, vscr), r2 = vmx_vcmpgefp_sse(g, a, b, vscr); check("vcmpgefp", w, g, a, b, 0, vscr);
          if (r1 != r2) { fails++; printf("FAIL vcmpgefp cr6 %u %u\n", r1, r2); } }
        { uint32_t r1 = vmx_vcmpgtfp(w, a, b, vscr), r2 = vmx_vcmpgtfp_sse(g, a, b, vscr); check("vcmpgtfp", w, g, a, b, 0, vscr);
          if (r1 != r2) { fails++; printf("FAIL vcmpgtfp cr6 %u %u\n", r1, r2); } }
        { unsigned u = rnd() & 31; bytes(b); vmx_vcfsx(w, b, u); vmx_vcfsx_sse(g, b, u); check("vcfsx", w, g, a, b, 0, u); }
        bytes(c);
        vperm_ref(w, a, b, c); ppu_vperm_sse(g, a, b, c); check("vperm", w, g, a, b, c, 0);
        memcpy(w, c, 16); memcpy(g, c, 16);
        vperm_ref(w, a, a, w); ppu_vperm_sse(g, a, a, g); check("vperm d=c a=b", w, g, a, a, c, 0);
        /* vsldoi / vspltw against the lifter's inline text, every constant */
#define SLD(N) { uint8_t tmp[32]; memcpy(tmp, a, 16); memcpy(tmp+16, b, 16); memcpy(w, tmp + N, 16); \
                 PPU_VSLDOI_SSE(g, a, b, N); check("vsldoi " #N, w, g, a, b, 0, N); }
        SLD(0) SLD(1) SLD(2) SLD(3) SLD(4) SLD(5) SLD(6) SLD(7) SLD(8) SLD(9) SLD(10) SLD(11) SLD(12) SLD(13) SLD(14) SLD(15)
#define SPW(U) { uint32_t* d = (uint32_t*)w; uint32_t val = ((uint32_t*)b)[U]; d[0] = d[1] = d[2] = d[3] = val; \
                 PPU_VSPLTW_SSE(g, b, U); check("vspltw " #U, w, g, a, b, 0, U); }
        SPW(0) SPW(1) SPW(2) SPW(3)
        /* merges as the lifter writes them */
        { uint32_t* d = (uint32_t*)w; uint32_t* x = (uint32_t*)a; uint32_t* y = (uint32_t*)b; uint32_t t[4];
          t[0]=x[0]; t[1]=y[0]; t[2]=x[1]; t[3]=y[1]; memcpy(d, t, 16); PPU_VMRG_SSE(g, a, b, _mm_unpacklo_epi32); check("vmrghw", w, g, a, b, 0, 0);
          t[0]=x[2]; t[1]=y[2]; t[2]=x[3]; t[3]=y[3]; memcpy(d, t, 16); PPU_VMRG_SSE(g, a, b, _mm_unpackhi_epi32); check("vmrglw", w, g, a, b, 0, 0); }
        { uint16_t* d = (uint16_t*)w; uint16_t* x = (uint16_t*)a; uint16_t* y = (uint16_t*)b; uint16_t t[8];
          for (int i = 0; i < 8; i++) t[i] = (i & 1) ? y[i / 2] : x[i / 2]; memcpy(d, t, 16); PPU_VMRG_SSE(g, a, b, _mm_unpacklo_epi16); check("vmrghh", w, g, a, b, 0, 0);
          for (int i = 0; i < 8; i++) t[i] = (i & 1) ? y[4 + i / 2] : x[4 + i / 2]; memcpy(d, t, 16); PPU_VMRG_SSE(g, a, b, _mm_unpackhi_epi16); check("vmrglh", w, g, a, b, 0, 0); }
        { uint8_t bb = (uint8_t)(rnd() % 17); for (int i = 0; i < 16; i++) w[i] = bb + i; PPU_LVSL_SSE(g, bb); check("lvsl", w, g, a, b, 0, bb); }
        { uint32_t r1 = vmx_vcmpequw(w, a, b), r2 = vmx_vcmpequw_sse(g, a, b); check("vcmpequw", w, g, a, b, 0, 0);
          if (r1 != r2) { fails++; printf("FAIL vcmpequw cr6 %u %u\n", r1, r2); } }
        { bytes(c); vmx_vslw(w, a, c); vmx_vslw_sse(g, a, c); check("vslw", w, g, a, c, 0, 0); }
        /* the lifter's two-half logic ops */
        { uint64_t* d = (uint64_t*)w; uint64_t* x = (uint64_t*)a; uint64_t* y = (uint64_t*)b; uint64_t* z = (uint64_t*)c;
          d[0] = x[0] ^ y[0]; d[1] = x[1] ^ y[1]; PPU_VLOGIC_SSE(g, _mm_xor_si128, a, b); check("vxor", w, g, a, b, 0, 0);
          d[0] = x[0] | y[0]; d[1] = x[1] | y[1]; PPU_VLOGIC_SSE(g, _mm_or_si128, a, b); check("vor", w, g, a, b, 0, 0);
          for (int i = 0; i < 2; i++) d[i] = x[i] & y[i]; PPU_VLOGIC_SSE(g, _mm_and_si128, a, b); check("vand", w, g, a, b, 0, 0);
          d[0] = x[0] & ~y[0]; d[1] = x[1] & ~y[1]; PPU_VLOGIC_SSE(g, ppu_vandc_sse, a, b); check("vandc", w, g, a, b, 0, 0);
          d[0] = (x[0] & ~z[0]) | (y[0] & z[0]); d[1] = (x[1] & ~z[1]) | (y[1] & z[1]); PPU_VSEL_SSE(g, a, b, c); check("vsel", w, g, a, b, c, 0); }
    }
    /* lvx / stvx: one 16-byte access, the same bytes as vm_raw_ld16 / vm_raw_st16 */
    {
        static uint8_t mem[4096]; vm_base = mem;
        for (int i = 0; i < 4096; i++) mem[i] = (uint8_t)rnd();
        for (uint32_t ea = 0; ea + 16 <= 4096; ea += 16) {
            uint8_t ref[16]; memcpy(ref, mem + ea, 16);
            ppu_vld16_sse(g, ea); check("lvx", ref, g, a, b, 0, ea);
            bytes(a); ppu_vst16_sse(ea, a); check("stvx", a, mem + ea, a, b, 0, ea);
        }
        for (uint32_t ea = 0; ea + 16 <= 4096; ea++) {   /* lvlx / lvrx, the lifter's loops */
            uint32_t sh = ea & 0xF; uint8_t* m = vm_base + (ea & ~0xFu);
            for (int i = 0; i < 16; i++) w[i] = (sh + (uint32_t)i < 16) ? m[sh + i] : 0;
            ppu_lvlx_sse(g, ea); check("lvlx", w, g, a, b, 0, ea);
            for (int i = 0; i < 16; i++) w[i] = ((uint32_t)i >= 16u - sh) ? m[i - (int)(16u - sh)] : 0;
            ppu_lvrx_sse(g, ea); check("lvrx", w, g, a, b, 0, ea);
        }
        for (uint32_t ea = 0; ea + 16 <= 4096; ea += 4) {   /* lvewx, the lifter's text */
            memset(w, 0, 16); { uint32_t _e = VM_RAW_LD(uint32_t, ea); memcpy(w + (ea & 15), &_e, 4); }
            memset(g, 0xEE, 16); ppu_lvewx_sse(g, ea); check("lvewx", w, g, a, b, 0, ea);
        }
    }
    /* PPU_VMX_FAST (ppu3): the fast forms against the scalar helpers, on the same mixed
     * operands and on mostly-normal ones (where the plain-result path is the one taken):
     * normal lanes over the whole exponent range, results that cancel to tiny or denormal
     * values (a - a*(1+e), a*c close to -b), and an occasional zero / inf / NaN / denormal. */
    {
        long taken = 0, total = 0;
        for (long it = 0; it < n; it++) {
            const uint32_t vscr = (it & 1) ? VMX_NJ : 0;
            const int mode = (int)(it % 3);   /* 0: mixed (lane()), 1: normal, 2: near-cancelling */
            for (int i = 0; i < 4; i++) {
                uint32_t x, y, z;
                if (mode == 0) { x = lane(); y = lane(); z = lane(); }
                else {
                    const uint32_t e1 = 1 + rnd() % 254, e2 = 1 + rnd() % 254;
                    x = (rnd() & 0x80000000u) | (e1 << 23) | (rnd() & 0x7FFFFFu);
                    y = (rnd() & 0x80000000u) | (e2 << 23) | (rnd() & 0x7FFFFFu);
                    z = (rnd() & 0x80000000u) | ((1 + rnd() % 254) << 23) | (rnd() & 0x7FFFFFu);
                    if (mode == 2) {   /* y close to -x or x (small exponents too), z near 1 */
                        const uint32_t ex = 1 + rnd() % 40;
                        x = (x & 0x807FFFFFu) | (ex << 23);
                        y = (x ^ ((rnd() & 1) ? 0x80000000u : 0)) + (rnd() % 5) - 2;
                        z = 0x3F800000u + (rnd() % 9) - 4;
                    }
                    if (rnd() % 64 == 0) x = SPECIAL[rnd() % NSP];
                    if (rnd() % 64 == 0) y = SPECIAL[rnd() % NSP];
                }
                a[4*i] = x >> 24; a[4*i+1] = x >> 16; a[4*i+2] = x >> 8; a[4*i+3] = x;
                b[4*i] = y >> 24; b[4*i+1] = y >> 16; b[4*i+2] = y >> 8; b[4*i+3] = y;
                c[4*i] = z >> 24; c[4*i+1] = z >> 16; c[4*i+2] = z >> 8; c[4*i+3] = z;
            }
            vmx_vaddfp(w, a, b, vscr);   vmx_vaddfp_fast(g, a, b, vscr);   check("vaddfp fast", w, g, a, b, 0, vscr);
            vmx_vsubfp(w, a, b, vscr);   vmx_vsubfp_fast(g, a, b, vscr);   check("vsubfp fast", w, g, a, b, 0, vscr);
            vmx_vmaddfp(w, a, b, c, vscr);  vmx_vmaddfp_fast(g, a, b, c, vscr);  check("vmaddfp fast", w, g, a, b, c, vscr);
            vmx_vnmsubfp(w, a, b, c, vscr); vmx_vnmsubfp_fast(g, a, b, c, vscr); check("vnmsubfp fast", w, g, a, b, c, vscr);
            /* the a*c + b cancellation: b = -(a*c) rounded, so the fused result is tiny */
            { uint8_t bb[16];
              for (int i = 0; i < 4; i++) { float fa, fc, fb; uint32_t ua = (uint32_t)a[4*i] << 24 | a[4*i+1] << 16 | a[4*i+2] << 8 | a[4*i+3];
                uint32_t uc = (uint32_t)c[4*i] << 24 | c[4*i+1] << 16 | c[4*i+2] << 8 | c[4*i+3];
                memcpy(&fa, &ua, 4); memcpy(&fc, &uc, 4); fb = -(fa * fc); uint32_t ub; memcpy(&ub, &fb, 4);
                bb[4*i] = ub >> 24; bb[4*i+1] = ub >> 16; bb[4*i+2] = ub >> 8; bb[4*i+3] = ub; }
              vmx_vmaddfp(w, a, bb, c, vscr);  vmx_vmaddfp_fast(g, a, bb, c, vscr);  check("vmaddfp fast cancel", w, g, a, bb, c, vscr);
              vmx_vnmsubfp(w, a, bb, c, vscr); vmx_vnmsubfp_fast(g, a, bb, c, vscr); check("vnmsubfp fast cancel", w, g, a, bb, c, vscr); }
            memcpy(w, a, 16); memcpy(g, a, 16);
            vmx_vmaddfp(w, w, b, c, vscr);  vmx_vmaddfp_fast(g, g, b, c, vscr);  check("vmaddfp fast d=a", w, g, a, b, c, vscr);
            memcpy(w, c, 16); memcpy(g, c, 16);
            vmx_vaddfp(w, a, w, vscr);  vmx_vaddfp_fast(g, a, g, vscr);  check("vaddfp fast d=b", w, g, a, c, 0, vscr);
            vmx_vmaxfp(w, a, b, vscr);   vmx_vmaxfp_fast(g, a, b, vscr);   check("vmaxfp fast", w, g, a, b, 0, vscr);
            vmx_vminfp(w, a, b, vscr);   vmx_vminfp_fast(g, a, b, vscr);   check("vminfp fast", w, g, a, b, 0, vscr);
            if ((it & 7) == 2) memcpy(b, a, 16);
            { uint32_t r1 = vmx_vcmpeqfp(w, a, b, vscr), r2 = vmx_vcmpeqfp_fast(g, a, b, vscr); check("vcmpeqfp fast", w, g, a, b, 0, vscr);
              if (r1 != r2) { fails++; printf("FAIL vcmpeqfp fast cr6 %u %u\n", r1, r2); } }
            { uint32_t r1 = vmx_vcmpgefp(w, a, b, vscr), r2 = vmx_vcmpgefp_fast(g, a, b, vscr); check("vcmpgefp fast", w, g, a, b, 0, vscr);
              if (r1 != r2) { fails++; printf("FAIL vcmpgefp fast cr6 %u %u\n", r1, r2); } }
            { uint32_t r1 = vmx_vcmpgtfp(w, a, b, vscr), r2 = vmx_vcmpgtfp_fast(g, a, b, vscr); check("vcmpgtfp fast", w, g, a, b, 0, vscr);
              if (r1 != r2) { fails++; printf("FAIL vcmpgtfp fast cr6 %u %u\n", r1, r2); } }
            /* how often the plain path decides (vmaddfp, as a sample) */
            { const __m128i x = vmxs_ld(a), y = vmxs_ld(b), cc = vmxs_ld(c);
              const __m128i z = vmxs_f2i(_mm_fmadd_ps(vmxs_i2f(x), vmxs_i2f(cc), vmxs_i2f(y))), z1 = vmxs_dm1(z);
              const __m128i m = _mm_min_epu32(_mm_min_epu32(vmxs_dm1(x), vmxs_dm1(y)), _mm_min_epu32(vmxs_dm1(cc), z1));
              total++; taken += !vmxs_special(m, z1); }
        }
        printf("fast forms: %ld cases, plain path on %ld (%.0f%%)\n", n, taken, total ? 100.0 * taken / total : 0.0);
    }
    /* PPU_VMX_CLEAN (ppu-vmx): the masked forms for every mask k. An operand whose bit is
     * clear is CLEAN, as tools/ppu_vmx_flow.py promises: with NJ = 1 no lane of it is a
     * denormal (one drawn as a denormal becomes a signed zero or the smallest normal),
     * with NJ = 0 it is anything. Same mixed / normal / near-cancelling lanes as above. */
    {
        long plain = 0;
        for (long it = 0; it < n; it++) {
            const uint32_t vscr = (it & 1) ? VMX_NJ : 0;
            const int mode = (int)((it >> 1) % 3);
            const unsigned k = (unsigned)(rnd() & 7);
            uint8_t* op3[3] = { a, b, c };
            for (int i = 0; i < 4; i++) {
                uint32_t v[3];
                if (mode == 0) { v[0] = lane(); v[1] = lane(); v[2] = lane(); }
                else {
                    for (int j = 0; j < 3; j++) v[j] = (rnd() & 0x80000000u) | ((1 + rnd() % 254) << 23) | (rnd() & 0x7FFFFFu);
                    if (mode == 2) { const uint32_t ex = 1 + rnd() % 40;
                        v[0] = (v[0] & 0x807FFFFFu) | (ex << 23);
                        v[1] = (v[0] ^ ((rnd() & 1) ? 0x80000000u : 0)) + (rnd() % 5) - 2;
                        v[2] = 0x3F800000u + (rnd() % 9) - 4; }
                    for (int j = 0; j < 3; j++) if (rnd() % 48 == 0) v[j] = SPECIAL[rnd() % NSP];
                }
                for (int j = 0; j < 3; j++) {
                    if (!(k >> j & 1) && (vscr & VMX_NJ) && (v[j] & 0x7F800000u) == 0 && (v[j] & 0x7FFFFFu))
                        v[j] = (v[j] & 0x80000000u) | ((rnd() & 1) ? 0x00800000u : 0);
                    op3[j][4*i] = v[j] >> 24; op3[j][4*i+1] = v[j] >> 16; op3[j][4*i+2] = v[j] >> 8; op3[j][4*i+3] = v[j];
                }
            }
            /* the 1-operand forms take their operand in b: b's clean rule is bit 1, the form's is bit 0 */
            const unsigned k1 = (k >> 1) & 1;
            vmx_vaddfp(w, a, b, vscr);   vmx_vaddfp_k(g, a, b, &vscr, k & 3);   check("vaddfp k", w, g, a, b, 0, vscr | k << 4);
            vmx_vsubfp(w, a, b, vscr);   vmx_vsubfp_k(g, a, b, &vscr, k & 3);   check("vsubfp k", w, g, a, b, 0, vscr | k << 4);
            vmx_vmaddfp(w, a, b, c, vscr);  vmx_vmaddfp_k(g, a, b, c, &vscr, k);  check("vmaddfp k", w, g, a, b, c, vscr | k << 4);
            vmx_vnmsubfp(w, a, b, c, vscr); vmx_vnmsubfp_k(g, a, b, c, &vscr, k); check("vnmsubfp k", w, g, a, b, c, vscr | k << 4);
            vmx_vrefp(w, b, vscr);       vmx_vrefp_k(g, b, &vscr, k1);       check("vrefp k", w, g, a, b, 0, vscr | k1 << 4);
            vmx_vrsqrtefp(w, b, vscr);   vmx_vrsqrtefp_k(g, b, &vscr, k1);   check("vrsqrtefp k", w, g, a, b, 0, vscr | k1 << 4);
            vmx_vmaxfp(w, a, b, vscr);   vmx_vmaxfp_k(g, a, b, &vscr, k & 3);   check("vmaxfp k", w, g, a, b, 0, vscr | k << 4);
            vmx_vminfp(w, a, b, vscr);   vmx_vminfp_k(g, a, b, &vscr, k & 3);   check("vminfp k", w, g, a, b, 0, vscr | k << 4);
            { uint8_t bb[16];   /* b = -(a*c): the fused result is the tiny residual */
              for (int i = 0; i < 4; i++) { float fa, fc, fb; uint32_t ua = (uint32_t)a[4*i] << 24 | a[4*i+1] << 16 | a[4*i+2] << 8 | a[4*i+3];
                uint32_t uc = (uint32_t)c[4*i] << 24 | c[4*i+1] << 16 | c[4*i+2] << 8 | c[4*i+3];
                memcpy(&fa, &ua, 4); memcpy(&fc, &uc, 4); fb = -(fa * fc); uint32_t ub; memcpy(&ub, &fb, 4);
                if (!(k & 2) && (vscr & VMX_NJ) && (ub & 0x7F800000u) == 0) ub &= 0x80000000u;
                bb[4*i] = ub >> 24; bb[4*i+1] = ub >> 16; bb[4*i+2] = ub >> 8; bb[4*i+3] = ub; }
              vmx_vmaddfp(w, a, bb, c, vscr);  vmx_vmaddfp_k(g, a, bb, c, &vscr, k);  check("vmaddfp k cancel", w, g, a, bb, c, vscr | k << 4);
              vmx_vnmsubfp(w, a, bb, c, vscr); vmx_vnmsubfp_k(g, a, bb, c, &vscr, k); check("vnmsubfp k cancel", w, g, a, bb, c, vscr | k << 4); }
            memcpy(w, a, 16); memcpy(g, a, 16);   /* d == a, d == c (the lifted code's aliasing) */
            vmx_vmaddfp(w, w, b, c, vscr);  vmx_vmaddfp_k(g, g, b, c, &vscr, k);  check("vmaddfp k d=a", w, g, a, b, c, vscr | k << 4);
            memcpy(w, c, 16); memcpy(g, c, 16);
            vmx_vnmsubfp(w, a, b, w, vscr); vmx_vnmsubfp_k(g, a, b, g, &vscr, k); check("vnmsubfp k d=c", w, g, a, b, c, vscr | k << 4);
            memcpy(w, b, 16); memcpy(g, b, 16);
            vmx_vaddfp(w, a, w, vscr);  vmx_vaddfp_k(g, a, g, &vscr, k & 3);  check("vaddfp k d=b", w, g, a, b, 0, vscr | k << 4);
            vmx_vaddfp(w, a, a, vscr);  vmx_vaddfp_k(g, a, a, &vscr, k & 1 ? 3 : 0);  check("vaddfp k a=b", w, g, a, a, 0, vscr | k << 4);
            if ((it & 7) == 2) memcpy(b, a, 16);   /* equal operands, zeros of both signs */
            vmx_vmaxfp(w, a, b, vscr);   vmx_vmaxfp_k(g, a, b, &vscr, k & 3);   check("vmaxfp k eq", w, g, a, b, 0, vscr | k << 4);
            vmx_vminfp(w, a, b, vscr);   vmx_vminfp_k(g, a, b, &vscr, k & 3);   check("vminfp k eq", w, g, a, b, 0, vscr | k << 4);
            { uint32_t r1 = vmx_vcmpeqfp(w, a, b, vscr), r2 = vmx_vcmpeqfp_k(g, a, b, &vscr, k & 3); check("vcmpeqfp k", w, g, a, b, 0, vscr | k << 4);
              if (r1 != r2) { fails++; printf("FAIL vcmpeqfp k cr6 %u %u\n", r1, r2); } }
            { uint32_t r1 = vmx_vcmpgefp(w, a, b, vscr), r2 = vmx_vcmpgefp_k(g, a, b, &vscr, k & 3); check("vcmpgefp k", w, g, a, b, 0, vscr | k << 4);
              if (r1 != r2) { fails++; printf("FAIL vcmpgefp k cr6 %u %u\n", r1, r2); } }
            { uint32_t r1 = vmx_vcmpgtfp(w, a, b, vscr), r2 = vmx_vcmpgtfp_k(g, a, b, &vscr, k & 3); check("vcmpgtfp k", w, g, a, b, 0, vscr | k << 4);
              if (r1 != r2) { fails++; printf("FAIL vcmpgtfp k cr6 %u %u\n", r1, r2); } }
            { const __m128i x = vmxs_ld(a), y = vmxs_ld(b), cc = vmxs_ld(c);   /* how often the plain path decides (vmaddfp) */
              plain += !vmxs_special_k(vmxs_f2i(_mm_fmadd_ps(vmxs_i2f(x), vmxs_i2f(cc), vmxs_i2f(y))), x, y, cc, k); }
        }
        printf("masked forms: %ld cases, plain path on %.0f%%\n", n, 100.0 * plain / (n ? n : 1));
#ifdef PPU_VMX_CLEAN_VERIFY
        /* the check build: a denormal operand left out of k under NJ = 1 is reported and the
         * exact form runs (same result) */
        { const long before = vmxs_clean_bad_n; const uint32_t nj = VMX_NJ;
          for (int i = 0; i < 16; i++) { a[i] = (uint8_t)(i * 7); b[i] = (uint8_t)(0x3F + i); c[i] = 0; }
          a[0] = 0x00; a[1] = 0x12; a[2] = 0x34; a[3] = 0x56;   /* a denormal lane in a */
          vmx_vmaddfp(w, a, b, c, nj); vmx_vmaddfp_k(g, a, b, c, &nj, 6); check("vmaddfp verify", w, g, a, b, c, nj);
          vmx_vaddfp(w, a, b, nj); vmx_vaddfp_k(g, a, b, &nj, 2); check("vaddfp verify", w, g, a, b, 0, nj);
          { uint32_t r1 = vmx_vcmpgtfp(w, a, b, nj), r2 = vmx_vcmpgtfp_k(g, a, b, &nj, 0); check("vcmpgtfp verify", w, g, a, b, 0, nj);
            if (r1 != r2) { fails++; printf("FAIL vcmpgtfp verify cr6\n"); } }
          if (vmxs_clean_bad_n - before != 3) { fails++; printf("FAIL verify: %ld reports, want 3\n", vmxs_clean_bad_n - before); }
          else printf("verify build: wrong claims reported and handled\n"); }
#endif
    }
    /* PPU_VMX_INT (ppu-vmx): the integer ops against ppu_vmx.h and the lifter's inline loops */
#define ADDSUB_REF(TY, CNT, W, OP, D, A, B) { const uint8_t* a_=(const uint8_t*)(A); const uint8_t* b_=(const uint8_t*)(B); uint8_t d_[16]; \
    for(int i=0;i<CNT;i++){ TY x=0,y=0; for(int k=0;k<W;k++){ x=(TY)((x<<8)|a_[i*W+k]); y=(TY)((y<<8)|b_[i*W+k]); } \
    TY r=(TY)(x OP y); for(int k=0;k<W;k++) d_[i*W+k]=(uint8_t)(r>>(8*(W-1-k))); } memcpy((D), d_, 16); }
#define MINMAX_REF(TY, CNT, W, CMP, D, A, B) { const uint8_t* a_=(const uint8_t*)(A); const uint8_t* b_=(const uint8_t*)(B); uint8_t o_[16]; \
    for(int i=0;i<CNT;i++){ uint32_t ux=0,uy=0; for(int k=0;k<W;k++){ ux=(ux<<8)|a_[i*W+k]; uy=(uy<<8)|b_[i*W+k]; } \
    TY x=(TY)ux, y=(TY)uy; TY r=(x CMP y)?x:y; for(int k=0;k<W;k++) o_[i*W+k]=(uint8_t)(((TY)r)>>(8*(W-1-k))); } memcpy((D), o_, 16); }
    for (long it = 0; it < n; it++) {
        bytes(a); bytes(b);
        if (it & 1) for (int i = 0; i < 16; i++) b[i] = a[i] ^ (uint8_t)(rnd() & rnd() & rnd());   /* close values */
        if ((it & 15) == 3) memcpy(b, a, 16);
        vmx_vslb(w, a, b);  vmx_vslb_sse(g, a, b);  check("vslb", w, g, a, b, 0, 0);
        vmx_vsrw(w, a, b);  vmx_vsrw_sse(g, a, b);  check("vsrw", w, g, a, b, 0, 0);
        vmx_vsraw(w, a, b); vmx_vsraw_sse(g, a, b); check("vsraw", w, g, a, b, 0, 0);
        vmx_vrlw(w, a, b);  vmx_vrlw_sse(g, a, b);  check("vrlw", w, g, a, b, 0, 0);
        { uint32_t r1 = vmx_vcmpgtsw(w, a, b), r2 = vmx_vcmpgtsw_sse(g, a, b); check("vcmpgtsw", w, g, a, b, 0, 0);
          if (r1 != r2) { fails++; printf("FAIL vcmpgtsw cr6 %u %u\n", r1, r2); } }
        { uint32_t r1 = vmx_vcmpgtuw(w, a, b), r2 = vmx_vcmpgtuw_sse(g, a, b); check("vcmpgtuw", w, g, a, b, 0, 0);
          if (r1 != r2) { fails++; printf("FAIL vcmpgtuw cr6 %u %u\n", r1, r2); } }
        ADDSUB_REF(uint8_t, 16, 1, +, w, a, b);  PPU_VINT8_SSE(g, _mm_add_epi8, a, b);   check("vaddubm", w, g, a, b, 0, 0);
        ADDSUB_REF(uint8_t, 16, 1, -, w, a, b);  PPU_VINT8_SSE(g, _mm_sub_epi8, a, b);   check("vsububm", w, g, a, b, 0, 0);
        ADDSUB_REF(uint16_t, 8, 2, +, w, a, b);  PPU_VINT16_SSE(g, _mm_add_epi16, a, b); check("vadduhm", w, g, a, b, 0, 0);
        ADDSUB_REF(uint16_t, 8, 2, -, w, a, b);  PPU_VINT16_SSE(g, _mm_sub_epi16, a, b); check("vsubuhm", w, g, a, b, 0, 0);
        ADDSUB_REF(uint32_t, 4, 4, +, w, a, b);  PPU_VINT32_SSE(g, _mm_add_epi32, a, b); check("vadduwm", w, g, a, b, 0, 0);
        ADDSUB_REF(uint32_t, 4, 4, -, w, a, b);  PPU_VINT32_SSE(g, _mm_sub_epi32, a, b); check("vsubuwm", w, g, a, b, 0, 0);
        MINMAX_REF(uint16_t, 8, 2, >, w, a, b);  PPU_VINT16_SSE(g, _mm_max_epu16, a, b); check("vmaxuh", w, g, a, b, 0, 0);
        MINMAX_REF(int16_t, 8, 2, >, w, a, b);   PPU_VINT16_SSE(g, _mm_max_epi16, a, b); check("vmaxsh", w, g, a, b, 0, 0);
        MINMAX_REF(uint32_t, 4, 4, >, w, a, b);  PPU_VINT32_SSE(g, _mm_max_epu32, a, b); check("vmaxuw", w, g, a, b, 0, 0);
        MINMAX_REF(int32_t, 4, 4, >, w, a, b);   PPU_VINT32_SSE(g, _mm_max_epi32, a, b); check("vmaxsw", w, g, a, b, 0, 0);
        MINMAX_REF(uint16_t, 8, 2, <, w, a, b);  PPU_VINT16_SSE(g, _mm_min_epu16, a, b); check("vminuh", w, g, a, b, 0, 0);
        MINMAX_REF(int16_t, 8, 2, <, w, a, b);   PPU_VINT16_SSE(g, _mm_min_epi16, a, b); check("vminsh", w, g, a, b, 0, 0);
        MINMAX_REF(uint32_t, 4, 4, <, w, a, b);  PPU_VINT32_SSE(g, _mm_min_epu32, a, b); check("vminuw", w, g, a, b, 0, 0);
        MINMAX_REF(int32_t, 4, 4, <, w, a, b);   PPU_VINT32_SSE(g, _mm_min_epi32, a, b); check("vminsw", w, g, a, b, 0, 0);
        { vec(b);   /* float lanes, and near the int32 / uint32 limits */
          if (it & 2) for (int i = 0; i < 4; i++) { uint32_t x = (rnd() & 0x80000000u) | ((150u + rnd() % 12) << 23) | (rnd() & 0x7FFFFFu);
              if (rnd() & 1) { static const uint32_t L[] = { 0x4F000000u, 0xCF000000u, 0x4F800000u, 0xBF800000u, 0x3F000000u };
                x = L[rnd() % 5] + (rnd() % 5) - 2; }   /* +-2^31, 2^32, -1, 0.5 */
              b[4*i] = x >> 24; b[4*i+1] = x >> 16; b[4*i+2] = x >> 8; b[4*i+3] = x; }
          const unsigned u = (it & 2) ? rnd() % 3 : rnd() & 31;
          uint32_t s1 = rnd() & 1, s2 = s1;
          vmx_vctsxs(w, b, u, &s1); vmx_vctsxs_sse(g, b, u, &s2); check("vctsxs", w, g, a, b, 0, u);
          if (s1 != s2) { fails++; printf("FAIL vctsxs SAT %u %u\n", s1, s2); }
          s1 = rnd() & 1; s2 = s1;
          vmx_vctuxs(w, b, u, &s1); vmx_vctuxs_sse(g, b, u, &s2); check("vctuxs", w, g, a, b, 0, u);
          if (s1 != s2) { fails++; printf("FAIL vctuxs SAT %u %u\n", s1, s2); } }
    }
    printf("%ld cases: %ld failures\n", n, fails);
    return fails != 0;
}
