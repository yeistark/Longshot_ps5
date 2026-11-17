#!/usr/bin/env bash
# Longshot for PS5: the whole build on a Linux PC (or Ubuntu under WSL on Windows), from your own game file to the
# title folder, in one command.
#
#   build-title.sh <your game.nup> [--ps5 <console address>] [--jobs N] [--out DIR]
#   build-title.sh --packages               the packages it needs, as apt names them (Ubuntu 24.04)
#   sudo build-title.sh --install-packages  installs them (apt only)
#
# Run from a checkout, it builds that checkout. Run on its own (just this file), it first clones Longshot_ps5 at REF
# into a PS5_Homebrews folder where it is run. What the title links is cloned beside Longshot_ps5, at the revisions this
# build was played with on a console, and built once:
#
#   PS5_Vulkan       the link recipe, the tool that makes the console's executable, libc.prx
#   PS5_Mesa         RADV, Mesa's Vulkan driver, built for the console by PS5_Vulkan (the long step)
#   PS5_PayloadSDK   the platform layer the title is compiled with, with ps5/sdk/ps5-payload-sdk-heap.patch
#
# A project that is already there is used as it is. A step that is done is not done again: after a change, or an
# interrupted run, run it again. Nothing made from your game leaves work/ and dist/ unless you name a console.
#
# --ps5 <address>: when the title is built, send it to the console over FTP (port 2121, or LONGSHOT_FTP_PORT):
#   the title folder to /data/homebrew/PPSA99025 and your game file to /data/lichii/roms/oot/oot.nup (skipped when
#   the console already has a file of its size). Close the game on the console first.
# --jobs N: compile N files at once. By default your cores less two, and no more than one for each 3 GB of memory.
#   A build that is killed for want of memory wants fewer (--jobs 2).
# --out DIR: also copy the finished title folder into DIR.
set -euo pipefail

REPO=${LONGSHOT_REPO:-https://github.com/yeistark/Longshot_ps5.git}
REF=${LONGSHOT_REF:-main}
TITLE=PPSA99025
VULKAN_REV=7f492b4c6f67a4e8c8c67a4272cfeef22abea9cc
MESA_REV=0f86253f674c425052948eb3cb116c81e646a734
SDK_REV=55a2de04e3364f0a6c8d6c337dc95436cf091ef0
# (the headers gpu/vk is compiled with: scripts/linux/deps.sh's pins, which ps5/build.py names)
VK_HEADERS=v1.4.345 SPIRV_HEADERS=vulkan-sdk-1.4.341.0 VMA=v3.3.0
PYTHON_PACKAGES=(lz4 numpy capstone "unicorn>=2.1,<2.2" zstandard meson ninja mako pyyaml packaging)
APT_PACKAGES="git curl wget unzip xz-utils rsync make nasm pkg-config bison flex file patch ca-certificates python3 python3-venv \
clang lld llvm llvm-dev libclang-rt-18-dev libclang-dev libclang-cpp-dev libclc-18-dev libllvmspirvlib-18-dev spirv-tools glslang-tools \
zlib1g-dev libzstd-dev"

die() { printf '\nbuild-title: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }

game=${LONGSHOT_GAME:-} out=${LONGSHOT_OUT:-} ps5=${LONGSHOT_PS5:-} jobs=
args=("$@")
while (( $# )); do
    case $1 in
        --packages) echo "$APT_PACKAGES"; exit 0 ;;
        --install-packages)
            command -v apt-get > /dev/null || die "--install-packages is for apt (Ubuntu, Debian): --packages lists them"
            apt-get update
            # shellcheck disable=SC2086
            DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $APT_PACKAGES
            exit 0 ;;
        --ps5) ps5=${2:?--ps5 takes the console address}; shift ;;
        --jobs) jobs=${2:?--jobs takes a number}; shift ;;
        --out) out=${2:?--out takes a folder}; shift ;;
        -h | --help) sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option $1 (--help says what there is)" ;;
        *) game=$1 ;;
    esac
    shift
