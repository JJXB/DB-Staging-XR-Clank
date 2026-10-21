// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 missing-hardware traps: BCST 0x320 / PIX proc 0x300+0x360 / VID
// 0x340. Log-only diagnosis for the post-tracker silent ETS exit: ETS is
// quiet on every emulated port for ~33 s, so whatever it probes next is
// invisible. BCST: 16-bit big-endian words (SU2000.md section 4d). PIX proc:
// byte reads answer 0xCF per the observed "Read 1 byte from 0300 (PIX1) - CF"
// after every broadcast block. VID: silent 0xFF. Repeats collapse like the
// tracker.

#include "hardware/su2000_traps.h"

#include "hardware/port.h"
#include "hardware/su2000_internal.h"

#include <cstdint>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "misc/support.h"

namespace {

uint16_t trap_bcst_base = 0;
uint16_t trap_pix1_base = 0;
uint16_t trap_pix2_base = 0;
uint16_t trap_vid_base  = 0;
bool traps_installed    = false;
IO_ReadHandleObject trap_bcst_read     = {};
IO_WriteHandleObject trap_bcst_write   = {};
IO_ReadHandleObject trap_pix1_read     = {};
IO_WriteHandleObject trap_pix1_write   = {};
IO_ReadHandleObject trap_pix2_read     = {};
IO_WriteHandleObject trap_pix2_write   = {};
IO_ReadHandleObject trap_vid_read      = {};
IO_WriteHandleObject trap_vid_write    = {};

struct Su2000TrapLog {
	uint16_t last   = 0xFFFF;
	bool last_write = false;
	unsigned repeat = 0;
	bool valid      = false;
};

Su2000TrapLog trap_log = {};

void su2000_trap_trace(const char* name, const io_port_t port,
                       const bool write, const uint16_t value)
{
	if (!su2000_log_enabled()) {
		return;
	}
	if (trap_log.valid && value == trap_log.last &&
	    write == trap_log.last_write) {
		++trap_log.repeat;
		return;
	}
	if (trap_log.valid && trap_log.repeat > 0) {
		LOG_MSG("SU2000: %s previous value repeated x%u",
		        name,
		        trap_log.repeat);
	}
	trap_log.last       = value;
	trap_log.last_write = write;
	trap_log.repeat     = 0;
	trap_log.valid      = true;
	LOG_MSG("SU2000: %s %s 0x%03x = 0x%04x [%04x:%04x]",
	        name,
	        write ? "write" : "read",
	        port,
	        value,
	        SegValue(cs),
	        reg_eip & 0xffffu);
}

const char* trap_name(const io_port_t port)
{
	const bool is_pix = (port == trap_pix1_base || port == trap_pix2_base);
	return (port == trap_bcst_base)      ? "BCST"
	       : is_pix                      ? "PIX"
	       : (port == trap_vid_base)     ? "VID"
	                                     : "TRAP";
}

io_val_t su2000_trap_read(io_port_t port, io_width_t)
{
	su2000_watch_gdt();
	const bool is_pix = (port == trap_pix1_base || port == trap_pix2_base);
	// Observed hardware value: PIX proc reads 0xCF after broadcast blocks.
	const uint8_t v = is_pix ? 0xCF : 0xFF;
	su2000_trap_trace(trap_name(port), port, false, v);
	return v;
}

void su2000_trap_write(io_port_t port, io_val_t val, io_width_t)
{
	su2000_watch_gdt();
	su2000_trap_trace(trap_name(port),
	                  port,
	                  true,
	                  static_cast<uint16_t>(val & 0xFFFFu));
}

} // namespace

