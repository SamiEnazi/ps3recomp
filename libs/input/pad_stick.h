/*
 * ps3recomp - analog stick and trigger shaping for the host pad (cellPad.c)
 *
 * A host stick (SDL: -32768..32767 per axis, +y = down) becomes a PS3 stick
 * (0..255 per axis, 128 = centre, 0 = left/up). Per stick:
 *
 *   deadzone       radial inner dead zone, 0..0.9 of full deflection. Inside
 *                  it the stick reads exactly 128,128 (worn sticks, drift);
 *                  outside it the magnitude is rescaled so the first step
 *                  past it is small, not a jump.
 *   anti_deadzone  where the output starts once past the dead zone, 0..0.9.
 *                  The game applies its OWN dead zone to what it receives;
 *                  a value near it makes small moves register at once.
 *   outer          the deflection that already counts as full, 0.5..1.0
 *                  (sticks that never quite reach their corners).
 *   squircle       0..1: how far the round gate is pushed toward a square.
 *                  A DualShock 3 reports nearly full X AND full Y on a
 *                  diagonal; modern pads report a circle (0.71 each), so a
 *                  game that reads the axes separately (a camera's yaw and
 *                  pitch speeds) turns slower on diagonals. 0 = untouched.
 *
 * Everything here is plain arithmetic on its inputs (no SDL, no globals), so
 * it builds into a standalone test: libs/input/tests/test_pad_stick.c.
 */
#ifndef PS3RECOMP_PAD_STICK_H
#define PS3RECOMP_PAD_STICK_H

#include <math.h>

typedef struct {
    float deadzone;       /* 0..0.9 */
    float anti_deadzone;  /* 0..0.9 */
    float outer;          /* 0.5..1 */
    float squircle;       /* 0..1 */
} PadStickShape;

/* Defaults: a small radial dead zone (it only has to hide jitter and mild
 * drift: the game keeps its own dead zone after ours), a little saturation,
 * and half way to the DualShock 3's square-ish range. */
#define PAD_STICK_DEFAULT_DEADZONE   0.08f
#define PAD_STICK_DEFAULT_ANTI       0.0f
#define PAD_STICK_DEFAULT_OUTER      0.97f
#define PAD_STICK_DEFAULT_SQUIRCLE   0.5f
#define PAD_TRIGGER_DEFAULT_THRESHOLD 0.12f   /* digital L2/R2 press point */
#define PAD_TRIGGER_DEFAULT_DEADZONE  0.02f   /* analog L2/R2 below this read 0 */

static inline float pad_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Sanitise a shape (any field out of range comes back clamped). */
static inline PadStickShape pad_stick_shape_fix(PadStickShape s)
{
    s.deadzone = pad_clampf(s.deadzone, 0.0f, 0.9f);
    s.anti_deadzone = pad_clampf(s.anti_deadzone, 0.0f, 0.9f);
    s.outer = pad_clampf(s.outer, 0.5f, 1.0f);
    if (s.outer < s.deadzone + 0.05f) s.outer = s.deadzone + 0.05f;
    s.squircle = pad_clampf(s.squircle, 0.0f, 1.0f);
    return s;
}

/* -1..1 -> 0..255 with 0 at 128 exactly: -1 = 0, +1 = 255. (Not 255*(v+1)/2:
 * that puts the centre at 127.5, which rounds to 127 or 128 depending on the
 * sign of the jitter.) */
static inline int pad_unit_to_byte(float v)
{
    v = pad_clampf(v, -1.0f, 1.0f);
    const int o = v >= 0.0f ? 128 + (int)lrintf(v * 127.0f) : 128 + (int)lrintf(v * 128.0f);
    return o < 0 ? 0 : o > 255 ? 255 : o;
}

/* Disc -> square, the elliptical grid mapping (Fong, "Analytical Methods for
 * Squaring the Disc", 2014): (r cos t, r sin t) on the unit disc maps onto the
 * unit square with the gate's edge going to the square's edge. */
static inline void pad_disc_to_square(float u, float v, float* x, float* y)
{
    const float u2 = u * u, v2 = v * v, k = 2.0f * 1.41421356f;
    const float tx = 2.0f + u2 - v2, ty = 2.0f - u2 + v2;
    const float ax = tx + k * u, bx = tx - k * u, ay = ty + k * v, by = ty - k * v;
    *x = 0.5f * sqrtf(ax > 0.0f ? ax : 0.0f) - 0.5f * sqrtf(bx > 0.0f ? bx : 0.0f);
    *y = 0.5f * sqrtf(ay > 0.0f ? ay : 0.0f) - 0.5f * sqrtf(by > 0.0f ? by : 0.0f);
}

/* One stick: raw SDL axes in, PS3 bytes out. */
static inline void pad_stick_shape_apply(int raw_x, int raw_y, const PadStickShape* s, int* out_x, int* out_y)
{
    float x = pad_clampf((float)raw_x / 32767.0f, -1.0f, 1.0f);
    float y = pad_clampf((float)raw_y / 32767.0f, -1.0f, 1.0f);
    const float m = sqrtf(x * x + y * y);
    if (m <= s->deadzone || m < 1e-6f) { *out_x = *out_y = 128; return; }
    float t = (m - s->deadzone) / (s->outer - s->deadzone);
    t = pad_clampf(t, 0.0f, 1.0f);
    const float mo = s->anti_deadzone + (1.0f - s->anti_deadzone) * t;   /* 0..1 */
    x = x / m * mo;
    y = y / m * mo;
    if (s->squircle > 0.0f) {
        float sx, sy;
        pad_disc_to_square(x, y, &sx, &sy);
        x += (sx - x) * s->squircle;
        y += (sy - y) * s->squircle;
    }
    *out_x = pad_unit_to_byte(x);
    *out_y = pad_unit_to_byte(y);
}

/* A trigger (SDL: 0..32767) -> the PS3's 0..255 pressure, with a small dead
 * zone so a trigger that rests a hair off zero reads 0. */
static inline int pad_trigger_shape(int raw, float deadzone)
{
    float v = pad_clampf((float)raw / 32767.0f, 0.0f, 1.0f);
    if (v <= deadzone) return 0;
    v = (v - deadzone) / (1.0f - deadzone);
    const int o = (int)lrintf(v * 255.0f);
    return o < 0 ? 0 : o > 255 ? 255 : o;
}

#endif /* PS3RECOMP_PAD_STICK_H */
