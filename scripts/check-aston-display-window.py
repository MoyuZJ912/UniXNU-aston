#!/usr/bin/env python3
"""Host check of the aston display window (aston-display=1).

Compiles the kernel's window origin search, taken from patches/xnu-12377/aston-display-window.patch as patched, and the touch
mapping of package/TouchBoard.h, and runs them on the Ace 3 panel: 1264 x 2780 at 5056 bytes per row, iOS window 1206 x 2622.
usage: check-aston-display-window.py [--cc clang]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def added_function(patch, signature):
    lines = [l[1:] for l in patch.read_text().splitlines() if l.startswith('+') and not l.startswith('+++')]
    text = '\n'.join(lines)
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    origin = added_function(ROOT / 'patches/xnu-12377/aston-display-window.patch', 'static int\naston_display_window_origin(')
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "TouchBoard.h"
''' + origin + r'''
static uint32_t panel_px(int v, uint32_t panel) {return (uint32_t)((int64_t)v * (panel - 1) / 32767);}
static int norm(uint32_t px, uint32_t panel) {return (int)((int64_t)px * 32767 / (panel - 1));}
int main(void) {
    uint32_t x = 0, y = 0;
    assert(aston_display_window_origin(1264, 2780, 5056, 1206, 2622, &x, &y));
    assert(x == 16 && y == 81);
    assert(((uint64_t)y * 5056 + x * 4) % 0x4000 == 0);
    assert(x + 1206 <= 1264 && y + 2622 <= 2780);
    // The whole panel, a window larger than the panel and a short stride are refused or trivial.
    assert(aston_display_window_origin(1264, 2780, 5056, 1264, 2780, &x, &y) && x == 0 && y == 0);
    assert(!aston_display_window_origin(1264, 2780, 5056, 1320, 2868, &x, &y));
    assert(!aston_display_window_origin(1264, 2780, 4000, 1206, 2622, &x, &y));
    // Touch: the window's first and last pixels are its edges, the border clamps to them, the middle stays in the middle.
    assert(tb_window_axis(norm(16, 1264), 1264, 16, 1206) == 0);
    assert(tb_window_axis(norm(16 + 1205, 1264), 1264, 16, 1206) == 32767);
    assert(tb_window_axis(0, 1264, 16, 1206) == 0);
    assert(tb_window_axis(32767, 1264, 16, 1206) == 32767);
    assert(tb_window_axis(norm(81, 2780), 2780, 81, 2622) == 0);
    assert(tb_window_axis(32767, 2780, 81, 2622) == 32767);
    for (uint32_t px = 16; px < 16 + 1206; ++px) {
        const int v = tb_window_axis(norm(px, 1264), 1264, 16, 1206);
        const int expect = norm(px - 16, 1206);
        assert(v >= expect - 30 && v <= expect + 30);   // one panel pixel is 26 units of the window's axis
    }
    for (int v = 0; v <= 32767; v += 7) {
        const int a = tb_window_axis(v, 2780, 81, 2622);
        assert(a >= 0 && a <= 32767);
        assert(v == 0 || a >= tb_window_axis(v - 7, 2780, 81, 2622));   // monotonic
    }
    assert(panel_px(32767, 1264) == 1263);
    // A window outside the panel maps nothing.
    assert(tb_window_axis(1234, 1264, 100, 1206) == 1234);
    puts("PASS: window 1206x2622 at (16,81) on 1264x2780, 16 KiB aligned; touch maps into the window and clamps the border");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='aston-window-') as td:
        source, binary = Path(td) / 'window.c', Path(td) / 'window'
        source.write_text(code)
        subprocess.run([*args.cc.split(), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'package'), str(source), '-o',
                        str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
