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
// 4 VIP bases x 16 registers (offsets +0..+0xF).
constexpr int Vfx1NumSlots = 4 * 16;

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
	// Failure-point tier (xr_log_vip): unusual VIP events only, so
	// healthy detection traffic stays silent.
	const auto* section = get_section("xr");
	if (!section) {
		return true;
	}
	return section->GetBool("xr_log_vip");
}

static bool vfx1_logging_full()
{
	// Full-trace tier (xr_log_vip_full): every VIP access with CS:IP.
	// Very verbose: read it from dosbox-xr.log, not the window.
	const auto* section = get_section("xr");
	if (!section) {
		return false;
	}
	return section->GetBool("xr_log_vip_full");
}

// Full-trace line for one classified access (no dedup by design:
// identical repeats still flip emulated device state).
static void vfx1_mon_trace(const io_port_t port, const int slot,
                           const bool write, const uint32_t value,
                           const bool wide)
{
	if (!vfx1_logging_full()) {
		return;
	}
	LOG_MSG("VFX1: VIP %s%s +0x%x (0x%03x) = 0x%02x [%04x:%04x]",
	        write ? "write" : "read",
	        wide ? "16" : "",
	        slot % 16,
	        port,
	        value,
	        SegValue(cs),
	        reg_eip & 0xffffu);
}

// Failure-point monitor implementations live after the responder state
// below (they need the latch/FIFO/IRQ state); the two getters and the
// full-trace helper above are all the logging tier logic needed here.

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
// Read-count state for the +0xB completion sequence below.
static int vfx1_b_reads[4] = {}; // +0xB reads since last write
static constexpr int Vfx1CompleteReads = 10; // echo reads before completion
// Build stamp: bump on responder changes so logs identify the code.
#define VFX1_BUILD_ID "2026-10-10/grace5000+echo+fed+irqfix+cfgready+fpmon+irqunmask+irqtrig1+backclear"
// Bases with responders installed (unclaimed ports only).
static bool vfx1_installed[4] = {};
// Ports the responder itself installed (to tell prior claimants apart).
static bool vfx1_port_installed[Vfx1NumSlots] = {};

// ---- Failure-point monitor (see VFX1.md sections 2, 5 and 8) ----
// Healthy traffic is silent at the xr_log_vip tier; each item below
// fires only on a condition that fails (or nearly fails) detection:
// FP1 responder-off touch, FP2 echo mismatch, FP3 HWID/version reject,
// FP4 CFGSTATUS unreadiness, FP5 IRQCFG readback divergence, FP6/FP7
// status/completion waiter spins, FP8 FIFO-replay dry, FP9 unserviced
// IRQ, FP11 VIP-IRQ masking. FP12 (word IO) is full-trace only.
static constexpr int Vfx1SpinWarn = 20000; // identical status polls: near the firmware ~30k wait cap (Error-27 class)
static constexpr int Vfx1SpinNote = 1000; // completed-wait summary threshold
struct Vfx1Mon {
	bool off_reported = false; // FP1 logged for this base
	bool echo_written = false; // +0x0 write seen (FP2 arms)
	uint8_t echo_val = 0;
	bool id_seen = false; // +0xC/D/E read seen (FP3)
	bool det_reported = false; // detection-OK summary logged
	int spin_off = -1; // offset of the active identical-read run (-1 none)
	uint8_t spin_val = 0;
	int spin_n = 0;
	bool spin_warned = false;
	bool dry_reported = false; // FP8 warning per write stream
};
static Vfx1Mon vfx1_mon[4] = {};
static int vfx1_raise_no_eoi = 0; // FP9: IRQ raises since last PIC EOI
static bool vfx1_irq_unmasked[4] = {}; // PIC line unmasked per base

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
// Valid IRQs per SDK: 5, 7, 10, 11, 12, 15 (from SET VIPPORT=2x0 Iy)
static int vfx1_irq()
{
	const auto* section = get_section("xr");
	int irq             = 7;
	if (section) {
		irq = section->GetInt("xr_vip_irq");
	}
	// Restrict to valid VIP card IRQs per hardware jumpers
	static const int valid_irqs[] = {5, 7, 10, 11, 12, 15};
	bool valid = false;
	for (int v : valid_irqs) {
		if (irq == v) {
			valid = true;
			break;
		}
	}
	if (!valid) {
		irq = 7; // Default to IRQ 7
	}
	return irq;
}

