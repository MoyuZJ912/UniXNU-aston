/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The OnePlus Ace 3 fuel gauge's I2C bus, polled and blocking, brought up from cold: QUP wrapper 2 serial engine 0 of the SM8550
 * (downstream kalama qupv3_se8_i2c, upstream i2c8, i2c@880000), 100 kHz, owned by the apps processor (Android's bq27541/bq28z610 driver
 * is the only user; aston oplus-chg-23801.dtsi &qupv3_se8_i2c: qcom,clk-freq-out = <100000>, bq27541-battery@55).
 *
 * Ported from UniXNU-Pagani-Drivers drivers/PaganiBattery/BatteryI2c.c (c888efd, BSD-2-Clause-Patent), whose transfer phases follow
 * UniXNU-sheng package/GeniI2c.c: a write that holds the stop condition, then a read; TX data in on the watermark; a failed command ends
 * with CMD_DONE and its error bits; cancel, then abort, on a timeout. What differs on this board:
 *   1 clocks   GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1 (GCC + 0x52010) bits 0 (gcc_qupv3_wrap2_core_clk), 1 (wrap_2_s_ahb), 2 (wrap_2_m_ahb),
 *              3 (wrap2_core_2x) and 4 (wrap2_s0), each branch seen running (CBCR 0x233d4, 0x233d0, 0x233cc, 0x23004, 0x1e004; a branch
 *              in hardware clock gating counts as running). No bit is ever cleared. Every transfer reads the vote register first and
 *              votes again what it finds dropped.
 *   2 root     gcc_qupv3_wrap2_s0_clk_src (cmd_rcgr 0x1e010) is DFS-managed: the engine picks a level with CLK_SEL. The level used is the
 *              one the boot firmware programmed as the 19.2 MHz XO undivided (source 0, no M/N, divider bypass or 1); none, no bring-up.
 *              With DFS off the root itself must run from the XO undivided. The RCG is never reprogrammed.
 *   3 engine   the I2C protocol (FW_REVISION bits 15:8 = 3), FIFO interface on, no command active (one left active is aborted); FIFO
 *              mode, 8-bit packing, divider 7 and SCL counters 10/12/26 (100 kHz from 19.2 MHz), no interrupt line.
 *   4 pins     gpio0 (SDA) and gpio1 (SCL) on function 2 (qup2_se0_l0_mirb / l1_mirb), 2 mA, pull-up, output enable clear, as the
 *              Android active pin state (aston oplus-chg-23801.dtsi &qupv3_se8_i2c_sda_active / _scl_active).
 * Register facts: upstream Linux v6.12 gcc-sm8550.c, pinctrl-sm8550.c, clk-rcg2.c (SE_* DFS offsets) and i2c-qcom-geni.c, and the aston
 * devicetree (OnePlusOSS android_kernel_modules_and_devicetree_oneplus_sm8550 oneplus/sm8550_v_15.0.0_ace_3); no Linux code is used.
 * The owner maps the windows and gives the delay. */
#ifndef ASTON_GAUGE_BUS_H
#define ASTON_GAUGE_BUS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The register windows (the owner maps each; the offsets below are inside them). */
enum {AgbWinGccVote, AgbWinGccSe, AgbWinGccAhb, AgbWinSda, AgbWinScl, AgbWinSe, AgbWinCount};
#define AGB_GCC_VOTE_BASE 0x152000ull   /* GCC 0x100000 + 0x52000 */
#define AGB_GCC_SE_BASE 0x11e000ull     /* GCC + 0x1e000: the s0 branch and its RCG */
#define AGB_GCC_AHB_BASE 0x123000ull    /* GCC + 0x23000: the wrapper branches */
#define AGB_GCC_PAGE 0x1000u
#define AGB_TLMM_SDA 0xf100000ull       /* TLMM gpio0 (0xf100000 + 0x1000 * n) */
#define AGB_TLMM_SCL 0xf101000ull       /* TLMM gpio1 */
#define AGB_TLMM_PAGE 0x1000u
#define AGB_SE_BASE 0x880000ull
#define AGB_SE_SIZE 0x4000u

