/*
 * rsx_button_icons -- see rsx_button_icons.h.
 *
 * The atlas (inFAMOUS 2, NPUA80638/BCUS98125): loc 0 offset 0x0BADA600,
 * format 0x85 (A8R8G8B8, swizzled), 512x256, one level, uploaded at boot and
 * never rewritten. Straight alpha; the glyphs are near-white (RGB ~240)
 * shapes, the symbol cut out of them, tinted by the text colour when drawn.
 * The boxes below are the glyphs' pixel extents in it; each redraw clears its
 * box (plus one pixel, which every neighbour leaves free) and draws the new
 * glyph inside the old extent, so whatever UV rectangle the font uses keeps
 * framing it.
 */
#include "rsx_button_icons.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"   /* the parser half of the header */
#endif
#include "../input/pad_input_map.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "../../include/ps3emu/ps3_toggles.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../include/ps3emu/ps3_pad_icons.h"   /* cellPad's side: last device, bindings */

/* ---- the atlas ------------------------------------------------------------ */

#define ATLAS_W 512u
#define ATLAS_H 256u

enum { K_FACE, K_SHOULDER, K_STICK_CLICK, K_STICK, K_DPAD_DIR, K_DPAD_ALL, K_STARTSEL };

typedef struct { short x0, y0, x1, y1; signed char ctl; unsigned char kind; } IconBox;

static const IconBox kBoxes[] = {
    {  57, 178,  94, 215, PIM_CROSS,    K_FACE },
    { 419,  57, 456,  94, PIM_CIRCLE,   K_FACE },
    { 243,  57, 280,  94, PIM_SQUARE,   K_FACE },
    {   6, 178,  43, 215, PIM_TRIANGLE, K_FACE },
    { 241,   8, 290,  41, PIM_L1,       K_SHOULDER },
    { 300,   8, 349,  41, PIM_L2,       K_SHOULDER },
    { 359,   8, 408,  41, PIM_R1,       K_SHOULDER },
    { 418,   8, 467,  41, PIM_R2,       K_SHOULDER },
    { 182, 122, 229, 169, PIM_L3,       K_STICK_CLICK },
    { 300,  52, 347,  99, PIM_R3,       K_STICK_CLICK },
    { 122, 121, 168, 168, PIM_LS_UP,    K_STICK },        /* the left stick */
    { 243, 121, 288, 168, PIM_RS_UP,    K_STICK },        /* the right stick */
    { 208, 176, 249, 218, PIM_UP,       K_DPAD_DIR },
    { 310, 106, 351, 148, PIM_DOWN,     K_DPAD_DIR },
    { 258, 176, 300, 218, PIM_LEFT,     K_DPAD_DIR },
    { 157, 176, 198, 218, PIM_RIGHT,    K_DPAD_DIR },
    { 105, 176, 147, 218, -1,           K_DPAD_ALL },
    {   1, 121,  57, 168, PIM_START,    K_STARTSEL },
    {  60, 121, 116, 168, PIM_SELECT,   K_STARTSEL },
};
#define N_BOXES (int)(sizeof kBoxes / sizeof kBoxes[0])

/* FNV-1a of the alpha of two glyphs (the cross, L1) in the original. */
static uint32_t alpha_fnv(const uint8_t* px, uint32_t row, int x0, int y0, int x1, int y1)
{
    uint32_t h = 2166136261u;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) { h ^= px[(size_t)y * row + (size_t)x * 4 + 3]; h *= 16777619u; }
    return h;
}
static int is_atlas(const uint8_t* px, uint32_t row)
{
    return alpha_fnv(px, row, 57, 178, 94, 215) == 0x35be9798u &&
           alpha_fnv(px, row, 241, 8, 290, 41) == 0x14f8e1bfu;
}

/* ---- a tiny vector renderer: signed distances, analytic antialiasing ------ */

enum { P_DISC, P_RING, P_RRECT, P_TEXT, P_SEG };
enum { OP_FILL, OP_CUT };

typedef struct {
    unsigned char type, op;
    float a, b, c, d, e, f;     /* geometry (see prim_sd) */
    float r, g, bl;             /* fill colour 0..1 */
    int seg0, nseg;             /* P_TEXT: its segments */
    int clip;                   /* coverage also limited to the box below */
    float cx0, cy0, cx1, cy1;
} Prim;

typedef struct { float x0, y0, x1, y1, rad; } Seg;

#define MAX_PRIMS 64
#define MAX_SEGS 640
typedef struct {
    Prim p[MAX_PRIMS]; int n;
    Seg s[MAX_SEGS]; int ns;
} Scene;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float seg_dist(const Seg* s, float x, float y)
{
    const float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    const float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((x - s->x0) * dx + (y - s->y0) * dy) / l2 : 0;
    t = clampf(t, 0, 1);
    const float ex = x - (s->x0 + t * dx), ey = y - (s->y0 + t * dy);
    return sqrtf(ex * ex + ey * ey) - s->rad;
}

/* Rounded box: centre (a,b), half size (c,d), corner radius e on top, f on
 * the bottom. */
static float rrect_sd(const Prim* p, float x, float y)
{
    const float rad = y < p->b ? p->e : p->f;
    const float qx = fabsf(x - p->a) - p->c + rad, qy = fabsf(y - p->b) - p->d + rad;
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    const float in = (qx > qy ? qx : qy);
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rad;
}

static float prim_sd(const Scene* sc, const Prim* p, float x, float y)
{
    switch (p->type) {
    case P_DISC: { const float dx = x - p->a, dy = y - p->b; return sqrtf(dx * dx + dy * dy) - p->c; }
    case P_RING: { const float dx = x - p->a, dy = y - p->b; return fabsf(sqrtf(dx * dx + dy * dy) - p->c) - p->d * 0.5f; }
    case P_RRECT: return rrect_sd(p, x, y);
    case P_SEG: { Seg s = { p->a, p->b, p->c, p->d, p->e }; return seg_dist(&s, x, y); }
    case P_TEXT: {
        float m = 1e9f;
        for (int i = 0; i < p->nseg; i++) { const float d = seg_dist(&sc->s[p->seg0 + i], x, y); if (d < m) m = d; }
        return m;
    }
    }
    return 1e9f;
}

