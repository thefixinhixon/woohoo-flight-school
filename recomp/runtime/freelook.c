/* Free look (optional, off by default; not in the original game): the right stick orbits the chase camera around the
 * vehicle. Each vehicle's chase camera update runs once per frame with the camera object in r0 (arm2c hooks it before and
 * after): plane f_00149aa4, rocket belt f_001e34f0, hang glider f_001ffb8c. After it, the camera's world matrices are rotated
 * around the vehicle by the stick's angle; before the next update the game's own matrices are put back, so its smoothing
 * and collision never see the change. Everything that runs between the two (visibility culling, rendering, the sun flare)
 * sees the orbited camera.
 *
 * Camera objects (one or two per vehicle, e.g. 0x096f351c for the trial flight's plane), told apart by their vtable:
 * +0x08 the chase parameters (chase_dist first, then chase_hit, chase_dy, chase_pitch, focus_dist, ...), then 3x4 world
 * matrices (rows = right, up, forward in world space, then the position; forward points at the vehicle) at +0x68 and
 * +0x98, for the plane also +0x10c and +0x13c (copies of the camera) and +0xdc (the vehicle). For the others the orbit
 * centre is the point chase_dist ahead of the camera. */
#include "cpu.h"
#include "runtime.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NCAM 4
typedef struct { uint32_t vtable; int nmat; uint32_t mat[NCAM]; uint32_t target; } CamClass;   /* target 0: chase_dist ahead */
static const CamClass classes[] = {
    { 0x2d24ac, 4, { 0x68, 0x98, 0x10c, 0x13c }, 0xdc },   /* plane */
    { 0x2d27a8, 2, { 0x68, 0x98 }, 0 },                    /* rocket belt */
    { 0x2e98e8, 2, { 0x68, 0x98 }, 0 },                    /* hang glider */
};
#define MAX_DIST 60.0f            /* camera farther than this from the vehicle: not a chase view, left alone */
#define YAW_RANGE 3.14159265f     /* full stick: around to the front */
#define PITCH_UP 1.0471976f       /* camera up to 60 degrees above its normal place */
#define PITCH_DOWN 1.6580628f     /* and 95 degrees below: under the vehicle, looking up (see freelook_post for the limits) */
#define MIN_ELEV -1.4835299f      /* the camera at most 85 degrees below the vehicle, so the view never flips over */
#define SEA_Y 0.5f                /* and above the sea (world y 0) */
#define EASE_S 0.25f              /* time constant of the angle following the stick (and of the return) */
#define DEADZONE 8000

static atomic_int enabled, stick_x, stick_y;
static uint32_t cur_obj, saved_obj; static const CamClass *cur_cls, *saved_cls; static int saved; static float saved_m[NCAM][12];
static float yaw, pitch; static double last_t;

void freelook_enable(int on) { atomic_store(&enabled, on); }
int freelook_enabled(void) { return atomic_load(&enabled); }
void freelook_stick(int x, int y) { atomic_store(&stick_x, x); atomic_store(&stick_y, y); }   /* -32768..32767, y up positive */

static void rdm(uint32_t a, float *m) { memcpy(m, g_mem + a, 48); }
static void wrm(uint32_t a, const float *m) { memcpy(g_mem + a, m, 48); }

void freelook_pre(CPU *c) {
    cur_obj = c->r[0]; cur_cls = NULL;
    uint32_t vt; memcpy(&vt, g_mem + cur_obj, 4);
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) if (classes[i].vtable == vt) cur_cls = &classes[i];
    if (saved && saved_obj == cur_obj) for (int i = 0; i < saved_cls->nmat; i++) wrm(cur_obj + saved_cls->mat[i], saved_m[i]);
    saved = 0;
}

