// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Virtuality SU-2000 FORMAT C detection skeleton + tracker sign-on + traps.
//
// What this answers (SU2000.md sections 4-5, 8): the 2 KB ISA memory window
// every SU-2000 binary probes first (version/status/sync/joystick map, now
// with optional FORMAT2 second window sharing the E0000 page) plus the
// IO+0..7 volume-register block MUTE.EXE zeroes; the InsideTrak open/init
// 'W' probe is answered with the TRACK1-firmware sign-on
// ("InsideTRAK VER.151.03"); BCST/PIX/VID log-only traps make post-tracker
// ETS probes visible. Nothing else: no Ensoniq audio, no N80C198 MCU, no
// FORMAT D, no PIX/VID video, no tracker streaming, no net.
//
// Live contract (SU2000.md sections 10-11): CONFIG1.VPC SU solo expects
// FORMAT1 at MEM 0xCC000 IO 0x210; ETS TEST REGISTERS shows the same map;
// MUTE prints "Write 0 to address %lx register %lx" per volume register.
// Success = MUTE runs instead of "Could not open config file" and ETS no
// longer reports "FORMAT-C NOT FOUND" (status reads 0x82).

#include "hardware/su2000.h"

#include "hardware/port.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "cpu/paging.h"
#include "misc/support.h"

// Build stamp: bump on responder changes so logs identify the code.
#define SU2000_BUILD_ID "2026-10-12/formatc-skeleton-v1+trk-answer-v1+missing-hw-traps-v10"

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

bool su2000_log_enabled()
{
	const auto* section = get_section("su2000");
	if (!section) {
		return true;
	}
	return section->GetBool("su2000_log");
}

// FORMAT C control window: 1x 4 KB page, low 2 KB emulated, high 2 KB open
// bus (0xFF). Command block 0x400-0x409 is latched with hardware defaults;
// the status area is read-only from the ISA side (MCU-owned).
//
// Live 2-player config (CONFIG2.VPC) parks FORMAT1 at E0000 and FORMAT2 at
// E0800: both 2 KB windows share one 4 KB page. The handler below covers
// both halves when the bases share a page (base2 = 0 disables the second
// window); a FORMAT2 on a different page is warned-and-skipped for now.
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
				LOG_MSG("SU2000: FORMAT C%d cmd+%02x = 0x%02x",
				        win,
				        static_cast<unsigned>(woff - 0x400),
				        val);
			}
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

// ---- Missing-hardware traps: BCST 0x320 / PIX proc 0x300+0x360 / VID 0x340.
// Log-only diagnosis for the post-tracker silent ETS exit: ETS is quiet on
// every emulated port for ~33 s, so whatever it probes next is invisible.
// BCST: 16-bit big-endian words (SU2000.md section 4d). PIX proc: byte reads
// answer 0xCF per the observed "Read 1 byte from 0300 (PIX1) - CF" after
// every broadcast block. VID: silent 0xFF. Repeats collapse like the tracker.
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
	uint16_t last    = 0xFFFF;
	bool last_write  = false;
	unsigned repeat  = 0;
	bool valid       = false;
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

io_val_t su2000_trap_read(io_port_t port, io_width_t)
{
	const bool is_pix = (port == trap_pix1_base || port == trap_pix2_base);
	const char* name  = (port == trap_bcst_base)    ? "BCST"
	                        : is_pix                ? "PIX"
	                        : (port == trap_vid_base) ? "VID"
	                                                  : "TRAP";
	// Observed hardware value: PIX proc reads 0xCF after broadcast blocks.
	const uint8_t v = is_pix ? 0xCF : 0xFF;
	su2000_trap_trace(name, port, false, v);
	return v;
}

void su2000_trap_write(io_port_t port, io_val_t val, io_width_t)
{
	const bool is_pix = (port == trap_pix1_base || port == trap_pix2_base);
	const char* name  = (port == trap_bcst_base)      ? "BCST"
	                        : is_pix                  ? "PIX"
	                        : (port == trap_vid_base) ? "VID"
	                                                  : "TRAP";
	su2000_trap_trace(name, port, true, static_cast<uint16_t>(val & 0xFFFFu));
}

