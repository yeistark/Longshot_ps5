// (PS5) Shader modules kept compressed, and made for the driver only while a pipeline is created.
//
// The driver keeps every shader module's SPIR-V in the heap for as long as the module lives, and the renderer keeps a
// module for every stage of every program it has met: ~600 MiB of the title's heap within minutes (2026-11-15, 400 of them
// read from the program cache at the start), in a pool of 12016 MiB the game's pages and the renderer's blocks share.
// PS5_Hookshot answered it inside the renderer (Stage::spirv_z, ModuleLease). Here the renderer is left as upstream writes
// it: the console's layer is what hands it the driver's functions (host/ps5/win32_ps5.c hos_ps5_dlsym), so the five that
// make, destroy and use a module are answered here.
//
//   vkCreateShaderModule            the SPIR-V compressed with zstd (about a fifth), the handle a record of ours
//   vkDestroyShaderModule           the record freed
//   vkCreateGraphicsPipelines,      each stage's module made for the driver from its record, the pipelines created, the
//   vkCreateComputePipelines        modules destroyed
//   vkGetShaderModuleIdentifierEXT  the same for the one call
//
// A module that cannot be made, for lack of memory, is tried again for half a second; then the pipeline is not made, which
// the renderer takes as a pipeline the driver could not build. HOS_PS5_EAGER_MODULES=1: modules at once, as on a PC.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_set>
#include <vector>

#include <vulkan/vulkan.h>
#include "zstd.h"

extern "C" void* vk_icdGetInstanceProcAddr(void* instance, const char* name);   // (RADV, linked in)
extern "C" int usleep(unsigned int);

