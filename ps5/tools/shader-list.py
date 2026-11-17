"""PS5: longshot_shaders.bin, the list of the game's shaders the first start prepares, made from your own .nup.

    python3 -I ps5/tools/shader-list.py <your game.nup> --out <longshot_shaders.bin> [--work DIR]

nx2/shader_census.py reads an extracted romfs, and the whole romfs is ~25 GB: under WSL, read from Windows' disk, its
extraction took Ubuntu down (2026-11-17, 7 GB of memory). The census reads only Pack/ZsDic.pack.zs and the archives in
Shader/, Layout2Shader/, Lib/, Movie/ and Effect/ (shader_census.archives), so only those are taken out of the .nup, into
--work (default: beside --out), and the folder is removed once the list is written. Nothing made here leaves the PC.
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import nup as nupmod   # noqa: E402  (nx2/nup.py)

FOLDERS = ("Shader", "Layout2Shader", "Lib", "Movie", "Effect")   # (shader_census.archives)


def wanted(rel):
    parts = rel.replace("\\", "/").strip("/").split("/")
    if parts == ["Pack", "ZsDic.pack.zs"]:
        return True
    name = parts[-1]
    return parts[0] in FOLDERS and (name.endswith(".zs") or ".bfsha" in name or ".sarc" in name or ".pck" in name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("nup")
    ap.add_argument("--out", required=True)
    ap.add_argument("--work")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or out + ".romfs")
    romfs = os.path.join(work, "romfs")
    package = nupmod.Nup(a.nup)
    t = nupmod.pick_title(package, None)
    where = nupmod.Destination(work)
    n = total = 0
    for rel, r in package.walk(t["romfs"]):
        if not wanted(rel):
            continue
        dst = where.path("romfs", rel)
        if not (os.path.isfile(dst) and os.path.getsize(dst) == r[2]):   # (an interrupted run: what is whole is kept)
            with open(dst + ".part", "wb") as g:
                package.copy_to(r, g, rel)
            os.replace(dst + ".part", dst)
        n += 1
        total += r[2]
    package.f.close()
    print("%d of the game's files with shaders taken out (%.0f MB)" % (n, total / 1e6), flush=True)
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "nx2", "shader_census.py"), romfs, "--out", out + ".part"],
                   check=True)
    os.replace(out + ".part", out)
    shutil.rmtree(work)
    print("written: %s (%.0f MB)" % (out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
