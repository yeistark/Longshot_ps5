/* (PS5) The guest's address layout on the console, over host/hos.h's: included from there, after its own.
   64 GiB of the 39-bit space: the window sits at A64_HOST_BASE = 64 GiB and ends where the platform layer's heap reserves
   its 16 GiB (128 GiB, PS5_HEAP_HINT; Mesa's device memory follows at 256 GiB: host/ps5/win32_ps5.h). The guest's heap
   starts at 16 GiB instead of 64, and its address regions come down from 64 GiB. */
#ifndef HOS_PS5_LAYOUT_H
#define HOS_PS5_LAYOUT_H
#undef HOS_HEAP_BASE
#undef HOS_GUEST_SPACE
#undef HOS_SPACE_END
#define HOS_HEAP_BASE 0x400000000ULL
#define HOS_GUEST_SPACE 0x1000000000ULL
#define HOS_SPACE_END HOS_GUEST_SPACE
#endif
