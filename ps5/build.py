"""PS5: the game, the runtime and the Vulkan back end compiled for the console and linked into one title, dist/<TITLE>.

    python3 -I ps5/build.py [--jobs N] [--compile-only] [--shortest-first]

What it builds from (docs/PS5.md has the whole route):
  - the lifted game: work/lift/sa/gen (nx2/lift.py --standalone --write, from your own dump; never committed);
  - host/ as the Linux build compiles it (host/posix's Win32 calls), with host/ps5 in place of SDL, and the PS5's
    memory, threads and modules (host/ps5/win32_ps5.c);
  - gpu/vk with Eden's shader recompiler, linked in rather than loaded (no dynamic loading on the console);
  - Mesa's RADV from PS5_Vulkan (its release archive and link recipe), the payload SDK fork's platform layer.

Paths: OOT_WORK (default work/lift), PS5_PAYLOAD_SDK (default .local/ps5-deps/ps5-payload-sdk, else ../PS5_VulkanTemplate's), PS5_VULKAN
(default ../PS5_Vulkan). Objects are kept by a key of their flags, source and headers: a rebuild compiles what changed.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
os.environ.setdefault("OOT_WORK", os.path.join(ROOT, "work", "lift"))
import kit       # noqa: E402  (nx2: the lift's paths)
import rtgen     # noqa: E402  (the 64-bit guest window's runtime header)
import logo_rgba  # noqa: E402
import ui_fonts   # noqa: E402  (the overlays' bundled fonts, compiled in)
import version   # noqa: E402
import translator_files   # noqa: E402  (what the shader caches are good for)

TITLE = "PPSA99025"   # (the title folder: dist/<TITLE>, /data/homebrew/<TITLE> on the console; ps5/sce_sys/param.json names it too)
# (the renderer's files that draw with Dear ImGui: its include folder and flags. vk_crash_window.cpp: the crash window, upstream #983)
IMGUI_UNITS = ("vk_imgui.cpp", "vk_menu.cpp", "vk_crash_window.cpp")

# (the SDK ps5/build-title.sh installs at its pinned revision; before it did, PS5_VulkanTemplate's)
SDK = os.environ.get("PS5_PAYLOAD_SDK") or next(
    (d for d in (os.path.join(ROOT, ".local", "ps5-deps", "ps5-payload-sdk"),) if os.path.exists(os.path.join(d, "bin", "prospero-lld"))),
    os.path.join(ROOT, "..", "PS5_VulkanTemplate", ".deps", "native", "ps5-payload-sdk"))
VULKAN = os.environ.get("PS5_VULKAN", os.path.join(ROOT, "..", "PS5_Vulkan"))
DEPS = os.path.join(ROOT, ".local", "ps5-deps")
CPM = os.path.join(DEPS, "cpm")
FFMPEG = os.path.join(DEPS, "ffmpeg")
GEN = os.path.join(kit.WORK, "sa", "gen")
OUT = os.path.join(ROOT, "work", "ps5")
HOST, POSIX, PS5 = (os.path.join(ROOT, "host", d) for d in ("", "posix", "ps5"))
VK = os.path.join(ROOT, "gpu", "vk")
OPUS = os.path.join(ROOT, "third_party", "opus")
# The guest window at 64 GiB (host/ps5/win32_ps5.h); Zen 2; the SDK wrapper adds the console's target and its
# required flags (PS5_Vulkan tooling/prospero-clang18). Frame pointers: console backtraces walk them.
COMMON = ["-march=znver2", "-ffp-contract=off", "-DA64_HOST_BASE=0x1000000000ULL", "-fno-omit-frame-pointer",
          "-ffunction-sections", "-fdata-sections", "-g0", "-Wno-everything"]


def pkg(name, version_):
    return os.path.join(CPM, name, version_)


def cc_wrapper(cxx=False):
    return ["sh", os.path.join(VULKAN, "tooling", "prospero-clang18")] + (["-x", "c++"] if cxx else [])


def tree_digest(*dirs, ext=(".h", ".hpp", ".inc")):
    h = hashlib.sha1()
    for d in dirs:
        for base, _, files in sorted(os.walk(d)):
            for f in sorted(files):
                if f.endswith(ext):
                    p = os.path.join(base, f)
                    h.update(p.encode())
                    with open(p, "rb") as fh:
                        h.update(fh.read())
    return h.hexdigest()


def jobs_for(rt64, gen_dir):
    """(source, object, flags, header digest) of everything the title links"""
    out = []
    game_digest = tree_digest(GEN, rt64, os.path.join(kit.KIT, "rt"), os.path.join(ROOT, "nx2", "rt"))
    game_fl = COMMON + ["-I" + rt64, "-I" + GEN, "-I" + os.path.join(kit.KIT, "rt"), "-I" + os.path.join(ROOT, "nx2", "rt"),
                        "-fno-strict-aliasing", "-mcx16", "-O1", "-mllvm", "-rotation-max-header-size=0", "-mllvm",
                        "-capture-tracking-max-uses-to-explore=5000", "-DA64_POLLING", "-DA64_WRITE_TRACK"]
    chunks = sorted(f for f in os.listdir(GEN) if f.startswith("code_") and f.endswith(".c"))
    game = [os.path.join(GEN, c) for c in chunks] + \
        [os.path.join(GEN, f) for f in ("a64_table.c", "a64_hooks.c", "a64_suyu.c", "a64_stubs.c", "hos_image.c", "nvn2_exports.c")] + \
        [os.path.join(rt64, "a64_rt.c")]
    # (the hot pieces, code_hot*.c: the lift's hot functions together, A64_HOT_FUNCS + A64_HOT_CHUNK=1, from upstream's committed
    # assets/profiles/hot_funcs.txt (#1635), at -O2 as nx2/build.py's hot_flags gives them: the hot callees inline into their hot
    # callers. Upstream measured +24 % fps on Death Mountain Trail and +10 % in Kakariko where the game's own code sets the frame)
    hot_fl = [("-" + os.environ.get("RX_HOT_OPT", "O2")) if f == "-O1" else f for f in game_fl]
    out += [(s, os.path.join(OUT, "obj", "game_" + os.path.basename(s)[:-2] + ".o"),
             hot_fl if os.path.basename(s).startswith("code_hot") else game_fl, game_digest) for s in game]

    # the runtime: host/ as Linux builds it, without SDL's loader (host/ps5/hos_sdl_ps5.c) and the Linux-only files
    host_digest = tree_digest(HOST, rt64, GEN) + tree_digest(os.path.join(VK), ext=("vk_api.h", "dlss_math.h"))
    host_fl = COMMON + ["-I" + os.path.join(PS5, "include"), "-I" + POSIX, "-D_GNU_SOURCE", "-include", os.path.join(PS5, "c_compat.h"),
                        "-I" + HOST, "-I" + rt64, "-I" + GEN, "-O2", "-mcx16", "-DA64_POLLING",
                        "-DA64_WRITE_TRACK", "-I" + os.path.join(OPUS, "include"), "-I" + os.path.join(FFMPEG, "include"),
                        "-I" + os.path.join(DEPS, "zstd", "include"),
                        "-I" + os.path.join(pkg("vulkanheaders", "v1.4.345"), "include"),
                        # (host/ps5/ps5_alloc.c: an allocation the full pool cannot back waits for memory)
                        "-Dmalloc=hos_ps5_malloc", "-Dcalloc=hos_ps5_calloc", "-Drealloc=hos_ps5_realloc"]
    # (control.c: the control bridge, a local socket for a desktop's tools, over debug tools host/hos_main.c builds for Windows and
    # Linux only; host/ps5/ps5_stubs.c answers its calls)
    skip = {"display.c", "mf_av1.c", "rx_nowgpu.c", "pgo_rt.c", "hos_sdl.c", "mingw_msvc_shims.c", "msvc_oldnames.c", "control.c"}
    host = [os.path.join(HOST, f) for f in sorted(os.listdir(HOST)) if f.endswith(".c") and f not in skip] + \
        [os.path.join(POSIX, f) for f in ("win32_posix.c", "display_sdl.c", "linux_stubs.c")] + \
        [os.path.join(PS5, f) for f in sorted(os.listdir(PS5)) if f.endswith(".c")]
    stamp = version.define()
    for s in host:
        rel = os.path.relpath(s, HOST).replace(os.sep, "_")[:-2]
        fl = host_fl + ([stamp] if os.path.basename(s) == "hos_main.c" else [])
        out.append((s, os.path.join(OUT, "obj", "host_" + rel + ".o"), fl, host_digest))

    # libopus (nn::codec's Opus decoder)
    ofl = COMMON + ["-O2", "-DOPUS_BUILD", "-DUSE_ALLOCA", "-DHAVE_LRINTF", "-DHAVE_LRINT"] + \
        ["-I" + os.path.join(OPUS, d) for d in ("include", "celt", "silk", "silk/float", "src")]
    for rel in open(os.path.join(OPUS, "sources.txt")).read().split():
        out.append((os.path.join(OPUS, rel), os.path.join(OUT, "obj", "opus_" + rel.replace("/", "_")[:-2] + ".o"), ofl,
                    "opus-1.5.2"))

    # the Vulkan back end, Eden's recompiler and sirit, as nx2/build.py's vk_sources (C++20 with exceptions: Eden throws)
    import build as upstream   # (nx2/build.py: the source list, kept in one place)
    spirv = pkg("spirv-headers", "vulkan-sdk-1.4.341.0")
    vfl = COMMON + ["-I" + POSIX, "-D_GNU_SOURCE", "-include", os.path.join(PS5, "win32_ps5.h"), "-include", os.path.join(PS5, "cxx_compat.h"),
                    "-include", "algorithm", "-include", "cstdio", "-include", "cstdlib", "-include", "cstring",
                    "-include", "functional", "-include", "string", "-include", "vector",
                    "-std=c++20", "-O2", "-DNDEBUG", "-fexceptions", "-fcxx-exceptions", "-DVK_NO_PROTOTYPES",
                    "-DMKWVK_BUILDING", "-DLONGSHOT_RELEASE", "-DMKWVK_INCLUDES=2",
                    "-I" + os.path.join(VK, "compat"), "-I" + os.path.join(ROOT, "gpu", "compat"), "-I" + os.path.join(VK, "eden"),
                    "-I" + os.path.join(ROOT, "gpu", "third_party", "suyu"),
                    "-I" + os.path.join(VK, "sirit", "include"), "-I" + os.path.join(VK, "sirit", "src"),
                    "-I" + os.path.join(spirv, "include"), "-I" + os.path.join(spirv, "include", "spirv", "unified1"),
                    "-I" + os.path.join(pkg("vulkanheaders", "v1.4.345"), "include"), "-I" + VK, "-I" + gen_dir,
                    "-I" + os.path.join(pkg("vulkanmemoryallocator", "v3.3.0"), "include"),
                    # (zstd.h: the console's zstd, with its compressor, ps5/tools/build-zstd.sh; not third_party/zbic's second copy)
                    "-I" + os.path.join(DEPS, "zstd", "include")]
    # (what the shader caches are good for: upstream's stamp of the translator's sources, nx2/translator_files.py. The PS5
    # layer changes none of those files, so a cache is good for as long as upstream's translator stays as it is)
    shader_stamp = "-DLONGSHOT_SHADER_STAMP=0x%sull" % translator_files.stamp(VK)
    lib_digest = tree_digest(os.path.join(VK, "eden"), os.path.join(VK, "sirit"), os.path.join(VK, "compat"))
    vk_digest = tree_digest(VK, os.path.join(ROOT, "gpu", "compat"), os.path.join(ROOT, "gpu", "render"), POSIX, PS5,
                            os.path.join(ROOT, "gpu", "third_party", "suyu", "video_core", "textures"))
    # Dear ImGui (the menu and the overlays: vk_imgui.cpp, vk_menu.cpp), as nx2/build.py compiles it; the game's font files
    # are zstd frames, read with the console's zstd (ps5/tools/build-zstd.sh), not third_party/zstd's second copy
    imgui = os.path.join(ROOT, "third_party", "imgui")
    imgui_fl = ["-I" + imgui, "-I" + os.path.join(DEPS, "zstd", "include"), "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
                "-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS", "-DIMGUI_DISABLE_WIN32_FUNCTIONS", "-DIMGUI_DISABLE_DEMO_WINDOWS",
                "-DIMGUI_DISABLE_DEBUG_TOOLS"]
    imgui_digest = tree_digest(imgui, os.path.join(DEPS, "zstd", "include"))
    for f in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp", "backends/imgui_impl_vulkan.cpp"):
        out.append((os.path.join(imgui, *f.split("/")), os.path.join(OUT, "obj", "vk_imgui_" + os.path.basename(f)[:-4] + ".o"),
                    vfl + imgui_fl, imgui_digest))
    out.append((os.path.join(gen_dir, "ui_fonts.cpp"), os.path.join(OUT, "obj", "vk_ui_fonts.o"), COMMON + ["-O0", "-w"], ""))
    # (the console's own C++: host/ps5/*.cpp, with the back end's flags)
    for f in sorted(os.listdir(PS5)):
        if f.endswith(".cpp"):
            out.append((os.path.join(PS5, f), os.path.join(OUT, "obj", "ps5_" + f[:-4] + ".o"), vfl, vk_digest))
    for s in upstream.vk_sources():
        if os.path.basename(s) == "vk_ngx_msvc_rt.cpp":
            continue
        rel = os.path.relpath(s, os.path.join(ROOT, "gpu")).replace(os.sep, "_")[:-4]
        lib = os.path.relpath(s, VK).split(os.sep)[0] in ("eden", "sirit")
        fl = vfl + ([shader_stamp] if os.path.basename(s) == "vk_shader.cpp" else []) + \
            ([stamp] if os.path.basename(s) == "vk_menu.cpp" else []) + \
            (imgui_fl if os.path.basename(s) in IMGUI_UNITS else [])

        out.append((s, os.path.join(OUT, "obj", "vk_" + rel + ".o"), fl,
                    lib_digest if lib else vk_digest + (imgui_digest if os.path.basename(s) in IMGUI_UNITS else "")))
    return out


def key_of(src, flags, digest):
    h = hashlib.sha1(" ".join(flags).encode() + digest.encode())
    with open(src, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def compile_one(job):
    src, obj, flags, digest = job
    key = key_of(src, flags, digest)
    stamp = obj + ".key"
    if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
        return src, None, 0.0
    cxx = src.endswith(".cpp")
    t = time.time()
    r = subprocess.run(cc_wrapper(cxx) + flags + ["-c", src, "-o", obj], capture_output=True, text=True,
                       env=dict(os.environ, PS5_PAYLOAD_SDK=SDK, PS5_CLANG=os.environ.get("PS5_CLANG") or shutil.which("clang")))
    if r.returncode:
        return src, (r.stdout + r.stderr)[-4000:], time.time() - t
    with open(stamp, "w") as f:
        f.write(key)
    return src, None, time.time() - t


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    p.add_argument("--compile-only", action="store_true")
    p.add_argument("--shortest-first", action="store_true", help="the runtime and the renderer before the lifted game: their errors in minutes")
    a = p.parse_args()
    for need, what in ((os.path.join(GEN, "a64_table.c"), "the lifted game (nx2/lift.py --standalone --write)"),
                       (os.path.join(SDK, "bin", "prospero-lld"), "the PS5 payload SDK (PS5_PAYLOAD_SDK)"),
                       (os.path.join(VULKAN, "tools", "radv-link.sh"), "PS5_Vulkan (PS5_VULKAN)"),
                       (os.path.join(FFMPEG, "include", "libavcodec", "avcodec.h"), "FFmpeg (ps5/tools/build-ffmpeg.sh)"),
                       (os.path.join(DEPS, "zstd", "lib", "libzstd.a"), "zstd (ps5/tools/build-zstd.sh)"),
                       (pkg("vulkanheaders", "v1.4.345"), "the Vulkan headers (ps5/build-title.sh fetches them)")):
        if not os.path.exists(need):
            sys.exit("ps5/build.py: missing %s: %s" % (what, need))
    os.makedirs(os.path.join(OUT, "obj"), exist_ok=True)
    # (a lift with a hot list leaves its hot functions in one code_hot.c, 96 MB: one -O2 compile of it took 79 minutes on a fast
    # core upstream. nx2/split_hot.py cuts it into 8 pieces that compile side by side, as nx2/build.py's route does)
    if os.path.exists(os.path.join(GEN, "code_hot.c")):
        subprocess.run([sys.executable, "-I", os.path.join(ROOT, "nx2", "split_hot.py"), GEN, "8"], check=True)
    rt64 = rtgen.write(os.path.join(OUT, "rt64"))
    gen_dir = os.path.join(OUT, "gen")
    logo_rgba.write(os.path.join(gen_dir, "longshot_logo.h"))
    ui_fonts.write(os.path.join(gen_dir, "ui_fonts.cpp"))
    jobs = jobs_for(rt64, gen_dir)
    # the longest first (the lifted chunks), so no long compile is left alone at the end
    jobs.sort(key=lambda j: os.path.getsize(j[0]) * (1 if a.shortest_first else -1))
    print("ps5: %d files, %d jobs" % (len(jobs), a.jobs), flush=True)
    failed, built, t0 = [], 0, time.time()
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for src, err, secs in pool.map(compile_one, jobs):
            if err:
                failed.append(src)
                print("FAILED %s\n%s" % (os.path.relpath(src, ROOT), err), flush=True)
            elif secs:
                built += 1
                if secs > 30:
                    print("  %s: %.0f s" % (os.path.relpath(src, ROOT), secs), flush=True)
    print("ps5: %d compiled, %d failed (%.0f s)" % (built, len(failed), time.time() - t0), flush=True)
    if failed:
        return 1
    with open(os.path.join(OUT, "objects.txt"), "w") as f:
        f.write("\n".join(j[1] for j in jobs) + "\n")
    if a.compile_only:
        return 0
    env = dict(os.environ, PS5_PAYLOAD_SDK=SDK, PS5_VULKAN=VULKAN, PS5_FFMPEG=FFMPEG)
    linked = subprocess.run(["bash", os.path.join(ROOT, "ps5", "tools", "link.sh"), OUT,
                             os.path.join(kit.WORK, "sa", "image", "image.bin")], env=env).returncode
    return linked


if __name__ == "__main__":
    sys.exit(main())
