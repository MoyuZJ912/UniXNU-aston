// SPDX-License-Identifier: MIT
// The battery level Android shows first after a boot. OPLUS's charger code keeps the level it last showed in a PMIC scratch byte (the
// audio DSP writes it for oplus_chg_8350_backup_soc, BATT_RTC_SOC) and, when Android starts, shows that byte instead of the gauge's level
// whenever the two are less than 20 apart (oplus_chg_comm.c, shutdown_soc), then lets the shown level drift to the gauge's. Without this
// file the byte still holds the level Android had before iOS ran: iOS showed 20 %, Android then 33 %.
// The byte is PMK8550 (SID 0) register 0x8250, SDAM peripheral 0x82 at offset 0x50: read on the phone under Android (2026-11-12) through
// the regmap of 0-00, it went 0x0b -> 0x0a in the same second the shown level went 11 -> 10, the only stable byte of 0x7000..0x9dff that
// did. Each change of the iOS level is written there through the SPMI arbiter (BootResetCore.h), as Android writes each change of its own.
// Nothing is written unless the arbiter gives this EE write ownership of the peripheral and the byte already reads as a level (0..100).
// Boot argument aston-soc-backup=0 turns it off.
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "BootResetCore.h"

enum {SpmiCore = 0xc400000, SpmiCoreSize = 0x3000, SpmiObsrvr = 0xc440000, SpmiChnls = 0xc500000, SpmiCnfg = 0xc42d000, SpmiCnfgSize = 0x4000};
enum {SocPpid = 0x082, SocCell = 0x50};

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);

// [0] state (1 ready; negative: the refused step, -5 owned by another EE, -6 the byte is not a level, -8 eight writes failed), [1] APID, [2] its owner EE,
// [3] the byte when iOS started, [4] the last level written, [5] writes, [6] failed writes or read-backs, [7] the last error
volatile int32_t aston_soc_backup_stats[8];

static struct br_arbiter arbiter;
static struct br_found found;
static volatile uint32_t *observer, *channels;

static volatile uint32_t *map(uint64_t phys, uint64_t size) {return (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)phys, (vm_size_t)size, 7);}

static int open_cell(void) {
  uint32_t on = 1;
  PE_parse_boot_argn("aston-soc-backup", &on, sizeof on);
  if (!on) return -1;
  arbiter.ee = 0;   // qcom,ee = <0>
  arbiter.core = map(SpmiCore, SpmiCoreSize);
  arbiter.cnfg = map(SpmiCnfg, SpmiCnfgSize);
  if (!arbiter.core || !arbiter.cnfg) return -2;
  if (br_find_ppid(&arbiter, SocPpid, &found)) return -3;
  aston_soc_backup_stats[1] = found.apid;
  aston_soc_backup_stats[2] = found.owner;
  observer = map(SpmiObsrvr, ((uint64_t)BR_OBS_APID_STRIDE * ((uint64_t)found.apid + 1) + 0xfff) & ~0xfffull);
  if (!observer) return -4;
  uint8_t cell = 0xff;
  const int rc = br_read8(observer, &arbiter, found.apid, SocCell, &cell);
  aston_soc_backup_stats[3] = rc ? -rc : cell;
  if (found.owner != arbiter.ee) return -5;
  if (rc || cell > 100) return -6;
  channels = map(SpmiChnls, (uint64_t)BR_CHNL_APID_STRIDE * ((uint64_t)found.apid + 1));
  return channels ? 1 : -7;
}

// The level iOS shows now (ShengBattery.cpp, the gauge path, on its work loop): written when it differs from the last one written.
void aston_soc_backup_note(int percent) {
  if (percent < 0 || percent > 100) return;
  if (!aston_soc_backup_stats[0]) aston_soc_backup_stats[0] = open_cell();
  if (aston_soc_backup_stats[0] != 1) return;
  if (aston_soc_backup_stats[5] && aston_soc_backup_stats[4] == percent) return;
  uint8_t back = 0xff;
  int rc = br_write8(channels, found.apid, SocCell, (uint8_t)percent);
  if (!rc) rc = br_read8(observer, &arbiter, found.apid, SocCell, &back) ? 3 : back == (uint8_t)percent ? 0 : 4;
  if (rc) {
    aston_soc_backup_stats[6] = aston_soc_backup_stats[6] + 1;
    aston_soc_backup_stats[7] = rc;
    if (aston_soc_backup_stats[6] >= 8) aston_soc_backup_stats[0] = -8;   // the arbiter keeps refusing: stop trying
    return;
  }
  aston_soc_backup_stats[4] = percent;
  aston_soc_backup_stats[5] = aston_soc_backup_stats[5] + 1;
}
