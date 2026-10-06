// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_PARALLEL_H
#define DOSBOX_PARALLEL_H

#include "config/config.h"

void PARALLEL_AddConfigSection(const ConfigPtr& conf);
void PARALLEL_Init();
void PARALLEL_Destroy();

#endif // DOSBOX_PARALLEL_H