static Prim* add(Scene* sc, int type, int op, float r, float g, float b)
{
    if (sc->n >= MAX_PRIMS) return NULL;
    Prim* p = &sc->p[sc->n++];
    memset(p, 0, sizeof *p);
    p->type = (unsigned char)type; p->op = (unsigned char)op; p->r = r; p->g = g; p->bl = b;
    return p;
}
static void disc(Scene* sc, int op, float cx, float cy, float rad, const float* col)
{
    Prim* p = add(sc, P_DISC, op, col ? col[0] : 1, col ? col[1] : 1, col ? col[2] : 1);
    if (p) { p->a = cx; p->b = cy; p->c = rad; }
}
static void ring(Scene* sc, int op, float cx, float cy, float rad, float w)
{
    Prim* p = add(sc, P_RING, op, 1, 1, 1);
    if (p) { p->a = cx; p->b = cy; p->c = rad; p->d = w; }
}
static void rrect(Scene* sc, int op, float x0, float y0, float x1, float y1, float rtop, float rbot, const float* col)
{
    Prim* p = add(sc, P_RRECT, op, col ? col[0] : 1, col ? col[1] : 1, col ? col[2] : 1);
    if (p) { p->a = (x0 + x1) * 0.5f; p->b = (y0 + y1) * 0.5f; p->c = (x1 - x0) * 0.5f; p->d = (y1 - y0) * 0.5f;
             p->e = rtop; p->f = rbot; }
}
static void seg(Scene* sc, int op, float x0, float y0, float x1, float y1, float rad)
{
    Prim* p = add(sc, P_SEG, op, 1, 1, 1);
    if (p) { p->a = x0; p->b = y0; p->c = x1; p->d = y1; p->e = rad; }
}

/* ---- the stroke font -------------------------------------------------------
 * Cap height 6 units, y down; M moves, L draws, Z closes. Original letter
 * shapes for this file (a condensed bold sans, like the title's prompts). */
typedef struct { unsigned char ch; float w; const char* path; } StrokeGlyph;

enum { CH_UP = 1, CH_DOWN, CH_LEFT, CH_RIGHT, CH_RETURN, CH_SHIFT, CH_SPACE, CH_BKSP,
       CH_TAB, CH_MENU = 11 };

static const StrokeGlyph kFont[] = {
    {'A', 4, "M0,6 L2,0 L4,6 M0.8,3.9 L3.2,3.9"},
    {'B', 3.8f, "M0,3 L0,0 L2.6,0 L3.4,0.7 L3.4,2.3 L2.6,3 L0,3 L0,6 L2.8,6 L3.8,5.2 L3.8,3.8 L2.8,3"},
    {'C', 3.8f, "M3.8,1 L3,0 L1,0 L0,1 L0,5 L1,6 L3,6 L3.8,5"},
    {'D', 3.8f, "M0,0 L0,6 L2.4,6 L3.8,4.8 L3.8,1.2 L2.4,0 Z"},
    {'E', 3.4f, "M3.4,0 L0,0 L0,6 L3.4,6 M0,3 L2.6,3"},
    {'F', 3.4f, "M3.4,0 L0,0 L0,6 M0,3 L2.6,3"},
    {'G', 3.8f, "M3.8,1 L3,0 L1,0 L0,1 L0,5 L1,6 L3,6 L3.8,5 L3.8,3.4 L2.2,3.4"},
    {'H', 3.8f, "M0,0 L0,6 M3.8,0 L3.8,6 M0,3 L3.8,3"},
    {'I', 0, "M0,0 L0,6"},
    {'J', 3.4f, "M3.4,0 L3.4,5 L2.4,6 L1,6 L0,5"},
    {'K', 3.8f, "M0,0 L0,6 M3.8,0 L0,3.8 M1.3,2.6 L3.8,6"},
    {'L', 3.4f, "M0,0 L0,6 L3.4,6"},
    {'M', 5, "M0,6 L0,0 L2.5,3.6 L5,0 L5,6"},
    {'N', 3.8f, "M0,6 L0,0 L3.8,6 L3.8,0"},
    {'O', 4, "M1,0 L3,0 L4,1 L4,5 L3,6 L1,6 L0,5 L0,1 Z"},
    {'P', 3.8f, "M0,6 L0,0 L2.8,0 L3.8,0.8 L3.8,2.4 L2.8,3.2 L0,3.2"},
    {'Q', 4, "M1,0 L3,0 L4,1 L4,5 L3,6 L1,6 L0,5 L0,1 Z M2.6,4.6 L4.2,6.2"},
    {'R', 3.8f, "M0,6 L0,0 L2.8,0 L3.8,0.8 L3.8,2.4 L2.8,3.2 L0,3.2 M2.2,3.2 L3.8,6"},
    {'S', 4, "M3.8,0.8 L3,0 L1,0 L0.1,0.8 L0.1,2.2 L0.9,3 L3.1,3 L3.9,3.8 L3.9,5.2 L3,6 L1,6 L0.1,5.2"},
    {'T', 4, "M0,0 L4,0 M2,0 L2,6"},
    {'U', 3.8f, "M0,0 L0,5 L1,6 L2.8,6 L3.8,5 L3.8,0"},
    {'V', 4, "M0,0 L2,6 L4,0"},
    {'W', 5.6f, "M0,0 L1.2,6 L2.8,1.4 L4.4,6 L5.6,0"},
    {'X', 4, "M0,0 L4,6 M4,0 L0,6"},
    {'Y', 4, "M0,0 L2,3 L4,0 M2,3 L2,6"},
    {'Z', 4, "M0,0 L4,0 L0,6 L4,6"},
    {'0', 4, "M1,0 L3,0 L4,1 L4,5 L3,6 L1,6 L0,5 L0,1 Z"},
    {'1', 1.4f, "M0,1 L1.4,0 L1.4,6"},
    {'2', 4, "M0,1 L1,0 L3,0 L4,1 L4,2.4 L0,6 L4,6"},
    {'3', 4, "M0,0.8 L1,0 L3,0 L4,0.8 L4,2.2 L3,3 L1.6,3 M3,3 L4,3.8 L4,5.2 L3,6 L1,6 L0,5.2"},
    {'4', 4.2f, "M3,6 L3,0 L0,4.2 L4.2,4.2"},
    {'5', 4, "M3.8,0 L0.4,0 L0.1,3 L3,2.7 L4,3.5 L4,5.2 L3,6 L1,6 L0,5.2"},
    {'6', 4, "M3.6,0.6 L2.8,0 L1,0 L0,1 L0,5 L1,6 L3,6 L4,5 L4,3.6 L3,2.8 L1,2.8 L0,3.6"},
    {'7', 4, "M0,0 L4,0 L1.6,6"},
    {'8', 4, "M1,0 L3,0 L3.8,0.8 L3.8,2.2 L3,3 L1,3 L0.2,2.2 L0.2,0.8 Z M1,3 L3,3 L4,3.8 L4,5.2 L3,6 L1,6 L0,5.2 L0,3.8 Z"},
    {'9', 4, "M0.4,5.4 L1.2,6 L3,6 L4,5 L4,1 L3,0 L1,0 L0,1 L0,2.4 L1,3.2 L3,3.2 L4,2.4"},
    {'-', 2.4f, "M0,3.2 L2.4,3.2"},
    {'+', 4, "M0,3.2 L4,3.2 M2,1.2 L2,5.2"},
    {'=', 3.6f, "M0,2.2 L3.6,2.2 M0,4.2 L3.6,4.2"},
    {'*', 3.6f, "M1.8,1.2 L1.8,5.2 M0.1,2.2 L3.5,4.2 M3.5,2.2 L0.1,4.2"},
    {'/', 3, "M0,6 L3,0"},
    {'\\', 3, "M0,0 L3,6"},
    {'.', 0, "M0,6 L0,6"},
    {',', 0.6f, "M0.6,5.6 L0,7"},
    {';', 0.6f, "M0.6,2 L0.6,2 M0.6,5.6 L0,7"},
    {'\'', 0, "M0,0 L0,2"},
    {'`', 1.2f, "M0,0 L1.2,1.4"},
    {'[', 2, "M2,0 L0,0 L0,6 L2,6"},
    {']', 2, "M0,0 L2,0 L2,6 L0,6"},
    {'?', 3.6f, "M0,1 L1,0 L2.6,0 L3.6,1 L3.6,2 L1.8,3.4 L1.8,4.2 M1.8,6 L1.8,6"},
    {CH_UP, 4, "M2,6 L2,0.4 M0,2.4 L2,0.3 L4,2.4"},
    {CH_DOWN, 4, "M2,0 L2,5.6 M0,3.6 L2,5.7 L4,3.6"},
    {CH_LEFT, 5.6f, "M5.6,3 L0.4,3 M2.4,1 L0.3,3 L2.4,5"},
    {CH_RIGHT, 5.6f, "M0,3 L5.2,3 M3.2,1 L5.3,3 L3.2,5"},
    {CH_RETURN, 5, "M5,0.4 L5,4 L0.6,4 M2.6,2 L0.5,4 L2.6,6"},
    {CH_SHIFT, 5, "M1.4,6 L1.4,3.4 L0,3.4 L2.5,0.3 L5,3.4 L3.6,3.4 L3.6,6 Z"},
    {CH_SPACE, 6, "M0,3.4 L0,5.6 L6,5.6 L6,3.4"},
    {CH_BKSP, 6, "M1.8,0.6 L6,0.6 L6,5.4 L1.8,5.4 L0,3 Z M3,1.9 L4.8,4.1 M4.8,1.9 L3,4.1"},
    {CH_TAB, 6, "M0,3 L5,3 M3,1 L5,3 L3,5 M5.9,0.6 L5.9,5.4"},
    {CH_MENU, 5, "M0,1 L5,1 M0,3 L5,3 M0,5 L5,5"},
};

