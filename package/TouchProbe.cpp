// SPDX-License-Identifier: BSD-2-Clause-Patent
// OnePlus Ace 3 (aston): default-off staged bring-up of the Synaptics S3910 touch controller on QUP1 SE4. `sheng-touch=<n>`:
//   1  read-only snapshot of the GCC branches, the S4 DFS levels and the touch pins
//   2  vote the QUP clocks, wait for them, configure the SPI pins, read the GENI engine identity
//   3  GENI SPI init and clock; power (ldob4 through RPMh, AVDD on GPIO 10) and reset (GPIO 24) the controller; identify it and
//      select on-controller (LBP) contact reports (S3910.c, the TouchComm v1/v2 implementation of the OnePlus 13T port)
//   4  report loop: the ATTN line (GPIO 25, active low) wakes the thread, contacts go to ShengHidInput through native_touch_*
// The transport (clock votes, pins, SE DMA behind an SMMU bypass, the TLMM summary interrupt) is the reference board's: its
// Novatek controller sits on the same serial engine and pins. The controller firmware stays in the S3910's flash; nothing is
// downloaded or erased. Every stage reports on the diagnostic page; the first failure stops the sequence. Nothing here is
// validated on the device until a boot has shown it. Board constants and their sources are in TouchBoard.h.
//
// Other arguments: sheng-touch-hz (SPI clock), sheng-touch-flip (bit 0 mirror X, bit 1 mirror Y, bit 2 swap the axes),
// sheng-touch-poll=1 (sample ATTN every 8 ms instead of waiting for its interrupt), sheng-touch-delay=<SE_SPI_DELAY_COUNTERS>,
// sheng-touch-rail=0 (no RPMh request for ldob4: the rail is assumed on).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>
#include "AppsSmmuLock.h"
extern "C" {
#include "GeniSpi.h"
#include "S3910.h"
#include "QcomRpmh.h"
#include "TouchBoard.h"
#include "TouchTrace.h"
#include "SmmuBypass.h"
}

// The finger arrays keep the reference board's size: ShengHidInput and BootProgress declare them with 12 entries.
#define TOUCH_SLOTS 12

extern "C" {
char native_touch_rows[4][120];
char native_touch_trace[8][120];
volatile uint32_t native_touch_loop_step;
volatile uint32_t native_touch_stage_req, native_touch_stage_done, native_touch_step, native_touch_frames,
    native_touch_valid, native_touch_contacts, native_touch_hid_reports;
volatile int native_touch_error;
volatile int native_touch_last_x = -1, native_touch_last_y = -1;
// Latest finger set for the HID digitizer source (see NativeHidInput); sequence is odd while being written.
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile int native_touch_xs[TOUCH_SLOTS], native_touch_ys[TOUCH_SLOTS], native_touch_ids[TOUCH_SLOTS];
// Finished strokes as published: a ring of {start ms, duration ms, first x, first y, last x, last y, frames} in published units,
// and the extent every published contact reached {min x, max x, min y, max y}. Read by kits/sheng BootLog.c.
volatile int32_t native_touch_strokes[16][7];
volatile uint32_t native_touch_stroke_count;
volatile int32_t native_touch_extent[4] = {0x7fffffff, -1, 0x7fffffff, -1};
// Counters the reference Novatek core filled; BootLog.c reads them, the S3910 path leaves them zero.
volatile uint32_t native_touch_jitter[3];
volatile uint32_t native_touch_hold[14];
volatile uint32_t native_swipe_stats[16];
// [0] 0 not armed, 1 armed, 2 attach failed, [1] interrupts, [2] spurious, [3] attach status
volatile uint32_t native_touch_irq_stats[4];
// S3910 loop: [0] reads, [1] contact reports, [2] other reports, [3] restarts recovered, [4] transport/protocol failures,
// [5] last failure code, [6] ATTN wait ms at start, [7] reported maxima x << 16 | y
volatile uint32_t native_touch_s3910[8];
}

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// The serial engine's DMA master reads and writes two 64 KiB buffers through the same pass-through mapping the UFS driver uses.
// Caches are maintained explicitly.
constexpr size_t kDmaBytes = 64 * 1024;
// One S3910 packet: at most S3910_PAYLOAD_MAX + 7 bytes read (v1 body with its trailer) or S3910_PAYLOAD_MAX + 6 written.
constexpr size_t kBufBytes = 1024;
void cacheRange(void *p, size_t len, int op) {   // 0 clean, 1 clean+invalidate, 2 invalidate
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1), end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
void dmaClean(void *, void *p, size_t len) {cacheRange(p, len, 0);}
void dmaCleanInval(void *, void *p, size_t len) {cacheRange(p, len, 1);}
void dmaInval(void *, void *p, size_t len) {cacheRange(p, len, 2);}
uint32_t smmuRd(void *ctx, unsigned o) {return rd32(reinterpret_cast<uintptr_t>(ctx), o);}
void smmuWr(void *ctx, unsigned o, uint32_t v) {wr32(reinterpret_cast<uintptr_t>(ctx), o, v);}
int smmuErrorCode(const char *e) {
    static const char *const names[] = {"SMMU AArch64 stage 1 unsupported", "DMA exceeds SMMU address width", "unsupported SMMU geometry",
        "conflicting stream mapping", "reserved context bank is not a bypass bank", "no free stream-match group",
        "SMMU bypass bank readback", "SMMU stream context readback", "SMMU stream mapping readback"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) if (strcmp(e, names[i]) == 0) return static_cast<int>(i) + 1;
    return 99;
}

uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
// Short status polls spin on the counter instead of blocking the thread (the reference board's measurement).
void ioDelayUs(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}

struct Session {
    Region gccLow, gccS4, gccHalt, gccVote, tlmmIrq, tlmmSpi, tlmmReset, tlmmAvdd, wrapper, se, smmu, rsc;
    IOBufferMemoryDescriptor *dmaMem;
    uint64_t dmaPhys;
    int smmuSlot, smmuBank, smmuErr;
    // Last six transfers (ring), for the diagnostic page.
    unsigned ringN;
    uint32_t ringLen[6];
    uint8_t ringTx[6][8], ringRx[6][8];
    int loopRc = 99;                  // engine loopback self-test (99 = not run)
    uint8_t loopRx[8];
    struct geni_spi spi;
    uint8_t *tx = nullptr, *rx = nullptr;
    struct s3910 tc;
    uint32_t vals[40];
    int lastRc = 0;
    uint32_t lastLen = 0, xIrq = 0, xGeni = 0, xTx = 0, xRx = 0;
    uint64_t levelHz[TB_DFS_LEVELS];
    unsigned level = 0, div = 0;
    uint64_t actualHz = 0;
    // Rail request: the resource address, the RSC DRV id, the result and the polls until the TCS was idle again.
    uint32_t vddAddr = 0, rscId = 0, railPolls = 0;
    int railRc = 99;
    uint32_t delayCounters = 0;
};

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    int rc = geni_spi_transfer(&s->spi, tx, rx, len, 0);
    unsigned i = s->ringN++ % 6;
    s->ringLen[i] = static_cast<uint32_t>(len);
    for (unsigned k = 0; k < 8; ++k) {
        s->ringTx[i][k] = (tx && k < len) ? tx[k] : 0;
        s->ringRx[i][k] = (rx && k < len && !rc) ? rx[k] : 0;
    }
    if (rc && !s->lastRc) {   // keep the engine state of the first failure, as read before the command was cancelled
        s->lastRc = rc;
        s->lastLen = static_cast<uint32_t>(len);
        s->xIrq = s->spi.snap_irq;
        s->xGeni = s->spi.snap_status;
        s->xTx = s->spi.snap_txfs;
        s->xRx = s->spi.snap_rxfs;
    }
    return rc;
}

// The S3910 bus: one full-duplex transfer per call with chip select held throughout. Reads clock out 0xff, as the vendor driver does.
int tcRead(void *ctx, uint8_t *buf, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    if (!len || len > kBufBytes) return -90;
    memset(s->tx, 0xff, len);
    const int rc = busXfer(s, s->tx, s->rx, len);
    if (!rc) memcpy(buf, s->rx, len);
    return rc;
}
int tcWrite(void *ctx, const uint8_t *buf, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    if (!len || len > kBufBytes) return -90;
    memcpy(s->tx, buf, len);
    return busXfer(s, s->tx, s->rx, len);
}
void tcSleep(void *, unsigned ms) {IOSleep(ms);}

void fail(unsigned step, int code) {
    if (!native_touch_error) {native_touch_step = step; native_touch_error = code ? code : -1;}
}

void rows(const Session &s);
void mark(const Session &s, unsigned step) {native_touch_step = step; rows(s);}

// snprintf returns the length it would have written, so a running offset must be clamped before the next append.
struct Out {
    char *p; size_t cap, n;
    void add(const char *fmt, unsigned a = 0, unsigned b = 0) {
        if (n + 1 >= cap) return;
        int w = snprintf(p + n, cap - n, fmt, a, b);
        if (w < 0) return;
        n = (n + static_cast<size_t>(w) >= cap) ? cap - 1 : n + static_cast<size_t>(w);
    }
    void hex(uint8_t v) {add("%02x", v);}
};

