// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 FORMAT C control window.
//
// 1x 4 KB page, low 2 KB emulated, high 2 KB open bus (0xFF). Command block
// 0x400-0x409 is latched with hardware defaults; the status area is read-only
// from the ISA side (MCU-owned).
//
// Live 2-player config (CONFIG2.VPC) parks FORMAT1 at E0000 and FORMAT2 at
// E0800: both 2 KB windows share one 4 KB page. The handler below covers
// both halves when the bases share a page (base2 = 0 disables the second
// window); a FORMAT2 on a different page is warned-and-skipped for now.

#include "hardware/su2000_formatc.h"

#include "hardware/port.h"
#include "hardware/su2000_internal.h"

#include <array>
#include <cstdint>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "cpu/paging.h"
#include "misc/support.h"

namespace {

constexpr uint32_t Su2000WindowSize = 0x800; // 2 KB control window

// 2 KB window map defaults (SU2000.md section 5, offsets from window base).
constexpr uint8_t Su2000Version   = 0x30; // 3.00 (0x21 = 2.10 on old cards)
constexpr uint8_t Su2000Status    = 0x82; // online; anything else = NOT FOUND
constexpr uint8_t Su2000Bright    = 0xFC;
constexpr uint8_t Su2000JoyCenter = 0x7F;
constexpr uint8_t Su2000MicPeak   = 0xB2; // real value observed on hardware
constexpr uint8_t Su2000CmdSync   = 0xA5;
constexpr uint8_t Su2000Effects   = 0x10;
constexpr uint8_t Su2000PixelShift = 0x05;

// IO+0..7 volume-block read defaults (SU2000.md section 5).
constexpr uint8_t Su2000Vol34 = 0xFF;
constexpr uint8_t Su2000Vol4  = 0xE8; // in the observed C0-E8 range
constexpr uint8_t Su2000Vol567 = 0xFF;

class Su2000FormatCHandler final : public PageHandler {
public:
	Su2000FormatCHandler(uint32_t mem_base, uint32_t mem_base2)
	        : base(mem_base & ~0xFFFu), base2(mem_base2)
	{
		flags = PFLAG_NOCODE; // force every access through readb/writeb
		cmd.fill(0x00);
		cmd[0x01] = Su2000CmdSync;
		cmd[0x04] = Su2000Effects;
		cmd[0x09] = Su2000PixelShift;
		cmd2.fill(0x00);
		cmd2[0x01] = Su2000CmdSync;
		cmd2[0x04] = Su2000Effects;
		cmd2[0x09] = Su2000PixelShift;
		win1_off = mem_base - base;
		win2_off = base2 ? base2 - base : 0xFFFFFFFFu;
	}

	uint8_t readb(PhysPt addr) override
	{
		su2000_watch_gdt();
		const uint32_t offset = addr - base;
		const int win = (offset >= win1_off &&
		                 offset < win1_off + Su2000WindowSize)
		                      ? 1
		                      : (base2 && offset >= win2_off &&
		                         offset < win2_off + Su2000WindowSize)
		                      ? 2
		                      : 0;
		if (!win) {
			return 0xFF; // outside both windows: open bus
		}
		const uint32_t woff = offset - (win == 1 ? win1_off : win2_off);
		if (woff >= 0x400 && woff <= 0x409) {
			return (win == 1 ? cmd : cmd2)[woff - 0x400];
		}
		if (woff >= Su2000WindowSize) {
			return 0xFF;
		}
		if (woff == 0x0007) {
			// Sync toggles A5/5A while online (SU2000.md section 5).
			bool& sync = (win == 1 ? sync_state : sync_state2);
			const uint8_t v = sync ? 0xA5 : 0x5A;
			sync      = !sync;
			maybe_report_detection(win);
			return v;
		}
		if (woff == 0x0000) {
			(win == 1 ? version_seen : version_seen2) = true;
		} else if (woff == 0x0006) {
			(win == 1 ? status_seen : status_seen2) = true;
		}
		const uint8_t v = status_area(woff);
		maybe_report_detection(win);
		return v;
	}

