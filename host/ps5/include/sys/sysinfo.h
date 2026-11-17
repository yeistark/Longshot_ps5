/* (PS5) Linux's sysinfo, for upstream's host code (host/mkw_glue.c hos_alloc_gave_up: the memory left when an allocation is
   refused): the console's free direct memory, with no buffers and no swap. ps5/build.py puts this folder on the include path. */
#pragma once
#include <stdint.h>
#include <string.h>

struct sysinfo {
    unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned int mem_unit;
};
uint64_t hos_ps5_direct_room(void);   /* (host/ps5/win32_ps5.c) */
static inline int sysinfo(struct sysinfo *si) {
    memset(si, 0, sizeof *si);
    si->freeram = (unsigned long)hos_ps5_direct_room();
    si->totalram = 12016ul << 20;
    si->mem_unit = 1;
    return 0;
}
