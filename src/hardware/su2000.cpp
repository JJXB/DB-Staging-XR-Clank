// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Virtuality SU-2000 FORMAT C detection skeleton.
//
// What this answers (SU2000.md section 5): the 2 KB ISA memory window every
// SU-2000 binary probes first (version/status/sync/joystick map) plus the
// IO+0..7 volume-register block MUTE.EXE zeroes. Nothing else: no Ensoniq
// audio, no N80C198 MCU, no FORMAT D, no PIX/VID video, no tracker, no net.
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
#include <string>
#include <vector>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "cpu/paging.h"
#include "misc/support.h"

// Build stamp: bump on responder changes so logs identify the code.
#define SU2000_BUILD_ID "2026-10-11/formatc-skeleton-v1+trk-answer-v1"

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
class Su2000FormatCHandler final : public PageHandler {
public:
	Su2000FormatCHandler(uint32_t mem_base) : base(mem_base)
	{
		flags = PFLAG_NOCODE; // force every access through readb/writeb
		cmd.fill(0x00);
		cmd[0x01] = Su2000CmdSync;
		cmd[0x04] = Su2000Effects;
		cmd[0x09] = Su2000PixelShift;
	}

	uint8_t readb(PhysPt addr) override
	{
		const uint32_t offset = addr - base;
		if (offset >= Su2000WindowSize) {
			return 0xFF; // upper half of the page: open bus
		}
		if (offset >= 0x400 && offset <= 0x409) {
			return cmd[offset - 0x400];
		}
		switch (offset) {
		case 0x0000:
			version_seen = true;
			break;
		case 0x0006:
			status_seen = true;
			break;
		case 0x0007: {
			// Sync toggles A5/5A while online (SU2000.md section 5).
			const uint8_t v = sync_state ? 0xA5 : 0x5A;
			sync_state      = !sync_state;
			maybe_report_detection();
			return v;
		}
		default: break;
		}
		const uint8_t v = status_area(offset);
		maybe_report_detection();
		return v;
	}

