// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware/vfx1.h"

#include "port.h"

#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

#include "config/setup.h"
#include "cpu/cpu.h"
#include "misc/support.h"
#include "pic.h"
#include "timer.h"

constexpr io_port_t Vfx1VipBases[] = {0x260, 0x280, 0x2a0, 0x2c0};
// 4 VIP bases x 16 registers (offsets +0..+0xF; offsets log raw).
constexpr int Vfx1NumSlots = 4 * 16;

static StereoPortSlot vfx1_slots[Vfx1NumSlots] = {};

// Bit masks (within master/slave IMR byte) of the watched IRQ lines.
constexpr uint8_t Vfx1WatchedIrqMaster = (1u << 5) | (1u << 7);
constexpr uint8_t Vfx1WatchedIrqSlave =
        (1u << (10 - 8)) | (1u << (11 - 8)) | (1u << (12 - 8)) |
        (1u << (15 - 8));
// VFX-1 VIP-card detection: base ports 260h/280h/2A0h/2C0h (the four
// VIPPORT jumper options). The VIP register map spans +0..+0xF
// (VFX1.H: AIR +0x0, HWID +0xC, HWVLOW +0xD, HWVHI +0xE, CFGSTATUS
// +0xF), so slots are 16 wide; offsets log raw (on-change: repeats collapse,`r`n// like the stereo spies in port_containers.cpp).
static bool vfx1_classify_port(const io_port_t port, int& slot)
{
	if (port < 0x260 || port > 0x2cf) {
		return false;
	}
	for (int i = 0; i < 4; ++i) {
		if (port >= Vfx1VipBases[i] && port < Vfx1VipBases[i] + 16) {
			slot = i * 16 + static_cast<int>(port - Vfx1VipBases[i]);
			return true;
		}
	}
	return false;
}

static bool vfx1_logging_enabled()
{
	// Live toggle from the [xr] section. Missing section means
	// fail-open: diagnostics default on (same as the LPT/COM spies).
	const auto* section = get_section("xr");
	if (!section) {
		return true;
	}
	return section->GetBool("xr_log_vip");
}

// Full-trace mode: log every VIP access with no dedup or burst
// collapsing. The collapsed log hides state-changing repeats
// (identical re-sends/re-reads still flip device state), so use this
// when the collapsed sequence contradicts itself. Very verbose:
// read it from dosbox-xr.log, not the window.
static bool vfx1_logging_all()
{
	const auto* section = get_section("xr");
	if (!section) {
		return false;
	}
	return section->GetBool("xr_log_vip_all");
}

// Burst collapsing for FIFO streams (DFIN writes, DFOUT reads):
// rapid same-port bursts log chunk summaries instead of one line per
// byte, so firmware downloads stay readable. First byte logs inline;
// every 32nd byte logs progress; the tail summarizes on the next
// boundary (gap, other port, or any other VIP access).
struct VipBurst {
	bool active    = false;
	io_port_t port = 0;
	int count      = 0;
	int64_t last_us = 0;
};
static VipBurst dfin_burst;
static VipBurst dfout_burst;
static constexpr int64_t Vfx1BurstGapUs = 100'000; // 100 ms
static constexpr int Vfx1BurstChunk = 32;

static void vfx1_burst_flush(VipBurst& b, const char* what)
{
	if (!b.active) {
		return;
	}
	b.active = false;
	if (b.count > 1) {
		LOG_MSG("VFX1: VIP %s burst (0x%03x): %d bytes total",
		        what,
		        b.port,
		        b.count);
	}
}

// Absorbs one stream byte; returns true when the caller should skip
// its inline log line.
static bool vfx1_burst_absorb(VipBurst& b, io_port_t port, const char* what)
{
	const int64_t now = GetTicksUs();
	if (b.active && port == b.port && now - b.last_us < Vfx1BurstGapUs) {
		++b.count;
		b.last_us = now;
		if (b.count % Vfx1BurstChunk == 0) {
			LOG_MSG("VFX1: VIP %s burst (0x%03x): %d bytes so far",
			        what,
			        port,
			        b.count);
		}
		return true;
	}
	vfx1_burst_flush(b, what);
	b.active  = true;
	b.port    = port;
	b.count   = 1;
	b.last_us = now;
	return false;
}

