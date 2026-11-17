/* PS5: the Win32 calls host/posix/win32_posix.c leaves to the console (host/ps5/win32_ps5.h): memory, thread and fiber
   stacks, futexes, modules and the process's exit. Built into the PS5 executable only (ps5/build.py).

   Memory. The guest window [HOS_PS5_GUEST_LO, HOS_PS5_GUEST_HI) is reserved in 16 GiB pieces (the most the console was
   measured to grant at a hint: PS5_PayloadSDK platform/docs/PROBE.md, "Virtual space") and committed in 64 KiB units,
   one state byte each. A commit maps direct memory taken from blocks of 64 MiB or more (one allocation per block, not
   per unit: a run of units is one mapping), read-write for the CPU and the GPU: the Vulkan back end imports the game's
   pools from these pages (VK_EXT_external_memory_host), and RADV's import grants GPU access the same way, so a later
   protection change here must keep the GPU bits or the GPU faults on the pool. A committed unit is never committed
   again (that would be a protection change too) and never given back: discards zero in place (host/hos_main.c
   hos_posix_guest_discard). Anything VirtualAlloc reserves outside the window is the platform layer's ps5_vrange. */
#define _GNU_SOURCE
#include <windows.h>

#include <errno.h>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/umtx.h>

#include <ps5platform/kernel.h>
#include <ps5platform/shm.h>
#include <ps5platform/heap.h>

#include "win32_ps5.h"

#undef dlopen
#undef dlsym
#undef dlclose

int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceKernelUsleep(unsigned microseconds);
int32_t sceKernelMprotect(const void *address, size_t bytes, int protection);

#define UNIT 0x10000ULL                       /* commit granularity: Windows' allocation granularity */
#define PIECE (16ULL << 30)                   /* reservation step */
#define BLOCK (64ULL << 20)                   /* smallest direct-memory block taken for guest pages */
#define GPU_RW 0x30                           /* the kernel's GPU read and write protection bits (RADV's own mappings) */
#define DIRECT_TYPE 12                        /* write-back, CPU and GPU coherent: RADV's and ProsperoEden's guest type */
#define GUEST_UNITS ((HOS_PS5_GUEST_HI - HOS_PS5_GUEST_LO) / UNIT)
#define LAZY_COMMIT (64ULL << 20)

static DWORD set_error(DWORD e) { SetLastError(e); return e; }
int sceKernelDebugOutText(int channel, const char *text);
/* A line straight to klog: standard error goes through the platform layer's pipe and thread, which a crash outruns */
void hos_ps5_klog_now(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n] = '\n', line[n + 1] = 0;
    sceKernelDebugOutText(0, line);
}

/* ---- the guest window ---------------------------------------------------------------------------------------- */
enum { U_RESERVED = 0, U_COMMITTED = 1 };
static uint8_t *unit_state;                   /* GUEST_UNITS bytes, made with the reservation */
static uint8_t *unit_ahead;                   /* (HOS_PS5_ZERO_SCAN) 1: backed by hos_ps5_commit_now, not by a touch */
static int64_t *unit_phys;                    /* a committed unit's direct memory (for its decommit) */
static int64_t *free_units;                   /* direct memory of decommitted units, taken first by the next commits */
static size_t free_count, free_cap;
static int window_reserved;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t block_next, block_end;         /* direct memory of the current block not mapped yet */
static uint64_t committed_bytes;
/* (HOS_PS5_COMMIT_TRACE=1, read-only: no unit is ever decommitted in practice (confirmed: "guest decommit" never
   appears in a session's klog), so every commit is a unit's first-ever touch. high_water_unit is the highest unit
   index committed so far in the game's own heap, plus one; a commit at or past it is new ground the game has never
   touched in this process, one short of it is a unit some earlier, non-contiguous commit skipped over and this one
   now fills in. Whether a zone change's commits are almost all past the frontier (an arena that only grows) or
   mostly fill gaps (something closer to reuse) is exactly what decides whether a future reclaim could ever help.
   Scoped to host/hos.h's HOS_HEAP_BASE..+hos_heap_size (SetMemoryHeapSize's one block, bottom-up): the address
   regions above it (stacks, TLS) are handed out top-down from the window's other end and would swamp this the
   moment any of them is touched, for a question they have nothing to do with */
#define HOS_PS5_HEAP_BASE 0x400000000ULL   /* (host/hos.h's HOS_HEAP_BASE under __PROSPERO__, always true in this file) */
extern uint64_t hos_heap_size;             /* (host/hos_mem.c: SetMemoryHeapSize's current size) */
static uint64_t high_water_unit, commit_frontier_bytes, commit_hole_bytes;

static int in_window(uintptr_t lo, uintptr_t hi) { return lo >= HOS_PS5_GUEST_LO && hi <= HOS_PS5_GUEST_HI && lo < hi; }

static int reserve_window(void) {
    if (window_reserved) return 1;
    unit_state = calloc(GUEST_UNITS, 1);
    unit_phys = calloc(GUEST_UNITS, sizeof *unit_phys);
    if (!unit_state || !unit_phys) return 0;
    for (uintptr_t at = HOS_PS5_GUEST_LO; at < HOS_PS5_GUEST_HI; at += PIECE) {
        const int r = ps5_vrange_reserve_at((void *)at, PIECE);
        if (r) {
            fprintf(stderr, "[ps5] the guest window: %#llx+16 GiB not reserved (%#x)\n", (unsigned long long)at, (unsigned)r);
            for (uintptr_t back = HOS_PS5_GUEST_LO; back < at; back += PIECE) ps5_vrange_release((void *)back, PIECE);
            free(unit_state), unit_state = NULL;
            return 0;
        }
    }
    window_reserved = 1;
    fprintf(stderr, "[ps5] the guest window: %#llx..%#llx reserved\n", (unsigned long long)HOS_PS5_GUEST_LO,
            (unsigned long long)HOS_PS5_GUEST_HI);
    return 1;
}

/* The pool's free pieces of a unit or more, the smallest first, for the guest's pages once no whole block is free. The
   halving below took the largest piece each time, and the pool ended as hundreds of pieces under 2 MiB, which is what
   the runtime heap's next segment needs: 147 MiB free in 272 pieces and 4 KiB not found for 3 s (Hyrule Field, the
   title gone). A guest page fits anywhere, so it takes the small pieces and leaves the large ones. Walked at most
   every 100 ms; a piece another allocation took meanwhile fails and the list is read again. */
static struct { int64_t at; uint64_t len; } small_gaps[1024];
static int small_gap_count, small_gap_next;
static double small_gaps_at;
static double now_ms(void);
static int gap_cmp(const void *a, const void *b) {
    const uint64_t x = ((const uint64_t *)a)[1], y = ((const uint64_t *)b)[1];
    return x < y ? -1 : x > y;
}
static void small_gaps_read(void) {
    struct { int64_t start, end; int type; } q;
    int sceKernelDirectMemoryQuery(int64_t offset, int flags, void *info, size_t size);
    const int64_t total = (int64_t)sceKernelGetDirectMemorySize();
    int64_t off = 0, last_end = 0;
    small_gap_count = small_gap_next = 0;
    for (;;) {
        const int more = off < total && sceKernelDirectMemoryQuery(off, 1, &q, sizeof q) == 0 && q.end > q.start;
        const int64_t gap_end = more ? q.start : total;
        const int64_t a = (last_end + (int64_t)UNIT - 1) & ~((int64_t)UNIT - 1), b = gap_end & ~((int64_t)UNIT - 1);
        if (b > a && small_gap_count < (int)(sizeof small_gaps / sizeof *small_gaps))
            small_gaps[small_gap_count].at = a, small_gaps[small_gap_count].len = (uint64_t)(b - a), small_gap_count++;
        if (!more) break;
        last_end = q.end, off = q.end;
    }
    qsort(small_gaps, (size_t)small_gap_count, sizeof *small_gaps, gap_cmp);
    small_gaps_at = now_ms();
}
static int small_gap_take(uint64_t most, int64_t *at, uint64_t *size) {
    for (int pass = 0; pass < 2; pass++) {
        if (pass || small_gap_next >= small_gap_count || now_ms() - small_gaps_at > 100) small_gaps_read();
        while (small_gap_next < small_gap_count) {
            const int64_t a = small_gaps[small_gap_next].at;
            uint64_t len = small_gaps[small_gap_next].len;
            small_gap_next++;
            if (len > most) len = most;
            if (sceKernelAllocateDirectMemory(a, a + (int64_t)len, len, UNIT, DIRECT_TYPE, at) == 0) {
                *size = len;
                return 1;
            }
        }
    }
    return 0;
}

/* n bytes of direct memory, contiguous in the current block or a new one of at least BLOCK; -1 when there is none */
static int64_t direct_take(uint64_t n, uint64_t *got) {
    if (block_next >= block_end) {
        uint64_t size = n > BLOCK ? (n + BLOCK - 1) & ~(BLOCK - 1) : BLOCK;
        if (size > (1ULL << 30)) size = 1ULL << 30;   /* (a large commit takes 1 GiB blocks, one after another) */
        int64_t at = 0;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, UNIT, DIRECT_TYPE, &at) == 0) {
            block_next = at, block_end = at + (int64_t)size;
        } else if (small_gap_take(size, &at, &size)) {
            block_next = at, block_end = at + (int64_t)size;
        } else {
        /* (fragmented: smaller, down to one unit. In Hyrule Castle Town 842 MiB were free in 203 pieces, the largest
           under a block: a commit of 37 MiB found "no direct memory left" and the game wrote to the page it did not get) */
        while (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, UNIT, DIRECT_TYPE, &at)) {
            if (size <= UNIT) return -1;
            size = (size / 2 + UNIT - 1) & ~(UNIT - 1);
        }
        block_next = at, block_end = at + (int64_t)size;
        }
    }
    const uint64_t left = (uint64_t)(block_end - block_next);
    *got = n < left ? n : left;
    const int64_t at = block_next;
    block_next += (int64_t)*got;
    return at;
}

static double now_ms(void);

/* (HOS_PS5_COMMIT_TRACE: what the game asks for and gives back, a line a second while it moves 64 MiB or more: its area
   changes show as pages asked for first and given back after, or the two together. Called with window_lock held) */
