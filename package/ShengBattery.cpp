// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery as iOS reads it (boot argument sheng-battery=1, started by BatteryProbe.cpp; needs sheng-adsp=2): an IOPMPowerSource whose
// values come from the battery manager in the audio DSP over the PMIC GLINK channel (ShengBattmgr.c). powerd finds every IOPMPowerSource,
// reads CurrentCapacity/MaxCapacity, IsCharging, ExternalConnected, voltage, amperage and AdapterDetails and listens for the status change
// message updateStatus sends; the status bar, the lock screen's charging state and Settings > Battery follow from that.
//
// Design: everything runs on the service's work loop. The DSP thread's callbacks only copy a message into a small ring and signal an
// IOInterruptEventSource (no provider: a software event), so the DSP thread never waits for this driver. One request is in flight at a
// time; a 2 s timer gives up on an unanswered one. Battery notifications start a refresh at once, and a 60 s timer refreshes in any case
// (the fields that hardly change, every tenth time). Charging itself is the DSP firmware's; the one setting written is the input current
// limit on a computer's USB port (ShengBattmgr.c; boot argument sheng-usb-sdp-ma, 0 leaves the firmware's limit).
//
// aston: sheng-battery=2 takes the values from the pack's fuel gauge instead (bq28z610 on QUP SE8, AstonGaugeBus.c and AstonGauge.c), with
// no DSP: the apps processor reads it over I2C on this service's work loop, read only, every 10 s (2 s until the first publication). The
// charger's own state is not known then: charging is inferred from the pack current (AstonGauge.c). The same properties are published.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "ShengAdsp.h"
#include "ShengBattmgr.h"
#include "AstonGaugeBus.h"
#include "AstonGauge.h"
}

extern "C" void qcom_dwc3_vbus(int) __attribute__((weak_import));
// The USB-C owner (32779) shares this channel: QcomDwc3 asks what is on the cable over UCSI.
extern "C" uint32_t qcom_ucsi_start(uint8_t *) __attribute__((weak_import));
extern "C" uint32_t qcom_ucsi_receive(const uint8_t *, uint32_t, uint8_t *) __attribute__((weak_import));
extern "C" uint32_t qcom_ucsi_tick(uint8_t *) __attribute__((weak_import));
extern "C" bool qcom_ucsi_polling(void) __attribute__((weak_import));
extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
// AstonSocBackup.c: the level shown, for the byte Android shows first after a boot.
extern "C" void aston_soc_backup_note(int percent) __attribute__((weak_import));
// [0] stage (1 class up, 2 channel registered, 3 channel up, 4 published; negative: the step that failed), [1] percent, [2] mV,
// [3] mA, [4] temperature (raw), [5] flags (bit 0 present, 1 external, 2 charging, 3 full, 4 notifications enabled), [6] refresh
// cycles, [7] answers, [8] timeouts | errors << 16, [9] notifications, [10] USB type (raw), [11] full mAh, [12] design mAh,
// [13] cycles, [14] messages dropped (ring full) | ignored << 16, [15] send refusals
extern "C" {extern volatile int32_t native_battery_stats[16];}
// The raw readings behind the values: [0] capacity property (hundredths of a percent), [1] gauge 1 remaining, [2] gauge 1 full, [3] gauge 2
// remaining, [4] gauge 2 full, [5] temperature property, [6] current property (uA, discharge positive), [7] valid field bits, [8] the field
// the last timeout skipped (-1 none)
extern "C" {volatile int32_t native_battery_raw[10];}
// The charger as the firmware reports it (-1 no reading): [0] USB online, [1] USB type, [2] adapter type, [3] the board's adapter type,
// [4] input current limit mA, [5] the most the port offers mA, [6] USB mV, [7] USB mA, [8] battery status, [9] charge type, [10] thermal
// level, [11] charge current mA, [12] adapter watts, [13] bits: 0 cell 1 authenticated, 1 cell 2, 2 PD adapter authenticated, 3 input
// suspended (each with bit +8 set when read), [14] limit sets | tries << 8 | port type << 16, [15] last limit result
extern "C" {volatile int32_t native_charger_stats[16];}
// The gauge path (sheng-battery=2): [0] bring-up stage | error << 8 (AgbReport), [1] bring-ups tried, [2] passes, [3] fields answered in
// the last pass (or its bus error), [4] reads | retries << 16, [5] failures, [6] bus transfers, [7] bus failures | NACKs << 16,
// [8] last bus error, [9] last M_IRQ status of a failure, [10] the DFS level (-1 DFS off), [11] the vote register before the bring-up,
// [12] pack mV, [13] BatteryStatus(), [14] valid field bits, [15] refused transfers
extern "C" {volatile int32_t native_gauge_stats[16];}