// Identical-read poll counts suppressed above (per slot).
static int vfx1_read_repeats[Vfx1NumSlots] = {};
// Last liveness line per slot: sustained polls re-log at most every
// few seconds so hangs identify their port without flooding.
static int64_t vfx1_live_at[Vfx1NumSlots] = {};
static constexpr int64_t Vfx1LiveEveryUs = 5'000'000; // 5 s

void log_vfx1_port_access(const io_port_t port, const bool write,
                                 const uint8_t value)
{
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	if (!vfx1_logging_enabled()) {
		return;
	}
	if (vfx1_logging_all()) {
		LOG_MSG("VFX1: VIP %s +0x%x (0x%03x) = 0x%02x [%04x:%04x]",
		        write ? "write" : "read",
		        slot % 16,
		        port,
		        value,
		        SegValue(cs),
		        reg_eip & 0xffffu);
		return;
	}
	// DFIN (+0x2) writes and DFOUT (+0x4) reads stream firmware: collapse
	// rapid same-port bursts so downloads stay readable. First byte
	// logs inline; every 32nd logs progress; the tail summarizes on
	// the next boundary (gap, other port, or any other VIP access,
	// which flushes below before logging).
	const int offset = slot % 16;
	if (write && offset == 0x2) {
		if (vfx1_burst_absorb(dfin_burst, port, "DFIN")) {
			return;
		}
		// fall through: first byte logs inline
	} else if (!write && offset == 0x4) {
		if (vfx1_burst_absorb(dfout_burst, port, "DFOUT")) {
			return;
		}
		// fall through: first byte logs inline
	} else {
		vfx1_burst_flush(dfin_burst, "DFIN");
		vfx1_burst_flush(dfout_burst, "DFOUT");
	}
	auto& entry = vfx1_slots[slot];
	if (write) {
		if (entry.write_valid && entry.last_write == value) {
			return; // steady state stays silent
		}
		entry.write_valid = true;
		entry.last_write  = value;
	} else {
		if (entry.read_valid && entry.last_read == value) {
			// Suppressed identical poll; counted for the
			// failure-boundary report below, with a liveness
			// line every few seconds so hangs identify ports.
			++vfx1_read_repeats[slot];
			const int64_t now = GetTicksUs();
			if (now - vfx1_live_at[slot] >= Vfx1LiveEveryUs) {
				vfx1_live_at[slot] = now;
				LOG_MSG("VFX1: VIP still polling +0x%x (0x%03x) = 0x%02x x%d",
				        slot % 16,
				        port,
				        value,
				        vfx1_read_repeats[slot]);
			}
			return;
		}
		entry.read_valid = true;
		entry.last_read  = value;
	}
	if (!write && vfx1_read_repeats[slot] > 0) {
		LOG_MSG("VFX1: VIP read +0x%x (0x%03x) = 0x%02x (after %d identical polls) [%04x:%04x]",
		        slot % 16,
		        port,
		        value,
		        vfx1_read_repeats[slot],
		        SegValue(cs),
		        reg_eip & 0xffffu);
		vfx1_read_repeats[slot] = 0;
	} else {
		LOG_MSG("VFX1: VIP %s +0x%x (0x%03x) = 0x%02x [%04x:%04x]",
		        write ? "write" : "read",
		        slot % 16,
		        port,
		        value,
		        SegValue(cs),
		        reg_eip & 0xffffu);
	}
}

// Word-wide twin of the line above: fires in addition to the two byte
// lines, so word IO (in ax,dx) is distinguishable from two byte IOs.
// No dedup here; word traffic to these ports is detection-only rare.
void log_vfx1_port_access_wide(const io_port_t port, const bool write,
                                      const uint16_t value)
{
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	if (!vfx1_logging_enabled()) {
		return;
	}
	if (vfx1_logging_all()) {
		LOG_MSG("VFX1: VIP %s16 +0x%x (0x%03x) = 0x%04x",
		        write ? "write" : "read",
		        slot % 16,
		        port,
		        value);
		return;
	}
	LOG_MSG("VFX1: VIP %s16 +0x%x (0x%03x) = 0x%04x",
	        write ? "write" : "read",
	        slot % 16,
	        port,
	        value);
}

