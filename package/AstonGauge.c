/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The bq28z610 reader of the Ace 3 (AstonGauge.h). The read loop, retries and range checks follow UniXNU-Pagani-Drivers BatteryGauge.c
 * (c888efd); register numbers, units and the current's sign are facts of TI's bq28z610 data sheet and of the OPLUS gauge driver's use of
 * this part (oplus_hal_bq27541.c, oplus_mms_gauge.c); no code of that driver is used. */
#include "AstonGauge.h"

static const uint8_t kRegister[AgCount] = {
    [AgVoltage] = AgRegVoltage, [AgCurrent] = AgRegCurrent, [AgTemperature] = AgRegTemperature, [AgSoc] = AgRegSoc,
    [AgStatus] = AgRegStatus, [AgRemaining] = AgRegRemaining, [AgFull] = AgRegFull, [AgAvgCurrent] = AgRegAvgCurrent,
    [AgTimeToEmpty] = AgRegTimeToEmpty, [AgTimeToFull] = AgRegTimeToFull, [AgCycles] = AgRegCycles, [AgSoh] = AgRegSoh,
};
#define SLOW_FIELDS ((1u << AgCycles) | (1u << AgSoh))
#define ALL_FIELDS ((1u << AgCount) - 1u)
#define ZERO_C_DECIKELVIN 2731

static int32_t signed16(uint16_t v) {return v & 0x8000u ? (int32_t)v - 0x10000 : (int32_t)v;}

static void field(AgReading *r, int f, int ok)
{
    if (ok) r->valid |= 1u << f;
    else r->valid &= ~(1u << f);
}

void aston_gauge_decode(AgReading *r, uint32_t fields)
{
    for (int f = 0; f < AgCount; ++f) {
        if (!((fields >> f) & 1u)) continue;
        if (!((r->answered >> f) & 1u)) {field(r, f, 0); continue;}
        const uint16_t raw = r->raw[f];
        switch (f) {
        case AgVoltage:
            /* the series pack: 2.5 .. 4.5 V a cell */
            r->pack_mv = raw;
            field(r, f, raw >= 2500 * AGAUGE_CELLS && raw <= 4500 * AGAUGE_CELLS);
            break;
        case AgCurrent:
        case AgAvgCurrent: {
            const int32_t ma = signed16(raw);
            if (f == AgCurrent) r->current_ma = ma;
            else r->avg_current_ma = ma;
            field(r, f, raw != 0x8000u && ma >= -16000 && ma <= 16000);
            break;
        }
        case AgTemperature:
            /* -40 .. 90 C; a gauge without its thermistor answers far outside */
            r->temperature_centi = ((int32_t)raw - ZERO_C_DECIKELVIN) * 10;
            field(r, f, raw >= ZERO_C_DECIKELVIN - 400 && raw <= ZERO_C_DECIKELVIN + 900);
            break;
        case AgSoc:
            r->soc = raw;
            field(r, f, raw <= 100);
            break;
        case AgStatus:
            r->status = raw;
            field(r, f, 1);
            break;
        case AgRemaining:
            r->remaining_mah = raw;
            field(r, f, raw <= 10000);
            break;
        case AgFull:
            r->full_mah = raw;
            field(r, f, raw >= 500 && raw <= 10000);
            break;
        case AgTimeToEmpty:
        case AgTimeToFull: {
            const int32_t minutes = raw == 0xffffu ? -1 : (int32_t)raw;
            if (f == AgTimeToEmpty) r->time_to_empty_min = minutes;
            else r->time_to_full_min = minutes;
            field(r, f, minutes <= 100 * 60);
            break;
        }
        case AgCycles:
            r->cycles = raw;
            field(r, f, raw <= 20000);
            break;
        case AgSoh:
            r->soh = raw;
            field(r, f, raw <= 100);
            break;
        default:
            break;
        }
    }
}