static uint64_t flow_up, flow_down;
static double flow_t0;
static void guest_flow(uint64_t up, uint64_t down) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_PS5_COMMIT_TRACE") != NULL;
    if (!on) return;
    const double t = now_ms();
    if (!flow_t0) flow_t0 = t;
    flow_up += up, flow_down += down;
    if (t - flow_t0 < 1000) return;
    if (flow_up >= (64ULL << 20) || flow_down >= (64ULL << 20))
        hos_ps5_klog_now("[ps5] guest pages in %.1f s: %llu MiB asked for, %llu MiB given back (%llu MiB backed)", (t - flow_t0) / 1e3,
                         (unsigned long long)(flow_up >> 20), (unsigned long long)(flow_down >> 20), (unsigned long long)(committed_bytes >> 20));
    flow_up = flow_down = 0, flow_t0 = t;
}
/* Maps fresh zeroed memory over every reserved unit of [lo, hi); window_lock held */
static int commit_units(uintptr_t lo, uintptr_t hi) {
    const double commit_t0 = now_ms();
    uint64_t commit_bytes = 0;
    uint64_t u = (lo - HOS_PS5_GUEST_LO) / UNIT;
    const uint64_t end = (hi - HOS_PS5_GUEST_LO + UNIT - 1) / UNIT;
    while (u < end) {
        if (unit_state[u] == U_COMMITTED) { u++; continue; }
        uint64_t run = u;
        while (run < end && unit_state[run] != U_COMMITTED) run++;
        uint64_t need = (run - u) * UNIT;
        while (need) {
            uint64_t got = 0;
            int64_t phys;
            if (free_count) {   /* (decommitted units' memory, again: as many as lie one after another, in one mapping; a run
                                   was pushed lowest first, so it comes off the top highest first) */
                uint64_t k = 1;
                while (k < need / UNIT && k < free_count && free_units[free_count - 1 - k] == free_units[free_count - k] - (int64_t)UNIT) k++;
                free_count -= k;
                phys = free_units[free_count], got = k * UNIT;
            } else phys = direct_take(need, &got);
            if (phys < 0) return 0;   /* (commit_units_wait says so) */
            void *want = (void *)(HOS_PS5_GUEST_LO + u * UNIT), *at = want;
            const int r = sceKernelMapDirectMemory(&at, got, PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | GPU_RW,
                                                   PS5_KERNEL_MAP_FIXED, phys, UNIT);
            if (r || at != want) {
                hos_ps5_klog_now("[ps5] guest commit: mapping %#llx+%#llx failed (%#x)", (unsigned long long)(uintptr_t)want,
                                 (unsigned long long)got, (unsigned)r);
                return 0;
            }
            memset(want, 0, got);   /* (the kernel hands direct memory out as it was left: PROBE.md) */
            for (uint64_t k = 0; k < got / UNIT; k++) {
                unit_phys[u + k] = phys + (int64_t)(k * UNIT);
                __atomic_store_n(&unit_state[u + k], U_COMMITTED, __ATOMIC_RELEASE);
            }
            if (getenv("HOS_PS5_COMMIT_TRACE")) {
                const uint64_t heap_lo = HOS_PS5_HEAP_BASE / UNIT;
                const uint64_t heap_hi = (HOS_PS5_HEAP_BASE + hos_heap_size + UNIT - 1) / UNIT;
                const uint64_t clip_lo = u > heap_lo ? u : heap_lo;
                const uint64_t batch_hi = u + got / UNIT;
                const uint64_t clip_hi = batch_hi < heap_hi ? batch_hi : heap_hi;
                if (clip_lo < clip_hi) {
                    if (clip_lo >= high_water_unit) commit_frontier_bytes += (clip_hi - clip_lo) * UNIT;
                    else if (clip_hi <= high_water_unit) commit_hole_bytes += (clip_hi - clip_lo) * UNIT;
                    else {
                        commit_hole_bytes += (high_water_unit - clip_lo) * UNIT;
                        commit_frontier_bytes += (clip_hi - high_water_unit) * UNIT;
                    }
                    if (clip_hi > high_water_unit) high_water_unit = clip_hi;
                }
            }
            committed_bytes += got;
            commit_bytes += got;
            guest_flow(got, 0);
            u += got / UNIT, need -= got;
        }
    }
    if (now_ms() - commit_t0 >= 20)
        hos_ps5_klog_now("[ps5] guest commit: %llu MiB backed in %.0f ms", (unsigned long long)(commit_bytes >> 20), now_ms() - commit_t0);
    return 1;
}

/* commit_units, waiting for memory when there is none: the game gives an area's pages back while it loads the next (leaving
   Hyrule Castle Town a worker's page found none with 85 MiB free, and the game was gone), the renderer's emptied blocks and
   the heap's free segments go back too. Tried again for up to 10 s before the caller is told no; window_lock held on
   entry and on return, not while waiting */
static int commit_units_wait(uintptr_t lo, uintptr_t hi) {
    for (int tries = 0;; tries++) {
        if (commit_units(lo, hi)) {
            if (tries) hos_ps5_klog_now("[ps5] guest commit: memory found after %d ms", tries * 20);
            return 1;
        }
        if (!tries || tries == 500)
            hos_ps5_klog_now("[ps5] guest commit: no direct memory left (%llu MiB committed)%s",
                             (unsigned long long)(committed_bytes >> 20), tries ? "" : ": waiting for some");
        if (tries == 500) return 0;
        pthread_mutex_unlock(&window_lock);
#ifdef PS5_HEAP_TRIM
        if (!tries) ps5_heap_trim();
#endif
        sceKernelUsleep(20000);
        pthread_mutex_lock(&window_lock);
    }
}

/* (ps5platform/heap.h) The heap's first segment, taken whole at the first allocation and kept: 1280 MiB, a little over what a
   long session's heap comes to (1107 MiB at its most in eleven minutes at "Balanced"). The console's pool ends in thousands of
   free pieces, and a request the heap had to ask the pool for found none of its size: 9 MiB for a 2048 x 1152 texture's bytes
   with 777 MiB free, the largest piece 3 MiB, an uncaught std::bad_alloc, the title closed. From its own segment the heap needs
   room in itself. What it costs: early in a session the heap holds some 500 MiB it does not use yet.
   (Longshot, 2026-11-15) On this base the heap comes to 1200-1400 MiB within minutes: ~600 MiB of shader modules the driver
   keeps (400 of them read from the program cache at the start) and ~300 MiB of the programs' lowered code
   (HOS_PS5_HEAP_PROFILE=1). 1792 MiB was tried for it and took what the game's pages needed: the console ran out whole
   (4 MiB free) and the title stood still. The answer is to hold less, as PS5_Hookshot came to (modules made for a pipeline's
   creation and let go, the lowered code let go once picked), not a larger segment. */
#ifdef PS5_HEAP_FIRST_ARENA
/* (1024 MiB with the shader modules kept compressed, host/ps5/ps5_vk_modules.cpp: what the segment holds and the heap does
   not use is taken from the game's pages, and the console ran out whole with 650 MiB of it unused)
   (1408 MiB, 2026-11-17, upstream's update with FSR 3.1 and frame interpolation on: the heap came to 1161 MiB in use, 1298
   mapped in 914 segments, and a 7 MiB request of texture_view_of found no piece of its size (425 MiB free, the largest 6
   MiB): std::bad_alloc, the title closed. Without them it held 830-850 MiB; 1408 covers the larger with room) */
const size_t ps5_heap_first_arena_bytes = (size_t)1408 << 20;
#endif

int hos_ps5_unit_backed(uintptr_t addr);

/* (window_lock held) The decommitted units the window keeps for the game's next commits (free_units) beyond HOS_PS5_KEEP_MB (256,
   0: none, -1: all, as before) go back to the console. They were held in full: after the game was told how little the console had
   left it gave back 1.0 to 2.0 GB in a session, which the console counted as taken, so it read as "0 free" to the game and to
   the renderer's rule, and the heap and the driver could not use a byte of it. The oldest are released, in runs of units that lie
   one after another, at most 2000 runs a call (each is a system call under the lock); a run the console refuses stays kept and
   ends the releasing for good */