// Register latches for the VIP baseport/init tests, reverse-engineered
// from SETVFX1.EXE/VFX1.COM: +0x0 AIR echoes, +0xC/D/E answer fixed IDs,
// +0x8 is an R/W control register (VFX1.COM does read/OR/AND/write on
// it) whose bit 5 reads AB2PCEMPTY (see below), +0x9/+0xB latch like
// AIR. Claimed ports keep their real handlers; nothing else is emulated.
static uint8_t vfx1_reg_latch[4][16] = {};
static bool vfx1_reg_valid[4][16]    = {};
// Response-ready per base: bit 5 of +0x8.
// +0x8 bit 5 is AB2PCEMPTY (SDK VFX1.H): SET while the VIP-to-PC FIFO
// is empty, CLEAR when response data is available. Command writes
// (+0x0/+0x2/+0x8/+0x9/+0xB) pend a response that surfaces after a short
// grace (per-byte polls still see EMPTY). The first +0x9 read after an
// arm reports EMPTY (0xFF) with a fresh window; later ones report idle
// (0x00) and leave state alone (CLEAR-waits still expire it). The
// out-FIFO itself is never synthesized.
static bool vfx1_resp_ready[4] = {};
static bool vfx1_resp_pending[4] = {}; // response surfacing
static int vfx1_resp_grace[4] = {}; // +0x8 reads before it surfaces
static bool vfx1_resp_fed[4] = {}; // +0x9 refresh used since arm
static uint8_t vfx1_resp_data[4] = {}; // computed +0x9 read value
static uint8_t vfx1_dfout_data[4] = {}; // computed +0x4 read value
static int vfx1_resp_clearreads[4] = {}; // CLEAR reads since surface
// VIP-bound byte stream (firmware downloads/config writes) kept for
// readback: bulk +0x9/+0x4 reads replay it in order (peek index resets
// on every new write). Lets firmware-verify passes pass by construction.
static std::vector<uint8_t> vfx1_echo[4];
static size_t vfx1_echo_pos[4] = {};
static constexpr size_t Vfx1EchoCap = 262144; // 256KB ring
static constexpr size_t Vfx1EchoDrop = 4096; // drop chunk on overflow
static constexpr int Vfx1RespGrace = 5000; // post-write SET polls
// (well within the firmware wait() cap (~30k): CLEAR-wait now succeeds
// on first attempt; SET-waits always match on the first SET)
static constexpr int Vfx1ClearReads = 128; // CLEAR pulse width
// Read-count state for progress/completion sequences below.
static int vfx1_f_reads[4] = {}; // +0xF progress reads since kick
static int vfx1_b_reads[4] = {}; // +0xB reads since last write
static constexpr int Vfx1ProgressReads = 1000; // reads per progress step
static constexpr int Vfx1CompleteReads = 10; // echo reads before completion
// Build stamp: bump on responder changes so logs identify the code.
#define VFX1_BUILD_ID "2026-10-10/grace5000+echo+fed+irqfix"
// Bases with responders installed (unclaimed ports only).
static bool vfx1_installed[4] = {};
// Ports the responder itself installed (to tell prior claimants apart).
static bool vfx1_port_installed[4 * 16] = {};

static bool vfx1_respond_enabled()
{
	// Fail-closed: never answer hardware unasked.
	const auto* section = get_section("xr");
	if (!section) {
		return false;
	}
	return section->GetBool("xr_vip_respond");
}

// VIP interrupt request line (VIPPORT IRQ, 7 by default). Raised when
// the emulated engine finishes command traffic (see latch_write):
// the driver waits on completion IRQs with ~40 ms timeouts, so a
// silent engine fails every init stage. Lowered only on EOI: status
// reads must not cancel a pending IRQ before the wait starts.
static int vfx1_irq()
{
	const auto* section = get_section("xr");
	int irq             = 7;
	if (section) {
		irq = section->GetInt("xr_vip_irq");
	}
	if (irq < 0) {
		irq = 0;
	}
	if (irq > 15) {
		irq = 15;
	}
	return irq;
}

static void vfx1_raise_irq(const int base)
{
	if (!vfx1_respond_enabled() || base < 0 || base > 3 ||
	    !vfx1_installed[base]) {
		return;
	}
	if (vfx1_logging_enabled()) {
		LOG_MSG("VFX1: VIP raise IRQ %d base%d",
		        vfx1_irq(),
		        base);
	}
	PIC_ActivateIRQ(static_cast<uint8_t>(vfx1_irq()));
}

