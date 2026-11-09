// SPDX-License-Identifier: MIT
// aston boot screen: from the start of the board probes (bsd_init, before the root mount) until iOS presents its first frame, the
// firmware framebuffer shows a title, a moving bar, the elapsed time, the stage the kernel can see and the newest lines of the kernel
// message buffer, so a long boot (the first-boot data migration) or a stopped one shows where it is. Boot argument
// aston-boot-screen=0 turns it off.
//
// It draws only into the framebuffer the firmware handed over (PE_current_console) and stops for good, without clearing, once iOS
// draws: the legacy display accepted a swap (native_legacy_swap_accepted_count), or a sentinel pixel this screen owns changed.
// One kernel thread does all drawing, with BootProgressDisplay.h's text rows (Mu-Silicium Font.h, cells redrawn only when they
// change); nothing else is touched.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "BootProgressCore.h"
#include "BootProgressDisplay.h"
#pragma clang diagnostic pop

extern "C" {
// The kernel message buffer (bsd/sys/msgbuf.h); only these leading fields are read, as in BootLog.c.
struct aston_boot_msgbuf {int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc;};
extern struct aston_boot_msgbuf *msgbufp;
extern volatile uint32_t native_legacy_swap_accepted_count __attribute__((weak_import));
extern volatile int aston_data_capacity_status __attribute__((weak_import));
extern volatile int aston_migration_rerun_status __attribute__((weak_import));
// [0] state: 0 off, 1 drawing, 2 iOS drew (swap accepted), 3 iOS drew (sentinel changed), 4 time limit, -1 no usable framebuffer,
// -2 no memory, -3 no thread; [1] screens drawn; [2] last second shown.
volatile int32_t aston_boot_screen_stats[3];
}

namespace {
constexpr unsigned kScale = 3;          // 18 x 42 pixel cells: 70 columns on the 1264-pixel panel
constexpr unsigned kRows = 48;          // the rows BootProgressDisplay.h keeps
constexpr unsigned kLogFirst = 6;       // rows 6..47 hold the log
constexpr unsigned kTail = 16384;       // bytes of the message buffer looked at
constexpr unsigned kLimitSeconds = 3600;
constexpr uint32_t kSentinel = 0x00010203, kTitle = 0x00ffffff, kTrack = 0x00202428, kBar = 0x003a7bd5, kStage = 0x00ffd166,
                   kLog = 0x009aa0a6;

bp_display *gDisplay;
IOMemoryMap *gMap;
char *gTail;                            // kTail + 1 bytes
char (*gLines)[BP_COLS];                // kRows - kLogFirst visual rows
uint64_t gStart;
unsigned gBarY, gBarX, gBarWidth;

unsigned seconds() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - gStart, &ns);
    return unsigned(ns / 1000000000ull);
}

volatile uint32_t *pixel(unsigned x, unsigned y) {
    return reinterpret_cast<volatile uint32_t *>(gDisplay->frame + uintptr_t(y) * gDisplay->stride + uintptr_t(x) * 4);
}

void fill(unsigned x, unsigned y, unsigned w, unsigned h, uint32_t color) {
    if (gDisplay->yield_requested || x >= gDisplay->width || y >= gDisplay->height) return;
    if (w > gDisplay->width - x) w = gDisplay->width - x;
    if (h > gDisplay->height - y) h = gDisplay->height - y;
    for (unsigned r = 0; r < h; ++r) {
        volatile uint32_t *p = pixel(x, y + r);
        for (unsigned c = 0; c < w; ++c) p[c] = color;
    }
}

// iOS has drawn once the legacy display accepted a swap, or once the 2x2 sentinel in the top left corner changed.
int iosDrew() {
    if (&native_legacy_swap_accepted_count && native_legacy_swap_accepted_count) return 2;
    for (unsigned y = 0; y < 2; ++y)
        for (unsigned x = 0; x < 2; ++x)
            if ((*pixel(x, y) & 0x00ffffff) != kSentinel) return 3;
    return 0;
}

// A bar of a fifth of the track's width moving back and forth, one step every 100 ms.
void drawBar(unsigned step) {
    const unsigned segment = gBarWidth / 5, travel = gBarWidth - segment, period = 2 * 40;
    unsigned phase = step % period;
    unsigned at = (phase < 40 ? phase : period - phase) * travel / 40;
    fill(gBarX, gBarY, gBarWidth, 12, kTrack);
    fill(gBarX + at, gBarY, segment, 12, kBar);
}

const char *stage() {
    if (&aston_migration_rerun_status && aston_migration_rerun_status == 0)
        return "Rebuilding app data (migration): slow, keep charging.";
    if (&aston_data_capacity_status && aston_data_capacity_status >= 0)
        return "Data mounted; starting iOS (a first boot migrates data).";
    return "Starting the kernel and storage.";
}