static int cmp_phys(const void *a, const void *b) {
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
static void release_kept_excess(void) {
    static int64_t keep_mb = -2;
    static int refused;
    if (keep_mb == -2) {
        const char *e = getenv("HOS_PS5_KEEP_MB");
        keep_mb = e ? atoll(e) : 256;
    }
    if (keep_mb < 0 || refused) return;
    const size_t keep_units = (size_t)((uint64_t)keep_mb << 20) / UNIT;
    if (free_count <= keep_units) return;
    const size_t n = free_count - keep_units;
    int64_t *tmp = malloc(n * sizeof *tmp);
    if (!tmp) return;
    memcpy(tmp, free_units, n * sizeof *tmp);
    qsort(tmp, n, sizeof *tmp, cmp_phys);
    size_t i = 0, runs = 0;
    while (i < n && runs < 2000) {
        size_t j = i + 1;
        while (j < n && tmp[j] == tmp[j - 1] + (int64_t)UNIT) j++;
        if (sceKernelReleaseDirectMemory(tmp[i], (size_t)(j - i) * UNIT) != 0) {
            refused = 1;
            hos_ps5_klog_now("[ps5] guest decommit: the console refused to take %llu MiB of kept units back at %#llx: they stay kept",
                             (unsigned long long)((j - i) * UNIT >> 20), (unsigned long long)tmp[i]);
            break;
        }
        i = j, runs++;
    }
    if (i) {   /* (tmp[0..i) are released: what stays is tmp[i..n), at the bottom, and the newest above it) */
        const size_t stay = n - i, top = free_count - n;
        memmove(free_units + stay, free_units + n, top * sizeof *free_units);
        memcpy(free_units, tmp + i, stay * sizeof *tmp);
        free_count = stay + top;
        static int told;
        if (told++ < 30)
            hos_ps5_klog_now("[ps5] guest decommit: %llu KiB of kept units given back to the console in %zu runs (%llu MiB stay kept)",
                             (unsigned long long)(i * UNIT >> 10), runs, (unsigned long long)((uint64_t)free_count * UNIT >> 20));
    }
    free(tmp);
}
uint64_t hos_ps5_kept_bytes(void) { return (uint64_t)free_count * UNIT; }

/* Guest memory the game gave back: each whole committed unit in [lo, hi) is unmapped and reserved again, its direct
   memory kept for the next commits; the parts of units only partly inside read as zero. Without it every scene the game
   loaded added memory it never returned: 7.1 GB of guest pages when Link's house loaded, and the next page found none.
   Pages a GPU import holds never come here (host/hos_mem.c keeps and zeroes those). */
int hos_ps5_decommit(void *addr, size_t size) {
    const uintptr_t lo = (uintptr_t)addr, hi = lo + size;
    if (!size || !window_reserved || !in_window(lo, hi)) return 0;
    const uintptr_t ulo = (lo + UNIT - 1) & ~(uintptr_t)(UNIT - 1), uhi = hi & ~(uintptr_t)(UNIT - 1);
    if (ulo >= uhi) {   /* (no whole unit) */
        if (hos_ps5_unit_backed(lo) && hos_ps5_unit_backed(hi - 1)) memset(addr, 0, size);
        return 1;
    }
    if (lo < ulo && hos_ps5_unit_backed(lo)) memset(addr, 0, ulo - lo);
    if (uhi < hi && hos_ps5_unit_backed(uhi)) memset((void *)uhi, 0, hi - uhi);
    /* (a run of committed units at a time, not a unit: two system calls a 64 KiB unit under the window's lock were 1.3 s of
       45 while a scene was up, and the game stood still for 1.8 s whenever it gave a large region back) */
    const double t0 = now_ms();
    uint64_t units = 0;
    pthread_mutex_lock(&window_lock);
    for (uintptr_t at = ulo; at < uhi;) {
        uint64_t u = (at - HOS_PS5_GUEST_LO) / UNIT;
        if (unit_state[u] != U_COMMITTED) { at += UNIT; continue; }
        uint64_t n = 1;
        while (at + n * UNIT < uhi && unit_state[u + n] == U_COMMITTED) n++;
        if (free_count + n > free_cap) {
            size_t cap = free_cap ? free_cap * 2 : 65536;
            while (cap < free_count + n) cap *= 2;
            int64_t *grown = realloc(free_units, cap * sizeof *grown);
            if (!grown) { memset((void *)at, 0, n * UNIT); at += n * UNIT; continue; }   /* (no room to keep them: zeroed and kept) */
            free_units = grown, free_cap = cap;
        }
        for (uint64_t k = 0; k < n; k++) __atomic_store_n(&unit_state[u + k], U_RESERVED, __ATOMIC_RELEASE);
        void *place = (void *)at;
        if (sceKernelMunmap(place, n * UNIT) != 0 ||
            sceKernelReserveVirtualRange(&place, n * UNIT, PS5_KERNEL_MAP_FIXED, UNIT) != 0 || place != (void *)at) {
            hos_ps5_klog_now("[ps5] guest decommit: %#llx+%#llx could not be reserved again", (unsigned long long)at,
                             (unsigned long long)(n * UNIT));
            at += n * UNIT;
            continue;   /* (their memory is lost to the game; the units read as reserved and are backed afresh if touched) */
        }
        for (uint64_t k = 0; k < n; k++) free_units[free_count++] = unit_phys[u + k];
        committed_bytes -= n * UNIT;
        units += n;
        guest_flow(0, n * UNIT);
        at += n * UNIT;
    }
    release_kept_excess();
    pthread_mutex_unlock(&window_lock);
    if (now_ms() - t0 >= 20) hos_ps5_klog_now("[ps5] guest decommit: %llu MiB given back in %.0f ms", (unsigned long long)(units * UNIT >> 20), now_ms() - t0);
    return 1;
}

static int prot_bits(DWORD protect) {
    switch (protect & 0xFF) {
    case PAGE_NOACCESS: return 0;
    case PAGE_READONLY: case PAGE_EXECUTE_READ: return PS5_KERNEL_PROT_CPU_READ;
    default: return PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    }
}

/* ---- reservations outside the window: the platform layer's ranges, with their sizes for MEM_RELEASE ---- */
static struct { uintptr_t base; size_t size; } regions[256];
static pthread_mutex_t regions_lock = PTHREAD_MUTEX_INITIALIZER;

static void *reserve_elsewhere(void *addr, SIZE_T size) {
    void *base = NULL;
    const size_t n = (size + UNIT - 1) & ~(size_t)(UNIT - 1);
    if ((addr ? ps5_vrange_reserve_at(addr, n) : ps5_vrange_reserve(n, NULL, UNIT, &base)) != 0) return NULL;
    if (addr) base = addr;
    pthread_mutex_lock(&regions_lock);
    for (int i = 0; i < 256; i++)
        if (!regions[i].base) { regions[i].base = (uintptr_t)base, regions[i].size = n; break; }
    pthread_mutex_unlock(&regions_lock);
    return base;
}

void *VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD protect) {
    const uintptr_t a = (uintptr_t)addr;
    if (type & MEM_RESET) {   /* (the contents are not wanted: they read as zero after this) */
        if (in_window(a, a + size)) hos_ps5_decommit(addr, size);
        return addr;
    }
    if (a == HOS_PS5_GUEST_LO && size == HOS_PS5_GUEST_HI - HOS_PS5_GUEST_LO && !(type & MEM_COMMIT)) {
        pthread_mutex_lock(&window_lock);
        const int ok = reserve_window();
        pthread_mutex_unlock(&window_lock);
        if (!ok) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        return addr;
    }
    if (addr && in_window(a, a + size)) {
        if (!(type & MEM_COMMIT)) return addr;   /* (reserved already) */
        /* A large commit (the heap's 6.9 GB at SetHeapSize, big address regions) stays reserved: its pages are backed as
           they are first touched (mkw_glue.c commit_on_touch), as Windows and Linux back untouched committed pages with
           nothing. Committed at once, the console ran out of direct memory with 9.2 GB of guest pages, most never used.
           What the kernel or the GPU must find backed is committed with hos_ps5_commit_now (ReadFile and WriteFile
           buffers, the renderer's imported pools). */
        if (size >= LAZY_COMMIT) return addr;
        pthread_mutex_lock(&window_lock);
        const int ok = window_reserved && commit_units_wait(a & ~(uintptr_t)(UNIT - 1), a + size);
        pthread_mutex_unlock(&window_lock);
        if (!ok) { set_error(ERROR_COMMITMENT_LIMIT); return NULL; }
        return addr;
    }
    void *base = addr;
    if (!addr || (type & MEM_RESERVE)) {
        base = reserve_elsewhere(addr, size);
        if (!base) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    }
    if (type & MEM_COMMIT) {
        const int p = (prot_bits(protect) & PS5_KERNEL_PROT_CPU_WRITE) ? PS5_SHM_READ | PS5_SHM_WRITE : PS5_SHM_READ;
        if (ps5_vrange_commit(base, size, p) != 0) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    }
    return base;
}

/* Whether a guest address's 64 KiB unit is backed (a signal handler's question: no lock) */
int hos_ps5_unit_backed(uintptr_t addr) {
    if (!window_reserved || !in_window(addr, addr + 1)) return 0;
    return __atomic_load_n(&unit_state[(addr - HOS_PS5_GUEST_LO) / UNIT], __ATOMIC_ACQUIRE) == U_COMMITTED;
}

/* Guest pages backed now, whatever their size (the lazy rule above is for the runtime's own commits) */
int hos_ps5_commit_now(void *addr, size_t size) {
    const uintptr_t a = (uintptr_t)addr;
    if (!size || !in_window(a, a + size)) return 1;
    pthread_mutex_lock(&window_lock);
    if (window_reserved && unit_ahead)   /* (HOS_PS5_ZERO_SCAN: the units backed here before the game touched them) */
        for (uint64_t u = (a - HOS_PS5_GUEST_LO) / UNIT; u < (a + size - HOS_PS5_GUEST_LO + UNIT - 1) / UNIT; u++)
            if (unit_state[u] != U_COMMITTED) unit_ahead[u] = 1;
    const int ok = window_reserved && commit_units_wait(a & ~(uintptr_t)(UNIT - 1), a + size);
    pthread_mutex_unlock(&window_lock);
    return ok;
}

BOOL VirtualFree(void *addr, SIZE_T size, DWORD type) {
    const uintptr_t a = (uintptr_t)addr;
    if (in_window(a, a + (size ? size : 1))) {
        if (type & MEM_DECOMMIT) hos_ps5_decommit(addr, size);
        return TRUE;
    }
    if (type & MEM_DECOMMIT) return ps5_vrange_decommit(addr, size) == 0;
    if (type & MEM_RELEASE) {
        pthread_mutex_lock(&regions_lock);
        for (int i = 0; i < 256; i++)
            if (regions[i].base == a) {
                ps5_vrange_release(addr, regions[i].size);
                regions[i].base = 0;
                pthread_mutex_unlock(&regions_lock);
                return TRUE;
            }
        pthread_mutex_unlock(&regions_lock);
    }
    set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/* Whole 16 KiB kernel pages; guest pages keep their GPU bits (see the top of the file) */
BOOL VirtualProtect(void *addr, SIZE_T size, DWORD protect, DWORD *old) {
    const uintptr_t lo = (uintptr_t)addr & ~(uintptr_t)0x3FFF, hi = ((uintptr_t)addr + size + 0x3FFF) & ~(uintptr_t)0x3FFF;
    if (old) *old = PAGE_READWRITE;
    const int gpu = in_window(lo, hi) ? GPU_RW : 0;
    return sceKernelMprotect((void *)lo, hi - lo, prot_bits(protect) | gpu) == 0 ? TRUE : (set_error(ERROR_INVALID_PARAMETER), FALSE);
}

SIZE_T VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T size) {
    if (size < sizeof *mbi) return 0;
    memset(mbi, 0, sizeof *mbi);
    const uintptr_t a = (uintptr_t)addr;
    mbi->BaseAddress = (void *)(a & ~(uintptr_t)0xFFF);
    mbi->Type = 0x20000;
    if (window_reserved && in_window(a, a + 1)) {
        const uint64_t u = (a - HOS_PS5_GUEST_LO) / UNIT;
        const uint8_t s = __atomic_load_n(&unit_state[u], __ATOMIC_ACQUIRE);
        uint64_t end = u + 1;
        while (end < GUEST_UNITS && __atomic_load_n(&unit_state[end], __ATOMIC_RELAXED) == s) end++;
        mbi->AllocationBase = (void *)HOS_PS5_GUEST_LO;
        mbi->RegionSize = HOS_PS5_GUEST_LO + end * UNIT - (uintptr_t)mbi->BaseAddress;
        mbi->State = s == U_COMMITTED ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = mbi->AllocationProtect = s == U_COMMITTED ? PAGE_READWRITE : PAGE_NOACCESS;
        return sizeof *mbi;
    }
    /* shortcut: outside the window every address reads as committed; ask the kernel (sceKernelQueryMemoryProtection)
       when a caller needs more */
    mbi->AllocationBase = mbi->BaseAddress;
    mbi->RegionSize = 0x1000;
    mbi->State = MEM_COMMIT, mbi->Protect = mbi->AllocationProtect = PAGE_READWRITE;
    return sizeof *mbi;
}

/* ---- stacks: direct memory with an unmapped guard page below (libkernel's come from flexible memory) ---------- */
static volatile int64_t stack_bytes, stack_count;   /* (the meter's: what the stacks have backed, and how many) */

/* Stacks grow as they are used. A stack reserves its whole size (and a guard unit below it) but backs only its top
   STACK_FIRST at first; a fault in the reserved part below what is backed (mc_err: page not present) backs down to the
   faulting unit and the access is retried (hos_ps5_stack_fault, from win32_posix.c's signal handler). The handler runs
   on a small alternate stack of the thread's own (hos_ps5_signal_stack): the faulting thread has no stack left to run
   it on. Each stack was 8 or 16 MiB backed whole: 1.7 GB for 96 threads that touch a fraction of it. */
