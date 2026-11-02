/* HLE kernel. All kernel state is guarded by one lock; blocking waits sleep on one condition variable
 * and re-check their predicate after every broadcast. Guest threads are host pthreads. */
#define _GNU_SOURCE
#include "kernel.h"
#include "runtime.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#define MAX_HANDLES 4096
#define MAX_THREADS 32
#define TICKS_PER_SEC 268111856ull
#define EXIT_MAGIC 0xFFFFFFF0u

static pthread_mutex_t K = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  KC = PTHREAD_COND_INITIALIZER;
static KObject *handles[MAX_HANDLES];
static KThread *threads[MAX_THREADS];
static __thread KThread *cur;
static uint32_t heap_next = VA_HEAP, linear_next = VA_LINEAR;
static struct timespec t0;
int g_trace;

typedef struct { uint32_t addr; int wake; } ArbWaiter;
#define MAX_ARB 64
static ArbWaiter *arb_waiters[MAX_ARB];

void k_trace(const char *fmt, ...) {
    if (!g_trace) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[k%d] ", cur ? cur->index : -1); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

KThread *k_current(void) { return cur; }
int k_current_index(void) { return cur ? cur->index : -1; }

KObject *k_alloc(KType t) { KObject *o = calloc(1, sizeof *o); o->type = t; o->refs = 1; return o; }

uint32_t k_new_handle(KObject *o) {
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i]) { handles[i] = o; return 0x1000u + (uint32_t)i; }
    fprintf(stderr, "kernel: out of handles\n"); abort();
}

KObject *k_obj(uint32_t h) {
    if (h == H_CUR_THREAD) return cur ? cur->obj : NULL;
    if (h < 0x1001u || h >= 0x1000u + MAX_HANDLES) return NULL;
    return handles[h - 0x1000u];
}

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t.tv_nsec - t0.tv_nsec);
}
uint64_t k_ticks(void) { return (uint64_t)((__uint128_t)now_ns() * TICKS_PER_SEC / 1000000000ull); }

/* wait on KC until deadline (ns since t0; UINT64_MAX = forever). returns 0 on timeout */
static void give_token(KThread *self);
static int kwait_until(uint64_t deadline) {
    if (cur) give_token(cur);   /* blocking: let another guest thread run */
    if (deadline == UINT64_MAX) { pthread_cond_wait(&KC, &K); return 1; }
    uint64_t n = now_ns(); if (n >= deadline) return 0;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t add = deadline - n;
    ts.tv_sec += (time_t)(add / 1000000000ull); ts.tv_nsec += (long)(add % 1000000000ull);
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&KC, &K, &ts) != ETIMEDOUT || now_ns() < deadline;
}
static uint64_t deadline_from(int64_t ns) { return ns < 0 ? UINT64_MAX : now_ns() + (uint64_t)ns; }

void k_signal(KObject *o) {
    pthread_mutex_lock(&K); o->signaled = 1; pthread_cond_broadcast(&KC); pthread_mutex_unlock(&K);
}

/* ---- scheduling: one guest thread runs at a time (the 3DS app core), highest priority first ----
 * running = thread allowed to execute guest code. A thread gives up the token when it blocks, sleeps or exits, and
 * at any svc when a strictly higher-priority thread is ready. All under K. */
static KThread *running;
int g_main_prio = 0x30;

static int best_ready(KThread *self) {
    for (int i = 0; i < MAX_THREADS; i++) { KThread *o = threads[i]; if (o && o != self && o->ready && o->prio < self->prio) return 0; }
    return 1;
}
static void give_token(KThread *self) { if (running == self) { running = NULL; pthread_cond_broadcast(&KC); } }
static void take_token(KThread *self) {
    if (running == self) return;
    self->ready = 1;
    while (!(running == NULL && best_ready(self))) pthread_cond_wait(&KC, &K);
    self->ready = 0; running = self;
}
/* svc boundary: re-acquire the token, or hand it to a strictly higher-priority ready thread */
static void sched_point(KThread *self) {
    pthread_mutex_lock(&K);
    if (running == self) {
        for (int i = 0; i < MAX_THREADS; i++) { KThread *o = threads[i];
            if (o && o != self && o->ready && o->prio < self->prio) { give_token(self); break; } }
    }
    take_token(self);
    pthread_mutex_unlock(&K);
}