static const StrokeGlyph* font_glyph(unsigned char c)
{
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
    for (size_t i = 0; i < sizeof kFont / sizeof kFont[0]; i++) if (kFont[i].ch == c) return &kFont[i];
    return NULL;
}

/* Adds `text` as one P_TEXT primitive fitted in the box (centred, condensed
 * down to 60% before it shrinks), stroke `sw` units, height at most max_h px. */
static void text(Scene* sc, int op, const float* col, const char* str,
                 float x0, float y0, float x1, float y1, float max_h, float sw)
{
    const float gap = 1.3f + sw;
    float W = 0; int n = 0;
    for (const char* s = str; *s; s++) {
        const StrokeGlyph* g = font_glyph((unsigned char)*s);
        if (!g) continue;
        W += g->w + (n ? gap : 0); n++;
    }
    if (!n) return;
    const float bw = x1 - x0, bh = y1 - y0;
    float h = bh < max_h ? bh : max_h;
    float sy = h / (6.0f + sw), sx = sy;
    if (W * sx + sw * sy > bw) {
        sx = (bw - sw * sy) / (W > 0 ? W : 1);
        if (sx < 0.6f * sy) { sy = bw / (0.6f * W + sw); sx = 0.6f * sy; }
    }
    const float rad = sw * sy * 0.5f;
    const float tw = W * sx, th = 6.0f * sy;
    const float ox = (x0 + x1) * 0.5f - tw * 0.5f, oy = (y0 + y1) * 0.5f - th * 0.5f;
    Prim* p = add(sc, P_TEXT, op, col ? col[0] : 1, col ? col[1] : 1, col ? col[2] : 1);
    if (!p) return;
    p->seg0 = sc->ns;
    float pen = 0; int first = 1;
    for (const char* s = str; *s; s++) {
        const StrokeGlyph* g = font_glyph((unsigned char)*s);
        if (!g) continue;
        if (!first) pen += gap;
        first = 0;
        float cx = 0, cy = 0, mx = 0, my = 0; int have = 0;
        for (const char* q = g->path; *q; ) {
            while (*q == ' ') q++;
            const char cmd = *q;
            if (!cmd) break;
            q++;
            if (cmd == 'Z') {
                if (have && sc->ns < MAX_SEGS)
                    sc->s[sc->ns++] = (Seg){ ox + (pen + cx) * sx, oy + cy * sy, ox + (pen + mx) * sx, oy + my * sy, rad };
                cx = mx; cy = my; continue;
            }
            char* e; const float x = strtof(q, &e); q = e; if (*q == ',') q++;
            const float y = strtof(q, &e); q = e;
            if (cmd == 'M') { cx = mx = x; cy = my = y; have = 1; }
            else if (cmd == 'L' && sc->ns < MAX_SEGS) {
                sc->s[sc->ns++] = (Seg){ ox + (pen + cx) * sx, oy + cy * sy, ox + (pen + x) * sx, oy + y * sy, rad };
                cx = x; cy = y;
            }
        }
        pen += g->w;
    }
    p->nseg = sc->ns - p->seg0;
}

