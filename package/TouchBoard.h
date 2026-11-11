// SPDX-License-Identifier: BSD-2-Clause-Patent
// sheng (SM8550) touch controller board facts and the pure helpers around them. Every constant states its source.
// Nothing here touches hardware; TouchProbe.cpp owns the MMIO. Host-tested by TouchBoardTests.c.
#ifndef SHENG_TOUCH_BOARD_H
#define SHENG_TOUCH_BOARD_H
#include <stdint.h>

// Board DT (build/sheng/sheng.preprocessed.dts): spi4 = wrapper geniqup@ac0000, engine reg 0xa90000 size 0x4000.
#define TB_QUP_WRAPPER_BASE 0x00ac0000ULL
#define TB_SE_BASE 0x00a90000ULL
#define TB_SE_SIZE 0x4000ULL
// sm8550.dtsi: geniqup@ac0000 (spi4's wrapper) has iommus = <&apps_smmu 0xa3 0>; apps_smmu@15000000 (docs/native-ufs.md).
#define TB_QUP_SMMU_SID 0xa3
#define TB_APPS_SMMU_BASE 0x15000000ULL
#define TB_APPS_SMMU_SIZE 0xd0000ULL
// gcc-sm8550.c (Linux mainline): GCC at 0x100000 per the DT (clock-controller@100000).
#define TB_GCC_BASE 0x00100000ULL
#define TB_GCC_VOTE_ENABLE 0x52008            // enable_reg of every QUP branch below
#define TB_GCC_GPLL0_ENABLE 0x52018           // gcc_gpll0 enable_reg, bit 0
#define TB_GCC_GPLL0_MODE 0x0
#define TB_VOTE_WRAP1_CORE_2X (1u<<18)        // gcc_qupv3_wrap1_core_2x_clk, halt 0x23294
#define TB_VOTE_WRAP1_CORE (1u<<19)           // gcc_qupv3_wrap1_core_clk, halt 0x23284
#define TB_VOTE_WRAP1_M_AHB (1u<<20)          // gcc_qupv3_wrap_1_m_ahb_clk, halt 0x2327c
#define TB_VOTE_WRAP1_S_AHB (1u<<21)          // gcc_qupv3_wrap_1_s_ahb_clk, halt 0x23280
#define TB_VOTE_WRAP1_S4 (1u<<26)             // gcc_qupv3_wrap1_s4_clk, halt 0x184e4
#define TB_VOTE_ALL (TB_VOTE_WRAP1_CORE_2X|TB_VOTE_WRAP1_CORE|TB_VOTE_WRAP1_M_AHB|TB_VOTE_WRAP1_S_AHB|TB_VOTE_WRAP1_S4)
#define TB_HALT_CORE_2X 0x23294
#define TB_HALT_CORE 0x23284
#define TB_HALT_M_AHB 0x2327c
#define TB_HALT_S_AHB 0x23280
#define TB_HALT_S4 0x184e4
#define TB_CBCR_CLK_OFF (1u<<31)
// gcc_qupv3_wrap1_s4_clk_src: cmd_rcgr 0x184f0, DFS-managed (clk-rcg2.c SE_* macros).
#define TB_S4_RCG 0x184f0
#define TB_RCG_CMD 0x0
#define TB_RCG_CFG 0x4
#define TB_RCG_DFSR_CMD 0x14                  // bit0 DFS enable, bits 4:1 current level
#define TB_RCG_PERF_CFG(l) (0x1c+4*(l))
#define TB_RCG_PERF_M(l) (0x5c+4*(l))
#define TB_RCG_PERF_N(l) (0x9c+4*(l))
#define TB_DFS_LEVELS 8
// TLMM (DT: pinctrl@f100000; pinctrl-sm8550.c: ctl_reg = 0x1000*gpio, io_reg = +4, mux_bit 2, drv_bit 6, oe_bit 9).
#define TB_TLMM_BASE 0x0f100000ULL
#define TB_GPIO_STRIDE 0x1000
#define TB_GPIO_SPI_MISO 44                   // sheng.dts qup_spi4_data_clk: gpio44/45/46, qup_spi4_cs: gpio47
#define TB_GPIO_SPI_MOSI 45
#define TB_GPIO_SPI_CLK 46
#define TB_GPIO_SPI_CS 47
#define TB_GPIO_IRQ 25                        // aston: synaptics,irq-gpio = <&tlmm 25 0x2008> (ATTN, active low)
#define TB_FUNC_QUP1_SE4 1                    // PINGROUP(44..47, qup1_se4, ...): function slot 1
#define TB_DRIVE_6MA 2                        // drive-strength = <6>: 2 mA steps encoded as (mA/2)-1
#define TB_DRIVE_8MA 3
#define TB_SPI_MODE 0                         // aston: synaptics,spi-mode = <0>
#define TB_SPI_DEFAULT_HZ 19000000u   // aston: spi-max-frequency = <19000000>
#define TB_SPI_MAX_HZ 19200000u

