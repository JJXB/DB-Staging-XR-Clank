// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// SU-2000 tracker: Polhemus InsideTrak minimal answers.
//
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

#include "hardware/su2000_tracker.h"

#include "hardware/port.h"
#include "hardware/su2000_internal.h"
#include "hardware/timer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "misc/support.h"

namespace {

constexpr unsigned Su2000TrkPorts = 8;

// Best-guess sign-on: required substring first, then firmware id.
const std::string Su2000TrkSignOn = "InsideTRAK VER.151.03\r\n";

struct Su2000TrkPortLog {
	uint8_t last    = 0xFF;
	bool last_write = false;
	unsigned repeat = 0;
	unsigned total  = 0;
	bool valid      = false;
};

struct Su2000TrkCard {
	uint16_t base                           = 0;
	bool card_installed                     = false;
	Su2000TrkPortLog ports[Su2000TrkPorts]  = {};
	IO_ReadHandleObject read_handle         = {};
	IO_WriteHandleObject write_handle       = {};
	// Reply FIFO: bytes the card owes the host (sign-on etc.).
	std::vector<uint8_t> fifo = {};
	size_t fifo_pos           = 0;
	// S-attempt accounting: words served since the last 'S', reported at
	// the next command so count-fail vs content-fail is explicit.
	bool s_armed     = false;
	unsigned s_words = 0;
	// W-attempt counter for the retry-path dump trigger.
	unsigned w_count = 0;
	// ASCII command assembly (l/v/x/y etc. arrive a byte at a time).
	std::string command = {};
	// Sensors the guest activated with 'l<station>,1'.
	uint8_t active_sensors = 0;
	// 'y' enables free-running data records, as on the real ISA card.
	bool streaming = false;
	// Next station (0..3) to emit and bytes emitted since 'y'.
	unsigned stream_sensor = 0;
	unsigned stream_bytes  = 0;
	// Free-running sample cadence: status reads ready only once the
	// wall-clock sample interval has elapsed since the last record, then
	// one 7-word record is queued and status reads dry again once it is
	// drained (so the reader lands exactly on its 8-count target).
	uint32_t stream_next_ms = 0;
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
	su2000_watch_gdt();
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

// Free-running sample period in milliseconds (a fresh 7-word record every
// interval). Fast enough that polling loops always find data promptly.
constexpr uint32_t Su2000TrkSampleMs = 5;

void su2000_trk_push(Su2000TrkCard& c, const uint8_t* data, const size_t len)
{
	// Append without disturbing a partially consumed reply.
	if (c.fifo_pos >= c.fifo.size()) {
		c.fifo.clear();
		c.fifo_pos = 0;
	}
	c.fifo.insert(c.fifo.end(), data, data + len);
}

// One 8-word card data record, as parsed by ETS at 0x64925: word 0 is a
// header, words 1-3 are X/Y/Z, words 4-6 Az/El/Roll, word 7 is unused.
// Values are little-endian words (the TRK library reads WORDs).
void su2000_trk_stream_record(Su2000TrkCard& c)
{
	// ETS's record reader (disassembled at ETS 0x1cb117) increments its
	// word counter *before* testing status, so it returns (words read)+1
	// and the caller accepts the sample only when that equals 8 -- i.e.
	// the card must present exactly 7 words per request. The first word
	// lands in the caller's buffer[1] and is parsed as X, so the record
	// is X/Y/Z/Az/El/Roll plus one spare word (no header word).
	int16_t words[7] = {};
	// Synthetic pose: a wall-clock-driven orbit so the motion-dependent
	// views (main sensor screen, tracker world, hemispheres, jitter) show
	// changing data. Position words are raw counts; orientation words use
	// the Polhemus convention that a full +-180 deg maps to +-32768 (so
	// 90 deg = 16384).
	const unsigned sensor = c.stream_sensor;
	const double t        = static_cast<double>(GetTicks()) * 0.001 * 1.5;
	const double radius   = (sensor == 0) ? 6000.0 : 4000.0;
	const double phase    = t + static_cast<double>(sensor) * 3.14159265358979;
	words[0] = static_cast<int16_t>(radius * std::cos(phase));
	words[1] = static_cast<int16_t>(radius * std::sin(phase));
	words[2] = static_cast<int16_t>(1000.0 * std::sin(2.0 * t + static_cast<double>(sensor)));
	constexpr double counts_per_deg = 32768.0 / 180.0;
	words[3] = static_cast<int16_t>(30.0 * std::sin(t) * counts_per_deg);
	words[4] = static_cast<int16_t>(20.0 * std::sin(1.3 * t + 1.0) * counts_per_deg);
	words[5] = static_cast<int16_t>(15.0 * std::cos(0.7 * t) * counts_per_deg);
	words[6] = 0;

	uint8_t bytes[14] = {};
	for (unsigned i = 0; i < 7; ++i) {
		const uint16_t w = static_cast<uint16_t>(words[i]);
		bytes[i * 2]     = static_cast<uint8_t>(w & 0xFFu);
		bytes[i * 2 + 1] = static_cast<uint8_t>(w >> 8);
	}
	su2000_trk_push(c, bytes, sizeof(bytes));

	if (c.stream_bytes < 64 && su2000_trk_log_enabled()) {
		LOG_MSG("SU2000: TRK stream record sensor %u (x=%d y=%d z=%d)",
		        sensor + 1,
		        words[0],
		        words[1],
		        words[2]);
	}
	c.stream_bytes += sizeof(bytes);
}

// The card is polled: ETS writes 0x00 to the control port (base+1) to
// request a sample, then reads words while status bit 0 is set. A request
// loads exactly one 7-word record so the reader returns 8 and the sample
// is accepted; status then reads dry until the next request.
void su2000_trk_refill_stream(Su2000TrkCard& c)
{
	if (!c.streaming) {
		return;
	}
	if (c.active_sensors == 0) {
		c.active_sensors = 0x03; // ETS activates 1 and 2; be forgiving
	}
	for (unsigned tries = 0; tries < 4; ++tries) {
		const unsigned sensor = c.stream_sensor & 3u;
		c.stream_sensor = (sensor + 1) & 3u;
		if (c.active_sensors & (1u << sensor)) {
			c.stream_sensor = sensor;
			c.fifo.clear();
			c.fifo_pos = 0;
			su2000_trk_stream_record(c);
			c.stream_sensor = (sensor + 1) & 3u;
			return;
		}
	}
}

// 'l' output record: 8 bytes, station states in bytes 3 and 4, which is
// where ETS looks for active-sensor '1' flags right after open.
void su2000_trk_queue_active(Su2000TrkCard& c)
{
	uint8_t record[8] = {'2', '.', 'l', '0', '0', '0', '0', '\r'};
	for (unsigned i = 0; i < 4; ++i) {
		if (c.active_sensors & (1u << i)) {
			record[3 + i] = '1';
		}
	}
	su2000_trk_push(c, record, sizeof(record));
	if (su2000_trk_log_enabled()) {
		LOG_MSG("SU2000: TRK command 'l' -> queued active-sensor record");
	}
}

void su2000_trk_command(Su2000TrkCard& c, const std::string& cmd)
{
	if (cmd.empty()) {
		return;
	}
	const char letter = cmd[0];
	if (letter == 'l') {
		const auto comma = cmd.find(',');
		if (comma == std::string::npos) {
			su2000_trk_queue_active(c);
			return;
		}
		const int station = (cmd.size() > 1) ? (cmd[1] - '1') : 0;
		const bool on     = cmd.find('1', comma) != std::string::npos;
		if (station >= 0 && station < 4) {
			if (on) {
				c.active_sensors |= static_cast<uint8_t>(1u << station);
			} else {
				c.active_sensors &= static_cast<uint8_t>(~(1u << station));
			}
			if (su2000_trk_log_enabled()) {
				LOG_MSG("SU2000: TRK sensor %d %s (mask 0x%02x)",
				        station + 1,
				        on ? "on" : "off",
				        c.active_sensors);
			}
		}
		return;
	}
	if (letter == 'y') {
		c.streaming     = true;
		c.stream_sensor = 0;
		c.stream_bytes  = 0;
		su2000_trk_refill_stream(c);
		if (su2000_trk_log_enabled()) {
			LOG_MSG("SU2000: TRK streaming enabled by '%.*s'",
			        static_cast<int>(cmd.size()),
			        cmd.c_str());
		}
		return;
	}
	if (letter == 'C') {
		c.streaming = true;
		su2000_trk_refill_stream(c);
		return;
	}
	if (letter == 'c') {
		c.streaming = false;
		return;
	}
	if (letter == 'P') {
		su2000_trk_refill_stream(c);
		return;
	}
	// 'D', 'v' and 'x' accept their parameters silently.
}

io_val_t su2000_trk_read(io_port_t port, io_width_t width)
{
	unsigned reg   = 0;
	const int card = su2000_trk_find(port, reg);
	if (card < 0) {
		return 0xFF;
	}
	auto& c = trk_cards[card];
	if (reg == 1) {
		// Status port. bit 0 = sample words available. The card free-runs
		// at a fixed cadence: once the interval since the last record has
		// elapsed a fresh 7-word record is queued, and status reads dry
		// again as soon as it has been drained. That dry edge is what the
		// reader's word counter relies on to accept exactly one sample.
		if (c.streaming && c.fifo_pos >= c.fifo.size()) {
			const uint32_t now = GetTicks();
			if (now >= c.stream_next_ms) {
				su2000_trk_refill_stream(c);
				c.stream_next_ms = now + Su2000TrkSampleMs;
			}
		}
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
					if (su2000_trk_log_enabled() &&
					    (!c.streaming || c.stream_bytes <= 64)) {
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
		c.fifo_pos       = 0;
		c.command.clear();
		c.active_sensors = 0;
		c.streaming      = false;
		c.stream_sensor  = 0;
		c.stream_bytes   = 0;
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
	} else if (reg == 0 && v == 'S') {
		// System-status-record probe: ETS validates 56 bytes (28 words).
		// Disassembly at ETS 0x16443..0x1646C (runtime 0x1A2A30-ish via
		// obj1) parses "%f" at byte 0x0F, rounds *100+0.1 and compares
		// 15100; that path then requires "InsideTRAK" at byte 0x17.
		// The firmware's own status block at merged-rom 0x149E supplies
		// the surrounding text. Keep the record ASCII so the FIFO's
		// low-byte-first word packing forms a normal string.
		auto& c = trk_cards[card];
		std::vector<uint8_t> record(56, 0x00);
		const auto put = [&record](size_t off, const char* s) {
			while (*s) {
				record[off++] = static_cast<uint8_t>(*s++);
			}
		};
		put(1, "*** ");
		put(6, "151.0");
		put(0x0f, "151.0");
		put(0x17, "InsideTRAK");
		put(0x27, "CPG2047-001-01");
		c.fifo = std::move(record);
		c.fifo_pos  = 0;
		c.s_armed   = true;
		c.s_words   = 0;
		c.command.clear();
		if (su2000_trk_log_enabled()) {
			LOG_MSG("SU2000: TRK%d command 'S' -> queued 28-word status record (ver 151.0, InsideTRAK)",
			        card + 1);
		}
	} else if (reg == 0) {
		auto& c = trk_cards[card];
		if (v == '\r' || v == '\n') {
			if (!c.command.empty()) {
				if (su2000_trk_log_enabled()) {
					char clean[80] = {};
					const auto n = static_cast<unsigned>(
					        std::min<size_t>(c.command.size(),
					                         sizeof(clean) - 1));
					for (unsigned i = 0; i < n; ++i) {
						const char ch = c.command[i];
						clean[i] = (ch >= 32 && ch < 127) ? ch : '.';
					}
					LOG_MSG("SU2000: TRK%d command '%s'",
					        card + 1,
					        clean);
				}
				su2000_trk_command(c, c.command);
				c.command.clear();
			}
		} else if (v == 'D' || v == 'C' || v == 'P') {
			su2000_trk_command(c, std::string(1, static_cast<char>(v)));
		} else if (c.command.size() < 63) {
			c.command.push_back(static_cast<char>(v));
		}
	} else if (reg == 1 && v == 0x00 && cardst.streaming) {
		// Sample request from the polled reader (ETS 0x1ca9a9). The card
		// already free-runs, so treat it as a nudge: make the next sample
		// due immediately rather than queueing a second record here.
		cardst.stream_next_ms = 0;
	}
	su2000_trk_trace(card, reg, true, v);
}

} // namespace

void SU2000_Tracker_AddConfig(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool = section.AddBool("tracker", OnlyAtStart, false);
	pbool->SetHelp(
	        "Claim the Polhemus InsideTrak tracker ports TRK1/TRK2 ('off' by\n"
	        "default). Every access is logged with CS:IP; the open probes are\n"
	        "answered ('W' sign-on, 'S' status) and after 'y' the card streams\n"
	        "an 8-word data record per active sensor. Records carry a synthetic\n"
	        "animated pose so motion-dependent views stay live.\n"
	        "\n"
	        "Note: TRK2 at 278 overlaps LPT2 (278-27A); keep 'parallel2' off.\n"
	        "Requires a guest restart; takes effect at startup.");

	auto phex = section.AddHex("tracker1", OnlyAtStart, 0x270);
	phex->SetHelp(
	        "TRK1 InsideTrak IO base (270 by default = SU solo in CONFIG.VPC).\n"
	        "S1 options: 270, 278, plus 4/8/10/20/40/80/100/200 adders.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	phex = section.AddHex("tracker2", OnlyAtStart, 0x278);
	phex->SetHelp(
	        "TRK2 InsideTrak IO base (278 by default = second card in a\n"
	        "2-player unit). Same options as 'tracker1'.\n"
	        "\n"
	        "Note: Requires a guest restart; takes effect at startup.");

	pbool = section.AddBool("tracker_log", Always, true);
	pbool->SetHelp(
	        "Log SU-2000 tracker port traffic with CS:IP ('on' by default).\n"
	        "Identical repeats collapse to a summary line so status-poll loops\n"
	        "stay readable.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
}

bool SU2000_Tracker_Install(const SectionProp& section)
{
	if (!section.GetBool("tracker")) {
		return false;
	}
	const uint16_t trk_bases[2] = {
	        static_cast<uint16_t>(section.GetHex("tracker1")),
	        static_cast<uint16_t>(section.GetHex("tracker2"))};
	bool tracker_ok = false;
	for (int c = 0; c < 2; ++c) {
		const auto base = trk_bases[c];
		if (base < 0x100 || base > 0x3F8 || (base & 0x7u) != 0u) {
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
		trk_cards[c].command.clear();
		trk_cards[c].active_sensors = 0;
		trk_cards[c].streaming      = false;
		trk_cards[c].stream_sensor  = 0;
		trk_cards[c].stream_bytes   = 0;
		trk_cards[c].read_handle.Install(base,
		                                 su2000_trk_read,
		                                 io_width_t::dword,
		                                 Su2000TrkPorts);
		trk_cards[c].write_handle.Install(base,
		                                  su2000_trk_write,
		                                  io_width_t::dword,
		                                  Su2000TrkPorts);
		trk_cards[c].card_installed = true;
		su2000_claim_range(base, Su2000TrkPorts);
		tracker_ok = true;
	}
	if (tracker_ok) {
		LOG_MSG("SU2000: tracker tracer build %s (TRK1 0x%03x TRK2 0x%03x)",
		        SU2000_BUILD_ID,
		        trk_cards[0].card_installed ? trk_cards[0].base : 0,
		        trk_cards[1].card_installed ? trk_cards[1].base : 0);
	}
	return tracker_ok;
}

void SU2000_Tracker_Uninstall()
{
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
	for (auto& card : trk_cards) {
		card.base = 0;
		for (auto& p : card.ports) {
			p = Su2000TrkPortLog{};
		}
		card.fifo.clear();
		card.fifo_pos = 0;
		card.s_armed  = false;
		card.s_words  = 0;
		card.w_count  = 0;
		card.command.clear();
		card.active_sensors = 0;
		card.streaming      = false;
		card.stream_sensor  = 0;
		card.stream_bytes   = 0;
	}
	stuck_dump_done = false;
}

bool SU2000_Tracker_Installed()
{
	return trk_cards[0].card_installed || trk_cards[1].card_installed;
}