	void writeb(PhysPt addr, uint8_t val) override
	{
		su2000_watch_gdt();
		const uint32_t offset = addr - base;
		const int win = (offset >= win1_off &&
		                 offset < win1_off + Su2000WindowSize)
		                      ? 1
		                      : (base2 && offset >= win2_off &&
		                         offset < win2_off + Su2000WindowSize)
		                      ? 2
		                      : 0;
		if (!win) {
			return;
		}
		const uint32_t woff = offset - (win == 1 ? win1_off : win2_off);
		// Only the command block is ISA-writable on real hardware.
		if (woff >= 0x400 && woff <= 0x409) {
			(win == 1 ? cmd : cmd2)[woff - 0x400] = val;
			if (su2000_log_enabled()) {
				LOG_MSG("SU2000: FORMAT C%d cmd+%02x = 0x%02x [%04x:%08x]",
				        win,
				        static_cast<unsigned>(woff - 0x400),
				        val,
				        SegValue(cs),
				        reg_eip);
			}
		} else if (val == 0x52) {
			// Trace fill-byte writes outside the command block: if a wild
			// rep-stos of 0x52 is walking memory through this window, these
			// lines reveal writer EIP and byte order (direction).
			static uint32_t fill_writes = 0;
			if (fill_writes < 64 && su2000_log_enabled()) {
				LOG_MSG("SU2000: FORMAT C%d fill woff %03x = 0x52 [%04x:%08x]",
				        win,
				        static_cast<unsigned>(woff),
				        SegValue(cs),
				        reg_eip);
			}
			++fill_writes;
		}
		// Status-area writes are MCU-owned: silently ignore.
	}

private:
	static uint8_t status_area(uint32_t offset)
	{
		switch (offset) {
		case 0x0000: return Su2000Version;
		case 0x0001: return 0x00;
		case 0x0002:
		case 0x0003:
		case 0x0004:
		case 0x0005: return 0x00; // packetnum to/from format_d
		case 0x0006: return Su2000Status;
		// 0x0007 handled in readb (toggle).
		case 0x0008:
		case 0x0009: return Su2000Bright;
		case 0x000A:
		case 0x000B:
		case 0x000C:
		case 0x000D: return Su2000JoyCenter;
		case 0x000E:
		case 0x000F:
		case 0x0010:
		case 0x0011: return 0x00; // glove1-4 (unused on SU)
		case 0x0012: return 0x00; // config
		case 0x0013: return 0x00; // switch: none
		case 0x0014: return 0x00; // console
		case 0x0015:
		case 0x0016: return 0x00; // reserved
		case 0x0017: return Su2000MicPeak;
		case 0x0018: return 0x00; // security
		case 0x0019: return 0x00; // backlight
		default: return 0x00; // 0x001A-0x03FF read 0x00
		}
	}

	void maybe_report_detection(const int win)
	{
		bool& reported = (win == 1 ? det_reported : det_reported2);
		const bool seen =
		        (win == 1 ? (version_seen && status_seen)
		                  : (version_seen2 && status_seen2));
		if (reported || !seen) {
			return;
		}
		reported = true;
		if (su2000_log_enabled()) {
			LOG_MSG("SU2000: FORMAT C%d detection sequence OK "
			        "(version 0x%02x + status 0x%02x) build %s",
			        win,
			        Su2000Version,
			        Su2000Status,
			        SU2000_BUILD_ID);
		}
	}

	uint32_t base = 0;
	uint32_t base2 = 0; // 0 = no second window
	uint32_t win1_off = 0;
	uint32_t win2_off = 0xFFFFFFFFu;
	std::array<uint8_t, 0x0A> cmd = {};
	std::array<uint8_t, 0x0A> cmd2 = {};
	bool sync_state    = true; // first sync read returns A5
	bool sync_state2   = true;
	bool version_seen  = false;
	bool status_seen   = false;
	bool det_reported  = false;
	bool version_seen2 = false;
	bool status_seen2  = false;
	bool det_reported2 = false;
};

Su2000FormatCHandler* mem_handler   = nullptr;
PageHandler* saved_page_handler     = nullptr;
uint32_t installed_mem_base         = 0;
uint16_t installed_io_base          = 0;
uint32_t installed_mem_base2        = 0;
uint16_t installed_io_base2         = 0;
bool installed                      = false;
std::array<uint8_t, 8> vol_latch    = {};
std::array<uint8_t, 8> vol_latch2   = {};
bool vol_touched                    = false;

IO_ReadHandleObject io_read_handle    = {};
IO_WriteHandleObject io_write_handle  = {};
IO_ReadHandleObject io2_read_handle   = {};
IO_WriteHandleObject io2_write_handle = {};

io_val_t su2000_io_read(io_port_t port, io_width_t)
{
	unsigned reg = 0;
	bool second  = false;
	if (port >= installed_io_base && port < installed_io_base + 8) {
		reg = static_cast<unsigned>(port - installed_io_base);
	} else if (installed_io_base2 &&
	           port >= installed_io_base2 && port < installed_io_base2 + 8) {
		reg    = static_cast<unsigned>(port - installed_io_base2);
		second = true;
	} else {
		return 0xFF;
	}
	const auto& latch = second ? vol_latch2 : vol_latch;
	uint8_t v         = 0xFF;
	switch (reg) {
	case 0:
	case 1:
	case 2: v = latch[reg]; break; // latched, init 0x00
	case 3: v = Su2000Vol34; break;
	case 4: v = Su2000Vol4; break;
	case 5:
	case 6:
	case 7: v = Su2000Vol567; break;
	default: break;
	}
	return v;
}

void su2000_io_write(io_port_t port, io_val_t val, io_width_t)
{
	unsigned reg = 0;
	bool second  = false;
	if (port >= installed_io_base && port < installed_io_base + 8) {
		reg = static_cast<unsigned>(port - installed_io_base);
	} else if (installed_io_base2 &&
	           port >= installed_io_base2 && port < installed_io_base2 + 8) {
		reg    = static_cast<unsigned>(port - installed_io_base2);
		second = true;
	} else {
		return;
	}
	// MUTE.EXE zeroes every volume register: latch so a readback
	// observer (and the log) can see it happened.
	(second ? vol_latch2 : vol_latch)[reg] = static_cast<uint8_t>(val & 0xFFu);
	vol_touched = true;
	if (su2000_log_enabled()) {
		LOG_MSG("SU2000: FORMAT C%d volume reg %u = 0x%02x",
		        second ? 2 : 1,
		        reg,
		        (second ? vol_latch2 : vol_latch)[reg]);
	}
}

} // namespace

