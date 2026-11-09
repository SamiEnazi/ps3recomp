/*
 * ps3recomp - GLSL for the Vulkan backend's own full-screen passes
 *
 * Ports of the Metal backend's helper MSL (rsx_metal_backend.m, kEngHelperMSL)
 * for the draw-engine hooks light_pass, ssao_pass and depth_snapshot_rgba8.
 * The arithmetic is the MSL's, statement for statement; what changes is only
 * the spelling:
 *   texture.read(uint2)           texelFetch(sampler2D, ivec2, 0)
 *   [[position]]                  gl_FragCoord (Vulkan: upper-left origin,
 *                                 pixel centres at .5 -- Metal's convention)
 *   saturate(x)                   clamp(x, 0.0, 1.0)
 *   rint(x)                       roundEven(x)
 *   round(x), x >= 0              floor(x + 0.5)  (MSL rounds half away from
 *                                 zero; GLSL round() leaves .5 to the driver)
 *   as_type<T>(x)                 floatBitsToUint / uintBitsToFloat
 *   float X[12] in `constant`     vec4 X4[3] (std140 strides a float array at
 *                                 16 bytes; MSL at 4, which is the C layout)
 *   ulong in ssao_recip           u32 arithmetic on the same 33-bit value
 * Every pass is a full-screen triangle that reads its inputs at its own pixel,
 * so neither stage needs a Y flip: output pixel (x, y) comes from input texel
 * (x, y), row 0 the first row in memory, in both APIs.
 *
 * The SSAO helpers reproduce the SPU's reciprocal estimate and rounding with
 * fused multiply-adds whose rounding error they test (ssao_mul_rz, ssao_lin);
 * those values are `precise` (SPIR-V NoContraction) so the driver may neither
 * fuse the separate multiplies into neighbouring adds nor reassociate. The
 * light pass is ordinary float math and has no such qualifiers, like its MSL.
 *
 * [res] PS3RECOMP_RENDER_SCALE (rsx_vulkan_backend.c's [res] block): the
 * MSL's `rs` scale is the block's `res.xy` (1 unscaled -- the old constant),
 * ssao_up4s_fs (the upsample at the scaled size) is kVkSsaoUpScFS, and the
 * full-res AO chain (PS3RECOMP_SSAO >= 1) runs at the scaled size with its
 * sample offsets and clamps scaled to match.
 */
#ifndef PS3RECOMP_RSX_VK_PASSES_GLSL_H
#define PS3RECOMP_RSX_VK_PASSES_GLSL_H

/* eng_fullscreen_vs: one triangle over the whole target, no vertex input. */
static const char kVkPassVS[] =
"#version 450\n"
"void main() {\n"
"    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
"    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

/* eng_light_fs (rsx_metal_backend.m, after "The engine's GPU light pass"):
 * inFAMOUS 2's SPU light job. Set 0: binding 0 the rsx_gpu_light_params block
 * (its C layout is std140-compatible: vec4 groups, 64-byte light records),
 * binding 1 the normals copy (RGBA8), binding 2 the resolved depth (R32F).
 * Two RGBA8 outputs: diffuse (location 0) and specular (location 1).
 *
 * VK_LIGHT_GLSL(QDEF): QDEF defines LQ(x, n), the step that turns a value in
 * 0..255 into the byte written. kVkLightFS truncates, as the SPU job does
 * (floor; n unused). kVkLightDitherFS (PS3RECOMP_LIGHT_DITHER, ps3_lightfx.h)
 * adds a static per-pixel dither n in 0..1 (interleaved gradient noise,
 * Jimenez 2014) before the truncation, less half a step: the mean stays the
 * truncation's, the 8-bit steps of a soft light falloff become fine noise.
 * A pixel with no light stays exactly 0. The scale byte (alpha) is never
 * dithered, so a pixel's colour ratios stay the job's. */