void traceRows(const Session &s) {
    Out l = {native_touch_trace[0], sizeof native_touch_trace[0], 0};
    l.p[0] = 0;
    l.add("TCH loop rc%d rx", static_cast<unsigned>(s.loopRc));
    for (unsigned k = 0; k < 8; ++k) l.hex(s.loopRx[k]);
    l.add(" reads%u reports%u", native_touch_s3910[0], native_touch_s3910[1]);
    l.add(" other%u restart%u", native_touch_s3910[2], native_touch_s3910[3]);
    l.add(" fail%u last%d", native_touch_s3910[4], native_touch_s3910[5]);
    for (unsigned r = 0; r < 2; ++r) {
        Out o = {native_touch_trace[1 + r], sizeof native_touch_trace[1 + r], 0};
        o.p[0] = 0;
        o.add("TCH last");
        for (unsigned j = 0; j < 2; ++j) {
            unsigned k = r * 2 + j;                       // last four, oldest first
            if (s.ringN < 4 && k >= s.ringN) break;
            unsigned seq = s.ringN >= 4 ? s.ringN - 4 + k : k;
            unsigned i = seq % 6;
            o.add(" #%u len%u tx", seq, s.ringLen[i]);
            for (unsigned q = 0; q < 8 && q < s.ringLen[i]; ++q) o.hex(s.ringTx[i][q]);
            o.add(" rx");
            for (unsigned q = 0; q < 8 && q < s.ringLen[i]; ++q) o.hex(s.ringRx[i][q]);
            o.add(" |");
        }
    }
    for (unsigned r = 3; r < 8; ++r) native_touch_trace[r][0] = 0;
}

void rows(const Session &s) {
    traceRows(s);
    const uint32_t *v = s.vals;
    snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
        "TCH s%u/%u st%u e%d vote%08x h%08x %08x %08x %08x %08x gp%08x dfs%08x",
        native_touch_stage_done, native_touch_stage_req, native_touch_step, native_touch_error,
        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1],
        "TCH p%08x %08x %08x %08x attn%08x rst%08x avdd%08x fw%08x if%08x L%u/%u %uHz",
        v[9], v[10], v[11], v[12], v[13], v[20], v[21], v[15], v[16], s.level, s.div, (unsigned)s.actualHz);
    snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
        "TCH S3910 v%u m%u %ux%u wr%u rd%u %s %08x c%02x/%02x fr%u c%u %d,%d hid%u",
        s.tc.version, s.tc.mode, s.tc.max_x, s.tc.max_y, s.tc.max_write, s.tc.max_read, s.tc.stage ? s.tc.stage : "-",
        s.tc.detail, s.tc.command, s.tc.code, native_touch_frames, native_touch_contacts, native_touch_last_x, native_touch_last_y,
        native_touch_hid_reports);
    snprintf(native_touch_rows[3], sizeof native_touch_rows[3],
        "TCH xf rc%d n%u ev%08x gs%08x tx%08x rx%08x sm%d/%d/e%d pa%llx vdd%d a%05x drv%08x p%u dly%x",
        s.lastRc, s.lastLen, s.xIrq, s.xGeni, s.xTx, s.xRx, s.smmuSlot, s.smmuBank, s.smmuErr, (unsigned long long)s.dmaPhys,
        s.railRc, s.vddAddr, s.rscId, s.railPolls, s.delayCounters);
}

struct Stroke {int id, x0, y0, x1, y1, frames; uint64_t t0;};
Stroke strokes[TOUCH_SLOTS];
unsigned strokeCount;
uint32_t touchMs() {
    uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return (uint32_t)(mach_absolute_time() / ((f ? f : 19200000) / 1000));
}
void trackStrokes(unsigned count, const int *ids, const int *xs, const int *ys) {
    const uint32_t now = touchMs();
    for (unsigned i = 0; i < count; ++i) {
        if (xs[i] < native_touch_extent[0]) native_touch_extent[0] = xs[i];
        if (xs[i] > native_touch_extent[1]) native_touch_extent[1] = xs[i];
        if (ys[i] < native_touch_extent[2]) native_touch_extent[2] = ys[i];
        if (ys[i] > native_touch_extent[3]) native_touch_extent[3] = ys[i];
        unsigned k = 0;
        while (k < strokeCount && strokes[k].id != ids[i]) ++k;
        if (k == strokeCount) {
            if (strokeCount == TOUCH_SLOTS) continue;
            strokes[strokeCount++] = {ids[i], xs[i], ys[i], xs[i], ys[i], 0, now};
        }
        strokes[k].x1 = xs[i]; strokes[k].y1 = ys[i]; ++strokes[k].frames;
    }
    for (unsigned k = 0; k < strokeCount;) {
        bool live = false;
        for (unsigned i = 0; i < count && !live; ++i) live = ids[i] == strokes[k].id;
        if (live) {++k; continue;}
        volatile int32_t *r = native_touch_strokes[native_touch_stroke_count & 15];
        r[0] = (int32_t)(uint32_t)strokes[k].t0; r[1] = (int32_t)(now - (uint32_t)strokes[k].t0);
        r[2] = strokes[k].x0; r[3] = strokes[k].y0; r[4] = strokes[k].x1; r[5] = strokes[k].y1; r[6] = strokes[k].frames;
        native_touch_stroke_count = native_touch_stroke_count + 1;
        strokes[k] = strokes[--strokeCount];
    }
}