/* GCC */
#define AGB_VOTE 0x010u                 /* in AgbWinGccVote: GCC_APCS_CLOCK_BRANCH_ENA_VOTE_1 */
#define AGB_VOTE_CORE (1u << 0)
#define AGB_VOTE_S_AHB (1u << 1)
#define AGB_VOTE_M_AHB (1u << 2)
#define AGB_VOTE_CORE_2X (1u << 3)
#define AGB_VOTE_S0 (1u << 4)
#define AGB_VOTE_BITS (AGB_VOTE_CORE | AGB_VOTE_S_AHB | AGB_VOTE_M_AHB | AGB_VOTE_CORE_2X | AGB_VOTE_S0)
#define AGB_CBCR_S0 0x004u              /* in AgbWinGccSe */
#define AGB_RCG_CMD 0x010u
#define AGB_RCG_CFG 0x014u
#define AGB_RCG_DFSR 0x024u             /* cmd_rcgr + 0x14: bit 0 DFS on */
#define AGB_RCG_PERF(level) (0x02cu + 4u * (level))   /* cmd_rcgr + 0x1c + 4 * level: that level's CFG */
#define AGB_DFS_LEVELS 8u
#define AGB_CBCR_M_AHB 0x3ccu           /* in AgbWinGccAhb */
#define AGB_CBCR_S_AHB 0x3d0u
#define AGB_CBCR_CORE 0x3d4u
#define AGB_CBCR_CORE_2X 0x004u
#define AGB_CBCR_OFF (1u << 31)
#define AGB_CBCR_HW_CTL (1u << 1)
#define AGB_RCG_ROOT_OFF (1u << 31)
/* An RCG CFG word: divider 4:0 (0 bypass, n: (n + 1) / 2), source 10:8 (0 the XO), M/N mode 13:12. */
#define AGB_CFG_XO_UNDIVIDED(cfg) ((((cfg) >> 8) & 7u) == 0 && (((cfg) >> 12) & 3u) == 0 && ((cfg) & 0x1fu) <= 1u)
/* TLMM CTL (offset 0 of each pin's page): pull 1:0, function 5:2, drive 8:6 (mA / 2 - 1), output enable 9 */
#define AGB_PIN_MASK 0x3ffu
#define AGB_PIN_I2C ((2u << 2) | 3u)    /* function 2, 2 mA, pull-up, no output enable */

/* GENI serial engine */
#define AGB_FORCE_DEFAULT 0x20u
#define AGB_OUTPUT_CTRL 0x24u
#define AGB_CGC_CTRL 0x28u
#define AGB_GENI_STATUS 0x40u
#define AGB_M_CLK_CFG 0x48u
#define AGB_IF_DISABLE 0x64u
#define AGB_FW_REVISION 0x68u
#define AGB_CLK_SEL 0x7cu
#define AGB_BYTE_GRAN 0x254u
#define AGB_DMA_MODE 0x258u
#define AGB_TX_PACK0 0x260u
#define AGB_TX_PACK1 0x264u
#define AGB_TX_LEN 0x26cu
#define AGB_RX_LEN 0x270u
#define AGB_SCL_COUNTERS 0x278u
#define AGB_RX_PACK0 0x284u
#define AGB_RX_PACK1 0x288u
#define AGB_M_CMD0 0x600u
#define AGB_M_CMD_CTRL 0x604u
#define AGB_M_IRQ_STATUS 0x610u
#define AGB_M_IRQ_EN 0x614u
#define AGB_M_IRQ_CLEAR 0x618u
#define AGB_S_IRQ_CLEAR 0x648u
#define AGB_TX_FIFO 0x700u
#define AGB_RX_FIFO 0x780u
#define AGB_RX_FIFO_STATUS 0x804u
#define AGB_TX_WATERMARK 0x80cu
#define AGB_RX_WATERMARK 0x810u
#define AGB_RX_RFR 0x814u
#define AGB_DMA_TX_IRQ_CLR 0xc44u
#define AGB_DMA_RX_IRQ_CLR 0xd44u
#define AGB_GSI_EVENT_EN 0xe18u
#define AGB_SE_IRQ_EN 0xe1cu
#define AGB_DMA_GENERAL_CFG 0xe30u

