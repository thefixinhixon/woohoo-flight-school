/* Minimal host runtime: guest address space, image loading, indirect dispatch, traps. */
#include "cpu.h"
#include "table.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *g_mem;

#ifdef _WIN32
/* Windows: the 4 GiB are only reserved; a page is committed (zero-filled) the first time anything touches it, as Linux's
   MAP_NORESERVE does, so the commit charge stays near what the game uses */
static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)r->ExceptionInformation[1];
    if (a < g_mem || a >= g_mem + ((size_t)1 << 32)) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *chunk = g_mem + (((size_t)(a - g_mem)) & ~(size_t)0xffff);
    return VirtualAlloc(chunk, 0x10000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#endif

int rt_mem_init(void) {
#ifdef _WIN32
    g_mem = VirtualAlloc(NULL, (SIZE_T)1 << 32, MEM_RESERVE, PAGE_READWRITE);
    if (!g_mem) return -1;
    AddVectoredExceptionHandler(1, commit_on_touch);
    return 0;
#else
    void *p = mmap(NULL, (size_t)1 << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    g_mem = p;
    return 0;
#endif
}

/* Windows: commit guest pages before the OS itself reads or writes them (file I/O does not fault into
   commit_on_touch, it just fails) */
void rt_mem_commit(uint32_t addr, size_t len) {
#ifdef _WIN32
    if (!len) return;
    uint64_t lo = addr & ~0xffffull, hi = ((uint64_t)addr + len + 0xffff) & ~0xffffull;
    if (hi > (1ull << 32)) hi = 1ull << 32;
    VirtualAlloc(g_mem + lo, (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE);
#else
    (void)addr; (void)len;
#endif
}

/* write tmp over dst (rename() does not replace an existing file on Windows) */
int rt_replace_file(const char *tmp, const char *dst) {
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(tmp, dst);
#endif
}

int rt_load_image(const char *code_bin, uint32_t base) {
    FILE *f = fopen(code_bin, "rb");
    if (!f) return -1;
    rt_mem_commit(base, 0x1000000);
    size_t n = fread(g_mem + base, 1, 0x1000000, f);
    fclose(f);
    return n ? 0 : -1;
}

GuestFn rt_lookup(uint32_t target) {
    uint32_t key = target & ~1u;
    unsigned lo = 0, hi = g_fn_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t a = g_fn_table[mid].addr & ~1u;
        if (a == key) return g_fn_table[mid].fn;
        if (a < key) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void rt_call_indirect(CPU *c, uint32_t target, uint32_t pc) {
    GuestFn f = rt_lookup(target);
    if (!f) {
        fprintf(stderr, "rt: indirect branch from %08x to unknown target %08x\n", pc, target);
        for (int i = 0; i < 16; i++) fprintf(stderr, "%sr%d=%08x", i % 8 ? " " : "    ", i, c->r[i]), i % 8 == 7 ? fputc('\n', stderr) : 0;
        for (int i = 0; i < 2; i++) {   /* r0 is usually the object; show it and what its first word points to */
            uint32_t a = i ? rd32(c->r[0]) : c->r[0];
            if (a >= 0x100000 && a < 0x20000000) fprintf(stderr, "    [%08x]: %08x %08x %08x %08x %08x %08x\n", a, rd32(a), rd32(a + 4), rd32(a + 8), rd32(a + 12), rd32(a + 16), rd32(a + 20));
        }
        const char *out = getenv("PWR_SEEDS_OUT");   /* collect run-time finds for recomp/extra_seeds.txt */
        FILE *sf = out ? fopen(out, "a") : NULL;
        if (sf) { fprintf(sf, "0x%08x %c  # from %08x\n", target & ~1u, (target & 1) ? 'T' : 'A', pc); fclose(sf); }
        abort();
    }
    f(c);
}

void rt_unimpl(CPU *c, uint32_t pc, uint32_t insn) {
    (void)c;
    fprintf(stderr, "rt: unimplemented/trap at %08x (insn %08x)\n", pc, insn);
    abort();
}

void rt_bkpt(CPU *c, uint32_t pc) {
    (void)c;
    fprintf(stderr, "rt: bkpt at %08x\n", pc);
    abort();
}

void rt_ret_mismatch(CPU *c, uint32_t pc, uint32_t got, uint32_t want) {
    (void)c;
    fprintf(stderr, "rt: non-standard return at %08x: popped %08x, entry lr %08x\n", pc, got, want);
    abort();
}

/* Weak default so tests can link without the kernel layer; the real HLE kernel overrides it. */
__attribute__((weak)) void rt_svc(CPU *c, uint32_t num, uint32_t pc) {
    (void)c;
    fprintf(stderr, "rt: svc %#x at %08x (no kernel layer linked)\n", num, pc);
    abort();
}

/* first distinct guest addresses whose VFP result was NaN, in order. PWR_NAN=<seconds>: start logging after that
 * much run time (0 = from boot), to separate one-time start-up NaNs from ones produced every frame. */
#include <time.h>
void rt_nan(CPU *c, uint32_t pc) {
    static uint32_t seen[64]; static int n, on = -1; static double after, t0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    if (on < 0) { const char *e = getenv("PWR_NAN"); on = e != NULL; after = e ? atof(e) : 0; t0 = now; }
    if (!on || n >= 64 || now - t0 < after) return;
    for (int i = 0; i < n; i++) if (seen[i] == pc) return;
    seen[n++] = pc;
    fprintf(stderr, "rt: nan #%d at %08x (t=%.1fs) r0=%08x r1=%08x lr=%08x  s0=%08x s1=%08x s2=%08x s3=%08x\n", n, pc, now - t0, c->r[0], c->r[1], c->r[14], c->s[0], c->s[1], c->s[2], c->s[3]);
}

/* hook on the game's printf-style debug log (VA 0x210814): r0 = format, r1-r3 then stack = arguments.
 * Supports %d %i %u %x %X %c %s %p %f %g (doubles in even register pairs / 8-aligned stack, AAPCS). PWR_LOG=1. */
void rt_log_printf(CPU *c) {
    static int on = -1; if (on < 0) on = getenv("PWR_LOG") != NULL;
    if (!on) return;
    const char *f = (const char *)(g_mem + c->r[0]);
    char out[1024]; size_t o = 0; int ai = 1; uint32_t sp = c->r[13];
    #define NEXT32() (ai < 4 ? c->r[ai++] : (ai++, rd32(sp + 4u * (uint32_t)(ai - 5))))
    for (int i = 0; f[i] && o < sizeof out - 64; i++) {
        if (f[i] != '%') { out[o++] = f[i]; continue; }
        char spec[16]; int k = 0; spec[k++] = '%'; i++;
        while (f[i] && strchr("-+ #0123456789.lh*", f[i]) && k < 12) { if (f[i] != 'l' && f[i] != 'h') spec[k++] = f[i]; i++; }
        char t = f[i]; if (!t) break;
        spec[k++] = t; spec[k] = 0;
        if (t == '%') { out[o++] = '%'; continue; }
        if (t == 's') { uint32_t p = NEXT32(); o += (size_t)snprintf(out + o, sizeof out - o, spec, p ? (const char *)(g_mem + p) : "(null)"); }
        else if (t == 'f' || t == 'g' || t == 'e') {
            if (ai < 4 && (ai & 1)) ai++;                      /* doubles go in an even register pair */
            uint32_t lo, hi;
            if (ai < 4) { lo = c->r[ai]; hi = c->r[ai + 1]; ai += 2; }
            else { uint32_t so = 4u * (uint32_t)(ai - 4); so = (so + 7) & ~7u; lo = rd32(sp + so); hi = rd32(sp + so + 4); ai = 4 + (int)(so / 4) + 2; }
            uint64_t b = (uint64_t)hi << 32 | lo; double d; memcpy(&d, &b, 8);
            o += (size_t)snprintf(out + o, sizeof out - o, spec, d);
        } else { uint32_t v = NEXT32(); o += (size_t)snprintf(out + o, sizeof out - o, spec, v); }
    }
    #undef NEXT32
    out[o] = 0;
    while (o && (out[o - 1] == '\n' || out[o - 1] == '\r')) out[--o] = 0;
    if (getenv("PWR_LOG_LR")) fprintf(stderr, "game: [lr=%08x] %s\n", c->r[14], out);
    else fprintf(stderr, "game: %s\n", out);
}

/* hooked GX enqueue (gsp.c copies queued command lists) chase camera updates (freelook.c) and settings reads (drawdist.c); test_known links none of them and gets these no-ops */
__attribute__((weak)) void gsp_gx_enqueue_hook(CPU *c) { (void)c; }
__attribute__((weak)) void freelook_pre(CPU *c) { (void)c; }
__attribute__((weak)) void freelook_post(CPU *c) { (void)c; }
__attribute__((weak)) void setting_pre(CPU *c) { (void)c; }
__attribute__((weak)) void setting_post(CPU *c) { (void)c; }

/* debug: functions listed in PWR_TRACE_FUNCS when the C was generated (arm2c.py) call this on entry and return */
void rt_trace_fn(CPU *c, uint32_t fn, int ret) {
    extern int k_current_index(void) __attribute__((weak));   /* absent in test_known (no kernel) */
    int ti = k_current_index ? k_current_index() : -1;
    if (ret) fprintf(stderr, "trace: [t%d] %08x -> r0=%08x r1=%08x\n", ti, fn, c->r[0], c->r[1]);
    else fprintf(stderr, "trace: [t%d] %08x(r0=%08x r1=%08x r2=%08x r3=%08x) lr=%08x\n", ti, fn, c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]);
    static int nw = -1; if (nw < 0) { const char *e = getenv("PWR_TRACE_MEM"); nw = e ? atoi(e) : 0; }   /* words at r0-r2 on entry */
    for (int r = 0; !ret && r < 3; r++) {
        if (!nw || c->r[r] < 0x00100000u || c->r[r] >= 0x20000000u) continue;
        fprintf(stderr, "trace:   [r%d]", r);
        for (int i = 0; i < nw; i++) fprintf(stderr, "%s%08x", i % 8 ? " " : (i ? "\n trace:       " : " "), rd32(c->r[r] + 4u * (uint32_t)i));
        fputc('\n', stderr);
    }
}

#ifdef PWR_WATCH
#include <execinfo.h>
/* first stores into the PWR_WATCH=<va>,<len> range per writing site; the host return address is inside the recompiled
   function (resolve with: addr2line -f -e pwr <host - base>, base printed on the first hit) */
uint32_t g_watch_lo, g_watch_len;
__attribute__((constructor)) static void watch_init(void) {
    const char *e = getenv("PWR_WATCH");
    if (e) { g_watch_lo = (uint32_t)strtoul(e, NULL, 16); const char *c = strchr(e, ','); g_watch_len = c ? (uint32_t)strtoul(c + 1, NULL, 0) : 4; }
}
__attribute__((noinline)) void rt_watch_hit(uint32_t a, uint32_t v, int size) {
    static void *seen[256][3]; static int n, total;
    void *ra = __builtin_return_address(0);
    total++;
    void *bt[5]; int nb = backtrace(bt, 5);   /* an out-of-line wr32 hides the guest function: name its callers too */
    void *k1 = nb > 2 ? bt[2] : NULL, *k2 = nb > 3 ? bt[3] : NULL;   /* one report per (site, caller, caller's caller) */
    for (int i = 0; i < n; i++) if (seen[i][0] == ra && seen[i][1] == k1 && seen[i][2] == k2) return;
    if (n == 256) return;
    seen[n][0] = ra; seen[n][1] = k1; seen[n][2] = k2; n++;
    extern char __executable_start;
    fprintf(stderr, "watch: store%d %08x = %08x from host %p (offset 0x%lx) after %d stores; callers", size * 8, a, v, ra,
            (unsigned long)((char *)ra - &__executable_start), total);
    for (int i = 2; i < nb; i++) fprintf(stderr, " 0x%lx", (unsigned long)((char *)bt[i] - &__executable_start));
    fputc('\n', stderr);
}
#endif