static int read_one(const AgBus *bus, AgReading *r, int f)
{
    int rc = -5;
    for (unsigned attempt = 0; attempt < AGAUGE_TRIES; ++attempt) {
        if (attempt) {
            r->retries++;
            if (bus->sleep_ms) bus->sleep_ms(bus->ctx, AGAUGE_RETRY_MS);
        }
        uint16_t value = 0;
        r->reads++;
        rc = bus->read16(bus->ctx, kRegister[f], &value);
        if (!rc) {
            r->raw[f] = value;
            r->answered |= 1u << f;
            return 0;
        }
    }
    r->answered &= ~(1u << f);
    r->failures++;
    r->last_error = rc;
    return rc;
}

int aston_gauge_read(const AgBus *bus, AgReading *r, int slow)
{
    if (!bus || !bus->read16 || !r) return -22;
    r->passes++;
    const uint32_t fields = slow ? ALL_FIELDS : ALL_FIELDS & ~SLOW_FIELDS;
    int answered = 0;
    for (int f = 0; f < AgCount; ++f) {
        if (!((fields >> f) & 1u)) continue;
        const int rc = read_one(bus, r, f);
        if (rc && f == AgVoltage) {
            /* The gauge does not answer at all: everything this pass would read is stale now. */
            r->answered &= ~fields;
            aston_gauge_decode(r, fields);
            r->charge_passes = 0;
            return rc;
        }
        if (!rc) answered++;
    }
    aston_gauge_decode(r, fields);
    if (aston_gauge_has(r, AgCurrent) && r->current_ma > AGAUGE_CHARGE_MA) r->charge_passes++;
    else r->charge_passes = 0;
    return answered;
}

void aston_gauge_snapshot(const AgReading *r, ShengBatterySnapshot *s)
{
    *s = (ShengBatterySnapshot){0};
    s->time_to_empty_min = s->time_to_full_min = -1;
    const int volts = aston_gauge_has(r, AgVoltage), amps = aston_gauge_has(r, AgCurrent);
    const int cell_mv = volts ? r->pack_mv / AGAUGE_CELLS : 0;
    s->present = volts;
    s->charge_known = aston_gauge_has(r, AgSoc);
    int percent = s->charge_known ? r->soc : 0;
    /* a 0 % reading with charge left in the cells would start a shutdown */
    if (s->charge_known && percent == 0 && cell_mv > 3450) percent = 1;
    s->percent = percent;
    s->voltage_mv = cell_mv;
    s->current_ma = amps ? r->current_ma * AGAUGE_CELLS : 0;
    /* Without the DSP's charger state, charging is a pack current into the battery seen twice in a row; a full battery the gauge holds
     * in charge mode with no current flowing out counts as on its charger. */
    s->charging = amps && r->charge_passes >= 2;
    s->fully_charged = aston_gauge_has(r, AgStatus) && (r->status & AGAUGE_STATUS_FC) && percent >= 99;
    s->external = s->charging ||
                  (s->fully_charged && amps && r->current_ma >= 0 && !(r->status & AGAUGE_STATUS_DSG));
    s->critical = s->present && !s->external && s->charge_known && percent <= 2 && (!volts || cell_mv < 3500);
    s->temperature_valid = aston_gauge_has(r, AgTemperature);
    s->temperature = s->temperature_valid ? r->temperature_centi : 0;
    s->raw_mah = aston_gauge_has(r, AgRemaining) ? r->remaining_mah * AGAUGE_CELLS : 0;
    s->full_mah = aston_gauge_has(r, AgFull) ? r->full_mah * AGAUGE_CELLS : 0;
    s->design_mah = AGAUGE_DESIGN_MAH;
    s->cycles = aston_gauge_has(r, AgCycles) ? r->cycles : 0;
    if (aston_gauge_has(r, AgTimeToEmpty)) s->time_to_empty_min = r->time_to_empty_min;
    if (aston_gauge_has(r, AgTimeToFull)) s->time_to_full_min = r->time_to_full_min;
    /* the adapter is not known without the DSP: the USB family's "unknown" code while on a charger */
    s->adapter_family = s->external ? 0xe0004007u : 0;
}

int aston_gauge_transfer_allowed(const uint8_t *tx, unsigned txn, unsigned rxn)
{
    return tx && txn == 1 && rxn >= 1 && rxn <= 4 && tx[0] < 0x80;
}
