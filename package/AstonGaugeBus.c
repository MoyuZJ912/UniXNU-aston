/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The Ace 3 fuel gauge bus (AstonGaugeBus.h). Transfer phases and bring-up order from UniXNU-Pagani-Drivers BatteryI2c.c (c888efd),
 * after UniXNU-sheng package/GeniI2c.c; the clocks, the DFS level and the pins are this board's. */
#include "AstonGaugeBus.h"

enum {
    kPollUs = 10,
    kBranchPolls = 500,          /* 5 ms for a branch clock to run */
    kPhaseUs = 25000,            /* one command at 100 kHz takes under 1 ms; the gauge may stretch the clock */
    kCancelUs = 2000,
};

static void zero(void *to, unsigned long n)
{
    volatile uint8_t *d = (volatile uint8_t *)to;
    for (unsigned long i = 0; i < n; ++i) d[i] = 0;
}

static uint32_t rd(const AgbIo *io, unsigned w, uint32_t at) {return io->read(io->ctx, w, at);}
static void wr(const AgbIo *io, unsigned w, uint32_t at, uint32_t v) {io->write(io->ctx, w, at, v);}
static void delay(const AgbIo *io, unsigned us) {if (io->delay_us) io->delay_us(io->ctx, us);}

static int fail(AgbReport *r, int stage, int error)
{
    r->stage = stage;
    r->error = error;
    return error;
}

/* A voted branch runs when its CLK_OFF bit clears; one under hardware clock gating may read off while idle and counts as running. */
static int branch_on(const AgbIo *io, unsigned window, uint32_t at, uint32_t *value)
{
    for (unsigned n = 0; n < kBranchPolls; ++n) {
        const uint32_t v = rd(io, window, at);
        *value = v;
        if ((v & AGB_CBCR_HW_CTL) || !(v & AGB_CBCR_OFF)) return 1;
        delay(io, kPollUs);
    }
    return 0;
}

/* The five votes, written only when one is missing (the register's other bits are kept), then read back. */
static int vote_bits(const AgbIo *io, uint32_t *wrote, uint32_t *after)
{
    const uint32_t v = rd(io, AgbWinGccVote, AGB_VOTE);
    if ((v & AGB_VOTE_BITS) != AGB_VOTE_BITS) {
        wr(io, AgbWinGccVote, AGB_VOTE, v | AGB_VOTE_BITS);
        *wrote = 1;
    }
    *after = rd(io, AgbWinGccVote, AGB_VOTE);
    return (*after & AGB_VOTE_BITS) == AGB_VOTE_BITS ? 0 : -5;
}

static int branches(const AgbIo *io, uint32_t cbcr[5])
{
    const int core = branch_on(io, AgbWinGccAhb, AGB_CBCR_CORE, &cbcr[0]);
    const int s_ahb = branch_on(io, AgbWinGccAhb, AGB_CBCR_S_AHB, &cbcr[1]);
    const int m_ahb = branch_on(io, AgbWinGccAhb, AGB_CBCR_M_AHB, &cbcr[2]);
    const int core2x = branch_on(io, AgbWinGccAhb, AGB_CBCR_CORE_2X, &cbcr[3]);
    const int se = branch_on(io, AgbWinGccSe, AGB_CBCR_S0, &cbcr[4]);
    return core && s_ahb && m_ahb && core2x && se ? 0 : -110;
}

static int clocks(const AgbIo *io, AgbReport *r)
{
    r->vote_before = r->vote_after = rd(io, AgbWinGccVote, AGB_VOTE);
    if ((r->vote_before & AGB_VOTE_BITS) != AGB_VOTE_BITS) {
        const int rc = vote_bits(io, &r->voted, &r->vote_after);
        if (rc) return rc;
    }
    return branches(io, r->cbcr);
}

/* Before every transfer: one read of the vote register. Bits an update elsewhere dropped since the bring-up are voted again and the
 * branches seen running before the engine is touched; until then no transfer touches it. */