// ---- OnePlus Ace 3 (aston, project 23801) Synaptics S3910 on the same QUP1 SE4 as the reference board ----
// tp/aston-oplus_tp-23801.dtsi synaptics_tcm_hbp@0 and salami_overlay_common.dtsi pinctrl (oneplus/sm8550_v_15.0.0_ace_3 4498f99b).
#define TB_GPIO_RESET 24                      // synaptics,reset-gpio = <&tlmm 24 0x1>; ts_reset_active: 8 mA, bias-disable
#define TB_GPIO_AVDD 10                       // synaptics,avdd-gpio = <&tlmm 10 0x1>, power-on-state 1: drive high to enable AVDD
#define TB_POWER_DELAY_MS 200                 // synaptics,power-delay-ms
#define TB_RESET_ACTIVE_MS 10                 // synaptics,reset-active-ms (reset-on-state 0: low while active)
#define TB_RESET_DELAY_MS 80                  // synaptics,reset-delay-ms
// vdd-supply = <&L4B>: PM8550 (PMIC B) LDO 4, RPMh VRM "ldob4". The vendor regulator default is 1.8 V.
#define TB_VDD_RESOURCE "ldob4"
#define TB_VDD_MV 1800
// Apps RSC DRV 2 (kalama.dtsi apps_rsc drv-2), TCS block at +0xd00; AOP command database (aop_cmd_db_region@81c60000).
// Active TCS 0 is the reference WiFi bring-up's and TCS 1 AdspProbe's; the touch rail request uses TCS 2.
#define TB_RSC_DRV2 0x17a20000ULL
#define TB_RSC_TCS 0xd00
#define TB_RSC_TOUCH_TCS 2
#define TB_CMD_DB 0x81c60000ULL
#define TB_CMD_DB_SIZE 0x20000
// qcom-rpmh-regulator.c: VRM register offsets in the resource and the PMIC5 LDO high power mode.
#define TB_VRM_VOLTAGE 0x0
#define TB_VRM_ENABLE 0x4
#define TB_VRM_MODE 0x8
#define TB_VRM_MODE_HPM 7
// GENI SPI delay counters (SE_SPI_DELAY_COUNTERS 0x278): CS-to-clock 50, word 0, the value OnePlus' SM8750 sibling runs with.
#define TB_SPI_DELAY_COUNTERS 0x278
#define TB_SPI_CS_DELAY 50
#define TB_SPI_WORD_DELAY 0
// Touch coordinate space (touchpanel,panel-coords = <20224 44480>) over the 1264 x 2780 portrait panel: X is the short axis.
// S3910 reports its own maxima in the application info; these are the expected values, checked on the diagnostic page.
#define TB_S3910_MAX_X 20224
#define TB_S3910_MAX_Y 44480
// ShengHidInput normalises published coordinates against these maxima (30480 x 20320, the reference panel's axes).
#define TB_HID_X_MAX 30480
#define TB_HID_Y_MAX 20320
// aston-display=1 (patches/xnu-12377/aston-display-window.patch): iOS sees a window of the panel. One axis of a contact, normalised to
// 0..32767 over the panel's `panel` pixels, is normalised again over the window's `size` pixels from `origin`. Contacts on the border
// clamp to the window's edge, so an edge swipe that starts on the border starts at the edge. A window outside the panel maps nothing.
static inline int tb_window_axis(int v, uint32_t panel, uint32_t origin, uint32_t size) {
    if (panel < 2 || size < 2 || origin > panel || size > panel - origin) return v;
    int64_t px = ((int64_t)v * (panel - 1) + 16383) / 32767 - origin;   // nearest panel pixel
    if (px < 0) px = 0;
    if (px > (int64_t)size - 1) px = (int64_t)size - 1;
    return (int)((px * 32767 + (size - 1) / 2) / (size - 1));
}
// TLMM pin configuration with a pull: bits 1:0 pull (0 none, 1 down, 3 up), 5:2 function, 8:6 drive, 9 output enable.
static inline uint32_t tb_gpio_cfg(unsigned pull, unsigned drive_code, unsigned output) {
    return (pull&3u)|(drive_code&7u)<<6|(output?1u<<9:0u);
}

