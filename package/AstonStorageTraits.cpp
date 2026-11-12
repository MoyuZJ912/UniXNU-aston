// SPDX-License-Identifier: BSD-2-Clause-Patent
// Settings > General > iPhone Storage read "Zero KB of Zero KB used". The sizes come from SpaceAttribution.framework (24A446): SAVolumeSizer
// computes a volume's used and total size only when the block storage device under it is internal, which it decides by the device's
// "Protocol Characteristics" dictionary ("Physical Interconnect Location" = "Internal"); without the dictionary it logs "No 'Protocol
// Characteristics' found, couldn't decide if device (%@) is internal or not" and "Can't compute size of volume, given volume URL (%@) isn't
// internal." and the volume counts as zero. On an iPhone the NAND's storage device carries {Physical Interconnect = Apple Fabric,
// Physical Interconnect Location = Internal}. None of this board's block devices does: the root windows (ShengUfs NativeUfsBlock) and
// the Data store (RamBlock NativeRamBlock). Every block storage device that publishes without one is given that dictionary.
// Boot argument aston-storage-internal=0 turns it off.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

// [0] 1 notification armed (negative: it could not be), [1] devices marked, [2] devices that already had the dictionary
extern "C" {volatile int32_t aston_storage_traits[3];}

namespace {
bool published(void *, void *, IOService *service, IONotifier *) {
    if (!service) return true;
    if (service->getProperty("Protocol Characteristics")) {aston_storage_traits[2] = aston_storage_traits[2] + 1; return true;}
    OSDictionary *traits = OSDictionary::withCapacity(2);
    const OSSymbol *fabric = OSSymbol::withCString("Apple Fabric"), *internal = OSSymbol::withCString("Internal");
    if (traits && fabric && internal) {
        traits->setObject("Physical Interconnect", fabric);
        traits->setObject("Physical Interconnect Location", internal);
        service->setProperty("Protocol Characteristics", traits);
        aston_storage_traits[1] = aston_storage_traits[1] + 1;
    }
    OSSafeReleaseNULL(fabric); OSSafeReleaseNULL(internal); OSSafeReleaseNULL(traits);
    return true;
}
}  // namespace

extern "C" void aston_storage_traits_install(void) {
    uint32_t on = 1;
    PE_parse_boot_argn("aston-storage-internal", &on, sizeof on);
    if (!on || aston_storage_traits[0]) return;
    OSDictionary *matching = IOService::serviceMatching("IOBlockStorageDevice");
    // Kept for good: it also marks the devices that publish after this (the Data store comes up after the root windows).
    IONotifier *notifier = matching ? IOService::addMatchingNotification(gIOPublishNotification, matching, &published, nullptr, nullptr, 0) : nullptr;
    OSSafeReleaseNULL(matching);
    aston_storage_traits[0] = notifier ? 1 : -1;
}
