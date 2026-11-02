/* Runs recompiled game routines whose behaviour is known from the verified Python tools, and compares.
 * usage: test_known <code.bin> <crc_cases.bin> <qlz_input.bin> <qlz_expected.bin>
 *   crc_cases.bin: repeated {u32 len, u32 expected_crc, u8 data[len]}
 *   qlz_input.bin: QuickLZ stream (PMCr payload after its 16-byte header); qlz_expected.bin: decompressed bytes */
#include "cpu.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

void f_001c4564(CPU *c);   /* mgi_crc(ptr, len) */
void f_002b34c4(CPU *c);   /* QuickLZ level-3 decompress(src, dst, state) -> size */

#define STACK_TOP 0x10000000u
#define BUF_A     0x20000000u
#define BUF_B     0x22000000u
#define SCRATCH   0x24000000u
#define RET_MAGIC 0xfffffff0u

static void *slurp(const char *p, long *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n ? *n : 1); if (fread(b, 1, *n, f) != (size_t)*n) exit(2); fclose(f); return b;
}

static void call(GuestFn fn, CPU *c) {
    c->r[13] = STACK_TOP - 0x100;
    c->r[14] = RET_MAGIC;
    fn(c);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s code.bin crc_cases.bin qlz_input.bin qlz_expected.bin\n", argv[0]); return 2; }
    if (rt_mem_init() || rt_load_image(argv[1], 0x100000)) { fprintf(stderr, "memory/image setup failed\n"); return 2; }
    int fails = 0;

    long n; uint8_t *cases = slurp(argv[2], &n);
    int ncrc = 0, okcrc = 0;
    for (long off = 0; off + 8 <= n; ) {
        uint32_t len, want; memcpy(&len, cases + off, 4); memcpy(&want, cases + off + 4, 4);
        memcpy(g_mem + BUF_A, cases + off + 8, len);
        CPU c = {0}; c.r[0] = BUF_A; c.r[1] = len;
        call(f_001c4564, &c);
        ncrc++; if (c.r[0] == want) okcrc++; else if (fails++ < 5) fprintf(stderr, "crc len %u: got %08x want %08x\n", len, c.r[0], want);
        off += 8 + len;
    }
    printf("mgi_crc:  %d/%d match\n", okcrc, ncrc);

    long nin, nexp; uint8_t *in = slurp(argv[3], &nin), *exp = slurp(argv[4], &nexp);
    memcpy(g_mem + BUF_A, in, nin);
    memset(g_mem + BUF_B, 0xcd, nexp + 64);
    CPU c = {0}; c.r[0] = BUF_A; c.r[1] = BUF_B; c.r[2] = SCRATCH;
    call(f_002b34c4, &c);
    int same = memcmp(g_mem + BUF_B, exp, nexp) == 0;
    printf("quicklz:  returned %u (expected %ld), output %s\n", c.r[0], nexp, same ? "identical" : "DIFFERS");
    if (!same || c.r[0] != (uint32_t)nexp) fails++;
    return fails ? 1 : 0;
}