static float stick_axis(int v) {
    if (v > -DEADZONE && v < DEADZONE) return 0;
    float f = (float)(v > 0 ? v - DEADZONE : v + DEADZONE) / (32767.f - DEADZONE);
    f = f > 1 ? 1 : f < -1 ? -1 : f;
    return f * fabsf(f);   /* squared: fine control near the centre, the full range at the edge */
}
/* v = rotation about unit axis k by angle a (Rodrigues) */
static void rot(float *v, const float *k, float a) {
    float c = cosf(a), s = sinf(a), d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    float x[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + x[i] * s + k[i] * d * (1 - c);
}

void freelook_post(CPU *c) {
    (void)c;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec * 1e-9, dt = last_t ? now - last_t : 0; last_t = now;
    if (dt > 0.25) dt = 0.25;
    float sx = 0, sy = 0;
    if (atomic_load(&enabled)) { sx = stick_axis(atomic_load(&stick_x)); sy = stick_axis(atomic_load(&stick_y)); }
    static const char *test = (const char *)1; if (test == (const char *)1) test = getenv("PWR_FREELOOK_STICK");
    if (test) { int x = 0, y = 0; sscanf(test, "%d,%d", &x, &y); sx = stick_axis(x); sy = stick_axis(y); }   /* headless tests: a fixed stick */
    /* like most third-person games: stick right looks right (the camera swings left around the vehicle), stick up looks
       up (the camera goes lower). yaw > 0 turns about world up; pitch > 0 raises the camera */
    float ty = sx * YAW_RANGE, tp = sy > 0 ? -sy * PITCH_DOWN : -sy * PITCH_UP;
    float k = 1.0f - expf(-(float)dt / EASE_S);
    yaw += (ty - yaw) * k; pitch += (tp - pitch) * k;
    if (fabsf(yaw) < 1e-4f && fabsf(pitch) < 1e-4f) { yaw = pitch = 0; return; }

    if (!cur_cls) return;
    const CamClass *K = cur_cls;
    float m[NCAM][12], t[3];
    for (int i = 0; i < K->nmat; i++) rdm(cur_obj + K->mat[i], m[i]);
    if (K->target) { float tgt[12]; rdm(cur_obj + K->target, tgt); memcpy(t, tgt + 9, sizeof t); }
    else {   /* the point the camera looks at, chase_dist ahead */
        float dist; memcpy(&dist, g_mem + cur_obj + 8, 4);
        if (!(dist > 0.1f && dist < MAX_DIST)) return;
        for (int j = 0; j < 3; j++) t[j] = m[0][9 + j] + m[0][6 + j] * dist;
    }
    float dx = m[0][9] - t[0], dy = m[0][10] - t[1], dz = m[0][11] - t[2];
    if (!(dx * dx + dy * dy + dz * dz < MAX_DIST * MAX_DIST)) return;   /* also NaN */
    /* below the vehicle: down to MIN_ELEV, and never into the sea. The game's camera keeps out of the ground, this one
       does not know where the island's ground is, so it can go through a hillside when flying low over land */
    float d = sqrtf(dx * dx + dy * dy + dz * dz), e0 = d > 1e-3f ? asinf(dy / d) : 0;
    float s_min = d > 1e-3f ? (SEA_Y - t[1]) / d : 0, e_min = s_min >= 1 ? 1.5707963f : s_min > sinf(MIN_ELEV) ? asinf(s_min) : MIN_ELEV;
    float p_eff = e0 + pitch < e_min ? e_min - e0 : pitch;
    if (p_eff > pitch && pitch >= 0) p_eff = pitch;   /* looking down: as the game's own camera allows */
    memcpy(saved_m, m, sizeof m); saved_obj = cur_obj; saved_cls = K; saved = 1;
    static const float up[3] = { 0, 1, 0 };
    for (int i = 0; i < K->nmat; i++) {
        float *M = m[i], right[3] = { M[0], M[1], M[2] };
        float n = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (!(n > 1e-3f)) continue;
        for (int j = 0; j < 3; j++) right[j] /= n;
        /* pitch about the camera's right axis, then yaw about world up; the rows (right, up, forward: a right-handed basis)
           turn with the offset, so the vehicle stays where it was on screen */
        float p[3] = { M[9] - t[0], M[10] - t[1], M[11] - t[2] };
        rot(p, right, p_eff); rot(p, up, yaw);
        for (int r = 0; r < 3; r++) { rot(M + 3 * r, right, p_eff); rot(M + 3 * r, up, yaw); }
        for (int j = 0; j < 3; j++) M[9 + j] = t[j] + p[j];
        wrm(cur_obj + K->mat[i], M);
    }
}
