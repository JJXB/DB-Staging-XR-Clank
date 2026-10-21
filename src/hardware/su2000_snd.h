// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 SND: Ensoniq Soundscape S-2000 host-IO bring-up tracer.

#ifndef DOSBOX_SU2000_SND_H
#define DOSBOX_SU2000_SND_H

#include <cstdint>

class SectionProp;

void SU2000_Snd_AddConfig(SectionProp& section);

// Reads 'snd_trap'/'snd_io'/'snd_irq'/'snd_dma'. Returns true when the
// tracer was installed. Claims the SND IO range it owns.
bool SU2000_Snd_Install(const SectionProp& section);

void SU2000_Snd_Uninstall();
bool SU2000_Snd_Installed();

// Configured IO base, or 0 when the tracer is not installed.
uint16_t SU2000_Snd_IoBase();

#endif // DOSBOX_SU2000_SND_H
