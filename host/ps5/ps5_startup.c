/* (PS5) The console's part of the start, called from host/hos_main.c's main: two calls there, the rest here, so the shared
   file changes little when upstream's does. */
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

void platform_init(const char *title_name);   /* (host/ps5/platform.c) */
void hos_ps5_quit_watch(void);                /* (host/ps5/win32_ps5.c, as the ones below) */
void hos_ps5_timing_probe(void);
void hos_ps5_cpu_topology(void);
void hos_ps5_sample_start(void);
void hos_ps5_cpu_meter(void);

/* Before anything logs: standard error into klog, the shell's splash dismissed, and a test run's clean stop. */
void hos_ps5_startup(const char *title_name) {
    platform_init(title_name);
    hos_ps5_quit_watch();
}

/* After longshot_env.txt is read: what the console plays with unless that file says otherwise. The game's memory, the
   renderer's and the program share one pool (12016 MiB on the base console): the handheld profile, small textures, the
   largest left out and every ASTC one kept as BC. The numbers are the ones tested on the Hookshot base over sessions of 10
   to 13 minutes (2026-11-12, docs/PS5.md); on this base they are a starting point, to be measured again.
     - HOS_MEMORY_MB: what the game is told it has (less than 9300 did not boot).
     - HOS_PS5_MEMINFO_RESERVE_MB: the game is told the console's free memory less this (host/hos_mem.c).
     - HOS_PS5_RENDER_MB, HOS_PS5_RESERVE_MB: the renderer's own limit, and what it leaves the game (gpu/vk/vk_vma.cpp).
     - RX_VK_PRECOMPILE=0: programs are read when drawn, not held from the start.
     - RX_VK_COMPACT=0: the VRAM compaction off; GPU faults followed it on the console.
     - RX_VK_FORCE_MESH_FALLBACK=1: the task and mesh stages as compute; the console's RADV hangs on the native ones. */
static void set_defaults(void) {
    static const char *const defaults[][2] = {
        {"HOS_HANDHELD", "1"}, {"RX_VK_TEX_MAX_DIM", "384"}, {"RX_VK_TEX_SKIP_MB", "16"}, {"RX_VK_ASTC_BC_ALL", "1"},
        {"RX_VK_ASTC_BC_MB", "0"}, {"RX_VK_FRAMEGEN_MULT", "2"}, {"RX_VK_PACE", "1"}, {"RX_VK_FSR_FG", "0"},
        {"RX_VK_NO_WARM", "1"}, {"RX_VK_DESC_POOL_DIV", "8"},
        {"HOS_MEMORY_MB", "9300"}, {"HOS_PS5_MEMINFO_RESERVE_MB", "800"}, {"HOS_PS5_RENDER_MB", "1500"},
        {"HOS_PS5_RESERVE_MB", "3072"}, {"RX_VK_PRECOMPILE", "0"}, {"RX_VK_WB_DIFF", "0"},
        {"RX_VK_IMPORT_CHUNK_MB", "1"}, {"RX_VK_IMPORT_SPAN_MB", "4"}, {"RX_VK_COMPACT", "0"},
        {"RX_VK_WORKERS", "1"}, {"RX_VK_FORCE_MESH_FALLBACK", "1"},
        /* (the renderer's image blocks at 16 MB, not 128: a block goes back to the console's pool only once empty, and the
           pool is shared with the game's pages and the heap. Below HOS_PS5_TEX_RESERVE_MB free, an image of 1 MB and up
           gets memory of its own: gpu/vk/vk_vma.cpp) */
        {"RX_VK_IMAGE_BLOCK_MB", "16"}, {"HOS_PS5_TEX_RESERVE_MB", "800"},
    };
    for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++) setenv(defaults[i][0], defaults[i][1], 0);
}

/* Before any C++ object of the renderer is made: some of its switches are read into constants at that time
   (RX_VK_IMAGE_BLOCK_MB: gpu/vk/vk_vma.cpp kImageBlock), long before main. Set from main, the 16 MB blocks were never
   taken: 11 blocks of 128 MB held 645 MB of images (2026-11-15). A constructor with a priority runs ahead of the ones
   without. Such a switch cannot be changed from longshot_env.txt, which is read in main. */
__attribute__((constructor(101))) static void early_defaults(void) { set_defaults(); }

void hos_ps5_defaults(void) {
    set_defaults();   /* (again: harmless, and the one place to look) */
    hos_ps5_cpu_topology();
    hos_ps5_timing_probe();   /* (HOS_PS5_TIMING=1) */
    hos_ps5_sample_start();   /* (HOS_PS5_SAMPLE=<start>,<seconds>) */
    hos_ps5_cpu_meter();      /* (HOS_PS5_CPU=1) */
}

/* (host/hos_crash_record.c) A read of memory that may not be there, for the crash record's stack walk. Linux asks the
   kernel (process_vm_readv), which answers an error where the address is not mapped; the console has no such call a
   title may make, and a plain read from a signal handler would fault again. Nothing is read: the record keeps the
   registers and the faulting address, and leaves the walk out. */
int hos_ps5_safe_read(uintptr_t address, void *out, size_t size) {
    (void)address, (void)out, (void)size;
    return 0;
}
