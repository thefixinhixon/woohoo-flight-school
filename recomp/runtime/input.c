/* hid:USER input: host keyboard/mouse (window events only, never raw devices) -> the HID shared memory the game reads.
 * Shared memory layout (3dbrew "HID Shared Memory"; inferred, not yet checked against the game's reader):
 *   0x00 pad:   s64 tick of last index reset, s64 previous, u32 latest index, ..., 0x1C u32 current buttons,
 *               0x20 s16 circle x, s16 circle y, 0x28 8 entries {u32 current, pressed, released; s16 cx, cy}
 *   0xA8 touch: s64 tick, s64 previous, u32 latest index, ..., 0xC0 raw {u16 x, u16 y, u32 valid},
 *               0xC8 8 entries {u16 x, u16 y, u32 valid}
 * The host updates once per VBlank and signals the PAD0 event. */
#include "input.h"
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { B_A = 1 << 0, B_B = 1 << 1, B_SELECT = 1 << 2, B_START = 1 << 3, B_RIGHT = 1 << 4, B_LEFT = 1 << 5, B_UP = 1 << 6, B_DOWN = 1 << 7,
       B_R = 1 << 8, B_L = 1 << 9, B_X = 1 << 10, B_Y = 1 << 11, B_TOUCH = 1 << 20,
       B_CRIGHT = 1u << 28, B_CLEFT = 1u << 29, B_CUP = 1u << 30, B_CDOWN = 1u << 31 };
#define CIRCLE_MAX 0x9C

static pthread_mutex_t I = PTHREAD_MUTEX_INITIALIZER;
static KObject *shmem, *pad_event;
static uint32_t keys, prev_buttons;          /* digital buttons from keys */
static int stick_l, stick_r, stick_u, stick_d;
static int touch_down, touch_x, touch_y;
static uint32_t pad_keys;                    /* digital buttons from a gamepad */
static int pad_x, pad_y;                     /* gamepad stick, -32768..32767 */

void input_bind(KObject *mem, KObject *pad0) { pthread_mutex_lock(&I); shmem = mem; pad_event = pad0; pthread_mutex_unlock(&I); }

/* keyboard layout (README): arrows = circle pad, I/J/K/L = D-pad, X = A, Z = B, S = X, A = Y,
 * Q = L, W = R, Return = Start, Backspace = Select; left mouse on the bottom screen = touch */
void input_key(int sym, int down) {
    uint32_t b = 0;
    switch (sym) {
    case SDLK_x: b = B_A; break;      case SDLK_z: b = B_B; break;
    case SDLK_s: b = B_X; break;      case SDLK_a: b = B_Y; break;
    case SDLK_q: b = B_L; break;      case SDLK_w: b = B_R; break;
    case SDLK_RETURN: b = B_START; break; case SDLK_BACKSPACE: b = B_SELECT; break;
    case SDLK_i: b = B_UP; break;     case SDLK_k: b = B_DOWN; break;
    case SDLK_j: b = B_LEFT; break;   case SDLK_l: b = B_RIGHT; break;
    case SDLK_LEFT: stick_l = down; return;  case SDLK_RIGHT: stick_r = down; return;
    case SDLK_UP: stick_u = down; return;    case SDLK_DOWN: stick_d = down; return;
    default: return;
    }
    pthread_mutex_lock(&I);
    keys = down ? keys | b : keys & ~b;
    pthread_mutex_unlock(&I);
}

/* gamepad (video.c opens it only without --input=none): positional layout, south = 3DS B, east = A, west = Y, north = X */
void input_pad_button(int b, int down) {
    static const uint32_t map[12] = { B_B, B_A, B_Y, B_X, B_L, B_R, B_SELECT, B_START, B_UP, B_DOWN, B_LEFT, B_RIGHT };
    if (b < 0 || b >= 12) return;
    pthread_mutex_lock(&I);
    pad_keys = down ? pad_keys | map[b] : pad_keys & ~map[b];
    pthread_mutex_unlock(&I);
}

void input_pad_stick(int x, int y) { pthread_mutex_lock(&I); pad_x = x; pad_y = y; pthread_mutex_unlock(&I); }