#define VK_LIGHT_GLSL(QDEF) \
"#version 450\n" \
QDEF \
"float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n" \
"struct GLight { vec4 pos_type; vec4 col_inner; vec4 oacbn; vec4 dir; };\n" \
"layout(std140, set = 0, binding = 0) uniform GLParams { vec4 m[4]; vec4 misc; GLight l[512]; } p;\n" \
"layout(set = 0, binding = 1) uniform sampler2D nt;\n" \
"layout(set = 0, binding = 2) uniform sampler2D zt;\n" \
"layout(location = 0) out vec4 o_diff;\n" \
"layout(location = 1) out vec4 o_spec;\n" \
"void main() {\n" \
"    uvec2 px = uvec2(gl_FragCoord.xy);\n" \
"    o_diff = vec4(0.0); o_spec = vec4(0.0);\n" \
"    uint z24 = uint(clamp(texelFetch(zt, ivec2(px), 0).x, 0.0, 1.0) * 16777215.0);\n" \
"    if (z24 >= 0xFFFFFFu) return;\n" \
"    float zf = float(z24) / 16777216.0;\n" \
"    float u = float(px.x) / p.misc.z, v = float(px.y) / p.misc.w;\n" \
"    vec4 h = u * p.m[0] + v * p.m[1] + zf * p.m[2] + p.m[3];\n" \
"    vec3 P = h.xyz / h.w;\n" \
"    vec4 nr = floor(texelFetch(nt, ivec2(px), 0) * 255.0 + 0.5);\n" \
"    vec3 N = normalize(nr.rgb * (2.0 / 255.0) - 1.0);\n" \
"    float s = exp2(nr.a * (8.0 / 255.0) - 0.88896);\n" \
"    vec3 V = -normalize(P);\n" \
"    vec3 acc = vec3(0.0); float sp = 0.0;\n" \
"    int n = min(int(p.misc.y), 512);\n" \
"    for (int k = 0; k < n; k++) {\n" \
"        GLight L = p.l[k];\n" \
"        vec3 Lv = L.pos_type.xyz - P; float d = length(Lv); vec3 l = Lv / d;\n" \
"        float ndl = max(dot(N, l), 0.0);\n" \
"        float att = clamp((L.oacbn.x - d) / (L.oacbn.x - L.col_inner.w), 0.0, 1.0); att *= att;\n" \
"        if (L.pos_type.w > 1.5) {\n" \
"            float c = clamp((dot(-l, L.dir.xyz) - L.oacbn.z) * -L.oacbn.y, 0.0, 1.0);\n" \
"            att *= c * c * (d > L.oacbn.w ? 1.0 : 0.0);\n" \
"        }\n" \
"        float w = att * ndl;\n" \
"        if (w <= 0.0) continue;\n" \
"        vec3 C = L.col_inner.xyz * p.misc.x;\n" \
"        float ndh = max(dot(N, normalize(l + V)), 0.0);\n" \
"        acc += w * C;\n" \
"        sp += w * length(C) * (1.0 + 25.0 / 27.0 * s) * pow(1.0 + s * (1.0 - ndh), -8.0);\n" \
"    }\n" \
"    const float K = 0.35355339 * 255.0;\n" \
"    vec3 q = sqrt(acc);\n" \
"    float a = min(255.0, floor(max(1.0, max(q.r, max(q.g, q.b))) * K));\n" \
"    float dn = ign(gl_FragCoord.xy);\n" \
"    o_diff = vec4(min(vec3(255.0), LQ(q * K * 255.0 / a, dn)), a) / 255.0;\n" \
"    o_spec = vec4(0.0, 0.0, 0.0, min(255.0, LQ(sqrt(sp) * K, fract(dn + 0.5))) / 255.0);\n" \
"}\n"
static const char kVkLightFS[] = VK_LIGHT_GLSL("#define LQ(x, n) floor(x)\n");
static const char kVkLightDitherFS[] = VK_LIGHT_GLSL("#define LQ(x, n) max(floor((x) + (n) - 0.5), 0.0)\n");

/* The SSAO passes' shared part: struct SsaoC (SsaoGpuConsts, 320 bytes) as a
 * std140 block, and the SPU-exact helpers. Set 0: binding 0 the block,
 * bindings 1..3 the pass's textures t0..t2 (MSL [[texture(0..2)]]). */