#define STACK_FIRST (256u << 10)
#define STACK_SLOTS 1024
typedef struct { uintptr_t low, high, backed; } StackRange;   /* (usable [low, high), backed from `backed` up) */
static StackRange stacks[STACK_SLOTS];
static pthread_mutex_t stacks_lock = PTHREAD_MUTEX_INITIALIZER;

void *hos_ps5_stack_alloc(size_t *size) {
    const size_t n = (*size + UNIT - 1) & ~(size_t)(UNIT - 1);
    void *base = NULL;
    if (ps5_vrange_reserve(n + UNIT, NULL, UNIT, &base) != 0) return NULL;
    const uintptr_t low = (uintptr_t)base + UNIT, high = low + n;
    const size_t first = n < STACK_FIRST ? n : STACK_FIRST;
    if (ps5_vrange_commit((void *)(high - first), first, PS5_SHM_READ | PS5_SHM_WRITE) != 0) {
        ps5_vrange_release(base, n + UNIT);
        return NULL;
    }
    pthread_mutex_lock(&stacks_lock);
    int slot = -1;
    for (int i = 0; i < STACK_SLOTS && slot < 0; i++)
        if (!stacks[i].high) slot = i;
    if (slot >= 0) __atomic_store_n(&stacks[slot].low, low, __ATOMIC_RELAXED), stacks[slot].backed = high - first,
                   __atomic_store_n(&stacks[slot].high, high, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&stacks_lock);
    if (slot < 0) {   /* (no slot: backed whole, as before) */
        if (ps5_vrange_commit((void *)low, n, PS5_SHM_READ | PS5_SHM_WRITE) != 0) {
            ps5_vrange_release(base, n + UNIT);
            return NULL;
        }
        __atomic_add_fetch(&stack_bytes, (int64_t)(n - first), __ATOMIC_RELAXED);
    }
    *size = n;
    __atomic_add_fetch(&stack_bytes, (int64_t)first, __ATOMIC_RELAXED);
    __atomic_add_fetch(&stack_count, 1, __ATOMIC_RELAXED);
    return (void *)low;   /* (the unit below stays reserved: the guard) */
}
void hos_ps5_stack_free(void *stack, size_t size) {
    const uintptr_t low = (uintptr_t)stack;
    pthread_mutex_lock(&stacks_lock);
    for (int i = 0; i < STACK_SLOTS; i++)
        if (stacks[i].high && stacks[i].low == low) {
            __atomic_sub_fetch(&stack_bytes, (int64_t)(stacks[i].high - stacks[i].backed), __ATOMIC_RELAXED);
            stacks[i].high = 0;
            goto found;
        }
    __atomic_sub_fetch(&stack_bytes, (int64_t)size, __ATOMIC_RELAXED);
found:
    pthread_mutex_unlock(&stacks_lock);
    __atomic_sub_fetch(&stack_count, 1, __ATOMIC_RELAXED);
    ps5_vrange_release((char *)stack - UNIT, size + UNIT);
}
/* A page-not-present fault at addr: 1 when it is in a stack's reserved part, now backed down to it (retry the access) */
int hos_ps5_stack_fault(uintptr_t addr) {
    for (int i = 0; i < STACK_SLOTS; i++) {
        StackRange *r = &stacks[i];
        const uintptr_t high = __atomic_load_n(&r->high, __ATOMIC_ACQUIRE);
        if (!high || addr < r->low || addr >= high) continue;
        pthread_mutex_lock(&stacks_lock);
        int ok = 1;
        if (addr < r->backed) {
            const uintptr_t from = (addr & ~(uintptr_t)(UNIT - 1)) > r->low + UNIT ? (addr & ~(uintptr_t)(UNIT - 1)) - UNIT : r->low;
            ok = ps5_vrange_commit((void *)from, r->backed - from, PS5_SHM_READ | PS5_SHM_WRITE) == 0;
            if (ok) __atomic_add_fetch(&stack_bytes, (int64_t)(r->backed - from), __ATOMIC_RELAXED), r->backed = from;
        }
        pthread_mutex_unlock(&stacks_lock);
        return ok;
    }
    return 0;
}
/* The calling thread's alternate signal stack (64 KiB of direct memory), so a stack fault can be handled */
void hos_ps5_signal_stack(void) {
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) return;
    void *base = NULL;
    if (ps5_vrange_reserve(64u << 10, NULL, UNIT, &base) != 0) return;
    if (ps5_vrange_commit(base, 64u << 10, PS5_SHM_READ | PS5_SHM_WRITE) != 0) return;
    stack_t ss = {.ss_sp = base, .ss_size = 64u << 10, .ss_flags = 0};
    sigaltstack(&ss, NULL);
}

/* Threads on those stacks. A Win32 thread here is detached; its stack can only go once it has ended, so it is made
   joinable and handed at its end to a reaper thread that joins it and frees the stack. Guest threads ask for 64 MiB,
   every byte of which the console commits: they get HOS_PS5_STACK_MB (16) at most. */
typedef struct Reap {
    struct Reap *next;
    pthread_t th;
    void *stack;
    size_t size;
    void *(*fn)(void *);
    void *arg;
} Reap;
static Reap *reap_list;
static pthread_mutex_t reap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reap_wake = PTHREAD_COND_INITIALIZER;
static pthread_once_t reaper_once = PTHREAD_ONCE_INIT;

static void *reaper_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&reap_lock);
    for (;;) {
        while (!reap_list) pthread_cond_wait(&reap_wake, &reap_lock);
        Reap *r = reap_list;
        reap_list = r->next;
        pthread_mutex_unlock(&reap_lock);
        pthread_join(r->th, NULL);
        hos_ps5_stack_free(r->stack, r->size);
        free(r);
        pthread_mutex_lock(&reap_lock);
    }
    return NULL;
}
static void reaper_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, reaper_main, NULL) == 0) pthread_detach(t);
}

void hos_ps5_sample_register(void);
static void *thread_trampoline(void *p) {
    Reap *r = (Reap *)p;
    hos_ps5_signal_stack();
    hos_ps5_sample_register();
    void *result = r->fn(r->arg);
    pthread_mutex_lock(&reap_lock);   /* (joined once this function has returned: the stack is not freed under it) */
    r->next = reap_list, reap_list = r;
    pthread_cond_signal(&reap_wake);
    pthread_mutex_unlock(&reap_lock);
    return result;
}

static size_t stack_cap(void) {
    static size_t cap;
    if (!cap) {
        const char *e = getenv("HOS_PS5_STACK_MB");
        cap = (size_t)(e && atoi(e) > 0 ? atoi(e) : 16) << 20;
    }
    return cap;
}

int hos_ps5_thread_start(pthread_t *th, size_t stack, void *(*fn)(void *), void *arg) {
    pthread_once(&reaper_once, reaper_start);
    size_t size = stack < stack_cap() ? stack : stack_cap();
    Reap *r = (Reap *)calloc(1, sizeof *r);
    if (!r) return ENOMEM;
    r->stack = hos_ps5_stack_alloc(&size);
    if (!r->stack) { free(r); return ENOMEM; }
    r->size = size, r->fn = fn, r->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstack(&a, r->stack, size);
    const int e = pthread_create(&r->th, &a, thread_trampoline, r);
    pthread_attr_destroy(&a);
    if (e) {
        hos_ps5_stack_free(r->stack, size);
        free(r);
        return e;
    }
    *th = r->th;
    return 0;
}

/* ---- futexes and thread ids ---- */
long hos_ps5_futex(volatile uint32_t *word, int op, uint32_t value, const struct timespec *relative) {
    if (op == FUTEX_WAKE_PRIVATE) return _umtx_op((void *)word, UMTX_OP_WAKE_PRIVATE, value, NULL, NULL);
    struct timespec t = relative ? *relative : (struct timespec){0};
    /* (a timespec in uaddr2 with its size in uaddr is a relative timeout; FreeBSD answers ETIMEDOUT as Linux does) */
    return _umtx_op((void *)word, UMTX_OP_WAIT_UINT_PRIVATE, value, relative ? (void *)(uintptr_t)sizeof t : NULL,
                    relative ? &t : NULL);
}
pid_t hos_ps5_tid(void) { return (pid_t)pthread_getthreadid_np(); }

/* ---- modules: the libraries the Linux build loads are linked in ---- */
#define MOD_VK ((void *)(uintptr_t)0x48530001)
#define MOD_VULKAN ((void *)(uintptr_t)0x48530002)
#define MOD_SELF ((void *)(uintptr_t)0x48530003)

void *mkwvk_init(const void *host);                                      /* (gpu/vk/vk_main.cpp) */
void mkwvk_close_asked(void);
/* (RADV, linked in, behind the console's own answers for shader modules: host/ps5/ps5_vk_modules.cpp) */
void *hos_ps5_vkGetInstanceProcAddr(void *instance, const char *name);
extern volatile uint8_t hos_posix_keys[256] __attribute__((weak));       /* (host/posix/display_sdl.c) */
extern volatile int hos_posix_window[4] __attribute__((weak));
int hos_posix_post_message(unsigned msg, uintptr_t wp, intptr_t lp) __attribute__((weak));
int hos_posix_guest_discard(void *p, size_t n) __attribute__((weak));   /* (host/hos_main.c) */
int hos_posix_guest_memfd(void) __attribute__((weak));

void *hos_ps5_dlopen(const char *path, int mode) {
    (void)mode;
    if (!path) return MOD_SELF;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strncasecmp(base, "longshot_vk.", 12)) return MOD_VK;
    if (!strncasecmp(base, "libvulkan.so", 12) || !strcasecmp(base, "vulkan-1.dll")) return MOD_VULKAN;
    return NULL;   /* (SDL, X11, Wayland ...: not on the console; host/ps5/hos_sdl_ps5.c stands in for SDL) */
}

void *hos_ps5_dlsym(void *module, const char *name) {
    if (!name) return NULL;
    if (module == MOD_VK) {
        if (!strcmp(name, "mkwvk_init")) return (void *)mkwvk_init;
        if (!strcmp(name, "mkwvk_close_asked")) return (void *)mkwvk_close_asked;
        return NULL;
    }
    if (module == MOD_VULKAN) return !strcmp(name, "vkGetInstanceProcAddr") ? (void *)hos_ps5_vkGetInstanceProcAddr : NULL;
    /* (RTLD_DEFAULT, or the executable: what the Linux build finds across its .so boundary) */
    if (!strcmp(name, "hos_posix_keys")) return (void *)hos_posix_keys;
    if (!strcmp(name, "hos_posix_window")) return (void *)hos_posix_window;
    if (!strcmp(name, "hos_posix_post_message")) return (void *)hos_posix_post_message;
    if (!strcmp(name, "hos_posix_guest_discard")) return (void *)hos_posix_guest_discard;
    if (!strcmp(name, "hos_posix_guest_memfd")) return NULL;   /* (no dma-bufs: pools are host-pointer imports) */
    return NULL;
}

