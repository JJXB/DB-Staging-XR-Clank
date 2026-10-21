// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 FORMAT C control window (and the optional FORMAT2 second window
// for 2-player units): 2 KB ISA memory window with a 2 KB open-bus half plus
// the IO+0..7 volume-register block.

#ifndef DOSBOX_SU2000_FORMATC_H
#define DOSBOX_SU2000_FORMATC_H

#include <cstdint>

class SectionProp;

void SU2000_FormatC_AddConfig(SectionProp& section);

// Reads 'formatc'/'formatc_mem'/'formatc_io' (and the FORMAT2 keys when
// 'format2' is on) from the section. Returns true when a window was
// installed. Claims the volume IO ranges it owns.
bool SU2000_FormatC_Install(const SectionProp& section);

void SU2000_FormatC_Uninstall();
bool SU2000_FormatC_Installed();
uint16_t SU2000_FormatC_IoBase1();
uint16_t SU2000_FormatC_IoBase2();

#endif // DOSBOX_SU2000_FORMATC_H
