// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Ace 3's panel brightness through DSI0's command path, for the iOS brightness service (AppleARMBacklight.cpp, sheng-backlight=2).
// The panel is the AA551 P 3 A0004 (Android cmdline msm_drm.dsi_display0=qcom,mdss_dsi_panel_AA551_P_3_A0004_dsc_cmd): an AMOLED in DSC
// command mode on DSI0 (qcom,dsi-ctrl-num = <0>, one interface), behind the Pixelworks Iris7P bridge. OnePlus' panel source
// (android_kernel_modules_and_devicetree_oneplus_sm8550 4498f99, display-devicetree/oplus/panel/dsi-panel-AA551-P-3-A0004-dsc-cmd.dtsi)
// and the phone's live devicetree give: brightness is DCS 0x51 with two bytes, high byte first (qcom,mdss-dsi-bl-inverted-dbv), on the
// panel's page 0, which every brightness write in the panel's own command sets selects first (39 ... FF 08 38 00); levels 1..4094
// (bl-min-level 1, bl-max-level 0xffe), of which 1..3349 (oplus,dsi-bl-normal-max-level 0xd15) is the normal range and the rest the high
// brightness mode; the default level is 1638 (oplus,dsi-brightness-default-level 0x666); command DMA is triggered by software
// (qcom,mdss-dsi-dma-trigger = "trigger_sw").
// A command goes out as UniXNU-asteroids' PanelProbe.cpp (6370e66) sends it on the Nothing Phone (3a), whose DSI 2.8 controller has the
// same command registers as this DSI 2.7: through the controller's TPG command FIFO, word by word, with no DMA buffer and so no display
// SMMU mapping (the downstream dsi_ctrl_hw_cmn_kickoff_fifo_command path); the software trigger starts it and INT_CTRL bit 0 (DMA done)
// ends it, then the FIFO and trigger setup are reset. A command mode panel needs no transfer line: that is only scheduled when the
// controller is in video mode.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <mach/mach_time.h>

// [0] stage (1 mapped, negative: the step that failed), [1] HW_VERSION, [2] DSI_CTRL, [3] COMMAND_MODE_DMA_CTRL, [4] TRIG_CTRL, [5] INT_CTRL,
// [6] commands sent, [7] commands that timed out, [8] last command's time (us), [9] last level written, [10] the longest command (us)
extern "C" {volatile int32_t aston_panel_stats[12];}

