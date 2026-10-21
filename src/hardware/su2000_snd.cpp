// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 SND card tracer (temporary bring-up aid).
//
// The SU-2000 SND block is a full Ensoniq Soundscape S-2000 island on FORMAT C:
// base 0x330 (CONFIG.VPC [SND] Port 0x330, IRQ 7, DMA 0), AD1848 codec, OTTO
// ES5506 synth, ODIE ES5706 gate array and an MC68EC000 OBP that runs the
// SNDSCAPE.COD firmware uploaded by the host over ISA DMA.
//
// Host-visible register map, relative to the configured base (the ALSA
// sound/isa/sscape.c layout, which matches the documentation and the local
// SNDSCAPE.COD):
//   +0  MPU-401 data (MIDI mode) / base register
//   +1  MPU-401 command (MIDI mode)
//   +2  HOST_CTRL  bit0 RX_READY, bit1 TX_READY
//   +3  HOST_DATA
//   +4  ODIE_ADDR
//   +5  ODIE_DATA
//   +8  AD1848/WSS codec
//
// This unit is deliberately log-only: every access is recorded with CS:IP and
// reads return open bus (0xFF), so ETS's SND-library probe sequence becomes
// visible. Firmware upload, ODIE register modelling and audio come next.
//
// Next milestones (see PROGRESS.md):
//   1. capture the ETS probe sequence with this tracer;
//   2. answer Soundscape detection (ODIE + host status) without the OBP;
//   3. accept the SNDSCAPE.COD DMA upload into OBP RAM (unswap on upload);
//   4. run the OBP firmware on a 68k core and wire OTTO + AD1848 to the mixer.

#include "hardware/su2000_snd.h"

#include "hardware/port.h"
#include "hardware/su2000_internal.h"

#include <array>
#include <cstdint>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "misc/support.h"

namespace {

constexpr uint16_t Su2000SndPorts = 16;
constexpr uint16_t Su2000SndIrqDefault = 7;
constexpr uint16_t Su2000SndDmaDefault = 0;

uint16_t snd_io_base = 0;
uint16_t snd_irq     = Su2000SndIrqDefault;
uint16_t snd_dma     = Su2000SndDmaDefault;

IO_ReadHandleObject snd_io_read   = {};
IO_WriteHandleObject snd_io_write = {};

struct Su2000SndPortLog {
	uint8_t last    = 0xFF;
	bool last_write = false;
	unsigned repeat = 0;
	bool valid      = false;
};

std::array<Su2000SndPortLog, Su2000SndPorts> snd_port_log = {};

const char* snd_reg_name(const unsigned offset)
{
	switch (offset) {
	case 0x0: return "mpu-data";
	case 0x1: return "mpu-cmd";
	case 0x2: return "host-ctrl";
	case 0x3: return "host-data";
	case 0x4: return "odie-addr";
	case 0x5: return "odie-data";
	case 0x8: return "codec";
	default: return "-";
	}
}

void su2000_snd_trace(const unsigned offset, const bool write,
                      const uint8_t value)
{
	if (!su2000_log_enabled() || offset >= Su2000SndPorts) {
		return;
	}
	auto& log = snd_port_log[offset];
	if (log.valid && value == log.last && write == log.last_write) {
		++log.repeat;
		return;
	}
	if (log.valid && log.repeat > 0) {
	LOG_MSG("SU2000: SND +%02x previous %s repeated x%u",
	        static_cast<unsigned>(offset),
	        log.last_write ? "write" : "read",
	        log.repeat);
	}
	log.last       = value;
	log.last_write = write;
	log.repeat     = 0;
	log.valid      = true;
	LOG_MSG("SU2000: SND %s +%02x (%-9s) = 0x%02x [%04x:%04x]",
	        write ? "write" : "read ",
	        static_cast<unsigned>(offset),
	        snd_reg_name(offset),
	        static_cast<unsigned>(value),
	        static_cast<unsigned>(SegValue(cs)),
	        static_cast<unsigned>(reg_eip & 0xffffu));
}

io_val_t su2000_snd_read(io_port_t port, io_width_t)
{
	su2000_watch_gdt();
	const auto offset = static_cast<unsigned>(port - snd_io_base);
	// No card yet: floating-bus reads, exactly what a missing board shows.
	su2000_snd_trace(offset, false, 0xFF);
	return 0xFF;
}

void su2000_snd_write(io_port_t port, io_val_t val, io_width_t)
{
	su2000_watch_gdt();
	const auto offset = static_cast<unsigned>(port - snd_io_base);
	const auto value  = static_cast<uint8_t>(val & 0xFFu);
	su2000_snd_trace(offset, true, value);
}

} // namespace

