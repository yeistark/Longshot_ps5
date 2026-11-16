/* (PS5) The runtime's allocations wait for memory. The console has one pool for the game's pages, the renderer and this
   heap, and it runs full: the heap's next 2 MiB segment then finds no piece that large although tens of MiB are free in
   hundreds of pieces, and malloc answers NULL for a few bytes (a mutex's 4 KiB in nn_os_sync.c, a 64 KiB recording
   buffer: the title gone in Hyrule Field) while the renderer, told the pool is short, gives textures back within
   presents and the game frees an area's pages as it loads the next. ps5/build.py names malloc, calloc and realloc
   hos_ps5_* for host/'s files; an allocation the heap cannot back is tried again for up to 3 s, its free segments
   given back first, before the caller is told no. */
#undef malloc
#undef calloc
#undef realloc
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern void ps5_heap_trim(void) __attribute__((weak));
/* the SDK's heap wraps wait the same way (platform/src/heap_wrap.c) when they have it: then this file's wait would only
   nest inside theirs */
extern const int ps5_malloc_waits __attribute__((weak));

static void *again(int kind, void *old, size_t n, size_t m) {
    static int told_found, told_lost;
    if (&ps5_malloc_waits && kind != 2) return NULL;
    for (int tries = 1; tries <= 150; tries++) {
        if (ps5_heap_trim) ps5_heap_trim();
        usleep(20000);
        void *p = kind == 0 ? malloc(n) : kind == 1 ? calloc(n, m) : realloc(old, n);
        if (p) {
            if (__atomic_fetch_add(&told_found, 1, __ATOMIC_RELAXED) < 32)
                fprintf(stderr, "[ps5] malloc: %zu bytes found after %d ms\n", kind == 1 ? n * m : n, tries * 20);
            return p;
        }
    }
    if (__atomic_fetch_add(&told_lost, 1, __ATOMIC_RELAXED) < 32)
        fprintf(stderr, "[ps5] malloc: %zu bytes not found in 3 s\n", kind == 1 ? n * m : n);
    return NULL;
}

void *hos_ps5_malloc(size_t n) {
    void *p = malloc(n);
    return p || !n ? p : again(0, NULL, n, 0);
}
void *hos_ps5_calloc(size_t n, size_t m) {
    void *p = calloc(n, m);
    return p || !n || !m ? p : again(1, NULL, n, m);
}
void *hos_ps5_realloc(void *old, size_t n) {
    void *p = realloc(old, n);
    return p || !n ? p : again(2, old, n, 0);
}
