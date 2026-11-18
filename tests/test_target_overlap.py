"""Compile and run the render-target overlap rule (vk_texture.cpp's dead transient targets; no game/GPU required).

texture_frame's drawn_over_shifted is cut out of the source unmodified and run over stub textures
(tests/test_target_overlap.cpp): one newer target over a part of an old one, and several whose parts add up.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "g++"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_source(src):
    """The eviction pass still uses the rule"""
    for needle in ('#include "target_overlap.h"', "target_overlap::ShiftedOver over{lo, hi, tex->last_frame};",
                   "const bool overwritten = shifted || drawn_over(tex);", "return !drawn_over(tex) && !drawn_over_shifted(tex);",
                   'getenv("RX_VK_DEAD_MIN_UNUSED")', "if (tex->last_value > completed)"):
        if needle not in src:
            raise SystemExit("vk_texture.cpp no longer has: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    src = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")
    check_source(src)
    frame = block(src, "void texture_frame(uint64_t completed)")
    rule = block(frame, "const auto drawn_over_shifted = [&](const Texture* tex) {") + ";\n"
    with tempfile.TemporaryDirectory(prefix="target-overlap-") as tmp:
        (Path(tmp) / "drawn_over_shifted.inc").write_text(rule, encoding="utf-8")
        exe = Path(tmp) / "target-overlap"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", tmp,
                        str(ROOT / "tests/test_target_overlap.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
