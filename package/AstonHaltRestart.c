// SPDX-License-Identifier: MIT
// Restart and power off for the Ace 3: PSCI SYSTEM_RESET and SYSTEM_OFF through the SMC conduit (kalama's psci node, method "smc",
// the conduit ArmDTPlatform's platform-stage reset and BootReset.c use).
//
// No driver of this fileset sets PE_halt_restart, so IOPlatformExpert::haltRestart returned -1 and halt_all_cpus spun until
// PEHaltRestart's 30 s timer panicked "Halt/Restart Timed Out" (iPadOS boots 6 and later: the shutdown notifications were done in
// 826 ms, the Data volume unmounted and synchronized before them). A panic's own restart is left as it was: the panic path calls
// PE_halt_restart with kPEPanicBegin first, and a restart after that returns -1, so the panic screen stays up.
#include <stdint.h>
#include <pexpert/pexpert.h>

extern int (*PE_halt_restart)(unsigned int type);

// IOKit/IOPlatformExpert.h
enum {
  kHaltCPU = 0, kRestartCPU = 1, kPanicRestartCPU = 4, kPanicBegin = 7, kPanicEnd = 8, kPanicRestartCPUNoCallouts = 9,
  kPanicDiagnosticsDone = 10, kPanicDiagnosticsInProgress = 11
};
enum {PsciSystemOff = 0x84000008u, PsciSystemReset = 0x84000009u};

static volatile int panicking;

static void psci(uint64_t fid) {
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = 0, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17");
}

static int aston_halt_restart(unsigned int type) {
  switch (type) {
  case kPanicBegin: case kPanicEnd: case kPanicRestartCPU: case kPanicRestartCPUNoCallouts: case kPanicDiagnosticsDone:
  case kPanicDiagnosticsInProgress:
    panicking = 1;
    return -1;
  case kRestartCPU: case kHaltCPU:
    if (panicking) return -1;
    __asm__ volatile("dsb sy" ::: "memory");
    psci(type == kHaltCPU ? PsciSystemOff : PsciSystemReset);
    return -1;   // PSCI returned: the caller spins and the timer panics, as without this hook
  default:
    return -1;
  }
}

// From the board probes (LegacyDisplayProbe.cpp). aston-halt-restart=0 leaves PE_halt_restart unset. Another driver's hook is kept.
void aston_halt_restart_install(void) {
  uint32_t on = 1;
  PE_parse_boot_argn("aston-halt-restart", &on, sizeof on);
  if (on && !PE_halt_restart) PE_halt_restart = aston_halt_restart;
}
