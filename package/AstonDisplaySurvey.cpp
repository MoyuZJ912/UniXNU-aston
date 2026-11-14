// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the boot chain left running on the Ace 3's display path, read once and kept for the boot log (boot argument aston-dpu=1): the
// starting point of a display driver of this board's own. The panel (AA551 P 3 A0004, DSC command mode, 1264x2780, 10 bits per
// component, DSC 1.2 with two encoders of 632x20 slices at 8 bits per pixel, TE on its pin, 120/90/60 Hz) sits on DSI0 behind the
// Iris7P bridge; Qualcomm's boot loader lit it and its DPU keeps sending the splash buffer by autorefresh.
// Addresses: the phone's devicetree (qcom,mdss_mdp@ae00000: qcom,sde-off 0x1000, CTL 0x16000 + 0x1000 n, pipes VIG 0x5000.., DMA
// 0x25000.., mixers 0x45000.., DSC 0x81000 with encoders at +0x100/+0x200 and controls at +0xf00/+0xf80, merge 3D 0x4f000..,
// interfaces 0x35000.. (INTF_1 0x36000 drives DSI0, its tear interrupts at 0x36800), MDSS interrupt SPI 83; qcom,mdss_dsi_ctrl0@ae94000,
// qcom,mdss_dsi_phy0@ae95500 with the PHY at 0xae95000; the display clock controller at 0xaf00000) and Linux's dpu_9_0_sm8550.h,
// dpu_hw_ctl.c and dispcc-sm8550.c (v6.12) for the registers.
// UniXNU-Pagani's DpuProbe hung the OnePlus 13T reading display blocks outside the boot loader's path (unused CTLs and pipes, possibly
// clock gated) and the apps SMMU. Here nothing is read that the running path does not use: the MDSS power domain and the MDP, AHB,
// pixel and byte clocks must be on first (else nothing in MDSS is touched); then CTL 0 must drive INTF_1, and only the pipes, mixers,
// DSC and merge blocks CTL 0 names active are read, besides INTF_1, DSI0 and its PHY. No SMMU, no VBIF, no other CTL or interface.
// Nothing is written.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>

// [0] stage: 99 mapping, 100 armed (waiting 15 s), 100 + n about to take step n (the steps are paced 3 s apart, so the boot log, written every 2 s,
// names the step a hang stopped in), 200 done; negative: the check that stopped it. [1] words kept, [2] segments,
// [3] MDSS GDSCR, [4] MDP clock CBCR, [5] CTL 0 INTF_ACTIVE, [6] FETCH_PIPE_ACTIVE, [7] DSC_ACTIVE, [8] MERGE_3D_ACTIVE, [9] CTL_TOP,
// [10] INTF_1 frame count, [11] one second later, [12] INTF_1 tear interrupt count, [13] one second later, [14] the sample's microseconds,
// [15] CTL 0 layer of mixers 0 and 1 (non-zero bits, mixer 0 in bit 0)
extern "C" {volatile int32_t aston_dpu_survey[16];}
// The kept words: segments of {physical address, word count} then that many words, in the order read.
extern "C" {uint32_t *aston_dpu_dump; volatile uint32_t aston_dpu_dump_words;}

