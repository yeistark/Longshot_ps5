/* (PS5) How far the console's free memory is below where the renderer must start letting textures go.

   The renderer's texture budget on the console (gpu/vk/vk_vma.cpp vram_usage) is what it holds plus the largest free piece
   above a reserve. Once that piece is under the reserve the budget is what it holds, and what it has allocated is under its
   blocks by the free space inside them (350-770 MB in klog_longshot_39): it is never over budget, and nothing is let go
   however short the console is. At a zone change the textures reached 2.15 GB and the driver's buffers 3.5 GB, the console
   had 4 MB, and the game's next operator new found nothing in 3 s (klog_longshot_41, klog_longshot_35).

   Below HOS_PS5_LOW_MB free in all (700), this answers the shortfall, and the renderer's budget falls that far under what
   it has allocated: the oldest textures go before the game runs out. 0: off. */
#include <stdint.h>
#include <stdlib.h>

uint64_t hos_ps5_direct_room(void);   /* (host/ps5/win32_ps5.c: the console's free direct memory, in all) */

uint64_t hos_ps5_render_short(void) {
    static int64_t low = -1;
    if (low < 0) {
        const char *e = getenv("HOS_PS5_LOW_MB");
        low = (int64_t)(e ? atoi(e) : 700) << 20;
    }
    if (!low) return 0;
    const uint64_t room = hos_ps5_direct_room();
    return room < (uint64_t)low ? (uint64_t)low - room : 0;
}
