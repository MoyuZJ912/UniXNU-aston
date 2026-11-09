// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ASTON_CPU_FREQ_POLICY_H
#define ASTON_CPU_FREQ_POLICY_H
#include <stdint.h>

// UINT64_MAX is an unavailable/offline processor. Rebase after it comes online
// or its accounting regresses; neither case establishes a busy interval.
static inline uint32_t cpufreq_busy_per_mille(uint64_t idle, uint64_t previous, uint64_t elapsed)
{
    if (!elapsed || idle == UINT64_MAX || previous == UINT64_MAX || idle < previous) return 0;
    const uint64_t delta = idle - previous;
    if (delta >= elapsed) return 0;
    // Governor intervals are milliseconds, but avoid overflow even after a
    // delayed sample. Division first bounds the scaled remainder.
    const uint64_t busy = elapsed - delta;
    if (elapsed > UINT64_MAX / 1000) return (uint32_t)(busy / (elapsed / 1000 + 1));
    return (uint32_t)(busy * 1000 / elapsed);
}
#endif