int hos_ps5_dlclose(void *module) { (void)module; return 0; }

uintptr_t hos_ps5_exe_base(void) {
    extern char __executable_start[] __attribute__((weak));
    return (uintptr_t)__executable_start;
}

/* ---- the end: the shell closes the title (exit() and _exit are a crash report) ---- */
void hos_ps5_exit(int code) {
    fprintf(stderr, "[ps5] exit %d: asking the shell to close the title\n", code);
    fflush(NULL);
    (void)sceSystemServiceLoadExec("exit", NULL);
    for (;;) sceKernelUsleep(100000);
}

/* ---- what host/posix/prof.c and the C library give the Linux build ---- */
/* shortcut: no sampling profiler on the console (prof.c reads Linux's perf events); HOS_PROFILE does nothing here */
void hos_prof_start(void) {}

/* timegm: the console's libc has none. UTC fields to seconds since 1970, days counted as civil-from-days does
   (Howard Hinnant's algorithm), any year, months normalised. shortcut: belongs in PS5_PayloadSDK's platform layer */
time_t timegm(struct tm *t) {
    long long y = t->tm_year + 1900LL, m = t->tm_mon;
    y += m / 12, m %= 12;
    if (m < 0) m += 12, y--;
    y -= m < 2;
    const long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 1 ? -2 : 10)) + 2) / 5 + t->tm_mday - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + t->tm_hour * 3600LL + t->tm_min * 60LL + t->tm_sec);
}

/* ---- three calls only libraries a title does not load export (null at run time: ps5/tools/link.sh refuses them) ---- */
int isatty(int fd) { (void)fd; return 0; }   /* (no terminal on the console: standard error goes to klog) */
void hos_ps5_sample_name(pthread_t thread, const char *name);
int pthread_setname_np(pthread_t thread, const char *name) {
    pthread_set_name_np(thread, name);
    hos_ps5_sample_name(thread, name);
    return 0;
}
/* fnmatch for the Win32 FindFirstFile patterns win32_posix.c passes: '*' and '?', FNM_CASEFOLD (the only flag used) */
int fnmatch(const char *pattern, const char *s, int flags) {
    const int fold = (flags & 0x10) != 0;   /* (FNM_CASEFOLD) */
    const char *star = NULL, *resume = NULL;
    while (*s) {
        const char a = fold && *pattern >= 'A' && *pattern <= 'Z' ? (char)(*pattern + 32) : *pattern;
        const char b = fold && *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s;
        if (*pattern == '*') { star = ++pattern, resume = s; continue; }
        if (*pattern && (*pattern == '?' || a == b)) { pattern++, s++; continue; }
        if (!star) return 1;
        pattern = star, s = ++resume;
    }
    while (*pattern == '*') pattern++;
    return *pattern ? 1 : 0;
}

/* ---- a test run's clean stop: ps5/tools/run.py puts /app0/quit-run.txt in the title folder when its measurement is done;
   the title deletes it and leaves through the shell, as a player's quit would (never a kill: PS5_Vulkan's rule) ---- */
#include <unistd.h>
static void *quit_watch(void *unused) {
    (void)unused;
    for (;;) {
        sceKernelUsleep(500000);
        if (access("/app0/quit-run.txt", F_OK) == 0) {
            unlink("/app0/quit-run.txt");
            fprintf(stderr, "[ps5] quit-run.txt: the run is over\n");
            hos_ps5_exit(0);
        }
    }
    return NULL;
}
#ifdef PS5_HEAP_TRIM
/* The heap's free memory goes back to the title's pool every 10 s (the platform heap gives its pieces back only whole, and
   only when asked or every few thousand frees): what a burst of shader compiles took is the game's again soon after.
   Not more often: every 2 s, an area loading mapped and unmapped the same memory again and again (23 fps for 27). */
static void *heap_trim_loop(void *unused) {
    (void)unused;
    for (;;) {
        sceKernelUsleep(10000000);
        ps5_heap_trim();
    }
    return NULL;
}
#endif
void hos_ps5_quit_watch(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, quit_watch, NULL) == 0) pthread_detach(t);
#ifdef PS5_HEAP_TRIM
    if (pthread_create(&t, NULL, heap_trim_loop, NULL) == 0) pthread_detach(t);
#endif
}

/* ---- HOS_PS5_TIMING=1: how long the waits the runtime builds on take on the console (each asked for the time named) ---- */
#include <sched.h>
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
/* The console's CPUs as the title sees them, one line at the start: which logical processors the title may run on, which
   share a core, and which share a level 3 cache (the pins below and GetLogicalProcessorInformation go by it). Read on each
   processor in turn (CPUID 0x8000001e: the core; 0x8000001d, cache 3: how many share it). */
#include <cpuid.h>
uint64_t hos_ps5_l3_mask[2];   /* (the two halves of the title's processors by level 3 cache; 0: not read) */
void hos_ps5_cpu_topology(void) {
    uint64_t was = 0;
    const int got = scePthreadGetaffinity(pthread_self(), &was);
    if (got != 0 || !was) {
        fprintf(stderr, "[ps5] cpus: the thread's affinity could not be read (%#x)\n", (unsigned)got);
        return;
    }
    char line[512];
    int n = 0, first_l3 = -1;
    for (int i = 0; i < 64; i++) {
        if (!(was >> i & 1)) continue;
        if (scePthreadSetaffinity(pthread_self(), 1ull << i) != 0) continue;
        sched_yield();
        unsigned a, b, c, d, share = 0;
        __cpuid_count(0x8000001e, 0, a, b, c, d);
        const unsigned apic = a, core = b & 0xff;
        __cpuid_count(0x8000001d, 3, a, b, c, d);
        if ((a & 0x1f) && ((a >> 5) & 7) == 3) share = ((a >> 14) & 0xfff) + 1;
        int l3 = (int)apic;
        for (unsigned s = share; s > 1; s >>= 1) l3 >>= 1;   /* (the processors of one cache: the APIC id less its low bits) */
        if (first_l3 < 0) first_l3 = l3;
        if (share) hos_ps5_l3_mask[l3 != first_l3] |= 1ull << i;
        if (n < (int)sizeof line - 32) n += snprintf(line + n, sizeof line - n, " %d:c%u/L%d", i, core, l3);
    }
    scePthreadSetaffinity(pthread_self(), was);
    fprintf(stderr, "[ps5] cpus: allowed %#llx; processor:core/L3%s; L3 halves %#llx %#llx\n", (unsigned long long)was, line,
            (unsigned long long)hos_ps5_l3_mask[0], (unsigned long long)hos_ps5_l3_mask[1]);
}

/* (HOS_PS5_TIMING=1) A file written in the title's folder, and what the other threads feel of it: one that only
   computes and one that opens another file there, while 24 MB are written, closed and renamed as the renderer saves its
   pipeline cache in play. Each step's time, and the longest pause each of the two threads saw. */
static volatile int file_probe_stop;
static double file_probe_gap[2];
static void *file_probe_cpu(void *unused) {
    (void)unused;
    double last = now_ms();
    while (!file_probe_stop) {
        const double t = now_ms();
        if (t - last > file_probe_gap[0]) file_probe_gap[0] = t - last;
        last = t;
    }
    return NULL;
}
static void *file_probe_open(void *unused) {
    (void)unused;
    while (!file_probe_stop) {
        const double t = now_ms();
        FILE *f = fopen("/app0/param.json", "rb");
        if (f) fclose(f);
        if (now_ms() - t > file_probe_gap[1]) file_probe_gap[1] = now_ms() - t;
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }
    return NULL;
}
/* QueryPerformanceCounter's clock (host/posix/windows.h): the time stamp counter's rate as the system gives it, or
   measured against CLOCK_MONOTONIC over 20 ms where it gives none; set before main, and at the first read otherwise. */
uint64_t hos_ps5_tsc0, hos_ps5_tsc_mult;
int64_t hos_ps5_ns0;
__attribute__((constructor)) void hos_ps5_clock_init(void) {
    uint64_t sceKernelGetTscFrequency(void);
    if (hos_ps5_tsc_mult) return;
    struct timespec t;
    uint64_t hz = sceKernelGetTscFrequency();
    if (hz < 100000000ull) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        const int64_t a = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
        const uint64_t ta = __builtin_ia32_rdtsc();
        usleep(20000);
        clock_gettime(CLOCK_MONOTONIC, &t);
        hz = (uint64_t)((__builtin_ia32_rdtsc() - ta) * 1e9 / (double)((int64_t)t.tv_sec * 1000000000LL + t.tv_nsec - a));
    }
    clock_gettime(CLOCK_MONOTONIC, &t);
    hos_ps5_tsc0 = __builtin_ia32_rdtsc();
    hos_ps5_ns0 = (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
    __atomic_store_n(&hos_ps5_tsc_mult, (uint64_t)(((unsigned __int128)1000000000ull << 32) / hz), __ATOMIC_RELEASE);
    fprintf(stderr, "[ps5] clock: the time stamp counter at %llu Hz\n", (unsigned long long)hz);
}

/* The game's package on the console: HOS_PS5_NUP, else the copy an emulator there already keeps, so one 25 GB file serves
   both: PS5_Lichii's folder, then the one it had before 2026-11-10 (when neither is there, the first, for the message). */
