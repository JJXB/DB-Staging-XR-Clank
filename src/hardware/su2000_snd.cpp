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
// This unit is a trace-first model: every access is recorded with CS:IP and
// the guest return address, and only the ODIE address/data latch is live so
// ETS's Soundscape detection can progress. Everything else reads as open bus
// and no firmware, DMA or audio is emulated yet.
//
// Observed ETS probe (2026-10-22, base 0x330):
//   out 0x334, 0x02   ; GA_DMAA = 0x20
//   out 0x335, 0x20
//   out 0x334, 0x03   ; GA_DMAB = 0x20
//   out 0x335, 0x20
//   out 0x334, 0x00   ; select GA_INTSTAT
//   in  0x334         ; expected ODIE address echo (0x00), got 0xFF
//
// Next milestones (see PROGRESS.md):
//   1. capture the post-detection HOST_CTRL/HOST_DATA traffic;
//   2. accept the SNDSCAPE.COD DMA upload into OBP RAM (unswap on upload);
//   3. run the OBP firmware on a 68k core and wire OTTO + AD1848 to the mixer.

#include "hardware/su2000_snd.h"

#include "hardware/port.h"
#include "hardware/memory.h"
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

// Minimal ODIE state: the address port latches a register index and the data
// port holds 16 registers. Only the latch echo is needed for detection.
uint8_t snd_odie_addr = 0;
std::array<uint8_t, 16> snd_odie_regs = {};

// GA_DMAA bit 0 is the transfer-done flag. ETS starts an upload by writing
// bit 0 set, then polls for the bit to come back set. We complete uploads
// immediately until a real DMA/OBP model exists.
bool snd_dma_done = false;

// Latched once HMCTL is written with bits 6+7 (board boot); the on-board
// processor then answers 0xFE on the host data port.
bool snd_obp_awake = false;

// Power-on GA state. ETS's SND open selects GA_HMCTL (reg 9) and requires
// (value & 0xC0) == 0x40; the other registers start clear.
void snd_odie_reset()
{
	snd_odie_regs.fill(0);
	snd_odie_regs[0x09] = 0x40;
	snd_dma_done = false;
	snd_obp_awake = false;
}

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

uint8_t snd_port_read(const unsigned offset)
{
	switch (offset) {
	case 0x02:
		// Host status: TX ready always, RX ready once the OBP has booted.
		return snd_obp_awake ? 0x03 : 0x02;
	case 0x03:
		// Host data: the on-board processor acknowledges with 0xFE.
		return snd_obp_awake ? 0xFE : 0xFF;
	case 0x04:
		// ODIE address port echoes the latched index in its low nibble.
		// Bits 5-7 stay clear for an ODIE (S-2000; OPUS sets bit 5/6).
		return static_cast<uint8_t>(snd_odie_addr & 0x0F);
	case 0x05: {
		// 0xFE is the ODIE address-echo test register.
		if (snd_odie_addr == 0xFE) {
			return 0x0E;
		}
		const unsigned reg = snd_odie_addr & 0x0F;
		uint8_t value = snd_odie_regs[reg];
		if (reg == 0x02 && snd_dma_done) {
			value |= 0x01;
		}
		return value;
	}
	default:
		return 0xFF;
	}
}

void snd_port_write(const unsigned offset, const uint8_t value)
{
	switch (offset) {
	case 0x04:
		snd_odie_addr = value;
		break;
	case 0x05: {
		const unsigned reg = snd_odie_addr & 0x0F;
		snd_odie_regs[reg] = value;
		if (reg == 0x02 && (value & 0x01)) {
			snd_dma_done = true;
		}
		if (reg == 0x09 && (value & 0xC0) == 0xC0) {
			snd_obp_awake = true;
		}
		break;
	}
	default:
		break;
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
	// The guest reaches the handler from the Watcom byte I/O helpers: the
	// write helper has two saved registers on the stack, the read helper one.
	const auto stack_skip = write ? 8u : 4u;
	const auto caller = mem_readd(SegPhys(ss) + reg_esp + stack_skip);
	LOG_MSG("SU2000: SND %s +%02x (%-9s) = 0x%02x [%04x:%04x] ret %08x",
	        write ? "write" : "read ",
	        static_cast<unsigned>(offset),
	        snd_reg_name(offset),
	        static_cast<unsigned>(value),
	        static_cast<unsigned>(SegValue(cs)),
	        static_cast<unsigned>(reg_eip & 0xffffu),
	        static_cast<unsigned>(caller));
}

io_val_t su2000_snd_read(io_port_t port, io_width_t)
{
	su2000_watch_gdt();
	const auto offset = static_cast<unsigned>(port - snd_io_base);
	const auto value  = snd_port_read(offset);
	su2000_snd_trace(offset, false, value);
	return static_cast<io_val_t>(value);
}

void su2000_snd_write(io_port_t port, io_val_t val, io_width_t)
{
	su2000_watch_gdt();
	const auto offset = static_cast<unsigned>(port - snd_io_base);
	const auto value  = static_cast<uint8_t>(val & 0xFFu);
	snd_port_write(offset, value);
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
	snd_odie_addr = 0;
	snd_odie_reset();
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
	snd_odie_addr = 0;
	snd_odie_reset();
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