namespace {
constexpr uint32_t kRequestMs = 2000, kRefreshMs = 60000, kSlowEvery = 10, kRing = 16, kMessage = 560;
constexpr uint32_t kGaugeMs = 10000, kGaugeFirstMs = 2000, kGaugeRetryMs = 30000, kGaugeBringups = 5;

struct GaugeWindow {uint64_t phys; uint32_t size;};
// In AgbWin* order.
const GaugeWindow kGaugeWindows[AgbWinCount] = {
    {AGB_GCC_VOTE_BASE, AGB_GCC_PAGE}, {AGB_GCC_SE_BASE, AGB_GCC_PAGE}, {AGB_GCC_AHB_BASE, AGB_GCC_PAGE},
    {AGB_TLMM_SDA, AGB_TLMM_PAGE}, {AGB_TLMM_SCL, AGB_TLMM_PAGE}, {AGB_SE_BASE, AGB_SE_SIZE},
};
static_assert(AgbWinGccVote == 0 && AgbWinGccSe == 1 && AgbWinGccAhb == 2 && AgbWinSda == 3 && AgbWinScl == 4 && AgbWinSe == 5 &&
              AgbWinCount == 6, "kGaugeWindows follows the AgbWin order");
}

class ShengBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(ShengBattery)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *request = nullptr, *refresh = nullptr;
    IOInterruptEventSource *inbox = nullptr;
    IOSimpleLock *ringLock = nullptr;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead = 0, ringCount = 0;
    volatile uint32_t channelUp = 0, channelChanged = 0;
    bool wasUp = false, published = false;
    uint32_t refreshes = 0;
    ShengBattmgr bm;
    uint32_t sdpLimit = 0;
    ShengAdspChannel *channel = nullptr;
    IOTimerEventSource *ucsiTimer = nullptr;
    uint8_t ucsiOut[kMessage];
    // The gauge path (sheng-battery=2).
    bool gaugeMode = false;
    IOMemoryMap *gaugeMaps[AgbWinCount] = {};
    volatile uint8_t *gaugeVa[AgbWinCount] = {};
    IOTimerEventSource *gaugeTimer = nullptr;
    AgbBus gaugeBus;
    AgReading gauge;
    uint32_t gaugeBringups = 0, gaugePasses = 0;

    static uint32_t gaugeRead32(void *ctx, unsigned window, uint32_t offset) {
        ShengBattery *self = static_cast<ShengBattery *>(ctx);
        return *reinterpret_cast<volatile uint32_t *>(self->gaugeVa[window] + offset);
    }
    static void gaugeWrite32(void *ctx, unsigned window, uint32_t offset, uint32_t value) {
        ShengBattery *self = static_cast<ShengBattery *>(ctx);
        *reinterpret_cast<volatile uint32_t *>(self->gaugeVa[window] + offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    static void gaugeDelayUs(void *, unsigned us) {IODelay(us);}
    static void gaugeSleepMs(void *, unsigned ms) {IOSleep(ms);}
    // Every transfer to the gauge goes through its allow list (a register address and its read, nothing else).
    static int gaugeRead16(void *ctx, uint8_t reg, uint16_t *value) {
        ShengBattery *self = static_cast<ShengBattery *>(ctx);
        uint8_t rx[2] = {0, 0};
        if (!aston_gauge_transfer_allowed(&reg, 1, 2)) {native_gauge_stats[15] = native_gauge_stats[15] + 1; return -1;}
        const int rc = aston_gauge_bus_xfer(&self->gaugeBus, AGAUGE_ADDRESS, &reg, 1, rx, 2);
        if (!rc) *value = static_cast<uint16_t>(rx[0] | rx[1] << 8);
        return rc;
    }

    void ucsiSend(uint32_t n) {
        if (n) sheng_adsp_send(channel, ucsiOut, n);
        if (qcom_ucsi_polling && qcom_ucsi_polling()) ucsiTimer->setTimeoutMS(20);
    }
    static void ucsiTick(OSObject *owner, IOTimerEventSource *) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (self && self->wasUp && qcom_ucsi_tick) self->ucsiSend(qcom_ucsi_tick(self->ucsiOut));
    }

    static void dspState(void *context, int up) {
        ShengBattery *self = static_cast<ShengBattery *>(context);
        __atomic_store_n(&self->channelUp, up ? 1u : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&self->channelChanged, 1u, __ATOMIC_RELEASE);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }
    static void dspReceive(void *context, const uint8_t *data, uint32_t length) {
        ShengBattery *self = static_cast<ShengBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        if (self->ringCount < kRing && length <= kMessage) {
            auto &slot = self->ring[(self->ringHead + self->ringCount) % kRing];
            memcpy(slot.data, data, length);
            slot.length = length;
            self->ringCount++;
        } else {
            native_battery_stats[14] = native_battery_stats[14] + 1;
        }
        IOSimpleLockUnlock(self->ringLock);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }

    void pump() {
        uint8_t out[SHENG_BATTMGR_REQUEST_BYTES];
        const uint32_t n = sheng_battmgr_next(&bm, out);
        if (!n) return;
        if (sheng_adsp_send(channel, out, n)) native_battery_stats[15] = native_battery_stats[15] + 1;  // the timer skips the field
        request->setTimeoutMS(kRequestMs);
    }
    void number(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
        if (symbol && n) setPSProperty(symbol, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(symbol);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
    void recordRaw() {
        static const int fields[8] = {BmCapacity, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmTemperature, BmCurrent, -1};
        for (unsigned i = 0; i < 7; ++i) native_battery_raw[i] = (bm.valid >> fields[i]) & 1 ? bm.value[fields[i]] : -999999;
        native_battery_raw[7] = (int32_t)bm.valid;
        native_battery_raw[8] = bm.last_timeout;
        static const int charger[13] = {BmUsbOnline, BmUsbType, BmUsbAdapter, BmRealType, BmUsbLimit, BmUsbMax, BmUsbVoltage, BmUsbCurrent,
                                        BmStatus, BmChargeType, BmThermalLevel, BmChargeCurrent, BmPowerMax};
        static const int milli[13] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0};
        if (qcom_dwc3_vbus && (bm.valid >> BmUsbOnline) & 1) qcom_dwc3_vbus(bm.value[BmUsbOnline]);
        for (unsigned i = 0; i < 13; ++i)
            native_charger_stats[i] = (bm.valid >> charger[i]) & 1 ? (milli[i] ? bm.value[charger[i]] / 1000 : bm.value[charger[i]]) : -1;
        static const int bits[4] = {BmAuthentic, BmSlaveAuthentic, BmPdVerified, BmInputSuspend};
        int32_t flags = 0;
        for (unsigned i = 0; i < 4; ++i)
            if ((bm.valid >> bits[i]) & 1) flags |= (bm.value[bits[i]] != 0) << i | 1 << (i + 8);
        native_charger_stats[13] = flags;
        native_charger_stats[14] = (int32_t)(bm.limit_sets | bm.limit_tries << 8 | (uint32_t)(bm.limit_port & 0xff) << 16);
        native_charger_stats[15] = bm.limit_result;
    }
    void publish() {
        ShengBatterySnapshot s;
        sheng_battmgr_snapshot(&bm, &s);
        publishSnapshot(s, true);
    }
    // The properties from a snapshot, whichever path made it (dsp: the battery manager's, with its raw and charger records).
    void publishSnapshot(const ShengBatterySnapshot &s, bool dsp) {
        setBatteryInstalled(s.present);
        setExternalConnected(s.external);
        setExternalChargeCapable(s.external);
        setIsCharging(s.charging);
        setAtCriticalLevel(s.critical);
        setCurrentCapacity((unsigned)s.percent);
        setMaxCapacity(100);
        setVoltage((unsigned)(s.voltage_mv > 0 ? s.voltage_mv : 0));
        setAmperage(s.current_ma);
        if (s.cycles > 0) setCycleCount((unsigned)s.cycles);
        setTimeRemaining(s.charging ? (s.time_to_full_min >= 0 ? s.time_to_full_min : 0xffff)
                                    : (s.time_to_empty_min >= 0 ? s.time_to_empty_min : 0xffff));
        flag("FullyCharged", s.fully_charged);
        number("InstantAmperage", s.current_ma);
        if (s.temperature_valid) number("Temperature", s.temperature);
        if (s.raw_mah > 0) number("AppleRawCurrentCapacity", s.raw_mah);
        if (s.full_mah > 0) {number("AppleRawMaxCapacity", s.full_mah); number("NominalChargeCapacity", s.full_mah);}
        if (s.design_mah > 0) number("DesignCapacity", s.design_mah);
        OSDictionary *adapter = OSDictionary::withCapacity(5);
        if (adapter) {
            OSNumber *family = OSNumber::withNumber(s.adapter_family, 32);
            if (family) {adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family); family->release();}
            if (s.external) {
                OSNumber *w = OSNumber::withNumber((unsigned long long)s.adapter_watts, 32), *mv = OSNumber::withNumber((unsigned long long)s.adapter_mv, 32),
                         *ma = OSNumber::withNumber((unsigned long long)s.adapter_ma, 32);
                if (w) {adapter->setObject(kIOPMPSAdapterDetailsWattsKey, w); w->release();}
                if (mv) {adapter->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
                if (ma) {adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
            }
            const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
            if (key) setPSProperty(key, adapter);
            OSSafeReleaseNULL(key);
            adapter->release();
        }
        native_battery_stats[1] = s.percent; native_battery_stats[2] = s.voltage_mv; native_battery_stats[3] = s.current_ma;
        native_battery_stats[4] = s.temperature;
        native_battery_stats[5] = s.present | s.external << 1 | s.charging << 2 | s.fully_charged << 3 | (dsp ? bm.enabled : 0) << 4;
        native_battery_stats[10] = dsp && (bm.valid >> BmUsbType) & 1 ? bm.value[BmUsbType] : -1;
        native_battery_stats[11] = s.full_mah; native_battery_stats[12] = s.design_mah; native_battery_stats[13] = s.cycles;
        if (dsp) recordRaw();
        if (!published && !s.charge_known) return;  // no charge reading yet: 0 % would read as an empty battery
        if (!published) {
            // The properties are in place before anyone finds the service.
            published = true;
            registerService();
            native_battery_stats[0] = 4;
        }
        updateStatus();
    }
    void counters() {
        native_battery_stats[6] = (int32_t)bm.cycles_done;
        native_battery_stats[7] = (int32_t)bm.responses;
        native_battery_stats[8] = (int32_t)(bm.timeouts | bm.errors << 16);
        native_battery_stats[9] = (int32_t)bm.notifications;
        native_battery_stats[14] = (native_battery_stats[14] & 0xffff) | (int32_t)(bm.ignored << 16);
    }
    void handle(int result) {
        if (result == BmCycleDone) publish();
        pump();
        counters();
    }

    static void inboxAction(OSObject *owner, IOInterruptEventSource *, int) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self) return;
        if (__atomic_exchange_n(&self->channelChanged, 0u, __ATOMIC_ACQ_REL)) {
            const bool up = __atomic_load_n(&self->channelUp, __ATOMIC_ACQUIRE) != 0;
            if (up && !self->wasUp) {
                native_battery_stats[0] = self->published ? 4 : 3;
                uint8_t enable[SHENG_BATTMGR_ENABLE_BYTES];
                sheng_adsp_send(self->channel, enable, sheng_battmgr_enable_request(enable));
                if (qcom_ucsi_start) self->ucsiSend(qcom_ucsi_start(self->ucsiOut));
                sheng_battmgr_refresh(&self->bm, 1);
                self->pump();
                self->refresh->setTimeoutMS(kRefreshMs);
            }
            if (!up && self->wasUp) {self->request->cancelTimeout(); sheng_battmgr_init(&self->bm); self->bm.sdp_limit_ua = self->sdpLimit;}
            self->wasUp = up;
        }
        for (;;) {
            uint8_t data[kMessage];
            uint32_t length = 0;
            IOSimpleLockLock(self->ringLock);
            if (self->ringCount) {
                auto &slot = self->ring[self->ringHead];
                length = slot.length;
                memcpy(data, slot.data, length);
                self->ringHead = (self->ringHead + 1) % kRing;
                self->ringCount--;
            }
            IOSimpleLockUnlock(self->ringLock);
            if (!length) break;
            if (qcom_ucsi_receive && length >= 4 && (data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24) == 32779u) {
                self->ucsiSend(qcom_ucsi_receive(data, length, self->ucsiOut));
                continue;
            }
            const int result = sheng_battmgr_receive(&self->bm, data, length);
            if (result == BmAnswer || result == BmCycleDone) self->request->cancelTimeout();
            self->handle(result);
        }
    }
    static void requestAction(OSObject *owner, IOTimerEventSource *) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->wasUp) return;
        const uint32_t before = self->bm.cycles_done;
        sheng_battmgr_timeout(&self->bm);
        self->handle(self->bm.cycles_done != before ? BmCycleDone : BmAnswer);
    }
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->wasUp) return;
        sheng_battmgr_refresh(&self->bm, ++self->refreshes % kSlowEvery == 0);
        self->handle(BmAnswer);
        sender->setTimeoutMS(kRefreshMs);
    }

    // The gauge path: bring the bus up (at most kGaugeBringups tries, kGaugeRetryMs apart), then one read pass per tick.
    void gaugeCounters(int passResult) {
        native_gauge_stats[2] = (int32_t)gauge.passes;
        native_gauge_stats[3] = passResult;
        native_gauge_stats[4] = (int32_t)(gauge.reads | gauge.retries << 16);
        native_gauge_stats[5] = (int32_t)gauge.failures;
        native_gauge_stats[6] = (int32_t)gaugeBus.transfers;
        native_gauge_stats[7] = (int32_t)(gaugeBus.failures | gaugeBus.nacks << 16);
        native_gauge_stats[8] = gaugeBus.last_error;
        native_gauge_stats[9] = (int32_t)gaugeBus.last_status;
        native_gauge_stats[12] = gauge.pack_mv;
        native_gauge_stats[13] = (int32_t)gauge.status;
        native_gauge_stats[14] = (int32_t)gauge.valid;
        native_battery_raw[0] = gauge.soc; native_battery_raw[1] = gauge.remaining_mah; native_battery_raw[2] = gauge.full_mah;
        native_battery_raw[3] = gauge.pack_mv; native_battery_raw[4] = (int32_t)gauge.status; native_battery_raw[5] = gauge.raw[AgTemperature];
        native_battery_raw[6] = gauge.current_ma; native_battery_raw[7] = (int32_t)gauge.valid; native_battery_raw[8] = gauge.last_error;
    }
    static void gaugeAction(OSObject *owner, IOTimerEventSource *sender) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self) return;
        if (!self->gaugeBus.ready) {
            if (self->gaugeBringups >= kGaugeBringups) return;
            self->gaugeBringups++;
            const AgbIo io = {self, &ShengBattery::gaugeRead32, &ShengBattery::gaugeWrite32, &ShengBattery::gaugeDelayUs};
            AgbReport report;
            const int rc = aston_gauge_bus_bringup(&self->gaugeBus, &io, &report);
            native_gauge_stats[0] = report.stage | (report.error & 0xff) << 8;
            native_gauge_stats[1] = (int32_t)self->gaugeBringups;
            native_gauge_stats[10] = report.level;
            native_gauge_stats[11] = (int32_t)report.vote_before;
            if (rc) {sender->setTimeoutMS(kGaugeRetryMs); return;}
            native_battery_stats[0] = 3;
        }
        const AgBus bus = {self, &ShengBattery::gaugeRead16, &ShengBattery::gaugeSleepMs};
        const int result = aston_gauge_read(&bus, &self->gauge, self->gaugePasses++ % kSlowEvery == 0);
        self->gaugeCounters(result);
        ShengBatterySnapshot s;
        aston_gauge_snapshot(&self->gauge, &s);
        self->publishSnapshot(s, false);
        if (s.charge_known && aston_soc_backup_note) aston_soc_backup_note(s.percent);
        sender->setTimeoutMS(self->published ? kGaugeMs : kGaugeFirstMs);
    }