namespace {
constexpr uint64_t kMdss = 0x0ae00000, kMdssBytes = 0x84000, kDsi = 0x0ae94000, kDsiBytes = 0x2000, kDispCc = 0x0af00000,
                   kDispCcBytes = 0x10000;
constexpr uint32_t kMdp = 0x1000, kCtl0 = 0x16000, kIntf1 = 0x36000, kIntf1Tear = 0x36800, kDsc01 = 0x81000, kDsc23 = 0x82000;
constexpr uint32_t kPipe[10] = {0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000, 0x2d000, 0x2f000};
constexpr uint32_t kMixer[6] = {0x45000, 0x46000, 0x47000, 0x48000, 0x49000, 0x4a000};
constexpr uint32_t kMerge3d[4] = {0x4f000, 0x50000, 0x51000, 0x67700};
// dispcc-sm8550.c: MDSS_CORE GDSC, the MDP, pclk0, byte0, byte0_intf, esc0, vsync and AHB branches
constexpr uint32_t kGdscMdss = 0x9000, kCbcrMdp = 0x800c, kCbcrPclk0 = 0x8004, kCbcrByte0 = 0x8028, kCbcrAhb = 0x80a4;
constexpr uint32_t kCapacity = 6144;

IOMemoryMap *map(uint64_t pa, uint64_t bytes) {
    IODeviceMemory *memory = IODeviceMemory::withRange(pa, bytes);
    IOMemoryMap *m = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    return m;
}

struct Region {uint64_t pa; volatile uint32_t *va;};
Region gMdss, gDsi, gCc;
uint32_t gUsed;

uint32_t rd(const Region &r, uint32_t off) {return r.va[off / 4];}
bool clockOn(uint32_t cbcr) {return (rd(gCc, cbcr) & (1u << 31)) == 0;}   // CBCR bit 31: CLK_OFF

// One segment: the words at [off, off + bytes) of a region.
void keep(const Region &r, uint32_t off, uint32_t bytes) {
    const uint32_t words = bytes / 4;
    if (!aston_dpu_dump || gUsed + 2 + words > kCapacity) return;
    aston_dpu_dump[gUsed++] = (uint32_t)(r.pa + off);
    aston_dpu_dump[gUsed++] = words;
    for (uint32_t i = 0; i < words; ++i) aston_dpu_dump[gUsed++] = rd(r, off + 4 * i);
    aston_dpu_survey[2] = aston_dpu_survey[2] + 1;
}

// The first try (aston boot 10, all at once) stopped the phone before the boot log's second write: the steps now go from the reads
// UniXNU-Pagani made safely on the 13T (the GDSC words) toward the MDSS blocks, one group per step.
void step(int n) {
    __atomic_store_n(&aston_dpu_survey[0], 100 + n, __ATOMIC_RELEASE);
    IOSleep(3000);
}
void published() {
    aston_dpu_survey[1] = (int32_t)gUsed;
    __atomic_store_n(&aston_dpu_dump_words, gUsed, __ATOMIC_RELEASE);
}

void survey(void *, wait_result_t) {
    aston_dpu_survey[0] = 99;   // mapping
    IOMemoryMap *mdss = map(kMdss, kMdssBytes), *dsi = map(kDsi, kDsiBytes), *cc = map(kDispCc, kDispCcBytes);
    aston_dpu_dump = static_cast<uint32_t *>(IOMallocData(kCapacity * 4));
    if (!mdss || !dsi || !cc || !aston_dpu_dump) {aston_dpu_survey[0] = -1; return;}
    gMdss = {kMdss, reinterpret_cast<volatile uint32_t *>(mdss->getVirtualAddress())};
    gDsi = {kDsi, reinterpret_cast<volatile uint32_t *>(dsi->getVirtualAddress())};
    gCc = {kDispCc, reinterpret_cast<volatile uint32_t *>(cc->getVirtualAddress())};
    aston_dpu_survey[0] = 100;
    IOSleep(15000);
    step(1);   // the two power domains' GDSCR words only
    aston_dpu_survey[3] = (int32_t)rd(gCc, kGdscMdss);
    keep(gCc, 0x9000, 0x4); keep(gCc, 0xb000, 0x4); published();
    if (!(rd(gCc, kGdscMdss) & (1u << 31))) {aston_dpu_survey[0] = -2; return;}   // GDSCR bit 31: PWR_ON
    step(2);   // the MDP, AHB, pclk0, byte0 and vsync branches
    aston_dpu_survey[4] = (int32_t)rd(gCc, kCbcrMdp);
    keep(gCc, kCbcrPclk0, 0x4); keep(gCc, kCbcrMdp, 0x4); keep(gCc, 0x8024, 0x8); keep(gCc, kCbcrAhb, 0x4); published();
    if (!clockOn(kCbcrMdp) || !clockOn(kCbcrAhb) || !clockOn(kCbcrPclk0) || !clockOn(kCbcrByte0)) {aston_dpu_survey[0] = -3; return;}
    step(3);   // the rest of the clock controller: PLLs, RCGs and branches, the always-on branches
    keep(gCc, 0x0000, 0x40); keep(gCc, 0x1000, 0x40); keep(gCc, 0x8000, 0x300); keep(gCc, 0x9000, 0x10); keep(gCc, 0xa000, 0x28);
    keep(gCc, 0xb000, 0x10); keep(gCc, 0xc000, 0x10); keep(gCc, 0xe000, 0x80); published();
    step(4); keep(gMdss, 0x0000, 0x100); published();   // MDSS top: hardware revision, interrupts (Android's debugfs window reads it)
    step(5); keep(gMdss, kMdp, 0x494); published();     // MDP top
    step(6); keep(gMdss, kCtl0, 0x290); published();    // CTL 0
    const uint32_t intf = rd(gMdss, kCtl0 + 0x0f4), pipes = rd(gMdss, kCtl0 + 0x0fc), dsc = rd(gMdss, kCtl0 + 0x0e8),
                   merge = rd(gMdss, kCtl0 + 0x0e4);
    aston_dpu_survey[5] = (int32_t)intf; aston_dpu_survey[6] = (int32_t)pipes; aston_dpu_survey[7] = (int32_t)dsc;
    aston_dpu_survey[8] = (int32_t)merge; aston_dpu_survey[9] = (int32_t)rd(gMdss, kCtl0 + 0x014);
    if (!(intf & (1u << 1))) {aston_dpu_survey[0] = -4; return;}   // CTL 0 must drive INTF_1 (BIT(intf - INTF_0))
    step(7); keep(gMdss, kIntf1, 0x300); keep(gMdss, kIntf1Tear, 0x10); published();
    step(8);   // the pipes CTL 0 fetches (dpu_hw_ctl.c fetch_tbl: VIG 0..3 are bits 16..19, DMA 0..5 bits 0..5)
    for (unsigned i = 0; i < 10; ++i) if (pipes & (1u << (i < 4 ? 16 + i : i - 4))) keep(gMdss, kPipe[i], 0x344);
    published();
    step(9);   // its mixers (CTL_LAYER(lm): mixers 0..4 at 4 lm, mixer 5 at 0x24; the extensions at 0x40, 0x70, 0xa0 and 0xb8 + 4 lm)
    uint32_t mixers = 0;
    for (unsigned lm = 0; lm < 6; ++lm) {
        const uint32_t layer = lm == 5 ? 0x24 : 4 * lm;
        if (rd(gMdss, kCtl0 + layer) || rd(gMdss, kCtl0 + 0x40 + 4 * lm) || rd(gMdss, kCtl0 + 0x70 + 4 * lm) ||
            rd(gMdss, kCtl0 + 0xa0 + 4 * lm) || rd(gMdss, kCtl0 + 0xb8 + 4 * lm))
            mixers |= 1u << lm;
    }
    aston_dpu_survey[15] = (int32_t)mixers;
    for (unsigned lm = 0; lm < 6; ++lm) if (mixers & (1u << lm)) keep(gMdss, kMixer[lm], 0x320);
    published();
    step(10);   // its DSC encoders and merge blocks
    if (dsc & 3u) {keep(gMdss, kDsc01, 0x10); keep(gMdss, kDsc01 + 0x100, 0x9c); keep(gMdss, kDsc01 + 0x200, 0x9c);
                   keep(gMdss, kDsc01 + 0xf00, 0x24); keep(gMdss, kDsc01 + 0xf80, 0x24);}
    if (dsc & 0xcu) {keep(gMdss, kDsc23, 0x10); keep(gMdss, kDsc23 + 0x100, 0x9c); keep(gMdss, kDsc23 + 0x200, 0x9c);
                     keep(gMdss, kDsc23 + 0xf00, 0x24); keep(gMdss, kDsc23 + 0xf80, 0x24);}
    for (unsigned m = 0; m < 4; ++m) if (merge & (1u << m)) keep(gMdss, kMerge3d[m], 0x8);
    published();
    step(11); keep(gDsi, 0x0000, 0x400); published();   // DSI0 controller
    step(12); keep(gDsi, 0x1000, 0xa00); published();   // DSI0 PHY, its PLL at +0x500
    step(13);   // the refresh the boot loader runs: INTF_1 frames and tear interrupts over one second
    const uint64_t t0 = mach_absolute_time();
    aston_dpu_survey[10] = (int32_t)rd(gMdss, kIntf1 + 0x0ac);
    aston_dpu_survey[12] = (int32_t)rd(gMdss, kIntf1 + 0x298);
    IOSleep(1000);
    aston_dpu_survey[11] = (int32_t)rd(gMdss, kIntf1 + 0x0ac);
    aston_dpu_survey[13] = (int32_t)rd(gMdss, kIntf1 + 0x298);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    aston_dpu_survey[14] = (int32_t)(ns / 1000);
    aston_dpu_survey[0] = 200;
}
}  // namespace

extern "C" void aston_dpu_survey_probe(void) {
    uint32_t on = 0;
    if (!PE_parse_boot_argn("aston-dpu", &on, sizeof on) || on != 1 || aston_dpu_survey[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(survey, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else aston_dpu_survey[0] = -5;
}