/* Renders the scene into the atlas box (cleared first, one pixel larger). */
static void render(uint8_t* px, uint32_t row, const IconBox* b, const Scene* sc)
{
    float base[3] = { 1, 1, 1 };
    for (int i = 0; i < sc->n; i++) if (sc->p[i].op == OP_FILL) { base[0] = sc->p[i].r; base[1] = sc->p[i].g; base[2] = sc->p[i].bl; break; }
    const int x0 = b->x0 - 1 < 0 ? 0 : b->x0 - 1, y0 = b->y0 - 1 < 0 ? 0 : b->y0 - 1;
    const int x1 = b->x1 + 1 >= (int)ATLAS_W ? (int)ATLAS_W - 1 : b->x1 + 1;
    const int y1 = b->y1 + 1 >= (int)ATLAS_H ? (int)ATLAS_H - 1 : b->y1 + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            float pr = 0, pg = 0, pb = 0, pa = 0;
            for (int i = 0; i < sc->n; i++) {
                const Prim* p = &sc->p[i];
                float k = clampf(0.5f - prim_sd(sc, p, fx, fy), 0, 1);
                if (p->clip && k > 0) {
                    float o = p->cx0 - fx, t;
                    if ((t = fx - p->cx1) > o) o = t;
                    if ((t = p->cy0 - fy) > o) o = t;
                    if ((t = fy - p->cy1) > o) o = t;
                    k *= clampf(0.5f - o, 0, 1);
                }
                if (k <= 0) continue;
                if (p->op == OP_FILL) {
                    pr = p->r * k + pr * (1 - k); pg = p->g * k + pg * (1 - k);
                    pb = p->bl * k + pb * (1 - k); pa = k + pa * (1 - k);
                } else { pr *= 1 - k; pg *= 1 - k; pb *= 1 - k; pa *= 1 - k; }
            }
            uint8_t* d = px + (size_t)y * row + (size_t)x * 4;
            /* The original's white is ~240; the edge colour of a transparent
             * texel stays the shape's, so filtering adds no dark fringe. */
            const float cr = pa > 1e-3f ? pr / pa : base[0], cg = pa > 1e-3f ? pg / pa : base[1],
                        cb = pa > 1e-3f ? pb / pa : base[2];
            d[0] = (uint8_t)(clampf(cr, 0, 1) * 240.0f + 0.5f);
            d[1] = (uint8_t)(clampf(cg, 0, 1) * 240.0f + 0.5f);
            d[2] = (uint8_t)(clampf(cb, 0, 1) * 240.0f + 0.5f);
            d[3] = (uint8_t)(clampf(pa, 0, 1) * 255.0f + 0.5f);
        }
}

/* ---- the glyphs ----------------------------------------------------------- */

static const float kWhite[3] = { 1, 1, 1 };

/* A round face button, the original's look: a disc with a thin ring cut in
 * near its edge; the label cut out (PlayStation-like white) or, with a
 * colour, a coloured disc with a white label. */
static void g_face(Scene* sc, const IconBox* b, const char* label, const float* col)
{
    const float cx = (b->x0 + b->x1 + 1) * 0.5f, cy = (b->y0 + b->y1 + 1) * 0.5f;
    const float w = (float)(b->x1 - b->x0 + 1), h = (float)(b->y1 - b->y0 + 1);
    const float r = (w < h ? w : h) * 0.5f - 0.3f;
    disc(sc, OP_FILL, cx, cy, r, col ? col : kWhite);
    /* The letter is cut out, as the original cuts its symbol: the game tints
     * the atlas with the text colour (light grey in the menus, dark grey on
     * the pause screen), so a printed white letter vanished into a dark
     * disc there, while a hole always shows the background. A coloured disc
     * drops the original's thin ring so the letter can be larger (it is
     * read at ~20 px). */
    if (!col) ring(sc, OP_CUT, cx, cy, r - 3.0f, 1.3f);
    const float th = r * (col ? 1.22f : 1.12f), hw = r * (col ? 0.72f : 0.62f);
    text(sc, OP_CUT, NULL, label, cx - hw, cy - th * 0.5f, cx + hw, cy + th * 0.5f, th, col ? 2.1f : 1.8f);
}

/* A shoulder button or trigger in the L1 box: bumper = rounded bar, trigger
 * = rounded on top; the label cut out. */
static void g_shoulder(Scene* sc, const IconBox* b, const char* label, int trigger)
{
    const float x0 = b->x0, y0 = b->y0, x1 = b->x1 + 1, y1 = b->y1 + 1;
    const float h = y1 - y0;
    if (trigger) rrect(sc, OP_FILL, x0 + 3, y0, x1 - 3, y1, h * 0.42f, 3.0f, kWhite);
    else         rrect(sc, OP_FILL, x0, y0 + 2, x1, y1 - 2, (h - 4) * 0.5f, (h - 4) * 0.5f, kWhite);
    text(sc, OP_CUT, NULL, label, x0 + 6, y0 + 4, x1 - 6, y1 - 4, h - 8, 1.8f);
}

/* A stick click (L3/R3 box): the stick's cap with a short stem, labelled. */
static void g_stick_click(Scene* sc, const IconBox* b, const char* label)
{
    const float x0 = b->x0, y0 = b->y0, x1 = b->x1 + 1, y1 = b->y1 + 1;
    const float w = x1 - x0, h = y1 - y0;
    const float r = (w < h ? w : h) * 0.40f, cx = (x0 + x1) * 0.5f, cy = y0 + r + 0.5f;
    disc(sc, OP_FILL, cx, cy, r, kWhite);
    rrect(sc, OP_FILL, cx - r * 0.38f, cy, cx + r * 0.38f, y1 - 0.5f, 2, 3, kWhite);
    ring(sc, OP_CUT, cx, cy, r - 3.0f, 1.3f);
    text(sc, OP_CUT, NULL, label, cx - r * 0.7f, cy - r * 0.5f, cx + r * 0.7f, cy + r * 0.5f, r, 1.75f);
}

/* A key: a keycap filling the box, its name cut out. */
static void g_key(Scene* sc, float x0, float y0, float x1, float y1, const char* label, float max_h)
{
    const float h = y1 - y0, w = x1 - x0;
    const float rad = (h < w ? h : w) * 0.22f;
    rrect(sc, OP_FILL, x0, y0, x1, y1, rad, rad, kWhite);
    const float m = (h < w ? h : w) * 0.16f;
    text(sc, OP_CUT, NULL, label, x0 + m, y0 + m, x1 - m, y1 - m, max_h, 1.7f);
}

/* A mouse: body outline, the middle split; `button` 1 left, 2 right, 4
 * middle (filled), 0 none; wheel = 1 up / 2 down shows an arrow. */