void vfx1_lower_irq()
{
	PIC_DeActivateIRQ(static_cast<uint8_t>(vfx1_irq()));
}

void vfx1_latch_write(const io_port_t port, const uint8_t value)
{
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	const int base   = slot / 16;
	const int offset = slot % 16;
	if (offset == 0x0 || offset == 0x2 || offset == 0x8 ||
	    offset == 0x9 || offset == 0xB) {
		// Command traffic pends a response (AB2PCEMPTY clears when it
		// surfaces, see decl).
		vfx1_resp_ready[base] = true;
		vfx1_resp_pending[base] = true;
		vfx1_resp_grace[base] = Vfx1RespGrace;
		vfx1_resp_fed[base] = false;
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP arm base%d (write +0x%x)",
			        base,
			        offset);
		}
	}
	if (offset == 0x9 || offset == 0x2) {
		// Record VIP-bound bytes for readback (firmware verify);
		// a new write stream restarts replay from the oldest kept.
		auto& fifo = vfx1_echo[base];
		if (fifo.size() >= Vfx1EchoCap) {
			fifo.erase(fifo.begin(),
			           fifo.begin() + Vfx1EchoDrop);
		}
		fifo.push_back(value);
		vfx1_echo_pos[base] = 0;
	}
	if (offset == 0xB) {
		// Raise only on completion traffic: per-byte raises would
		// fire the guest ISR mid-handshake and its status polls
		// would consume the response window early.
		vfx1_raise_irq(base);
	}
	if (offset == 0x2) {
		return;
	}
	// +0xC/D/E are ID ROM (writes ignored, reads fixed); everything
	// else latched below mirrors AIR-style R/W registers.
	if (offset != 0x0 && offset != 0x8 && offset != 0x9 &&
	    offset != 0xB && offset != 0xF) {
		return;
	}
	vfx1_reg_latch[base][offset] = value;
	vfx1_reg_valid[base][offset] = true;
	if (offset == 0xB) {
		vfx1_b_reads[base] = 0; // echo window re-arms
	}
	if (offset == 0xF) {
		vfx1_f_reads[base] = 0; // progress restarts on re-config
	}
}

