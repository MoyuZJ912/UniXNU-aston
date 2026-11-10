/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* aston: the board objects without the WiFi stack (build-gop-legacy-display-27.py without ASTON_WIFI=1). The kernel link cannot
 * leave the display and Bluetooth probes' references to it undefined, so these stand in: WiFi does not start, and the shared WCN
 * supplies, which WifiProbe owns, refuse every vote, so Bluetooth reports a power-on failure instead of driving those rails. */
#include "WcnPower.h"

#define WCN_ABSENT 19   /* ENODEV: positive errno, as WcnPower.h asks */

void sheng_wifi_probe(void);
void sheng_wifi_probe(void) {}
int sheng_wcn_power_get(unsigned radio) { (void)radio; return WCN_ABSENT; }
int sheng_wcn_power_put(unsigned radio) { (void)radio; return 0; }
unsigned sheng_wcn_power_owners(void) { return 0; }