static void g_mouse(Scene* sc, const IconBox* b, int button, int wheel, const char* extra)
{
    const float bw = (float)(b->x1 - b->x0 + 1), bh = (float)(b->y1 - b->y0 + 1);
    const float h = bh - 2, w = h * 0.68f < bw - 2 ? h * 0.68f : bw - 2;
    const float cx = (b->x0 + b->x1 + 1) * 0.5f, y0 = b->y0 + 1, y1 = y0 + h;
    const float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
    const float split = y0 + h * 0.42f;
    rrect(sc, OP_FILL, x0, y0, x1, y1, w * 0.5f, w * 0.5f, kWhite);
    rrect(sc, OP_CUT, x0 + 2.4f, y0 + 2.4f, x1 - 2.4f, y1 - 2.4f, w * 0.5f - 2.4f, w * 0.5f - 2.4f, NULL);
    seg(sc, OP_FILL, x0 + 1, split, x1 - 1, split, 1.1f);
    seg(sc, OP_FILL, cx, y0 + 1, cx, split, 1.1f);
    if (button == 1 || button == 2) {   /* the button's quarter, filled */
        rrect(sc, OP_FILL, x0 + 2.4f, y0 + 2.4f, x1 - 2.4f, y1 - 2.4f, w * 0.5f - 2.4f, w * 0.5f - 2.4f, kWhite);
        Prim* p = &sc->p[sc->n - 1];
        p->clip = 1; p->cy0 = y0; p->cy1 = split;
        p->cx0 = button == 1 ? x0 : cx; p->cx1 = button == 1 ? cx : x1;
    }
    if (button == 4 || wheel) {
        rrect(sc, OP_FILL, cx - 2.2f, y0 + h * 0.12f, cx + 2.2f, y0 + h * 0.34f, 2.2f, 2.2f, kWhite);
    }
    if (wheel || (extra && *extra)) {
        const char* t = extra && *extra ? extra : (wheel == 1 ? "\x01" : "\x02");
        text(sc, OP_FILL, kWhite, t, x0 + 3, split + 3, x1 - 3, y1 - 3, (y1 - split) * 0.7f, 1.4f);
    }
}

/* Four small keys in an inverted T (up over left/down/right). */
static void g_cluster(Scene* sc, const IconBox* b, const char* up, const char* left,
                      const char* down, const char* right)
{
    const float x0 = b->x0, y0 = b->y0, x1 = b->x1 + 1, y1 = b->y1 + 1;
    const float w = (x1 - x0) / 3.0f, h = (y1 - y0) * 0.5f;
    const float k = (w < h ? w : h) - 1.2f;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float ty = cy - k - 0.6f, by = cy + 0.6f;
    g_key(sc, cx - k * 0.5f, ty, cx + k * 0.5f, ty + k, up, k);
    g_key(sc, cx - k * 1.5f - 1.2f, by, cx - k * 0.5f - 1.2f, by + k, left, k);
    g_key(sc, cx - k * 0.5f, by, cx + k * 0.5f, by + k, down, k);
    g_key(sc, cx + k * 0.5f + 1.2f, by, cx + k * 1.5f + 1.2f, by + k, right, k);
}

/* A D-pad with one arm (1 up, 2 down, 3 left, 4 right) solid, the others
 * faint, as the original does it. */
static void g_dpad(Scene* sc, const IconBox* b, int arm)
{
    const float x0 = b->x0, y0 = b->y0, x1 = b->x1 + 1, y1 = b->y1 + 1;
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float s = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) * 0.5f - 0.5f, t = s * 0.36f;
    /* faint arms are drawn with the alpha the original uses (~92/255) by a
     * second scene pass: here only the solid one, the rest via 'faint'. */
    const float fx[4][4] = { { cx - t, cy - s, cx + t, cy - t * 0.4f }, { cx - t, cy + t * 0.4f, cx + t, cy + s },
                             { cx - s, cy - t, cx - t * 0.4f, cy + t }, { cx + t * 0.4f, cy - t, cx + s, cy + t } };
    for (int i = 0; i < 4; i++) {
        const float* f = fx[i];
        const float g = (i + 1 == arm) ? 1.0f : 0.36f;
        const float col[3] = { 1, 1, 1 };
        Prim* p = add(sc, P_RRECT, OP_FILL, col[0], col[1], col[2]);
        if (!p) return;
        p->a = (f[0] + f[2]) * 0.5f; p->b = (f[1] + f[3]) * 0.5f; p->c = (f[2] - f[0]) * 0.5f; p->d = (f[3] - f[1]) * 0.5f;
        p->e = p->f = 2.0f;
        p->r = g;   /* coverage scale, applied in render_dpad */
    }
}

/* ---- labels ---------------------------------------------------------------- */

typedef struct { unsigned char code; const char* shrt; const char* lng; } KeyLabel;
static const KeyLabel kKeyLabels[] = {
    {0x24, "\x05", "ENTER"}, {0x4C, "\x05", "ENTER"}, {0x30, "TAB", "TAB"}, {0x31, "\x07", "SPACE"},
    {0x33, "\x08", "BKSP"}, {0x35, "ESC", "ESC"}, {0x38, "\x06", "SHIFT"}, {0x3C, "\x06", "SHIFT"},
    {0x3A, "OPT", "OPT"}, {0x3D, "OPT", "OPT"}, {0x3B, "CTRL", "CTRL"}, {0x3E, "CTRL", "CTRL"},
    {0x7E, "\x01", "\x01"}, {0x7D, "\x02", "\x02"}, {0x7B, "\x03", "\x03"}, {0x7C, "\x04", "\x04"},
    {0x73, "HOME", "HOME"}, {0x77, "END", "END"}, {0x74, "PGUP", "PGUP"}, {0x79, "PGDN", "PGDN"},
    {0x75, "DEL", "DEL"}, {0x72, "INS", "INS"}, {0x0A, "SEC", "SEC"}, {0x47, "CLR", "CLR"},
    {0x41, "N.", "NUM ."}, {0x43, "N*", "NUM *"}, {0x45, "N+", "NUM +"}, {0x4B, "N/", "NUM /"},
    {0x4E, "N-", "NUM -"}, {0x51, "N=", "NUM ="},
};

