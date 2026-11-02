/* PICA200 vertex shader -> GLSL -> SPIR-V, one specialized Vulkan shader module per guest program configuration.
 * Registers become local variables, swizzles and operand indices are fixed, the attribute permutation and the output
 * map are baked in. Flow control: the reachable code is split into basic blocks run by a switch(pc) dispatcher, with
 * an explicit call/if/loop stack (only emitted when the program uses one). Semantics mirror vs_run() in pica_sw.c
 * and shaders/pica_vs.vert (the interpreter used when no compiler is available).
 * GLSL is compiled with libshaderc, loaded at run time (no build dependency); the module cache is keyed by a hash of
 * the program, swizzles, entry point, permutation and output map. */
#include "pica_int.h"
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#define dlopen(name, flags) ((void *)LoadLibraryA(name))
#define dlsym(h, name) ((void *)GetProcAddress((HMODULE)(h), name))
#else
#include <dlfcn.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R(i) pica_regs[i]

/* ---- libshaderc C API (subset), resolved with dlsym ---- */
typedef void *sc_compiler, *sc_options, *sc_result;
static sc_compiler (*sc_compiler_initialize)(void);
static sc_options (*sc_compile_options_initialize)(void);
static void (*sc_set_optimization_level)(sc_options, int);
static void (*sc_set_target_env)(sc_options, int, uint32_t);
static sc_result (*sc_compile_into_spv)(sc_compiler, const char *, size_t, int, const char *, const char *, sc_options);
static size_t (*sc_result_get_length)(sc_result);
static const char *(*sc_result_get_bytes)(sc_result);
static int (*sc_result_get_compilation_status)(sc_result);
static const char *(*sc_result_get_error_message)(sc_result);
static void (*sc_result_release)(sc_result);
static sc_compiler compiler; static sc_options options;
static int sc_state = -1;

static int shaderc_load(void) {
    if (sc_state >= 0) return sc_state;
    sc_state = 0;
    if (getenv("PWR_VK_NO_VSGEN")) return 0;
    const char *names[] = { "libshaderc.so.1", "libshaderc_shared.so.1", "libshaderc_shared.so", "libshaderc.so", "shaderc_shared.dll", "libshaderc_shared.dll" };
    void *h = NULL;
    for (size_t i = 0; i < sizeof names / sizeof *names && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "vsgen: libshaderc not found; vertex shaders use the GPU interpreter\n"); return 0; }
    #define SYM(v, n) if (!(*(void **)&v = dlsym(h, n))) { fprintf(stderr, "vsgen: libshaderc lacks %s\n", n); return 0; }
    SYM(sc_compiler_initialize, "shaderc_compiler_initialize");
    SYM(sc_compile_options_initialize, "shaderc_compile_options_initialize");
    SYM(sc_set_optimization_level, "shaderc_compile_options_set_optimization_level");
    SYM(sc_set_target_env, "shaderc_compile_options_set_target_env");
    SYM(sc_compile_into_spv, "shaderc_compile_into_spv");
    SYM(sc_result_get_length, "shaderc_result_get_length");
    SYM(sc_result_get_bytes, "shaderc_result_get_bytes");
    SYM(sc_result_get_compilation_status, "shaderc_result_get_compilation_status");
    SYM(sc_result_get_error_message, "shaderc_result_get_error_message");
    SYM(sc_result_release, "shaderc_result_release");
    #undef SYM
    compiler = sc_compiler_initialize(); options = sc_compile_options_initialize();
    if (!compiler || !options) return 0;
    sc_set_target_env(options, 0 /* vulkan */, (1u << 22) | (3u << 12) /* 1.3 */);
    sc_set_optimization_level(options, 2 /* performance */);
    sc_state = 1;
    return 1;
}

/* ---- text buffer ---- */
static char *out; static size_t olen, ocap;
static void emit(const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt); int n = vsnprintf(out + olen, ocap - olen, fmt, ap); va_end(ap);
        if (n >= 0 && olen + (size_t)n < ocap) { olen += (size_t)n; return; }
        ocap = ocap ? ocap * 2 : 1 << 16; out = realloc(out, ocap);
    }
}

#define prog pica_vs_prog
#define swz (pica_vs_prog + 4096)

