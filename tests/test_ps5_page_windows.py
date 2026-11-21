"""The page table that takes a guest address to its pool's device address maps the platform's own guest layout.

Programs that follow a pointer no bound buffer holds read it through this table (spirv_emit_context.cpp guest_lookup,
kGuestPagesBase...), which the back end fills as pools come and go (vk_memory.cpp guest_pages_set, page_table_census.h
kPageWindows). The windows must be where host/hos.h (host/ps5/ps5_layout.h on the console) puts the guest's heap (window one) and the top of its address space,
where address regions are handed out (window two). The console's port moved the heap from 64 GiB to 16 GiB and the top
from 512 GiB to 64 GiB; with the PC's windows no page was ever mapped there and every followed pointer read 0 (OoT's
foliage culling then left one of its two list sets empty: the Sacred Meadow's tree crowns every other frame).
tests/test_page_table_census.py holds the PC's windows and the census; this holds each platform's against host/hos.h.

python -I -B tests/test_ps5_page_windows.py [--cxx clang++]
No game or GPU.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
NAMES = ('kGuestPagesBase', 'kGuestPagesBytes', 'kGuestPagesBase2', 'kGuestPagesBytes2')


def layout(hos, ps5_layout, ps5):
    """HOS_HEAP_BASE and the end of the guest window for that platform: the PC's from host/hos.h, the console's from
    host/ps5/ps5_layout.h, which host/hos.h includes under __PROSPERO__ and which #undefs and defines them again"""
    if ps5 and '#include "ps5/ps5_layout.h"' not in hos:
        raise SystemExit('FAIL: host/hos.h no longer includes ps5/ps5_layout.h for the console')
    text = ps5_layout if ps5 else hos

    def pick(name):
        found = re.findall(r'^#define ' + name + r' (.+)$', text, re.M)
        if len(found) != 1:
            raise SystemExit('FAIL: %s defines %s %d times, not once' % ('host/ps5/ps5_layout.h' if ps5 else 'host/hos.h', name,
                                                                          len(found)))
        return eval(found[0].split('/*')[0].strip().replace('ULL', ''))
    return pick('HOS_HEAP_BASE'), pick('HOS_GUEST_SPACE')


def shader_windows(emit, ps5):
    """The four constants the translated shaders are made with on that platform: the PC's first, under #ifndef __PROSPERO__,
    the console's under its #else. With no such block both platforms have the same."""
    at = emit.index('constexpr unsigned long long kGuestPagesBase =')
    opened = emit.rfind('#ifndef __PROSPERO__', 0, at)
    part = emit[at:emit.index('void EmitContext::DefineGlobalMemoryFunctions', at)]
    if opened >= 0 and '#endif' not in emit[opened:at] and '#else' in part:
        part = part.split('#else')[1 if ps5 else 0]
    got = {}
    for name in NAMES:
        m = re.search(r'constexpr unsigned long long %s = (0x[0-9a-fA-F]+|\d+)ull(?:\s*<<\s*(\d+))?;' % name, part)
        if not m:
            raise SystemExit('FAIL: spirv_emit_context.cpp: no %s for %s' % (name, 'the console' if ps5 else 'the PC'))
        got[name] = int(m.group(1), 0) << int(m.group(2) or 0)
    return got


FIXTURE = r"""
#include "page_table_census.h"
#include <cstdio>
#include <cstdlib>
using namespace mkwvk;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
    const uint64_t heap = HEAP_BASE, end = SPACE_END;
    CHECK(sizeof kPageWindows / sizeof kPageWindows[0] == 2);
    const PageWindow& a = kPageWindows[0];
    const PageWindow& b = kPageWindows[1];
    CHECK(a.base == heap);                    // (window one: from the heap's base)
    CHECK(b.base + b.bytes == end);           // (window two: up to the end of the guest's space)
    CHECK(a.base + a.bytes <= b.base);        // (apart)
    // (the shaders' own constants, which index the same table)
    CHECK(a.base == S_BASE && a.bytes == S_BYTES && b.base == S_BASE2 && b.bytes == S_BYTES2);
    CHECK(a.first == 0 && b.first == a.bytes >> 12);
    CHECK(kPageTableEntries == (a.bytes + b.bytes) >> 12);
    CHECK(page_table_outside(heap, heap + 0x1000) == 0);
    CHECK(page_table_outside(heap - 0x1000, heap) == 0x1000);
    CHECK(page_table_outside(end - 0x1000, end) == 0);
    CHECK(page_table_outside(end, end + 0x1000) == 0x1000);
#ifdef __PROSPERO__
    // (addresses the console's log gave OoT's culling buffers and a region near the top, 2026-11-16)
    CHECK(page_table_outside(0x578860000ull, 0x578861000ull) == 0);
    CHECK(page_table_outside(0x57fc0e000ull, 0x57fc0f000ull) == 0);
    CHECK(page_table_outside(0xfd8000000ull, 0xfd8001000ull) == 0);
    CHECK(page_table_outside(heap, heap + (6894ull << 20)) == 0);   // (the whole heap the game asks for: 6894 MB)
#endif
    std::puts("PASS");
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--cxx', default='clang++')
    a = parser.parse_args()
    vk = a.source / 'gpu/vk'
    hos = (a.source / 'host/hos.h').read_text(encoding='utf-8')
    ps5_layout = (a.source / 'host/ps5/ps5_layout.h').read_text(encoding='utf-8')
    emit = (vk / 'eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='page-windows-') as tmp:
        folder = Path(tmp)
        cpp = folder / 'fixture.cpp'
        cpp.write_text(FIXTURE, encoding='utf-8')
        for ps5 in (False, True):
            who = 'the console' if ps5 else 'the PC'
            heap, end = layout(hos, ps5_layout, ps5)
            s = shader_windows(emit, ps5)
            exe = folder / ('fixture_ps5' if ps5 else 'fixture_pc')
            cmd = [a.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(vk), '-DHEAP_BASE=%dull' % heap,
                   '-DSPACE_END=%dull' % end, '-DS_BASE=%dull' % s['kGuestPagesBase'], '-DS_BYTES=%dull' % s['kGuestPagesBytes'],
                   '-DS_BASE2=%dull' % s['kGuestPagesBase2'], '-DS_BYTES2=%dull' % s['kGuestPagesBytes2']]
            cmd += (['-D__PROSPERO__'] if ps5 else []) + [str(cpp), '-o', str(exe)]
            subprocess.run(cmd, cwd=folder, check=True)
            if subprocess.run([str(exe)]).returncode:
                raise SystemExit('FAIL: %s: the page table\'s windows are not its guest layout (heap %#x, end %#x)' % (who, heap, end))
            print('PASS: %s: the table maps the heap from %#x and the 4 GB below %#x' % (who, heap, end))


if __name__ == '__main__':
    main()
