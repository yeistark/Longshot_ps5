// (PS5) operator new for the whole title: an allocation the console's one pool cannot back is tried again for 3 s with the
// heap's free segments given back (the renderer's emptied blocks and the game's freed pages come back meanwhile), and one
// that still fails says how large it was and what the largest free piece is. The pool is shared by the game's pages, the
// renderer and this heap, and runs in pieces: 16 MB went unfound with 392 MiB free (2026-11-15, Longshot's first sessions).
// Carried from PS5_Hookshot, where it lived in gpu/vk/vk_main.cpp.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

extern "C" void ps5_heap_trim(void) __attribute__((weak));   // (the SDK's heap: its free segments back to the console)
extern "C" uint64_t hos_ps5_direct_free(void);               // (host/ps5/win32_ps5.c: the largest free piece)
extern "C" int usleep(unsigned int);

static void* ps5_new(std::size_t n) {
    if (!n) n = 1;
    if (void* p = std::malloc(n)) return p;
    // (3 s by the clock, not a count of tries: the SDK's malloc waits 0.3 s itself when it finds nothing)
    const auto t0 = std::chrono::steady_clock::now();
    const auto until = t0 + std::chrono::seconds(3);
    for (int tries = 1; std::chrono::steady_clock::now() < until; tries++) {
        if (ps5_heap_trim) ps5_heap_trim();
        usleep(20000);
        if (void* p = std::malloc(n)) {
            static std::atomic<int> told{0};
            if (told++ < 16)
                std::fprintf(stderr, "[ps5] operator new: %zu bytes found after %d ms (try %d)\n", n,
                             (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(), tries);
            return p;
        }
    }
    static std::atomic<int> told{0};
    if (told++ < 32)
        std::fprintf(stderr, "[ps5] operator new: %zu bytes not found (largest free piece %llu MiB)\n", n,
                     (unsigned long long)(hos_ps5_direct_free() >> 20));
    throw std::bad_alloc();
}
void* operator new(std::size_t n) { return ps5_new(n); }
void* operator new[](std::size_t n) { return ps5_new(n); }
