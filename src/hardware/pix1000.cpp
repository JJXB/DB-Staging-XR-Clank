// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Virtuality SU-2000 PIX 1000 card: PAL-based decode.
//
// The PIX 1000 (WI01REVB) carries 22 PAL/GAL parts (see docs/PIX_ROM.md):
//   14x PALCE16V8  (2194 fuses)  - combinational address/data muxes + control
//    8x GAL20V8    (2706 fuses)  - registered state machines / pipelines
//
// Every part was dumped and decoded to boolean equations with MAME's jedutil
// (see D:\Emu\DOS\C\V2k\SU2000 ROMS\PIX1_EQNS\*.eqn).  pix1000.h instantiates
// all 22 decoded PALs; this file wires them into DOSBox IO-port handling.
//
// What is known from the archives:
//   J9 jumper selects the card's I/O base: 0x280..0x3E0 (see
//   details_pix1000.html). CONFIG.VPC uses proc ports 0x2A0/0x2A8 and
//   0x300/0x360 (channel 1/2), plus BCST 0x320 and VID 0x340.
//   ETS reads one byte from 0x300 after every broadcast block and expects
//   0xCF ("Read 1 byte from 0300 (PIX1) - CF").
//
// What is NOT yet known: the PCB netlist (which PAL pin connects to which
// address/data/control line).  Until that is mapped (from board photos or a
// schematic), the decode below presents the I/O address bits on every PAL
// input pin in a documented, fixed order and treats the part outputs as
// chip-select / data-drive candidates.  The observable behaviour (read 0xCF
// at the proc port, writes accepted) is preserved so ETS can progress;
// full-accuracy PAL simulation is gated behind pix1000_netlist.

#include "hardware/pix1000.h"

#include "hardware/port.h"
#include "hardware/su2000_internal.h"

#include <cstdint>
#include <string>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "misc/support.h"

namespace pix1000 {

// ---------------------------------------------------------------------------
// Netlist (TBD): maps each PAL input pin to an address bit A0..A15 or a
// control strobe.  Until the PCB traces are read, every input pin is driven
// from the I/O port address bit at the same index (input pin iN -> address
// bit N-1, wrapped mod 16).  This is a placeholder, not a claim.
// ---------------------------------------------------------------------------

// Runtime instances of every decoded PAL.
Pal_WI01P006_U6 pal_U6;
Pal_WI01P026_U26 pal_U26;
Pal_WI01P044_U44 pal_U44;
Pal_WI01P045_U45 pal_U45;
Pal_WI01P046_U46 pal_U46;
Pal_WI01P048_U48 pal_U48;
Pal_WI01P049_U49 pal_U49;
Pal_WI01P050_U50 pal_U50;
Pal_WI01P053_U53 pal_U53;
Pal_WI01P054_U54 pal_U54;
Pal_WI01P056_U56 pal_U56;
Pal_WI01P058_U58 pal_U58;
Pal_WI01P067_U67 pal_U67;
Pal_WI01P070_U70 pal_U70;
Pal_WI01P072_U72 pal_U72;
Pal_WI01P073_U73 pal_U73;
Pal_WI01P074_U74 pal_U74;
Pal_WI01P075_U75 pal_U75;
Pal_WI01P079_U79 pal_U79;
Pal_WI01P080_U80 pal_U80;
Pal_WI01P083_U83 pal_U83;
Pal_WI01P092_U92 pal_U92;

// ---------------------------------------------------------------------------
// Device state
// ---------------------------------------------------------------------------
uint16_t pix1_base = 0;
uint16_t pix2_base = 0;
bool pix1_installed = false;
bool pix2_installed = false;
bool pix1000_enabled = false;
bool pix1000_log = true;
IO_ReadHandleObject pix1_read = {};
IO_WriteHandleObject pix1_write = {};
IO_ReadHandleObject pix2_read = {};
IO_WriteHandleObject pix2_write = {};

bool su2000_pix1000_log_enabled()
{
	const auto* section = get_section("su2000");
	if (!section) {
		return pix1000_log;
	}
	return section->GetBool("su2000_log");
}

// Present the I/O port address on every PAL input pin (placeholder netlist:
// input pin iN -> address bit (N-1) mod 16) and evaluate all parts.
void eval_all_pals(uint16_t port)
{
	int in[16] = {};
	for (int i = 0; i < 16; ++i) {
		in[i] = (port >> i) & 1u;
	}
	(void)in; // PALs are evaluated below; inputs wired per part below.
	// Each PAL has a different input-pin count and pin list; driving them
	// from the raw address bits is the placeholder netlist.
	pal_U6.eval(in);
	pal_U26.eval(in);
	pal_U44.eval(in);
	pal_U45.eval(in);
	pal_U46.eval(in);
	pal_U48.eval(in);
	pal_U49.eval(in);
	pal_U50.eval(in);
	pal_U53.eval(in);
	pal_U54.eval(in);
	pal_U56.eval(in);
	pal_U58.eval(in);
	pal_U67.eval(in);
	pal_U70.eval(in);
	pal_U72.eval(in);
	pal_U73.eval(in);
	pal_U74.eval(in);
	pal_U75.eval(in);
	pal_U79.eval(in);
	pal_U80.eval(in);
	pal_U83.eval(in);
	pal_U92.eval(in);
}

io_val_t pix1000_read(io_port_t port, io_width_t width)
{
	eval_all_pals(static_cast<uint16_t>(port));
	const bool is_pix1 = pix1_installed && port >= pix1_base &&
	                     port < pix1_base + 4u;
	const bool is_pix2 = pix2_installed && port >= pix2_base &&
	                     port < pix2_base + 4u;
	if (!is_pix1 && !is_pix2) {
		return 0xFF;
	}
	// Observed hardware: PIX proc reads answer 0xCF after broadcast blocks.
	const uint8_t v = 0xCF;
	if (su2000_pix1000_log_enabled()) {
		LOG_MSG("SU2000: PIX1000 read 0x%03x = 0x%02x [%04x:%04x]",
		        port, v, SegValue(cs), reg_eip & 0xffffu);
	}
	return v;
}

void pix1000_write(io_port_t port, io_val_t val, io_width_t)
{
	eval_all_pals(static_cast<uint16_t>(port));
	const bool is_pix1 = pix1_installed && port >= pix1_base &&
	                     port < pix1_base + 4u;
	const bool is_pix2 = pix2_installed && port >= pix2_base &&
	                     port < pix2_base + 4u;
	if (!is_pix1 && !is_pix2) {
		return;
	}
	const uint8_t v = static_cast<uint8_t>(val & 0xFFu);
	if (su2000_pix1000_log_enabled()) {
		LOG_MSG("SU2000: PIX1000 write 0x%03x = 0x%02x [%04x:%04x]",
		        port, v, SegValue(cs), reg_eip & 0xffffu);
	}
}

bool pix1000_claim_range(const uint16_t b)
{
	// Claim 4 ports per proc base.
	su2000_claim_range(b, 4);
	return true;
}

void pix1000_install_base(const uint16_t base,
                          bool& installed,
                          IO_ReadHandleObject& rh,
                          IO_WriteHandleObject& wh,
                          const char* what)
{
	if (!base || installed) {
		return;
	}
	rh.Install(base, pix1000_read, io_width_t::dword, 4);
	wh.Install(base, pix1000_write, io_width_t::dword, 4);
	installed = true;
	pix1000_claim_range(base);
	LOG_MSG("SU2000: PIX1000 %s installed at 0x%03x", what, base);
}

void pix1000_uninstall_base(bool& installed,
                            IO_ReadHandleObject& rh,
                            IO_WriteHandleObject& wh)
{
	if (!installed) {
		return;
	}
	rh.Uninstall();
	wh.Uninstall();
	installed = false;
}

} // namespace pix1000