// Whoever consumes the finger set (NativeHidInput) registers a function here; it is called from the thread that published, after the
// seqlock closes.
static void (*gTouchSink)(void *);
static void *gTouchSinkContext;
unsigned gFlip;
// Publishes the active contacts of one S3910 frame. S3910.c normalises each axis to 0..32767 over the controller's own maxima; X is
// the panel's short axis, which is also the framebuffer's width (1264 x 2780 portrait), so no rotation is needed. The values are
// rescaled to the axis maxima ShengHidInput normalises against (TB_HID_X_MAX x TB_HID_Y_MAX). Contact IDs 0..9 become 1..10.
void publish(const struct s3910_frame &frame) {
    int ids[TOUCH_SLOTS], xs[TOUCH_SLOTS], ys[TOUCH_SLOTS];
    unsigned count = 0;
    for (unsigned id = 0; id < S3910_CONTACTS && count < TOUCH_SLOTS; ++id) {
        const struct s3910_contact &c = frame.contact[id];
        if (!c.active) continue;
        int x = c.x, y = c.y;
        if (gFlip & 4) {const int t = x; x = y; y = t;}
        if (gFlip & 1) x = 32767 - x;
        if (gFlip & 2) y = 32767 - y;
        ids[count] = static_cast<int>(id) + 1;
        xs[count] = static_cast<int>((int64_t)x * (TB_HID_X_MAX - 1) / 32767);
        ys[count] = static_cast<int>((int64_t)y * (TB_HID_Y_MAX - 1) / 32767);
        ++count;
    }
    trackStrokes(count, ids, xs, ys);
    native_touch_seq = native_touch_seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count; ++i) {native_touch_xs[i] = xs[i]; native_touch_ys[i] = ys[i]; native_touch_ids[i] = ids[i];}
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    native_touch_contacts = count;
    if (count) {native_touch_last_x = xs[0]; native_touch_last_y = ys[0];}
    if (void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE)) sink(gTouchSinkContext);
}
// Lifts every contact (transport failure, controller restart): the HID layer sends tip-up for each identifier that vanished.
void releaseAll() {
    struct s3910_frame none;
    memset(&none, 0, sizeof none);
    publish(none);
}

bool snapshot(Session &s) {
    mark(s, 1);
    if (!mapRegion(TB_GCC_BASE + 0x0000, 0x1000, s.gccLow) || !mapRegion(TB_GCC_BASE + 0x18000, 0x1000, s.gccS4) ||
        !mapRegion(TB_GCC_BASE + 0x23000, 0x1000, s.gccHalt) || !mapRegion(TB_GCC_BASE + 0x52000, 0x1000, s.gccVote) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_IRQ), 0x1000, s.tlmmIrq) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_RESET), 0x1000, s.tlmmReset) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_AVDD), 0x1000, s.tlmmAvdd) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_SPI_MISO), 4 * TB_GPIO_STRIDE, s.tlmmSpi)) {
        fail(1, -12);
        return false;
    }
    uint32_t *v = s.vals;
    v[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    v[1] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    v[2] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    v[3] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    v[4] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    v[5] = rd32(s.gccS4.va, TB_HALT_S4 & 0xfff);
    v[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    v[7] = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_DFSR_CMD) & 0xfff);
    v[8] = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_CFG) & 0xfff);
    for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
        uint32_t cfg = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_CFG(l)) & 0xfff);
        uint32_t m = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_M(l)) & 0xfff);
        uint32_t n = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_N(l)) & 0xfff);
        s.levelHz[l] = tb_dfs_level_hz(cfg, m, n);
        if (l < 4) s.vals[24 + l] = cfg;
    }
    for (unsigned i = 0; i < 4; ++i) v[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    v[13] = rd32(s.tlmmIrq.va, 4) | rd32(s.tlmmIrq.va, 0) << 8;      // ATTN level, then its CFG
    v[20] = rd32(s.tlmmReset.va, 4) | rd32(s.tlmmReset.va, 0) << 8;  // RESET IN_OUT (bit 0 in, bit 1 out), then its CFG
    v[21] = rd32(s.tlmmAvdd.va, 4) | rd32(s.tlmmAvdd.va, 0) << 8;    // AVDD enable
    return true;
}