namespace {

struct Kept {
    std::vector<uint8_t> z;     // (the SPIR-V, compressed; as it came when compression did not make it smaller)
    uint32_t bytes = 0;         // (its size as SPIR-V)
    bool compressed = false;
    VkShaderModuleCreateFlags flags = 0;
};

std::mutex mu;
std::unordered_set<const Kept*> kept;   // (the handles that are ours: any other is the driver's own)
std::atomic<uint64_t> n_kept{0}, raw_bytes{0}, held_bytes{0}, n_made{0}, n_failed{0};

PFN_vkGetDeviceProcAddr real_device_proc = nullptr;
PFN_vkCreateShaderModule real_create = nullptr;
PFN_vkDestroyShaderModule real_destroy = nullptr;
PFN_vkCreateGraphicsPipelines real_graphics = nullptr;
PFN_vkCreateComputePipelines real_compute = nullptr;
PFN_vkGetShaderModuleIdentifierEXT real_identifier = nullptr;

bool lazy() {
    static const bool v = !getenv("HOS_PS5_EAGER_MODULES");
    return v;
}

const Kept* ours(VkShaderModule m) {
    if (!m) return nullptr;
    const Kept* k = reinterpret_cast<const Kept*>(m);
    std::lock_guard lk(mu);
    return kept.count(k) ? k : nullptr;
}

/// The driver's module of a record, for one use. Null when it could not be made
VkShaderModule make(VkDevice device, const Kept& k) {
    std::vector<uint32_t> spirv((k.bytes + 3) / 4);
    if (k.compressed) {
        const size_t n = ZSTD_decompress(spirv.data(), spirv.size() * 4, k.z.data(), k.z.size());
        if (ZSTD_isError(n) || n != k.bytes) return VK_NULL_HANDLE;
    } else {
        std::memcpy(spirv.data(), k.z.data(), k.bytes);
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.flags = k.flags, ci.codeSize = k.bytes, ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    // (out of memory is for a moment, while the game gives pages back and the renderer its blocks: a pipeline left without
    // its module would be a program lost for the session)
    for (int tries = 0; tries < 25; tries++) {
        if (tries) usleep(20000);
        if (real_create(device, &ci, nullptr, &m) == VK_SUCCESS && m) {
            n_made++;
            return m;
        }
        m = VK_NULL_HANDLE;
    }
    if (n_failed++ < 16) std::fprintf(stderr, "[ps5] shader module of %u bytes not made for its pipeline\n", k.bytes);
    return VK_NULL_HANDLE;
}

VKAPI_ATTR VkResult VKAPI_CALL create_module(VkDevice device, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                             VkShaderModule* out) {
    // (with something chained to it, or an allocator of the caller's: the driver's own, as asked)
    if (!lazy() || !ci || ci->pNext || alloc || !ci->pCode || !ci->codeSize || ci->codeSize > UINT32_MAX)
        return real_create(device, ci, alloc, out);
    try {
        auto k = new Kept;
        k->bytes = (uint32_t)ci->codeSize, k->flags = ci->flags;
        std::vector<uint8_t> z(ZSTD_compressBound(ci->codeSize));
        const size_t n = ZSTD_compress(z.data(), z.size(), ci->pCode, ci->codeSize, 3);
        if (!ZSTD_isError(n) && n < ci->codeSize) {
            k->z.assign(z.begin(), z.begin() + (ptrdiff_t)n);
            k->compressed = true;
        } else {
            k->z.assign((const uint8_t*)ci->pCode, (const uint8_t*)ci->pCode + ci->codeSize);
        }
        {
            std::lock_guard lk(mu);
            kept.insert(k);
        }
        raw_bytes += k->bytes, held_bytes += k->z.size();
        const uint64_t n_now = ++n_kept;
        if (n_now == 1)
            std::fprintf(stderr, "[ps5] shader modules: kept compressed, made for the driver at a pipeline's creation "
                                 "(HOS_PS5_EAGER_MODULES=1: at once)\n");
        if (n_now % 2000 == 0)
            std::fprintf(stderr, "[ps5] shader modules: %llu kept, %llu MiB as %llu MiB; %llu made for pipelines, %llu not made\n",
                         (unsigned long long)n_now, (unsigned long long)(raw_bytes.load() >> 20),
                         (unsigned long long)(held_bytes.load() >> 20), (unsigned long long)n_made.load(),
                         (unsigned long long)n_failed.load());
        *out = reinterpret_cast<VkShaderModule>(k);
        return VK_SUCCESS;
    } catch (const std::bad_alloc&) {
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VKAPI_ATTR void VKAPI_CALL destroy_module(VkDevice device, VkShaderModule m, const VkAllocationCallbacks* alloc) {
    if (const Kept* k = ours(m)) {
        {
            std::lock_guard lk(mu);
            kept.erase(k);
        }
        raw_bytes -= k->bytes, held_bytes -= k->z.size();
        delete k;
        return;
    }
    real_destroy(device, m, alloc);
}

/// The driver's modules made for a create call, destroyed when it is over
struct Made {
    VkDevice device;
    std::vector<VkShaderModule> list;
    bool failed = false;
    VkShaderModule of(VkShaderModule asked) {
        const Kept* k = ours(asked);
        if (!k) return asked;
        const VkShaderModule m = make(device, *k);
        if (m) list.push_back(m);
        else failed = true;
        return m;
    }
    ~Made() {
        for (const VkShaderModule m : list) real_destroy(device, m, nullptr);
    }
};

VKAPI_ATTR VkResult VKAPI_CALL create_graphics(VkDevice device, VkPipelineCache cache, uint32_t count,
                                               const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc,
                                               VkPipeline* out) {
    try {
        Made made{device};
        std::vector<VkGraphicsPipelineCreateInfo> ci(infos, infos + count);
        std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(count);
        for (uint32_t i = 0; i < count; i++) {
            if (!ci[i].pStages || !ci[i].stageCount) continue;
            stages[i].assign(ci[i].pStages, ci[i].pStages + ci[i].stageCount);
            for (auto& st : stages[i]) st.module = made.of(st.module);
            ci[i].pStages = stages[i].data();
        }
        if (!made.failed) return real_graphics(device, cache, count, ci.data(), alloc, out);
    } catch (const std::bad_alloc&) {
    }
    for (uint32_t i = 0; i < count; i++) out[i] = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR VkResult VKAPI_CALL create_compute(VkDevice device, VkPipelineCache cache, uint32_t count,
                                              const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* alloc,
                                              VkPipeline* out) {
    try {
        Made made{device};
        std::vector<VkComputePipelineCreateInfo> ci(infos, infos + count);
        for (auto& c : ci) c.stage.module = made.of(c.stage.module);
        if (!made.failed) return real_compute(device, cache, count, ci.data(), alloc, out);
    } catch (const std::bad_alloc&) {
    }
    for (uint32_t i = 0; i < count; i++) out[i] = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL module_identifier(VkDevice device, VkShaderModule m, VkShaderModuleIdentifierEXT* id) {
    try {
        Made made{device};
        const VkShaderModule real = made.of(m);
        if (real) return real_identifier(device, real, id);
    } catch (const std::bad_alloc&) {
    }
    if (id) id->identifierSize = 0;
}

/// Ours for name, with the driver's kept to call; null when the driver has none, or the name is not one of the five
void* answer(const char* name, void* real) {
    if (!real) return nullptr;
#define OURS(vk, slot, fn)                    \
    if (!std::strcmp(name, #vk)) {            \
        slot = (PFN_##vk)real;                \
        return (void*)fn;                     \
    }
    OURS(vkCreateShaderModule, real_create, create_module)
    OURS(vkDestroyShaderModule, real_destroy, destroy_module)
    OURS(vkCreateGraphicsPipelines, real_graphics, create_graphics)
    OURS(vkCreateComputePipelines, real_compute, create_compute)
    OURS(vkGetShaderModuleIdentifierEXT, real_identifier, module_identifier)
#undef OURS
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice device, const char* name) {
    const PFN_vkVoidFunction real = real_device_proc(device, name);
    if (!name || !real) return real;
    void* const mine = answer(name, (void*)real);
    return mine ? (PFN_vkVoidFunction)mine : real;
}

} // namespace

/// What the renderer loads as the driver's vkGetInstanceProcAddr (host/ps5/win32_ps5.c hos_ps5_dlsym)
extern "C" void* hos_ps5_vkGetInstanceProcAddr(void* instance, const char* name) {
    void* const real = vk_icdGetInstanceProcAddr(instance, name);
    if (!name || !real) return real;
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) {
        real_device_proc = (PFN_vkGetDeviceProcAddr)real;
        return (void*)device_proc;
    }
    void* const mine = answer(name, real);   // (a device's function asked of the instance, as Dear ImGui's loader does)
    return mine ? mine : real;
}