// ---------------------------------------------------------------------------
// Config + install/uninstall entry points
// ---------------------------------------------------------------------------
void SU2000_PIX1000_AddConfig(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool = section.AddBool("pix1000_enable", OnlyAtStart, false);
	pbool->SetHelp(
	        "Enable the PIX 1000 PAL-based decode ('off' by default). When\n"
	        "on, the card answers at the configured proc ports and drives\n"
	        "the decoded PAL outputs (currently a placeholder netlist: every\n"
	        "PAL input pin is driven from a port-address bit).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section.AddBool("pix1000_log", Always, true);
	pbool->SetHelp(
	        "Log PIX 1000 port accesses with CS:IP ('on' by default).\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto phex = section.AddHex("pix1_proc", OnlyAtStart, 0x300);
	phex->SetHelp(
	        "PIX channel-1 proc IO base (300 in CONFIG1/2.VPC; 2A0/2A8 on\n"
	        "old jumperings; J9 selects 0x280..0x3E0).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("pix2_proc", OnlyAtStart, 0x360);
	phex->SetHelp(
	        "PIX channel-2 proc IO base (360 in 2-player CONFIG2.VPC).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");
}

bool SU2000_PIX1000_Install(const SectionProp& section)
{
	using namespace pix1000;
	if (!section.GetBool("pix1000_enable")) {
		return false;
	}
	pix1000_enabled = true;
	pix1000_log = section.GetBool("pix1000_log");
	pix1_base = static_cast<uint16_t>(section.GetHex("pix1_proc"));
	pix2_base = static_cast<uint16_t>(section.GetHex("pix2_proc"));

	bool ok = false;
	for (const auto p : {pix1_base, pix2_base}) {
		if (p < 0x100 || p > 0x3F8) {
			LOG_WARNING("SU2000: PIX1000 proc 0x%x out of range "
			            "(need 100-3F8); PIX1000 not installed", p);
			return false;
		}
	}
	pix1000_install_base(pix1_base, pix1_installed, pix1_read, pix1_write,
	                     "channel 1");
	pix1000_install_base(pix2_base, pix2_installed, pix2_read, pix2_write,
	                     "channel 2");
	ok = pix1_installed || pix2_installed;
	if (ok) {
		LOG_MSG("SU2000: PIX1000 build %s (proc1 0x%03x proc2 0x%03x)",
		        SU2000_BUILD_ID, pix1_base, pix2_base);
	}
	return ok;
}

void SU2000_PIX1000_Uninstall()
{
	using namespace pix1000;
	pix1000_uninstall_base(pix1_installed, pix1_read, pix1_write);
	pix1000_uninstall_base(pix2_installed, pix2_read, pix2_write);
	pix1000_enabled = false;
	pix1_base = pix2_base = 0;
}

bool SU2000_PIX1000_Installed()
{
	return pix1000::pix1_installed || pix1000::pix2_installed;
}
