// (PS5) RX_VK_PREPARE_ONCE=1: a start that finds the game's shader list (longshot_shaders.bin, beside the eboot) not prepared for
// this translator prepares all of it before the game, with nothing else running, and then closes; the next start goes straight
// to the game. Carried from PS5_Hookshot (b4437e65, 67df21c9, 34d44c5f).
//
// Longshot's own --prepare-shaders does the same work, but a console start has no command line, and its last step
// (precompile_run) keeps every noted pipeline alive and the programs read in: memory the console does not have (RX_VK_PRECOMPILE=1
// is off on the console for that). Here each pipeline is built for the driver's own disk cache and let go:
//   1. prepare_run: every program translated into longshot_programs.bin, the compute pipelines with them (whatever the console's
//      RX_VK_NO_WARM says: that is for play); records written 8 MB at a time (ps5_program_hold.cpp); the heap's eight arenas;
//   2. predict_run: the pipelines the notes predict, for the driver's cache;
//   3. every noted pipeline (longshot_pipelines.bin), and the mesh fallback programs' compute pipelines, built and let go.
// Our pipeline cache is not written by it (it is read whole at every start). RX_VK_PREPARE_ONCE_NO_DRIVER=1: step 1's
// translation alone. RX_VK_PREPARE_ONCE_EXIT=0: the game goes on in this process.
//
// Included at the end of gpu/vk/vk_draw.cpp, inside namespace mkwvk, under __PROSPERO__: it uses that file's own parts
// (prepare_check, build_graphics, pipe_notes_read...), which an upstream change may reshape. Called by vk_main.cpp started_up.

void ps5_program_hold(bool on);   // (host/ps5/ps5_program_hold.cpp)
// (PS5_PayloadSDK f3f76cc: the heap arenas new threads are spread over, one by default; an older SDK has none of it)
extern "C" void ps5_heap_arenas(unsigned count) __attribute__((weak));
extern "C" uint64_t hos_ps5_direct_room(void);   // (host/ps5/win32_ps5.c: the console's free direct memory)
extern "C" uint64_t hos_ps5_direct_free(void);   // (and its largest free piece)

namespace {
std::atomic<int> ps5_once_part{0};

void ps5_once_progress(size_t done, size_t total) {   // (without the start-up screen: in the log every 10 s)
    static std::atomic<uint32_t> next{0};
    const uint32_t now = (uint32_t)GetTickCount();
    uint32_t at = next.load();
    if (at && (int32_t)(now - at) < 0) return;
    if (!next.compare_exchange_strong(at, now + 10000)) return;
    static const char* const parts[] = {"programs", "predicted pipelines", "noted pipelines"};
    logf("vk: prepare once: %s %zu of %zu (the console has %llu MB free)", parts[ps5_once_part.load()], done, total,
         (unsigned long long)(hos_ps5_direct_room() >> 20));
}

bool ps5_once_wanted() {
    if (!env_on("RX_VK_PREPARE_ONCE")) return false;
    uint64_t size = 0;
    int64_t time = 0;
    if (!seed_file_id(seed_path(), &size, &time)) {
        logf("vk: prepare once: no shader list at %s: nothing to prepare from, the game starts", seed_path().c_str());
        return false;
    }
    PreparedFor now;
    double hash_ms = 0;
    return prepare_check(prepared_path(), seed_path(), program_cache_stamp(), &now, &hash_ms) != PrepareWhy::None;
}

/// Its workers for step 3: RX_VK_PREPARE_ONCE_THREADS=<n>, or all the CPUs but two (six at most: the console's eight less the
/// two the screen and the system keep)
unsigned ps5_once_workers() {
    if (const int n = env_int("RX_VK_PREPARE_ONCE_THREADS", 0); n > 0) return (unsigned)n;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hw > 2 ? hw - 2 : 1u, 1u, 6u);
}

/// Step 3: every noted pipeline built once for the driver's own disk cache and let go, a program at a time (borrowed: read for
/// it, then let go), with a mesh fallback program's compute pipelines. Nothing is kept
void ps5_once_pipelines(void (*progress)(size_t done, size_t total)) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::unique_ptr<PipeNote>> notes = pipe_notes_read();
    if (notes.empty()) {
        logf("vk: prepare once: no noted pipelines");
        return;
    }
    std::unordered_map<uint64_t, std::vector<const PipeNote*>> by_program;
    for (const auto& n : notes) by_program[n->prog].push_back(n.get());
    std::vector<uint64_t> keys;
    keys.reserve(by_program.size());
    for (const auto& kv : by_program) keys.push_back(kv.first);
    std::atomic<size_t> next{0}, done{0}, made{0}, fallbacks{0};
    const auto worker = [&] {
        ub_thread_off();   // (a background thread: see ub_thread_off)
        driver_cache_only = true;   // (build_graphics: no cache of ours handed to the driver)
        for (;;) {
            if (prepare_stopped()) return;
            const size_t i = next++;
            if (i >= keys.size()) return;
            const auto& list = by_program[keys[i]];
            program_cache_borrow(keys[i], [&](Program& p) {
                ensure_layout(p);
                bool drawn = false;
                for (const PipeNote* n : list)
                    if (n->variant == p.variant) {
                        drawn = true;
                        if (const VkPipeline pl = build_graphics(p, n->s, note_targets(*n), (VkPrimitiveTopology)n->topo))
                            vkDestroyPipeline(dev.device, pl, nullptr), made++;
                    }
                if (drawn && p.fallback && fallback_compute_pipelines(p)) {   // (a borrowed program: no one else holds it)
                    for (VkPipeline pl : p.fallback_compute)
                        if (pl) vkDestroyPipeline(dev.device, pl, nullptr);
                    p.fallback_compute.clear();
                    fallbacks++;
                }
            });
            done += list.size();
        }
    };
    std::vector<std::thread> threads;
    const unsigned workers = ps5_once_workers();
    for (unsigned i = 0; i < workers; i++) threads.emplace_back(worker);
    while (next.load() < keys.size() + threads.size() && !prepare_stopped()) {
        progress(done.load(), notes.size());
        Sleep(100);
    }
    for (std::thread& t : threads) t.join();
    logf("vk: prepare once: %zu of %zu noted pipelines made for the driver's cache, %zu mesh fallback programs' compute pipelines, "
         "%u workers, in %.1f s", made.load(), notes.size(), fallbacks.load(), workers,
         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

/// (before the process ends) the driver writes its disk cache on a thread of its own: waited for until the folder stops
/// growing for 3 s, a minute at most, so that what was built is not lost with the process
void ps5_once_cache_settle() {
    std::string where;
    uint64_t last = driver_cache_bytes(&where);
    int still = 0;
    for (int s = 0; s < 60 && still < 3; s++) {
        Sleep(1000);
        const uint64_t now = driver_cache_bytes(&where);
        still = now == last ? still + 1 : 0;
        last = now;
    }
    driver_cache_log("after the prepare once");
}
}  // namespace

