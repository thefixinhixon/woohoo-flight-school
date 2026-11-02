/* Draw distance (optional, --draw-distance=1..4, 1 = the original game): the free-flight information points and the
 * mission rings fade in at distances the game reads from its art tweaks (common.res/art_twks.val: ring_fade_in_distance
 * 200, i_fade_in_distance 220, text_fade_in_distance 120). arm2c hooks the game's "read a REAL setting by name" methods
 * (f_0019b50c, f_0019b634; r1 = the name, result in s0) and these settings are scaled as they are read. The scene's
 * culling frustum far distance (1000, a literal in f_0020d6cc) is scaled too, below the projection's far plane (5000).
 * Fade distances are read when a flight is set up: a change in the F1 overlay applies from the next flight.
 * The island's background cars and boats (wuhu.trk/carsboats.val) are shown only within 400 of the camera, measured
 * flat (f_0017cf1c: x/z distance squared against the global at 0x2f2ee0, which a static constructor, f_00253e88, sets
 * from a literal); both are scaled, so this one applies at once. */
#include "cpu.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float scale = 1.0f;
static void apply_literals(void);
void drawdist_set(int k) { scale = k < 1 ? 1.0f : k > 4 ? 4.0f : (float)k; if (g_mem) apply_literals(); }   /* also from the F1 overlay */

static const char *const scaled[] = { "ring_fade_in_distance", "i_fade_in_distance", "text_fade_in_distance" };
#define CULL_FAR_LIT 0x20d774u   /* f_0020d6cc: s1 = 1000.0, the culling frustum's far distance */
#define CARS_LIT 0x253ec0u       /* f_00253e88: 400^2, copied to CARS_VAR at start */
#define CARS_VAR 0x2f2ee0u

/* settings getters may nest; the name of each open call */
static uint32_t names[8]; static int depth;
void setting_pre(CPU *c) { if (depth < 8) names[depth] = c->r[1]; depth++; }
void setting_post(CPU *c) {
    if (--depth >= 8 || depth < 0 || scale == 1.0f) { if (depth < 0) depth = 0; return; }
    uint32_t a = names[depth];
    if (a < 0x100000 || a >= 0x20000000) return;
    const char *n = (const char *)(g_mem + a);
    for (size_t i = 0; i < sizeof scaled / sizeof scaled[0]; i++)
        if (!strcmp(n, scaled[i])) { sS(c, 0, fS(c, 0) * scale); return; }
}

/* PWR_PATCH_F32=<hex addr>=<float>[,...]: debug, overwrite single float literals (experiments) */
static void debug_patches(void) {
    const char *e = getenv("PWR_PATCH_F32");
    for (; e && *e; ) {
        char *end; uint32_t a = (uint32_t)strtoul(e, &end, 16);
        if (*end != '=') break;
        float v = strtof(end + 1, &end), old; memcpy(&old, g_mem + a, 4); memcpy(g_mem + a, &v, 4);
        fprintf(stderr, "pwr: patched %08x: %g -> %g\n", a, old, v);
        if (*end != ',') break;
        e = end + 1;
    }
}

static int lits_ok;
static void apply_literals(void) {
    if (!lits_ok) return;
    float far = 1000.0f * scale; if (far > 4900.0f) far = 4900.0f;
    memcpy(g_mem + CULL_FAR_LIT, &far, 4);
    float cars = 400.0f * scale * 400.0f * scale;
    memcpy(g_mem + CARS_LIT, &cars, 4); memcpy(g_mem + CARS_VAR, &cars, 4);
}
/* once code.bin is loaded (the recompiled code reads literals from guest memory) */
void drawdist_apply(void) {
    float far, cars; memcpy(&far, g_mem + CULL_FAR_LIT, 4); memcpy(&cars, g_mem + CARS_LIT, 4);
    lits_ok = far == 1000.0f && cars == 160000.0f;
    if (!lits_ok) fprintf(stderr, "pwr: draw distance: unexpected literals %g at %08x, %g at %08x, left alone\n", far, CULL_FAR_LIT, cars, CARS_LIT);
    apply_literals();
    debug_patches();
}
