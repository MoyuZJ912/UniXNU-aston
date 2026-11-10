/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* aston: the board objects without the WiFi stack (build-gop-legacy-display-27.py without ASTON_WIFI=1). The kernel link cannot
 * leave the display and Bluetooth probes' references to it undefined, so these stand in: WiFi does not start, and the shared WCN
 * supplies, which WifiProbe owns, refuse every vote, so Bluetooth reports a power-on failure instead of driving those rails. */
#include <stdint.h>
#include "WcnPower.h"

#define WCN_ABSENT 19   /* ENODEV: positive errno, as WcnPower.h asks */

void sheng_wifi_probe(void);
void sheng_wifi_probe(void) {}
int sheng_wcn_power_get(unsigned radio) { (void)radio; return WCN_ABSENT; }
int sheng_wcn_power_put(unsigned radio) { (void)radio; return 0; }
unsigned sheng_wcn_power_owners(void) { return 0; }

/* WifiProbe's log rings and register snapshot, which BootLog declares weak at their full sizes. Without these the source storage
 * build gives each a zero default of that size: 72 KiB of __DATA, more than the iPhone17,2 collection leaves before Libm's data.
 * BootLog reads the rings only below their counts, the snapshot only after a nonzero first word and the dump only after
 * native_wifi_stats[20], all of which stay zero here, so one element of each is never read past. */
extern char native_wifi_text[1][112];
extern volatile uint32_t native_wifi_text_n;
extern char native_wifi_note[1][112];
extern volatile uint32_t native_wifi_note_n;
extern volatile uint32_t native_wifi_pmic[1];
extern volatile uint32_t native_wifi_dump[1];
char native_wifi_text[1][112];
volatile uint32_t native_wifi_text_n;
char native_wifi_note[1][112];
volatile uint32_t native_wifi_note_n;
volatile uint32_t native_wifi_pmic[1];
volatile uint32_t native_wifi_dump[1];