#define AGB_M_CMD_DONE (1u << 0)
#define AGB_M_CMD_OVERRUN (1u << 1)
#define AGB_M_ILLEGAL_CMD (1u << 2)
#define AGB_M_CMD_FAILURE (1u << 3)
#define AGB_M_CMD_CANCEL (1u << 4)
#define AGB_M_CMD_ABORT (1u << 5)
#define AGB_M_GP_NACK (1u << 10)        /* GP_IRQ1: address or data not acknowledged */
#define AGB_M_GP_BUS (1u << 12)         /* GP_IRQ3: bus protocol error */
#define AGB_M_GP_ARB (1u << 13)         /* GP_IRQ4: arbitration lost */
#define AGB_M_GP_ALL (0x3fu << 9)
#define AGB_M_TX_WATERMARK (1u << 30)
#define AGB_M_ERRORS (AGB_M_CMD_OVERRUN | AGB_M_ILLEGAL_CMD | AGB_M_CMD_FAILURE | AGB_M_GP_ALL)
#define AGB_M_WANTED (AGB_M_CMD_DONE | AGB_M_ERRORS | AGB_M_CMD_CANCEL | AGB_M_CMD_ABORT | AGB_M_TX_WATERMARK)
#define AGB_CTRL_ABORT (1u << 1)
#define AGB_CTRL_CANCEL (1u << 2)
#define AGB_PROTO_I2C 3u
#define AGB_OP_WRITE 1u
#define AGB_OP_READ 2u
#define AGB_STOP_STRETCH (1u << 2)
#define AGB_CLK_100K ((7u << 4) | 1u)                       /* divider 7, serial clock on */
#define AGB_SCL_100K ((10u << 20) | (12u << 10) | 26u)      /* high 10, low 12, cycle 26 */
#define AGB_PACK0 0x7f8feu                                   /* 8-bit words, 4 per FIFO word, first byte in bits 7:0 */
#define AGB_PACK1 0xffefeu

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, unsigned window, uint32_t offset);
    void (*write)(void *ctx, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
} AgbIo;

/* Bring-up stages: report.stage is the last one reached, report.error the failing step's code (0 when ready). */
enum {AgbStNone = 0, AgbStClocks = 1, AgbStRoot = 2, AgbStEngine = 3, AgbStPins = 4, AgbStReady = 5};

typedef struct {
    int stage, error;
    uint32_t vote_before, vote_after, voted;  /* voted: 1 when this driver wrote the vote register */
    uint32_t cbcr[5];                          /* CORE, S_AHB, M_AHB, CORE_2X, S0 after the votes */
    uint32_t rcg_cmd, rcg_cfg, dfsr;
    int level;                                 /* the DFS level selected (-1: DFS off) */
    uint32_t level_cfg[AGB_DFS_LEVELS];
    uint32_t fw, if_disable, geni_status;
    uint32_t clk_before, scl_before, clk_after, scl_after;
    uint32_t pin_before[2], pin_after[2];
} AgbReport;

typedef struct {
    AgbIo io;
    uint8_t ready;
    uint32_t transfers, failures, nacks, timeouts, bus_errors;
    uint32_t last_status;                     /* M_IRQ status bits of the last failure */
    int last_error;
    uint32_t revotes, dropped_vote;           /* transfers that found the clock votes dropped; the vote register they found */
    int revote_error;                         /* the last new vote: 0 back, -5 not kept, -110 a branch stays off */
} AgbBus;

/* The DFS level that runs the engine from the 19.2 MHz XO undivided, from the level CFG words; -1 when there is none. */
int aston_gauge_bus_xo_level(const uint32_t cfg[AGB_DFS_LEVELS]);
int aston_gauge_bus_bringup(AgbBus *b, const AgbIo *io, AgbReport *report);
/* Writes txn (1..8) bytes to the 7-bit address, then, when rxn (1..4) is not 0, reads rxn bytes with a repeated start. 0 done;
 * -6 not acknowledged, -71 bus protocol error, -11 arbitration lost, -5 engine error, -61 no data, -110 timed out (the command was
 * cancelled or aborted), -16 the clock votes were found dropped and are not back (revote_error; the engine was not touched), -19 not
 * brought up, -22 bad arguments. */
int aston_gauge_bus_xfer(AgbBus *b, uint8_t address, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn);

#ifdef __cplusplus
}
#endif
#endif
