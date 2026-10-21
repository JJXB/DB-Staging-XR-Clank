// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 NET block tracer (temporary bring-up aid): DP8390 IO window plus
// the 16 KB card-RAM window.

#ifndef DOSBOX_SU2000_NET_H
#define DOSBOX_SU2000_NET_H

#include <cstdint>

class SectionProp;

void SU2000_Net_AddConfig(SectionProp& section);

// Reads 'net_trap'/'net_io'/'net_mem'. Returns true when the trap was
// installed. The port claim is made by the core so the hardwired 0x280
// block is reserved even when the tracer is off.
bool SU2000_Net_Install(const SectionProp& section);

void SU2000_Net_Uninstall();
bool SU2000_Net_Installed();

// Configured IO base, or 0 when the trap is not installed.
uint16_t SU2000_Net_IoBase();

#endif // DOSBOX_SU2000_NET_H