bool enable(Session &s) {
    mark(s, 2);
    // GPLL0 feeds the DFS levels; vote for it (a shared vote, not an exclusive enable) and wait for lock.
    wr32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    s.vals[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff) | TB_VOTE_ALL);
    // Branches with hardware clock gating (bit 1) show CLK_OFF while idle and wake on access, so only a branch without that bit must
    // have CLK_OFF clear.
    auto running = [](uint32_t cbcr) {return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = running(rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff)) && running(rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccS4.va, TB_HALT_S4 & 0xfff) & TB_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    s.vals[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[1] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    s.vals[2] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    s.vals[3] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    s.vals[4] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    s.vals[5] = rd32(s.gccS4.va, TB_HALT_S4 & 0xfff);
    if (!ok) {fail(2, -110); return false;}   // never touch the engine without running clocks
    const uint32_t cfg = tb_pin_cfg(TB_FUNC_QUP1_SE4, TB_DRIVE_6MA);
    for (unsigned i = 0; i < 4; ++i) wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, cfg);
    for (unsigned i = 0; i < 4; ++i) s.vals[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    mark(s, 3);   // the next accesses touch the engine: if the page freezes here, the engine access hung
    if (!mapRegion(TB_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) {fail(3, -12); return false;}
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    mark(s, 31);   // first engine register read
    int r = geni_spi_read_info(&s.spi);
    s.vals[14] = s.spi.info.qup_hw_version;
    s.vals[15] = s.spi.info.fw_revision;
    s.vals[16] = s.spi.info.if_disable;
    s.vals[17] = s.spi.info.hw_param0;
    if (r) {fail(3, r); return false;}
    return true;
}

bool prepareDma(Session &s) {
    mark(s, 41);
    s.dmaMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
                                                               2 * kDmaBytes, 0xfffffff000ULL);
    if (!s.dmaMem || s.dmaMem->prepare() != kIOReturnSuccess) {fail(41, -12); return false;}
    mark(s, 42);
    addr64_t seg_len = 0;
    s.dmaPhys = s.dmaMem->getPhysicalSegment(0, &seg_len);
    uint8_t *virt = static_cast<uint8_t *>(s.dmaMem->getBytesNoCopy());
    if (!virt || !s.dmaPhys || seg_len < 2 * kDmaBytes) {fail(42, -14); return false;}
    // Cached allocation (an uncached mapping of RAM above 4 GiB panics in phystokv_range), kept coherent by cache maintenance.
    memset(virt, 0, 2 * kDmaBytes);
    cacheRange(virt, 2 * kDmaBytes, 1);
    if (!mapRegion(TB_APPS_SMMU_BASE, TB_APPS_SMMU_SIZE, s.smmu)) {fail(43, -12); return false;}
    mark(s, 43);
    AppsSmmuLock smmuGuard;
    if (!smmuGuard.ready()) {fail(43, -12); return false;}
    struct smmu_io io = {reinterpret_cast<void *>(s.smmu.va), smmuRd, smmuWr};
    struct smmu_plan plan = smmu_plan_bypass(&io, TB_QUP_SMMU_SID, s.dmaPhys, 2 * kDmaBytes);
    s.smmuSlot = static_cast<int>(plan.slot); s.smmuBank = static_cast<int>(plan.bank);
    if (plan.error) {s.smmuErr = smmuErrorCode(plan.error); fail(43, -100 - s.smmuErr); return false;}
    const char *e = smmu_configure_bypass(&io, &plan, TB_QUP_SMMU_SID);
    if (e) {s.smmuErr = smmuErrorCode(e); fail(44, -100 - s.smmuErr); return false;}
    s.spi.use_dma = 1;
    s.spi.dma.ctx = nullptr;
    s.spi.dma.clean = dmaClean;
    s.spi.dma.clean_inval = dmaCleanInval;
    s.spi.dma.inval = dmaInval;
    s.spi.dma.tx_virt = virt;
    s.spi.dma.rx_virt = virt + kDmaBytes;
    s.spi.dma.tx_phys = s.dmaPhys;
    s.spi.dma.rx_phys = s.dmaPhys + kDmaBytes;
    s.spi.dma.size = kDmaBytes;
    return true;
}

bool openBus(Session &s) {
    mark(s, 4);
    s.spi.poll_limit = 400000;
    s.spi.poll_only = 2;             // keep the top-level IRQ enable: the status registers must stay visible to the poll
    // On SM8550 the firmware leaves FIFO_IF_DISABLE set, so the SE DMA engine is the host-visible path (reference board v53-v55).
    if (s.spi.info.fifo_disabled && !prepareDma(s)) return false;
    int r = geni_spi_init(&s.spi);
    if (r) {fail(4, r); return false;}
    return true;
}