void SU2000_Snd_AddConfig(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool = section.AddBool("snd_trap", OnlyAtStart, false);
	pbool->SetHelp(
	        "Log-only trap for the SU-2000 SND card ('off' by default).\n"
	        "The card is an Ensoniq Soundscape S-2000 at 0x330: host control\n"
	        "and data (+2/+3), ODIE gate array (+4/+5), AD1848 codec (+8).\n"
	        "This trap reserves 16 ports at 'snd_io' and logs every access\n"
	        "with CS:IP; reads return open bus (0xFF). No firmware upload,\n"
	        "DMA, synthesis or audio is emulated yet.\n"
	        "\n"
	        "Keep [mpu401] off this base (mpu401 = none) and do not map\n"
	        "[sblaster] here: the Soundscape owns 0x330 on this machine.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	auto phex = section.AddHex("snd_io", OnlyAtStart, 0x330);
	phex->SetHelp(
	        "SU-2000 SND Soundscape host IO base (330 in CONFIG.VPC [SND]\n"
	        "Port 0x330). 16 consecutive ports are reserved, so the base\n"
	        "must fit below 0x3F0.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	auto pint = section.AddInt("snd_irq", OnlyAtStart, Su2000SndIrqDefault);
	pint->SetHelp(
	        "SU-2000 SND IRQ (7 in CONFIG.VPC [SND] IRQ 7). Reserved for\n"
	        "the responder stage; the tracer does not raise interrupts.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pint = section.AddInt("snd_dma", OnlyAtStart, Su2000SndDmaDefault);
	pint->SetHelp(
	        "SU-2000 SND 8-bit DMA channel (0 in CONFIG.VPC [SND] DMA 0).\n"
	        "Reserved for the firmware-upload stage; the tracer does not\n"
	        "perform DMA.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");
}

bool SU2000_Snd_Install(const SectionProp& section)
{
	if (!section.GetBool("snd_trap")) {
		return false;
	}
	const auto io  = static_cast<uint16_t>(section.GetHex("snd_io"));
	const auto irq = static_cast<uint16_t>(section.GetInt("snd_irq"));
	const auto dma = static_cast<uint16_t>(section.GetInt("snd_dma"));

	const bool io_ok  = io >= 0x100 && io <= 0x3E0;
	const bool irq_ok = irq >= 2 && irq <= 15;
	const bool dma_ok = dma <= 3;
	if (!io_ok || !irq_ok || !dma_ok) {
		LOG_WARNING("SU2000: snd trap needs IO 100-3E0, IRQ 2-15 and "
		            "DMA 0-3 (got IO 0x%x IRQ %u DMA %u); SND trap not "
		            "installed",
		            static_cast<unsigned>(io),
		            static_cast<unsigned>(irq),
		            static_cast<unsigned>(dma));
		return false;
	}

	snd_io_base = io;
	snd_irq     = irq;
	snd_dma     = dma;
	snd_port_log.fill(Su2000SndPortLog{});

	snd_io_read.Install(io,
	                    su2000_snd_read,
	                    io_width_t::byte,
	                    Su2000SndPorts);
	snd_io_write.Install(io,
	                     su2000_snd_write,
	                     io_width_t::byte,
	                     Su2000SndPorts);
	su2000_claim_range(io, Su2000SndPorts);

	LOG_MSG("SU2000: SND tracer build %s (IO 0x%03x-0x%03x IRQ %u DMA %u)",
	        SU2000_BUILD_ID,
	        static_cast<unsigned>(snd_io_base),
	        static_cast<unsigned>(snd_io_base + Su2000SndPorts - 1),
	        static_cast<unsigned>(snd_irq),
	        static_cast<unsigned>(snd_dma));
	return true;
}

void SU2000_Snd_Uninstall()
{
	snd_io_read.Uninstall();
	snd_io_write.Uninstall();
	snd_io_base = 0;
	snd_irq     = Su2000SndIrqDefault;
	snd_dma     = Su2000SndDmaDefault;
	snd_port_log.fill(Su2000SndPortLog{});
}

bool SU2000_Snd_Installed()
{
	return snd_io_base != 0;
}

uint16_t SU2000_Snd_IoBase()
{
	return snd_io_base;
}