static const char *comp = "xyzw";
/* source operand expression */
static void src_expr(char *buf, size_t n, uint32_t idx, uint32_t sel, int neg, uint32_t rel) {
    char base[48];
    if (idx < 0x10) snprintf(base, sizeof base, "v%u", idx);
    else if (idx < 0x20) snprintf(base, sizeof base, "r%u", idx - 0x10);
    else if (rel) snprintf(base, sizeof base, "cr(%u + %s)", idx - 0x20, rel == 1 ? "a0x" : rel == 2 ? "a0y" : "aL");
    else snprintf(base, sizeof base, "d.c[%u]", idx - 0x20);
    snprintf(buf, n, "%s(%s.%c%c%c%c)", neg ? "-" : "", base, comp[(sel >> 6) & 3], comp[(sel >> 4) & 3], comp[(sel >> 2) & 3], comp[sel & 3]);
}
static void dst_assign(uint32_t dst, uint32_t mask, const char *val) {
    if (!(mask & 15)) return;
    char name[8]; if (dst < 0x10) snprintf(name, sizeof name, "o%u", dst); else snprintf(name, sizeof name, "r%u", (dst - 0x10) & 15);
    char m[5]; int k = 0;
    if (mask & 8) m[k++] = 'x'; if (mask & 4) m[k++] = 'y'; if (mask & 2) m[k++] = 'z'; if (mask & 1) m[k++] = 'w'; m[k] = 0;
    emit("    { vec4 t_ = %s; %s.%s = t_.%s; }\n", val, name, m, m);
}
static void cond_expr(char *buf, size_t n, uint32_t ins) {
    int refx = (ins >> 25) & 1, refy = (ins >> 24) & 1, op = (ins >> 22) & 3;
    char x[24], y[24]; snprintf(x, sizeof x, "(cx == %s)", refx ? "true" : "false"); snprintf(y, sizeof y, "(cy == %s)", refy ? "true" : "false");
    if (op == 0) snprintf(buf, n, "(%s || %s)", x, y);
    else if (op == 1) snprintf(buf, n, "(%s && %s)", x, y);
    else snprintf(buf, n, "%s", op == 2 ? x : y);
}
static const char *cmp_glsl[8] = { "==", "!=", "<", "<=", ">", ">=", "!=", "!=" };