#define VK_SSAO_GLSL_COMMON \
"#version 450\n" \
"layout(std140, set = 0, binding = 0) uniform SsaoC {\n" \
"    vec4 depth_ab;\n" \
"    vec4 X4[3]; vec4 Y4[3]; vec4 Z4[3];\n" \
"    vec4 M[3];\n" \
"    vec4 C0, D0, E0, F0;\n" \
"    vec4 blur;\n" \
"    vec4 up;\n" \
"    uvec4 dims;\n" \
"    vec4 lfx;\n" /* ps3_lightfx: x = AO strength (1 = the game's); never read by the exact passes */ \
"    vec4 res;\n" /* [res] xy = the scale of the depth / normals read (1 unscaled) */ \
"} c;\n" \
"layout(set = 0, binding = 1) uniform sampler2D t0;\n" \
"layout(set = 0, binding = 2) uniform sampler2D t1;\n" \
"layout(set = 0, binding = 3) uniform sampler2D t2;\n" \
"#define rs (c.res.xy)\n" \
"float cX(int k) { return c.X4[k >> 2][k & 3]; }\n" \
"float cY(int k) { return c.Y4[k >> 2][k & 3]; }\n" \
"float cZ(int k) { return c.Z4[k >> 2][k & 3]; }\n" \
"const uint ssao_frest_lut[32] = uint[32](\n" \
"    0x7FFBE0u, 0x7F87A6u, 0x70EF72u, 0x708B40u, 0x638B12u, 0x633AEAu, 0x5792C4u, 0x574AA0u,\n" \
"    0x4CCA7Eu, 0x4C9262u, 0x430A44u, 0x42D62Au, 0x3A2E12u, 0x39FDFAu, 0x3215E4u, 0x31F1D2u,\n" \
"    0x2AA9BEu, 0x2A85ACu, 0x23D59Au, 0x23BD8Eu, 0x1D8576u, 0x1D8576u, 0x17AD5Au, 0x17AD5Au,\n" \
"    0x124543u, 0x124543u, 0x0D392Du, 0x0D392Du, 0x08851Au, 0x08851Au, 0x041D07u, 0x041D07u);\n" \
/* The MSL's m = ((8192 | base) << 19) - step * y is a 33-bit value: \
 * 2^32 + L - P with L = base << 19 and P = step * y (< 2^29). It is below \
 * 2^32 exactly when L < P, and then u32 wrap-around gives it exactly; the \
 * MSL's (m >> 9) or, renormalised, ((m << 1) >> 9) = m >> 8, masked to 23 \
 * bits, never sees bit 32. */ \
"float ssao_recip(float x) {\n" \
"    uint b = floatBitsToUint(x);\n" \
"    uint sgn = b & 0x80000000u, e = (b >> 23) & 0xFFu;\n" \
"    uint re = (e == 0u) ? 255u : (e >= 253u ? 0u : 253u - e);\n" \
"    uint est = sgn | (re << 23) | (ssao_frest_lut[(b >> 18) & 31u] & 0x7FFFFFu);\n" \
"    int ee = int((est >> 23) & 0xFFu);\n" \
"    if (ee == 0) return 0.0;\n" \
"    uint base = (est >> 10) & 0x1FFFu, stp = est & 0x3FFu, y = b & 0x7FFFFu;\n" \
"    uint L = base << 19, P = stp * y, d = L - P;\n" \
"    uint mant;\n" \
"    if (L < P) { mant = (d >> 8) & 0x7FFFFFu; if (--ee == 0) return 0.0; }\n" \
"    else       { mant = (d >> 9) & 0x7FFFFFu; }\n" \
"    return uintBitsToFloat(sgn | (uint(ee) << 23) | mant);\n" \
"}\n" \
"float ssao_mul_rz(float a, float b) {\n" \
"    precise float p = a * b;\n" \
"    precise float r = fma(a, b, -p);\n" \
"    return (r < 0.0 && p > 0.0) ? uintBitsToFloat(floatBitsToUint(p) - 1u) : p;\n" \
"}\n" \
"float ssao_sat(float x) { return x >= 1.0 ? uintBitsToFloat(0x3F7FFFFFu) : max(x, 0.0); }\n" \
"float ssao_z24(float d01) { return float(uint(clamp(d01, 0.0, 1.0) * 16777215.0)); }\n" \
"float ssao_lin(float z24, float A, float B) {\n" \
"    float c1 = uintBitsToFloat(0x33800001u);\n" \
"    precise float k = A * c1;\n" \
"    precise float pe = fma(A, c1, -k);\n" /* round k toward zero like fm */ \
"    if ((pe > 0.0 && k < 0.0) || (pe < 0.0 && k > 0.0)) k = uintBitsToFloat(floatBitsToUint(k) - 1u);\n" \
"    precise float v = fma(z24, k, B);\n" \
"    float r = ssao_recip(v);\n" \
"    precise float res = fma(fma(-v, r, 1.0), r, r);\n" \
"    return res;\n" \
"}\n" \
"float ssao_byte(sampler2D t, ivec2 p) { return roundEven(texelFetch(t, p, 0).r * 255.0); }\n" \
"uvec2 ssao_sc(ivec2 p, vec2 s) { return uvec2((vec2(p) + 0.5) * s); }\n"

/* ssao_hd_fs: half-res linear depth. t0 = resolved depth. R32F out. */
static const char kVkSsaoHdFS[] = VK_SSAO_GLSL_COMMON
"layout(location = 0) out float o;\n"
"void main() {\n"
"    uvec2 p = ssao_sc(ivec2(gl_FragCoord.xy) * 2, rs);\n"
"    o = ssao_lin(ssao_z24(texelFetch(t0, ivec2(p), 0).r), c.depth_ab.x, c.depth_ab.y);\n"
"}\n";