io_val_t su2000_io_read(io_port_t port, io_width_t)
{
	unsigned reg   = 0;
	bool second    = false;
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

// ---- Tracker: Polhemus InsideTrak minimal answers ----
// Two ISA cards (TRK1/TRK2, 8 ports each). Observed TRK-library open
// sequence (ETS, "Trying to open card %ld as an InsideTRAK at address
// 0x%lx"): read status +0x1 once, write 'W' to data +0x0, then poll data
// +0x0 for a reply (hangs on floating-bus 0xFF). The reply is the card
// sign-on: the library checks it for the "InsideTRAK" string ("InsideTRAK
// string not found in correct place" otherwise). Sign-on anchored in the
// TRACK1 firmware (VER_151_03 U16 holds a UTF-16LE block:
// "0123456789ABCDEF", "*** ", "151.03",
// "    InsideTRAK CPG2047-001-01   ", " XYZxyz", " AERaer" at ~0x149E;
// U17 is the zero high-half): "InsideTRAK" at offset 0 is what ETS matches,
// "VER.151.03" is the TRACK1 firmware id (SU2000.md section 8), CRLF
// terminator per Polhemus ASCII convention.
//
// Disassembled wait loop (ETS code ~0x1CB142, via stuck-dump backtrace;
// helpers: status reader ~0x1CCE46, WORD data reader ~0x1D913F
// `in ax,dx`, IO base from global 0x295810, all calls via vtable):
//   counter++; s = status(port+1); exit if (!(s & 1)); exit if
//   (counter >= limit); buf[counter] = data_word(port); repeat.
// So status bit 0 = RX DATA AVAILABLE (1 while the reply FIFO is nonempty,
// 0 when dry) and data reads are WORDs packing two FIFO bytes low-first,
// exactly like real FIFO hardware on word INs. Replies are a per-card FIFO;
// dry reads are floating-bus (0xFF per byte, 0xFFFF per word).
// FIFO pops always log (progress); idle reads collapse (poll loops) with a
// heartbeat so hangs stay visible.
constexpr unsigned Su2000TrkPorts = 8;

// Best-guess sign-on: required substring first, then firmware id.
const std::string Su2000TrkSignOn = "InsideTRAK VER.151.03\r\n";

struct Su2000TrkPortLog {
	uint8_t last     = 0xFF;
	bool last_write  = false;
	unsigned repeat  = 0;
	unsigned total   = 0;
	bool valid       = false;
};

struct Su2000TrkCard {
	uint16_t base                          = 0;
	bool card_installed                    = false;
	Su2000TrkPortLog ports[Su2000TrkPorts] = {};
	IO_ReadHandleObject read_handle        = {};
	IO_WriteHandleObject write_handle      = {};
	// Reply FIFO: bytes the card owes the host (sign-on etc.).
	std::vector<uint8_t> fifo = {};
	size_t fifo_pos           = 0;
	// S-attempt accounting: words served since the last 'S', reported at
	// the next command so count-fail vs content-fail is explicit.
	bool s_armed     = false;
	unsigned s_words = 0;
	// W-attempt counter for the retry-path dump trigger.
	unsigned w_count = 0;
};

Su2000TrkCard trk_cards[2] = {};

// Stuck-loop code dump: when the guest polls a dry data port past this many
// reads, dump 512 bytes around CS:EIP once per session so the wait loop can
// be disassembled offline (exit mask, byte-vs-word reads). The TRK open
// strings live in ETS.EXE ~0x6F10C ('W', 'S', '28 words expected').
bool stuck_dump_done = false;

void su2000_dump_stuck_code()
{
	stuck_dump_done = true;
	const PhysPt seg_base = SegPhys(cs);
	const uint32_t eip    = reg_eip;
	LOG_MSG("SU2000: stuck-loop code dump CS:IP %04x:%08x segbase 0x%08x",
	        SegValue(cs),
	        eip,
	        seg_base);
	for (unsigned row = 0; row < 32; ++row) {
		const uint32_t off = (eip - 256u) + row * 16u;
		char hex[64]       = {};
		char asc[20]       = {};
		for (unsigned i = 0; i < 16; ++i) {
			const uint8_t b = mem_readb(seg_base + off + i);
			snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02x ", b);
			asc[i] = (b >= 32 && b < 127) ? static_cast<char>(b) : '.';
		}
		asc[16] = '\0';
		LOG_MSG("SU2000: code %08x: %s %s", off, hex, asc);
	}
	// Stack backtrace: return addresses reveal the polling loop that
	// called the port helper (calls are indirect through a vtable, so no
	// static xrefs exist). Dump code around each plausible address.
	const PhysPt ss_base = SegPhys(ss);
	const uint32_t esp   = reg_esp;
	LOG_MSG("SU2000: stuck-loop stack SS:ESP %04x:%08x", SegValue(ss), esp);
	unsigned dumped = 0;
	for (unsigned k = 0; k < 24 && dumped < 6; ++k) {
		const uint32_t v = mem_readd(ss_base + esp + k * 4u);
		if (v < eip - 0x10000u || v > eip + 0x10000u || v == 0u) {
			continue;
		}
		++dumped;
		LOG_MSG("SU2000: stack[%u] ret %08x", k, v);
		for (unsigned row = 0; row < 4; ++row) {
			const uint32_t off = (v - 32u) + row * 16u;
			char hex[64]       = {};
			for (unsigned i = 0; i < 16; ++i) {
				const uint8_t b = mem_readb(seg_base + off + i);
				snprintf(hex + i * 3,
				         sizeof(hex) - i * 3,
				         "%02x ",
				         b);
			}
			LOG_MSG("SU2000: loop %08x: %s", off, hex);
		}
	}
}

bool su2000_trk_log_enabled()
{
	const auto* section = get_section("su2000");
	if (!section) {
		return true;
	}
	return section->GetBool("tracker_log");
}

int su2000_trk_find(const io_port_t port, unsigned& reg_out)
{
	for (int c = 0; c < 2; ++c) {
		if (!trk_cards[c].card_installed) {
			continue;
		}
		if (port >= trk_cards[c].base &&
		    port < trk_cards[c].base + Su2000TrkPorts) {
			reg_out = static_cast<unsigned>(port -
			                                trk_cards[c].base);
			return c;
		}
	}
	return -1;
}

void su2000_trk_trace(const int card, const unsigned reg, const bool write,
                      const uint8_t value)
{
	if (!su2000_trk_log_enabled()) {
		return;
	}
	auto& slot = trk_cards[card].ports[reg];
	++slot.total;
	if (slot.valid && value == slot.last && write == slot.last_write) {
		++slot.repeat;
		// Poll loops otherwise vanish into the collapse: heartbeat every
		// 1M identical accesses so a hang stays visible with its count.
		if ((slot.repeat % 1000000u) == 0u) {
			LOG_MSG("SU2000: TRK%d %s +0x%x (0x%03x) still polling 0x%02x x%u total [%04x:%04x]",
			        card + 1,
			        write ? "write" : "read",
			        reg,
			        trk_cards[card].base + reg,
			        value,
			        slot.total,
			        SegValue(cs),
			        reg_eip & 0xffffu);
		}
		return;
	}
	if (slot.valid && slot.repeat > 0) {
		LOG_MSG("SU2000: TRK%d %s +0x%x previous value repeated x%u",
		        card + 1,
		        slot.last_write ? "write" : "read",
		        reg,
		        slot.repeat);
	}
	slot.last       = value;
	slot.last_write = write;
	slot.repeat     = 0;
	slot.valid      = true;
	LOG_MSG("SU2000: TRK%d %s +0x%x (0x%03x) = 0x%02x [%04x:%04x]",
	        card + 1,
	        write ? "write" : "read",
	        reg,
	        trk_cards[card].base + reg,
	        value,
	        SegValue(cs),
	        reg_eip & 0xffffu);
}

// Status bit 0 = RX data available (disassembled wait loop tests `al & 1`
// and exits when clear). All other bits read back set, matching the 0xFF
// the open probe already accepted.
constexpr uint8_t Su2000TrkStatusDry = 0xFE;
constexpr uint8_t Su2000TrkStatusRdy = 0xFF;

io_val_t su2000_trk_read(io_port_t port, io_width_t width)
{
	unsigned reg   = 0;
	const int card = su2000_trk_find(port, reg);
	if (card < 0) {
		return 0xFF;
	}
	auto& c = trk_cards[card];
	if (reg == 1) {
		// Status port: bit 0 tracks the reply FIFO.
		const uint8_t v = (c.fifo_pos < c.fifo.size()) ? Su2000TrkStatusRdy
		                                               : Su2000TrkStatusDry;
		su2000_trk_trace(card, reg, false, v);
		return v;
	}
	if (reg == 0) {
		// Data port: pack the reply FIFO into the access width, low
		// byte first, like real FIFO hardware on word/dword INs (the
		// TRK lib reads WORDs). Short reads pad floating-bus 0xFF.
		if (c.fifo_pos < c.fifo.size()) {
			unsigned n = 1;
			if (width == io_width_t::dword) {
				n = 4;
			} else if (width == io_width_t::word) {
				n = 2;
			}
			uint32_t v = 0;
			for (unsigned i = 0; i < n; ++i) {
				uint8_t b = 0xFF;
				if (c.fifo_pos < c.fifo.size()) {
					b = c.fifo[c.fifo_pos++];
					if (su2000_trk_log_enabled()) {
						LOG_MSG("SU2000: TRK%d read +0x0 FIFO = 0x%02x ('%c') [%04x:%04x]",
						        card + 1,
						        b,
						        (b >= 32 && b < 127) ? b : '.',
						        SegValue(cs),
						        reg_eip & 0xffffu);
					}
				}
				v |= static_cast<uint32_t>(b) << (i * 8u);
			}
			// Progress: word pops collapse on the low byte; the byte
			// lines above keep the true count.
			auto& slot    = c.ports[reg];
			slot.last     = static_cast<uint8_t>(v & 0xFFu);
			slot.valid    = true;
			slot.repeat   = 0;
			++slot.total;
			if (c.s_armed) {
				c.s_words = (56u - static_cast<unsigned>(c.fifo.size() -
				                                         c.fifo_pos)) /
				            2u;
			}
			return v;
		}
	}
	su2000_trk_trace(card, reg, false, 0xFF);
	if (!stuck_dump_done && card == 0 && reg == 0 &&
	    trk_cards[card].ports[reg].total > 2000000u) {
		su2000_dump_stuck_code();
	}
	return 0xFF; // floating bus when the card has nothing to say
}

void su2000_trk_write(io_port_t port, io_val_t val, io_width_t)
{
	unsigned reg   = 0;
	const int card = su2000_trk_find(port, reg);
	if (card < 0) {
		return;
	}
	const uint8_t v = static_cast<uint8_t>(val & 0xFFu);
	auto& cardst = trk_cards[card];
	if (reg == 0 && v == 'W') {
		// Open/init probe: queue the sign-on (fresh stream restarts
		// any half-read reply, like real hardware re-transmitting).
		if (cardst.s_armed && su2000_trk_log_enabled()) {
			LOG_MSG("SU2000: TRK%d previous 'S' attempt delivered %u of 28 words",
			        card + 1,
			        cardst.s_words);
		}
		cardst.s_armed = false;
		auto& c        = trk_cards[card];
		c.fifo.assign(Su2000TrkSignOn.begin(), Su2000TrkSignOn.end());
		c.fifo_pos = 0;
		if (su2000_trk_log_enabled()) {
			LOG_MSG("SU2000: TRK%d command 'W' -> queued %u sign-on bytes",
			        card + 1,
			        static_cast<unsigned>(c.fifo.size()));
		}
		// Second 'W' means the previous attempt was content-rejected:
		// dump the retry path (validation code runs just before here).
		if (++cardst.w_count == 2 && !stuck_dump_done) {
			su2000_dump_stuck_code();
		}
	}
	if (reg == 0 && v == 'S') {
		// System-status-record probe ("28 words expected", "Firmware
		// version %ld"). Layout undecoded: reply with a marker record
		// (words 1..28, high bytes zero so any %s print stops safely)
		// and read the field offsets back from what ETS prints.
		auto& c = trk_cards[card];
		c.fifo.clear();
		for (uint32_t w = 1; w <= 28; ++w) {
			c.fifo.push_back(static_cast<uint8_t>(w & 0xFFu));
			c.fifo.push_back(static_cast<uint8_t>((w >> 8) & 0xFFu));
		}
		c.fifo_pos  = 0;
		c.s_armed   = true;
		c.s_words   = 0;
		if (su2000_trk_log_enabled()) {
			LOG_MSG("SU2000: TRK%d command 'S' -> queued 28 marker status words",
			        card + 1);
		}
	}
	su2000_trk_trace(card, reg, true, v);
}

void uninstall()
{
	if (!installed && !traps_installed) {
		return;
	}
	for (auto& card : trk_cards) {
		if (card.card_installed) {
			card.read_handle.Uninstall();
			card.write_handle.Uninstall();
			card.card_installed = false;
		}
	}
	// Flush collapsed poll counts so shutdown shows the true totals: an
	// infinite guest poll loop is otherwise one log line + silence.
	if (su2000_log_enabled()) {
		for (int c = 0; c < 2; ++c) {
			for (unsigned r = 0; r < Su2000TrkPorts; ++r) {
				const auto& slot = trk_cards[c].ports[r];
				if (slot.valid && slot.total > 1) {
					LOG_MSG("SU2000: TRK%d %s +0x%x (0x%03x) total x%u last=0x%02x",
					        c + 1,
					        slot.last_write ? "write" : "read",
					        r,
					        trk_cards[c].base + r,
					        slot.total,
					        slot.last);
				}
			}
		}
	}
	io_read_handle.Uninstall();
	io_write_handle.Uninstall();
	io2_read_handle.Uninstall();
	io2_write_handle.Uninstall();
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
	stuck_dump_done = false;
	const Bitu page = installed_mem_base >> 12;
	if (saved_page_handler) {
		MEM_SetPageHandler(page, 1, saved_page_handler);
		saved_page_handler = nullptr;
	} else if (installed_mem_base) {
		MEM_ResetPageHandler(page, 1);
	}
	PAGING_ClearTLB();
	delete mem_handler;
	mem_handler  = nullptr;
	installed    = false;
	if (su2000_log_enabled()) {
		LOG_MSG("SU2000: FORMAT C responder uninstalled");
	}
}

} // namespace

