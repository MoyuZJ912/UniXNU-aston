/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The OnePlus Ace 3 fuel gauge: a TI bq28z610 in the two-cell pack (2S, 2 x 2750 mAh), address 0x55 on QUP SE8 (AstonGaugeBus.h)
 * (aston oplus-chg-23801.dtsi bq27541-battery@55: "qcom,batt_bq28z610", oplus,batt_num = <2>). OPLUS reads it with the bq27541/bq28z610
 * standard commands (16-bit little-endian registers, SMBus word reads), the current from 0x0c (oplus_hal_bq27541.c: BQ28Z610_REG_TI for
 * DEVICE_TYPE_BQ28Z610). Units (TI bq28z610 data sheet): temperature 0.1 K, voltage mV of the whole pack, current mA signed with charge
 * positive, capacities mAh of the series pack, times minutes (0xffff: none), state of charge percent. Register 0x0a is BatteryStatus():
 * FC bit 5, DSG bit 6.
 *
 * What Android shows, and so what iOS is given (oplus_mms_gauge.c): capacities times the cell count (the pack's 2750 mAh read as the
 * 5500 mAh of both cells), the voltage of one cell. Here one cell is half the pack voltage (OPLUS reads the higher cell through a
 * ManufacturerBlockAccess command, which this reader does not send), and the current doubles with the capacities, so that voltage
 * times current stays the pack's power.
 *
 * Read only: nothing is written to the gauge (aston_gauge_transfer_allowed lets a register address and its read through, nothing else;
 * the pack's protection FETs are driven by ManufacturerAccess subcommands of this family). Every value is checked against the range a
 * phone battery can have; a reading outside it is not used. No I/O of its own: the bus operations come from the owner.
 *
 * Ported from UniXNU-Pagani-Drivers drivers/PaganiBattery/BatteryGauge.c (c888efd, BSD-2-Clause-Patent), the OnePlus 13T's reader of an
 * NFG8011B with the same command set; the 2S ranges and conversions are this board's. */
#ifndef ASTON_GAUGE_H
#define ASTON_GAUGE_H
#include <stdint.h>
#include "ShengBattmgr.h"
#ifdef __cplusplus
extern "C" {
#endif

#define AGAUGE_ADDRESS 0x55u
#define AGAUGE_CELLS 2
#define AGAUGE_DESIGN_MAH 5500        /* PJE110 rated capacity of both cells (2 x 2750 mAh) */
#define AGAUGE_STATUS_FC (1u << 5)    /* BatteryStatus(): fully charged */
#define AGAUGE_STATUS_DSG (1u << 6)   /* BatteryStatus(): discharge or relax mode (0: charge mode) */
#define AGAUGE_TRIES 3u
#define AGAUGE_RETRY_MS 5u
#define AGAUGE_CHARGE_MA 50           /* pack current above this, two passes in a row: a charger is feeding the battery */

enum {
    AgRegTemperature = 0x06, AgRegVoltage = 0x08, AgRegStatus = 0x0a, AgRegCurrent = 0x0c, AgRegRemaining = 0x10, AgRegFull = 0x12,
    AgRegAvgCurrent = 0x14, AgRegTimeToEmpty = 0x16, AgRegTimeToFull = 0x18, AgRegCycles = 0x2a, AgRegSoc = 0x2c, AgRegSoh = 0x2e,
};

/* Fields, in reading order; the voltage first (it decides whether the gauge answers at all). */
enum {
    AgVoltage, AgCurrent, AgTemperature, AgSoc, AgStatus, AgRemaining, AgFull, AgAvgCurrent, AgTimeToEmpty, AgTimeToFull, AgCycles,
    AgSoh, AgCount
};

typedef struct {
    void *ctx;
    int (*read16)(void *ctx, uint8_t reg, uint16_t *value);    /* 0, or a negative bus error */
    void (*sleep_ms)(void *ctx, unsigned ms);
} AgBus;

typedef struct {
    uint16_t raw[AgCount];
    uint32_t answered;              /* bit per field: delivered by the bus in the last pass that read it */
    uint32_t valid;                 /* bit per field: answered and in range */
    int32_t pack_mv, current_ma, avg_current_ma;      /* pack values; current: charge positive */
    int32_t temperature_centi;      /* hundredths of a degree Celsius */
    int32_t soc, remaining_mah, full_mah, cycles, soh; /* capacities of the series pack */
    int32_t time_to_empty_min, time_to_full_min;      /* -1: the gauge has no estimate */
    uint32_t status;
    uint32_t charge_passes;         /* consecutive passes with the current above AGAUGE_CHARGE_MA */
    int last_error;                 /* the last bus error (0 none) */
    uint32_t passes, reads, retries, failures;
} AgReading;

/* One pass over the fields (slow: also cycles and health; otherwise those keep their last values). Returns the number of fields answered,
 * or the bus error when the voltage register stays unanswered after its retries (then nothing else is tried). */
int aston_gauge_read(const AgBus *bus, AgReading *r, int slow);
/* Converts the raw words of the given fields (bit per field) and sets their valid bits. */
void aston_gauge_decode(AgReading *r, uint32_t fields);
/* What iOS is shown from the last pass, in the battery manager's snapshot form (ShengBattmgr.h). Nothing is claimed without a reading:
 * charge_known stays 0 until the state of charge is valid, and the power source is not published before that. */
void aston_gauge_snapshot(const AgReading *r, ShengBatterySnapshot *s);
/* The owner passes every transfer to the gauge through this check first: a register address followed by a read of 1..4 bytes. */
int aston_gauge_transfer_allowed(const uint8_t *tx, unsigned txn, unsigned rxn);
static inline int aston_gauge_has(const AgReading *r, int field) {return (int)((r->valid >> field) & 1u);}

#ifdef __cplusplus
}
#endif
#endif