// Read handler installed lazily on the first classified touch of an
// unclaimed VIP base: answers echo/ID/latch registers while responding,
// open bus otherwise. Claimed ports never reach here (their handlers
// win), and later claimants overwrite this entry, so real hardware
// always wins.
static io_val_t vfx1_reg_handler(const io_port_t port, const io_width_t)
{
	uint8_t value = 0xff;
	if (vfx1_respond_enabled()) {
		int slot = -1;
		if (vfx1_classify_port(port, slot)) {
			const int base   = slot / 16;
			const int offset = slot % 16;
			if (offset == 0x9) {
				// First read after an arm reports idle (0x00)
				// with a fresh window; later ones replay the
				// recorded VIP-bound stream while CLEAR
				// (firmware verify), else report live state
				// (EMPTY 0xFF, drained 0x00). Retry loops can't
				// re-arm it forever.
				const bool first = !vfx1_resp_fed[base];
				if (vfx1_logging_enabled()) {
					LOG_MSG("VFX1: VIP data9 base%d fed=%d rdy=%d gr=%d ech=%u pos=%u",
					        base,
					        (int)vfx1_resp_fed[base],
					        (int)vfx1_resp_ready[base],
					        vfx1_resp_grace[base],
					        (unsigned)vfx1_echo[base].size(),
					        (unsigned)vfx1_echo_pos[base]);
				}
				vfx1_resp_fed[base] = true;
				if (first) {
					vfx1_resp_data[base] = 0x00u;
					vfx1_resp_ready[base] = true;
					vfx1_resp_grace[base] =
					        Vfx1RespGrace;
				} else if (!vfx1_resp_ready[base] &&
				           vfx1_echo_pos[base] <
				                   vfx1_echo[base].size()) {
					vfx1_resp_data[base] =
					        vfx1_echo[base]
					                [vfx1_echo_pos[base]++];
				} else {
					vfx1_resp_data[base] =
					        vfx1_resp_ready[base] ? 0xffu
					                              : 0x00u;
				}
			}
			if (offset == 0x4) {
				// DFOUT readback replays the recorded stream.
				if (vfx1_echo_pos[base] <
				    vfx1_echo[base].size()) {
					vfx1_dfout_data[base] =
					        vfx1_echo[base]
					                [vfx1_echo_pos[base]++];
				} else {
					vfx1_dfout_data[base] = 0xffu;
				}
			}
			switch (offset) {
			case 0x0a: value = vfx1_irq(); break; // VIP_IRQCFG: returns configured IRQ
			case 0x0c: value = 0x0d; break;
			case 0x0d: value = 0x01; break; // any non-FF passes
			case 0x0e: value = 0x01; break; // any non-FF passes
			case 0x4: value = vfx1_dfout_data[base]; break;
			case 0x0:
			case 0x8:
			case 0x9:
			case 0xB:
			case 0xF:
				if (vfx1_reg_valid[base][offset]) {
					// Data reads report the computed FIFO
					// value (see above); the latch only backs
					// control/echo registers.
					value = (offset == 0x9)
					        ? vfx1_resp_data[base]
					        : vfx1_reg_latch[base][offset];
static uint16_t vfx1_last_ret[4] = {}; // last logged wait site
					if (offset == 0x8) {
						// Log the wait() call site once per
						// site (return IP off the stack).
						if (vfx1_logging_all()) {
							const uint32_t site_ss =
							        static_cast<uint32_t>(
							                SegValue(ss)) *
							        16u;
							const uint16_t site_ret =
							        mem_readw(site_ss +
							                 reg_sp + 6);
							if (site_ret !=
							    vfx1_last_ret[base]) {
								vfx1_last_ret[base] =
								        site_ret;
								LOG_MSG("VFX1: VIP wait-site base%d ret=%04x",
								        base,
								        site_ret);
							}
						}
						// AB2PCEMPTY: a pending response
						// surfaces when the grace window
						// runs out (see decl); idle always
						// reads EMPTY-SET.
						if (vfx1_resp_pending[base]) {
							if (vfx1_resp_grace[base] >
							    0) {
								--vfx1_resp_grace
								        [base];
							}
							if (vfx1_resp_grace
							            [base] ==
							    0) {
								if (vfx1_resp_ready
								            [base] &&
								    vfx1_logging_enabled()) {
									LOG_MSG("VFX1: VIP surface base%d (EMPTY clear grace=%d)",
									        base,
									        vfx1_resp_grace
									                [base]);
								}
								vfx1_resp_ready
								        [base] =
								        false;
								// Raise IRQ when response surfaces (AB2PCEMPTY clears):
								// firmware waits on completion IRQ with ~40ms timeout.
								vfx1_raise_irq(base);
								if (++vfx1_resp_clearreads
								            [base] >=
								    Vfx1ClearReads) {
									// CLEAR is a pulse, not
									// sticky: re-arm so
									// SET-waits always
									// terminate too.
									vfx1_resp_clearreads
									        [base] =
									        0;
									vfx1_resp_ready
									        [base] =
									        true;
									vfx1_resp_grace
									        [base] =
									        Vfx1RespGrace;
									if (vfx1_logging_enabled()) {
										LOG_MSG("VFX1: VIP rearm base%d (pulse end cc=%d)",
										        base,
										        vfx1_resp_clearreads
										                [base]);
									}
								}
							}
						}
						value &= ~0x20u;
						if (vfx1_resp_ready[base]) {
							value |= 0x20u;
						} else if (vfx1_logging_all()) {
							// CLEAR read: dump the guest
							// stack to locate the wait().
							const uint32_t ss_base =
							        static_cast<uint32_t>(
							                SegValue(ss)) *
							        16u;
							const uint32_t sp_base =
							        ss_base + reg_sp;
							LOG_MSG("VFX1: VIP clear-ctx base%d stk=%04x %04x %04x %04x %04x %04x %04x %04x %04x %04x",
							        base,
							        mem_readw(sp_base),
							        mem_readw(sp_base + 2),
							        mem_readw(sp_base + 4),
							        mem_readw(sp_base + 6),
							        mem_readw(sp_base + 8),
							        mem_readw(sp_base + 10),
							        mem_readw(sp_base + 12),
							        mem_readw(sp_base + 14),
							        mem_readw(sp_base + 16),
							        mem_readw(sp_base + 18));
						}
					}
					if (offset == 0xB) {
						// Command completion posts 0x03 in
						// the low two bits (the init poll
						// waits (x & 3) == 3 with a 30000
						// timeout). The first few reads
						// after a write still echo, so the
						// write-verify reads pass.
						if (vfx1_b_reads[base] >=
						    Vfx1CompleteReads) {
							value = (value & ~0x03u) |
							        0x03u;
						}
						++vfx1_b_reads[base];
					}
					if (offset == 0xF) {
						// Boot progress 0..7 (~1000 reads
						// per step): the init polls wait
						// (x & 3) == 3 then (x & 6) == 6
						// with 30000 timeouts. Upper bits
						// echo the configured latch.
						int step = vfx1_f_reads[base] /
						           Vfx1ProgressReads;
						if (step > 7) {
							step = 7;
						}
						++vfx1_f_reads[base];
						value = (value & ~0x07u) |
						        static_cast<uint8_t>(
						                step);
					}
				}
				break;
			default: break;
			}
		}
	}
	return value;
}