// ISA ownership map for the VFX-1 snooper guard (see su2000.h): ranges are
// (base, len) pairs recorded at install, cleared at uninstall. File scope
// (not anonymous) so hardware/vfx1.cpp can link SU2000_ClaimsPort.
std::vector<std::pair<uint16_t, uint16_t>> claimed_ranges = {};
bool su2000_active = false;

bool SU2000_ClaimsPort(const uint16_t port)
{
	if (!su2000_active) {
		return false;
	}
	for (const auto& [base, len] : claimed_ranges) {
		if (port >= base && port < base + len) {
			return true;
		}
	}
	return false;
}

static void su2000_claim_range(const uint16_t base, const uint16_t len)
{
	if (base) {
		claimed_ranges.emplace_back(base, len);
	}
}

static void su2000_clear_claims()
{
	claimed_ranges.clear();
	su2000_active = false;
}

void SU2000_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	using enum Property::Changeable::Value;

	auto section = conf->AddSection("su2000");

	auto pbool = section->AddBool("su2000_enable", OnlyAtStart, false);
	pbool->SetHelp(
	        "Master switch for all Virtuality SU-2000 emulation ('off' by\n"
	        "default). When off, nothing is installed regardless of the\n"
	        "other keys below (safe for non-SU-2000 sessions sharing this\n"
	        "config, e.g. VFX-1). When on, each of 'formatc', 'tracker',\n"
	        "'format2', 'bcst_trap', 'pix_trap' and 'vid_trap' enables its\n"
	        "own piece.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section->AddBool("formatc", OnlyAtStart, false);
	pbool->SetHelp(
	        "Emulate the Virtuality SU-2000 FORMAT C control window ('off' by\n"
	        "default). Answers the 2 KB ISA memory-window probes (version 0x30,\n"
	        "status 0x82, A5/5A sync toggle, centred joysticks) and latches the\n"
	        "IO volume-block writes, so MUTE.EXE and the ETS FORMAT register\n"
	        "tests can pass detection. No audio, network or video is emulated;\n"
	        "tracker ports are trace-only (see 'tracker').\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	auto phex = section->AddHex("formatc_mem", OnlyAtStart, 0xCC000);
	phex->SetHelp(
	        "FORMAT C ISA memory-window base (CC000 by default = SU solo in\n"
	        "CONFIG1.VPC). Must be 4 KB page-aligned inside C0000-EFFFF.\n"
	        "The 2-player second head (E0800, not page-aligned) is covered\n"
	        "by 'format2' when it shares this page (E0000 + E0800).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("formatc_io", OnlyAtStart, 0x210);
	phex->SetHelp(
	        "FORMAT C volume-register IO base (210 by default = SU solo in\n"
	        "CONFIG1.VPC). JP3 options: 210, 218, 260, 268, 298, 2C0, 2C8.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section->AddBool("format2", OnlyAtStart, false);
	pbool->SetHelp(
	        "Emulate the second SU-2000 FORMAT C window for 2-player units\n"
	        "('off' by default). Live CONFIG2.VPC parks FORMAT2 at MEM 0xE0800\n"
	        "IO 0x218, sharing one 4 KB page with FORMAT1 at 0xE0000 (both\n"
	        "halves answered; separate command latches). A FORMAT2 on a\n"
	        "different page is warned-and-skipped for now.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("format2_mem", OnlyAtStart, 0xE0800);
	phex->SetHelp(
	        "FORMAT2 ISA memory-window base (E0800 = 2-player in CONFIG2.VPC).\n"
	        "Must be 2 KB aligned inside C0000-EFFFF.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("format2_io", OnlyAtStart, 0x218);
	phex->SetHelp(
	        "FORMAT2 volume-register IO base (218 = 2-player in CONFIG2.VPC).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section->AddBool("bcst_trap", OnlyAtStart, false);
	pbool->SetHelp(
	        "Log-only trap for the SU-2000 broadcast/OSD FIFO ('off' by\n"
	        "default). Logs 16-bit words the guest writes (SU2000.md section\n"
	        "4d); reads return 0xFF. Diagnosis for the post-tracker silent\n"
	        "ETS exit — ETS is quiet on every emulated port for ~33 s, so\n"
	        "whatever it probes next is currently invisible.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("bcst_io", OnlyAtStart, 0x320);
	phex->SetHelp(
	        "Broadcast FIFO IO base (320 in CONFIG1/2.VPC).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section->AddBool("pix_trap", OnlyAtStart, false);
	pbool->SetHelp(
	        "Log-only trap for the PIX 1000 proc ports ('off' by default).\n"
	        "Byte reads answer 0xCF per the observed 'Read 1 byte from 0300\n"
	        "(PIX1) - CF' after every broadcast block (SU2000.md section 4b).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("pix_proc1", OnlyAtStart, 0x300);
	phex->SetHelp(
	        "PIX channel-1 proc port (300 in CONFIG1/2.VPC; 2A0/2A8 on old\n"
	        "jumperings).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("pix_proc2", OnlyAtStart, 0x360);
	phex->SetHelp(
	        "PIX channel-2 proc port (360 in 2-player CONFIG2.VPC).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section->AddBool("vid_trap", OnlyAtStart, false);
	pbool->SetHelp(
	        "Log-only trap for the VID 1000 video port ('off' by default).\n"
	        "Logs accesses; reads return 0xFF.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("vid_io", OnlyAtStart, 0x340);
	phex->SetHelp(
	        "VID 1000 IO base (340 in CONFIG1/2.VPC; JP5: 340/2C0/1C0).\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section->AddBool("su2000_log", Always, true);
	pbool->SetHelp(
	        "Log SU-2000 FORMAT C detection and volume traffic ('on' by\n"
	        "default). Healthy polling stays silent; detection-OK, command\n"
	        "writes and volume writes log once each.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	pbool = section->AddBool("tracker", OnlyAtStart, false);
	pbool->SetHelp(
	        "Claim the Polhemus InsideTrak tracker ports TRK1/TRK2 ('off' by\n"
	        "default). Bring-up stage: every access is logged with CS:IP and\n"
	        "the open/init probe ('W' command) is answered with a sign-on\n"
	        "message; anything else reads floating-bus 0xFF.\n"
	        "\n"
	        "Note: TRK2 at 278 overlaps LPT2 (278-27A); keep 'parallel2' off.\n"
	        "Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("tracker1", OnlyAtStart, 0x270);
	phex->SetHelp(
	        "TRK1 InsideTrak IO base (270 by default = SU solo in CONFIG.VPC).\n"
	        "S1 options: 270, 278, plus 4/8/10/20/40/80/100/200 adders.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("tracker2", OnlyAtStart, 0x278);
	phex->SetHelp(
	        "TRK2 InsideTrak IO base (278 by default = second card in a\n"
	        "2-player unit). Same options as 'tracker1'.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section->AddBool("tracker_log", Always, true);
	pbool->SetHelp(
	        "Log SU-2000 tracker port traffic with CS:IP ('on' by default).\n"
	        "Identical repeats collapse to a summary line so status-poll loops\n"
	        "stay readable.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
}

void SU2000_Init()
{
	const auto* section = get_section("su2000");
	if (!section) {
		return;
	}
	const bool want_all     = section->GetBool("su2000_enable");
	const bool want_formatc = section->GetBool("formatc");
	const bool want_tracker = section->GetBool("tracker");
	const bool want_format2 = section->GetBool("format2");
	const bool want_bcst    = section->GetBool("bcst_trap");
	const bool want_pix     = section->GetBool("pix_trap");
	const bool want_vid     = section->GetBool("vid_trap");
	if (!want_all) {
		return; // master switch off: never answer hardware unasked
	}
	if (!want_formatc && !want_tracker && !want_format2 && !want_bcst &&
	    !want_pix && !want_vid) {
		return; // fail-closed: never answer hardware unasked
	}

	uninstall(); // in case a previous init left state behind

	bool formatc_ok = false;
	bool tracker_ok = false;

	if (want_formatc) {
		const auto mem_base =
		        static_cast<uint32_t>(section->GetHex("formatc_mem"));
		const auto io_base =
		        static_cast<uint16_t>(section->GetHex("formatc_io"));

		if (mem_base < 0xC0000 || mem_base > 0xEFFFF ||
		    (mem_base & 0xFFFu) != 0u) {
			LOG_WARNING("SU2000: formatc_mem 0x%x out of range or not "
			            "page-aligned (need C0000-EFFFF, 4 KB aligned); "
			            "FORMAT C not installed",
			            mem_base);
		} else if (io_base < 0x100 || io_base > 0x3F8) {
			LOG_WARNING("SU2000: formatc_io 0x%x out of range (need "
			            "100-3F8); FORMAT C not installed",
			            io_base);
		} else {
			installed_mem_base = mem_base;
			installed_io_base  = io_base;
			vol_latch.fill(0x00);
			vol_latch2.fill(0x00);
			vol_touched = false;

			// Optional second window (CONFIG2.VPC: E0800 shares the
			// E0000 page). 2 KB aligned is enough for the second card.
			uint32_t mem_base2 = 0;
			if (want_format2) {
				const auto f2m = static_cast<uint32_t>(
				        section->GetHex("format2_mem"));
				if (f2m < 0xC0000 || f2m > 0xEFFFF ||
				    (f2m & 0x7FFu) != 0u) {
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
				io_base2 = static_cast<uint16_t>(
				        section->GetHex("format2_io"));
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

			const Bitu page    = mem_base >> 12;
			saved_page_handler = MEM_GetPageHandler(page);
			mem_handler = new Su2000FormatCHandler(mem_base, mem_base2);
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
			formatc_ok = true;
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
		}
	}

	// Log-only missing-hardware traps (BCST/PIX/VID). Installed even when
	// FORMAT C itself is off: they answer nothing real, only make the
	// post-tracker ETS probes visible in dosbox-xr.log.
	if (want_bcst || want_pix || want_vid) {
		bool any_trap      = false;
		uint16_t bcst_base = 0, pix1_base = 0, pix2_base = 0, vid_base = 0;
		if (want_bcst) {
			bcst_base = static_cast<uint16_t>(
			        section->GetHex("bcst_io"));
			if (bcst_base < 0x100 || bcst_base > 0x3F8) {
				LOG_WARNING("SU2000: bcst_io 0x%x out of range "
				            "(need 100-3F8); BCST trap not "
				            "installed",
				            bcst_base);
				bcst_base = 0;
			}
		}
		if (want_pix) {
			pix1_base = static_cast<uint16_t>(
			        section->GetHex("pix_proc1"));
			pix2_base = static_cast<uint16_t>(
			        section->GetHex("pix_proc2"));
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
			vid_base = static_cast<uint16_t>(
			        section->GetHex("vid_io"));
			if (vid_base < 0x100 || vid_base > 0x3F8) {
				LOG_WARNING("SU2000: vid_io 0x%x out of range "
				            "(need 100-3F8); VID trap not "
				            "installed",
				            vid_base);
				vid_base = 0;
			}
		}
		// One 4-port window per trap base keeps installs disjoint:
		// BCST is a single word port, PIX/VID single byte ports.
		// Skip any base already claimed by FORMAT/TRK volumes.
		auto base_claimed = [&](uint16_t b) {
			if (formatc_ok &&
			    ((b >= installed_io_base &&
			      b < installed_io_base + 8) ||
			     (installed_io_base2 && b >= installed_io_base2 &&
			      b < installed_io_base2 + 8))) {
				return true;
			}
			return false;
		};
		auto install_trap = [&](uint16_t b, const char* what,
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
			any_trap       = true;
		}
		if (pix1_base &&
		    install_trap(pix1_base, "PIX1", trap_pix1_read, trap_pix1_write)) {
			trap_pix1_base = pix1_base;
			any_trap       = true;
		}
		if (pix2_base &&
		    install_trap(pix2_base, "PIX2", trap_pix2_read, trap_pix2_write)) {
			trap_pix2_base = pix2_base;
			any_trap       = true;
		}
		if (vid_base &&
		    install_trap(vid_base, "VID", trap_vid_read, trap_vid_write)) {
			trap_vid_base = vid_base;
			any_trap      = true;
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
	}

	if (want_tracker) {
		const uint16_t trk_bases[2] = {
		        static_cast<uint16_t>(section->GetHex("tracker1")),
		        static_cast<uint16_t>(section->GetHex("tracker2"))};
		for (int c = 0; c < 2; ++c) {
			const auto base = trk_bases[c];
			if (base < 0x100 || base > 0x3F8 ||
			    (base & 0x7u) != 0u) {
				LOG_WARNING("SU2000: tracker%d 0x%x out of range "
				            "or not 8-aligned (need 100-3F8); "
				            "TRK%d not installed",
				            c + 1,
				            base,
				            c + 1);
				continue;
			}
			trk_cards[c].base = base;
			for (auto& p : trk_cards[c].ports) {
				p = Su2000TrkPortLog{};
			}
			trk_cards[c].fifo.clear();
			trk_cards[c].fifo_pos = 0;
			trk_cards[c].s_armed  = false;
			trk_cards[c].s_words  = 0;
			trk_cards[c].w_count  = 0;
			trk_cards[c].read_handle.Install(base,
			                                 su2000_trk_read,
			                                 io_width_t::dword,
			                                 Su2000TrkPorts);
			trk_cards[c].write_handle.Install(base,
			                                  su2000_trk_write,
			                                  io_width_t::dword,
			                                  Su2000TrkPorts);
			trk_cards[c].card_installed = true;
			tracker_ok = true;
		}
		if (tracker_ok) {
			LOG_MSG("SU2000: tracker tracer build %s (TRK1 0x%03x TRK2 0x%03x)",
			        SU2000_BUILD_ID,
			        trk_cards[0].card_installed ? trk_cards[0].base
			                                    : 0,
			        trk_cards[1].card_installed ? trk_cards[1].base
			                                    : 0);
		}
	}

	installed = formatc_ok || tracker_ok || traps_installed;
	su2000_clear_claims();
	su2000_active = installed;
	if (installed) {
		if (formatc_ok) {
			su2000_claim_range(installed_io_base, 8);
			su2000_claim_range(installed_io_base2, 8);
		}
		if (tracker_ok) {
			for (const auto& card : trk_cards) {
				if (card.card_installed) {
					su2000_claim_range(card.base,
					                   Su2000TrkPorts);
				}
			}
		}
		if (traps_installed) {
			su2000_claim_range(trap_bcst_base, 1);
			su2000_claim_range(trap_pix1_base, 1);
			su2000_claim_range(trap_pix2_base, 1);
			su2000_claim_range(trap_vid_base, 1);
		}
		// Hardwired SU-2000 NET block (CONFIG1/2.VPC [NET] IO 0x280):
		// unemulated, but reserving it keeps the VFX-1 VIP snooper
		// (base 0x280) off ETS's NET probes.
		su2000_claim_range(0x280, 16);
	}
}

void SU2000_Destroy()
{
	uninstall();
	su2000_clear_claims();
}
