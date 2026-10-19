// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Virtuality SU-2000 FORMAT C detection skeleton + tracker sign-on + traps.
//
// ISA map ownership: while SU-2000 is active it owns its documented ports
// (TRK/FORMAT/traps + the hardwired NET 0x280 block from CONFIG1/2.VPC).
// The VFX-1 VIP snooper (base 0x280 collides with SU-2000 NET) must yield
// them: SU2000_ClaimsPort answers for any ISA port.

#ifndef DOSBOX_SU2000_H
#define DOSBOX_SU2000_H

#include <cstdint>

#include "config/config.h"

void SU2000_AddConfigSection(const ConfigPtr& conf);
void SU2000_Init();
void SU2000_Destroy();

// Called from the CPU core on LGDT so the module can guard the extender's GDT
// from the SU-2000 card-image fill (a no-op unless 'gdt_guard' is enabled).
void SU2000_OnGdtLoad(uint32_t base, uint32_t limit);

// True when the SU-2000 module is active and the port belongs to its
// hardware map (configured TRK/FORMAT/trap ranges, hardcoded NET block).
bool SU2000_ClaimsPort(const uint16_t port);

#endif // DOSBOX_SU2000_H
