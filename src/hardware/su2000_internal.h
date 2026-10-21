// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Internal glue shared by the SU-2000 hardware components (su2000_*.cpp).
// This is not part of the public module API; see su2000.h for that.

#ifndef DOSBOX_SU2000_INTERNAL_H
#define DOSBOX_SU2000_INTERNAL_H

#include <cstdint>

// Build stamp: bump on responder changes so logs identify the code.
#define SU2000_BUILD_ID "2026-10-12/formatc-skeleton-v1+trk-answer-v1+trk-stream-v1+trk-pose-v1+trk-readfix-v1+missing-hw-traps-v22"

// 'su2000_log' config gate (on by default when the section is absent).
bool su2000_log_enabled();

// GDT-corruption watch (defined in su2000_gdt.cpp): snapshot GDT[0x168] on
// the first emulated access and report the first change. No-op unless the
// guard has been enabled via SU2000_GDT_SetWanted().
void su2000_watch_gdt();

// Enable/disable the GDT guard for this session (from 'gdt_guard').
void SU2000_GDT_SetWanted(bool wanted);

// Restore GDT pages, drop the guard and clear the corruption watch.
void SU2000_GDT_Uninstall();

// Record an ISA port range owned by the active SU-2000 hardware map (read by
// SU2000_ClaimsPort so the VFX-1 VIP snooper can yield it).
void su2000_claim_range(uint16_t base, uint16_t len);

#endif // DOSBOX_SU2000_INTERNAL_H
