/*
 * ps3recomp - guest shader -> MSL translation test
 *
 * Hand-assembles the smallest programs the Metal backend has to run (the
 * encoders live in rsx_test_programs.h, shared with the host harness),
 * pushes each through its decompiler, then through rsx_hlsl_to_msl, and
 * checks the MSL carries the bindings the backend depends on. Needs no GPU,
 * so it runs on Linux CI too -- which is the point: glslang's HLSL front end
 * and spirv-cross are checked against the decompilers' actual output on every
 * push, not only on the Mac that happens to have Metal.
 *
 * Programs:
 *   VP  MOV o0, v0 ; MOV o1, v3 (END)      position through, colour through
 *   VP  MOV o0, v0 ; MOV o5, v3 (END)      + user clip planes (o5.yzw, o6.yzw)
 *   FP  MOV r0, COL0 (END)                 colour out
 *   FP  MOV r0, COL0.zyxw (END)            colour out, red and blue swapped:
 *                                          what the host's --shader mode runs
 *   FP  MOV r0, -abs(COL1.zyxw) (END)      colour clamp before source modifiers
 *   FP  TEX r0, TC0 unit 0 (END)           texture unit 0 sampled at texcoord0
 *   FP  TEX r0, TC0 unit 0, CUBE (END)     ... with unit 0 a cube map
 *   VP  MOV o0, v0 ; TXL o1, v8 (END)      a vertex texture sampled at t16
 *   FP  MOV r0, COL0 ; MOV r2, COL0.zyxw   two colour targets: what --mrt runs
 *   FP  (both FPs above) + depth export    SHADER_CONTROL & 0xE: depth = r1.z
 *
 * Each program's HLSL is checked for what was meant before the MSL is looked
 * at, so a wrong bit in an encoder shows up as a decompiler-level failure
 * rather than a mysterious MSL one.
 *
 * Built as a CMake target (test_shader_msl) because it links glslang and
 * spirv-cross. -v prints the HLSL and MSL of every program.
 */
#include "../rsx_vp_decompiler.h"
#include "../rsx_fp_decompiler.h"
#include "../rsx_shader_msl.h"
#include "../rsx_test_programs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0, g_verbose = 0;

static void check(const char* what, const char* text, const char* needle)
{
    if (text && strstr(text, needle)) {
        printf("[PASS] %s -- found \"%s\"\n", what, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- missing \"%s\"\n", what, needle);
        if (!g_verbose && text) printf("----\n%s\n----\n", text);
        g_fail++;
    }
}

static void check_absent(const char* what, const char* text, const char* needle)
{
    if (text && !strstr(text, needle)) {
        printf("[PASS] %s -- no \"%s\"\n", what, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- unexpected \"%s\"\n", what, needle);
        if (!g_verbose && text) printf("----\n%s\n----\n", text);
        g_fail++;
    }
}

static void check_count(const char* what, int got, int want)
{
    if (got == want) { printf("[PASS] %s: %d instruction(s)\n", what, got); g_pass++; }
    else { printf("[FAIL] %s: decompile returned %d, expected %d\n", what, got, want); g_fail++; }
}

/* ---- the translation step, shared by every case -------------------------- */

static char g_hlsl[65536];
static char g_msl[65536];
static char g_log[4096];