done
[[ -n $game ]] || die "which game? build-title.sh <your game.nup>"
[[ -f $game ]] || die "no such file: $game"
game=$(realpath -- "$game")
[[ -z $out ]] || { mkdir -p -- "$out"; out=$(realpath -- "$out"); }

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ ! -f $here/build.py || ! -f $here/../nx2/lift.py ]]; then
    # On its own: the repository first, then its copy of this script takes over.
    command -v git > /dev/null || die "git is needed (sudo apt install git)"
    home=${LONGSHOT_HOME:-$PWD/PS5_Homebrews}
    mkdir -p -- "$home"
    step "Longshot_ps5 at $REF, into $home/Longshot_ps5"
    if [[ ! -d $home/Longshot_ps5/.git ]]; then
        git -c advice.detachedHead=false clone -q --branch "$REF" "$REPO" "$home/Longshot_ps5" ||
            die "could not clone $REPO"
    else
        # (a folder of the same name from another repository, such as a checkout of upstream's: never moved)
        [[ $(git -C "$home/Longshot_ps5" remote get-url origin 2> /dev/null) == "$REPO" ]] ||
            die "$home/Longshot_ps5 is a checkout of another repository: move it, or set LONGSHOT_HOME to another folder"
        git -C "$home/Longshot_ps5" fetch -q --force --tags origin
        git -C "$home/Longshot_ps5" checkout -q "$REF" ||
            die "$home/Longshot_ps5 has changes of its own: build it with its own ps5/build-title.sh"
        git -C "$home/Longshot_ps5" merge -q --ff-only "origin/$REF" 2> /dev/null || true
    fi
    LONGSHOT_GAME=$game LONGSHOT_OUT=$out LONGSHOT_PS5=$ps5 exec bash "$home/Longshot_ps5/ps5/build-title.sh" "${args[@]}"
fi
root=$(dirname "$here")
parent=$(dirname "$root")
logs="$root/work/build-title"
deps="$root/.local/ps5-deps"
mkdir -p "$logs" "$deps/downloads"
cd "$root"

run() {   # log-name command...: quiet while it works, the end of its log when it fails
    local log="$logs/$1.log" start=$SECONDS
    shift
    "$@" > "$log" 2>&1 || { tail -n 30 "$log" >&2; die "that step failed; the whole log: $log"; }
    note "done in $(( SECONDS - start )) s"
}

step "this PC"
missing=()
for command in git curl wget unzip tar xz rsync make nasm pkg-config bison flex patch python3 clang clang++ llvm-ar \
        glslangValidator flock sha256sum; do
    command -v "$command" > /dev/null || missing+=("$command")