static void vfx1_raise_irq(const int base)
{
	if (!vfx1_respond_enabled() || base < 0 || base > 3 ||
	    !vfx1_installed[base]) {
		return;
	}
	if (vfx1_logging_full()) {
		LOG_MSG("VFX1: VIP raise IRQ %d base%d",
		        vfx1_irq(),
		        base);
	}
	++vfx1_raise_no_eoi;
	// FP9: repeated raises with no EOI means the guest ISR never runs.
	if (vfx1_raise_no_eoi == 10 && vfx1_logging_enabled()) {
		LOG_MSG("VFX1: VIP IRQ %d raised 10x with no PIC EOI (guest ISR may not be servicing it)",
		        vfx1_irq());
	}
	PIC_ActivateIRQ(static_cast<uint8_t>(vfx1_irq()));
}

void vfx1_lower_irq()
{
	vfx1_raise_no_eoi = 0;
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
	if (offset == 0x0 || offset == 0x1 || offset == 0x2 || offset == 0x8 ||
	    offset == 0x9 || offset == 0xB) {
		// Command traffic pends a response (AB2PCEMPTY clears when it
		// surfaces, see decl). Arming is normal traffic: full tier only.
		vfx1_resp_ready[base] = true;
		vfx1_resp_pending[base] = true;
		vfx1_resp_grace[base] = Vfx1RespGrace;
		vfx1_resp_fed[base] = false;
		vfx1_mon_trace(port, slot, true, value, false);
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
		vfx1_mon[base].dry_reported = false;
	}
	if (offset == 0xB) {
		// Raise only on completion traffic: per-byte raises would
		// fire the guest ISR mid-handshake and its status polls
		// would consume the response window early.
		vfx1_raise_irq(base);
	}
	if (offset == 0xA && (value & 0x10u) != 0u) {
		// IRQCFG test-mode write fires the card's single test interrupt:
		// SETVFX1's IRQ test hooks the vector, writes IRQCFG (modes 1
		// then 0, combined byte = (irq & 0x7F) | (mode << 4)) and waits
		// for exactly one ISR run that only EOI's and bumps a flag.
		// Only the mode-1 (bit 4) write edges: with a raise on both,
		// the flag would land on 2 and the exact-1 verdict would fail.
		// PIC IRR collapse keeps even doubled writes to one pending
		// line, same as hardware edges.
		vfx1_raise_irq(base);
	}
	if (offset == 0x2) {
		return;
	}
	// +0xC/D/E are ID ROM (writes ignored, reads fixed); everything
	// else latched below mirrors AIR-style R/W registers.
	if (offset != 0x0 && offset != 0x1 && offset != 0x8 && offset != 0x9 &&
	    offset != 0xB && offset != 0xF) {
		return;
	}
	if (offset == 0x8) {
		// PC2ABACK (bit 1) is VIP-driven status: the bit the guest sets
		// in its own control writes is meaningless to real hardware and
		// must not be echoed back, or VFX1.COM's BACK-waiter (fast-pass
		// on BACK-set with caller retry-while-zero) spins forever on a
		// self-sustaining latch. The VIP deasserts BACK once it consumes
		// our ACK, and this model is never busy, so keep it clear.
		auto stored = static_cast<uint8_t>(value & ~0x02u);
		vfx1_reg_latch[base][offset] = stored;
	} else {
		vfx1_reg_latch[base][offset] = value;
	}
	vfx1_reg_valid[base][offset] = true;
	if (offset == 0xB) {
		vfx1_b_reads[base] = 0; // echo window re-arms
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
			case 0x01: // CSR - ACCESS.bus Control/Status Register
				if (vfx1_reg_valid[base][0x1]) {
					value = vfx1_reg_latch[base][0x1];
				} else {
					// Default: AB2PCEMPTY bit reflects response state
					value = vfx1_resp_ready[base] ? 0x20u : 0x00u;
				}
				break;
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
						if (vfx1_logging_full()) {
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
								    vfx1_logging_full()) {
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
									if (vfx1_logging_full()) {
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
						} else if (vfx1_logging_full()) {
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
					// CFGSTATUS - Configuration Status register
					// Bits 0-2: Boot progress: report complete (7)
					// immediately. Firmware checks
					// (IN & 0x06) == 0x06 with a single read, so
					// a multi-thousand-read ramp fails detection.
					// Bit 3:   Configuration valid
					// Bit 4:   ACCESS.bus enabled
					// Bit 5:   IRQ configured
					// Bits 6-7: Reserved
					uint8_t cfg = 0x07; // boot complete
					if (vfx1_installed[base]) cfg |= 0x08; // Config valid
					if (vfx1_reg_valid[base][0x1] && (vfx1_reg_latch[base][0x1] & 0x80)) cfg |= 0x10; // PCIEN/ACCESS.bus enabled
					if (vfx1_irq() > 0) cfg |= 0x20; // IRQ configured
					value = (value & ~0x3Fu) | cfg;
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
	install(0x0);  // AIR
	install(0x1);  // CSR (ACCESS.bus Control/Status Register)
	install(0x2);  // DFIN (Data FIFO In)
	install(0x4);  // DFOUT (Data FIFO Out)
	install(0x8);  // Control Register
	install(0x9);  // Data Register
	install(0xA);  // VIP_IRQCFG
	install(0xB);  // Completion Register
	install(0xC);  // HWID
	install(0xD);  // HWVLOW
	install(0xE);  // HWVHI
	install(0xF);  // CFGSTATUS
	if (vfx1_installed[base] && !vfx1_irq_unmasked[base]) {
		// The SDK software never unmasks its own line (no OUT 0x21/0xA1
		// in SETVFX1.EXE or VFX1.COM) and DOSBox boots the VIP IRQ
		// masked, so a raised IRQ would pend forever and the IRQ test
		// time out. Real BIOS POST left the line unmasked: mirror that
		// here, same as IDE/MPU-401/PS2-mouse do for their lines.
		vfx1_irq_unmasked[base] = true;
		PIC_SetIRQMask(static_cast<uint32_t>(vfx1_irq()), false);
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP unmasked IRQ %d (base%d)",
			        vfx1_irq(),
			        base);
		}
	}
}
// Failure-point monitor: every classified VIP access lands here after
// the handler runs (reads carry the value the guest actually saw).
void log_vfx1_port_access(const io_port_t port, const bool write,
                          const uint8_t value)
{
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	const int base   = slot / 16;
	const int offset = slot % 16;
	auto& mon = vfx1_mon[base];
	vfx1_mon_trace(port, slot, write, value, false);
	if (!vfx1_respond_enabled()) {
		// FP1: guest probes while the responder is off -> open-bus
		// 0xFF reads, i.e. VFX1.md section 5b "base port not
		// detected". Once per base.
		if (vfx1_logging_enabled() && !mon.off_reported) {
			mon.off_reported = true;
			LOG_MSG("VFX1: VIP base%d touched while xr_vip_respond is off (reads float 0xFF: firmware reports 'base port not detected')",
			        base);
		}
		return;
	}
	if (write) {
		// Any write ends an identical-read run; summarize long waits.
		if ((mon.spin_off == 0x8 || mon.spin_off == 0xB) &&
		    vfx1_logging_enabled() && mon.spin_n >= Vfx1SpinNote) {
			LOG_MSG("VFX1: VIP base%d status wait on +0x%x ended after %d identical polls (0x%02x)",
			        base,
			        mon.spin_off,
			        mon.spin_n,
			        mon.spin_val);
		}
		mon.spin_off = -1;
		mon.spin_n = 0;
		mon.spin_warned = false;
		if (offset == 0x0) {
			mon.echo_written = true;
			mon.echo_val = value;
		}
		return;
	}
	// --- reads below ---
	if (offset == 0x8 || offset == 0xB) {
		// FP6/FP7: AB2PCEMPTY (+0x8) and completion (+0xB) waiter loops.
		if (mon.spin_off == offset && mon.spin_val == value) {
			++mon.spin_n;
			if (mon.spin_n == Vfx1SpinWarn && !mon.spin_warned) {
				mon.spin_warned = true;
				if (vfx1_logging_enabled()) {
					LOG_MSG("VFX1: VIP base%d STILL polling +0x%x = 0x%02x x%d (firmware wait cap ~30k: Error-27 class, see VFX1.md section 5c)",
					        base,
					        offset,
					        value,
					        mon.spin_n);
				}
			}
		} else {
			mon.spin_off = offset;
			mon.spin_val = value;
			mon.spin_n = 1;
			mon.spin_warned = false;
		}
	} else if (mon.spin_off == 0x8 || mon.spin_off == 0xB) {
		// Wait ended by a read elsewhere; summarize long waits.
		if (vfx1_logging_enabled() && mon.spin_n >= Vfx1SpinNote) {
			LOG_MSG("VFX1: VIP base%d status wait on +0x%x ended after %d identical polls (0x%02x)",
			        base,
			        mon.spin_off,
			        mon.spin_n,
			        mon.spin_val);
		}
		mon.spin_off = -1;
		mon.spin_n = 0;
		mon.spin_warned = false;
	}
	switch (offset) {
	case 0x0:
		// FP2: echo-latch readback (VFX1.md section 5a: 0x0A then 0x05).
		if (mon.echo_written && value != mon.echo_val) {
			mon.echo_written = false; // report once per write
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: VIP base%d echo MISMATCH: wrote 0x%02x, read 0x%02x (latch test fails: detection fail code 1/2)",
				        base,
				        mon.echo_val,
				        value);
			}
		}
		break;
	case 0xA:
		// FP5: IRQCFG readback vs what the guest wrote (VFX1.md
		// section 5c: firmware writes a combined byte, responder
		// answers the plain IRQ number).
		if (vfx1_reg_valid[base][0xA] &&
		    value != vfx1_reg_latch[base][0xA] &&
		    vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP base%d IRQCFG wrote 0x%02x but reads 0x%02x (combined-vs-plain mismatch)",
			        base,
			        vfx1_reg_latch[base][0xA],
			        value);
		}
		break;
	case 0xC:
		mon.id_seen = true;
		// FP3: HWID must be 0x0D or VFX1.COM rejects the card.
		if (value != 0x0D && vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP base%d HWID reads 0x%02x, firmware requires 0x0D (card rejected)",
			        base,
			        value);
		}
		break;
	case 0xD:
	case 0xE:
		mon.id_seen = true;
		// FP3: version bytes must not be 0xFF (floating bus).
		if (value == 0xFFu && vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP base%d HWV (+0x%x) reads 0xFF (open bus: card rejected)",
			        base,
			        offset);
		}
		break;
	case 0xF:
		// FP4: (IN & 0x06) == 0x06 or the card reads unusable.
		if ((value & 0x06u) != 0x06u) {
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: VIP base%d CFGSTATUS reads 0x%02x: (x & 6) != 6 (card unusable)",
				        base,
				        value);
			}
		} else if (mon.echo_written && mon.id_seen &&
		           !mon.det_reported) {
			mon.det_reported = true;
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: VIP base%d detection sequence OK (echo latch + HWID 0x0D + CFGSTATUS 0x%02x)",
				        base,
				        value);
			}
		}
		break;
	case 0x9:
	case 0x4:
		// FP8: FIFO replay dry while a response is still pending:
		// firmware verify reads would fail.
		if (value == 0xFFu && vfx1_resp_pending[base] &&
		    vfx1_echo_pos[base] >= vfx1_echo[base].size() &&
		    !mon.dry_reported) {
			mon.dry_reported = true;
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: VIP base%d FIFO replay dry on +0x%x (%u bytes replayed, response still pending)",
				        base,
				        offset,
				        static_cast<unsigned>(
				                vfx1_echo_pos[base]));
			}
		}
		break;
	default:
		break;
	}
}