const char *hos_ps5_nup(void) {
    static const char *const places[] = {"/data/lichii/roms/oot/oot.nup", "/data/prosperoeden/roms/oot/oot.nup"};
    const char *set = getenv("HOS_PS5_NUP");
    if (set && *set) return set;
    for (size_t i = 0; i < sizeof places / sizeof *places; i++)
        if (access(places[i], R_OK) == 0) return places[i];
    return places[0];
}
static double file_probe_read_gap;
static void *file_probe_read(void *unused) {   /* (the game's own reads: 64 KiB pieces of its package, here and there) */
    (void)unused;
    const char *nup = hos_ps5_nup();
    FILE *f = fopen(nup, "rb");
    if (!f) return NULL;
    static char piece[65536];
    uint64_t at = 12345;
    while (!file_probe_stop) {
        at = at * 6364136223846793005ull + 1442695040888963407ull;
        const double t = now_ms();
        fseeko(f, (off_t)((at >> 20) % (20ull << 30)) & ~(off_t)0xffff, SEEK_SET);
        if (fread(piece, 1, sizeof piece, f) == 0) clearerr(f);
        if (now_ms() - t > file_probe_read_gap) file_probe_read_gap = now_ms() - t;
    }
    fclose(f);
    return NULL;
}
static void file_probe_at(const char *dir) {
    char path[256], tmp[256];
    snprintf(path, sizeof path, "%s/probe-file.bin", dir);
    snprintf(tmp, sizeof tmp, "%s/probe-file.bin.tmp", dir);
    const size_t n = 24u << 20;
    char *data = malloc(n);
    if (!data) return;
    memset(data, 0x5a, n);
    file_probe_stop = 0, file_probe_gap[0] = file_probe_gap[1] = 0;
    pthread_t a, b, c;
    file_probe_read_gap = 0;
    pthread_create(&a, NULL, file_probe_cpu, NULL);
    pthread_create(&b, NULL, file_probe_open, NULL);
    pthread_create(&c, NULL, file_probe_read, NULL);
    { struct timespec warm = {0, 300000000}; nanosleep(&warm, NULL); }
    const double idle_read = file_probe_read_gap;
    file_probe_read_gap = 0;
    const double t0 = now_ms();
    FILE *f = fopen(tmp, "wb");
    const double t1 = now_ms();
    if (f) fwrite(data, 1, n, f);
    const double t2 = now_ms();
    if (f) fclose(f);
    const double t3 = now_ms();
    rename(tmp, path);
    const double t4 = now_ms();
    unlink(path);
    const double t5 = now_ms();
    file_probe_stop = 1;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_join(c, NULL);
    free(data);
    fprintf(stderr, "[ps5] timing: a read of 64 KiB from the game's package: %.1f ms at most with nothing written, %.1f ms while "
            "the 24 MB went to %s\n", idle_read, file_probe_read_gap, dir);
    fprintf(stderr, "[ps5] timing: 24 MB to %s: open %.1f ms, write %.1f, close %.1f, rename %.1f, unlink %.1f; meanwhile a "
            "computing thread paused %.1f ms at most, one opening a file in /app0 %.1f ms\n", dir, t1 - t0, t2 - t1, t3 - t2,
            t4 - t3, t5 - t4, file_probe_gap[0], file_probe_gap[1]);
}
/* (HOS_PS5_TIMING=2) A file grown to 2 GiB in pieces of 24 MB, each piece timed: whether a write at the end of a large file
   costs more than one at the end of a small one (the program cache is one file of several GB, written at its end). */
static void file_probe_grow(void) {
    const size_t n = 24u << 20;
    char *data = malloc(n);
    if (!data) return;
    memset(data, 0x5a, n);
    FILE *f = fopen("/app0/probe-grow.bin", "wb");
    if (!f) { free(data); return; }
    setvbuf(f, NULL, _IONBF, 0);
    char line[1024];
    int at = 0;
    for (int i = 0; i < 86; i++) {
        const double t0 = now_ms();
        fwrite(data, 1, n, f);
        if (i % 6 == 0 && at < (int)sizeof line - 24) at += snprintf(line + at, sizeof line - at, " %d MB: %.0f ms;", i * 24, now_ms() - t0);
        if (i % 6 == 0) fprintf(stderr, "[ps5] timing: (growing) at %d MB a piece of 24 MB took %.0f ms\n", i * 24, now_ms() - t0);
    }
    const double t0 = now_ms();
    fclose(f);
    const double t1 = now_ms();
    unlink("/app0/probe-grow.bin");
    free(data);
    fprintf(stderr, "[ps5] timing: 24 MB written at the end of a file of%s close %.0f ms, unlink %.0f ms\n", line, t1 - t0, now_ms() - t1);
}
static void hos_ps5_file_probe(void) {
    if (getenv("HOS_PS5_TIMING") && *getenv("HOS_PS5_TIMING") == '2') file_probe_grow();
    file_probe_at("/app0");
    file_probe_at("/data/homebrew/PPSA99025");
    mkdir("/data/longshot", 0777);
    file_probe_at("/data/longshot");
}

void hos_ps5_timing_probe(void) {
    const char *e = getenv("HOS_PS5_TIMING");
    if (!e || (*e != '1' && *e != '2')) return;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_condattr_t ca;
    pthread_cond_t cv;
    pthread_condattr_init(&ca);
    const int clock_set = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &ca);
    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_nsec += 10000000;
    if (at.tv_nsec >= 1000000000) at.tv_sec++, at.tv_nsec -= 1000000000;
    pthread_mutex_lock(&mu);
    double t0 = now_ms();
    const int r = pthread_cond_timedwait(&cv, &mu, &at);
    const double cond_ms = now_ms() - t0;
    pthread_mutex_unlock(&mu);
    volatile uint32_t word = 0;
    struct timespec ten = {0, 10000000};
    t0 = now_ms();
    const long fr = hos_ps5_futex(&word, FUTEX_WAIT_PRIVATE, 0, &ten);
    const double futex_ms = now_ms() - t0;
    t0 = now_ms();
    for (int i = 0; i < 10; i++) Sleep(1);
    const double sleep_ms = (now_ms() - t0) / 10;
    struct timespec one = {0, 1000000};
    t0 = now_ms();
    for (int i = 0; i < 10; i++) nanosleep(&one, NULL);
    const double nano_ms = (now_ms() - t0) / 10;
    t0 = now_ms();
    for (int i = 0; i < 1000; i++) sched_yield();
    const double yield_us = (now_ms() - t0);
    {   /* (the log: longshot_log.txt is unbuffered, a write a line) */
        FILE *f = fopen("/app0/probe-write.txt", "w");
        if (f) {
            setvbuf(f, NULL, _IONBF, 0);
            double worst = 0;
            const double w0 = now_ms();
            for (int i = 0; i < 200; i++) {
                const double s0 = now_ms();
                fputs("[hos probe] a line of about the length the runtime logs, eighty characters or so....\n", f);
                fflush(f);
                if (now_ms() - s0 > worst) worst = now_ms() - s0;
            }
            fprintf(stderr, "[ps5] timing: 200 unbuffered log lines to /app0 took %.1f ms (longest %.2f ms)\n", now_ms() - w0, worst);
            fclose(f);
            unlink("/app0/probe-write.txt");
        }
    }
    {   /* (the VSync thread's sleep: host/hos_thread.c sleep_ns, a waitable timer, 60 times 16.7 ms) */
        HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        double worst = 0;
        const double v0 = now_ms();
        for (int i = 0; i < 60; i++) {
            LARGE_INTEGER due;
            due.QuadPart = -166666;
            const double s0 = now_ms();
            SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0);
            WaitForSingleObject(timer, INFINITE);
            if (now_ms() - s0 > worst) worst = now_ms() - s0;
        }
        fprintf(stderr, "[ps5] timing: 60 waitable-timer sleeps of 16.7 ms took %.1f ms (longest %.1f ms)\n", now_ms() - v0, worst);
    }
    hos_ps5_file_probe();
    fprintf(stderr, "[ps5] timing: cond 10 ms (monotonic clock set %d) took %.2f ms (result %d); futex 10 ms took %.2f ms "
            "(result %ld); Sleep(1) %.2f ms; nanosleep 1 ms %.2f ms; sched_yield %.2f us\n", clock_set, cond_ms, r, futex_ms,
            fr, sleep_ms, nano_ms, yield_us);
}


/* ---- HOS_PS5_SAMPLE=<start>,<seconds>: a sampling profiler. From <start> seconds after launch, for <seconds>, every
   thread started through CreateThread is interrupted (SIGUSR1) every 2 ms in turn, and the chain of calls it was in is
   counted: the instruction, then up to 10 return addresses found through the frame pointers (everything here is built
   with them; a sample in a system library starts from the word at its stack pointer, as its system-call wrappers are
   leaf functions). /app0/ps5_samples.txt gets one line per thread and chain: "<count> <thread> <eboot offset>..."
   (ps5/tools/samples.py names them). No allocation and no thread-local storage in the handler. ---- */
#include <signal.h>
#include <ucontext.h>
#include <ps5platform/context.h>   /* (the console's machine context: mc_rip where the fork's header puts it) */
#define SAMPLE_THREADS 256
#define SAMPLE_SLOTS 2048
#define SAMPLE_DEPTH 11
typedef struct { uint64_t hash; uint32_t count; uint8_t n; uintptr_t at[SAMPLE_DEPTH]; } SampleChain;
typedef struct { pthread_t th; char name[32]; volatile int used; SampleChain chain[SAMPLE_SLOTS]; uint32_t other; } SampleThread;
static SampleThread *sample_threads;
static volatile int sample_n;
static volatile int sample_on;