/* ssao_ao_fs: 12-sample AO. t0 = half depth, t1 = normals copy. R8 out. */
static const char kVkSsaoAoFS[] = VK_SSAO_GLSL_COMMON
"layout(location = 0) out float o;\n"
"void main() {\n"
"    ivec2 p = ivec2(gl_FragCoord.xy);\n"
"    ivec2 hm = ivec2(c.dims.zw) - 1;\n"
"    float d   = texelFetch(t0, p, 0).r;\n"
"    float dc  = clamp(d, c.E0.z, c.E0.w);\n"
"    float rcp = ssao_recip(dc);\n"
"    precise float q = d * rcp;\n"
"    vec3 nb = roundEven(texelFetch(t1, ivec2(ssao_sc(p * 2, rs)), 0).rgb * 255.0);\n"
"    precise vec3 n  = fma(nb * (1.0 / 256.0), vec3(2.0078430175781), vec3(-1.0));\n"
"    precise vec3 n2 = fma(vec3(n.z), c.M[2].xyz, fma(vec3(n.y), c.M[1].xyz, n.x * c.M[0].xyz));\n"
"    precise float dref = fma(n2.z, q, d);\n"
"    precise float bx = fma(n2.x, rcp, 0.5), by = fma(n2.y, rcp, 0.5);\n"
"    precise float sx = c.D0.y * rcp, sy = c.D0.z * rcp;\n"
"    precise float hz = c.D0.w * q;\n"
"    precise float e0 = c.E0.x * ssao_recip(q), e1 = c.E0.y * q;\n"
"    precise float acc = 0.0;\n"
"    for (int k = 0; k < 12; k++) {\n"
"        precise float fx = fma(cX(k), sx, bx), fy = fma(cY(k), sy, by);\n"
"        int tx = p.x + int(floor(fx));\n"
"        int ty = p.y + int(floor(fy));\n"
"        float ds = texelFetch(t0, clamp(ivec2(tx, ty), ivec2(0), hm), 0).r;\n"
"        precise float h = cZ(k) * hz;\n"
"        precise float t = (ds - (dref - h)) * 0.5;\n"
"        acc += fma(ssao_sat(fma(t, e0, e1)), h, clamp(t, 0.0, h));\n"
"    }\n"
"    acc *= ssao_recip(c.D0.x * q);\n"
"    precise float vv = fma(acc, c.F0.x, c.F0.y);\n"
"    float v = ssao_sat(vv);\n"
"    o = floor(ssao_mul_rz(v, 255.0 / 256.0) * 256.0) / 255.0;\n"   /* <= 254 */
"}\n";

/* ssao_blur: the depth-aware 5-tap blur, along `dir`. t0 = AO, t1 = half
 * depth. R8 out. */
#define VK_SSAO_GLSL_BLUR \
"float ssao_blur(ivec2 p, ivec2 dir) {\n" \
"    ivec2 hm = ivec2(c.dims.zw) - 1;\n" \
"    float d0 = texelFetch(t1, p, 0).r;\n" \
"    precise float kk = c.blur.x * ssao_recip(d0);\n" \
"    float w[5], a[5];\n" \
"    for (int o = -2; o <= 2; o++) {\n" \
"        ivec2 s = clamp(p + o * dir, ivec2(0), hm);\n" \
"        a[o + 2] = ssao_byte(t0, s);\n" \
"        precise float wo = max(0.0, fma(-abs(texelFetch(t1, s, 0).r - d0), kk, 1.0)) * ((o == -2 || o == 2) ? c.blur.y : c.blur.z);\n" \
"        w[o + 2] = wo;\n" \
"    }\n" \
"    precise float num = fma(w[4], a[4], fma(w[3], a[3], fma(w[1], a[1], fma(w[0], a[0], a[2]))));\n" \
"    precise float den = w[4] + (w[3] + (1.0 + (w[0] + w[1])));\n" \
"    return floor(ssao_mul_rz(num, ssao_recip(den))) / 255.0;\n" \
"}\n"

/* ssao_blur_h_fs / ssao_blur_v_fs */
static const char kVkSsaoBlurHFS[] = VK_SSAO_GLSL_COMMON VK_SSAO_GLSL_BLUR
"layout(location = 0) out float o;\n"
"void main() { o = ssao_blur(ivec2(gl_FragCoord.xy), ivec2(1, 0)); }\n";
static const char kVkSsaoBlurVFS[] = VK_SSAO_GLSL_COMMON VK_SSAO_GLSL_BLUR
"layout(location = 0) out float o;\n"
"void main() { o = ssao_blur(ivec2(gl_FragCoord.xy), ivec2(0, 1)); }\n";