static int clocks_kept(AgbBus *b)
{
    const uint32_t seen = rd(&b->io, AgbWinGccVote, AGB_VOTE);
    if ((seen & AGB_VOTE_BITS) == AGB_VOTE_BITS) return 0;
    b->revotes++;
    b->dropped_vote = seen;
    uint32_t wrote = 0, after = 0, cbcr[5];
    int rc = vote_bits(&b->io, &wrote, &after);
    if (!rc) rc = branches(&b->io, cbcr);
    b->revote_error = rc;
    return rc;
}

int aston_gauge_bus_xo_level(const uint32_t cfg[AGB_DFS_LEVELS])
{
    for (unsigned level = 0; level < AGB_DFS_LEVELS; ++level)
        if (AGB_CFG_XO_UNDIVIDED(cfg[level])) return (int)level;
    return -1;
}

/* The root: its DFS level at the XO undivided (CLK_SEL), or with DFS off the root itself on the XO undivided. Nothing is written here. */
static int root(const AgbIo *io, AgbReport *r)
{
    r->rcg_cmd = rd(io, AgbWinGccSe, AGB_RCG_CMD);
    r->rcg_cfg = rd(io, AgbWinGccSe, AGB_RCG_CFG);
    r->dfsr = rd(io, AgbWinGccSe, AGB_RCG_DFSR);
    if (r->dfsr & 1u) {
        for (unsigned level = 0; level < AGB_DFS_LEVELS; ++level) r->level_cfg[level] = rd(io, AgbWinGccSe, AGB_RCG_PERF(level));
        r->level = aston_gauge_bus_xo_level(r->level_cfg);
        return r->level < 0 ? -95 : 0;
    }
    r->level = -1;
    if ((r->rcg_cmd & AGB_RCG_ROOT_OFF) || !AGB_CFG_XO_UNDIVIDED(r->rcg_cfg)) return -95;
    return 0;
}

/* Waits for any of the bits; returns the status seen (cleared), 0 on timeout. */
static uint32_t wait_bits(const AgbIo *io, uint32_t bits, unsigned us)
{
    for (unsigned waited = 0;; waited += kPollUs) {
        const uint32_t status = rd(io, AgbWinSe, AGB_M_IRQ_STATUS);
        if (status) wr(io, AgbWinSe, AGB_M_IRQ_CLEAR, status);
        if (status & bits) return status;
        if (waited >= us) return 0;
        delay(io, kPollUs);
    }
}

/* Cancels the running command, then aborts it when the cancel is not acknowledged (the engine's documented order). */
static void stop_command(const AgbIo *io)
{
    wr(io, AgbWinSe, AGB_TX_WATERMARK, 0);
    wr(io, AgbWinSe, AGB_M_CMD_CTRL, AGB_CTRL_CANCEL);
    if (!wait_bits(io, AGB_M_CMD_CANCEL, kCancelUs)) {
        wr(io, AgbWinSe, AGB_M_CMD_CTRL, AGB_CTRL_ABORT);
        wait_bits(io, AGB_M_CMD_ABORT, kCancelUs);
    }
    wr(io, AgbWinSe, AGB_M_IRQ_CLEAR, 0xffffffffu);
}

