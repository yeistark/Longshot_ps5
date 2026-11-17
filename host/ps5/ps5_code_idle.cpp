// (PS5) The lowered code of programs the replay has not drawn for a while, let go.
//
// gpu/vk/vk_shader.cpp keeps, for every program the game has and the replay has looked up, its stages read from the game's
// memory and lowered to Maxwell (codes, by the program's serial), until the game frees the program. The game keeps most of its
// programs for the whole session: ~140 MiB of the title's heap after 16 minutes (klog_longshot_40's heap profile:
// AppendNativeAddressMap), in the one pool the game's pages and the renderer's blocks share. The entry is only read when a
// draw's state needs a program variant not picked before, and is made again from the game's memory then (codes_of).
//
// vk_shader.cpp is part of the translator's stamp (a change there makes every player prepare the shaders again), so it is left
// as it is: an entry is let go the way the game's free call lets it go, through the replay's own release (vk_replay.cpp
// do_program_released: under the replay's lock and the translator's), from a thread of ours. The renderer calls
// hos_ps5_code_idle_touch with each draw's and dispatch's program (gpu/vk/vk_draw.cpp) and hos_ps5_code_idle_start once.
//
// HOS_PS5_CODE_IDLE_S=<s>: after how long without a draw (60); 0: never.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../gpu/render/rx_nvn.h"

extern "C" void hos_ps5_sample_register(void);   // (host/ps5/win32_ps5.c: the sampler sees this thread too)

namespace {
constexpr int kTickS = 10;
std::mutex mu;
std::unordered_map<uint64_t, uint32_t> last_tick;   // program serial -> the tick of its last draw
std::atomic<uint32_t> tick{1};
std::atomic<bool> on{false};

int idle_s() {
    static const int v = [] {
        const char* e = getenv("HOS_PS5_CODE_IDLE_S");
        return e ? atoi(e) : 60;
    }();
    return v;
}

void run(void (*release)(const nvn_prog*)) {
    hos_ps5_sample_register();
    const uint32_t idle_ticks = (uint32_t)((idle_s() + kTickS - 1) / kTickS);
    uint64_t total = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(kTickS));
        const uint32_t now = ++tick;
        std::vector<uint64_t> gone;
        size_t tracked = 0;
        {
            std::lock_guard lk(mu);
            for (auto it = last_tick.begin(); it != last_tick.end();)
                if (now - it->second > idle_ticks) gone.push_back(it->first), it = last_tick.erase(it);
                else ++it;
            tracked = last_tick.size();
        }
        for (const uint64_t serial : gone) {
            nvn_prog p;
            std::memset(&p, 0, sizeof p);
            p.serial = serial;
            release(&p);   // (vk_shader.cpp program_released: only the serial is read)
            std::this_thread::sleep_for(std::chrono::microseconds(200));   // (the replay's lock: never long in one go)
        }
        total += gone.size();
        if (!gone.empty())
            fprintf(stderr, "[ps5] program code: %zu let go (not drawn for %d s), %llu in all; %zu drawn lately\n", gone.size(),
                    idle_s(), (unsigned long long)total, tracked);
    }
}
}  // namespace

/// (vk_replay.cpp, the renderer's start) release: its do_program_released
extern "C" void hos_ps5_code_idle_start(void (*release)(const nvn_prog*)) {
    if (idle_s() <= 0 || on.exchange(true)) return;
    std::thread(run, release).detach();
}

/// (vk_draw.cpp, each draw's and dispatch's program, before its look-up)
extern "C" void hos_ps5_code_idle_touch(uint64_t serial) {
    if (!serial || !on.load(std::memory_order_relaxed)) return;
    // (each thread's own note of what it told this tick: the shared map's lock once a program a tick, not once a draw)
    static thread_local std::unordered_map<uint64_t, uint32_t> told;
    const uint32_t now = tick.load(std::memory_order_relaxed);
    uint32_t& at = told[serial];
    if (at == now) return;
    at = now;
    if (told.size() > 65536) told.clear();
    std::lock_guard lk(mu);
    last_tick[serial] = now;
}