/* ssao_up4_fs: the bilateral 2x upsample, the AO byte in every channel.
 * t0 = AO, t1 = half depth, t2 = resolved depth. RGBA8 out.
 * VK_SSAO_UP4(OUT): OUT(b) is the byte written (0..254); kVkSsaoUp4FS writes
 * it as computed, kVkSsaoUp4sFS (PS3RECOMP_SSAO_STRENGTH != 100) scales the
 * occlusion: 254 - (254 - b) * strength, so open ground stays open. */
#define VK_SSAO_UP4(OUT) VK_SSAO_GLSL_COMMON \
"#define UPOUT(b) " OUT "\n" \
"layout(location = 0) out vec4 o;\n" \
"void main() {\n" \
"    ivec2 P  = ivec2(gl_FragCoord.xy);\n" \
"    ivec2 hm = ivec2(c.dims.zw) - 1;\n" \
"    float Z  = ssao_lin(ssao_z24(texelFetch(t2, ivec2(ssao_sc(P, rs)), 0).r), c.up.x, c.up.y);\n" \
"    precise float kz = c.up.z * ssao_recip(Z);\n" \
"    ivec2 b = P >> 1;\n" \
"    ivec2 s = ivec2((P.x & 1) != 0 ? 1 : -1, (P.y & 1) != 0 ? 1 : -1);\n" \
"    ivec2 off[4] = ivec2[4](ivec2(0, 0), ivec2(0, s.y), ivec2(s.x, 0), s);\n" \
"    float bw[4]  = float[4](9.0, 3.0, 3.0, 1.0);\n" \
"    float eps[4] = float[4](uintBitsToFloat(0x3A6BEDFAu), uintBitsToFloat(0x399D4952u),\n" \
"                            uintBitsToFloat(0x399D4952u), uintBitsToFloat(0x38D1B717u));\n" \
"    precise float num = 0.0, den = 0.0;\n" \
"    for (int k = 0; k < 4; k++) {\n" \
"        ivec2 t = clamp(b + off[k], ivec2(0), hm);\n" \
"        precise float w = fma(max(0.0, fma(-abs(texelFetch(t1, t, 0).r - Z), kz, 1.0)), bw[k], eps[k]);\n" \
"        num = fma(w, ssao_byte(t0, t), num);\n" \
"        den += w;\n" \
"    }\n" \
"    o = vec4(UPOUT(floor(ssao_mul_rz(num, ssao_recip(den)))) / 255.0);\n" \
"}\n"
static const char kVkSsaoUp4FS[] = VK_SSAO_UP4("(b)");
static const char kVkSsaoUp4sFS[] = VK_SSAO_UP4("clamp(254.0 - (254.0 - (b)) * c.lfx.x, 0.0, 254.0)");

/* [res] Metal's ssao_up4s_fs: the same upsample run per SCALED pixel (the
 * output is the size of the scaled targets). Each pixel takes its own depth
 * and the half-res AO texel nearest to its centre plus the neighbours on its
 * side; at rs = 1 exactly ssao_up4_fs's b = P >> 1 and parity sign. The
 * job-size output stretched over the scaled image had its depth-aware edges
 * in 720p steps; this one follows the scaled depth. kVkSsaoUpScsFS: with the
 * strength, as kVkSsaoUp4sFS. */
#define VK_SSAO_UP4SC(OUT) VK_SSAO_GLSL_COMMON \
"#define UPOUT(b) " OUT "\n" \
"layout(location = 0) out vec4 o;\n" \
"void main() {\n" \
"    ivec2 P  = ivec2(gl_FragCoord.xy);\n" \
"    ivec2 hm = ivec2(c.dims.zw) - 1;\n" \
"    float Z  = ssao_lin(ssao_z24(texelFetch(t2, P, 0).r), c.up.x, c.up.y);\n" \
"    precise float kz = c.up.z * ssao_recip(Z);\n" \
"    vec2 h = (vec2(P) + 0.5) / (2.0 * rs) - 0.5;\n" \
"    ivec2 b = ivec2(floor(h + 0.5));\n" \
"    ivec2 s = ivec2(h.x >= float(b.x) ? 1 : -1, h.y >= float(b.y) ? 1 : -1);\n" \
"    ivec2 off[4] = ivec2[4](ivec2(0, 0), ivec2(0, s.y), ivec2(s.x, 0), s);\n" \
"    float bw[4]  = float[4](9.0, 3.0, 3.0, 1.0);\n" \
"    float eps[4] = float[4](uintBitsToFloat(0x3A6BEDFAu), uintBitsToFloat(0x399D4952u),\n" \
"                            uintBitsToFloat(0x399D4952u), uintBitsToFloat(0x38D1B717u));\n" \
"    precise float num = 0.0, den = 0.0;\n" \
"    for (int k = 0; k < 4; k++) {\n" \
"        ivec2 t = clamp(b + off[k], ivec2(0), hm);\n" \
"        precise float w = fma(max(0.0, fma(-abs(texelFetch(t1, t, 0).r - Z), kz, 1.0)), bw[k], eps[k]);\n" \
"        num = fma(w, ssao_byte(t0, t), num);\n" \
"        den += w;\n" \
"    }\n" \
"    o = vec4(UPOUT(floor(ssao_mul_rz(num, ssao_recip(den)))) / 255.0);\n" \
"}\n"
static const char kVkSsaoUpScFS[]  = VK_SSAO_UP4SC("(b)");
static const char kVkSsaoUpScsFS[] = VK_SSAO_UP4SC("clamp(254.0 - (254.0 - (b)) * c.lfx.x, 0.0, 254.0)");


