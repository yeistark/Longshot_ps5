<h1 align="center">
  <br>
  <img src="assets/longshot.png" alt="Longshot" width="200">
  <br>
  <b>Longshot for PS5</b>
  <br>
</h1>

<h4 align="center">Longshot, the static recompilation of a supported console game, built as a native PS5 homebrew title.</h4>

<p align="center">
  <a href="#status">Status</a> |
  <a href="#what-you-need">What you need</a> |
  <a href="#build">Build</a> |
  <a href="#install-on-the-console">Install</a> |
  <a href="#first-start">First start</a> |
  <a href="#settings">Settings</a> |
  <a href="#known-issues">Known issues</a> |
  <a href="#updating">Updating</a> |
  <a href="#license-and-credits">License + Credits</a>
</p>

This repository builds [Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot) (the static recompilation of *The
Legend of Zelda: Ocarina of Time* for Nintendo Switch 2, for Windows and Linux) as a native title for a PS5 that runs
homebrew. The game's code is translated to C and compiled for the console's CPU, and its graphics are rebuilt on Vulkan
through Mesa's RADV driver ported to the PS5. No emulator runs on the console.

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy of the game. The build makes the title **on your PC, from your own copy**: the executable
> (`eboot.bin`), `image.bin`, the generated code, the shader list and the caches are all made from the game. **Never share
> them.** That is also why there is no ready-made download: everyone builds their own.