/* The label of macOS virtual key `vk` (short: 1-3 characters or a symbol). */
static void key_label(unsigned vk, int lng, char* out, size_t n)
{
    for (size_t i = 0; i < sizeof kKeyLabels / sizeof kKeyLabels[0]; i++)
        if (kKeyLabels[i].code == vk) { snprintf(out, n, "%s", lng ? kKeyLabels[i].lng : kKeyLabels[i].shrt); return; }
    if (vk >= 0x52 && vk <= 0x5C) {   /* keypad digits (0x5A is F20) */
        static const signed char d[] = { 0, 1, 2, 3, 4, 5, 6, 7, -1, 8, 9 };
        const int v = d[vk - 0x52];
        if (v >= 0) { snprintf(out, n, lng ? "NUM %d" : "N%d", v); return; }
    }
    for (int i = 0; i < PIM_COUNT(pim_keys); i++)
        if (pim_keys[i].id == (int)vk) {
            const char* nm = pim_keys[i].name;
            if (strlen(nm) == 1) { snprintf(out, n, "%c", toupper((unsigned char)nm[0])); return; }
            if (nm[0] == 'f' && isdigit((unsigned char)nm[1])) { snprintf(out, n, "F%s", nm + 1); return; }
            if (!strcmp(nm, "minus")) { snprintf(out, n, "-"); return; }
            if (!strcmp(nm, "equal")) { snprintf(out, n, "="); return; }
            if (!strcmp(nm, "leftbracket")) { snprintf(out, n, "["); return; }
            if (!strcmp(nm, "rightbracket")) { snprintf(out, n, "]"); return; }
            if (!strcmp(nm, "backslash")) { snprintf(out, n, "\\"); return; }
            if (!strcmp(nm, "semicolon")) { snprintf(out, n, ";"); return; }
            if (!strcmp(nm, "quote")) { snprintf(out, n, "'"); return; }
            if (!strcmp(nm, "comma")) { snprintf(out, n, ","); return; }
            if (!strcmp(nm, "period")) { snprintf(out, n, "."); return; }
            if (!strcmp(nm, "slash")) { snprintf(out, n, "/"); return; }
            if (!strcmp(nm, "grave")) { snprintf(out, n, "`"); return; }
            break;
        }
    snprintf(out, n, "?");
}

/* The first key bound to control c, or -1. */
static int first_key(const PadInputMap* m, int c)
{
    for (unsigned vk = 0; vk < 128; vk++)
        if (m->key[c][vk >> 5] & (1u << (vk & 31))) return (int)vk;
    return -1;
}
static int first_bit(uint32_t v) { for (int i = 0; i < 32; i++) if (v & (1u << i)) return i; return -1; }

/* ---- per-mode drawing ------------------------------------------------------ */

/* Bright: the game multiplies the atlas by its text colour (~75% grey). */
static const float kColA[3] = { 0.40f, 0.86f, 0.28f };
static const float kColB[3] = { 0.98f, 0.30f, 0.26f };
static const float kColX[3] = { 0.26f, 0.58f, 1.00f };
static const float kColY[3] = { 1.00f, 0.80f, 0.16f };

static void render_scene(uint8_t* px, uint32_t row, const IconBox* b, Scene* sc)
{
    render(px, row, b, sc);
}

/* D-pad arms with faint ones: rendered as two layers (solid scene, then the
 * faint arms at 36% coverage), straight into the box. */
static void render_dpad(uint8_t* px, uint32_t row, const IconBox* b, int arm)
{
    Scene sc; sc.n = sc.ns = 0;
    g_dpad(&sc, b, arm);
    const int x0 = b->x0 - 1, y0 = b->y0 - 1, x1 = b->x1 + 1, y1 = b->y1 + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float a = 0;
            for (int i = 0; i < sc.n; i++) {
                const Prim* p = &sc.p[i];
                const float k = clampf(0.5f - rrect_sd(p, x + 0.5f, y + 0.5f), 0, 1) * p->r;
                a = k + a * (1 - k);
            }
            uint8_t* d = px + (size_t)y * row + (size_t)x * 4;
            d[0] = d[1] = d[2] = 240; d[3] = (uint8_t)(clampf(a, 0, 1) * 255.0f + 0.5f);
        }
}

/* Xbox: the glyph of SDL controller input `in` in box b. */
static void draw_pad_input(uint8_t* px, uint32_t row, const IconBox* b, int in)
{
    Scene sc; sc.n = sc.ns = 0;
    switch (in) {
    case 0: g_face(&sc, b, "A", kColA); break;
    case 1: g_face(&sc, b, "B", kColB); break;
    case 2: g_face(&sc, b, "X", kColX); break;
    case 3: g_face(&sc, b, "Y", kColY); break;
    case 9: case 10: case PIM_PAD_TRIGGER_LEFT: case PIM_PAD_TRIGGER_RIGHT: {
        const char* l = in == 9 ? "LB" : in == 10 ? "RB" : in == PIM_PAD_TRIGGER_LEFT ? "LT" : "RT";
        const int trig = in >= PIM_PAD_TRIGGER_LEFT;
        if (b->kind == K_SHOULDER) g_shoulder(&sc, b, l, trig);
        else {
            /* a shoulder in a round box: the same shape, scaled to fit */
            IconBox s = *b; const int h = (b->y1 - b->y0) * 7 / 10, m = (b->y1 - b->y0 - h) / 2;
            s.y0 = (short)(b->y0 + m); s.y1 = (short)(s.y0 + h);
            g_shoulder(&sc, &s, l, trig);
        }
        break; }
    case 7: case 8: g_stick_click(&sc, b, in == 7 ? "LS" : "RS"); break;
    case 4: case 6: {
        const int menu = in == 6;
        if (b->kind == K_STARTSEL) {
            const float x0 = b->x0, x1 = b->x1 + 1, y0 = b->y0 + 3, ym = b->y0 + 23, y1 = b->y1 + 1;
            text(&sc, OP_FILL, kWhite, menu ? "MENU" : "VIEW", x0 + 1, y0, x1 - 1, ym, ym - y0, 1.5f);
            const float cx = (x0 + x1) * 0.5f, iw = 17, ih = (y1 - ym - 4) * 0.5f, icy = (ym + 3 + y1) * 0.5f;
            if (menu) {
                rrect(&sc, OP_FILL, cx - iw, ym + 3, cx + iw, y1 - 1, 5, 5, kWhite);
                text(&sc, OP_CUT, NULL, "\x0b", cx - iw + 6, ym + 6, cx + iw - 6, y1 - 4, y1 - ym - 10, 1.6f);
            } else {
                rrect(&sc, OP_FILL, cx - iw + 9, ym + 3, cx + iw, icy + ih * 0.5f, 4, 4, kWhite);
                rrect(&sc, OP_CUT, cx - iw + 11.4f, ym + 5.4f, cx + iw - 2.4f, icy + ih * 0.5f - 2.4f, 2, 2, NULL);
                rrect(&sc, OP_FILL, cx - iw, icy - ih * 0.5f + 2, cx + iw - 9, y1 - 1, 4, 4, kWhite);
                rrect(&sc, OP_CUT, cx - iw + 2.4f, icy - ih * 0.5f + 4.4f, cx + iw - 11.4f, y1 - 3.4f, 2, 2, NULL);
            }
        } else {
            const float x0 = b->x0, x1 = b->x1 + 1, y0 = b->y0, y1 = b->y1 + 1;
            const float r = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) * 0.5f - 0.3f;
            disc(&sc, OP_FILL, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, r, kWhite);
            text(&sc, OP_CUT, NULL, menu ? "\x0b" : "VIEW", (x0 + x1) * 0.5f - r * 0.62f, (y0 + y1) * 0.5f - r * 0.4f,
                 (x0 + x1) * 0.5f + r * 0.62f, (y0 + y1) * 0.5f + r * 0.4f, r, 1.5f);
        }
        break; }
    case 11: case 12: case 13: case 14:
        render_dpad(px, row, b, in == 11 ? 1 : in == 12 ? 2 : in == 13 ? 3 : 4);
        return;
    default: {
        char l[8];
        if (in == 5) snprintf(l, sizeof l, "HOME");
        else if (in == 15) snprintf(l, sizeof l, "SHARE");
        else if (in >= 16 && in <= 19) snprintf(l, sizeof l, "P%d", in - 15);
        else if (in == 20) snprintf(l, sizeof l, "PAD");
        else snprintf(l, sizeof l, "?");
        const float x0 = b->x0, x1 = b->x1 + 1, y0 = b->y0, y1 = b->y1 + 1;
        g_key(&sc, x0, y0 + (y1 - y0) * 0.15f, x1, y1 - (y1 - y0) * 0.15f, l, (y1 - y0) * 0.5f);
        break; }
    }
    render_scene(px, row, b, &sc);
}