/* ---- waitable objects ---- */
static int ready(KObject *o, KThread *t) {
    switch (o->type) {
    case KO_EVENT: case KO_TIMER: return o->signaled;
    case KO_SEMAPHORE: return o->count > 0;
    case KO_MUTEX: return o->owner == NULL || o->owner == t;
    case KO_THREAD: return o->thread && o->thread->finished;
    default: return 0;
    }
}
static void acquire(KObject *o, KThread *t) {
    switch (o->type) {
    case KO_EVENT: case KO_TIMER: if (o->reset_type == 0) o->signaled = 0; break;
    case KO_SEMAPHORE: o->count--; break;
    case KO_MUTEX: o->owner = t; o->lock_count++; break;
    default: break;
    }
}

static uint32_t wait_n(KThread *t, uint32_t *hs, int n, int all, int64_t ns, int32_t *out_idx) {
    KObject *objs[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) { objs[i] = k_obj(hs[i]); if (!objs[i]) return RES_BAD_HANDLE; }
    uint64_t dl = deadline_from(ns);
    for (;;) {
        if (all) {
            int ok = 1; for (int i = 0; i < n; i++) ok &= ready(objs[i], t);
            if (ok) { for (int i = 0; i < n; i++) acquire(objs[i], t); *out_idx = 0; return RES_OK; }
        } else {
            for (int i = 0; i < n; i++) if (ready(objs[i], t)) { acquire(objs[i], t); *out_idx = i; return RES_OK; }
        }
        if (ns == 0 || !kwait_until(dl)) { *out_idx = -1; return RES_TIMEOUT; }
    }
}

/* ---- threads ---- */
typedef struct { KThread *t; GuestFn fn; } ThreadStart;

static KThread *new_thread(void) {
    for (int i = 0; i < MAX_THREADS; i++) if (!threads[i]) {
        KThread *t = calloc(1, sizeof *t); t->index = i; threads[i] = t;
        t->cpu.tls = VA_TLS + 0x200u * (uint32_t)i;
        t->obj = k_alloc(KO_THREAD); t->obj->thread = t; t->handle = k_new_handle(t->obj);
        return t;
    }
    fprintf(stderr, "kernel: too many threads\n"); abort();
}

static void thread_finish(KThread *t) {
    pthread_mutex_lock(&K); t->finished = 1; give_token(t); pthread_cond_broadcast(&KC); pthread_mutex_unlock(&K);
}

static void *thread_main(void *p) {
    ThreadStart s = *(ThreadStart *)p; free(p);
    cur = s.t;
    pthread_mutex_lock(&K); take_token(s.t); pthread_mutex_unlock(&K);
    k_trace("thread start entry=%08x arg=%08x sp=%08x", s.t->cpu.r[15], s.t->cpu.r[0], s.t->cpu.r[13]);
    s.fn(&s.t->cpu);
    k_trace("thread returned");
    thread_finish(s.t);
    return NULL;
}

/* ---- svc ---- */
static uint32_t get_tick_lo, get_tick_hi;
static void svc_body(CPU *c, uint32_t num, uint32_t pc);

void rt_svc(CPU *c, uint32_t num, uint32_t pc) {
    KThread *t = cur;
    if (t) { t->svc_num = num; t->svc_lr = c->r[14]; memcpy(t->svc_r, c->r, sizeof t->svc_r);
             t->svc_cmd = num == 0x32 ? rd32(c->tls + TLS_CMDBUF) : 0; t->in_svc = 1; }
    svc_body(c, num, pc);
    if (t) t->in_svc = 0;
    sched_point(cur);
}

/* printed by main.c when the game stops presenting frames: each guest thread, the svc it is blocked in and the state
   of the objects it waits on (racy reads, diagnostics only) */
