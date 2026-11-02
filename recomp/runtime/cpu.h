/* Guest CPU model for recompiled Pilotwings Resort ARM11 code.
 * Every recompiled function has the signature void f_XXXXXXXX(CPU *c).
 * Guest memory is a single 4 GiB host reservation: guest address a lives at g_mem + a. */
#ifndef PWR_CPU_H
#define PWR_CPU_H
#include <stdint.h>
#include <string.h>

typedef struct CPU {
    uint32_t r[16];
    uint32_t n, z, cf, v, q;      /* APSR flags, each 0 or 1 */
    uint32_t ge;                  /* APSR.GE[3:0] for ARMv6 SIMD */
    uint32_t s[32];               /* VFPv2 single registers; d[i] = s[2i] (low) : s[2i+1] (high) */
    uint32_t fpscr;
    uint32_t tls;                 /* thread-local storage base (mrc p15,0,rX,c13,c0,3) */
    uint32_t excl_addr, excl_val; /* LDREX/STREX monitor */
    int excl_valid;
} CPU;

typedef void (*GuestFn)(CPU *c);

extern uint8_t *g_mem;

/* runtime services (implemented in runtime/) */
void rt_svc(CPU *c, uint32_t num, uint32_t pc);
void rt_call_indirect(CPU *c, uint32_t target, uint32_t pc);   /* call: lr already set */
void rt_unimpl(CPU *c, uint32_t pc, uint32_t insn);
void rt_bkpt(CPU *c, uint32_t pc);
void rt_ret_mismatch(CPU *c, uint32_t pc, uint32_t got, uint32_t want);
void rt_nan(CPU *c, uint32_t pc);                       /* a VFP op produced NaN (diagnostics) */

/* ---- memory ---- */
/* debug builds with -DPWR_WATCH (cmake -DPWR_WATCH=ON): guest stores into PWR_WATCH=<va>,<len> report the writing
   function (runtime.c rt_watch_hit) */