bool configureClock(Session &s, unsigned hz) {
    struct tb_clock_choice choice = {};
    if (tb_choose_clock(s.levelHz, TB_DFS_LEVELS, s.spi.oversampling, hz, &choice)) {fail(5, -22); return false;}
    s.level = choice.level;
    s.div = choice.div;
    s.actualHz = choice.actual_hz;
    int r = geni_spi_set_clock(&s.spi, hz, static_cast<unsigned>(choice.sclk_hz), choice.level);
    if (r) {fail(5, r); return false;}
    r = geni_spi_setup(&s.spi, TB_SPI_MODE, 0);
    if (r) {fail(5, r); return false;}
    unsigned delay = 0;
    if (PE_parse_boot_argn("sheng-touch-delay", &delay, sizeof delay)) ioWr(s.spi.se.ctx, TB_SPI_DELAY_COUNTERS, delay & 0xfffff);
    s.delayCounters = ioRd(s.spi.se.ctx, TB_SPI_DELAY_COUNTERS);
    return true;
}

// ---- power: the vdd rail through RPMh, AVDD and reset through TLMM ----
// One active request of three VRM commands (voltage, mode, enable) in TCS 2 of the apps RSC DRV 2. The TCS must be idle and unused;
// completion is read from its status register (1 = idle), so the shared RSC interrupt stays as AdspProbe left it.
int railOn(Session &s) {
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(TB_CMD_DB_SIZE));
    if (!db) return -12;
    Region window;
    if (!mapRegion(TB_CMD_DB, TB_CMD_DB_SIZE, window)) {IOFreeData(db, TB_CMD_DB_SIZE); return -12;}
    for (uint32_t i = 0; i < TB_CMD_DB_SIZE; i += 4) {const uint32_t w = rd32(window.va, i); memcpy(db + i, &w, 4);}
    window.keep.reset();
    QcomRpmhResource vdd = {};
    const int found = qcom_cmd_db_find(db, TB_CMD_DB_SIZE, TB_VDD_RESOURCE, &vdd);
    IOFreeData(db, TB_CMD_DB_SIZE);
    if (found) return -2;
    s.vddAddr = vdd.address;
    if (!mapRegion(TB_RSC_DRV2, 0x10000, s.rsc)) return -12;
    s.rscId = rd32(s.rsc.va, 0);
    const QcomRscLayout layout = qcom_rsc_layout(s.rscId);
    const uint32_t tcs = TB_RSC_TCS + layout.tcs_stride * TB_RSC_TOUCH_TCS;
    if (!rd32(s.rsc.va, tcs + layout.status) || rd32(s.rsc.va, tcs + layout.control) || rd32(s.rsc.va, tcs + layout.cmd_enable))
        return -16;   // in use: never take another owner's TCS
    const uint32_t addresses[3] = {vdd.address + TB_VRM_VOLTAGE, vdd.address + TB_VRM_MODE, vdd.address + TB_VRM_ENABLE};
    const uint32_t values[3] = {TB_VDD_MV, TB_VRM_MODE_HPM, 1};
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&layout, TB_RSC_TOUCH_TCS, addresses, values, 3, writes, 16);
    if (!n) return -22;
    for (unsigned i = 0; i < n; ++i) wr32(s.rsc.va, TB_RSC_TCS + writes[i].offset, writes[i].value);
    IODelay(100);
    bool idle = false;
    for (unsigned i = 0; i < 1000 && !idle; ++i) {
        idle = rd32(s.rsc.va, tcs + layout.status) != 0;
        s.railPolls = i;
        if (!idle) IODelay(100);
    }
    const unsigned m = qcom_rpmh_finish(&layout, TB_RSC_TOUCH_TCS, writes, 16);
    for (unsigned i = 0; i < m; ++i) wr32(s.rsc.va, TB_RSC_TCS + writes[i].offset, writes[i].value);
    return idle ? 0 : -110;
}

void pinOut(const Region &pin, unsigned high) {
    wr32(pin.va, 4, high ? 2u : 0u);                       // IN_OUT bit 1 first, so the pin never glitches
    wr32(pin.va, 0, tb_gpio_cfg(0, TB_DRIVE_8MA, 1));      // GPIO function, no pull, output
}