static int engine(const AgbIo *io, AgbReport *r)
{
    r->fw = rd(io, AgbWinSe, AGB_FW_REVISION);
    r->if_disable = rd(io, AgbWinSe, AGB_IF_DISABLE);
    r->geni_status = rd(io, AgbWinSe, AGB_GENI_STATUS);
    r->clk_before = rd(io, AgbWinSe, AGB_M_CLK_CFG);
    r->scl_before = rd(io, AgbWinSe, AGB_SCL_COUNTERS);
    if (((r->fw >> 8) & 0xffu) != AGB_PROTO_I2C) return -71;
    if (r->if_disable & 1u) return -95;   /* FIFO interface off: a GPI DMA engine owns it */
    if (r->geni_status & 1u) {
        /* a command left running (a boot loader's transfer cut short) */
        stop_command(io);
        if (rd(io, AgbWinSe, AGB_GENI_STATUS) & 1u) return -16;
    }
    /* The common setup (interrupt state, clock gating, output defaults), then FIFO mode without an interrupt line. */
    wr(io, AgbWinSe, AGB_GSI_EVENT_EN, 0);
    wr(io, AgbWinSe, AGB_M_IRQ_CLEAR, 0xffffffffu);
    wr(io, AgbWinSe, AGB_S_IRQ_CLEAR, 0xffffffffu);
    wr(io, AgbWinSe, AGB_DMA_TX_IRQ_CLR, 0xffffffffu);
    wr(io, AgbWinSe, AGB_DMA_RX_IRQ_CLR, 0xffffffffu);
    wr(io, AgbWinSe, AGB_CGC_CTRL, rd(io, AgbWinSe, AGB_CGC_CTRL) | 0x7fu);
    wr(io, AgbWinSe, AGB_DMA_GENERAL_CFG, rd(io, AgbWinSe, AGB_DMA_GENERAL_CFG) | 0xfu);
    wr(io, AgbWinSe, AGB_OUTPUT_CTRL, 0x7fu);
    wr(io, AgbWinSe, AGB_FORCE_DEFAULT, 1u);
    wr(io, AgbWinSe, AGB_RX_WATERMARK, 15u);     /* at most 4 bytes are read: one FIFO word, far below any engine's depth */
    wr(io, AgbWinSe, AGB_RX_RFR, 16u);
    wr(io, AgbWinSe, AGB_DMA_MODE, 0);
    wr(io, AgbWinSe, AGB_SE_IRQ_EN, 0);
    wr(io, AgbWinSe, AGB_M_IRQ_EN, AGB_M_WANTED);
    wr(io, AgbWinSe, AGB_TX_PACK0, AGB_PACK0);
    wr(io, AgbWinSe, AGB_TX_PACK1, AGB_PACK1);
    wr(io, AgbWinSe, AGB_RX_PACK0, AGB_PACK0);
    wr(io, AgbWinSe, AGB_RX_PACK1, AGB_PACK1);
    wr(io, AgbWinSe, AGB_BYTE_GRAN, 0);
    wr(io, AgbWinSe, AGB_TX_WATERMARK, 0);
    wr(io, AgbWinSe, AGB_CLK_SEL, r->level > 0 ? (uint32_t)r->level : 0u);
    wr(io, AgbWinSe, AGB_M_CLK_CFG, AGB_CLK_100K);
    wr(io, AgbWinSe, AGB_SCL_COUNTERS, AGB_SCL_100K);
    r->clk_after = rd(io, AgbWinSe, AGB_M_CLK_CFG);
    r->scl_after = rd(io, AgbWinSe, AGB_SCL_COUNTERS);
    if ((r->clk_after & 0xfff1u) != AGB_CLK_100K || (r->scl_after & 0x3fffffffu) != AGB_SCL_100K) return -5;
    return 0;
}

static int pins(const AgbIo *io, AgbReport *r)
{
    static const unsigned windows[2] = {AgbWinSda, AgbWinScl};
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t before = rd(io, windows[i], 0);
        r->pin_before[i] = before;
        const uint32_t want = (before & ~AGB_PIN_MASK) | AGB_PIN_I2C;
        if (want != before) wr(io, windows[i], 0, want);
        r->pin_after[i] = rd(io, windows[i], 0);
        if ((r->pin_after[i] & AGB_PIN_MASK) != AGB_PIN_I2C) return -5;
    }
    return 0;
}

int aston_gauge_bus_bringup(AgbBus *b, const AgbIo *io, AgbReport *report)
{
    AgbReport scratch;
    AgbReport *r = report ? report : &scratch;
    zero(r, sizeof *r);
    r->level = -1;
    if (!b || !io || !io->read || !io->write) return fail(r, AgbStNone, -22);
    zero(b, sizeof *b);
    b->io = *io;
    /* Nothing of the engine is touched before its clocks run (its registers would not answer). */
    int rc = clocks(io, r);
    if (rc) return fail(r, AgbStNone, rc);
    r->stage = AgbStClocks;
    rc = root(io, r);
    if (rc) return fail(r, AgbStClocks, rc);
    r->stage = AgbStRoot;
    rc = engine(io, r);
    if (rc) return fail(r, AgbStRoot, rc);
    r->stage = AgbStEngine;
    /* The pins last: the engine drives its idle levels before it reaches the bus. */
    rc = pins(io, r);
    if (rc) return fail(r, AgbStEngine, rc);
    r->stage = AgbStPins;
    b->ready = 1;
    r->stage = AgbStReady;
    r->error = 0;
    return 0;
}