void k_hang_report(void) {
    static const char *tn[] = { "none", "event", "semaphore", "mutex", "arbiter", "thread", "session", "shmem", "process", "reslimit", "timer" };
    fprintf(stderr, "hang: guest threads\n");
    for (int i = 0; i < MAX_THREADS; i++) {
        KThread *t = threads[i];
        if (!t) continue;
        fprintf(stderr, "hang:  k%d prio %d%s%s", t->index, t->prio, t->ready ? " ready" : "", t->finished ? " finished" : "");
        if (!t->in_svc) { fprintf(stderr, " running guest code (or between svcs) pc %08x lr %08x\n", t->cpu.r[15], t->cpu.r[14]); continue; }
        fprintf(stderr, " in svc %02x lr %08x r0-r5 %08x %08x %08x %08x %08x %08x", t->svc_num, t->svc_lr,
                t->svc_r[0], t->svc_r[1], t->svc_r[2], t->svc_r[3], t->svc_r[4], t->svc_r[5]);
        uint32_t hs[8]; int n = 0;
        if (t->svc_num == 0x24) hs[n++] = t->svc_r[0];
        if (t->svc_num == 0x25) for (int k = 0; k < (int)t->svc_r[2] && k < 8; k++) hs[n++] = rd32(t->svc_r[1] + 4u * (uint32_t)k);
        if (t->svc_num == 0x32) { KObject *o = k_obj(t->svc_r[0]); fprintf(stderr, " ipc %s cmd %08x", o && o->svc ? o->svc->name : "?", t->svc_cmd); }
        if (t->svc_num == 0x22) fprintf(stderr, " arbiter *addr=%d", (int32_t)rd32(t->svc_r[1]));
        for (int k = 0; k < n; k++) { KObject *o = k_obj(hs[k]);
            if (!o) { fprintf(stderr, " [%x bad]", hs[k]); continue; }
            fprintf(stderr, " [%x %s sig %d count %d%s]", hs[k], o->type < 11 ? tn[o->type] : "?", o->signaled, o->count,
                    o->type == KO_MUTEX && o->owner ? (o->owner->index == 0 ? " owner k0" : " owned") : ""); }
        fputc('\n', stderr);
    }
}