// The vendor sequence (synaptics_tcm hbp power and reset): vdd, AVDD high, 200 ms, reset low 10 ms, reset high, 80 ms.
bool powerUp(Session &s) {
    mark(s, 6);
    wr32(s.tlmmIrq.va, 0, tb_gpio_cfg(3, TB_DRIVE_8MA, 0));   // ATTN: GPIO input with a pull-up (ts_int_active)
    pinOut(s.tlmmReset, 0);                                     // hold the controller in reset while its supplies come up
    unsigned rail = 1;
    PE_parse_boot_argn("sheng-touch-rail", &rail, sizeof rail);
    s.railRc = rail ? railOn(s) : 0;
    if (s.railRc) {fail(6, s.railRc); return false;}
    pinOut(s.tlmmAvdd, 1);
    IOSleep(TB_POWER_DELAY_MS);
    IOSleep(TB_RESET_ACTIVE_MS);
    pinOut(s.tlmmReset, 1);
    IOSleep(TB_RESET_DELAY_MS);
    s.vals[13] = rd32(s.tlmmIrq.va, 4) | rd32(s.tlmmIrq.va, 0) << 8;
    s.vals[20] = rd32(s.tlmmReset.va, 4) | rd32(s.tlmmReset.va, 0) << 8;
    s.vals[21] = rd32(s.tlmmAvdd.va, 4) | rd32(s.tlmmAvdd.va, 0) << 8;
    // After reset the controller asserts ATTN with its identify report.
    unsigned waited = 0;
    while ((rd32(s.tlmmIrq.va, 4) & 1) && waited < 1000) {IOSleep(5); waited += 5;}
    native_touch_s3910[6] = waited;
    return true;
}

// Identify the controller and select LBP contact reports; one controller reset and a second attempt on failure.
bool startController(Session &s) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        mark(s, 7);
        s.tc.bus = (struct s3910_bus){&s, tcRead, tcWrite, tcSleep};
        const int rc = s3910_start(&s.tc);
        native_touch_s3910[7] = (uint32_t)s.tc.max_x << 16 | s.tc.max_y;
        if (!rc) return true;
        native_touch_s3910[5] = (uint32_t)rc;
        if (attempt) {fail(7, rc); return false;}
        pinOut(s.tlmmReset, 0);
        IOSleep(TB_RESET_ACTIVE_MS);
        pinOut(s.tlmmReset, 1);
        IOSleep(TB_RESET_DELAY_MS);
    }
    return false;
}
}  // namespace

// The ATTN interrupt (GPIO 25): registers per pin at TLMM + 0x1000 * pin as pinctrl-msm.c defines them: +0x4 IN_OUT (bit 0 the level),
// +0x8 INTR_CFG (bit 0 enable, bit 1 polarity, bits 3:2 detection 0 level / 1 rising / 2 falling / 3 both, bit 4 raw status enable,
// bits 7:5 target, 3 = the application processor), +0xc INTR_STATUS (bit 0; acknowledged by writing 0). All TLMM pin interrupts share
// the summary SPI 208 (INTID 240). The S3910 drives ATTN low while a report is pending (irq-on-state 0), so the edge is the falling one.
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" int armcpu_spi_share(uint32_t, void (*)(void *), void *) __attribute__((weak_import));
static volatile uint32_t gTouchIrqEvents;
static volatile uint32_t *gTouchPin;

extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {gTouchSinkContext = context; __atomic_store_n(&gTouchSink, sink, __ATOMIC_RELEASE);}
// The reference board's path trace (sheng-tlog) records Novatek tracking state; the S3910 path has none.
extern "C" const touch_trace_row *sheng_touch_trace_freeze(uint32_t *total) {if (total) *total = 0; return nullptr;}

static void touchIrq(void *) {
    if (!(gTouchPin[0xc / 4] & 1)) {native_touch_irq_stats[2] = native_touch_irq_stats[2] + 1; return;}
    gTouchPin[0xc / 4] = 0;
    native_touch_irq_stats[1] = native_touch_irq_stats[1] + 1;
    __atomic_fetch_add(&gTouchIrqEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gTouchIrqEvents);
}
static bool armTouchIrq(Session &s) {
    gTouchPin = reinterpret_cast<volatile uint32_t *>(s.tlmmIrq.va);
    uint32_t cfg = gTouchPin[0x8 / 4];
    cfg &= ~((7u << 5) | (3u << 2) | (1u << 1) | 1u);
    cfg |= (3u << 5) | (2u << 2) | (1u << 1) | (1u << 4);   // application processor, falling edge, raw status on
    gTouchPin[0x8 / 4] = cfg;
    gTouchPin[0xc / 4] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (!armcpu_spi_share && !armcpu_spi_attach) {native_touch_irq_stats[0] = 2; native_touch_irq_stats[3] = 0xffff; return false;}
    const int rc = armcpu_spi_share ? armcpu_spi_share(32 + 208, touchIrq, nullptr) :
        armcpu_spi_attach(32 + 208, false, touchIrq, nullptr, nullptr, nullptr);
    native_touch_irq_stats[3] = (uint32_t)rc;
    if (rc) {native_touch_irq_stats[0] = 2; return false;}
    gTouchPin[0x8 / 4] = cfg | 1u;   // enable last
    native_touch_irq_stats[0] = 1;
    return true;
}