#ifdef PWR_WATCH
extern uint32_t g_watch_lo, g_watch_len;
void rt_watch_hit(uint32_t a, uint32_t v, int size);
#define WATCH(a, v, n) do { if ((uint32_t)((a) - g_watch_lo) < g_watch_len) rt_watch_hit(a, v, n); } while (0)
#else
#define WATCH(a, v, n) do { } while (0)
#endif
static inline uint32_t rd8(uint32_t a)  { return g_mem[a]; }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline void wr8(uint32_t a, uint32_t v)  { WATCH(a, v, 1); g_mem[a] = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { WATCH(a, v, 2); uint16_t x = (uint16_t)v; memcpy(g_mem + a, &x, 2); }
static inline void wr32(uint32_t a, uint32_t v) { WATCH(a, v, 4); memcpy(g_mem + a, &v, 4); }

/* ---- flags / ALU helpers ---- */
static inline void set_nz(CPU *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }
static inline uint32_t adc_flags(CPU *c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t w = (uint64_t)a + b + cin; uint32_t r = (uint32_t)w;
    c->n = r >> 31; c->z = r == 0; c->cf = (uint32_t)(w >> 32); c->v = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
/* a - b - !cin  ==  a + ~b + cin */
static inline uint32_t sbc_flags(CPU *c, uint32_t a, uint32_t b, uint32_t cin) { return adc_flags(c, a, ~b, cin); }

static inline uint32_t ror32(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

/* shift by register amount (low byte), returning result and carry-out in *co (unchanged when amount is 0) */
static inline uint32_t lsl_rs(uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xff; if (!s) return v;
    if (s < 32) { *co = (v >> (32 - s)) & 1; return v << s; }
    *co = s == 32 ? (v & 1) : 0; return 0;
}
static inline uint32_t lsr_rs(uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xff; if (!s) return v;
    if (s < 32) { *co = (v >> (s - 1)) & 1; return v >> s; }
    *co = s == 32 ? (v >> 31) : 0; return 0;
}
static inline uint32_t asr_rs(uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xff; if (!s) return v;
    if (s < 32) { *co = ((int32_t)v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
    *co = v >> 31; return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ror_rs(uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xff; if (!s) return v;
    s &= 31; if (!s) { *co = v >> 31; return v; }
    uint32_t r = ror32(v, s); *co = r >> 31; return r;
}

static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

static inline int32_t sat_s(int64_t v, int bits, CPU *c) {
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
    if (v > hi) { c->q = 1; return (int32_t)hi; }
    if (v < lo) { c->q = 1; return (int32_t)lo; }
    return (int32_t)v;
}
static inline uint32_t sat_u(int64_t v, int bits, CPU *c) {
    int64_t hi = ((int64_t)1 << bits) - 1;
    if (v > hi) { c->q = 1; return (uint32_t)hi; }
    if (v < 0) { c->q = 1; return 0; }
    return (uint32_t)v;
}

/* ---- exclusive access: STREX succeeds iff memory still holds the value seen by LDREX (host CAS) ---- */
static inline uint32_t ldrex32(CPU *c, uint32_t a) { uint32_t v = rd32(a); c->excl_addr = a; c->excl_val = v; c->excl_valid = 1; return v; }
static inline uint32_t strex32(CPU *c, uint32_t a, uint32_t v) {
    if (!c->excl_valid || c->excl_addr != a) return 1;
    c->excl_valid = 0;
    uint32_t expect = c->excl_val;
    return __atomic_compare_exchange_n((uint32_t *)(g_mem + a), &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
static inline uint32_t ldrexb(CPU *c, uint32_t a) { uint32_t v = rd8(a); c->excl_addr = a; c->excl_val = v; c->excl_valid = 1; return v; }
static inline uint32_t strexb(CPU *c, uint32_t a, uint32_t v) {
    if (!c->excl_valid || c->excl_addr != a) return 1;
    c->excl_valid = 0; uint8_t expect = (uint8_t)c->excl_val;
    return __atomic_compare_exchange_n(g_mem + a, &expect, (uint8_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
static inline uint32_t ldrexh(CPU *c, uint32_t a) { uint32_t v = rd16(a); c->excl_addr = a; c->excl_val = v; c->excl_valid = 1; return v; }
static inline uint32_t strexh(CPU *c, uint32_t a, uint32_t v) {
    if (!c->excl_valid || c->excl_addr != a) return 1;
    c->excl_valid = 0; uint16_t expect = (uint16_t)c->excl_val;
    return __atomic_compare_exchange_n((uint16_t *)(g_mem + a), &expect, (uint16_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}

/* ---- VFP ---- */
static inline float  fS(CPU *c, int i) { float f; memcpy(&f, &c->s[i], 4); return f; }
static inline void   sS(CPU *c, int i, float f) { memcpy(&c->s[i], &f, 4); }
static inline double fD(CPU *c, int i) { double d; memcpy(&d, &c->s[2 * i], 8); return d; }
static inline void   sD(CPU *c, int i, double d) { memcpy(&c->s[2 * i], &d, 8); }
static inline void vfp_cmp(CPU *c, double a, double b) {
    uint32_t f = (a != a || b != b) ? 0x3u : a == b ? 0x6u : a < b ? 0x8u : 0x2u;  /* NZCV */
    c->fpscr = (c->fpscr & 0x0fffffffu) | (f << 28);
}
static inline int32_t  vcvt_s32(double x) { if (x != x) return 0; if (x >= 2147483647.0) return 2147483647; if (x <= -2147483648.0) return (-2147483647 - 1); return (int32_t)x; }
static inline uint32_t vcvt_u32(double x) { if (x != x || x <= 0) return 0; if (x >= 4294967295.0) return 4294967295u; return (uint32_t)x; }
/* round-to-nearest-even variants for VCVTR (FPSCR rounding mode, assumed RN) */
static inline int32_t  vcvtr_s32(double x) { return vcvt_s32(__builtin_nearbyint(x)); }
static inline uint32_t vcvtr_u32(double x) { return vcvt_u32(__builtin_nearbyint(x)); }

#endif