static void draw_xbox(uint8_t* px, uint32_t row, const PadInputMap* m)
{
    static const int natural_dpad[] = { [PIM_UP] = 11, [PIM_DOWN] = 12, [PIM_LEFT] = 13, [PIM_RIGHT] = 14 };
    for (int i = 0; i < N_BOXES; i++) {
        const IconBox* b = &kBoxes[i];
        if (b->kind == K_STICK || b->kind == K_DPAD_ALL) continue;   /* sticks and the D-pad look alike */
        const int in = first_bit(m->pad[b->ctl]);
        if (in < 0) continue;                                       /* unbound: the original stays */
        if (b->kind == K_DPAD_DIR && in == natural_dpad[b->ctl]) continue;
        draw_pad_input(px, row, b, in);
    }
}

static void draw_keyboard(uint8_t* px, uint32_t row, const PadInputMap* m, int mouse_camera)
{
    for (int i = 0; i < N_BOXES; i++) {
        const IconBox* b = &kBoxes[i];
        Scene sc; sc.n = sc.ns = 0;
        const float x0 = b->x0, y0 = b->y0, x1 = b->x1 + 1, y1 = b->y1 + 1;
        const float w = x1 - x0, h = y1 - y0;
        if (b->kind == K_STICK || b->kind == K_DPAD_ALL) {
            const int base = b->kind == K_DPAD_ALL ? PIM_UP : b->ctl;   /* up, then down/left/right */
            int c[4];
            if (b->kind == K_DPAD_ALL) { c[0] = PIM_UP; c[1] = PIM_LEFT; c[2] = PIM_DOWN; c[3] = PIM_RIGHT; }
            else { c[0] = base; c[1] = base + 2; c[2] = base + 1; c[3] = base + 3; }   /* LS_UP, LS_LEFT, LS_DOWN, LS_RIGHT */
            if (b->ctl == PIM_RS_UP && mouse_camera) { g_mouse(&sc, b, 0, 0, NULL); render_scene(px, row, b, &sc); continue; }
            char l[4][8]; int any = 0;
            for (int k = 0; k < 4; k++) {
                const int vk = first_key(m, c[k]);
                if (vk >= 0) { key_label((unsigned)vk, 0, l[k], sizeof l[k]); any = 1; } else snprintf(l[k], sizeof l[k], " ");
            }
            if (!any) continue;
            g_cluster(&sc, b, l[0], l[1], l[2], l[3]);
            render_scene(px, row, b, &sc);
            continue;
        }
        const int c = b->ctl;
        const uint32_t mb = m->mouse[c];
        const int vk = first_key(m, c);
        if (mb && (mouse_camera || vk < 0)) {
            const int bit = first_bit(mb);
            int button = 0, wheel = 0; char extra[8] = "";
            if (mb & PIM_MOUSE_WHEEL_UP) wheel = 1;
            else if (mb & PIM_MOUSE_WHEEL_DOWN) wheel = 2;
            if (bit == 0) button = 1; else if (bit == 1) button = 2; else if (bit == 2) button = 4;
            else if (bit >= 3 && bit < 8) snprintf(extra, sizeof extra, "%d", bit + 1);
            g_mouse(&sc, b, wheel ? 0 : button, wheel, extra);
            render_scene(px, row, b, &sc);
            continue;
        }
        if (vk < 0) continue;
        char l[12];
        const int wide = w > h * 1.15f;
        key_label((unsigned)vk, wide, l, sizeof l);
        if (b->kind == K_STARTSEL) g_key(&sc, x0, y0 + h * 0.18f, x1, y1 - h * 0.18f, l, h * 0.42f);
        else if (b->kind == K_SHOULDER) g_key(&sc, x0, y0, x1, y1, l, h * 0.62f);
        else if (b->kind == K_STICK_CLICK) g_key(&sc, x0 + 2, y0 + 2, x1 - 2, y1 - 2, l, h * 0.55f);
        else g_key(&sc, x0 + 0.5f, y0 + 0.5f, x1 - 0.5f, y1 - 0.5f, l, h * 0.58f);
        render_scene(px, row, b, &sc);
    }
}

void rsx_button_icons_draw(uint8_t* rgba, uint32_t row_bytes, int mode,
                           const void* map, int mouse_camera)
{
    PadInputMap def;
    const PadInputMap* m = (const PadInputMap*)map;
    if (!m) { pim_defaults(&def); m = &def; }
    if (mode == PS3_ICONS_XBOX) draw_xbox(rgba, row_bytes, m);
    else if (mode == PS3_ICONS_KEYBOARD) draw_keyboard(rgba, row_bytes, m, mouse_camera);
}