/* one arithmetic instruction */
static void emit_arith(uint32_t ins) {
    uint32_t op = ins >> 26; char a[96], b[96], c[96], v[320];
    if (op >= 0x30) {
        uint32_t ds = swz[ins & 0x1f], dst = (ins >> 24) & 0x1f, rel = (ins >> 22) & 3; int madi = op < 0x38;
        uint32_t s1 = (ins >> 17) & 0x1f, s2, s3;
        if (madi) { s2 = (ins >> 12) & 0x1f; s3 = (ins >> 5) & 0x7f; } else { s2 = (ins >> 10) & 0x7f; s3 = (ins >> 5) & 0x1f; }
        src_expr(a, sizeof a, s1, (ds >> 5) & 0xff, (ds >> 4) & 1, 0);
        src_expr(b, sizeof b, s2, (ds >> 14) & 0xff, (ds >> 13) & 1, madi ? 0 : rel);
        src_expr(c, sizeof c, s3, (ds >> 23) & 0xff, (ds >> 22) & 1, madi ? rel : 0);
        snprintf(v, sizeof v, "pmul4(%s, %s) + %s", a, b, c);
        dst_assign(dst, ds & 0xf, v); return;
    }
    uint32_t ds = swz[ins & 0x7f], dst = (ins >> 21) & 0x1f, rel = (ins >> 19) & 3;
    int inv = op >= 0x18 && op <= 0x1b;
    uint32_t s1 = inv ? (ins >> 14) & 0x1f : (ins >> 12) & 0x7f, s2 = inv ? (ins >> 7) & 0x7f : (ins >> 7) & 0x1f;
    src_expr(a, sizeof a, s1, (ds >> 5) & 0xff, (ds >> 4) & 1, inv ? 0 : rel);
    src_expr(b, sizeof b, s2, (ds >> 14) & 0xff, (ds >> 13) & 1, inv ? rel : 0);
    switch (op) {
    case 0x00: snprintf(v, sizeof v, "(%s + %s)", a, b); break;
    case 0x01: snprintf(v, sizeof v, "vec4(dp3(%s, %s))", a, b); break;
    case 0x02: snprintf(v, sizeof v, "vec4(dp4(%s, %s))", a, b); break;
    case 0x03: case 0x18: snprintf(v, sizeof v, "vec4(dph(%s, %s))", a, b); break;
    case 0x04: case 0x19: snprintf(v, sizeof v, "dst_(%s, %s)", a, b); break;
    case 0x05: snprintf(v, sizeof v, "vec4(exp2((%s).x))", a); break;
    case 0x06: snprintf(v, sizeof v, "vec4(log2((%s).x))", a); break;
    case 0x07: emit("    { vec4 a_ = %s; cx = a_.x >= 0.0; cy = a_.y >= 0.0;\n", a); dst_assign(dst, ds & 0xf, "vec4(max(a_.x, 0.0), max(a_.y, 0.0), a_.z, clamp(a_.w, -127.9961, 127.9961))"); emit("    }\n"); return;
    case 0x08: snprintf(v, sizeof v, "pmul4(%s, %s)", a, b); break;
    case 0x09: case 0x1a: snprintf(v, sizeof v, "vec4(greaterThanEqual(%s, %s))", a, b); break;
    case 0x0a: case 0x1b: snprintf(v, sizeof v, "vec4(lessThan(%s, %s))", a, b); break;
    case 0x0b: snprintf(v, sizeof v, "floor(%s)", a); break;
    case 0x0c: snprintf(v, sizeof v, "max_(%s, %s)", a, b); break;
    case 0x0d: snprintf(v, sizeof v, "min_(%s, %s)", a, b); break;
    case 0x0e: snprintf(v, sizeof v, "vec4(1.0 / (%s).x)", a); break;
    case 0x0f: snprintf(v, sizeof v, "vec4(inversesqrt((%s).x))", a); break;
    case 0x12: emit("    { vec4 a_ = %s;%s%s }\n", a, (ds & 8) ? " a0x = int(a_.x);" : "", (ds & 4) ? " a0y = int(a_.y);" : ""); return;
    case 0x13: snprintf(v, sizeof v, "%s", a); break;
    case 0x2e: case 0x2f:
        emit("    { vec4 a_ = %s, b_ = %s; cx = a_.x %s b_.x; cy = a_.y %s b_.y; }\n", a, b, cmp_glsl[(ins >> 24) & 7], cmp_glsl[(ins >> 21) & 7]);
        if (((ins >> 24) & 7) >= 6) emit("    cx = false;\n");
        if (((ins >> 21) & 7) >= 6) emit("    cy = false;\n");
        return;
    default: return;
    }
    dst_assign(dst, ds & 0xf, v);
}

static int is_flow(uint32_t op) { return op >= 0x20 && op < 0x2e; }