// Word-wide twin: firmware uses byte IO, so word traffic to VIP ports
// is anomalous by itself (FP12). Full trace only.
void log_vfx1_port_access_wide(const io_port_t port, const bool write,
                               const uint16_t value)
{
	int slot = -1;
	if (!vfx1_classify_port(port, slot)) {
		return;
	}
	vfx1_mon_trace(port, slot, write, value, true);
}

// External linkage (declared in hardware/port.h): failure-boundary dump,
// called when DOS prints an ERROR line: active waiter spins, pending
// responses and unserviced IRQs show up with the error.
void VFX1_LogPollCounts()
{
	if (!vfx1_logging_enabled()) {
		return;
	}
	for (int base = 0; base < 4; ++base) {
		auto& mon = vfx1_mon[base];
		if (!vfx1_installed[base] && !mon.off_reported) {
			continue;
		}
		if (mon.spin_off == 0x8 || mon.spin_off == 0xB) {
			LOG_MSG("VFX1: VIP base%d was polling +0x%x = 0x%02x x%d at failure boundary",
			        base,
			        mon.spin_off,
			        mon.spin_val,
			        mon.spin_n);
		}
		if (vfx1_resp_pending[base]) {
			LOG_MSG("VFX1: VIP base%d response pending (grace %d, fed %d) at failure boundary",
			        base,
			        vfx1_resp_grace[base],
			        static_cast<int>(vfx1_resp_fed[base]));
		}
	}
	if (vfx1_raise_no_eoi > 0) {
		LOG_MSG("VFX1: VIP IRQ raised %d time(s) with no PIC EOI at failure boundary (guest ISR not servicing?)",
		        vfx1_raise_no_eoi);
	}
}