/* ---- PS3RECOMP_SSAO=1|2 (ps3_lightfx.h): the game's occlusion at full
 * resolution. Plain float math (no SPU emulation: this is not the job's
 * output any more), same formula and constants as ssao_ao_fs, with:
 *   - every 1280x720 pixel computed (the game: one per 2x2 block, then a
 *     bilateral upsample), sample offsets in full-res pixels (x2);
 *   - the 12-sample pattern rotated per pixel (4x4 Bayer angles), LFX_SETS
 *     rotations per pixel (1: 12 samples, 2: 24);
 *   - a 9-tap separable depth-aware blur at full res (the game's 5 taps at
 *     half res, its depth weight), which also removes the 4x4 rotation
 *     pattern; the second pass writes the RGBA8 result with the strength.
 * Passes: hd (R32F fw x fh) <- depth; a <- (hd, normals); b <- blur_h(a, hd);
 * out <- blur_v(b, hd). Credits: the occlusion formula is the game's
 * (decoded in notes/ssao of the Mac port); rotation + blur are the standard
 * interleaved-sampling recipe (Keller & Heidrich 2001). */
static const char kVkLfxHdFS[] = VK_SSAO_GLSL_COMMON
"layout(location = 0) out float o;\n"
"void main() {\n"
"    o = ssao_lin(ssao_z24(texelFetch(t0, ivec2(gl_FragCoord.xy), 0).r), c.depth_ab.x, c.depth_ab.y);\n"
"}\n";

#define VK_LFX_AO(SETS) VK_SSAO_GLSL_COMMON \
"layout(location = 0) out float o;\n" \
"const float bayer4[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);\n" \
"void main() {\n" \
"    ivec2 p = ivec2(gl_FragCoord.xy);\n" \
"    ivec2 fm = ivec2(floor(vec2(c.dims.xy) * rs + 0.5)) - 1;\n" /* [res] the scaled full size */ \
"    float d = texelFetch(t0, p, 0).r;\n" \
"    float dc = clamp(d, c.E0.z, c.E0.w);\n" \
"    float rcp = 1.0 / dc;\n" \
"    float q = d * rcp;\n" \
"    vec3 n = roundEven(texelFetch(t1, p, 0).rgb * 255.0) * (1.0 / 127.5) - 1.0;\n" \
"    vec3 n2 = n.x * c.M[0].xyz + n.y * c.M[1].xyz + n.z * c.M[2].xyz;\n" \
"    float dref = d + n2.z * q;\n" \
"    vec2 b = 2.0 * rs * n2.xy * rcp + 0.5;\n" /* [res] offsets in scaled pixels */ \
"    vec2 sc = 2.0 * rs * c.D0.yz * rcp;\n" \
"    float hz = c.D0.w * q;\n" \
"    float e0 = c.E0.x / q, e1 = c.E0.y * q;\n" \
"    float a0 = (bayer4[(p.y & 3) * 4 + (p.x & 3)] + 0.5) * (6.2831853 / 16.0);\n" \
"    float acc = 0.0;\n" \
"    for (int r = 0; r < " #SETS "; r++) {\n" \
"        float a = a0 + float(r) * 1.1999816;\n" \
"        vec2 cs = vec2(cos(a), sin(a));\n" \
"        for (int k = 0; k < 12; k++) {\n" \
"            vec2 xy = vec2(cX(k) * cs.x - cY(k) * cs.y, cX(k) * cs.y + cY(k) * cs.x);\n" \
"            ivec2 t = clamp(p + ivec2(floor(xy * sc + b)), ivec2(0), fm);\n" \
"            float ds = texelFetch(t0, t, 0).r;\n" \
"            float h = cZ(k) * hz;\n" \
"            float tt = (ds - (dref - h)) * 0.5;\n" \
"            acc += clamp(tt, 0.0, h) + h * clamp(tt * e0 + e1, 0.0, 1.0);\n" \
"        }\n" \
"    }\n" \
"    acc /= c.D0.x * q * float(" #SETS ");\n" \
"    o = clamp(acc * c.F0.x + c.F0.y, 0.0, 1.0);\n" \
"}\n"
static const char kVkLfxAo1FS[] = VK_LFX_AO(1);
static const char kVkLfxAo2FS[] = VK_LFX_AO(2);