namespace {
bool ps5_once_done = false;   // (hos_ps5_prepare_once_run's answer, for hos_ps5_prepare_once_end)
std::chrono::steady_clock::time_point ps5_once_t0;
std::string ps5_once_no_warm;
bool ps5_once_no_warm_set = false, ps5_once_no_save_was = false;
}  // namespace

/// (vk_main.cpp started_up, the device made) 1: this start prepares (the shader list is not prepared for this translator)
extern "C" int hos_ps5_prepare_once_wanted(void) { return ps5_once_wanted(); }

/// The three parts, on the thread that runs them: on the start-up screen's worker (vk_main.cpp prepare_with_screen, which
/// draws their progress on its bar) or on started_up's own. progress, stage: the screen's (null: every 10 s in the log)
extern "C" int hos_ps5_prepare_once_run(void (*progress)(size_t done, size_t total), void (*stage)(int part)) {
    ps5_once_t0 = std::chrono::steady_clock::now();
    if (!progress) progress = ps5_once_progress;
    const auto part = [&](int k) {
        ps5_once_part = k;
        if (stage) stage(k);
    };
    const bool driver = !env_on("RX_VK_PREPARE_ONCE_NO_DRIVER");
    logf("vk: prepare once: the shaders are not prepared for this translator: all of them now, before the game (the driver's "
         "part %s; the console has %llu MB free, the largest piece %llu MB)", driver ? "too" : "left out",
         (unsigned long long)(hos_ps5_direct_room() >> 20), (unsigned long long)(hos_ps5_direct_free() >> 20));
    // (the console's RX_VK_NO_WARM is for play: here the compute pipelines are what the driver's part is for)
    const char* no_warm_was = getenv("RX_VK_NO_WARM");
    ps5_once_no_warm_set = no_warm_was != nullptr;
    ps5_once_no_warm = no_warm_was ? no_warm_was : "";
    if (driver) unsetenv("RX_VK_NO_WARM");
    else setenv("RX_VK_NO_WARM", "1", 1);
    pcache_init();
    ps5_once_no_save_was = pcache_no_save;
    pcache_no_save = true;   // (our pipeline cache: built into, never written: it would be read whole at every start)
    if (ps5_heap_arenas) ps5_heap_arenas(8);   // (six workers on the title's one arena waited on its lock 97% of the time)
    ps5_program_hold(true);
    part(0);
    const bool prepared = prepare_run(progress, false, true);
    ps5_program_hold(false);
    if (prepared && driver && !prepare_stopped()) {
        part(1);
        predict_run(progress);
    }
    if (prepared && driver && !prepare_stopped()) {
        part(2);
        ps5_once_pipelines(progress);
    }
    if (ps5_heap_arenas) ps5_heap_arenas(1);   // (play keeps one: its pool is the GPU's and the guest's too)
    ps5_once_done = prepared && !prepare_stopped();
    if (ps5_once_done && driver) ps5_once_cache_settle();
    return ps5_once_done;
}

/// (after the run, the start-up screen gone) ends the process when it prepared; else the game goes on in this one, with play's
/// settings back. RX_VK_PREPARE_ONCE_EXIT=0: the game goes on in this one either way
extern "C" void hos_ps5_prepare_once_end(void) {
    logf("vk: prepare once: %s in %.0f s; the console has %llu MB free, the largest piece %llu MB",
         ps5_once_done ? "done" : "nothing prepared",
         std::chrono::duration<double>(std::chrono::steady_clock::now() - ps5_once_t0).count(),
         (unsigned long long)(hos_ps5_direct_room() >> 20), (unsigned long long)(hos_ps5_direct_free() >> 20));
    const char* e = getenv("RX_VK_PREPARE_ONCE_EXIT");
    if (ps5_once_done && (!e || atoi(e) != 0)) {
        logf("vk: prepare once: the shaders are prepared; closing, the next start goes straight to the game");
        fflush(nullptr);
        // (the game starts in a new process, the heap the compiles left in pieces given back. _exit, not exit: exit's teardown
        // took a SIGSYS on a thread of the driver's and the console showed its crash report, klog_longshot_29; the files are
        // written by now, the driver's cache waited for above)
        _exit(0);
    }
    if (ps5_once_no_warm_set) setenv("RX_VK_NO_WARM", ps5_once_no_warm.c_str(), 1);
    else unsetenv("RX_VK_NO_WARM");
    pcache_no_save = ps5_once_no_save_was;
}