done
if (( ${#missing[@]} )); then
    note "not found: ${missing[*]}"
    if command -v apt-get > /dev/null; then
        note "install what this needs with:"
        note "  sudo bash ${BASH_SOURCE[0]} --install-packages"
    else
        note "install them with your system's packages (and LLVM's and Clang's development files, libclc,"
        note "the SPIR-V LLVM translator and SPIRV-Tools, which the driver's build needs)"
    fi
    exit 1
fi
clang_major=$(clang --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)
[[ -n $clang_major && $clang_major -ge 18 ]] || die "clang 18 or later is needed (this one: ${clang_major:-unknown})"
free_gb=$(df -Pk "$root" | awk 'NR == 2 { printf "%d", $4 / 1048576 }')
note "ok: clang $clang_major, $(nproc) cores, $(awk '/MemTotal/ { printf "%.0f GB of memory", $2 / 1048576 }' /proc/meminfo), $free_gb GB free here"
(( free_gb >= 40 )) || note "warning: the first build wants about 60 GB free"

step "Python's packages, in .local/venv"
python="$root/.local/venv/bin/python"
stamp="${PYTHON_PACKAGES[*]}"
if [[ ! -x $python || $(cat "$root/.local/venv/.packages" 2> /dev/null) != "$stamp" ]]; then
    [[ -x $python ]] || python3 -m venv "$root/.local/venv" ||
        die "python3 cannot make a virtual environment (sudo apt install python3-venv)"
    run pip "$python" -m pip install --disable-pip-version-check "${PYTHON_PACKAGES[@]}"
    printf '%s\n' "$stamp" > "$root/.local/venv/.packages"
else
    note "there already"
fi
export PATH="$root/.local/venv/bin:$PATH"

step "the projects beside this one"
project() {   # name revision [clone options]
    local name=$1 revision=$2 path="$parent/$1"
    shift 2
    if [[ -d $path/.git ]]; then
        # (a checkout of its own, detached and unchanged, moves to the pinned revision; one on a branch, or with
        # changes, is someone's own: used as it is)
        git -C "$path" cat-file -e "$revision^{commit}" 2> /dev/null || { note "$name: fetching ${revision:0:11}"; git -C "$path" fetch -q origin || true; }
        if [[ " $* " != *" --no-checkout "* ]] && ! git -C "$path" symbolic-ref -q HEAD > /dev/null &&
            [[ -z $(git -C "$path" status --porcelain --untracked-files=no) && $(git -C "$path" rev-parse HEAD) != "$revision"* ]]; then
            git -C "$path" checkout -q --detach "$revision" 2> /dev/null && note "$name: moved to ${revision:0:11}"
        fi
        note "$name: there already ($(git -C "$path" rev-parse --short HEAD))"
    else
        note "$name: cloning"
        git clone -q "$@" "https://github.com/mihawk-99/$name.git" "$path"
        [[ " $* " == *" --no-checkout "* ]] || git -C "$path" checkout -q --detach "$revision"
    fi
    git -C "$path" cat-file -e "$revision^{commit}" 2> /dev/null || die "$path does not have $revision: update it (git fetch)"
}
vulkan="$parent/PS5_Vulkan"
project PS5_Vulkan "$VULKAN_REV"
project PS5_Mesa "$MESA_REV" --no-checkout   # (its revision is exported by PS5_Vulkan's build: no files of its own)
project PS5_PayloadSDK "$SDK_REV" --no-checkout

step "PS5_Vulkan: its payload SDK"
sdk_pin=$(sed -n 's/^sdk_revision=//p' "$vulkan/tools/setup-native-dependencies.sh")
if [[ $(cat "$vulkan/.deps/native/ps5-payload-sdk/.ps5-sdk-revision" 2> /dev/null) == "$sdk_pin" ]]; then
    note "there already"
else
    (cd "$vulkan" && run vulkan-sdk bash tools/setup-native-dependencies.sh)
fi

step "PS5_Vulkan: the tool that makes the console's executable, and libc.prx"
if [[ -x $vulkan/build/host/ps5-native-tool && -f $vulkan/runtime/libc.prx ]]; then
    note "there already"
else
    # (make goes on to PS5_Vulkan's own sample title, which this build does not need: the two files are what counts)
    (cd "$vulkan" && make > "$logs/vulkan-make.log" 2>&1) || true
    [[ -x $vulkan/build/host/ps5-native-tool && -f $vulkan/runtime/libc.prx ]] ||
        { tail -n 30 "$logs/vulkan-make.log" >&2; die "PS5_Vulkan's make did not build them; the whole log: $logs/vulkan-make.log"; }
    note "done"
fi

step "RADV, the Vulkan driver, at PS5 Mesa ${MESA_REV:0:11} (the long step: 10 to 30 minutes the first time)"
archive="$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
if [[ -f $archive ]] && grep -q "revision: $MESA_REV" "$vulkan/.deps/native/radv-release/PROVENANCE.txt" 2> /dev/null; then
    note "there already"
else
    (cd "$vulkan" && PS5_MESA_FORK="$parent/PS5_Mesa" PS5_MESA_REVISION=$MESA_REV run radv bash tools/build-radv.sh release)
fi
[[ -f $archive ]] || die "no RADV archive at $archive"

step "the payload SDK the title is compiled with, at ${SDK_REV:0:11}, with the heap patch"
sdk="$deps/ps5-payload-sdk"
heap_patch="$root/ps5/sdk/ps5-payload-sdk-heap.patch"
patch_stamp="$SDK_REV $(sha256sum "$heap_patch" | cut -c1-16)"
if [[ $(cat "$sdk/.longshot-sdk" 2> /dev/null) == "$patch_stamp" && -x $sdk/bin/prospero-lld ]]; then
    note "there already"
else
    tree=$(mktemp -d)
    git -C "$parent/PS5_PayloadSDK" archive "$SDK_REV" | tar -x -C "$tree"
    patch -s -d "$tree" -p1 < "$heap_patch" || die "the heap patch does not apply to PS5_PayloadSDK ${SDK_REV:0:11}"
    run sdk bash "$tree/platform/tools/setup-sdk.sh" "$sdk" "$SDK_REV" "$deps/downloads"
    rm -rf -- "$tree"
    printf '%s\n' "$patch_stamp" > "$sdk/.longshot-sdk"
fi
[[ -x $sdk/bin/prospero-lld ]] || die "the payload SDK at $sdk is incomplete"
grep -q ps5_heap_arenas "$sdk/target/include/ps5platform/heap.h" || die "the payload SDK at $sdk lacks the heap patch"
export PS5_PAYLOAD_SDK="$sdk" PS5_VULKAN="$vulkan"

step "the Vulkan, SPIR-V and VMA headers"
headers() {   # repository tag name
    local to="$deps/cpm/$3/$2"
    [[ -d $to/include ]] && return
    curl -fsSL --retry 5 --retry-all-errors --proto '=https' -o "$deps/downloads/$3-$2.tar.gz" "https://github.com/$1/archive/refs/tags/$2.tar.gz"
    mkdir -p "$to"
    tar -xzf "$deps/downloads/$3-$2.tar.gz" -C "$to" --strip-components=1
}
headers KhronosGroup/Vulkan-Headers "$VK_HEADERS" vulkanheaders
headers KhronosGroup/SPIRV-Headers "$SPIRV_HEADERS" spirv-headers
headers GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator "$VMA" vulkanmemoryallocator
note "there"

game_stamp="$(stat -c '%s %Y' -- "$game") $(git -C "$root" rev-parse HEAD 2> /dev/null || echo none)"
step "your game: its program read out of $(basename -- "$game") and translated to C (2 to 5 minutes)"
if [[ -f work/lift/sa/gen/a64_table.c && -f work/lift/sa/image/image.bin && $(cat work/lift/.game 2> /dev/null) == "$game_stamp" ]]; then
    note "done already, from this file"
else
    run game-extract "$python" -I -u -X utf8 nx2/nup.py "$game" --only exefs --extract "$root/work/game"
    mkdir -p "$root/work/lift/image"   # (the lift writes its chunk list there and does not make the folder)
    OOT_WORK="$root/work/lift" run game-lift "$python" -I -u -X utf8 nx2/lift.py --standalone --write \
        --exefs "$root/work/game/exefs" --work "$root/work/lift"
    printf '%s\n' "$game_stamp" > work/lift/.game
fi

step "your game: the list of its shaders, for the first start's preparation (a few minutes)"
shaders="$root/work/longshot_shaders.bin"
if [[ -s $shaders && $(cat "$shaders.game" 2> /dev/null) == "$game_stamp" ]]; then
    note "done already, from this file"
else
    # (only the archives with shaders are taken out of the .nup, and removed once the list is written: the console reads
    # the game's files from the .nup itself)
    run shader-list "$python" -I -u -X utf8 ps5/tools/shader-list.py "$game" --out "$shaders"
    printf '%s\n' "$game_stamp" > "$shaders.game"
fi

step "FFmpeg and zstd for the console (the game's movies, and the game file's compressed parts)"
run ffmpeg bash ps5/tools/build-ffmpeg.sh
run zstd bash ps5/tools/build-zstd.sh

step "the title: the game, the runtime and the renderer compiled and linked (30 to 60 minutes the first time)"
title="$root/dist/$TITLE"
if [[ -z $jobs ]]; then
    # (one for each 3 GB: with 8 GB under WSL, three at once took Ubuntu down for want of memory, 2026-11-17)
    jobs=$(awk -v cores="$(nproc)" '/MemTotal/ { m = int($2 / 3145728); c = cores - 2; j = c < m ? c : m; print j < 1 ? 1 : j }' /proc/meminfo)
fi
note "$jobs files at once (--jobs)"
"$python" -I ps5/build.py --jobs "$jobs" --shortest-first 2>&1 | tee "$logs/title.log" ||
    die "the title was not built; the whole log: $logs/title.log (if the compiler was killed for want of memory, use --jobs 2)"
[[ -f $title/eboot.bin && -f $title/image.bin ]] || die "no title at $title; the log: $logs/title.log"
cp -- "$shaders" "$title/longshot_shaders.bin"
if [[ -n $out ]]; then
    rm -rf -- "${out:?}/$TITLE"
    cp -r -- "$title" "$out/$TITLE"
    title="$out/$TITLE"
fi

if [[ -n $ps5 ]]; then
    port=${LONGSHOT_FTP_PORT:-2121}
    ftp="ftp://$ps5:$port"
    step "sending it to the console at $ps5 (FTP port $port)"
    curl -s --max-time 10 "$ftp/" > /dev/null ||
        die "no FTP server answers at $ps5:$port: is the console on, with its homebrew environment and FTP running?"
    remote_size() { curl -s --max-time 30 -I "$1" 2> /dev/null | tr -d '\r' | sed -n 's/^Content-Length: //p'; }
    (cd "$title" && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
        note "$f"
        curl -s --ftp-create-dirs -T "$title/$f" "$ftp/data/homebrew/$TITLE/$f" || die "could not send $f"
    done
    nup_size=$(stat -c %s -- "$game")
    if [[ $(remote_size "$ftp/data/lichii/roms/oot/oot.nup") == "$nup_size" ]]; then
        note "oot.nup: the console has it already"
    else
        note "oot.nup: your game file, $(( nup_size >> 20 )) MB (a few minutes)"
        curl -s --ftp-create-dirs -T "$game" "$ftp/data/lichii/roms/oot/oot.nup" || die "could not send the game file"
    fi
    note "sent"
fi

cat << EOF

Built: $title ($(du -sh -- "$title" | cut -f1))
EOF
if [[ -z $ps5 ]]; then cat << EOF

On the console, with its homebrew environment running and an FTP server on (port 2121):
  1. Copy the whole $TITLE folder to /data/homebrew/$TITLE.
  2. Copy your game file to /data/lichii/roms/oot/oot.nup (make the folders; the name must be oot.nup).
  (Or run this again with --ps5 <your console's address>: it sends both.)
EOF
fi
cat << EOF

Then start Longshot from the home screen (ShadowMount puts it there), or from your homebrew launcher.
The first start prepares every shader (about 2.5 hours) and closes by itself; the second goes to the game.
eboot.bin, image.bin and longshot_shaders.bin are made from your game: keep them on your PC and your console.
EOF