namespace {
constexpr uint64_t kDsi0 = 0x0ae94000, kDsiBytes = 0x1000;
constexpr uint32_t kHwVersion = 0x000, kCtrl = 0x004, kActiveV = 0x028, kDmaCtrl = 0x03c, kDmaLength = 0x04c, kTrigCtrl = 0x084,
                   kSwTrigger = 0x090, kScheduleCtrl = 0x100, kScheduleCtrl2 = 0x104, kIntCtrl = 0x110, kTpgCtrl = 0x15c, kTpgInit = 0x17c,
                   kTpgFifoReset = 0x1ec;
constexpr uint32_t kCtrlVideo = 1u << 1, kCtrlCommand = 1u << 2;
constexpr uint32_t kMaxLevel = 4094, kTimeoutUs = 50000;

IOMemoryMap *gMap;
volatile uint32_t *gDsi;
IOLock *gLock;

uint32_t rd(uint32_t reg) {return gDsi[reg / 4];}
void wr(uint32_t reg, uint32_t value) {gDsi[reg / 4] = value; __asm__ volatile("dsb sy" ::: "memory");}

// One packet through the TPG FIFO: the header word (data0, data1, data type, flags) and the payload words. High speed. 0, or -1 when DMA
// done never came.
int sendPacket(const uint32_t *words, unsigned count) {
    const uint64_t began = mach_absolute_time();
    wr(kTrigCtrl, (rd(kTrigCtrl) & ~((1u << 16) | 0xfu)) | 0x4u);   // software trigger for the command DMA
    const bool video = rd(kCtrl) & kCtrlVideo;
    if (video) wr(kScheduleCtrl, (1u << 28) | (((rd(kActiveV) >> 16) & 0xffffu) + 1));   // video mode: the first front porch line
    wr(kCtrl, rd(kCtrl) | kCtrlCommand);
    wr(kIntCtrl, (rd(kIntCtrl) & ~((1u << 24) | (1u << 28))) | 1u);   // clear a stale DMA done (bit 0 is write-1-to-clear)
    wr(kTpgCtrl, (1u << 1) | (1u << 2) | (3u << 16));
    for (unsigned i = 0; i < count; ++i) wr(kTpgInit, words[i]);
    if (count & 1) wr(kTpgInit, 0);
    const uint32_t dma = rd(kDmaCtrl) & ~((1u << 31) | (1u << 30) | (1u << 26));
    wr(kDmaCtrl, dma | (1u << 28));
    wr(kDmaLength, count * 4);
    wr(kSwTrigger, 1);
    int rc = -1;
    for (uint32_t waited = 0; waited < kTimeoutUs; waited += 20) {
        if (rd(kIntCtrl) & 1u) {rc = 0; break;}
        IODelay(20);
    }
    wr(kIntCtrl, (rd(kIntCtrl) & ~((1u << 24) | (1u << 28))) | 1u);
    wr(kTpgCtrl, 0);
    wr(kTpgFifoReset, 1);
    IODelay(1);
    wr(kTpgFifoReset, 0);
    wr(kTrigCtrl, (rd(kTrigCtrl) & ~((1u << 16) | 0xfu)) | 0x4u);
    if (video) {wr(kScheduleCtrl2, 0); wr(kScheduleCtrl, 0);}
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    aston_panel_stats[8] = (int32_t)(ns / 1000);
    if (aston_panel_stats[8] > aston_panel_stats[10]) aston_panel_stats[10] = aston_panel_stats[8];
    aston_panel_stats[6] = aston_panel_stats[6] + 1;
    if (rc) aston_panel_stats[7] = aston_panel_stats[7] + 1;
    return rc;
}
// DCS long write (data type 0x39) header: word count in bits 15:0, flags in the top byte (bit 31 last packet, bit 30 long packet). Each
// packet here is a DMA transfer of its own, so each is the last one.
uint32_t longHeader(unsigned bytes) {return 0xc0000000u | (0x39u << 16) | bytes;}
}  // namespace

extern "C" int aston_panel_open(void) {
    if (gDsi) return 0;
    if (aston_panel_stats[0] < 0) return -1;
    gLock = IOLockAlloc();
    IODeviceMemory *memory = IODeviceMemory::withRange(kDsi0, kDsiBytes);
    gMap = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    volatile uint32_t *dsi = gMap ? reinterpret_cast<volatile uint32_t *>(gMap->getVirtualAddress()) : nullptr;
    if (!gLock || !dsi) {aston_panel_stats[0] = -1; return -1;}
    gDsi = dsi;
    aston_panel_stats[1] = (int32_t)rd(kHwVersion);
    aston_panel_stats[2] = (int32_t)rd(kCtrl);
    aston_panel_stats[3] = (int32_t)rd(kDmaCtrl);
    aston_panel_stats[4] = (int32_t)rd(kTrigCtrl);
    aston_panel_stats[5] = (int32_t)rd(kIntCtrl);
    // The controller the boot loader left running: enabled (bit 0). Off, the commands would go nowhere.
    if (!(rd(kCtrl) & 1u)) {gDsi = nullptr; aston_panel_stats[0] = -2; return -1;}
    aston_panel_stats[0] = 1;
    return 0;
}

// The panel's brightness, 0..4094: page 0 (FF 08 38 00), then 51 hi lo. On a thread or work loop: it waits up to 50 ms per packet.
extern "C" int aston_panel_level(uint32_t level) {
    if (!gDsi) return -1;
    if (level > kMaxLevel) level = kMaxLevel;
    const uint32_t page[2] = {longHeader(4), 0xffu | 0x08u << 8 | 0x38u << 16 | 0x00u << 24};
    const uint32_t brightness[2] = {longHeader(3), 0x51u | ((level >> 8) & 0xffu) << 8 | (level & 0xffu) << 16 | 0xffu << 24};
    IOLockLock(gLock);
    int rc = sendPacket(page, 2);
    if (!rc) rc = sendPacket(brightness, 2);
    IOLockUnlock(gLock);
    if (!rc) aston_panel_stats[9] = (int32_t)level;
    return rc;
}
