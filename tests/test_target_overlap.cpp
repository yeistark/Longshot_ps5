// (vk_texture.cpp's drawn_over_shifted, cut out of texture_frame by test_target_overlap.py into drawn_over_shifted.inc, run over
// stub textures: whether a render target is a dead transient one, its storage drawn over by newer targets from other starts)
#include "../gpu/vk/target_overlap.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); failed = 1; } } while (0)

namespace mkwvk {

struct Texture {   // (the real one is larger; drawn_over_shifted reads these)
    uint64_t gpu = 0, guest_size = 0, last_frame = 0, last_written = 0, first_render_frame = 1;
    bool gpu_written = true;
    uint32_t depth = 1, levels = 1, layers = 1;
};

struct Span { uint64_t lo, hi, written; };

/// Whether the target [lo, hi), last used at frame 100, is let go as drawn over, with the newer targets `newer` beside it
static bool dead(uint64_t lo, uint64_t hi, const std::vector<Span>& newer) {
    std::vector<Texture> all(1);
    all[0].gpu = lo, all[0].guest_size = hi - lo, all[0].last_frame = all[0].last_written = 100;
    for (const Span& s : newer) {
        Texture t;
        t.gpu = s.lo, t.guest_size = s.hi - s.lo, t.last_frame = t.last_written = s.written;
        all.push_back(t);
    }
    std::vector<const Texture*> by_start;   // (as texture_frame makes it)
    for (const Texture& t : all) by_start.push_back(&t);
    std::sort(by_start.begin(), by_start.end(), [](const Texture* a, const Texture* b) { return a->gpu < b->gpu; });
    uint64_t longest = 0;
    for (const Texture* t : by_start) longest = std::max(longest, t->guest_size);
    const bool dead_targets = true;   // (the PS5 branch's RX_VK_DEAD_TARGETS, on as upstream's default)
#include "drawn_over_shifted.inc"
    return drawn_over_shifted(&all[0]);
}

} // namespace mkwvk

int main() {
    using mkwvk::dead;
    int failed = 0;
    // (OoT's 768 x 432 RGBA16F transient targets: 2.6 MB each, made ~1.2 MB apart in a scratch heap)
    const uint64_t size = 768ull * 432 * 8, step = 0x128200;
    const uint64_t a = 0x10005db1600, b = a + step;

    CHECK("a newer target from a shifted start kills it", dead(a, a + size, {{b, b + size, 101}}));
    CHECK("and one that starts before it", dead(b, b + size, {{a, a + size, 101}}));
    CHECK("not when it was written before the last use", !dead(a, a + size, {{b, b + size, 100}}));
    CHECK("not when it was written before the last use (older)", !dead(a, a + size, {{b, b + size, 40}}));
    CHECK("the same start is drawn_over's case", !dead(a, a + size, {{a, a + size / 2, 101}}));
    CHECK("a view inside it is not a new target", !dead(a, a + size, {{a + 4096, a + size - 4096, 101}}));
    CHECK("one covering it whole is", dead(a + 4096, a + size, {{a, a + 2 * size, 101}}));
    CHECK("an overlap of an eighth is enough", dead(a, a + 8 * 4096, {{a + 7 * 4096, a + 20 * 4096, 101}}));
    CHECK("less than an eighth is not", !dead(a, a + 8 * 4096, {{a + 7 * 4096 + 1, a + 20 * 4096, 101}}));
    CHECK("apart is not", !dead(a, a + size, {{a + size, a + 2 * size, 101}}));
    CHECK("an empty one is not", !dead(a, a + size, {{b, b, 101}}));

    // (several newer targets: their parts add up)
    const uint64_t e = a + size, s16 = size / 16;
    CHECK("a sixteenth over its head alone is not", !dead(a, e, {{a - s16, a + s16, 101}}));
    CHECK("a sixteenth over its head and one over its tail, together an eighth, are",
          dead(a, e, {{a - s16, a + s16, 101}, {e - s16, e + s16, 101}}));
    CHECK("one written before its last use adds nothing", !dead(a, e, {{a - s16, a + s16, 101}, {e - s16, e + s16, 99}}));
    CHECK("a view inside it adds nothing", !dead(a, e, {{a - s16, a + s16, 101}, {a + 4096, a + size / 2, 101}}));
    CHECK("and the edges still add up beside one",
          dead(a, e, {{a - s16, a + s16, 101}, {a + 4096, a + size / 2, 101}, {e - s16, e + s16, 101}}));
    // (two over its head that overlap: 5 KB of 64 KB covered, not 9)
    CHECK("parts that overlap count once", !dead(a, a + 64 * 1024, {{a - 5 * 1024, a + 5 * 1024, 101}, {a - 2 * 1024, a + 4 * 1024, 101}}));
    if (failed) return 1;
    std::puts("target overlap: all checks passed");
}
