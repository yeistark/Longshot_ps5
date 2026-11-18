"""A render target's image the GPU is done with goes to the target pool as it is evicted, not at the next present.

texture_frame evicts after it has handled the dead list. An image put in the dead list there was pooled at the next present,
after the next frame had been recorded: that frame's target of the same create info missed it and made a new image, and the
old one was destroyed unused 120 presents later (first seen in Hookshot-old's 7bba12ad). One the GPU may still read (a
timed-out write-back's source: tests/test_readback_timeout_source.py) still waits in the dead list until it is done.

Cut out of gpu/vk/vk_texture.cpp unmodified: struct Dead, destroy_later and texture_frame's loop over the dead list. They run
against stub Vulkan, VMA and pool calls; no GPU or game is involved.

python -I -B tests/test_target_image_reuse.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <set>
#include <unordered_map>
#include <vector>

#define VK_NULL_HANDLE nullptr
struct Obj { int id; };
typedef Obj* VkDevice;
typedef Obj* VkImage;
typedef Obj* VkImageView;
typedef Obj* VkDeviceMemory;
typedef Obj* VkBuffer;
typedef Obj* VmaAllocation;

static Obj the_device{1}, the_image{2}, the_alloc{3}, the_view{4};
struct { VkDevice device = &the_device; } dev;
static void vkDestroyImageView(VkDevice, VkImageView, const void*) {}
static void vkFreeMemory(VkDevice, VkDeviceMemory, const void*) {}

struct Texture {   // (the real one is larger; destroy_later reads these)
    VkImage image = &the_image;
    VmaAllocation allocation = &the_alloc;
    uint64_t bytes = 1 << 20, pad = 0, last_value = 0, pool_key = 0;
    std::vector<VkDeviceMemory> sparse_chunks;
    std::unordered_map<uint64_t, VkImageView> views;
    struct BlockAttachment { VkImage image; VmaAllocation allocation; int format; uint32_t level; };
    std::vector<BlockAttachment> block_attachments;
};
static uint64_t destroy_gen = 1, total_bytes = 0, cap = 256ull << 20;
static uint64_t pool_cap() { return cap; }
static std::set<VkImage> freed, pooled;
static void destroy_image(VkImage i, VmaAllocation) { if (i) freed.insert(i); }
static void destroy_buffer(VkBuffer, VmaAllocation) {}
struct { bool in_pass = false; } compact;

namespace {
/* DEAD */
std::deque<Dead> dead;
void pool_put(Dead& d) { pooled.insert(d.image); }
/* DESTROY_LATER */
void dead_list(uint64_t completed) {
/* DEAD_LOOP */
}
}

#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL: %s\n", what); return 1; } } while (0)

// (an eviction at the present whose GPU has finished submission `completed`, of a texture last used in `last_value`)
static Texture evict(bool target, uint64_t last_value, uint64_t completed) {
    dead.clear(), freed.clear(), pooled.clear();
    Texture t;
    if (target) t.pool_key = 0x1235;
    t.views[7] = &the_view;
    t.last_value = last_value;
    total_bytes = t.bytes + t.pad;
    destroy_later(t, completed);
    return t;
}

int main() {
    {   // (a render target the GPU is done with: the pool's at once, for the next frame's target of its shape)
        const Texture t = evict(true, 5, 9);
        CHECK(pooled.count(&the_image), "a render target's image the GPU is done with was not pooled as it was evicted");
        CHECK(dead.empty(), "it also waits in the dead list, for the next present");
        CHECK(t.views.empty(), "its views stayed with the texture, not with the image in the pool");
        CHECK(total_bytes == 0, "its bytes were not taken off the textures' total");
        CHECK(destroy_gen > 1, "what was resolved to it was not made stale (destroy_gen)");
        dead_list(9);
        CHECK(!freed.count(&the_image), "the pooled image was destroyed by the next present's dead list");
    }
    {   // (one the GPU may still read: in the dead list, pooled only once the GPU is past it)
        evict(true, 11, 9);
        CHECK(!pooled.count(&the_image) && !freed.count(&the_image), "an image the GPU may still read was pooled or destroyed");
        CHECK(dead.size() == 1 && dead.front().pool_key, "it is not in the dead list as a render target's image");
        dead_list(10);
        CHECK(!pooled.count(&the_image), "it was pooled before the GPU was past it");
        dead_list(11);
        CHECK(pooled.count(&the_image) && dead.empty(), "it was not pooled once the GPU was past it");
    }
    {   // (a texture not made for a target, and a target with the pool off: destroyed by the dead list, as before)
        evict(false, 5, 9);
        CHECK(!pooled.count(&the_image) && dead.size() == 1, "a sampled texture's image skipped the dead list");
        cap = 0;
        evict(true, 5, 9);
        cap = 256ull << 20;
        CHECK(!pooled.count(&the_image) && dead.size() == 1, "with the pool off, a target's image skipped the dead list");
        dead_list(9);
        CHECK(freed.count(&the_image), "it was not destroyed once the GPU was done with it");
    }
    std::printf("target image reuse: ok\n");
    return 0;
}
"""


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    source = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    frame = block(source, "void texture_frame(uint64_t completed)")
    if "destroy_later(*tex, completed);" not in frame:
        raise SystemExit("texture_frame's eviction no longer tells destroy_later what the GPU has finished")
    # (the PS5 branch: the console's margin in an #ifdef that opens a brace in each branch; the cut reads the other side)
    source = source.replace("#ifdef __PROSPERO__\n        if (it->value + hos_ps5_dead_margin() > completed) {   // (host/ps5/ps5_render_room.c: a margin past its last use)\n#else\n", "").replace("        if (it->value > completed) {\n#endif\n", "        if (it->value > completed) {\n", 1)
    frame = block(source, "void texture_frame(uint64_t completed)")
    fixture = (FIXTURE.replace("/* DEAD */", block(source, "struct Dead {") + ";")
               .replace("/* DESTROY_LATER */", block(source, "void destroy_later(Texture& tex, uint64_t completed)"))
               .replace("/* DEAD_LOOP */", block(frame, "    for (auto it = dead.begin(); it != dead.end();)")))
    with tempfile.TemporaryDirectory(prefix="longshot-target-reuse-") as temp:
        cpp, exe = Path(temp) / "fixture.cpp", Path(temp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                        "-Wno-unused-function", "-Wno-unused-variable", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