/* builds GLSL for the current registers; returns 0 on success */
static int generate(void) {
    static uint8_t reach[4096], leader[4096];
    memset(reach, 0, sizeof reach); memset(leader, 0, sizeof leader);
    uint32_t entry = R(0x2BA) & 0xffff, work[8192]; int nw = 0, uses_stack = 0;
    if (entry >= 4096) return -1;
    leader[entry] = 1; work[nw++] = entry;
    #define ADD(x) do { uint32_t x_ = (x); if (x_ < 4096) { leader[x_] = 1; if (nw < 8192) work[nw++] = x_; } } while (0)
    #define LEAD(x) do { uint32_t x_ = (x); if (x_ < 4096) leader[x_] = 1; } while (0)
    while (nw) {
        uint32_t pc = work[--nw];
        while (pc < 4096 && !reach[pc]) {
            reach[pc] = 1;
            uint32_t ins = prog[pc], op = ins >> 26, dst = (ins >> 10) & 0xfff, num = ins & 0xff;
            if (!is_flow(op)) { pc++; continue; }
            if (op == 0x22) break;                                                   /* END */
            if (op == 0x21) { pc++; continue; }                                      /* NOP */
            uses_stack |= op == 0x24 || op == 0x25 || op == 0x26 || op == 0x27 || op == 0x28 || op == 0x29 || op == 0x20 || op == 0x23;
            if (op == 0x20) break;                                                   /* BREAK */
            if (op == 0x23) { ADD(pc + 1); break; }                                  /* BREAKC */
            if (op >= 0x24 && op <= 0x26) { ADD(dst); LEAD(dst + num); ADD(pc + 1); break; }
            if (op == 0x27 || op == 0x28) { ADD(pc + 1); LEAD(dst); ADD(dst); ADD(dst + num); break; }
            if (op == 0x29) { ADD(pc + 1); ADD(dst + 1); break; }
            if (op == 0x2c || op == 0x2d) { ADD(dst); ADD(pc + 1); break; }
            pc++;                                                                    /* EMIT/SETEMIT */
        }
    }
    #undef ADD
    #undef LEAD

    olen = 0;
    emit("#version 450\n"
         "layout(set = 0, binding = 5, std140) uniform VsDraw { vec4 c[96]; uvec4 ib; uvec4 iu; uvec4 perm; uvec4 omap[2]; uvec4 fa[12]; vec4 fdef[12]; } d;\n"
         "layout(set = 0, binding = 7, std430) readonly buffer Raw { uint raw[]; };\n"
         /* attribute a: d.fa[a] = (byte offset of vertex 0, stride, type | count << 4 | 0x100 when read from raw); else
            the default d.fdef[a]. Types as the PICA loaders: 0 s8, 1 u8, 2 s16, 3 f32 (pica_vk.c fills these) */
         "uint rb(uint a) { return (raw[a >> 2] >> ((a & 3u) * 8u)) & 0xffu; }\n"
         "vec4 fetch(uint a) {\n"
         "  uvec4 f = d.fa[a]; if ((f.z & 0x100u) == 0u) return d.fdef[a];\n"
         "  uint p = f.x + uint(gl_VertexIndex) * f.y, t = f.z & 3u, n = (f.z >> 4) & 7u; vec4 v = vec4(0, 0, 0, 1);\n"
         "  for (uint k = 0u; k < n; k++) {\n"
         "    if (t == 3u) v[k] = uintBitsToFloat(raw[(p >> 2) + k]);\n"
         "    else if (t == 2u) { uint q = p + 2u * k; v[k] = float(int(((raw[q >> 2] >> ((q & 2u) * 8u)) & 0xffffu) << 16) >> 16); }\n"
         "    else { uint b = rb(p + k); v[k] = t == 0u ? float(int(b << 24) >> 24) : float(b); }\n"
         "  }\n"
         "  return v;\n"
         "}\n"
         "layout(location = 0) out vec4 v_col; layout(location = 1) out vec4 v_uv01; layout(location = 2) out vec3 v_uv2tw;\n"
         "layout(location = 3) out vec4 v_quat; layout(location = 4) out vec3 v_view;\n"
         "float pmul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
         "vec4 pmul4(vec4 a, vec4 b) { return vec4(pmul(a.x, b.x), pmul(a.y, b.y), pmul(a.z, b.z), pmul(a.w, b.w)); }\n"
         "float dp3(vec4 a, vec4 b) { return pmul(a.x, b.x) + pmul(a.y, b.y) + pmul(a.z, b.z); }\n"
         "float dp4(vec4 a, vec4 b) { return dp3(a, b) + pmul(a.w, b.w); }\n"
         "float dph(vec4 a, vec4 b) { return dp3(a, b) + b.w; }\n"
         "vec4 dst_(vec4 a, vec4 b) { return vec4(1.0, pmul(a.y, b.y), a.z, b.w); }\n"
         "vec4 max_(vec4 a, vec4 b) { return vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w); }\n"
         "vec4 min_(vec4 a, vec4 b) { return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w); }\n"
         "vec4 cr(int i) { return (i >= 0 && i < 96) ? d.c[i] : vec4(1.0); }\n"
         "bool bu(uint i) { return ((d.ib.x >> i) & 1u) != 0u; }\n"
         "void main() {\n");
    uint32_t nattr = (uint32_t)pica_num_attrs();
    uint64_t perm = (uint64_t)R(0x2BB) | (uint64_t)R(0x2BC) << 32;
    for (int i = 0; i < 16; i++) emit("  vec4 v%d = vec4(0, 0, 0, 1), r%d = vec4(0), o%d = vec4(0);\n", i, i, i);
    for (uint32_t a = 0; a < nattr && a < 12; a++) emit("  v%u = fetch(%uu);\n", (uint32_t)(perm >> (4 * a)) & 15, a);
    emit("  int a0x = 0, a0y = 0, aL = 0; bool cx = false, cy = false;\n");
    if (uses_stack) emit("  uint st_final[16], st_ret[16], st_start[16]; int st_rep[16], st_inc[16]; bool st_loop[16]; int sp = 0;\n");
    emit("  uint pc = %uu;\n  for (int steps = 0; steps < 65536; steps++) {\n", entry);
    if (uses_stack)
        emit("    bool popped = true;\n"
             "    while (popped && sp > 0) { popped = false; if (pc == st_final[sp - 1]) { int t = sp - 1;\n"
             "      if (st_loop[t]) { aL += st_inc[t]; if (st_rep[t] > 0) { st_rep[t]--; pc = st_start[t]; } else { pc = st_ret[t]; sp--; popped = true; } }\n"
             "      else { pc = st_ret[t]; sp--; popped = true; } } }\n");
    emit("    if (pc >= 4096u) break;\n    switch (pc) {\n");
    #define PUSH(start, fin, ret, rep, inc, loop) emit("      if (sp < 16) { st_start[sp] = %uu; st_final[sp] = %uu; st_ret[sp] = %uu; st_rep[sp] = %s; st_inc[sp] = %s; st_loop[sp] = %s; sp++; }\n", \
                                                       (uint32_t)(start), (uint32_t)(fin), (uint32_t)(ret), rep, inc, loop)
    for (uint32_t pc = 0; pc < 4096; pc++) {
        if (!reach[pc] || !leader[pc]) continue;
        emit("    case %uu: {\n", pc);
        uint32_t p = pc;
        for (;;) {
            uint32_t ins = prog[p], op = ins >> 26, dst = (ins >> 10) & 0xfff, num = ins & 0xff;
            if (!is_flow(op) || op == 0x21 || op >= 0x2a && op <= 0x2b) {   /* arithmetic, NOP, EMIT/SETEMIT */
                if (!is_flow(op)) emit_arith(ins);
                p++;
                if (p >= 4096) { emit("      pc = 4096u; break;\n"); break; }
                if (leader[p]) { emit("      pc = %uu; break;\n", p); break; }
                continue;
            }
            char cnd[96];
            switch (op) {
            case 0x22: emit("      pc = 4096u; break;\n"); break;                                         /* END */
            case 0x20: case 0x23:                                                                           /* BREAK / BREAKC */
                if (op == 0x23) { cond_expr(cnd, sizeof cnd, ins); emit("      if (!%s) { pc = %uu; break; }\n", cnd, p + 1); }
                emit("      while (sp > 0 && !st_loop[sp - 1]) sp--;\n      if (sp == 0) { pc = 4096u; break; }\n      pc = st_ret[sp - 1]; sp--; break;\n");
                break;
            case 0x24: case 0x25: case 0x26:                                                                /* CALL / CALLC / CALLU */
                if (op == 0x24) snprintf(cnd, sizeof cnd, "true");
                else if (op == 0x25) cond_expr(cnd, sizeof cnd, ins);
                else snprintf(cnd, sizeof cnd, "bu(%uu)", (ins >> 22) & 15);
                emit("      if (%s) {\n", cnd); PUSH(dst, dst + num, p + 1, "0", "0", "false"); emit("      pc = %uu; } else pc = %uu;\n      break;\n", dst, p + 1);
                break;
            case 0x27: case 0x28:                                                                           /* IFU / IFC */
                if (op == 0x27) snprintf(cnd, sizeof cnd, "bu(%uu)", (ins >> 22) & 15); else cond_expr(cnd, sizeof cnd, ins);
                emit("      if (%s) {\n", cnd); PUSH(p + 1, dst, dst + num, "0", "0", "false"); emit("      pc = %uu; } else {\n", p + 1);
                PUSH(dst, dst + num, dst + num, "0", "0", "false"); emit("      pc = %uu; }\n      break;\n", dst);
                break;
            case 0x29: {                                                                                    /* LOOP */
                char rep[64], inc[64]; uint32_t i = (ins >> 22) & 3;
                snprintf(rep, sizeof rep, "int(d.iu[%u] & 0xffu)", i); snprintf(inc, sizeof inc, "(int(d.iu[%u] << 8) >> 24)", i);
                emit("      aL = int((d.iu[%u] >> 8) & 0xffu);\n", i);
                PUSH(p + 1, dst + 1, dst + 1, rep, inc, "true"); emit("      pc = %uu; break;\n", p + 1);
                break;
            }
            case 0x2c: cond_expr(cnd, sizeof cnd, ins); emit("      pc = %s ? %uu : %uu; break;\n", cnd, dst, p + 1); break;          /* JMPC */
            case 0x2d: emit("      pc = (bu(%uu) != %s) ? %uu : %uu; break;\n", (ins >> 22) & 15, (num & 1) ? "true" : "false", dst, p + 1); break;   /* JMPU */
            default: emit("      pc = %uu; break;\n", p + 1); break;
            }
            break;
        }
        emit("    }\n");
    }
    #undef PUSH
    emit("    default: pc = 4096u; break;\n    }\n    if (pc >= 4096u) break;\n  }\n");
    /* output map */
    char sem[24][16];
    for (int i = 0; i < 24; i++) snprintf(sem[i], sizeof sem[i], "%s", i == 3 || i == 11 ? "1.0" : "0.0");
    uint32_t nout = R(0x04F) & 7, mask = R(0x2BD);
    for (uint32_t i = 0, o = 0; i < 16 && o < nout; i++) {
        if (!((mask >> i) & 1)) continue;
        uint32_t m = R(0x050 + o);
        for (int c = 0; c < 4; c++) { uint32_t se = (m >> (8 * c)) & 0x1f; if (se < 24) snprintf(sem[se], sizeof sem[se], "o%u.%c", i, comp[c]); }
        o++;
    }
    emit("  gl_Position = vec4(%s, %s, -(%s), %s);\n", sem[0], sem[1], sem[2], sem[3]);
    emit("  v_col = min(abs(vec4(%s, %s, %s, %s)), vec4(1.0));\n", sem[8], sem[9], sem[10], sem[11]);
    emit("  v_uv01 = vec4(%s, %s, %s, %s);\n  v_uv2tw = vec3(%s, %s, %s);\n", sem[12], sem[13], sem[14], sem[15], sem[22], sem[23], sem[16]);
    emit("  v_quat = vec4(%s, %s, %s, %s);\n  v_view = vec3(%s, %s, %s);\n}\n", sem[4], sem[5], sem[6], sem[7], sem[18], sem[19], sem[20]);
    return 0;
}

