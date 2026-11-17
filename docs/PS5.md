# Longshot on the PS5

Longshot_ps5 builds [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (the static recompilation of *The Legend
of Zelda: Ocarina of Time* for Nintendo Switch 2) as a native PS5 title, **PPSA99025**. Longshot is the `upstream`
remote and the `main` branch here is upstream's, untouched; the console's work is on the `ps5` branch.

> [!IMPORTANT]
> No game files, keys or anything made from the game are in this repository. The title is built on a PC from your own
> copy of the game, and what that build makes from the game (`image.bin`, the lifted C, the executable) stays on your PC
> and your console.

## Where the PS5 layer comes from

The platform layer in `host/ps5/` and the build in `ps5/` were written by Mihawk for
[PS5_Hookshot](https://github.com/mihawk-99/PS5_Hookshot) (GPL-3.0-or-later, as Longshot is), which ported Hookshot,
Longshot's predecessor, and were then worked on in that repository by YeiStark. They are carried here onto
Longshot's code. The driver and the SDK are Mihawk's too and are not part of this repository:
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (Mesa's RADV for the console) and a fork of
[PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK).

## The rule: the console's code stays in its own files

Upstream moves by hundreds of commits a week, and every line changed in a file of upstream's is a conflict waiting at
the next update. So:

- What is the console's goes in `host/ps5/`, `ps5/` and this file.
- A file of upstream's gets at most a call into `host/ps5/` behind `#ifdef __PROSPERO__`, never a block of logic.
- What the console's C++ library lacks is supplied by `host/ps5/cxx_compat.h`, which `ps5/build.py` includes ahead of
  every unit of `gpu/vk`, so the renderer's sources stay as upstream writes them.
- A fix that is not the console's alone goes upstream, not here.

`git diff upstream/main --stat -- . ':!host/ps5' ':!ps5' ':!docs/PS5.md'` shows what is still changed in upstream's
files. `host/posix/win32_posix.c` holds most of it (the console's threads, files and signals inside the POSIX layer,
as PS5_Hookshot had them); moving that into `host/ps5/` is open work.

## The route

| Step | What | Where |
| --- | --- | --- |
| Lift | The game's ARM64 code to C, from your `.nup`, as upstream does it | `nx2/nup.py --only exefs`, `nx2/lift.py --standalone --write` |
| Compile | The lifted game, `host/` and `gpu/vk` for `x86_64-sie-ps5` (Zen 2) | `ps5/build.py` |
| Link | One executable with RADV linked in, signed as the console's fake SELF | `ps5/tools/link.sh` |
| Install | `dist/PPSA99025` to `/data/homebrew/PPSA99025`; `romfs/` is read in place from your `.nup` on the console (`HOS_PS5_NUP`, else `/data/lichii/roms/oot/oot.nup`, else `/data/prosperoeden/roms/oot/oot.nup`) | FTP, `host/ps5/nup_romfs.c` |

With PS5_Vulkan and the SDK built beside this repository (`PS5_VULKAN`, `PS5_PAYLOAD_SDK`), and FFmpeg, zstd and the
Vulkan, SPIR-V and VMA headers in `.local/ps5-deps` (`ps5/tools/build-ffmpeg.sh`, `ps5/tools/build-zstd.sh`):

```sh
python3 -I nx2/nup.py <your game.nup> --only exefs --extract work/game
mkdir -p work/lift/image
OOT_WORK=$PWD/work/lift python3 -I nx2/lift.py --standalone --write --exefs $PWD/work/game/exefs --work $PWD/work/lift
python3 -I ps5/build.py
```

The Python packages are upstream's (`lz4 numpy capstone "unicorn>=2.1,<2.2" zstandard`).

## What is the PS5's

Upstream's Linux build is the base: `host/` with `host/posix`'s Win32 calls, and `gpu/vk`.

| Linux build | PS5 | File |
| --- | --- | --- |
| SDL (window, controllers, sound) | VK_KHR_display, the DualSense through scePad, sceAudioOut; no microphone | `host/ps5/hos_sdl_ps5.c`, `host/ps5/platform.c` |
| The guest window at 16 TiB, pages made on first touch | at 64 GiB, 64 GiB of it, reserved and committed in 64 KiB units of direct memory | `host/ps5/win32_ps5.c`, `host/hos.h`, `host/hos_main.c` |
| `longshot_vk.so` and `libvulkan.so.1` loaded at run time | linked in; `dlopen`/`dlsym` answer from a table | `host/ps5/win32_ps5.c` |
| Thread and fiber stacks from mmap | reserved in full, grown on fault, freed once the thread has ended | `host/ps5/win32_ps5.c` |
| futex, gettid, /proc, /sys | `_umtx_op`, `pthread_getthreadid_np`, the console's known layout | `host/posix/win32_posix.c` |
| The environment and the defaults | what the console plays with unless `longshot_env.txt` says otherwise; a test run's `longshot_test_env.txt` | `host/ps5/ps5_startup.c`, `host/hos_main.c` |
| What the game is told of memory | the console's free memory less a reserve (`HOS_PS5_MEMINFO_RESERVE_MB`) | `host/hos_mem.c` |
| The renderer's memory budget | its own blocks against the console's one pool (`HOS_PS5_RENDER_MB`, `HOS_PS5_RESERVE_MB`) | `gpu/vk/vk_vma.cpp` |
| Storage buffers bound from the driver's alignment | from multiples of 256 (`RX_VK_SSBO_ALIGN`) | `gpu/vk/vk_main.cpp` |
| A pool lookup by the import's span | by the game's own pool, since imports are rounded to 16 KB | `gpu/vk/vk_memory.cpp` |

**Why the window moved.** The console grants no address space at or above 1 TiB, RADV keeps [8, 12) GiB for its 32-bit
window and [256, 512) GiB for device memory, and the platform's heap reserves 16 GiB at 128 GiB. At 64 GiB with 64 GiB
of guest space (the guest's heap from guest 16 GiB), all of it lands in host [64, 128) GiB.

**Guest pages carry the GPU bits.** The renderer imports the game's memory pools straight from these pages
(`VK_EXT_external_memory_host`); RADV grants the GPU access with a protection change, and any later protection change
without the GPU bits would make the GPU fault on the pool. So guest pages are mapped with the GPU bits from the start.

## Preparing every shader once (`RX_VK_PREPARE_ONCE`)

In play the console translates each program and has the driver compile it when it is first drawn: stutter and slow
loads. With `RX_VK_PREPARE_ONCE=1` (the console's default, `host/ps5/ps5_startup.c`; `=0` in `longshot_env.txt`: off) and the game's shader list beside the eboot
(`longshot_shaders.bin`; PS5_Hookshot's `hookshot_shaders.bin` is the same file, renamed), a start whose shaders are not
prepared for this translator prepares all of them before the game and then closes; the next start goes straight to the
game. Longshot's start-up screen shows its progress on one bar, with the time left (`RX_VK_STARTUP_SCREEN=0`: none, and
the log says where it is every 10 s, `vk: prepare once:`).

Longshot's `--prepare-shaders` does the same work but cannot be used on the console: a console start has no command line,
and its last step keeps every noted pipeline alive, memory the console does not have. Here:

1. every program is translated into `longshot_programs.bin`, with its compute pipelines, whatever `RX_VK_NO_WARM` says
   (that is play's). The records go to disk 8 MB at a time (`host/ps5/ps5_program_hold.cpp`: one at a time into the
   title folder cost 157 ms each), and the workers get the heap's eight arenas (PS5_PayloadSDK `ps5_heap_arenas`);
2. the pipelines the notes predict are built for the driver's own disk cache;
3. every noted pipeline, and the mesh fallback programs' compute pipelines, are built for the driver's cache and let go.

What it does not do is take the stutter of a first draw away: play still reads each program from `longshot_programs.bin`
(6.5 GB once all are prepared) when it is first drawn, and that read is the stutter (79 slow submits after the preparation,
80 before, 2026-11-16); the sampler puts 90% of that read in the kernel's read call. Longshot's way of reading them
earlier, `RX_VK_LOAD=create` with `RX_VK_PRECOMPILE=1`, does fit once one step is left out: on a GPU without mesh shaders
upstream reads every mesh fallback program before the title at each start (over 20 minutes on the console), and the
console leaves those to the create calls' loads and their first draws (`gpu/vk/vk_draw.cpp` `precompile_at_create`;
`RX_VK_AT_CREATE_FALLBACKS=1`: as upstream). Its first start builds every noted pipeline into the caches once (24 minutes
after a preparation, with the bar). Over four minutes of zone changes with the sampler on: the stutter's program reads
went from 393 ms to 56-84 ms. With the two load workers upstream gives play, a zone change queued hundreds of loads and a
draw waited up to 3.1 s for its program, and the map flickered; `RX_VK_LOAD_WORKERS=4` and
`RX_VK_LOAD_WORKERS_LOADING=6` took both away (the longest frame 282 ms). All four are the console's defaults. Upstream's safety net writes
`longshot_load_start.txt` after sessions with long waits; `RX_VK_LOAD=create` set in the environment goes first.

Our pipeline cache is not written by it. `RX_VK_PREPARE_ONCE_NO_DRIVER=1`: step 1's translation alone;
`RX_VK_PREPARE_ONCE_THREADS=<n>`: step 3's workers (all the CPUs but two, six at most); `RX_VK_PREPARE_ONCE_EXIT=0`: the
game goes on in the same process. The code is `host/ps5/ps5_vk_prepare.inc`, included at the end of `gpu/vk/vk_draw.cpp`
because it uses that file's own parts; `gpu/vk/vk_main.cpp` `started_up` calls it.

## Not carried from PS5_Hookshot yet

PS5_Hookshot changed the renderer in many places that Longshot has since rewritten. Each of these is to be looked at
again against Longshot's code, and carried only if Longshot does not already answer it:

- the renderer's memory under pressure: small textures below a reserve (`HOS_PS5_TEX_RESERVE_MB`), eviction by age in
  milliseconds, images of their own while memory is short, upload scratch buffers kept, retries where an allocation
  finds no memory (`operator new`, uploads, the recording buffers' pool);
- a program's SPIR-V kept compressed and its lowered code let go (Longshot reads a cached program's code at its first use);
- the mesh fallback's buffer cap and trim (`RX_VK_MESH_FALLBACK_MB`, `RX_VK_MESH_FALLBACK_TRIM`);
- the driver-fault handling for a console whose memory is in pieces;
- the hang watch's and the skip report's diagnostics (Longshot has a GPU hang watch of its own);
- frame generation by PS5_MihawkGen;
- the one-command build (`build-title.sh`) and the Windows wrapper.

## Driver and SDK gaps (PS5_Vulkan, PS5_PayloadSDK)

- **`setvbuf` with a large buffer.** After `setvbuf(stream, NULL, _IOFBF, 9 MiB)` every thread started later had no
  thread-local data, and the game crashed 7 s into each start.
- `pthread_getaffinity_np` / `pthread_setaffinity_np` do nothing; `scePthreadGetaffinity` / `scePthreadSetaffinity` are used.
- The C++ library has no `std::atomic_ref` (`host/ps5/cxx_compat.h`).
- The native task and mesh stages hang the GPU; the compute fallback is the console's default (`RX_VK_FORCE_MESH_FALLBACK=1`).

## Known gaps

- Motion controls: the game sees a pad lying still (scePad's sensors are not read yet).
- Microphone (humming the ocarina): none.