// External linkage (declared in hardware/port.h): guest PIC mask
// writes call in here with the pre/post mask bytes. Masking the VIP IRQ
// itself (FP11) is a failure; other watched-line transitions are
// full-trace only. Programmatic mask changes (emulator init) do not
// come through here.
void VFX1_NotifyIrqMask(const int pic, const uint8_t old_imr,
                        const uint8_t new_imr)
{
	if (pic < 0 || pic > 1) {
		return;
	}
	const uint8_t watched = (pic == 0) ? Vfx1WatchedIrqMaster
	                                   : Vfx1WatchedIrqSlave;
	const uint8_t changed = (old_imr ^ new_imr) & watched;
	if (!changed) {
		return;
	}
	const int vip_irq = vfx1_irq();
	for (int bit = 0; bit < 8; ++bit) {
		if (!(changed & (1u << bit))) {
			continue;
		}
		const int irq     = (pic == 0) ? bit : bit + 8;
		const bool in_use = (new_imr & (1u << bit)) == 0;
		if (irq == vip_irq && !in_use) {
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: VIP IRQ %d MASKED by guest (completion IRQs lost: init-timeout class)",
				        irq);
			}
		} else if (vfx1_logging_full()) {
			LOG_MSG("VFX1: IRQ %d %s",
			        irq,
			        in_use ? "unmasked (in use)" : "masked");
		}
	}
}