public:
    // sheng-battery=2: the values from the fuel gauge over I2C, without the DSP.
    bool startGauge() {
        gaugeMode = true;
        loop = IOWorkLoop::workLoop();
        gaugeTimer = IOTimerEventSource::timerEventSource(this, &ShengBattery::gaugeAction);
        if (!loop || !gaugeTimer || loop->addEventSource(gaugeTimer) != kIOReturnSuccess) return false;
        for (unsigned w = 0; w < AgbWinCount; ++w) {
            IODeviceMemory *memory = IODeviceMemory::withRange(kGaugeWindows[w].phys, kGaugeWindows[w].size);
            if (!memory) return false;
            gaugeMaps[w] = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
            memory->release();
            if (!gaugeMaps[w]) return false;
            gaugeVa[w] = reinterpret_cast<volatile uint8_t *>(gaugeMaps[w]->getVirtualAddress());
        }
        native_battery_stats[0] = 2;
        gaugeTimer->setTimeoutMS(1000);
        return true;
    }

    bool startBattery() {
        sheng_battmgr_init(&bm);
        uint32_t ma = 0;
        if (PE_parse_boot_argn("sheng-usb-sdp-ma", &ma, sizeof ma)) bm.sdp_limit_ua = ma > 3000 ? 3000000u : ma * 1000u;
        sdpLimit = bm.sdp_limit_ua;
        loop = IOWorkLoop::workLoop();
        ringLock = IOSimpleLockAlloc();
        inbox = IOInterruptEventSource::interruptEventSource(this, &ShengBattery::inboxAction);
        request = IOTimerEventSource::timerEventSource(this, &ShengBattery::requestAction);
        refresh = IOTimerEventSource::timerEventSource(this, &ShengBattery::refreshAction);
        ucsiTimer = IOTimerEventSource::timerEventSource(this, &ShengBattery::ucsiTick);
        if (!loop || !ringLock || !inbox || !request || !refresh || !ucsiTimer || loop->addEventSource(inbox) != kIOReturnSuccess ||
            loop->addEventSource(request) != kIOReturnSuccess || loop->addEventSource(refresh) != kIOReturnSuccess ||
            loop->addEventSource(ucsiTimer) != kIOReturnSuccess)
            return false;
        native_battery_stats[0] = 1;
        const ShengAdspClient client = {&ShengBattery::dspState, &ShengBattery::dspReceive};
        channel = sheng_adsp_channel ? sheng_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, this) : nullptr;
        if (!channel) return false;
        native_battery_stats[0] = 2;
        return true;
    }
};
OSDefineMetaClassAndStructors(ShengBattery, IOPMPowerSource)

// Called by BatteryProbe.cpp once this file's classes are registered. source: 1 the DSP's battery manager, 2 the fuel gauge (aston).
extern "C" void sheng_battery_start(unsigned source) {
    ShengBattery *battery = OSTypeAlloc(ShengBattery);
    if (!battery || !battery->init() || !battery->attach(IOService::getPlatform())) {
        if (battery) battery->release();
        native_battery_stats[0] = -1;
        return;
    }
    if (!(source == 2 ? battery->startGauge() : battery->startBattery())) {
        native_battery_stats[0] = native_battery_stats[0] > 0 ? -native_battery_stats[0] - 1 : -1;
        battery->detach(IOService::getPlatform());
        battery->release();
        return;
    }
    battery->release();  // the platform's attachment keeps it
}
