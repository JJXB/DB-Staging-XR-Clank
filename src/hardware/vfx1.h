// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// VFX-1 VIP-card emulation (ACCESS.bus responder on the VIPPORT bases
// 260h/280h/2A0h/2C0h): install-on-read register answers, echo/ID ROM,
// AB2PCEMPTY response windowing, boot-progress/completion sequences,
// command/data FIFOs, IRQ generation, and diagnostic port logging.
// The raw IO system (handler maps, byte/word/dword dispatch) stays in
// port_containers.cpp, which calls in here.

#ifndef DOSBOX_VFX1_H
#define DOSBOX_VFX1_H

#include "hardware/port.h"

#include <cstdint>

// Shared port-slot tracker (last read/write + validity), used by the
// stereo spies in port_containers.cpp and the VIP responder below.
struct StereoPortSlot {
	bool read_valid   = false;
	bool write_valid  = false;
	uint8_t last_read  = 0;
	uint8_t last_write = 0;
};

// Latch a byte written to a classified VIP port (mirrors AIR-style R/W
// registers, arms the AB2PCEMPTY response window, raises the VIP IRQ
// on command traffic). Called unconditionally from write_byte_to_port.
void vfx1_latch_write(const io_port_t port, const uint8_t value);

// Lower a raised VIP IRQ line (called from the PIC EOI hook in
// write_byte_to_port: the guest ISR EOIs, ending the request).
void vfx1_lower_irq();

// Install one VIP base's answer set on first classified touch (probe-
// first detection must see the card immediately). Called from
// read_byte_from_port; unclaimed ports only, real handlers win.
void vfx1_maybe_install_base(const io_port_t port);

// On-change VIP port access logger (collapsed bursts + identical-poll
// suppression). Called from every byte/word access path.
void log_vfx1_port_access(const io_port_t port, const bool write,
                          const uint8_t value);
void log_vfx1_port_access_wide(const io_port_t port, const bool write,
                               const uint16_t value);

#endif