static int classify(uint32_t status)
{
    if (status & AGB_M_GP_NACK) return -6;
    if (status & AGB_M_GP_BUS) return -71;
    if (status & AGB_M_GP_ARB) return -11;
    return -5;
}

static int failed(AgbBus *b, int error, uint32_t status)
{
    b->failures++;
    b->last_error = error;
    b->last_status = status;
    if (error == -6) b->nacks++;
    else if (error == -110) b->timeouts++;
    else b->bus_errors++;
    return error;
}

/* One command to its CMD_DONE: the TX data go in on the watermark. Returns 0, or the error with the engine idle again. */
static int run(AgbBus *b, uint32_t command, const uint32_t *words, unsigned nwords)
{
    const AgbIo *io = &b->io;
    uint32_t errors = 0;
    unsigned filled = nwords == 0;
    wr(io, AgbWinSe, AGB_M_CMD0, command);
    if (!filled) wr(io, AgbWinSe, AGB_TX_WATERMARK, 1);
    for (unsigned waited = 0;; waited += kPollUs) {
        const uint32_t status = rd(io, AgbWinSe, AGB_M_IRQ_STATUS);
        if (status) wr(io, AgbWinSe, AGB_M_IRQ_CLEAR, status);
        errors |= status & AGB_M_ERRORS;
        if (!filled && (status & AGB_M_TX_WATERMARK) && !errors) {
            for (unsigned i = 0; i < nwords; ++i) wr(io, AgbWinSe, AGB_TX_FIFO, words[i]);
            wr(io, AgbWinSe, AGB_TX_WATERMARK, 0);
            filled = 1;
        }
        if (status & AGB_M_CMD_DONE) {
            /* a failed command also ends with CMD_DONE; its error bits decide */
            wr(io, AgbWinSe, AGB_TX_WATERMARK, 0);
            if (errors) return failed(b, classify(errors), errors);
            if (!filled) return failed(b, -5, status);   /* done before the data went in */
            return 0;
        }
        if (waited >= kPhaseUs) {
            stop_command(io);
            return failed(b, -110, errors | status);
        }
        delay(io, kPollUs);
    }
}

int aston_gauge_bus_xfer(AgbBus *b, uint8_t address, const uint8_t *tx, unsigned txn, uint8_t *rx, unsigned rxn)
{
    if (!b || !b->ready) return -19;
    if (!tx || !txn || txn > 8 || rxn > 4 || (rxn && !rx) || address > 0x7f) return -22;
    const AgbIo *io = &b->io;
    b->transfers++;
    if (clocks_kept(b)) return failed(b, -16, 0);
    uint32_t words[2] = {0, 0};
    for (unsigned i = 0; i < txn; ++i) words[i / 4] |= (uint32_t)tx[i] << (8 * (i % 4));
    wr(io, AgbWinSe, AGB_M_IRQ_CLEAR, 0xffffffffu);
    wr(io, AgbWinSe, AGB_TX_LEN, txn);
    int rc = run(b, AGB_OP_WRITE << 27 | (uint32_t)address << 9 | (rxn ? AGB_STOP_STRETCH : 0), words, (txn + 3) / 4);
    if (rc || !rxn) return rc;
    /* words a transfer cut short left behind would be read as this one's */
    for (unsigned n = 0; n < 16 && (rd(io, AgbWinSe, AGB_RX_FIFO_STATUS) & 0x1ffffffu); ++n) (void)rd(io, AgbWinSe, AGB_RX_FIFO);
    wr(io, AgbWinSe, AGB_RX_LEN, rxn);
    rc = run(b, AGB_OP_READ << 27 | (uint32_t)address << 9, 0, 0);
    if (rc) return rc;
    if (!(rd(io, AgbWinSe, AGB_RX_FIFO_STATUS) & 0x1ffffffu)) return failed(b, -61, 0);
    const uint32_t word = rd(io, AgbWinSe, AGB_RX_FIFO);
    for (unsigned i = 0; i < rxn; ++i) rx[i] = (uint8_t)(word >> (8 * i));
    return 0;
}