/* ---- cache ---- */
typedef struct { uint64_t key; VkShaderModule mod; } Entry;
static Entry *cache; static int ncache, cap;
static uint32_t prog_hash_version = UINT32_MAX; static uint64_t prog_hash;
unsigned g_vsgen_compiled, g_vsgen_failed;

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v; h *= 0x100000001B3ull; h ^= h >> 31; return h; }

/* specialized module for the current registers, VK_NULL_HANDLE when unavailable (caller uses the interpreter) */
VkShaderModule pica_vsgen_module(VkDevice dev) {
    if (!shaderc_load()) return VK_NULL_HANDLE;
    if (prog_hash_version != pica_vs_version) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (int i = 0; i < 4096 + 128; i++) h = mix(h, pica_vs_prog[i]);
        prog_hash = h; prog_hash_version = pica_vs_version;
    }
    uint64_t key = mix(mix(mix(mix(mix(prog_hash, R(0x2BA) & 0xffff), R(0x2BB)), R(0x2BC)), R(0x2BD)), (uint32_t)(pica_num_attrs() - 1) | (R(0x04F) & 7) << 8);
    for (int i = 0; i < 7; i++) key = mix(key, R(0x050 + i));
    for (int i = 0; i < ncache; i++) if (cache[i].key == key) return cache[i].mod;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (!generate()) {
        sc_result r = sc_compile_into_spv(compiler, out, olen, 0 /* vertex */, "pica_vs_gen.vert", "main", options);
        if (r && sc_result_get_compilation_status(r) == 0) {
            VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sc_result_get_length(r), .pCode = (const uint32_t *)sc_result_get_bytes(r) };
            if (vkCreateShaderModule(dev, &ci, NULL, &mod)) mod = VK_NULL_HANDLE;
        } else {
            fprintf(stderr, "vsgen: compile failed: %s\n", r ? sc_result_get_error_message(r) : "(no result)");
            if (getenv("PWR_VK_TRACE")) fprintf(stderr, "%.*s\n", (int)olen, out);
        }
        if (r) sc_result_release(r);
    }
    if (mod) g_vsgen_compiled++; else g_vsgen_failed++;
    if (ncache == cap) { cap = cap ? cap * 2 : 64; cache = realloc(cache, (size_t)cap * sizeof *cache); }
    cache[ncache].key = key; cache[ncache].mod = mod; ncache++;   /* failures are cached too (interpreter fallback) */
    return mod;
}