static int translate(const char* what, int stage)
{
    if (g_verbose) printf("---- %s HLSL ----\n%s\n", what, g_hlsl);
    int rc = rsx_hlsl_to_msl(g_hlsl, stage, g_msl, sizeof g_msl, g_log, sizeof g_log);
    if (rc != 0) {
        printf("[FAIL] %s -- translation failed: %s\n", what, g_log);
        if (!g_verbose) printf("---- HLSL ----\n%s\n", g_hlsl);
        g_fail++;
        return -1;
    }
    printf("[PASS] %s -- translated (%u bytes of MSL)\n", what, (unsigned)strlen(g_msl));
    g_pass++;
    if (g_verbose) printf("---- %s MSL ----\n%s\n", what, g_msl);
    return 0;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-v") == 0) g_verbose = 1;

    if (!rsx_hlsl_to_msl_available()) {
        printf("HLSL to MSL translation not built; nothing to test\n");
        return 2;
    }

    /* ---- vertex program: MOV o0, v0 ; MOV o1, v3 ------------------------- */
    {
        u8 vp[32];
        rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0 */
        rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);   /* MOV o1, v3, END */

        check_count("VP", rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl), 2);
        /* o0 takes v0 and o1 takes v3, unswizzled. */
        check("VP HLSL", g_hlsl, "(v[0]).xyzw");
        check("VP HLSL", g_hlsl, "o[0].xyzw = _v.xyzw;");
        check("VP HLSL", g_hlsl, "(v[3]).xyzw");
        check("VP HLSL", g_hlsl, "o[1].xyzw = _v.xyzw;");

        if (translate("VP", RSX_SHADER_STAGE_VERTEX) == 0) {
            check("VP MSL", g_msl, "vertex ");
            check("VP MSL", g_msl, "main0");
            /* The legalization pass drops inputs the program never reads,
             * so only v0 and v3 survive -- and v3 must still be attribute 3,
             * not renumbered to 1: the backend's vertex descriptor puts RSX
             * attribute i at offset i*16, used or not. */
            check("VP MSL", g_msl, "[[attribute(0)]]");
            check("VP MSL", g_msl, "[[attribute(3)]]");
            check("VP MSL", g_msl, "[[buffer(0)]]");      /* VPConst */
            check("VP MSL", g_msl, "[[position]]");
            check("VP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 */
            check("VP MSL", g_msl, "[[user(locn10)]]");   /* TEXCOORD7 */
        }
    }

    /* ---- user clip planes: MOV o0, v0 ; MOV o5, v3 ------------------------
     * The distances live in o5.yzw / o6.yzw and the planes are register
     * state (NV4097 0x1478), so they are added to the decompiled text:
     * plane 0 ENABLE_GE keeps d >= 0, plane 1 ENABLE_LT keeps d <= 0, the
     * rest of the vector never clips, and the varyings keep their slots. */
    {
        u8 vp[32];
        rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0 */
        rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 5, 1);   /* MOV o5, v3, END */
        check_count("clip VP", rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl), 2);
        const int none = rsx_vp_apply_user_clip(g_hlsl, sizeof g_hlsl, 0);
        const int small = rsx_vp_apply_user_clip(g_hlsl, strlen(g_hlsl) + 8, 2u);
        const int two = rsx_vp_apply_user_clip(g_hlsl, sizeof g_hlsl, 2u | (1u << 2));
        if (none == 0 && small == -1 && two == 0) { printf("[PASS] clip VP patch results\n"); g_pass++; }
        else { printf("[FAIL] clip VP patch results %d %d %d (want 0 -1 0)\n", none, small, two); g_fail++; }
        check("clip VP HLSL", g_hlsl, "float4 clipd0:SV_ClipDistance0;");
        check("clip VP HLSL", g_hlsl, "Out.clipd0 = float4(o[5].y, -(o[5].z), 0.5, 0.5);");
        check_absent("clip VP HLSL", g_hlsl, "SV_ClipDistance1");
        if (translate("clip VP", RSX_SHADER_STAGE_VERTEX) == 0) {
            check("clip VP MSL", g_msl, "[[clip_distance]]");
            check("clip VP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 */
            check("clip VP MSL", g_msl, "[[user(locn10)]]");   /* TEXCOORD7 */
        }
        /* Plane 5 (o6.w) adds the second vector. */
        check_count("clip VP 6", rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl), 2);
        if (rsx_vp_apply_user_clip(g_hlsl, sizeof g_hlsl, 2u << 10) == 0) { printf("[PASS] clip VP 6 patch\n"); g_pass++; }
        else { printf("[FAIL] clip VP 6 patch\n"); g_fail++; }
        check("clip VP 6 HLSL", g_hlsl, "float2 clipd1:SV_ClipDistance1;");
        check("clip VP 6 HLSL", g_hlsl, "Out.clipd1 = float2(0.5, o[6].w);");
        if (translate("clip VP 6", RSX_SHADER_STAGE_VERTEX) == 0)
            check("clip VP 6 MSL", g_msl, "[[clip_distance]]");
    }

    /* ---- fragment program: MOV r0, COL0 ---------------------------------- */
    {
        u8 fp[16];
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
        u32 nconst = 0;
        check_count("FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u /* r0 exports */, 0,
                                                       g_hlsl, sizeof g_hlsl, &nconst), 1);
        check("FP HLSL", g_hlsl, "(saturate(input.col0)).xyzw");
        check("FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
        check("FP HLSL", g_hlsl, "cbuffer PSConstants : register(b1)");
        /* The alpha test patch must survive translation as well. */
        if (rsx_fp_apply_alpha_test_buffered(g_hlsl, sizeof g_hlsl, 0x204u /* GREATER */) < 0) {
            printf("[FAIL] FP alpha test patch\n"); g_fail++;
        } else { printf("[PASS] FP alpha test patch\n"); g_pass++; }
        check("FP HLSL", g_hlsl, "fp_alpha.x");

        if (translate("FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("FP MSL", g_msl, "fragment ");
            check("FP MSL", g_msl, "main0");
            check("FP MSL", g_msl, "[[buffer(1)]]");      /* PSConstants */
            check("FP MSL", g_msl, "[[color(0)]]");
            check("FP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 from the VP */
            check("FP MSL", g_msl, "discard_fragment");
        }
    }

    /* ---- fragment program: MOV r0, COL0.zyxw (the host's --shader FP) ---- */
    {
        u8 fp[16];
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        check_count("swizzled FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                                g_hlsl, sizeof g_hlsl, NULL), 1);
        check("swizzled FP HLSL", g_hlsl, "(saturate(input.col0)).zyxw");
        check("swizzled FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
        if (translate("swizzled FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("swizzled FP MSL", g_msl, "fragment ");
    }

    /* ---- fragment program: MOV r0, -abs(COL1.zyxw) ------------------------
     * Clamp the interpolated colour before abs/negate: clamping a negated
     * colour instead would discard the negative result. DWORD1 bits 29 and
     * 17 are source abs and negate, respectively. */
    {
        u8 fp[16];
        rsx_test_fp_instr(fp,
                          RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                          RSX_TEST_FP_INSRC(2) /* COL1 */ | RSX_TEST_FP_END,
                          RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ(2, 1, 0, 3) |
                          (1u << 29) /* abs */ | (1u << 17) /* negate */);
        check_count("modified COL1 FP",
                    rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("modified COL1 FP HLSL", g_hlsl,
              "-(abs((saturate(input.col1)).zyxw))");
        if (translate("modified COL1 FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("modified COL1 FP MSL", g_msl, "fragment ");
            check("modified COL1 FP MSL", g_msl, "[[user(locn1)]]");
            check("modified COL1 FP MSL", g_msl, "[[color(0)]]");
        }
    }

    /* ---- fragment program: MOV r0, FACING ---------------------------------
     * Input 0xE is the face flag, +1 front / -1 back. Two-sided materials
     * multiply the normal by it; it used to read as zero. */
    {
        u8 fp[16];
        rsx_test_fp_instr(fp,
                          RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                          RSX_TEST_FP_INSRC(0xE) | RSX_TEST_FP_END,
                          RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        check_count("FACING FP",
                    rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("FACING FP HLSL", g_hlsl, "bool _rsx_front : SV_IsFrontFace;");
        check("FACING FP HLSL", g_hlsl, "input._rsx_front ? float4(1,1,1,1) : float4(-1,-1,-1,-1)");
        if (translate("FACING FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("FACING FP MSL", g_msl, "[[front_facing]]");
    }

    /* ---- fragment program: MOV r0, TC8 / TC9 ------------------------------
     * They follow TEXCOORD7 (locn10), and the VP writes them from o15 / o6. */
    for (u32 n = 8; n <= 9; n++) {
        u8 fp[16];
        rsx_test_fp_instr(fp,
                          RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                          RSX_TEST_FP_INSRC(0xC + (n - 8)) | RSX_TEST_FP_END,
                          RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        check_count("TC8/9 FP",
                    rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("TC8/9 FP HLSL", g_hlsl, n == 8 ? "(input.tc8).xyzw" : "(input.tc9).xyzw");
        check("TC8/9 FP HLSL", g_hlsl, "float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;");
        if (translate("TC8/9 FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("TC8/9 FP MSL", g_msl, n == 8 ? "[[user(locn11)]]" : "[[user(locn12)]]");
    }
    {
        u8 vp[16];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 15, 1);   /* MOV o15, v0, END */
        rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl);
        check("TC8 VP HLSL", g_hlsl, "Out.t8=o[15]; Out.t9=o[6];");
        if (translate("TC8 VP", RSX_SHADER_STAGE_VERTEX) == 0)
            check("TC8 VP MSL", g_msl, "[[user(locn11)]]");
    }

    /* ---- fragment program: TEX r0, TC0 (unit 0) --------------------------- */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        check_count("TEX FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                           g_hlsl, sizeof g_hlsl, NULL), 1);
        check("TEX FP HLSL", g_hlsl, "rsx_tex[0].Sample(rsx_samp[0]");
        /* Colour input clamping must not affect texture coordinates. */
        check("TEX FP HLSL", g_hlsl, "(input.tc0).xyzw");
        check("TEX FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");

        if (translate("TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("TEX FP MSL", g_msl, "[[texture(0)]]");
            check("TEX FP MSL", g_msl, "[[sampler(0)]]");
            check("TEX FP MSL", g_msl, "[[user(locn3)]]");    /* TEXCOORD0 */
        }
    }

    /* ---- vertex program sampling a VERTEX TEXTURE ------------------------
     * MOV o0, v0 ; TXL o1, v8 on vertex-texture unit 0 (END). A transform
     * program that samples is how a title places instances or displaces a
     * mesh, and the point of the case is the binding slot: the decompiler
     * declares the unit at HLSL register t16, and the backend binds it with
     * setVertexTexture at index 16, so what has to hold is that spirv-cross
     * carries that register number into the vertex function rather than
     * renumbering it from zero. */
    {
        u8 vp[32];
        rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_txl_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 0, 1, 1);

        check_count("vtex VP",
                    rsx_vp_decompile_ex(vp, sizeof vp, 1u /* unit 0 bound */,
                                        g_hlsl, sizeof g_hlsl), 2);
        check("vtex VP HLSL", g_hlsl, "Texture2D rsx_vtex0 : register(t16);");
        check("vtex VP HLSL", g_hlsl, "rsx_vtex0.SampleLevel(rsx_vsamp0,");

        if (translate("vtex VP", RSX_SHADER_STAGE_VERTEX) == 0) {
            check("vtex VP MSL", g_msl, "[[texture(16)]]");
            /* The sampler does NOT follow the texture's number: the
             * decompiler declares rsx_vsampN at register(sN), so unit 0's
             * sampler is [[sampler(0)]] while its texture is [[texture(16)]].
             * The backend binds them at those two different indices. */
            check("vtex VP MSL", g_msl, "[[sampler(0)]]");
        }

        /* Unmasked, the same program keeps its defined-zero fallback and
         * declares no texture at all -- which is why the mask has to be part
         * of the vertex program's cache key. */
        check_count("vtex VP unmasked",
                    rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl), 2);
        if (strstr(g_hlsl, "rsx_vtex0")) {
            printf("[FAIL] vtex VP unmasked -- declared rsx_vtex0 with no unit bound\n");
            g_fail++;
        } else {
            printf("[PASS] vtex VP unmasked -- no texture declared\n");
            g_pass++;
        }
    }

    /* ---- fragment program: TEX r0, TC0 on a CUBE unit --------------------
     * The same program, decompiled with unit 0 marked as a cube map. The
     * declaration and the sample both have to change: a cube is looked up
     * with a 3-component direction, not a 2D coordinate, and the backend
     * binds an MTLTextureTypeCube to that slot -- Metal rejects a 2D texture
     * bound where the shader declared a texturecube, so a mask that did not
     * survive translation would be a validation failure rather than a wrong
     * pixel. */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        check_count("cube TEX FP",
                    rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u,
                                                 1u /* unit 0 is a cube */,
                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("cube TEX FP HLSL", g_hlsl, "TextureCube rsx_tex0 : register(t0);");
        check("cube TEX FP HLSL", g_hlsl, "rsx_tex0.Sample(rsx_samp[0]");

        if (translate("cube TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("cube TEX FP MSL", g_msl, "texturecube");
            check("cube TEX FP MSL", g_msl, "[[texture(0)]]");
            check("cube TEX FP MSL", g_msl, "[[sampler(0)]]");
        }
    }

    /* ---- fragment program: TEX r0, TC0 with perspective_corr --------------
     * SRC2 bit 31 scales the texcoord by 1/w. Through glslang SV_POSITION.w
     * becomes FragCoord.w, which is already 1/w, so the helper must resolve
     * to the bare .w here (RSX_FRAGCOORD_W_IS_RCP) -- a division would undo
     * the correction and inFAMOUS 2's river would sample outside its copy. */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        rsx_test_fp_put_word(fp + 12, 1u << 31);
        check_count("persp TEX FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("persp TEX FP HLSL", g_hlsl, "(input.tc0 * _rsx_inv_w(input.position))");
        if (translate("persp TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("persp TEX FP MSL", g_msl, "gl_FragCoord.w");
            check_absent("persp TEX FP MSL", g_msl, "1.0 / gl_FragCoord.w");
        }
    }

    /* ---- fragment program: TEX r0, TC0 on a 3D unit ------------------------
     * A volume texture (SET_TEXTURE_FORMAT dimension 3): Texture3D in HLSL,
     * texture3d in MSL, sampled with three coordinates -- inFAMOUS 2's
     * occlusion volumes look their 8x8x8 cone LUT up this way. */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        check_count("3D TEX FP",
                    rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(fp, sizeof fp, 0x40u,
                        0, 1u /* unit 0 is 3D */, NULL, NULL, NULL, g_hlsl, sizeof g_hlsl, NULL), 1);
        check("3D TEX FP HLSL", g_hlsl, "Texture3D   rsx_tex0 : register(t0);");
        check("3D TEX FP HLSL", g_hlsl, "rsx_tex0.Sample(rsx_samp[0]");
        if (translate("3D TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("3D TEX FP MSL", g_msl, "texture3d");
    }

    /* ---- fragment program: TEX r0, TC0 with EXP_TEX on a biased unit -----
     * The _bx2 fetch: the lanes the unit's biased remap names come back as
     * x*2-1. The helper must survive glslang/spirv-cross intact. */
    {
        u8 fp[16];
        u8 expand[16] = {0};
        expand[0] = 0x7;
        rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX) | RSX_TEST_FP_MASK_XYZW |
                              RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_TEXU(0) | (1u << 21) | RSX_TEST_FP_END,
                              RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        check_count("expand TEX FP", rsx_fp_decompile_buffered_shadow_color_expand_ex(fp, sizeof fp, 0x40u, 0,
                        NULL, NULL, expand, g_hlsl, sizeof g_hlsl, NULL), 1);
        check("expand TEX FP HLSL", g_hlsl, "_rsx_expand_input(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy), 15u, true)");
        if (translate("expand TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("expand TEX FP MSL", g_msl, "* 2.0");
    }

    /* ---- fragment program writing TWO colour targets ---------------------
     * MOV r0, COL0 ; MOV r2, COL0.zyxw (END). With 32-bit exports r0 is
     * target A and r2 is target B, so the decompiler returns a struct of two
     * SV_TARGETs rather than one value. What has to hold on the way out is
     * that spirv-cross keeps them as [[color(0)]] and [[color(1)]] in that
     * order: the backend attaches the guest's MRT set in the set's own order,
     * and a pair that came back renumbered or collapsed would write the two
     * planes to the wrong surfaces with nothing to say so. This is the
     * program the host's --mrt mode draws with. */
    {
        u8 fp[32];
        rsx_test_fp_mrt_col0(fp, RSX_TEST_FP_SWZ_IDENT, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        check_count("MRT FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                           g_hlsl, sizeof g_hlsl, NULL), 2);
        check("MRT FP HLSL", g_hlsl, "float4 t0 : SV_TARGET0;");
        check("MRT FP HLSL", g_hlsl, "float4 t1 : SV_TARGET1;");
        check("MRT FP HLSL", g_hlsl, "PSOutput main(PSInput input) {");
        check("MRT FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
        check("MRT FP HLSL", g_hlsl, "r[2].xyzw = _v.xyzw;");
        check("MRT FP HLSL", g_hlsl, "(saturate(input.col0)).zyxw");

        if (translate("MRT FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("MRT FP MSL", g_msl, "fragment ");
            check("MRT FP MSL", g_msl, "[[color(0)]]");
            check("MRT FP MSL", g_msl, "[[color(1)]]");
        }
    }

    /* ---- depth export: SHADER_CONTROL & 0xE replaces depth with r1.z -----
     * Single target behind the alpha-test wrapper, then the MRT form. */
    {
        u8 fp[16];
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
        check_count("depth FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u | RSX_FP_CTRL_DEPTH_EXPORT, 0,
                                                             g_hlsl, sizeof g_hlsl, NULL), 1);
        const int at = rsx_fp_apply_alpha_test_buffered(g_hlsl, sizeof g_hlsl, 0x204u /* GREATER */);
        const int de = rsx_fp_apply_depth_export(g_hlsl, sizeof g_hlsl);
        const int again = rsx_fp_apply_depth_export(g_hlsl, sizeof g_hlsl);   /* refuses twice */
        if (at == 1 && de == 1 && again == -1) { printf("[PASS] depth FP patches\n"); g_pass++; }
        else { printf("[FAIL] depth FP patches %d %d %d (want 1 1 -1)\n", at, de, again); g_fail++; }
        check("depth FP HLSL", g_hlsl, "float d : SV_Depth;");
        check("depth FP HLSL", g_hlsl, "_rsx_dout.c = _rsx_out;");
        check("depth FP HLSL", g_hlsl, "_rsx_dout.d = saturate(r[1].z);");
        check("depth FP HLSL", g_hlsl, "discard");
        if (translate("depth FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("depth FP MSL", g_msl, "[[depth(any)]]");
            check("depth FP MSL", g_msl, "[[color(0)]]");
        }
    }
    {
        u8 fp[32];
        rsx_test_fp_mrt_col0(fp, RSX_TEST_FP_SWZ_IDENT, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        check_count("depth MRT FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u | RSX_FP_CTRL_DEPTH_EXPORT, 0,
                                                                 g_hlsl, sizeof g_hlsl, NULL), 2);
        if (rsx_fp_apply_depth_export(g_hlsl, sizeof g_hlsl) == 1) { printf("[PASS] depth MRT FP patch\n"); g_pass++; }
        else { printf("[FAIL] depth MRT FP patch\n"); g_fail++; }
        check("depth MRT FP HLSL", g_hlsl, "float _rsx_depth : SV_Depth;");
        check("depth MRT FP HLSL", g_hlsl, "_rsx_dout._rsx_depth = saturate(r[1].z);");
        if (translate("depth MRT FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("depth MRT FP MSL", g_msl, "[[depth(any)]]");
            check("depth MRT FP MSL", g_msl, "[[color(1)]]");
        }
    }

    /* ---- depth bounds (NV4097 0x0380): emulated in the program ----------
     * The stored depth comes from a copy on a unit the program does not
     * sample (15 here); outside [fp_dbounds.x, fp_dbounds.y] the fragment is
     * discarded before anything else runs. Single target behind the
     * alpha-test wrapper, then the MRT form; a cube slot is refused. */
    {
        u8 fp[16];
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
        check_count("dbounds FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0, g_hlsl, sizeof g_hlsl, NULL), 1);
        const int at = rsx_fp_apply_alpha_test_buffered(g_hlsl, sizeof g_hlsl, 0x204u /* GREATER */);
        const int db = rsx_fp_apply_depth_bounds(g_hlsl, sizeof g_hlsl, 15);
        const int again = rsx_fp_apply_depth_bounds(g_hlsl, sizeof g_hlsl, 15);   /* refuses twice */
        if (at == 1 && db == 1 && again == -1) { printf("[PASS] dbounds FP patches\n"); g_pass++; }
        else { printf("[FAIL] dbounds FP patches %d %d %d (want 1 1 -1)\n", at, db, again); g_fail++; }
        check("dbounds FP HLSL", g_hlsl, "    float4 fp_alpha;\n    float4 fp_dbounds;\n};");
        check("dbounds FP HLSL", g_hlsl, "round(rsx_tex[15].Load(int3(int2(input.position.xy), 0)) * 255.0)");
        check("dbounds FP HLSL", g_hlsl, "_rsx_zs > fp_dbounds.y) discard;");
        if (translate("dbounds FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("dbounds FP MSL", g_msl, "rsx_tex [[texture(0)]]");
            check("dbounds FP MSL", g_msl, "discard_fragment()");
            check("dbounds FP MSL", g_msl, "fp_dbounds");
        }
    }
    {
        u8 fp[32];
        rsx_test_fp_mrt_col0(fp, RSX_TEST_FP_SWZ_IDENT, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        check_count("dbounds MRT FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0, g_hlsl, sizeof g_hlsl, NULL), 2);
        if (rsx_fp_apply_depth_bounds(g_hlsl, sizeof g_hlsl, 12) == 1) { printf("[PASS] dbounds MRT FP patch\n"); g_pass++; }
        else { printf("[FAIL] dbounds MRT FP patch\n"); g_fail++; }
        check("dbounds MRT FP HLSL", g_hlsl, "PSOutput main(PSInput input) {\n    { const float4 _rsx_zb = round(rsx_tex[12].Load(");
        if (translate("dbounds MRT FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("dbounds MRT FP MSL", g_msl, "[[color(1)]]");
        }
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
        rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 1u << 3 /* unit 3 a cube */, g_hlsl, sizeof g_hlsl, NULL);
        if (rsx_fp_apply_depth_bounds(g_hlsl, sizeof g_hlsl, 3) == -1) { printf("[PASS] dbounds refuses a cube slot\n"); g_pass++; }
        else { printf("[FAIL] dbounds took a cube slot\n"); g_fail++; }
        if (rsx_fp_apply_depth_bounds(g_hlsl, sizeof g_hlsl, 15) == 1) { printf("[PASS] dbounds beside a cube\n"); g_pass++; }
        else { printf("[FAIL] dbounds beside a cube\n"); g_fail++; }
        check("dbounds cube FP HLSL", g_hlsl, "round(rsx_tex15.Load(int3(int2(input.position.xy), 0)) * 255.0)");
        if (translate("dbounds cube FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("dbounds cube FP MSL", g_msl, "[[texture(15)]]");
    }

    /* ---- WPOS with wide targets (PS3_RES_SCALE x PS3_ASPECT) -----------
     * MOV r0, WPOS: at 1x the position goes through untouched; with x and y
     * scaled apart (21:9 targets at 2x: x 2.625, y 2) each axis divides back
     * by its own factor, and back at 1x the program is byte-identical. */
    {
        u8 fp[16];
        rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                              RSX_TEST_FP_INSRC(0) | RSX_TEST_FP_END,
                              RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        static char base[sizeof g_hlsl];
        check_count("WPOS FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0, g_hlsl, sizeof g_hlsl, NULL), 1);
        check_absent("WPOS FP HLSL", g_hlsl, "_rsx_wpos");
        memcpy(base, g_hlsl, sizeof base);
        rsx_fp_set_wpos_scale_xy(2.625f, 2.0f);
        check_count("WPOS wide FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0, g_hlsl, sizeof g_hlsl, NULL), 1);
        check("WPOS wide FP HLSL", g_hlsl, "float4((p).x * 0.380952381, ((p).y * 0.5) * fp_alpha.y + fp_alpha.z, (p).zw)");
        check("WPOS wide FP HLSL", g_hlsl, "_rsx_wpos(input.position)");
        if (translate("WPOS wide FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("WPOS wide FP MSL", g_msl, "fragment ");
        rsx_fp_set_wpos_scale(1.0f);
        check_count("WPOS FP again", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0, g_hlsl, sizeof g_hlsl, NULL), 1);
        if (!strcmp(base, g_hlsl)) { printf("[PASS] WPOS FP at 1x unchanged\n"); g_pass++; }
        else { printf("[FAIL] WPOS FP at 1x changed\n"); g_fail++; }
    }

    /* ---- framebuffer fetch (rsx_fp_apply_fbfetch): a decal in inFAMOUS 2's
     * shape reads two of its own targets at the projected screen texcoord
     * (units 2 and 3 -> attachments 0 and 1, unit 3 through a crossbar) and
     * one ordinary texture (unit 0, untouched); unit 1 is a candidate but is
     * also sampled at a plain texcoord, so it stays a texture. */
    {
        snprintf(g_hlsl, sizeof g_hlsl, "%s",
            "struct PSInput { float4 position : SV_POSITION; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3; };\n"
            "struct PSOutput { float4 t0 : SV_TARGET0; float4 t1 : SV_TARGET1; };\n"
            "Texture2D    rsx_tex[16] : register(t0);\n"
            "SamplerState rsx_samp[16] : register(s0);\n"
            "float4 _rsx_inv_w(float4 p) { return (float4)(1.0 / p.w); }\n"
            "PSOutput main(PSInput input) {\n"
            "    PSOutput o;\n"
            "    float4 a = rsx_tex[2].Sample(rsx_samp[2], (((input.tc3 * _rsx_inv_w(input.position))).xyzw).xy);\n"
            "    float4 b = rsx_tex[3].Sample(rsx_samp[3], (((input.tc3 * _rsx_inv_w(input.position))).xyzw).xy);\n"
            "    float4 c = rsx_tex[0].Sample(rsx_samp[0], ((input.tc2).xyzw).xy);\n"
            "    float4 d = rsx_tex[1].Sample(rsx_samp[1], (((input.tc3 * _rsx_inv_w(input.position))).xyzw).xy)\n"
            "             + rsx_tex[1].Sample(rsx_samp[1], ((input.tc2).xyzw).xy);\n"
            "    o.t0 = lerp(a, c, 0.5) + d; o.t1 = b;\n"
            "    return o;\n"
            "}\n");
        u8 att[16] = {0}, sel[16][4] = {{0}};
        att[1] = 0; att[2] = 0; att[3] = 1;
        for (int u = 0; u < 16; u++) { sel[u][0] = 3; sel[u][1] = 0; sel[u][2] = 1; sel[u][3] = 2; }
        sel[3][0] = 0; sel[3][1] = 1; sel[3][2] = 2; sel[3][3] = 5;   /* A <- R, R <- G, G <- B, B <- 1 */
        u32 applied = 0;
        const int rc = rsx_fp_apply_fbfetch(g_hlsl, sizeof g_hlsl, 0xEu, att, sel, &applied);
        if (rc == 0 && applied == 0xCu) { printf("[PASS] fbfetch units 0x%X\n", applied); g_pass++; }
        else { printf("[FAIL] fbfetch rc %d units 0x%X (want 0 0xC)\n", rc, applied); g_fail++; }
        check("fbfetch HLSL", g_hlsl, "SubpassInput<float4> _rsx_att0;");
        check("fbfetch HLSL", g_hlsl, "return float4(c.y, c.z, 1.0, c.x);");
        check("fbfetch HLSL", g_hlsl, "float4 a = _rsx_fb2();");
        check("fbfetch HLSL", g_hlsl, "rsx_tex[1].Sample(rsx_samp[1], ((input.tc2).xyzw).xy)");
        check("fbfetch HLSL", g_hlsl, "rsx_tex[0].Sample(rsx_samp[0]");
        if (translate("fbfetch FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("fbfetch MSL", g_msl, "[[color(0)]]");
            check("fbfetch MSL", g_msl, "_rsx_att1 [[color(1)]]");
        }
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