void SU2000_FormatC_AddConfig(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool = section.AddBool("formatc", OnlyAtStart, false);
	pbool->SetHelp(
	        "Emulate the Virtuality SU-2000 FORMAT C control window ('off' by\n"
	        "default). Answers the 2 KB ISA memory-window probes (version 0x30,\n"
	        "status 0x82, A5/5A sync toggle, centred joysticks) and latches the\n"
	        "IO volume-block writes, so MUTE.EXE and the ETS FORMAT register\n"
	        "tests can pass detection. No audio, network or video is emulated;\n"
	        "tracker ports are trace-only (see 'tracker').\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	auto phex = section.AddHex("formatc_mem", OnlyAtStart, 0xCC000);
	phex->SetHelp(
	        "FORMAT C ISA memory-window base (CC000 by default = SU solo in\n"
	        "CONFIG1.VPC). Must be 4 KB page-aligned inside C0000-EFFFF.\n"
	        "The 2-player second head (E0800, not page-aligned) is covered\n"
	        "by 'format2' when it shares this page (E0000 + E0800).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("formatc_io", OnlyAtStart, 0x210);
	phex->SetHelp(
	        "FORMAT C volume-register IO base (210 by default = SU solo in\n"
	        "CONFIG1.VPC). JP3 options: 210, 218, 260, 268, 298, 2C0, 2C8.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section.AddBool("format2", OnlyAtStart, false);
	pbool->SetHelp(
	        "Emulate the second SU-2000 FORMAT C window for 2-player units\n"
	        "('off' by default). Live CONFIG2.VPC parks FORMAT2 at MEM 0xE0800\n"
	        "IO 0x218, sharing one 4 KB page with FORMAT1 at 0xE0000 (both\n"
	        "halves answered; separate command latches). A FORMAT2 on a\n"
	        "different page is warned-and-skipped for now.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("format2_mem", OnlyAtStart, 0xE0800);
	phex->SetHelp(
	        "FORMAT2 ISA memory-window base (E0800 = 2-player in CONFIG2.VPC).\n"
	        "Must be 2 KB aligned inside C0000-EFFFF.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("format2_io", OnlyAtStart, 0x218);
	phex->SetHelp(
	        "FORMAT2 volume-register IO base (218 = 2-player in CONFIG2.VPC).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");
}

bool SU2000_FormatC_Install(const SectionProp& section)
{
	if (!section.GetBool("formatc")) {
		return false;
	}
	const auto mem_base = static_cast<uint32_t>(section.GetHex("formatc_mem"));
	const auto io_base  = static_cast<uint16_t>(section.GetHex("formatc_io"));

	if (mem_base < 0xC0000 || mem_base > 0xEFFFF ||
	    (mem_base & 0xFFFu) != 0u) {
		LOG_WARNING("SU2000: formatc_mem 0x%x out of range or not "
		            "page-aligned (need C0000-EFFFF, 4 KB aligned); "
		            "FORMAT C not installed",
		            mem_base);
		return false;
	}
	if (io_base < 0x100 || io_base > 0x3F8) {
		LOG_WARNING("SU2000: formatc_io 0x%x out of range (need "
		            "100-3F8); FORMAT C not installed",
		            io_base);
		return false;
	}

	installed_mem_base = mem_base;
	installed_io_base  = io_base;
	vol_latch.fill(0x00);
	vol_latch2.fill(0x00);
	vol_touched = false;

	// Optional second window (CONFIG2.VPC: E0800 shares the E0000 page).
	// 2 KB aligned is enough for the second card.
	uint32_t mem_base2 = 0;
	if (section.GetBool("format2")) {
		const auto f2m = static_cast<uint32_t>(section.GetHex("format2_mem"));
		if (f2m < 0xC0000 || f2m > 0xEFFFF || (f2m & 0x7FFu) != 0u) {
			LOG_WARNING("SU2000: format2_mem 0x%x out "
			            "of range or not 2 KB aligned "
			            "(need C0000-EFFFF); FORMAT2 "
			            "not installed",
			            f2m);
		} else if ((f2m >> 12) != (mem_base >> 12)) {
			LOG_WARNING("SU2000: format2_mem 0x%x on a "
			            "different page than formatc_mem "
			            "0x%x (not yet supported); "
			            "FORMAT2 not installed",
			            f2m,
			            mem_base);
		} else if (f2m == mem_base) {
			LOG_WARNING("SU2000: format2_mem 0x%x same "
			            "as formatc_mem; FORMAT2 not "
			            "installed",
			            f2m);
		} else {
			mem_base2 = f2m;
		}
	}
	uint16_t io_base2 = 0;
	if (mem_base2) {
		io_base2 = static_cast<uint16_t>(section.GetHex("format2_io"));
		if (io_base2 < 0x100 || io_base2 > 0x3F8) {
			LOG_WARNING("SU2000: format2_io 0x%x out of "
			            "range (need 100-3F8); FORMAT2 "
			            "not installed",
			            io_base2);
			mem_base2 = 0;
			io_base2  = 0;
		}
	}
	installed_mem_base2 = mem_base2;
	installed_io_base2  = io_base2;

	const Bitu page     = mem_base >> 12;
	saved_page_handler  = MEM_GetPageHandler(page);
	mem_handler         = new Su2000FormatCHandler(mem_base, mem_base2);
	MEM_SetPageHandler(page, 1, mem_handler);
	io_read_handle.Install(io_base,
	                       su2000_io_read,
	                       io_width_t::dword,
	                       8);
	io_write_handle.Install(io_base,
	                        su2000_io_write,
	                        io_width_t::dword,
	                        8);
	if (io_base2) {
		io2_read_handle.Install(io_base2,
		                        su2000_io_read,
		                        io_width_t::dword,
		                        8);
		io2_write_handle.Install(io_base2,
		                         su2000_io_write,
		                         io_width_t::dword,
		                         8);
	}
	PAGING_ClearTLB();
	su2000_claim_range(io_base, 8);
	su2000_claim_range(io_base2, 8);
	installed = true;
	LOG_MSG("SU2000: FORMAT C responder build %s (MEM 0x%05x IO 0x%03x%s)",
	        SU2000_BUILD_ID,
	        mem_base,
	        io_base,
	        mem_base2 ? " + FORMAT2" : "");
	if (mem_base2) {
		LOG_MSG("SU2000: FORMAT C2 window MEM 0x%05x IO 0x%03x",
		        mem_base2,
		        io_base2);
	}
	return true;
}

void SU2000_FormatC_Uninstall()
{
	const bool was_installed = installed;
	io_read_handle.Uninstall();
	io_write_handle.Uninstall();
	io2_read_handle.Uninstall();
	io2_write_handle.Uninstall();

	const Bitu page = installed_mem_base >> 12;
	if (saved_page_handler) {
		MEM_SetPageHandler(page, 1, saved_page_handler);
		saved_page_handler = nullptr;
	} else if (installed_mem_base) {
		MEM_ResetPageHandler(page, 1);
	}
	if (installed_mem_base) {
		PAGING_ClearTLB();
	}
	delete mem_handler;
	mem_handler = nullptr;

	installed_mem_base  = 0;
	installed_mem_base2 = 0;
	installed_io_base   = 0;
	installed_io_base2  = 0;
	installed           = false;
	if (was_installed && su2000_log_enabled()) {
		LOG_MSG("SU2000: FORMAT C responder uninstalled");
	}
}

bool SU2000_FormatC_Installed()
{
	return installed;
}

uint16_t SU2000_FormatC_IoBase1()
{
	return installed_io_base;
}

uint16_t SU2000_FormatC_IoBase2()
{
	return installed_io_base2;
}