void hos_ps5_sample_register(void) {
    static _Thread_local int registered;   /* (once a thread: the renderer's own threads come by SetThreadPriority) */
    if (!sample_threads || registered) return;
    registered = 1;
    const int i = __atomic_fetch_add(&sample_n, 1, __ATOMIC_ACQ_REL);
    if (i >= SAMPLE_THREADS) return;
    sample_threads[i].th = pthread_self();
    snprintf(sample_threads[i].name, sizeof sample_threads[i].name, "thread%d", i);
    __atomic_store_n(&sample_threads[i].used, 1, __ATOMIC_RELEASE);
}
void hos_ps5_sample_name(pthread_t thread, const char *name) {
    if (!sample_threads) return;
    for (int i = 0; i < sample_n && i < SAMPLE_THREADS; i++)
        if (sample_threads[i].used && pthread_equal(sample_threads[i].th, thread)) {
            snprintf(sample_threads[i].name, sizeof sample_threads[i].name, "%s", name);
            for (char *c = sample_threads[i].name; *c; c++) if (*c == ' ') *c = '_';
        }
}
static void sample_handler(int sig, siginfo_t *si, void *uctx) {
    (void)sig, (void)si;
    if (!sample_on) return;
    const pthread_t me = pthread_self();
    const ucontext_t *uc = (const ucontext_t *)uctx;
    const uintptr_t base = hos_ps5_exe_base();
    uintptr_t at[SAMPLE_DEPTH];
    int n = 0;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.mc_rip - base;
    at[n++] = rip;
    const uintptr_t lo = (uintptr_t)uc->uc_mcontext.mc_rsp, hi = lo + (16u << 20);
    if (rip >= 0x7f0000000ull) at[n++] = *(const uintptr_t *)lo - base;   /* (a system-call wrapper's caller) */
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.mc_rbp;
    while (n < SAMPLE_DEPTH && fp >= lo && fp < hi && !(fp & 7)) {
        at[n++] = ((const uintptr_t *)fp)[1] - base;
        const uintptr_t next = ((const uintptr_t *)fp)[0];
        if (next <= fp) break;
        fp = next;
    }
    for (int i = 0; i < sample_n && i < SAMPLE_THREADS; i++) {
        SampleThread *t = &sample_threads[i];
        if (!t->used || !pthread_equal(t->th, me)) continue;
        /* (the whole chain; when the thread's table has no place left for a new one, its first three addresses, then the
           instruction alone: a thread with thousands of different chains, the one that replays the draws, lost a quarter
           of its samples to "other", the draws' own, and its profile read as if it waited more than it worked) */
        static const int depths[3] = {SAMPLE_DEPTH, 3, 1};
        for (int d = 0; d < 3; d++) {
            const int m = n < depths[d] ? n : depths[d];
            if (d && m == n) continue;   /* (tried whole already) */
            uint64_t h = 1469598103934665603ull + (uint64_t)m;
            for (int k = 0; k < m; k++) h = (h ^ at[k]) * 1099511628211ull;
            uint32_t slot = (uint32_t)(h >> 40) & (SAMPLE_SLOTS - 1);
            for (int probe = 0; probe < (d ? 512 : 64); probe++, slot = (slot + 1) & (SAMPLE_SLOTS - 1)) {
                SampleChain *c = &t->chain[slot];
                if (c->count && c->hash != h) continue;
                if (!c->count) {
                    c->hash = h, c->n = (uint8_t)m;
                    for (int k = 0; k < m; k++) c->at[k] = at[k];
                }
                c->count++;
                return;
            }
        }
        t->other++;
        return;
    }
}
static void *sampler_main(void *arg) {
    const int start = (int)(intptr_t)arg >> 16, seconds = (int)(intptr_t)arg & 0xffff;
    sceKernelUsleep((unsigned)start * 1000000u);
    sample_on = 1;
    fprintf(stderr, "[ps5] sampling %d threads for %d s\n", sample_n, seconds);
    const double end = now_ms() + seconds * 1000.0;
    while (now_ms() < end) {
        for (int i = 0; i < sample_n && i < SAMPLE_THREADS; i++)
            if (sample_threads[i].used) pthread_kill(sample_threads[i].th, SIGUSR1);
        sceKernelUsleep(2000);
    }
    sample_on = 0;
    sceKernelUsleep(50000);
    FILE *f = fopen("/app0/ps5_samples.txt", "w");
    if (!f) return NULL;
    for (int i = 0; i < sample_n && i < SAMPLE_THREADS; i++) {
        SampleThread *t = &sample_threads[i];
        for (int k = 0; k < SAMPLE_SLOTS; k++) {
            const SampleChain *c = &t->chain[k];
            if (!c->count) continue;
            fprintf(f, "%u %s", c->count, t->name);
            for (int d = 0; d < c->n; d++) fprintf(f, " 0x%lx", (unsigned long)c->at[d]);
            fputc('\n', f);
        }
        if (t->other) fprintf(f, "%u %s other\n", t->other, t->name);
    }
    fclose(f);
    fprintf(stderr, "[ps5] samples written to /app0/ps5_samples.txt\n");
    return NULL;
}
void hos_ps5_sample_start(void) {
    const char *e = getenv("HOS_PS5_SAMPLE");
    int start = 0, seconds = 0;
    if (!e || sscanf(e, "%d,%d", &start, &seconds) != 2 || seconds <= 0) return;
    sample_threads = calloc(SAMPLE_THREADS, sizeof *sample_threads);
    if (!sample_threads) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sample_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, sampler_main, (void *)(intptr_t)(start << 16 | (seconds & 0xffff))) == 0) pthread_detach(t);
}

/* ---- SRW locks (host/posix/windows.h): one word, zero when free. Bit 31 a writer holds it, bit 30 a thread sleeps on
   it, bits 0-29 the readers holding it. A thread that cannot take it spins a little, then sets the sleeper bit and waits
   on the word; a release that finds the bit clears it and wakes every sleeper, which try again. Readers do not come in
   while a sleeper is marked, so a waiting writer is not starved by a stream of readers. ---- */