	void writeb(PhysPt addr, uint8_t val) override
	{
		const uint32_t offset = addr - base;
		// Only the command block is ISA-writable on real hardware.
		if (offset >= 0x400 && offset <= 0x409) {
			cmd[offset - 0x400] = val;
			if (su2000_log_enabled()) {
				LOG_MSG("SU2000: FORMAT C cmd+%02x = 0x%02x",
				        static_cast<unsigned>(offset - 0x400),
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

	void maybe_report_detection()
	{
		if (det_reported || !version_seen || !status_seen) {
			return;
		}
		det_reported = true;
		if (su2000_log_enabled()) {
			LOG_MSG("SU2000: FORMAT C detection sequence OK "
			        "(version 0x%02x + status 0x%02x) build %s",
			        Su2000Version,
			        Su2000Status,
			        SU2000_BUILD_ID);
		}
	}

	uint32_t base = 0;
	std::array<uint8_t, 0x0A> cmd = {};
	bool sync_state    = true; // first sync read returns A5
	bool version_seen  = false;
	bool status_seen   = false;
	bool det_reported  = false;
};

Su2000FormatCHandler* mem_handler   = nullptr;
PageHandler* saved_page_handler     = nullptr;
uint32_t installed_mem_base         = 0;
uint16_t installed_io_base          = 0;
bool installed                      = false;
std::array<uint8_t, 8> vol_latch    = {};
bool vol_touched                    = false;

IO_ReadHandleObject io_read_handle    = {};
IO_WriteHandleObject io_write_handle  = {};

io_val_t su2000_io_read(io_port_t port, io_width_t)
{
	const unsigned reg = static_cast<unsigned>(port - installed_io_base);
	uint8_t v          = 0xFF;
	switch (reg) {
	case 0:
	case 1:
	case 2: v = vol_latch[reg]; break; // latched, init 0x00
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
	const unsigned reg = static_cast<unsigned>(port - installed_io_base);
	if (reg > 7) {
		return;
	}
	// MUTE.EXE zeroes every volume register: latch so a readback
	// observer (and the log) can see it happened.
	vol_latch[reg] = static_cast<uint8_t>(val & 0xFFu);
	vol_touched    = true;
	if (su2000_log_enabled()) {
		LOG_MSG("SU2000: FORMAT C volume reg %u = 0x%02x",
		        reg,
		        vol_latch[reg]);
	}
}

// ---- Tracker: Polhemus InsideTrak minimal answers ----
// Two ISA cards (TRK1/TRK2, 8 ports each). Observed TRK-library open
// sequence (ETS, "Trying to open card %ld as an InsideTRAK at address
// 0x%lx"): read status +0x1 once, write 'W' to data +0x0, then poll data
// +0x0 for a reply (hangs on floating-bus 0xFF). The reply is the card
// sign-on: the library checks it for the "InsideTRAK" string ("InsideTRAK
// string not found in correct place" otherwise). Sign-on guess, anchored in
// SU2000.md section 8: "InsideTRAK" at offset 0 + TRACK1 firmware
// VER.151.03 + CRLF terminator (Polhemus ASCII convention). Replies are a
// per-card FIFO; empty reads 0xFF. FIFO pops always log (progress); idle
// 0xFF reads collapse (poll loops).
constexpr unsigned Su2000TrkPorts = 8;

// Best-guess sign-on: required substring first, then firmware id.
const std::string Su2000TrkSignOn = "InsideTRAK VER.151.03\r\n";

struct Su2000TrkPortLog {
	uint8_t last     = 0xFF;
	bool last_write  = false;
	unsigned repeat  = 0;
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
};

Su2000TrkCard trk_cards[2] = {};

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
	if (slot.valid && value == slot.last && write == slot.last_write) {
		++slot.repeat;
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

io_val_t su2000_trk_read(io_port_t port, io_width_t)
{
	unsigned reg   = 0;
	const int card = su2000_trk_find(port, reg);
	if (card < 0) {
		return 0xFF;
	}
	if (reg == 0) {
		// Data port: pop the reply FIFO; empty reads floating-bus.
		auto& c = trk_cards[card];
		if (c.fifo_pos < c.fifo.size()) {
			const uint8_t v = c.fifo[c.fifo_pos++];
			// Progress: always log pops (collapse hides counts).
			auto& slot    = c.ports[reg];
			slot.last     = v;
			slot.valid    = true;
			slot.repeat   = 0;
			if (su2000_trk_log_enabled()) {
				LOG_MSG("SU2000: TRK%d read +0x0 FIFO = 0x%02x ('%c') [%04x:%04x]",
				        card + 1,
				        v,
				        (v >= 32 && v < 127) ? v : '.',
				        SegValue(cs),
				        reg_eip & 0xffffu);
			}
			return v;
		}
	}
	su2000_trk_trace(card, reg, false, 0xFF);
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
	if (reg == 0 && v == 'W') {
		// Open/init probe: queue the sign-on (fresh stream restarts
		// any half-read reply, like real hardware re-transmitting).
		auto& c   = trk_cards[card];
		c.fifo.assign(Su2000TrkSignOn.begin(), Su2000TrkSignOn.end());
		c.fifo_pos = 0;
		if (su2000_trk_log_enabled()) {
			LOG_MSG("SU2000: TRK%d command 'W' -> queued %u sign-on bytes",
			        card + 1,
			        static_cast<unsigned>(c.fifo.size()));
		}
	}
	su2000_trk_trace(card, reg, true, v);
}

void uninstall()
{
	if (!installed) {
		return;
	}
	for (auto& card : trk_cards) {
		if (card.card_installed) {
			card.read_handle.Uninstall();
			card.write_handle.Uninstall();
			card.card_installed = false;
		}
	}
	io_read_handle.Uninstall();
	io_write_handle.Uninstall();
	const Bitu page = installed_mem_base >> 12;
	if (saved_page_handler) {
		MEM_SetPageHandler(page, 1, saved_page_handler);
		saved_page_handler = nullptr;
	} else {
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

void SU2000_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	using enum Property::Changeable::Value;

	auto section = conf->AddSection("su2000");

	auto pbool = section->AddBool("formatc", OnlyAtStart, false);
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
	        "SD second head E0800 is not page-aligned and not yet supported.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section->AddHex("formatc_io", OnlyAtStart, 0x210);
	phex->SetHelp(
	        "FORMAT C volume-register IO base (210 by default = SU solo in\n"
	        "CONFIG1.VPC). JP3 options: 210, 218, 260, 268, 298, 2C0, 2C8.\n"
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
	const bool want_formatc = section->GetBool("formatc");
	const bool want_tracker = section->GetBool("tracker");
	if (!want_formatc && !want_tracker) {
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
			vol_touched = false;

			const Bitu page    = mem_base >> 12;
			saved_page_handler = MEM_GetPageHandler(page);
			mem_handler = new Su2000FormatCHandler(mem_base);
			MEM_SetPageHandler(page, 1, mem_handler);
			io_read_handle.Install(io_base,
			                       su2000_io_read,
			                       io_width_t::dword,
			                       8);
			io_write_handle.Install(io_base,
			                        su2000_io_write,
			                        io_width_t::dword,
			                        8);
			PAGING_ClearTLB();
			formatc_ok = true;
			LOG_MSG("SU2000: FORMAT C responder build %s (MEM 0x%05x IO 0x%03x)",
			        SU2000_BUILD_ID,
			        mem_base,
			        io_base);
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

	installed = formatc_ok || tracker_ok;
}

void SU2000_Destroy()
{
	uninstall();
}
