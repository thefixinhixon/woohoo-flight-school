#ifndef PWR_RUNTIME_H
#define PWR_RUNTIME_H
#include "cpu.h"
int rt_mem_init(void);                                 /* reserve the 4 GiB guest address space */
int rt_load_image(const char *code_bin, uint32_t base); /* load the decompressed ExeFS .code */
int rt_replace_file(const char *tmp, const char *dst);  /* rename() that also replaces dst on Windows */
void rt_mem_commit(uint32_t addr, size_t len);         /* Windows: commit guest pages before file I/O into them */
GuestFn rt_lookup(uint32_t target);                    /* guest address -> recompiled function, or NULL */
#endif