/* The 9-tap blur: t0 = AO, t1 = full-res linear depth. Gaussian (sigma ~2.4
 * px) times the game's depth weight 1 - |dz| * k / z (k = its blur.x). */
#define VK_LFX_BLUR \
"float lfx_blur(ivec2 p, ivec2 dir) {\n" \
"    ivec2 fm = ivec2(floor(vec2(c.dims.xy) * rs + 0.5)) - 1;\n" /* [res] */ \
"    float d0 = texelFetch(t1, p, 0).r;\n" \
"    float kk = c.blur.x / d0;\n" \
"    float num = 0.0, den = 0.0;\n" \
"    for (int k = -4; k <= 4; k++) {\n" \
"        ivec2 s = clamp(p + k * dir, ivec2(0), fm);\n" \
"        float w = exp2(-0.125 * float(k * k)) * max(0.0, 1.0 - abs(texelFetch(t1, s, 0).r - d0) * kk);\n" \
"        num += w * texelFetch(t0, s, 0).r;\n" \
"        den += w;\n" \
"    }\n" \
"    return num / den;\n" \
"}\n"
static const char kVkLfxBlurHFS[] = VK_SSAO_GLSL_COMMON VK_LFX_BLUR
"layout(location = 0) out float o;\n"
"void main() { o = lfx_blur(ivec2(gl_FragCoord.xy), ivec2(1, 0)); }\n";
/* ... and the vertical one writes the composite's RGBA8 texture: the AO in
 * every channel, open = 254/255 as the game's, occlusion x strength. */
static const char kVkLfxBlurVFS[] = VK_SSAO_GLSL_COMMON VK_LFX_BLUR
"layout(location = 0) out vec4 o;\n"
"void main() {\n"
"    float v = lfx_blur(ivec2(gl_FragCoord.xy), ivec2(0, 1));\n"
"    o = vec4(clamp(1.0 - (1.0 - v) * c.lfx.x, 0.0, 1.0) * (254.0 / 255.0));\n"
"}\n";

/* ---- PS3RECOMP_TONE_* / PS3RECOMP_CLARITY (ps3_lightfx.h): the Metal
 * backend's present-pass clarity and tone curve (rsx_metal_backend.m,
 * eng_present_fs), ported to a pass that writes the frame instead of the
 * display-surface blit (vk_lfx_tone). Same arithmetic; the clarity taps are
 * texel fetches at rounded offsets (Metal: bilinear samples). Binding 0:
 * c = (shadows, midtones, highlights, clarity) in -1..1, sz = (w, h, 0, 0);
 * binding 1 the game's finished frame. RGBA8 out, alpha kept. */
static const char kVkToneFS[] =
"#version 450\n"
"layout(std140, set = 0, binding = 0) uniform ToneP { vec4 c; vec4 sz; } p;\n"
"layout(set = 0, binding = 1) uniform sampler2D src;\n"
"layout(location = 0) out vec4 o;\n"
"const vec3 W = vec3(0.2126, 0.7152, 0.0722);\n"
"float tl(ivec2 q) { return dot(clamp(texelFetch(src, clamp(q, ivec2(0), ivec2(p.sz.xy) - 1), 0).rgb, 0.0, 1.0), W); }\n"
"void main() {\n"
"    ivec2 P = ivec2(gl_FragCoord.xy);\n"
"    vec4 c = texelFetch(src, P, 0);\n"
"    if (p.c.w != 0.0) {\n"
"        float l0 = dot(clamp(c.rgb, 0.0, 1.0), W), acc = l0 * 0.5, wsum = 0.5;\n"
"        float r = 0.011 * p.sz.y;\n"
"        for (int k = 0; k < 8; k++) {\n"
"            float a = float(k) * 0.785398 + 0.392699;\n"
"            vec2 d = vec2(cos(a), sin(a)) * r;\n"
"            acc += tl(P + ivec2(round(d))) * 0.6;\n"
"            acc += tl(P + ivec2(round(d * 0.5)));\n"
"            wsum += 1.6;\n"
"        }\n"
"        float det = clamp(l0 - acc / wsum, -0.15, 0.15);\n"
"        float mid = clamp(1.0 - abs(l0 * 2.0 - 1.0), 0.0, 1.0);\n"
"        c.rgb = clamp(c.rgb + det * p.c.w * 1.25 * (0.35 + 0.65 * mid), 0.0, 1.0);\n"
"    }\n"
"    if (p.c.x != 0.0 || p.c.y != 0.0 || p.c.z != 0.0) {\n"
"        vec3 x = clamp(c.rgb, 0.0, 1.0), y = 1.0 - x;\n"
"        vec3 ws = 9.4815 * x * y * y * y;\n"
"        vec3 wm = 16.0 * x * x * y * y;\n"
"        vec3 wh = 9.4815 * x * x * x * y;\n"
"        c.rgb = clamp(x + 0.1 * (p.c.x * ws + p.c.y * wm + p.c.z * wh), 0.0, 1.0);\n"
"    }\n"
"    o = c;\n"
"}\n";