void SU2000_Traps_AddConfig(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool = section.AddBool("bcst_trap", OnlyAtStart, false);
	pbool->SetHelp(
	        "Log-only trap for the SU-2000 broadcast/OSD FIFO ('off' by\n"
	        "default). Logs 16-bit words the guest writes (SU2000.md section\n"
	        "4d); reads return 0xFF. Diagnosis for the post-tracker silent\n"
	        "ETS exit — ETS is quiet on every emulated port for ~33 s, so\n"
	        "whatever it probes next is currently invisible.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	auto phex = section.AddHex("bcst_io", OnlyAtStart, 0x320);
	phex->SetHelp(
	        "Broadcast FIFO IO base (320 in CONFIG1/2.VPC).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section.AddBool("pix_trap", OnlyAtStart, false);
	pbool->SetHelp(
	        "Log-only trap for the PIX 1000 proc ports ('off' by default).\n"
	        "Byte reads answer 0xCF per the observed 'Read 1 byte from 0300\n"
	        "(PIX1) - CF' after every broadcast block (SU2000.md section 4b).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("pix_proc1", OnlyAtStart, 0x300);
	phex->SetHelp(
	        "PIX channel-1 proc port (300 in CONFIG1/2.VPC; 2A0/2A8 on old\n"
	        "jumperings).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("pix_proc2", OnlyAtStart, 0x360);
	phex->SetHelp(
	        "PIX channel-2 proc port (360 in 2-player CONFIG2.VPC).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section.AddBool("vid_trap", OnlyAtStart, false);
	pbool->SetHelp(
	        "Log-only trap for the VID 1000 video port ('off' by default).\n"
	        "Logs accesses; reads return 0xFF.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("vid_io", OnlyAtStart, 0x340);
	phex->SetHelp(
	        "VID 1000 IO base (340 in CONFIG1/2.VPC; JP5: 340/2C0/1C0).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");
}

bool SU2000_Traps_Install(const SectionProp& section,
                          const bool formatc_installed,
                          const uint16_t formatc_io1,
                          const uint16_t formatc_io2)
{
	const bool want_bcst = section.GetBool("bcst_trap");
	const bool want_pix  = section.GetBool("pix_trap");
	const bool want_vid  = section.GetBool("vid_trap");
	if (!want_bcst && !want_pix && !want_vid) {
		return false;
	}

	bool any_trap      = false;
	uint16_t bcst_base = 0, pix1_base = 0, pix2_base = 0, vid_base = 0;
	if (want_bcst) {
		bcst_base = static_cast<uint16_t>(section.GetHex("bcst_io"));
		if (bcst_base < 0x100 || bcst_base > 0x3F8) {
			LOG_WARNING("SU2000: bcst_io 0x%x out of range "
			            "(need 100-3F8); BCST trap not "
			            "installed",
			            bcst_base);
			bcst_base = 0;
		}
	}
	if (want_pix) {
		pix1_base = static_cast<uint16_t>(section.GetHex("pix_proc1"));
		pix2_base = static_cast<uint16_t>(section.GetHex("pix_proc2"));
		for (const auto p : {pix1_base, pix2_base}) {
			if (p < 0x100 || p > 0x3F8) {
				LOG_WARNING("SU2000: pix proc 0x%x out of "
				            "range (need 100-3F8); PIX "
				            "traps not installed",
				            p);
				pix1_base = pix2_base = 0;
				break;
			}
		}
	}
	if (want_vid) {
		vid_base = static_cast<uint16_t>(section.GetHex("vid_io"));
		if (vid_base < 0x100 || vid_base > 0x3F8) {
			LOG_WARNING("SU2000: vid_io 0x%x out of range "
			            "(need 100-3F8); VID trap not "
			            "installed",
			            vid_base);
			vid_base = 0;
		}
	}
	// One 4-port window per trap base keeps installs disjoint: BCST is a
	// single word port, PIX/VID single byte ports. Skip any base already
	// claimed by FORMAT volume blocks.
	auto base_claimed = [&](const uint16_t b) {
		if (formatc_installed &&
		    ((b >= formatc_io1 && b < formatc_io1 + 8) ||
		     (formatc_io2 && b >= formatc_io2 && b < formatc_io2 + 8))) {
			return true;
		}
		return false;
	};
	auto install_trap = [&](const uint16_t b, const char* what,
	                        IO_ReadHandleObject& rh,
	                        IO_WriteHandleObject& wh) {
		if (!b || base_claimed(b)) {
			if (b) {
				LOG_WARNING("SU2000: %s trap 0x%x "
				            "collides with FORMAT volume "
				            "block; not installed",
				            what,
				            b);
			}
			return false;
		}
		rh.Install(b, su2000_trap_read, io_width_t::dword, 1);
		wh.Install(b, su2000_trap_write, io_width_t::dword, 1);
		return true;
	};
	if (bcst_base &&
	    install_trap(bcst_base, "BCST", trap_bcst_read, trap_bcst_write)) {
		trap_bcst_base = bcst_base;
		su2000_claim_range(bcst_base, 1);
		any_trap = true;
	}
	if (pix1_base &&
	    install_trap(pix1_base, "PIX1", trap_pix1_read, trap_pix1_write)) {
		trap_pix1_base = pix1_base;
		su2000_claim_range(pix1_base, 1);
		any_trap = true;
	}
	if (pix2_base &&
	    install_trap(pix2_base, "PIX2", trap_pix2_read, trap_pix2_write)) {
		trap_pix2_base = pix2_base;
		su2000_claim_range(pix2_base, 1);
		any_trap = true;
	}
	if (vid_base &&
	    install_trap(vid_base, "VID", trap_vid_read, trap_vid_write)) {
		trap_vid_base = vid_base;
		su2000_claim_range(vid_base, 1);
		any_trap = true;
	}
	if (any_trap) {
		traps_installed = true;
		LOG_MSG("SU2000: missing-hw traps build %s (BCST 0x%03x PIX 0x%03x/0x%03x VID 0x%03x)",
		        SU2000_BUILD_ID,
		        trap_bcst_base,
		        trap_pix1_base,
		        trap_pix2_base,
		        trap_vid_base);
	}
	return any_trap;
}

void SU2000_Traps_Uninstall()
{
	trap_bcst_read.Uninstall();
	trap_bcst_write.Uninstall();
	trap_pix1_read.Uninstall();
	trap_pix1_write.Uninstall();
	trap_pix2_read.Uninstall();
	trap_pix2_write.Uninstall();
	trap_vid_read.Uninstall();
	trap_vid_write.Uninstall();
	traps_installed = false;
	trap_bcst_base = trap_pix1_base = trap_pix2_base = trap_vid_base = 0;
	trap_log = Su2000TrapLog{};
}

bool SU2000_Traps_Installed()
{
	return traps_installed;
}