void input_touch(int down, int x, int y) {
    static int tr = -1; if (tr < 0) tr = getenv("PWR_INPUT_TRACE") != NULL;
    if (tr) fprintf(stderr, "input: touch %s %d,%d\n", down ? "down" : "up", x, y);
    pthread_mutex_lock(&I);
    touch_down = down && x >= 0 && x < 320 && y >= 0 && y < 240;
    if (touch_down) { touch_x = x; touch_y = y; }
    pthread_mutex_unlock(&I);
}

void input_release_all(void) {
    pthread_mutex_lock(&I); keys = pad_keys = 0; stick_l = stick_r = stick_u = stick_d = 0; pad_x = pad_y = 0; touch_down = 0; pthread_mutex_unlock(&I);
}
int input_stick_active(void) {
    pthread_mutex_lock(&I); int a = stick_l || stick_r || stick_u || stick_d || abs(pad_x) > 6000 || abs(pad_y) > 6000; pthread_mutex_unlock(&I);
    return a;
}
uint32_t input_buttons(void) { pthread_mutex_lock(&I); uint32_t b = keys | pad_keys; pthread_mutex_unlock(&I); return b; }

void input_update(void) {
    pthread_mutex_lock(&I);
    uint32_t base = shmem ? shmem->addr : 0;
    if (base < 0x1000) { pthread_mutex_unlock(&I); return; }   /* not mapped by the game yet */
    int cx = (stick_r - stick_l) * CIRCLE_MAX, cy = (stick_u - stick_d) * CIRCLE_MAX;
    if (!cx && !cy && (abs(pad_x) > 6000 || abs(pad_y) > 6000)) { cx = pad_x * CIRCLE_MAX / 32768; cy = pad_y * CIRCLE_MAX / 32768; }
    uint32_t btn = keys | pad_keys;
    if (cx > 0x28) btn |= B_CRIGHT; if (cx < -0x28) btn |= B_CLEFT;
    if (cy > 0x28) btn |= B_CUP;    if (cy < -0x28) btn |= B_CDOWN;
    if (touch_down) btn |= B_TOUCH;
    uint64_t now = k_ticks();
    /* pad ring */
    uint32_t idx = (rd32(base + 0x10) + 1) & 7;
    if (idx == 0) { uint64_t t = rd32(base) | (uint64_t)rd32(base + 4) << 32; wr32(base + 8, (uint32_t)t); wr32(base + 12, (uint32_t)(t >> 32)); wr32(base, (uint32_t)now); wr32(base + 4, (uint32_t)(now >> 32)); }
    uint32_t e = base + 0x28 + idx * 0x10;
    wr32(e, btn); wr32(e + 4, btn & ~prev_buttons); wr32(e + 8, ~btn & prev_buttons);
    wr16(e + 12, (uint32_t)cx); wr16(e + 14, (uint32_t)cy);
    wr32(base + 0x1C, btn); wr16(base + 0x20, (uint32_t)cx); wr16(base + 0x22, (uint32_t)cy);
    wr32(base + 0x10, idx);
    prev_buttons = btn;
    /* touch ring */
    uint32_t tb = base + 0xA8, ti = (rd32(tb + 0x10) + 1) & 7;
    if (ti == 0) { uint64_t t = rd32(tb) | (uint64_t)rd32(tb + 4) << 32; wr32(tb + 8, (uint32_t)t); wr32(tb + 12, (uint32_t)(t >> 32)); wr32(tb, (uint32_t)now); wr32(tb + 4, (uint32_t)(now >> 32)); }
    uint32_t te = tb + 0x20 + ti * 8;
    wr16(te, touch_down ? (uint32_t)touch_x : 0); wr16(te + 2, touch_down ? (uint32_t)touch_y : 0); wr32(te + 4, (uint32_t)touch_down);
    wr16(tb + 0x18, touch_down ? (uint32_t)touch_x : 0); wr16(tb + 0x1A, touch_down ? (uint32_t)touch_y : 0); wr32(tb + 0x1C, (uint32_t)touch_down);
    wr32(tb + 0x10, ti);
    KObject *ev = pad_event;
    pthread_mutex_unlock(&I);
    if (ev) k_signal(ev);
}