The PC version's own documentation (menus, controls, mods, how the recompilation works) is in
[upstream's README](https://git.magicbean.dev/MagicBeanDev/Longshot). The technical notes for the console are in
[docs/PS5.md](docs/PS5.md).

## Status

Playable. These numbers come from the latest build, on the one console it was developed and tested on:

> [!NOTE]
> **Tested on a launch PS5 only** (the original "fat" model, standard edition). It has not been tried on any other PS5
> yet. The slim and the later fat models have the same CPU, GPU and memory, so they should behave the same, but nobody
> has confirmed it. The **PS5 Pro** has a different GPU and memory layout: it may need other memory settings, or may not
> work at all. Your firmware must be one your homebrew enabler supports. If you try it on another model or firmware,
> please open an issue with the result and the first start's log.

| | |
| --- | --- |
| Frame rate | 37-50 fps in most places, 20-28 in the heaviest scenes, 60 in menus and light areas |
| Stutter | 4 frames over 100 ms in 20 minutes of play, once the shaders are prepared |
| AMD FSR 3.1 | works (as anti-aliasing at the game's own resolution), under 1 ms of GPU |
| AMD frame generation | works (2x), off by default: it costs memory and adds latency (see [Known issues](#known-issues)) |
| Saves | the game's own, in the title folder (`user/save`) |
| Language | any of the game's 16 (see [Settings](#settings)) |

The renderer replays the game's GPU commands on one CPU thread, ~34 ms a frame in busy scenes. That, not the GPU, is what
limits the frame rate today.

## What you need

**On the console**

- A PS5 (tested on a launch "fat" model only: see [Status](#status)) that runs homebrew: a homebrew enabler that loads ELF payloads and installs fake-signed titles from
  `/data/homebrew`. The title was tested with ShadowMount, which registers a folder copied there by itself.
- An FTP server on the console to copy the title over (the examples use port 2121).
- About 7.5 GB free in `/data` for the shader caches, plus ~1.2 GB for the title.
- Your game's `.nup` on the console. The title reads the game's files (romfs) from it in place, from
  `/data/lichii/roms/oot/oot.nup`, else `/data/prosperoeden/roms/oot/oot.nup`; `HOS_PS5_NUP=<path>` in the settings file
  names another place.

**On the PC that builds** (Linux, or WSL 2 on Windows)

- 16 GB of RAM or more (8 GB works with `--jobs 2`, slowly), ~60 GB free.
- `clang` 18, `python3` 3.11 or newer, `git`, `make`, `cmake`, `ninja`, `meson`, `curl`.
- The Python packages: `pip install lz4 numpy capstone "unicorn>=2.1,<2.2" zstandard`.
- Your own copy of the game as a `.nup`.

## Build

### The easy way: one command

**On Windows** (10 or 11, with WSL 2):

1. Download [`ps5/build-windows.bat`](ps5/build-windows.bat) and [`ps5/build-title.sh`](ps5/build-title.sh) into the
   same folder (on the GitHub page of each file: *Download raw file*).
2. If Ubuntu is not installed yet: open PowerShell as administrator, run `wsl --install -d Ubuntu-24.04`, restart
   Windows if it asks, then open "Ubuntu 24.04" from the Start menu once and choose a user name and a password.
3. Drag your game's `.nup` file onto `build-windows.bat`. It asks for your PS5's IP address: type it to have the title
   sent to the console when it is built (its homebrew environment and FTP server must be running), or press Enter to
   copy it yourself.
4. Wait. The first build takes one to two hours; it asks once for the Ubuntu password (to install the packages it
   needs). When it ends, the `PPSA99025` folder is beside the `.bat`, and on the console if you gave its address.

**On Linux** (Ubuntu 24.04 or similar):

```sh
sudo bash build-title.sh --install-packages     # once: the packages it needs (apt)
bash build-title.sh <your game.nup> --ps5 <your PS5's address>
```

Without `--ps5` it only builds; the title is in `PS5_Homebrews/Longshot_ps5/dist/PPSA99025`. Run either one again
after an interruption or to update: what is done is not done again, and an update rebuilds only what changed.

The rest of this section is what that script does, step by step, for those who want to build by hand.

### By hand

Beside this repository the build needs the PS5 Vulkan driver, the payload SDK with this port's heap changes, a few
headers, and FFmpeg and zstd built for the console. All of them are public.

```sh
mkdir ~/PS5 && cd ~/PS5
git clone https://github.com/yeistark/Longshot_ps5.git
git clone https://github.com/mihawk-99/PS5_Vulkan.git       # Mesa's RADV for the PS5, and the link recipe
git clone https://github.com/mihawk-99/PS5_PayloadSDK.git
```

#### 1. The payload SDK, with the heap patch

The port needs two heap features the SDK fork does not have yet: more arenas while the shaders are prepared, and a large
first segment so that the console's fragmented memory cannot starve the heap. They are in
[`ps5/sdk/ps5-payload-sdk-heap.patch`](ps5/sdk/ps5-payload-sdk-heap.patch), made against the fork's revision `55a2de0`:

```sh
cd ~/PS5
mkdir sdk-src && git -C PS5_PayloadSDK archive 55a2de0 | tar -x -C sdk-src
patch -d sdk-src -p1 < Longshot_ps5/ps5/sdk/ps5-payload-sdk-heap.patch
bash sdk-src/platform/tools/setup-sdk.sh "$PWD/Longshot_ps5/.local/ps5-deps/ps5-payload-sdk" 55a2de0 "$PWD/downloads"
```

#### 2. The driver

```sh
cd ~/PS5/PS5_Vulkan
git checkout 7f492b4                 # the revision this port was tested with
tools/setup-native-dependencies.sh
tools/build-radv.sh release          # .deps/native/radv-release: the build titles link
```

#### 3. Headers, FFmpeg and zstd

```sh
cd ~/PS5/Longshot_ps5
cpm=.local/ps5-deps/cpm
git clone -q --depth 1 -b v1.4.345 https://github.com/KhronosGroup/Vulkan-Headers.git $cpm/vulkanheaders/v1.4.345
git clone -q --depth 1 -b vulkan-sdk-1.4.341.0 https://github.com/KhronosGroup/SPIRV-Headers.git $cpm/spirv-headers/vulkan-sdk-1.4.341.0
git clone -q --depth 1 -b v3.3.0 https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git $cpm/vulkanmemoryallocator/v3.3.0
export PS5_PAYLOAD_SDK=$PWD/.local/ps5-deps/ps5-payload-sdk
ps5/tools/build-ffmpeg.sh
ps5/tools/build-zstd.sh
```

#### 4. The game: lift, shader list, title

```sh
cd ~/PS5/Longshot_ps5
# the game's code, from your .nup
python3 -I nx2/nup.py <your game.nup> --only exefs --extract work/game
mkdir -p work/lift/image
OOT_WORK=$PWD/work/lift python3 -I nx2/lift.py --standalone --write --exefs $PWD/work/game/exefs --work $PWD/work/lift

# the list of the game's shaders, for the first start's preparation
# (only the archives with shaders are taken out of the .nup, then removed: 15 s, not the whole 25 GB romfs)
python3 -I ps5/tools/shader-list.py <your game.nup> --out work/longshot_shaders.bin

# compile and link the title into dist/PPSA99025
python3 -I ps5/build.py --jobs 4 --shortest-first
```

The first build compiles ~500 files and takes 30-60 minutes. Later builds compile only what changed. If a step above was
skipped, `ps5/build.py` says what is missing.

## Install on the console

Copy these to `/data/homebrew/PPSA99025` on the console:

- everything in `dist/PPSA99025` (`eboot.bin`, `image.bin`, `sce_sys/`, `sce_module/`);
- `work/longshot_shaders.bin`.

For example, with curl from the PC (put your console's address in place of `192.168.1.50`):

```sh
PS5=ftp://192.168.1.50:2121/data/homebrew/PPSA99025
cd dist/PPSA99025
for f in eboot.bin image.bin $(find sce_sys sce_module -type f); do curl -s --ftp-create-dirs -T "$f" "$PS5/$f"; done
curl -s -T ../../work/longshot_shaders.bin "$PS5/longshot_shaders.bin"
```

Then register the folder with your homebrew enabler (ShadowMount does it by itself) and start **Longshot** from the home
screen.

## First start

1. **The first start prepares every shader before the game.** It takes about **2.5 hours**, and a bar shows the progress
   and the time left. When it is done, **the title closes by itself**: that is expected. Leave the console on and do not
   close the title meanwhile.
2. **The second start builds the pipelines into the cache** (2-3 minutes, with the bar), then goes to the game.
3. From then on the title starts straight away.

The first time you visit an area, some objects can appear a moment late while their pipelines are made. The title notes
them and builds them before the game at the next preparation.

**Starting with someone else's pipeline lists (optional).** The lists of the pipelines a player has met
(`cache/longshot_pipeline_states.bin`, `cache/longshot_computes.bin`, `cache/longshot_compute_variants.bin`) hold only
hashes, formats and draw state, nothing of the game, so they can be shared. Copied into your title's `cache/` folder
before the first start, they make the preparation build those pipelines too, and areas show complete from the first
visit. `longshot_shaders.bin` is different: it holds the game's shader code, so make your own (see [Build](#build)) and
never share it.

## Settings

At each start the title reads `longshot_env.txt` from its folder (`/data/homebrew/PPSA99025/longshot_env.txt`), with
one `NAME=VALUE` per line and `#` for notes. Change it over FTP and start the title again. The defaults are the ones the
console plays best with. These are the settings worth knowing:

| Setting | What it does |
| --- | --- |
| `RX_VK_FSR=1` | AMD FSR 3.1 on (anti-aliasing). Also in the in-game menu |
| `RX_VK_FRAMEGEN=1` | AMD frame generation on (2x). `=0` keeps it off, whatever the menu says |
| `HOS_LANGUAGE=es-419` | the game's language: `en-US`, `en-GB`, `fr`, `fr-CA`, `de`, `it`, `es`, `es-419`, `pt-BR`, `nl`, `pl`, `ru`, `ja`, `ko`, `zh-Hans`, `zh-Hant` |
| `HOS_PS5_LOW_MB=1000` | below this much free memory the renderer lets its oldest textures go (1000 is the default) |
| `HOS_PS5_NUP=/data/.../oot.nup` | where your game's `.nup` is, if not in one of the default places |
| `RX_VK_PREPARE_ONCE=0` | skips the first start's preparation (not recommended: heavy stutter) |

A starting `longshot_env.txt`:

```
RX_VK_FSR=1
HOS_LANGUAGE=es-419
```

The in-game menu saves its own settings in `longshot_options.ini`, and the language can also be set in `language.txt`.
Both are in the title folder. The title writes those two files itself, and some FTP servers cannot overwrite a file the
title wrote. If a change does not stick, use `longshot_env.txt` instead, which wins over both.

If your homebrew enabler has a klog server (port 3232), the console's log says what the title is doing:

```sh
nc <console address> 3232 > klog.txt
```

## Known issues

- **Out of memory in the heaviest areas.** The PS5 gives the title one pool of 12 GB, shared by the game's memory
  (~7 GB), the GPU's buffers and textures (2.6-3.8 GB) and the program's heap (1.4 GB). After a while that memory is in
  hundreds of small pieces. In busy places (the castle town and the Citadel are the worst) the title can close when a
  request finds no piece large enough; the log then shows `bad_alloc` or `not found (largest free piece ...)`. Start the
  title again: the save is not affected.
- **Frame generation** works, but it needs ~500 MB more memory (so more closes in heavy areas). It doubles the frames
  shown but does not lower latency, and some players find it less smooth than FSR alone. It is off by default.
- **Frame rate**: 37-50 fps in most places, as low as 20 in the heaviest scenes, because the renderer's command replay
  is single-threaded. On the console this update of upstream replays ~20% slower than the one before it; that is being
  looked at.
- **First visits**: the first time an area is seen, an object can appear a moment late while its pipeline is made.
- **The first start takes ~2.5 hours** for the shader preparation, and closes the title when done. An update that changes
  the shader translator prepares them again.
- **Language**: the title starts in English until `HOS_LANGUAGE` or `language.txt` says otherwise.
- **No motion controls** (the pad's sensors are not read yet) and **no microphone** (humming the ocarina does nothing).
- **Native mesh shaders hang the GPU**, so the console draws them through the compute fallback (the default).
- Rarely, a start closes on the title screen (a null pointer in the game's code, seen once). Start it again.

When the title closes, the log's last lines (`CRASH:` and the lines before it) say why. Attach them to an issue.

## Updating

To update: pull this repository, build again (`python3 -I ps5/build.py`), and copy the new `eboot.bin` over the old one.
Keep the rest of the folder: the caches (`cache/`), the saves (`user/save`) and your settings stay valid. If the update
changed the shader translator, the next start prepares the shaders again, taking about as long as the first time.

**Trying a build without touching your installed title.** Build with
`LONGSHOT_PS5_SCE_SYS=ps5/sce_sys_next python3 -I ps5/build.py`. That makes a second title, **PPSA99026** ("Longshot
(nuevo)"), with its own icon and caches, installed beside the first in `/data/homebrew/PPSA99026`. To carry your progress
over, copy these into the new folder:

- `user/save`, `longshot_options.ini` and `longshot_shaders.bin`;
- from `cache/`: `longshot_pipeline_states.bin`, `longshot_computes.bin` and `longshot_compute_variants.bin`. These hold
  the pipelines you have met; without them, the new title shows objects late until it has met them again.

## For developers

- The console's code stays in its own files (`host/ps5/`, `ps5/`, `docs/PS5.md`). Upstream's files get at most a call
  behind `#ifdef __PROSPERO__`, which keeps upstream's updates mergeable. [docs/PS5.md](docs/PS5.md) has the rule, the
  route, and every console-specific decision with its measurements.
- `ps5_test_env/` holds the settings files used for the measurements in the docs. Copy one into the title folder as
  `longshot_test_env.txt`, and it is read instead of `longshot_env.txt`.
- `HOS_PS5_SAMPLE=<every ms>,<seconds>` writes a CPU sample of the title to `ps5_samples.txt`, which
  `ps5/tools/samples.py` reads.

## License and credits

Longshot for PS5 is free software under the [GNU General Public License, version 3 or later](LICENSE), as Longshot is.

    Hookshot: Copyright (C) 2026 NotRexed and the Hookshot contributors
    Longshot: Copyright (C) 2026 the Longshot contributors

It includes code from Eden and suyu, which are under the GPL too. Third-party parts under other licenses that work with
the GPL keep their own notices in their folders.

This is an unofficial fan project, not affiliated with or endorsed by Nintendo or Sony. The Legend of Zelda and Ocarina
of Time are trademarks of Nintendo. PlayStation and PS5 are trademarks of Sony Interactive Entertainment.

- **[Longshot](https://git.magicbean.dev/MagicBeanDev/Longshot)** and its contributors, and NotRexed's
  **[Hookshot](https://github.com/NotRexed/Hookshot)** before it: the recompilation, the renderer and everything the
  game runs on. Their credits list everyone whose work is in it (Eden, suyu, EvilOOT, Lichii and more).
- **Mihawk**: the PS5 platform layer and build ([PS5_Hookshot](https://github.com/mihawk-99/PS5_Hookshot)), the
  driver ([PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), Mesa's RADV for the console) and the
  [payload SDK fork](https://github.com/mihawk-99/PS5_PayloadSDK).
- **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)**: the payload SDK the fork is based on.
- **AMD FSR 3.1**: AMD FidelityFX Super Resolution and frame generation, built from AMD's MIT-licensed FidelityFX SDK
  shaders. AMD and FidelityFX are trademarks of Advanced Micro Devices, Inc.
