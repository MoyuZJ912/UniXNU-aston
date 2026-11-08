/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host checks of the Ace 3 fuel gauge path (AstonGaugeBus.c bring-up against a model of the clocks, RCG, engine and pins; AstonGauge.c
 * decoding, the 2S conversion and the transfer allow list). Run by scripts/check-aston-gauge-host.py. */
#include <stdio.h>
#include <string.h>
#include "AstonGaugeBus.h"
#include "AstonGauge.h"

static int failures;
#define CHECK(c) do {if (!(c)) {failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);}} while (0)

/* --- a model of the registers the bring-up touches --- */
typedef struct {
    uint32_t page[AgbWinCount][0x4000 / 4];
    uint32_t writes[AgbWinCount];
    int dfs, xo_level;
} Soc;

static uint32_t soc_read(void *ctx, unsigned w, uint32_t at)
{
    Soc *s = ctx;
    if (w == AgbWinGccAhb || w == AgbWinGccSe) {
        /* a branch reads off until its vote bit is set */
        const uint32_t votes = s->page[AgbWinGccVote][AGB_VOTE / 4];
        uint32_t bit = 0;
        if (w == AgbWinGccSe && at == AGB_CBCR_S0) bit = AGB_VOTE_S0;
        if (w == AgbWinGccAhb && at == AGB_CBCR_CORE) bit = AGB_VOTE_CORE;
        if (w == AgbWinGccAhb && at == AGB_CBCR_S_AHB) bit = AGB_VOTE_S_AHB;
        if (w == AgbWinGccAhb && at == AGB_CBCR_M_AHB) bit = AGB_VOTE_M_AHB;
        if (w == AgbWinGccAhb && at == AGB_CBCR_CORE_2X) bit = AGB_VOTE_CORE_2X;
        if (bit) return votes & bit ? 0u : AGB_CBCR_OFF;
    }
    return s->page[w][at / 4];
}

static void soc_write(void *ctx, unsigned w, uint32_t at, uint32_t v)
{
    Soc *s = ctx;
    s->writes[w]++;
    s->page[w][at / 4] = v;
}

static void soc_init(Soc *s, int dfs, int xo_level)
{
    memset(s, 0, sizeof *s);
    s->page[AgbWinGccVote][AGB_VOTE / 4] = 1u << 9;   /* someone else's vote, kept */
    s->page[AgbWinGccSe][AGB_RCG_DFSR / 4] = dfs ? 1u : 0u;
    for (unsigned l = 0; l < AGB_DFS_LEVELS; ++l)
        s->page[AgbWinGccSe][AGB_RCG_PERF(l) / 4] = (1u << 8) | 5u;   /* GPLL0, divided */
    if (xo_level >= 0) s->page[AgbWinGccSe][AGB_RCG_PERF((unsigned)xo_level) / 4] = 0u;
    s->page[AgbWinGccSe][AGB_RCG_CFG / 4] = dfs ? (1u << 8) : 0u;
    s->page[AgbWinSe][AGB_FW_REVISION / 4] = AGB_PROTO_I2C << 8 | 0x10;
    s->page[AgbWinSda][0] = 0x1c1;   /* gpio mode, 8 mA, pull-down: what a sleep state might leave */
    s->page[AgbWinScl][0] = 0x1c1;
}