// The newest lines of the message buffer, wrapped to the screen width, into gLines; returns the row count.
unsigned tailLines(unsigned cols) {
    aston_boot_msgbuf *m = msgbufp;
    unsigned n = 0;
    if (m && m->msg_bufc && m->msg_size > 0) {
        int size = m->msg_size, x = m->msg_bufx;
        if (x < 0 || x >= size) x = 0;
        int take = size < int(kTail) ? size : int(kTail);
        for (int i = size - take; i < size; ++i) {
            char c = m->msg_bufc[(x + i) % size];
            if (c) gTail[n++] = c;
        }
    }
    gTail[n] = 0;
    const unsigned capacity = kRows - kLogFirst;
    unsigned rows = 0;
    // Walk the lines from the newest back; each one becomes ceil(length / cols) rows, filled from the bottom.
    unsigned end = n;
    while (end && rows < capacity) {
        unsigned start = end;
        while (start && gTail[start - 1] != '\n') --start;
        unsigned length = end - start;
        if (length) {
            unsigned pieces = (length + cols - 1) / cols;
            for (unsigned p = pieces; p-- && rows < capacity;) {
                unsigned from = start + p * cols, count = length - p * cols < cols ? length - p * cols : cols;
                char *row = gLines[capacity - 1 - rows];
                for (unsigned i = 0; i < count; ++i) row[i] = gTail[from + i] == '\t' ? ' ' : gTail[from + i];
                row[count] = 0;
                ++rows;
            }
        }
        end = start ? start - 1 : 0;
    }
    // Move the filled rows to the top of the array.
    if (rows < capacity) memmove(gLines, gLines + (capacity - rows), rows * sizeof gLines[0]);
    return rows;
}

void release() {
    if (gMap) gMap->release();
    if (gDisplay) IOFree(gDisplay, sizeof *gDisplay);
    if (gTail) IOFree(gTail, kTail + 1);
    if (gLines) IOFree(gLines, (kRows - kLogFirst) * sizeof gLines[0]);
    gMap = nullptr; gDisplay = nullptr; gTail = nullptr; gLines = nullptr;
}

void run(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "aston-boot-screen");
    bp_clear(gDisplay);
    fill(0, 0, 2, 2, kSentinel);
    const unsigned cols = gDisplay->width / (6 * kScale) < BP_COLS - 1 ? gDisplay->width / (6 * kScale) : BP_COLS - 1;
    char row[BP_COLS];
    int lastWrite = -1;
    unsigned lastSecond = ~0u;
    for (unsigned step = 0;; ++step) {
        int drew = iosDrew();
        if (drew) {aston_boot_screen_stats[0] = drew; gDisplay->yield_requested = 1; break;}
        unsigned now = seconds();
        if (now > kLimitSeconds) {aston_boot_screen_stats[0] = 4; break;}
        drawBar(step);
        if (now != lastSecond) {
            lastSecond = now;
            bp_line(gDisplay, 1, "  UniXNU  OnePlus Ace 3", kTitle);
            snprintf(row, sizeof row, "  Booting   %u:%02u", now / 60, now % 60);
            bp_line(gDisplay, 2, row, kTitle);
            snprintf(row, sizeof row, "  %s", stage());
            bp_line(gDisplay, 4, row, kStage);
            aston_boot_screen_stats[2] = int32_t(now);
        }
        // The log every 500 ms, when the buffer moved.
        aston_boot_msgbuf *m = msgbufp;
        int write = m ? m->msg_bufx : -1;
        if (step % 5 == 0 && write != lastWrite) {
            lastWrite = write;
            unsigned rows = tailLines(cols);
            for (unsigned r = 0; r < kRows - kLogFirst; ++r) bp_line(gDisplay, kLogFirst + r, r < rows ? gLines[r] : "", kLog);
            aston_boot_screen_stats[1] = aston_boot_screen_stats[1] + 1;
        }
        IOSleep(100);
    }
    release();
}
}  // namespace

extern "C" void aston_boot_screen_probe(void) {
    unsigned enabled = 1;
    PE_parse_boot_argn("aston-boot-screen", &enabled, sizeof enabled);
    if (!enabled || aston_boot_screen_stats[0]) return;
    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr || video.v_depth != 32 || video.v_rotate || video.v_width < 720 ||
        video.v_width > 4096 || video.v_height < 720 || video.v_height > 4096 || video.v_rowBytes < video.v_width * 4 ||
        video.v_rowBytes > 65536) {
        aston_boot_screen_stats[0] = -1;
        return;
    }
    gDisplay = static_cast<bp_display *>(IOMallocZero(sizeof *gDisplay));
    gTail = static_cast<char *>(IOMalloc(kTail + 1));
    gLines = static_cast<char (*)[BP_COLS]>(IOMallocZero((kRows - kLogFirst) * sizeof gLines[0]));
    IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, uint64_t(video.v_rowBytes) * video.v_height);
    gMap = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    if (memory) memory->release();
    if (!gDisplay || !gTail || !gLines || !gMap) {
        aston_boot_screen_stats[0] = gMap ? -2 : -1;
        release();
        return;
    }
    gDisplay->frame = uintptr_t(gMap->getVirtualAddress());
    gDisplay->width = unsigned(video.v_width);
    gDisplay->height = unsigned(video.v_height);
    gDisplay->stride = unsigned(video.v_rowBytes);
    gDisplay->scale = kScale;
    memset(gDisplay->previous, 0xff, sizeof gDisplay->previous);
    for (unsigned i = 0; i < kRows; ++i) gDisplay->previous[i][BP_COLS - 1] = 0;
    gDisplay->bound = 1;
    gBarX = 36;
    gBarWidth = gDisplay->width - 72;
    gBarY = 3 * 14 * kScale + 12;
    gStart = mach_absolute_time();
    aston_boot_screen_stats[0] = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(run, nullptr, &thread) != KERN_SUCCESS) {
        aston_boot_screen_stats[0] = -3;
        release();
        return;
    }
    thread_deallocate(thread);
}
