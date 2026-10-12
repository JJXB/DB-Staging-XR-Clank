// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Virtuality SU-2000 FORMAT C detection skeleton (control + volume only).
// See SU2000.md section 5: 2 KB ISA memory window (status/version/sync map)
// plus IO+0..7 volume-register block. No audio/DSP/video emulation here.

#ifndef DOSBOX_SU2000_H
#define DOSBOX_SU2000_H

#include "config/config.h"

void SU2000_AddConfigSection(const ConfigPtr& conf);
void SU2000_Init();
void SU2000_Destroy();

#endif // DOSBOX_SU2000_H