static void test_bus(void)
{
    static Soc soc;
    AgbBus bus;
    AgbReport r;
    const AgbIo io = {&soc, soc_read, soc_write, 0};

    soc_init(&soc, 1, 2);
    CHECK(aston_gauge_bus_bringup(&bus, &io, &r) == 0);
    CHECK(r.stage == AgbStReady && bus.ready);
    CHECK(r.voted == 1);
    CHECK(soc.page[AgbWinGccVote][AGB_VOTE / 4] == (AGB_VOTE_BITS | 1u << 9));   /* other votes kept */
    CHECK(r.level == 2);
    CHECK(soc.page[AgbWinSe][AGB_CLK_SEL / 4] == 2);
    CHECK(soc.page[AgbWinSe][AGB_M_CLK_CFG / 4] == AGB_CLK_100K);
    CHECK(soc.page[AgbWinSe][AGB_SCL_COUNTERS / 4] == AGB_SCL_100K);
    CHECK(soc.page[AgbWinSda][0] == 0x00b && soc.page[AgbWinScl][0] == 0x00b);   /* function 2, 2 mA, pull-up */
    CHECK(soc.writes[AgbWinGccSe] == 0 && soc.writes[AgbWinGccAhb] == 0);         /* the RCG and the branches are never written */

    /* DFS on, but no level at the XO: refused before the engine is touched */
    soc_init(&soc, 1, -1);
    CHECK(aston_gauge_bus_bringup(&bus, &io, &r) == -95);
    CHECK(r.stage == AgbStClocks && soc.writes[AgbWinSe] == 0 && !bus.ready);

    /* DFS off: the root itself must run from the XO */
    soc_init(&soc, 0, -1);
    CHECK(aston_gauge_bus_bringup(&bus, &io, &r) == 0 && r.level == -1);
    soc_init(&soc, 0, -1);
    soc.page[AgbWinGccSe][AGB_RCG_CFG / 4] = 1u << 8;
    CHECK(aston_gauge_bus_bringup(&bus, &io, &r) == -95);

    /* an engine without the I2C firmware */
    soc_init(&soc, 1, 0);
    soc.page[AgbWinSe][AGB_FW_REVISION / 4] = 2u << 8;
    CHECK(aston_gauge_bus_bringup(&bus, &io, &r) == -71 && r.stage == AgbStRoot);
    CHECK(soc.page[AgbWinSda][0] == 0x1c1);   /* the pins are untouched */

    /* transfers before a bring-up */
    AgbBus cold;
    memset(&cold, 0, sizeof cold);
    uint8_t reg = 0x08, rx[2];
    CHECK(aston_gauge_bus_xfer(&cold, AGAUGE_ADDRESS, &reg, 1, rx, 2) == -19);

    const uint32_t levels[AGB_DFS_LEVELS] = {0x105, 0x100, 0x001, 0, 0, 0, 0, 0};
    CHECK(aston_gauge_bus_xo_level(levels) == 2);
    const uint32_t mn[AGB_DFS_LEVELS] = {0x2000, 0x105, 0x105, 0x105, 0x105, 0x105, 0x105, 0x003};
    CHECK(aston_gauge_bus_xo_level(mn) == -1);
}

/* --- a model of the gauge's registers --- */
typedef struct {
    uint16_t reg[0x40];
    int dead;
    unsigned reads;
} Gauge;

static int gauge_read16(void *ctx, uint8_t reg, uint16_t *value)
{
    Gauge *g = ctx;
    g->reads++;
    if (g->dead) return -6;
    *value = g->reg[reg];
    return 0;
}

static void pack(Gauge *g, unsigned mv, int ma, unsigned soc, unsigned status)
{
    memset(g, 0, sizeof *g);
    g->reg[AgRegVoltage] = (uint16_t)mv;
    g->reg[AgRegCurrent] = (uint16_t)(int16_t)ma;
    g->reg[AgRegAvgCurrent] = (uint16_t)(int16_t)ma;
    g->reg[AgRegTemperature] = 2981;   /* 25.0 C */
    g->reg[AgRegSoc] = (uint16_t)soc;
    g->reg[AgRegStatus] = (uint16_t)status;
    g->reg[AgRegRemaining] = (uint16_t)(2750 * soc / 100);
    g->reg[AgRegFull] = 2750;
    g->reg[AgRegTimeToEmpty] = ma < 0 ? 300 : 0xffff;
    g->reg[AgRegTimeToFull] = ma > 0 ? 45 : 0xffff;
    g->reg[AgRegCycles] = 120;
    g->reg[AgRegSoh] = 97;
}

