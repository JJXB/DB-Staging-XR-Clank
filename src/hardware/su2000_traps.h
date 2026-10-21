// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 missing-hardware log-only traps: BCST 0x320 broadcast/OSD FIFO,
// PIX proc ports 0x300+0x360 and VID 0x340.

#ifndef DOSBOX_SU2000_TRAPS_H
#define DOSBOX_SU2000_TRAPS_H

#include <cstdint>

class SectionProp;

void SU2000_Traps_AddConfig(SectionProp& section);

// Reads 'bcst_trap'/'pix_trap'/'vid_trap' (and their base keys). The FORMAT
// C IO ranges are passed in so a trap never claims a base already owned by
// the volume block. Returns true when at least one trap was installed.
bool SU2000_Traps_Install(const SectionProp& section,
                          bool formatc_installed,
                          uint16_t formatc_io1,
                          uint16_t formatc_io2);

void SU2000_Traps_Uninstall();
bool SU2000_Traps_Installed();

#endif // DOSBOX_SU2000_TRAPS_H
