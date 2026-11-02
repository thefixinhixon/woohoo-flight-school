/* Guest-address -> recompiled function table (generated table.c), sorted by address.
 * Bit 0 of addr is set for Thumb functions. */
#ifndef PWR_TABLE_H
#define PWR_TABLE_H
#include "cpu.h"
typedef struct { uint32_t addr; GuestFn fn; } FnEntry;
extern const FnEntry g_fn_table[];
extern const unsigned g_fn_count;
#endif