#define SRW_W 0x80000000u
#define SRW_S 0x40000000u
#define SRW_R 0x3fffffffu
#define SRW_SPINS 100
static void srw_sleep(volatile uint32_t *w, uint32_t seen) {
    if (!(seen & SRW_S) && !__atomic_compare_exchange_n(w, &seen, seen | SRW_S, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    hos_ps5_futex(w, FUTEX_WAIT_PRIVATE, seen | SRW_S, NULL);
}
static void srw_wake(volatile uint32_t *w) {
    __atomic_fetch_and(w, ~SRW_S, __ATOMIC_RELAXED);
    hos_ps5_futex(w, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL);
}
int hos_srw_try_lock(SRWLOCK *l) {
    uint32_t s = __atomic_load_n(&l->w, __ATOMIC_RELAXED);
    while (!(s & (SRW_W | SRW_R)))
        if (__atomic_compare_exchange_n(&l->w, &s, s | SRW_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}
void hos_srw_lock(SRWLOCK *l) {
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(&l->w, __ATOMIC_RELAXED);
        if (!(s & (SRW_W | SRW_R))) {
            if (__atomic_compare_exchange_n(&l->w, &s, s | SRW_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < SRW_SPINS) __builtin_ia32_pause();
        else srw_sleep(&l->w, s);
    }
}
void hos_srw_unlock(SRWLOCK *l) {
    if (__atomic_fetch_and(&l->w, ~SRW_W, __ATOMIC_RELEASE) & SRW_S) srw_wake(&l->w);
}
int hos_srw_try_lock_shared(SRWLOCK *l) {
    uint32_t s = __atomic_load_n(&l->w, __ATOMIC_RELAXED);
    while (!(s & (SRW_W | SRW_S)))
        if (__atomic_compare_exchange_n(&l->w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}
void hos_srw_lock_shared(SRWLOCK *l) {
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(&l->w, __ATOMIC_RELAXED);
        if (!(s & (SRW_W | SRW_S))) {
            if (__atomic_compare_exchange_n(&l->w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < SRW_SPINS) __builtin_ia32_pause();
        else srw_sleep(&l->w, s);
    }
}
void hos_srw_unlock_shared(SRWLOCK *l) {
    const uint32_t s = __atomic_sub_fetch(&l->w, 1, __ATOMIC_RELEASE);
    if (!(s & SRW_R) && (s & SRW_S)) srw_wake(&l->w);
}
void hos_srw_release(SRWLOCK *l) {
    if (__atomic_load_n(&l->w, __ATOMIC_RELAXED) & SRW_W) hos_srw_unlock(l);
    else hos_srw_unlock_shared(l);
}

/* The title's free direct memory in all, whatever its pieces' sizes: every allocated range walked (~240 of them in play). */
static uint64_t direct_room_now(uint64_t *pieces) {
    struct { int64_t start, end; int type; } q;
    int sceKernelDirectMemoryQuery(int64_t offset, int flags, void *info, size_t size);
    const int64_t total = (int64_t)sceKernelGetDirectMemorySize();
    uint64_t held = 0, n = 0;
    int64_t off = 0, last_end = 0;
    while (off < total && sceKernelDirectMemoryQuery(off, 1, &q, sizeof q) == 0 && q.end > q.start) {
        if (q.start > last_end) n++;
        held += (uint64_t)(q.end - q.start), last_end = q.end, off = q.end;
    }
    if (last_end < total) n++;
    if (pieces) *pieces = n;
    return (uint64_t)total - held;
}
/* The same for the renderer, which asks at every texture it makes (gpu/vk/vk_texture.cpp): counted at most every 50 ms. */
uint64_t hos_ps5_direct_room(void) {
    static int64_t at;
    static uint64_t room;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t was = __atomic_load_n(&at, __ATOMIC_RELAXED);
    if (!was || now.QuadPart - was > 50000000LL) {
        __atomic_store_n(&room, direct_room_now(NULL), __ATOMIC_RELAXED);
        __atomic_store_n(&at, now.QuadPart, __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&room, __ATOMIC_RELAXED);
}

/* HOS_PS5_ZERO_SCAN=1: how much of the game's backed memory holds nothing but zeros, by 64 KiB unit (pages it never wrote, or
   cleared: what a first-touch commit would not have backed). Read 64 MiB at a time under the window's lock, so no unit goes
   away under the read; units the game has made unreadable are left out */
extern int hos_ps5_gpu_imports(uint64_t *out, int cap) __attribute__((weak));   /* (gpu/vk/vk_memory.cpp) */
static uint64_t zero_in_imports, zero_out_touched, zero_out_ahead, backed_in_imports;   /* (bytes: the last scan's) */
static int zero_imports_known;
static uint64_t zero_units_now(uint64_t *ahead) {
    uint64_t zero = 0;
    *ahead = 0;
    /* (which of them the GPU can reach: the renderer's imports, as guest ranges, marked a bit a unit) */
    static uint64_t ranges[2 * 4096];
    static uint8_t *reach;
    if (!reach) reach = calloc(GUEST_UNITS / 8 + 1, 1);
    const int nr = hos_ps5_gpu_imports && reach ? hos_ps5_gpu_imports(ranges, 4096) : -1;
    zero_imports_known = nr >= 0;
    zero_in_imports = zero_out_touched = zero_out_ahead = backed_in_imports = 0;
    if (nr >= 0) {
        memset(reach, 0, GUEST_UNITS / 8 + 1);
        for (int r = 0; r < nr; r++)
            for (uint64_t k = ranges[2 * r] / UNIT; k < (ranges[2 * r + 1] + UNIT - 1) / UNIT && k < GUEST_UNITS; k++)
                reach[k >> 3] |= (uint8_t)(1u << (k & 7));
    }
    for (uint64_t u = 0; u < GUEST_UNITS; u += 1024) {
        pthread_mutex_lock(&window_lock);
        for (uint64_t k = u; k < u + 1024 && k < GUEST_UNITS; k++) {
            if (unit_state[k] != U_COMMITTED) continue;
            void *at = (void *)(HOS_PS5_GUEST_LO + k * UNIT), *start = NULL, *end = NULL;
            uint32_t prot = 0;
            if (sceKernelQueryMemoryProtection(at, &start, &end, &prot) != 0 || !(prot & PS5_KERNEL_PROT_CPU_READ) ||
                (uintptr_t)end < (uintptr_t)at + UNIT)
                continue;
            const uint64_t *p = at;
            size_t i = 0;
            while (i < UNIT / 8 && !p[i]) i++;
            zero += i == UNIT / 8;
            if (i == UNIT / 8 && unit_ahead && unit_ahead[k]) (*ahead)++;
            if (nr >= 0) {
                const int in = (reach[k >> 3] >> (k & 7)) & 1;
                if (in) backed_in_imports += UNIT;
                if (i == UNIT / 8) {
                    if (in) zero_in_imports += UNIT;
                    else if (unit_ahead && unit_ahead[k]) zero_out_ahead += UNIT;
                    else zero_out_touched += UNIT;
                }
            }
        }
        pthread_mutex_unlock(&window_lock);
    }
    *ahead *= UNIT;
    return zero * UNIT;
}

/* ---- HOS_PS5_CPU=1: the whole process's CPU use every 5 s (every thread, the ones the sampler cannot see too) ---- */
#include <sys/resource.h>
static void *cpu_meter(void *unused) {
    (void)unused;
    if (getenv("HOS_PS5_ZERO_SCAN")) {
        uint8_t *marks = calloc(GUEST_UNITS, 1);
        pthread_mutex_lock(&window_lock);
        unit_ahead = marks;
        pthread_mutex_unlock(&window_lock);
    }
    struct rusage before;
    getrusage(RUSAGE_SELF, &before);
    double t0 = now_ms();
    for (;;) {
        sceKernelUsleep(5000000);
        struct rusage now;
        getrusage(RUSAGE_SELF, &now);
        const double t = now_ms();
        const double user = (now.ru_utime.tv_sec - before.ru_utime.tv_sec) * 1e3 + (now.ru_utime.tv_usec - before.ru_utime.tv_usec) / 1e3;
        const double sys = (now.ru_stime.tv_sec - before.ru_stime.tv_sec) * 1e3 + (now.ru_stime.tv_usec - before.ru_stime.tv_usec) / 1e3;
        int64_t at = 0;
        size_t largest = 0;
        sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &at, &largest);
        struct ps5_heap_stats heap = {0};
        ps5_heap_stats(&heap);
        {   /* (who holds the title's direct memory: what is taken, what is free in how many pieces, and what the guest window
               keeps without using it (the units a decommit left, the rest of its current block)) */
            uint64_t pieces = 0;
            const uint64_t room = direct_room_now(&pieces), total = sceKernelGetDirectMemorySize();
            pthread_mutex_lock(&window_lock);
            const uint64_t kept = (uint64_t)free_count * UNIT, ahead = (uint64_t)(block_end - block_next);
            pthread_mutex_unlock(&window_lock);
            if (getenv("HOS_PS5_ZERO_SCAN")) {
                const double z0 = now_ms();
                uint64_t ahead = 0;
                const uint64_t zero = zero_units_now(&ahead);
                fprintf(stderr, "[ps5] guest pages: %llu MiB of %llu backed hold only zeros, %llu MiB of those backed for the "
                        "GPU or a file read before the game touched them (read in %.0f ms)\n", (unsigned long long)(zero >> 20),
                        (unsigned long long)(committed_bytes >> 20), (unsigned long long)(ahead >> 20), now_ms() - z0);
                if (zero_imports_known)
                    fprintf(stderr, "[ps5] guest pages the GPU can reach (the renderer's imports): %llu MiB backed, %llu MiB of "
                            "them only zeros; only zeros out of its reach: %llu MiB the game touched, %llu MiB backed ahead\n",
                            (unsigned long long)(backed_in_imports >> 20), (unsigned long long)(zero_in_imports >> 20),
                            (unsigned long long)(zero_out_touched >> 20), (unsigned long long)(zero_out_ahead >> 20));
            }
            if (getenv("HOS_PS5_COMMIT_TRACE")) {
                pthread_mutex_lock(&window_lock);
                const uint64_t hw = high_water_unit, frontier = commit_frontier_bytes, hole = commit_hole_bytes;
                pthread_mutex_unlock(&window_lock);
                const uint64_t heap_lo = HOS_PS5_HEAP_BASE / UNIT;
                const uint64_t heap_hi = (HOS_PS5_HEAP_BASE + hos_heap_size + UNIT - 1) / UNIT;
                fprintf(stderr, "[ps5] guest commit shape: %llu MiB ever past the frontier, %llu MiB filled in short of it "
                        "(unit %llu of %llu of the heap touched at the furthest)\n", (unsigned long long)(frontier >> 20),
                        (unsigned long long)(hole >> 20), (unsigned long long)(hw > heap_lo ? hw - heap_lo : 0),
                        (unsigned long long)(heap_hi - heap_lo));
                {   /* (where the backed pages are, by 256 MiB of the guest window: what grows while the game plays) */
                    char line[512];
                    int len = snprintf(line, sizeof line, "[ps5] guest pages by 256 MiB of the window:");
                    const uint64_t per = (256ULL << 20) / UNIT;
                    for (uint64_t r = 0; r < GUEST_UNITS && len < (int)sizeof line - 40; r += per) {
                        uint64_t n = 0;
                        for (uint64_t k = r; k < r + per && k < GUEST_UNITS; k++)
                            n += __atomic_load_n(&unit_state[k], __ATOMIC_RELAXED) == U_COMMITTED;
                        if (n) len += snprintf(line + len, sizeof line - len, " %llx:%llu",
                                               (unsigned long long)((HOS_PS5_GUEST_LO + r * UNIT) >> 20), (unsigned long long)(n * UNIT >> 20));
                    }
                    fprintf(stderr, "%s (address in MiB: backed MiB)\n", line);
                }
            }
            extern uint64_t radv_ps5_allocated_bytes;   /* (the driver's buffers, all of them: its winsys counts them) */
            fprintf(stderr, "[ps5] direct memory: %llu MiB taken, %llu free in %llu pieces; the driver's buffers %llu MiB; the guest "
                    "window keeps %llu MiB of decommitted units and %llu MiB of its block unused\n",
                    (unsigned long long)((total - room) >> 20), (unsigned long long)(room >> 20), (unsigned long long)pieces,
                    (unsigned long long)(__atomic_load_n(&radv_ps5_allocated_bytes, __ATOMIC_RELAXED) >> 20),
                    (unsigned long long)(kept >> 20), (unsigned long long)(ahead >> 20));
            /* (a driver built with its buffers counted by purpose and size, for diagnosis: PS5_Mesa's radv_ps5_bo.c) */
            extern uint64_t radv_ps5_account_bytes[6][3] __attribute__((weak)), radv_ps5_account_count[6][3] __attribute__((weak));
            if (radv_ps5_account_bytes) {
                static const char *const rows[6] = {"application", "queries", "descriptor pools", "uploads and fences",
                                                    "shader code and scratch", "command streams"};
                char line[640];
                int n = 0;
                for (int r = 0; r < 6; r++)
                    n += snprintf(line + n, sizeof line - n, "; %s %llu MiB (%llu+%llu+%llu in %llu+%llu+%llu)", rows[r],
                                  (unsigned long long)((radv_ps5_account_bytes[r][0] + radv_ps5_account_bytes[r][1] +
                                                        radv_ps5_account_bytes[r][2]) >> 20),
                                  (unsigned long long)(radv_ps5_account_bytes[r][0] >> 20),
                                  (unsigned long long)(radv_ps5_account_bytes[r][1] >> 20),
                                  (unsigned long long)(radv_ps5_account_bytes[r][2] >> 20),
                                  (unsigned long long)radv_ps5_account_count[r][0], (unsigned long long)radv_ps5_account_count[r][1],
                                  (unsigned long long)radv_ps5_account_count[r][2]);
                fprintf(stderr, "[ps5] driver buffers by purpose (under 1 MiB + under 16 + larger)%s\n", line);
            }
        }
#ifdef PS5_HEAP_TRIM   /* (a platform heap that counts what its arenas hold in blocks handed out: PS5_PayloadSDK 55a2de0) */
        fprintf(stderr, "[ps5] heap: %llu MiB mapped in %u segments, %llu MiB of it in blocks in use, %u arenas\n",
                (unsigned long long)(heap.mapped_bytes >> 20), heap.segments, (unsigned long long)(heap.in_use_bytes >> 20), heap.arenas);
        {   /* (HOS_PS5_HEAP_PROFILE=1, every sixth report: what the heap holds, by block size and by who asked for the big
               ones; the platform heap's own counts, PS5_PayloadSDK ps5_heap_profile. The callers' addresses are the title's:
               runtime address minus 0x400000 in work/ps5/link/llvm-pie.elf) */
            extern int ps5_heap_profile(char (*lines)[192], int max) __attribute__((weak));
            extern void ps5_heap_profile_enable(int on) __attribute__((weak));
            static int profile_n, sampling;
            /* (the heap samples one block in 16 of 4 to 128 KiB, with its callers, only once asked: the sampling's lock cost 5-10 fps) */
            if (!sampling && ps5_heap_profile_enable && getenv("HOS_PS5_HEAP_PROFILE")) ps5_heap_profile_enable(sampling = 1);
            if (ps5_heap_profile && getenv("HOS_PS5_HEAP_PROFILE") && profile_n++ % 6 == 0) {
                static char lines[64][192];
                const int n = ps5_heap_profile(lines, 64);
                for (int i = 0; i < n; i++) fprintf(stderr, "[ps5] %s\n", lines[i]);
            }
        }
#endif
        {   /* (the title's other budget: anonymous mappings and libc's own heap come from it, not from direct memory) */
            extern int32_t sceKernelAvailableFlexibleMemorySize(size_t *available);
            extern int32_t sceKernelConfiguredFlexibleMemorySize(size_t *configured);
            size_t flex_free = 0, flex_all = 0;
            const int a = sceKernelAvailableFlexibleMemorySize(&flex_free), b = sceKernelConfiguredFlexibleMemorySize(&flex_all);
            fprintf(stderr, "[ps5] flexible memory: %llu MiB free of %llu configured (results %#x, %#x)\n",
                    (unsigned long long)(flex_free >> 20), (unsigned long long)(flex_all >> 20), a, b);
        }
        fprintf(stderr, "[ps5] cpu: %.0f%% of one core (user %.0f%%, system %.0f%%) over %.1f s; guest pages %llu MiB; heap %llu MiB "
                "(peak %llu); stacks %lld MiB in %lld; direct memory %lld MiB, largest free piece %llu MiB\n",
                100 * (user + sys) / (t - t0), 100 * user / (t - t0), 100 * sys / (t - t0), (t - t0) / 1e3,
                (unsigned long long)(committed_bytes >> 20), (unsigned long long)(heap.mapped_bytes >> 20),
                (unsigned long long)(heap.peak_bytes >> 20), (long long)(stack_bytes >> 20), (long long)stack_count,
                (long long)(sceKernelGetDirectMemorySize() >> 20), (unsigned long long)(largest >> 20));
        before = now, t0 = t;
    }
    return NULL;
}
void hos_ps5_cpu_meter(void) {
    const char *e = getenv("HOS_PS5_CPU");
    if (!e || *e != '1') return;
    pthread_t t;
    if (pthread_create(&t, NULL, cpu_meter, NULL) == 0) pthread_detach(t);
}

/* The console's largest free direct-memory piece now (the renderer's budget: gpu/vk/vk_vma.cpp ps5_render_budget) */
uint64_t hos_ps5_direct_free(void) {
    int64_t at = 0;
    size_t largest = 0;
    if (sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &at, &largest) != 0) return 0;
    return largest;
}