void vfx1_maybe_install_base(const io_port_t port)
{
	// Any classified touch installs the base's whole answer set, so
	// probe-first detection (ID reads before any echo write) sees
	// the card on its very first access.
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	if (!vfx1_respond_enabled()) {
		return;
	}
	// Index 0 is the byte lane (same as io_read_byte_handler below).
	// Echo on base+0, control latch on +0x8/+0x9/+0xB, ID answers on
	// +0xC/+0xD/+0xE; only where nothing is claimed.
	auto& byte_lane = io_read_handlers[0];
	const int base = slot / 16;
	const io_port_t base_port = Vfx1VipBases[base];
	auto install = [&](const int offset) {
		const io_port_t p = base_port + offset;
		const int idx     = base * 16 + offset;
		if (vfx1_port_installed[idx]) {
			return; // already ours
		}
		if (!vfx1_installed[base]) {
			LOG_MSG("VFX1: VIP responder build %s (base %d)",
			        VFX1_BUILD_ID,
			        base);
		}
		if (byte_lane.find(p) != byte_lane.end()) {
			// Opt-in VIP responder wins: no real card shares
			// its base, so a prior claimant is a snooper.
			LOG_WARNING("VFX1: VIP responder takes over port 0x%03x",
			            p);
		}
		byte_lane[p]              = vfx1_reg_handler;
		vfx1_port_installed[idx]  = true;
		vfx1_installed[base]      = true;
	};
	install(0x0);
	install(0x8);
	install(0x9);
	install(0xA); // VIP_IRQCFG
	install(0xB);
	install(0xC);
	install(0xD);
	install(0xE);
	install(0xF);
}
// External linkage (declared in hardware/port.h): reports (and resets)
// suppressed identical-poll counts so hidden status-poll loops show up
// at failure boundaries.
void VFX1_LogPollCounts()
{
	for (int slot = 0; slot < Vfx1NumSlots; ++slot) {
		if (vfx1_read_repeats[slot] <= 0) {
			continue;
		}
		LOG_MSG("VFX1: VIP polled +0x%x (0x%03x) = 0x%02x x%d (unchanged)",
		        slot % 16,
		        Vfx1VipBases[slot / 16] + slot % 16,
		        vfx1_slots[slot].last_read,
		        vfx1_read_repeats[slot]);
		vfx1_read_repeats[slot] = 0;
	}
}

// External linkage (declared in hardware/port.h): guest PIC mask
// writes call in here with the pre/post mask bytes; transitions on
// the watched IRQ lines (5/7/10/11/12/15) log. Programmatic mask
// changes (emulator init) do not come through here.
void VFX1_NotifyIrqMask(const int pic, const uint8_t old_imr,
                        const uint8_t new_imr)
{
	if (pic < 0 || pic > 1) {
		return;
	}
	if (!vfx1_logging_enabled()) {
		return;
	}
	const uint8_t watched = (pic == 0) ? Vfx1WatchedIrqMaster
	                                   : Vfx1WatchedIrqSlave;
	const uint8_t changed = (old_imr ^ new_imr) & watched;
	for (int bit = 0; bit < 8; ++bit) {
		if (changed & (1u << bit)) {
			const int irq      = (pic == 0) ? bit : bit + 8;
			const bool in_use  = (new_imr & (1u << bit)) == 0;
			LOG_MSG("VFX1: IRQ %d %s",
			        irq,
			        in_use ? "unmasked (in use)" : "masked");
		}
	}
}
