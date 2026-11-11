// SPDX-License-Identifier: MIT
// aston display window (aston-display=1, patches/xnu-12377/aston-display-window.patch): the kernel shows iOS a 1206 x 2622 window of
// the 1264 x 2780 panel (402 x 874 points at scale 3, a screen class iOS knows). This paints the panel outside the window black once,
// at the start of the board probes; until then the borders keep what the firmware drew there. TouchProbe.cpp maps contacts into the
// window (TouchBoard.h tb_window_axis).
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>

extern "C" {
// x, y, width, height, panel width, panel height (all zero: the whole panel), set by the kernel at boot.
extern uint32_t aston_display_window[6];
// BootLog: 0 whole panel, 1 borders painted, -1 the console does not match the window, -2 no mapping.
volatile int32_t aston_display_window_state;
}

extern "C" void aston_display_window_probe(void) {
    const uint32_t x = aston_display_window[0], y = aston_display_window[1], w = aston_display_window[2], h = aston_display_window[3];
    const uint32_t pw = aston_display_window[4], ph = aston_display_window[5];
    if (!w || aston_display_window_state) return;
    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr || video.v_width != w || video.v_height != h || video.v_depth != 32 ||
        video.v_rotate || video.v_rowBytes < uint64_t(pw) * 4 || video.v_rowBytes % 4 || x > pw || w > pw - x || y > ph || h > ph - y) {
        aston_display_window_state = -1;
        return;
    }
    const uint64_t stride = video.v_rowBytes, offset = uint64_t(y) * stride + uint64_t(x) * 4;
    if (uint64_t(video.v_baseAddr) < offset) {
        aston_display_window_state = -1;
        return;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(uint64_t(video.v_baseAddr) - offset, stride * ph);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    if (memory) memory->release();
    if (!map) {
        aston_display_window_state = -2;
        return;
    }
    volatile uint32_t *frame = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    for (uint32_t row = 0; row < ph; ++row) {
        volatile uint32_t *line = frame + uint64_t(row) * (stride / 4);
        const bool inside = row >= y && row < y + h;
        for (uint32_t col = 0; col < pw; ++col) {
            if (!inside || col < x || col >= x + w) line[col] = 0xff000000u;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    map->release();
    aston_display_window_state = 1;
}