static void svc_body(CPU *c, uint32_t num, uint32_t pc) {
    KThread *t = cur;
    switch (num) {
    case 0x01: { /* ControlMemory: r0 op, r1 addr0, r2 addr1, r3 size, r4 perm -> r1 addr */
        uint32_t op = c->r[0], a0 = c->r[1], size = c->r[3];
        uint32_t base = op & 0xff, out = a0;
        pthread_mutex_lock(&K);
        if (base == 3) {               /* ALLOC */
            if (op & 0x10000) { out = linear_next; linear_next += (size + 0xfff) & ~0xfffu; }
            else if (!a0) { out = heap_next; heap_next += (size + 0xfff) & ~0xfffu; }
            else { if (a0 + size > heap_next) heap_next = a0 + size; }
        } else if (base == 1) {        /* FREE: memory stays mapped; zero it so reuse looks fresh */
            if (a0 && size) memset(g_mem + a0, 0, size);
        }
        pthread_mutex_unlock(&K);
        k_trace("ControlMemory op=%x addr0=%08x size=%x -> %08x", op, a0, size, out);
        c->r[0] = RES_OK; c->r[1] = out; return;
    }
    case 0x03: k_trace("ExitProcess"); exit(0);
    case 0x08: { /* CreateThread: r0 prio, r1 entry, r2 arg, r3 stacktop, r4 cpu -> r1 handle */
        GuestFn fn = rt_lookup(c->r[1]);
        if (!fn) { fprintf(stderr, "kernel: CreateThread entry %08x unknown\n", c->r[1]); abort(); }
        pthread_mutex_lock(&K); KThread *nt = new_thread(); pthread_mutex_unlock(&K);
        nt->prio = (int)c->r[0];
        nt->cpu.r[0] = c->r[2]; nt->cpu.r[13] = c->r[3]; nt->cpu.r[14] = EXIT_MAGIC; nt->cpu.r[15] = c->r[1];
        ThreadStart *s = malloc(sizeof *s); s->t = nt; s->fn = fn;
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
        pthread_create(&th, &at, thread_main, s); pthread_detach(th);
        k_trace("CreateThread entry=%08x prio=%d cpu=%d lr=%08x -> %x", c->r[1], (int)c->r[0], (int)c->r[4], c->r[14], nt->handle);
        c->r[0] = RES_OK; c->r[1] = nt->handle; return;
    }
    case 0x09: k_trace("ExitThread"); thread_finish(t); pthread_exit(NULL);
    case 0x0A: { /* SleepThread(r0:r1 ns) */
        int64_t ns = (int64_t)(((uint64_t)c->r[1] << 32) | c->r[0]);
        static int sleep_trace = -1; if (sleep_trace < 0) sleep_trace = getenv("PWR_TRACE_SLEEP") != NULL;
        if (sleep_trace) fprintf(stderr, "sleep: [k%d] t=%.4f ns=%lld lr=%08x\n", t ? t->index : -1, k_ticks() / 268111856.0, (long long)ns, c->r[14]);
        pthread_mutex_lock(&K); give_token(t); pthread_mutex_unlock(&K);   /* sleep(0) = yield; sched_point re-takes it */
        if (ns > 0) { struct timespec ts = { (time_t)(ns / 1000000000), (long)(ns % 1000000000) }; nanosleep(&ts, NULL); }
        else sched_yield();
        return;
    }
    case 0x13: { KObject *o = k_alloc(KO_MUTEX); pthread_mutex_lock(&K); if (c->r[1]) { o->owner = t; o->lock_count = 1; } c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x14: { /* ReleaseMutex */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]);
        if (o && o->type == KO_MUTEX && o->owner == t && --o->lock_count == 0) o->owner = NULL;
        pthread_cond_broadcast(&KC); pthread_mutex_unlock(&K); c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x15: { KObject *o = k_alloc(KO_SEMAPHORE); o->count = (int32_t)c->r[1]; o->max = (int32_t)c->r[2];
        pthread_mutex_lock(&K); c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x16: { /* ReleaseSemaphore: r1 handle, r2 release -> r1 previous count */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[1]); uint32_t prev = 0;
        if (o && o->type == KO_SEMAPHORE) { prev = (uint32_t)o->count; o->count += (int32_t)c->r[2]; if (o->count > o->max) o->count = o->max; }
        pthread_cond_broadcast(&KC); pthread_mutex_unlock(&K); c->r[0] = o ? RES_OK : RES_BAD_HANDLE; c->r[1] = prev; return;
    }
    case 0x17: { KObject *o = k_alloc(KO_EVENT); o->reset_type = (int)c->r[1];
        pthread_mutex_lock(&K); c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x18: case 0x19: { /* SignalEvent / ClearEvent */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]);
        if (o) o->signaled = num == 0x18;
        if (o && num == 0x18) pthread_cond_broadcast(&KC);
        pthread_mutex_unlock(&K); c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x1F: { /* MapMemoryBlock: r0 handle, r1 addr, r2 myperm, r3 otherperm */
        pthread_mutex_lock(&K); KObject *o = k_obj(c->r[0]);
        if (o && o->type == KO_SHMEM) {
            if (c->r[1] && c->r[1] != o->addr) { memcpy(g_mem + c->r[1], g_mem + o->addr, o->size); o->addr = c->r[1]; }
        }
        pthread_mutex_unlock(&K);
        k_trace("MapMemoryBlock h=%x addr=%08x", c->r[0], c->r[1]);
        c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return;
    }
    case 0x20: c->r[0] = RES_OK; return;   /* UnmapMemoryBlock */
    case 0x21: { KObject *o = k_alloc(KO_ARBITER); pthread_mutex_lock(&K); c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x22: { /* ArbitrateAddress: r0 handle, r1 addr, r2 type, r3 value, r4:r5 ns */
        uint32_t addr = c->r[1], type = c->r[2]; int32_t val = (int32_t)c->r[3];
        int64_t ns = (int64_t)(((uint64_t)c->r[5] << 32) | c->r[4]);
        static uint32_t arb_watch = 1; if (arb_watch == 1) { const char *e = getenv("PWR_TRACE_ARB"); arb_watch = e ? (uint32_t)strtoul(e, NULL, 16) : 0; }
        if (arb_watch && addr == arb_watch) fprintf(stderr, "arb: [k%d] type=%u value=%d *addr=%d count/ns=%lld lr=%08x\n", t ? t->index : -1, type, val, (int32_t)rd32(addr), (long long)ns, c->r[14]);
        static int ntrace; if (ntrace < 20) { ntrace++; k_trace("ArbitrateAddress h=%x addr=%08x type=%u value=%d *addr=%d lr=%08x", c->r[0], addr, type, val, (int32_t)rd32(addr), c->r[14]); }
        pthread_mutex_lock(&K);
        uint32_t res = RES_OK;
        if (type == 0) {            /* signal: wake up to val waiters (val < 0: all) */
            int woken = 0;
            for (int i = 0; i < MAX_ARB && (val < 0 || woken < val); i++)
                if (arb_waiters[i] && arb_waiters[i]->addr == addr && !arb_waiters[i]->wake) { arb_waiters[i]->wake = 1; woken++; }
            pthread_cond_broadcast(&KC);
        } else {
            int32_t cur_v = (int32_t)rd32(addr);
            if (cur_v < val) {
                if (type == 2 || type == 4) wr32(addr, (uint32_t)(cur_v - 1));
                ArbWaiter w = { addr, 0 }; int slot = -1;
                for (int i = 0; i < MAX_ARB; i++) if (!arb_waiters[i]) { arb_waiters[i] = &w; slot = i; break; }
                if (slot < 0) { fprintf(stderr, "kernel: arbiter waiter table full\n"); abort(); }
                uint64_t dl = (type >= 3) ? deadline_from(ns) : UINT64_MAX;
                static int arb_trace = -1; if (arb_trace < 0) arb_trace = getenv("PWR_TRACE_SLEEP") != NULL;
                double t0 = arb_trace ? k_ticks() / 268111856.0 : 0;
                while (!w.wake) if (!kwait_until(dl)) { res = RES_TIMEOUT; break; }
                arb_waiters[slot] = NULL;
                if (arb_trace) fprintf(stderr, "arbw: [k%d] t=%.4f waited=%.4f addr=%08x type=%u lr=%08x\n", t ? t->index : -1, t0, k_ticks() / 268111856.0 - t0, addr, type, c->r[14]);
            }
        }
        pthread_mutex_unlock(&K);
        c->r[0] = res; return;
    }
    case 0x23: { pthread_mutex_lock(&K); uint32_t h = c->r[0];
        if (h > 0x1000u && h < 0x1000u + MAX_HANDLES) handles[h - 0x1000u] = NULL;
        pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x24: { /* WaitSynchronization1: r0 handle, r2:r3 ns */
        int32_t idx; uint32_t h = c->r[0]; int64_t ns = (int64_t)(((uint64_t)c->r[3] << 32) | c->r[2]);
        static int wait_trace = -1; if (wait_trace < 0) wait_trace = getenv("PWR_TRACE_SLEEP") != NULL;
        double t0 = wait_trace ? k_ticks() / 268111856.0 : 0;
        pthread_mutex_lock(&K); c->r[0] = wait_n(t, &h, 1, 0, ns, &idx); pthread_mutex_unlock(&K);
        if (wait_trace) fprintf(stderr, "wait1: [k%d] t=%.4f waited=%.4f h=%x ns=%lld lr=%08x\n", t ? t->index : -1, t0, k_ticks() / 268111856.0 - t0, h, (long long)ns, c->r[14]);
        return;
    }
    case 0x25: { /* WaitSynchronizationN: r1 handles, r2 count, r3 waitAll, r0:r4 ns -> r1 index */
        int32_t idx; uint32_t hs[64]; int n = (int)c->r[2]; if (n > 64) n = 64;
        for (int i = 0; i < n; i++) hs[i] = rd32(c->r[1] + 4u * (uint32_t)i);
        int64_t ns = (int64_t)(((uint64_t)c->r[4] << 32) | c->r[0]);
        k_trace("WaitSynchronizationN n=%d all=%u h0=%x h1=%x ns=%lld lr=%08x", n, c->r[3], n > 0 ? hs[0] : 0, n > 1 ? hs[1] : 0, (long long)ns, c->r[14]);
        pthread_mutex_lock(&K); uint32_t r = wait_n(t, hs, n, (int)c->r[3], ns, &idx); pthread_mutex_unlock(&K);
        c->r[0] = r; c->r[1] = (uint32_t)idx; return;
    }
    case 0x27: { pthread_mutex_lock(&K); KObject *o = k_obj(c->r[1]); if (o) o->refs++; c->r[1] = o ? k_new_handle(o) : 0; pthread_mutex_unlock(&K); c->r[0] = o ? RES_OK : RES_BAD_HANDLE; return; }
    case 0x28: { uint64_t ticks = k_ticks();
        c->r[0] = (uint32_t)ticks; c->r[1] = (uint32_t)(ticks >> 32); (void)get_tick_lo; (void)get_tick_hi; return; }
    case 0x2D: { /* ConnectToPort: r1 name -> r1 session */
        const char *name = (const char *)(g_mem + c->r[1]);
        pthread_mutex_lock(&K); uint32_t h = k_session_for(name); pthread_mutex_unlock(&K);
        k_trace("ConnectToPort(\"%s\") -> %x", name, h);
        c->r[0] = h ? RES_OK : 0xD88007FAu; c->r[1] = h; return;
    }
    case 0x32: { /* SendSyncRequest(r0 session) */
        KObject *o = k_obj(c->r[0]);
        if (!o || o->type != KO_SESSION) { fprintf(stderr, "kernel: SendSyncRequest on bad handle %x at %08x\n", c->r[0], pc); c->r[0] = RES_BAD_HANDLE; return; }
        uint32_t *cmd = (uint32_t *)(g_mem + c->tls + TLS_CMDBUF);
        k_trace("ipc %s cmd=%08x", o->svc->name, cmd[0]);
        o->svc->handle(t, cmd, o);
        c->r[0] = RES_OK; return;
    }
    case 0x35: c->r[0] = RES_OK; c->r[1] = 0x28; return;            /* GetProcessId */
    case 0x37: { KObject *o = k_obj(c->r[1]); c->r[0] = RES_OK; c->r[1] = o && o->thread ? 0x100u + (uint32_t)o->thread->index : 0; return; }
    case 0x38: { KObject *o = k_alloc(KO_RESLIMIT); pthread_mutex_lock(&K); c->r[1] = k_new_handle(o); pthread_mutex_unlock(&K); c->r[0] = RES_OK; return; }
    case 0x3A: { /* GetResourceLimitCurrentValues: r0 values(s64*), r1 handle, r2 names, r3 count */
        for (uint32_t i = 0; i < c->r[3]; i++) {
            uint32_t name = rd32(c->r[2] + 4 * i); uint64_t v = 0;
            if (name == 1) v = heap_next - VA_HEAP + (linear_next - VA_LINEAR);   /* commit */
            wr32(c->r[0] + 8 * i, (uint32_t)v); wr32(c->r[0] + 8 * i + 4, (uint32_t)(v >> 32));
        }
        c->r[0] = RES_OK; return;
    }
    case 0x3C: fprintf(stderr, "kernel: svcBreak(%u) at %08x\n", c->r[0], pc); abort();
    default:
        fprintf(stderr, "kernel: unimplemented svc 0x%02x at %08x\n", num, pc); abort();
    }
}

uint32_t k_session_for(const char *name) {
    const Service *s = svc_find(name);
    if (!s) return 0;
    KObject *o = k_alloc(KO_SESSION); o->svc = s;
    return k_new_handle(o);
}

static void init_config_pages(void) {
    /* kernel config page: values for an old-3DS retail unit, application memory type 0 (64 MiB) */
    wr8(VA_CONFIG + 0x02, 0x22);              /* KERNEL_VERSIONREVISION/minor (kernel 2.34) */
    wr8(VA_CONFIG + 0x03, 0x02);              /* KERNEL_VERSIONMAJOR */
    wr32(VA_CONFIG + 0x04, 2);                /* UPDATEFLAG */
    wr32(VA_CONFIG + 0x08, 0x00040130);       /* NS_TID low */
    wr32(VA_CONFIG + 0x10, 0x0000002B);       /* SYSCOREVER */
    wr8(VA_CONFIG + 0x14, 1);                 /* ENVINFO: retail */
    wr8(VA_CONFIG + 0x16, 1);                 /* UNITINFO */
    wr32(VA_CONFIG + 0x30, 0);                /* APPMEMTYPE */
    wr32(VA_CONFIG + 0x40, 0x04000000);       /* APPMEMALLOC */
    wr32(VA_CONFIG + 0x44, 0x02C00000);       /* SYSMEMALLOC */
    wr32(VA_CONFIG + 0x48, 0x01400000);       /* BASEMEMALLOC */
    wr32(VA_SHARED + 0x04, 1);                /* RUNNING_HW: product */
}

void k_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_trace = getenv("PWR_TRACE") != NULL;
    init_config_pages();
}

typedef struct { GuestFn entry; volatile int done; int ret; } MainStart;
static void *main_thread(void *p) { MainStart *m = p; m->ret = k_run_main(m->entry); m->done = 1; return NULL; }

/* run the guest main thread on its own host thread (recompiled code nests deeply on the host stack) */
volatile int *k_start_main(GuestFn entry) {
    static MainStart m; m.entry = entry;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_create(&th, &at, main_thread, &m); pthread_detach(th);
    return &m.done;
}

int k_run_main(GuestFn entry) {
    pthread_mutex_lock(&K); KThread *t = new_thread(); pthread_mutex_unlock(&K);
    cur = t;
    t->prio = g_main_prio;
    pthread_mutex_lock(&K); take_token(t); pthread_mutex_unlock(&K);
    t->cpu.r[13] = VA_STACK_TOP; t->cpu.r[14] = EXIT_MAGIC;
    k_trace("main thread start, tls=%08x", t->cpu.tls);
    entry(&t->cpu);
    k_trace("main thread returned r0=%08x", t->cpu.r[0]);
    return (int)t->cpu.r[0];
}