static inline uint32_t tb_pin_cfg(unsigned func, unsigned drive_code) {return (func&15u)<<2|(drive_code&7u)<<6;}   // pull disabled
static inline uint32_t tb_tlmm_ctl(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE;}
static inline uint32_t tb_tlmm_io(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE+4;}

// Source clock rates by RCG source-select value (gcc_parent_map_0: 0 = TCXO, 1 = GPLL0 main, 6 = GPLL0 even).
// GPLL0 main is 600 MHz and even 300 MHz, inferred from the frequency table (100 MHz = main / 6, 75 MHz = even / 4).
static inline uint64_t tb_src_hz(unsigned sel) {return sel==0?19200000ULL:sel==1?600000000ULL:sel==6?300000000ULL:0;}
// clk-rcg2.c calc_rate: half-integer divider, then M/N.
static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) {prate*=2;prate/=hid+1;}
    if (mode && n) {prate*=m;prate/=n;}
    return prate;
}
// Frequency of DFS level l from its CFG (src 10:8, mode 13:12, hid 4:0 on this 5-bit RCG), M register and N register
// (stored inverted: n = (~N & mask) + m, mnd_width 16).
static inline uint64_t tb_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src=(cfg>>8)&7,mode=(cfg>>12)&3,hid=cfg&0x1f;
    uint32_t m=0,n=0;
    if (mode) {m=mreg&0xffff;n=((~nreg)&0xffff)+m;}
    return tb_calc_rate(tb_src_hz(src),m,n,mode,hid);
}
struct tb_clock_choice {unsigned level, div;uint64_t sclk_hz, actual_hz;};
// Choose the DFS level that gives the highest SPI rate not above target with divider div = ceil(sclk / (os * target)).
// Returns 0 on success.
static inline int tb_choose_clock(const uint64_t *level_hz, unsigned n, unsigned os, unsigned target, struct tb_clock_choice *out) {
    int found=0;
    for (unsigned l=0;l<n;++l) {
        uint64_t f=level_hz[l];if (!f) continue;
        uint64_t d=((f+(uint64_t)os*target-1)/((uint64_t)os*target));if (d<1) d=1;if (d>0xfff) continue;
        uint64_t actual=f/(os*d);if (actual>target) continue;
        if (!found || actual>out->actual_hz || (actual==out->actual_hz && d<out->div)) {
            out->level=l;out->div=(unsigned)d;out->sclk_hz=f;out->actual_hz=actual;found=1;
        }
    }
    return found?0:-1;
}
#endif