/* ---- the mode --------------------------------------------------------------- */

static int s_menu_mode = -1;            /* the in-game menu's choice, -1 none */
static int s_knob_mode = PS3_ICONS_AUTO;
static int s_knob_dirty = 1;
static int s_effective = PS3_ICONS_PLAYSTATION;
static uint32_t s_gen, s_key;
static PadInputMap s_map;               /* the bindings the atlas was drawn with */
static int s_mouse_cam = 1;

static int parse_mode(const char* e)
{
    if (!e || !*e) return PS3_ICONS_AUTO;
    if (!strcasecmp(e, "playstation") || !strcasecmp(e, "ps") || !strcasecmp(e, "ps3") || !strcmp(e, "1")) return PS3_ICONS_PLAYSTATION;
    if (!strcasecmp(e, "xbox") || !strcmp(e, "2")) return PS3_ICONS_XBOX;
    if (!strcasecmp(e, "keyboard") || !strcasecmp(e, "kb") || !strcmp(e, "3")) return PS3_ICONS_KEYBOARD;
    return PS3_ICONS_AUTO;
}
static void knob_changed(int id, int v) { (void)id; (void)v; __atomic_store_n(&s_knob_dirty, 1, __ATOMIC_RELAXED); }

void ps3_button_icons_set_mode(int mode)
{
    __atomic_store_n(&s_menu_mode, mode < 0 || mode > 3 ? -1 : mode, __ATOMIC_RELAXED);
}
int ps3_button_icons_mode_setting(void)
{
    const int m = __atomic_load_n(&s_menu_mode, __ATOMIC_RELAXED);
    if (m >= 0) return m;
    char buf[PS3_TOGGLE_STR];
    return parse_mode(ps3_toggle_str(PS3T_BUTTON_ICONS, buf, sizeof buf));
}
int ps3_button_icons_effective(void) { return __atomic_load_n(&s_effective, __ATOMIC_RELAXED); }

/* SDL_GameControllerType + 1 -> the glyph family. */
static int family_of(int src)
{
    switch (src - 1) {
    case 3: case 4: case 7:          /* PS3, PS4, PS5 */
    case 5: case 11: case 12: case 13:   /* Switch Pro / Joy-Cons: the original glyphs for now */
        return PS3_ICONS_PLAYSTATION;
    default: return PS3_ICONS_XBOX;  /* Xbox 360/One/Series and the rest (ABXY labels) */
    }
}

static int resolve(int setting)
{
    if (setting != PS3_ICONS_AUTO) return setting;
    const int src = g_ps3_pad_icons.source;
    if (src > 0) return family_of(src);
    if (src == 0) return PS3_ICONS_KEYBOARD;
    const int con = g_ps3_pad_icons.connected;
    if (con > 0) return family_of(con);
    static int headless = -1;
    if (headless < 0) { const char* hl = getenv("PS3RECOMP_METAL_HEADLESS"); headless = hl && *hl && *hl != '0'; }
    return headless ? PS3_ICONS_PLAYSTATION : PS3_ICONS_KEYBOARD;
}

uint32_t rsx_button_icons_frame(void)
{
    static int init;
    if (!init) { init = 1; ps3_toggle_watch(PS3T_BUTTON_ICONS, knob_changed); }
    if (__atomic_exchange_n(&s_knob_dirty, 0, __ATOMIC_RELAXED)) {
        char buf[PS3_TOGGLE_STR];
        s_knob_mode = parse_mode(ps3_toggle_str(PS3T_BUTTON_ICONS, buf, sizeof buf));
    }
    const int menu = __atomic_load_n(&s_menu_mode, __ATOMIC_RELAXED);
    const int eff = resolve(menu >= 0 ? menu : s_knob_mode);
    uint32_t key = (uint32_t)eff * 0x9E3779B1u;
    if (eff != PS3_ICONS_PLAYSTATION) {
        /* The glyphs follow the bindings (and the mouse camera, for the keys). */
        if (g_ps3_pad_icons.input_map) g_ps3_pad_icons.input_map(&s_map); else pim_defaults(&s_map);
        s_mouse_cam = g_ps3_pad_icons.mouse_camera ? g_ps3_pad_icons.mouse_camera() : 1;
        uint32_t h = 2166136261u;
        const uint8_t* p = (const uint8_t*)s_map.key;
        for (size_t i = 0; i < sizeof s_map.key; i++) { h ^= p[i]; h *= 16777619u; }
        p = (const uint8_t*)s_map.mouse; for (size_t i = 0; i < sizeof s_map.mouse; i++) { h ^= p[i]; h *= 16777619u; }
        p = (const uint8_t*)s_map.pad;   for (size_t i = 0; i < sizeof s_map.pad; i++)   { h ^= p[i]; h *= 16777619u; }
        key ^= h ^ (uint32_t)s_mouse_cam;
    }
    if (key != s_key || !s_gen) {
        if (s_gen && eff != s_effective)
            fprintf(stderr, "[button icons] %s\n", eff == PS3_ICONS_XBOX ? "xbox" : eff == PS3_ICONS_KEYBOARD ? "keyboard" : "playstation");
        s_key = key; s_gen++;
        __atomic_store_n(&s_effective, eff, __ATOMIC_RELAXED);
    }
    return s_gen;
}

int rsx_button_icons_upload(uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels,
                            uint8_t* rgba, uint32_t row_bytes)
{
    if ((fmt & 0x9Fu) != 0x85u || w != ATLAS_W || h != ATLAS_H || !rgba || row_bytes < ATLAS_W * 4u) return 0;
    if (!is_atlas(rgba, row_bytes)) return 0;
    static int logged;
    if (!logged) { logged = 1; fprintf(stderr, "[button icons] prompt atlas found (mode %s)\n",
                                       s_effective == PS3_ICONS_XBOX ? "xbox" : s_effective == PS3_ICONS_KEYBOARD ? "keyboard" : "playstation"); }
    if (levels > 1) return 1;   /* (one level in this title; a mip chain would need every level redrawn) */
    if (s_effective == PS3_ICONS_XBOX) draw_xbox(rgba, row_bytes, &s_map);
    else if (s_effective == PS3_ICONS_KEYBOARD) draw_keyboard(rgba, row_bytes, &s_map, s_mouse_cam);
    return 1;
}