static void touchThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-touch");
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, -12); return;}
    bzero(s, sizeof *s);
    s->loopRc = 99;
    s->railRc = 99;
    unsigned stage = native_touch_stage_req, hz = TB_SPI_DEFAULT_HZ;
    PE_parse_boot_argn("sheng-touch-hz", &hz, sizeof hz);
    if (hz < 100000 || hz > TB_SPI_MAX_HZ) hz = TB_SPI_DEFAULT_HZ;
    PE_parse_boot_argn("sheng-touch-flip", &gFlip, sizeof gFlip);
    do {
        if (!snapshot(*s)) break;
        native_touch_stage_done = 1;
        rows(*s);
        if (stage < 2) break;
        if (!enable(*s)) break;
        native_touch_stage_done = 2;
        rows(*s);
        if (stage < 3) break;
        if (!openBus(*s) || !configureClock(*s, hz)) break;
        s->tx = static_cast<uint8_t *>(IOMalloc(kBufBytes));
        s->rx = static_cast<uint8_t *>(IOMalloc(kBufBytes));
        if (!s->tx || !s->rx) {fail(5, -12); break;}
        // Engine loopback: MOSI is fed back to MISO inside the controller, so this checks the DMA, packing and clock path alone.
        {
            static const uint8_t pattern[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
            geni_spi_setup(&s->spi, GENI_SPI_LOOP, 0);
            s->loopRc = geni_spi_transfer(&s->spi, pattern, s->rx, sizeof pattern, 0);
            for (unsigned k = 0; k < 8; ++k) s->loopRx[k] = s->rx[k];
            geni_spi_setup(&s->spi, TB_SPI_MODE, 0);
            s->ringN = 0;
            rows(*s);
        }
        if (!powerUp(*s) || !startController(*s)) {rows(*s); break;}
        native_touch_stage_done = 3;
        rows(*s);
        if (stage < 4) break;
        mark(*s, 11);
        unsigned pollOnly = 0;
        PE_parse_boot_argn("sheng-touch-poll", &pollOnly, sizeof pollOnly);
        const bool irqArmed = !pollOnly && armTouchIrq(*s);
        uint32_t seen = gTouchIrqEvents;
        bool down = false;
        unsigned rowTick = 0, failures = 0;
        for (;;) {
            native_touch_loop_step = 1;
            // Wait for ATTN; while a finger is down, wake every 8 ms anyway so a missed edge never holds a contact.
            if (irqArmed && !down) {
                assert_wait_timeout((event_t)&gTouchIrqEvents, THREAD_UNINT, 100, kMillisecondScale);
                if (gTouchIrqEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
                else thread_block(THREAD_CONTINUE_NULL);
            } else {
                IOSleep(8);
            }
            seen = gTouchIrqEvents;
            for (unsigned n = 0; n < 32 && !(rd32(s->tlmmIrq.va, 4) & 1); ++n) {   // every report pending now
                native_touch_loop_step = 2;
                struct s3910_frame frame;
                const int rc = s3910_poll(&s->tc, &frame);
                native_touch_s3910[0] = native_touch_s3910[0] + 1;
                if (rc == 1) {
                    native_touch_loop_step = 3;
                    native_touch_s3910[1] = native_touch_s3910[1] + 1;
                    native_touch_frames = native_touch_frames + 1;
                    native_touch_valid = native_touch_valid + 1;
                    publish(frame);
                    down = native_touch_count != 0;
                    failures = 0;
                    continue;
                }
                if (rc == 0) {native_touch_s3910[2] = native_touch_s3910[2] + 1; continue;}
                native_touch_s3910[5] = (uint32_t)rc;
                if (down) {releaseAll(); down = false;}
                if (rc == -104 && startController(*s)) {native_touch_s3910[3] = native_touch_s3910[3] + 1; break;}
                native_touch_s3910[4] = native_touch_s3910[4] + 1;
                ++failures;
                IOSleep(20);
                break;
            }
            // The controller reports a lift as a contact report without that contact, so a held contact needs no timeout here.
            if (failures >= 20) {fail(11, (int)native_touch_s3910[5]); break;}
            if ((++rowTick & 63) == 0) rows(*s);
        }
    } while (0);
    rows(*s);
}

extern "C" void native_touch_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("sheng-touch", &stage, sizeof stage) || stage < 1 || native_touch_stage_req) return;
    if (stage > 4) stage = 4;
    native_touch_stage_req = stage;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_touch_error = -12;
}