static void test_gauge(void)
{
    Gauge g;
    AgReading r;
    ShengBatterySnapshot s;
    const AgBus bus = {&g, gauge_read16, 0};

    /* discharging: 7.70 V pack, 500 mA out, 64 % */
    pack(&g, 7700, -500, 64, AGAUGE_STATUS_DSG);
    memset(&r, 0, sizeof r);
    CHECK(aston_gauge_read(&bus, &r, 1) == AgCount);
    aston_gauge_snapshot(&r, &s);
    CHECK(s.present && s.charge_known && s.percent == 64);
    CHECK(s.voltage_mv == 3850 && s.current_ma == -1000);
    CHECK(s.raw_mah == 2 * 1760 && s.full_mah == 5500 && s.design_mah == AGAUGE_DESIGN_MAH);
    CHECK(!s.charging && !s.external && !s.fully_charged && !s.critical);
    CHECK(s.temperature_valid && s.temperature == 2500);
    CHECK(s.time_to_empty_min == 300 && s.time_to_full_min == -1 && s.cycles == 120);
    CHECK(s.adapter_family == 0);

    /* charging: the current into the pack, seen twice before it counts */
    pack(&g, 8200, 1500, 70, 0);
    memset(&r, 0, sizeof r);
    aston_gauge_read(&bus, &r, 0);
    aston_gauge_snapshot(&r, &s);
    CHECK(!s.charging);
    aston_gauge_read(&bus, &r, 0);
    aston_gauge_snapshot(&r, &s);
    CHECK(s.charging && s.external && s.current_ma == 3000 && s.time_to_full_min == 45);
    CHECK(s.adapter_family == 0xe0004007u);
    pack(&g, 8200, -200, 70, AGAUGE_STATUS_DSG);
    aston_gauge_read(&bus, &r, 0);
    aston_gauge_snapshot(&r, &s);
    CHECK(!s.charging && !s.external);

    /* full on the charger: fully charged, charge mode, no current out */
    pack(&g, 8800, 0, 100, AGAUGE_STATUS_FC);
    memset(&r, 0, sizeof r);
    aston_gauge_read(&bus, &r, 0);
    aston_gauge_snapshot(&r, &s);
    CHECK(s.fully_charged && s.external && !s.charging);

    /* 0 % with charge left in the cells shows 1 %; low and draining is critical */
    pack(&g, 6950, -300, 0, AGAUGE_STATUS_DSG);   /* 3475 mV a cell */
    memset(&r, 0, sizeof r);
    aston_gauge_read(&bus, &r, 0);
    aston_gauge_snapshot(&r, &s);
    CHECK(s.percent == 1 && s.critical);
    pack(&g, 6800, -300, 2, AGAUGE_STATUS_DSG);
    memset(&r, 0, sizeof r);
    aston_gauge_read(&bus, &r, 0);
    aston_gauge_snapshot(&r, &s);
    CHECK(s.percent == 2 && s.critical);

    /* a single-cell reading is out of the 2S range: not used */
    pack(&g, 3900, -300, 50, AGAUGE_STATUS_DSG);
    memset(&r, 0, sizeof r);
    aston_gauge_read(&bus, &r, 0);
    aston_gauge_snapshot(&r, &s);
    CHECK(!s.present && s.voltage_mv == 0 && s.charge_known);

    /* no gauge: only the voltage is tried, three times; nothing is claimed */
    pack(&g, 7700, -500, 64, 0);
    g.dead = 1;
    memset(&r, 0, sizeof r);
    CHECK(aston_gauge_read(&bus, &r, 1) == -6);
    CHECK(g.reads == AGAUGE_TRIES && r.retries == AGAUGE_TRIES - 1);
    aston_gauge_snapshot(&r, &s);
    CHECK(!s.present && !s.charge_known && s.percent == 0 && !s.critical);

    /* out of range words: a resetting gauge answers 0xffff */
    pack(&g, 7700, -500, 0xffff, 0);
    g.reg[AgRegFull] = 0xffff;
    g.reg[AgRegTemperature] = 0xffff;
    memset(&r, 0, sizeof r);
    aston_gauge_read(&bus, &r, 0);
    aston_gauge_snapshot(&r, &s);
    CHECK(!s.charge_known && !s.full_mah && !s.temperature_valid);

    /* the allow list: a register address and a read of 1..4 bytes, nothing else */
    const uint8_t reg = AgRegVoltage, block[3] = {0x3e, 0x01, 0x00}, fet[3] = {0x3e, 0x1f, 0x00}, high = 0x80;
    CHECK(aston_gauge_transfer_allowed(&reg, 1, 2));
    CHECK(!aston_gauge_transfer_allowed(&reg, 1, 0));
    CHECK(!aston_gauge_transfer_allowed(&reg, 1, 5));
    CHECK(!aston_gauge_transfer_allowed(block, 3, 0));
    CHECK(!aston_gauge_transfer_allowed(fet, 3, 0));
    CHECK(!aston_gauge_transfer_allowed(&high, 1, 2));
}

int main(void)
{
    test_bus();
    test_gauge();
    if (failures) {printf("%d check(s) failed\n", failures); return 1;}
    printf("PASS: aston gauge bus bring-up, gauge decoding, 2S conversion and allow list\n");
    return 0;
}