/* eng_depth_rgba8_fs: Z24S8 read through A8R8G8B8 (RPCS3 decode_depth24):
 * a = Z[23:16], r = Z[15:8], g = Z[7:0], b = stencil. t0 = resolved depth
 * (R32F, read at this pixel: a target smaller than the depth buffer is its
 * top-left corner). The Metal version reads b from the stencil aspect; this
 * backend's depth formats (D32_SFLOAT, X8_D24, D16) have none -- its clears
 * and draws keep no stencil either -- so b is the 0 such a buffer holds.
 * RGBA8 out. */
static const char kVkDepthRgba8FS[] =
"#version 450\n"
"layout(set = 0, binding = 1) uniform sampler2D zt;\n"
"layout(set = 0, binding = 2) uniform sampler2D st;   /* the stencil byte (R8), or the black stand-in */\n"
"layout(location = 0) out vec4 o;\n"
"void main() {\n"
"    ivec2 px = ivec2(gl_FragCoord.xy);\n"
"    uint v = uint(clamp(texelFetch(zt, px, 0).r, 0.0, 1.0) * 16777215.0);\n"
"    uint sv = uint(texelFetch(st, px, 0).r * 255.0 + 0.5);\n"
"    o = vec4(float((v >> 8) & 255u), float(v & 255u), float(sv & 255u), float((v >> 16) & 255u)) / 255.0;\n"
"}\n";

/* [res] PS3RECOMP_VK_ZDIRECT (default on): the depth target itself sampled
 * through a depth-aspect view (and its stencil through a stencil-aspect
 * view), instead of copying it through a buffer into the R32F / R8 scratch
 * first -- the same values (a D32 depth reads back as the float the copy
 * held, the stencil as the byte), no copies. kVkDepthRgba8DFS is
 * kVkDepthRgba8FS reading them; kVkDepthRgba8DnFS has no stencil aspect
 * (b = 0, as the R8 stand-in held); kVkDepthR32FS is depth_snapshot's copy as
 * a pass into an R32F target. t0 (binding 1) the depth view, t1 (binding 2)
 * the stencil view. */
static const char kVkDepthRgba8DFS[] =
"#version 450\n"
"layout(set = 0, binding = 1) uniform sampler2D zt;\n"
"layout(set = 0, binding = 2) uniform usampler2D st;\n"
"layout(location = 0) out vec4 o;\n"
"void main() {\n"
"    ivec2 px = ivec2(gl_FragCoord.xy);\n"
"    uint v = uint(clamp(texelFetch(zt, px, 0).r, 0.0, 1.0) * 16777215.0);\n"
"    uint sv = texelFetch(st, px, 0).r & 255u;\n"
"    o = vec4(float((v >> 8) & 255u), float(v & 255u), float(sv), float((v >> 16) & 255u)) / 255.0;\n"
"}\n";
static const char kVkDepthRgba8DnFS[] =
"#version 450\n"
"layout(set = 0, binding = 1) uniform sampler2D zt;\n"
"layout(location = 0) out vec4 o;\n"
"void main() {\n"
"    ivec2 px = ivec2(gl_FragCoord.xy);\n"
"    uint v = uint(clamp(texelFetch(zt, px, 0).r, 0.0, 1.0) * 16777215.0);\n"
"    o = vec4(float((v >> 8) & 255u), float(v & 255u), 0.0, float((v >> 16) & 255u)) / 255.0;\n"
"}\n";
static const char kVkDepthR32FS[] =
"#version 450\n"
"layout(set = 0, binding = 1) uniform sampler2D zt;\n"
"layout(location = 0) out float o;\n"
"void main() { o = texelFetch(zt, ivec2(gl_FragCoord.xy), 0).r; }\n";

#endif /* PS3RECOMP_RSX_VK_PASSES_GLSL_H */
