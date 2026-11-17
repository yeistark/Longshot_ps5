/* (PS5) How far the console's free memory is below where the renderer must start letting textures go.

   The renderer's texture budget on the console (gpu/vk/vk_vma.cpp vram_usage) is what it holds plus the largest free piece
   above a reserve. Once that piece is under the reserve the budget is what it holds, and what it has allocated is under its
   blocks by the free space inside them (350-770 MB in klog_longshot_39): it is never over budget, and nothing is let go
   however short the console is. At a zone change the textures reached 2.15 GB and the driver's buffers 3.5 GB, the console
   had 4 MB, and the game's next operator new found nothing in 3 s (klog_longshot_41, klog_longshot_35).

   Below HOS_PS5_LOW_MB free in all (1000), this answers the shortfall, and the renderer's budget falls that far under what
   it has allocated: the oldest textures go before the game runs out. 0: off. (1000 since 2026-11-17: the sessions with FSR
   and with frame interpolation ran on it without a close; 700 left frame interpolation 5 MB, 1600 evicted drawn textures) */
#include <stdint.h>
#include <stdlib.h>

uint64_t hos_ps5_direct_room(void);   /* (host/ps5/win32_ps5.c: the console's free direct memory, in all) */

uint64_t hos_ps5_render_short(void) {
    static int64_t low = -1;
    if (low < 0) {
        const char *e = getenv("HOS_PS5_LOW_MB");
        low = (int64_t)(e ? atoi(e) : 1000) << 20;
    }
    if (!low) return 0;
    const uint64_t room = hos_ps5_direct_room();
    return room < (uint64_t)low ? (uint64_t)low - room : 0;
}

/* (PS5) How many timeline values past a dead texture's last use its image and memory wait before they are freed
   (gpu/vk/vk_texture.cpp's sweep of the dead ones). Upstream frees at its last use; on the console a texture freed while a
   submission still read it hung the GPU (unmapped at once, and the GPU waits on the fault instead of reporting it): an
   upload after an hour at 338 MiB free, a draw within a minute with the textures' memory at 4 GB (2026-11-17). About six
   values a frame: 48 is some eight frames. HOS_PS5_DEAD_MARGIN=0: as upstream. */
uint64_t hos_ps5_dead_margin(void) {
    static int64_t margin = -1;
    if (margin < 0) {
        const char *e = getenv("HOS_PS5_DEAD_MARGIN");
        margin = e ? atoll(e) : 48;
        if (margin < 0) margin = 0;
    }
    return (uint64_t)margin;
}
