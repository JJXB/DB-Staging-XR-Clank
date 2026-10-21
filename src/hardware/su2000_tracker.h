// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 tracker: Polhemus InsideTrak minimal answers (TRK1/TRK2).

#ifndef DOSBOX_SU2000_TRACKER_H
#define DOSBOX_SU2000_TRACKER_H

#include <cstdint>

class SectionProp;

void SU2000_Tracker_AddConfig(SectionProp& section);

// Reads 'tracker'/'tracker1'/'tracker2'. Returns true when at least one card
// was installed. Claims the TRK port ranges it owns.
bool SU2000_Tracker_Install(const SectionProp& section);

void SU2000_Tracker_Uninstall();
bool SU2000_Tracker_Installed();

#endif // DOSBOX_SU2000_TRACKER_H
