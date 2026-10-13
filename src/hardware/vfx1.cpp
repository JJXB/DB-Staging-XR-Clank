// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware/vfx1.h"

#include "dosbox_config.h"

#include "hardware/su2000.h"
#include "port.h"

#include <algorithm>
#include <cassert>
#include <cmath>
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
	// Failure-point tier (vfx1_log): unusual VIP events only, so
	// healthy detection traffic stays silent.
	const auto* section = get_section("vfx1");
	if (!section) {
		return true;
	}
	return section->GetBool("vfx1_log");
}

static bool vfx1_logging_full()
{
	// Full-trace tier (vfx1_log_all): every VIP access with CS:IP.
	// Very verbose: read it from dosbox-xr.log, not the window.
	const auto* section = get_section("vfx1");
	if (!section) {
		return false;
	}
	return section->GetBool("vfx1_log_all");
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
// Forward: IRQ raise is defined with the responder below but the bus
// queue (above it) asserts the VIP IRQ on disarmed arrivals.
static void vfx1_raise_irq(const int base);
static int  vfx1_irq();
// Numpad range-sweep testing inputs (mapper hotkeys, momentary): KP7/8/9
// hold sweeps the HMD yaw/pitch/roll, KP2 hold sweeps the puck axes,
// KP4/5/6 hold the puck Top/Middle/Bottom buttons. Only present in
// OpenXR builds (the mapper hotkeys are XR-gated); non-XR builds stay
// centered. No real-hardware motion feeds VFX-1 (no headset pose, no
// controller stick/buttons).
#if C_OPENXR
extern float        sdl_xr_test_hmd[3];
extern float        sdl_xr_test_puck[2];
extern unsigned int sdl_xr_test_puckbtn;
#endif
static constexpr int Vfx1CompleteReads = 10; // echo reads before completion
// Build stamp: bump on responder changes so logs identify the code.
#define VFX1_BUILD_ID "2026-10-10/grace5000+echo+fed+irqfix+cfgready+fpmon+irqunmask+irqtrig1+backclear+dcbias+puckbtn+busatt+rxdrain+rxempty+streamdrip+rxirq+dirid+idaddr+pucklive+headlive+idtruth+isrkick+fullbus+tsrmem+irqlevel+isrpulse+pollreply+irqedge+irqcmd+idseed+rxquench+streamread+validbit+eoiprecise+idkeep+flagmirror+int33drv+int33cfg+numpadonly+puckxy"
// Bases with responders installed (unclaimed ports only).
static bool vfx1_installed[4] = {};
// Ports the responder itself installed (to tell prior claimants apart).
static bool vfx1_port_installed[Vfx1NumSlots] = {};

// ---- Failure-point monitor (see VFX1.md sections 2, 5 and 8) ----
// Healthy traffic is silent at the vfx1_log tier; each item below
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

// ---- Virtual ACCESS.bus devices (VFX1.md section 6) ----
// Host (firmware) packets are detected on the +0x9 write stream; device
// replies are served on +0x9 reads ahead of the echo replay, but ONLY
// inside a firmware RX transfer (armed by an entry-shape +0x8 write).
// NOTE: nothing auto-seeds device bytes. VFX1.COM never sends bus packets
// (69k+31k writes observed, zero IDREQ/ASSIGN shapes; its IDREQ builder is
// dead code with no callers) — the on-card DSP owns enumeration, and an
// always-queued ATTENTION was poisoning the firmware's idle checks
// (expects 0xFF, got 0x50 -> error 27). Replies queue only in answer to a
// real host request, so this layer is inert until engaged.
struct VipBusDev {
	const char* fwver; // 7 chars, e.g. "V1.2___"
	const char* name; // 8 chars, e.g. "VFX1101_"
	uint8_t serial[4];
	uint8_t assigned = 0; // 0 = still on default 0x6E
};
struct VipBus {
	int stage = 0; // 0 = pre-ID (ATTENTION repeating), 1 = IDREQ seen, 2 = both assigned
	std::vector<uint8_t> rx;
	size_t rx_pos = 0;
	std::vector<uint8_t> txwin; // recent host bytes for pattern detect
	int since_ctrl = 1000000; // +0x9 writes since last +0x8 write
	int aa_run = 0; // consecutive +0x9 0xAA writes (bus reset loop)
	bool armed = false; // firmware RX transfer in progress
	bool reporting = false; // host enabled tracking reports (F5)
	int att_seeded = 0; // ATTENTION packets seeded this episode (cap: quiet bus drains)
	bool id_seeded = false; // broadcast ID replies pre-seeded once (see below)
	bool quenched = false; // this armed transfer sees forced-dry (see below)
	unsigned int xfer = 0; // armed transfers since reset (quench cadence)
	bool att_logged = false;
	VipBusDev hmd = {"V1.2___", "VFX1101_", {0xA1, 0xB2, 0xC3, 0xD4}, 0};
	VipBusDev puck = {"V1.4___", "PUCK102_", {0x12, 0x34, 0x56, 0x78}, 0};
};
static VipBus vfx1_bus[4] = {};
static bool vfx1_bus_req[4] = {}; // VIP-side AB2PCREQ (bit 2): transfer completion pending

static uint8_t vfx1_bus_xor(const uint8_t* p, size_t n)
{
	uint8_t x = 0;
	for (size_t i = 0; i < n; ++i) {
		x = static_cast<uint8_t>(x ^ p[i]);
	}
	return x;
}

static void vfx1_bus_queue(const int base, const uint8_t* p, size_t n)
{
	auto& b = vfx1_bus[base];
	(void)base;
	b.rx.insert(b.rx.end(), p, p + n);
	// FIFO level intentionally does NOT raise: on real hardware the
	// VIP interrupts on command edges (IRQCFG strobe, TX completion),
	// and per-byte FIFO interrupts would over-count the exact-1 IRQ
	// test (its ISR runs per vector while streaming). Polled readers
	// drain normally; command sources below raise explicitly.
}

static void vfx1_bus_idreply(const int base, const VipBusDev& d)
{
	// 0x9D = 29 bytes follow: E1 + B + fwver(7) + FORTE___(8) + name(8)
	// + serial(4); trailing XOR checksum (33 bytes on the wire).
	// Byte 1 is the device's current address: default 0x6E until the
	// host assigns it (the TSR only assigns devices still on
	// default), the assigned address after. Never the probed address:
	// echoing the request reads as already-assigned and wedges the
	// TSR's assign decision.
	uint8_t pkt[33];
	size_t i = 0;
	pkt[i++] = 0x50;
	pkt[i++] = d.assigned != 0u ? d.assigned : 0x6Eu;
	pkt[i++] = 0x9D;
	pkt[i++] = 0xE1;
	pkt[i++] = 0x42;
	memcpy(pkt + i, d.fwver, 7);
	i += 7;
	memcpy(pkt + i, "FORTE___", 8);
	i += 8;
	memcpy(pkt + i, d.name, 8);
	i += 8;
	memcpy(pkt + i, d.serial, 4);
	i += 4;
	pkt[i++] = vfx1_bus_xor(pkt, i);
	vfx1_bus_queue(base, pkt, i);
}

static bool vfx1_bus_rx_pending(const int base)
{
	const auto& b = vfx1_bus[base];
	return b.rx_pos < b.rx.size();
}

static uint8_t vfx1_bus_rx_next(const int base)
{
	auto& b = vfx1_bus[base];
	const uint8_t v = b.rx[b.rx_pos++];
	if (b.rx_pos >= b.rx.size() && b.armed) {
		// Drained mid-transfer: completion for the firmware RX exit,
		// which requires AB2PCREQ set to finish (VFX1.COM @0x28DC).
		vfx1_bus_req[base] = true;
	}
	return v;
}

// Live HMD tracking stream (VFX1.md section 6: 12-byte reports
// 50 <src> 08 cosH cosL sinH sinL pitH pitL rolH rolL XOR).
// Raw values are UNSIGNED with a mid-scale DC bias (the documented
// extraction is value=(high*256)+low then DC-correct/normalize), so
// everything is centered on 0x8000: signed zeros would read as pegged
// extremes ("BEYOND TRACKING RANGE"). Yaw sweeps a slow unit circle
// around bias (reads as a live, gently-turning head); pitch/roll hover
// near center. Src 02 = HMD (full sweep), 04 = CyberPuck (yaw unused and
// centered per docs, pitch/roll sweeping gently, buttons released).
static uint32_t vfx1_trk_n[4] = {};

static uint16_t vfx1_trk_bias(int32_t v)
{
	if (v < 0) {
		v = 0;
	}
	if (v > 65535) {
		v = 65535;
	}
	return static_cast<uint16_t>(v);
}

// Builds one 12-byte tracking report (wire format, checksummed) for
// the HMD (dev 0) or CyberPuck (dev 1) from the numpad range-sweep
// holds only. No real-hardware motion feeds VFX-1 (no headset pose, no
// controller stick/buttons): KP7 hold sweeps the HMD yaw across the full
// circle, KP8 sweeps the HMD pitch (±1024), KP9 sweeps the HMD roll
// (±1024); KP2/KP3 hold sweeps the puck X/Y axes (±1024); KP4/5/6 hold
// the puck Top/Middle/Bottom buttons. Idle (nothing held) is a
// rock-static centered packet. Shared by the bus queue and the TSR
// report-slot refresh below.
static void vfx1_build_tracking(const int base, const int dev,
                                uint8_t pkt[12])
{
	const double ph =
	        static_cast<double>((vfx1_trk_n[base] + (dev != 0 ? 64u : 0u)) %
	                            256u) *
	        3.141592653589793 / 128.0;
	const double ph2 = ph * 0.5;
	++vfx1_trk_n[base];
	pkt[0] = 0x50;
	pkt[1] = dev != 0 ? 0x04 : 0x02;
	pkt[2] = 0x08;
#if C_OPENXR
	const bool sweep_hmd_yaw   = sdl_xr_test_hmd[0] != 0.0f; // KP7
	const bool sweep_hmd_pitch = sdl_xr_test_hmd[1] != 0.0f; // KP8
	const bool sweep_hmd_roll  = sdl_xr_test_hmd[2] != 0.0f; // KP9
	const bool sweep_puck_x    = sdl_xr_test_puck[0] != 0.0f; // KP2
	const bool sweep_puck_y    = sdl_xr_test_puck[1] != 0.0f; // KP3
	const unsigned int puck_btn = sdl_xr_test_puckbtn & 0x07u; // KP4/5/6
#else
	constexpr bool sweep_hmd_yaw   = false;
	constexpr bool sweep_hmd_pitch = false;
	constexpr bool sweep_hmd_roll  = false;
	constexpr bool sweep_puck      = false;
	constexpr unsigned int puck_btn = 0u;
#endif
	if (dev == 0) {
		// Raw values are UNSIGNED DC-biased around 0x8000 (signed
		// zeros read as pegged extremes, "BEYOND TRACKING RANGE"), so
		// unheld axes stay centered.
		const uint16_t yu = sweep_hmd_yaw
		        ? vfx1_trk_bias(32768 +
		                        static_cast<int32_t>(8192 * cos(ph)))
		        : 0x8000u;
		const uint16_t su = sweep_hmd_yaw
		        ? vfx1_trk_bias(32768 +
		                        static_cast<int32_t>(8192 * sin(ph)))
		        : 0x8000u;
		const uint16_t pu = sweep_hmd_pitch
		        ? vfx1_trk_bias(32768 +
		                        static_cast<int32_t>(1024 * sin(ph2)))
		        : 0x8000u;
		const uint16_t ru = sweep_hmd_roll
		        ? vfx1_trk_bias(32768 +
		                        static_cast<int32_t>(1024 * cos(ph2)))
		        : 0x8000u;
		pkt[3] = static_cast<uint8_t>((yu >> 8) & 0xFFu);
		// Byte[4] bit 1 = dispatcher validity gate (VFX1.md 5f): the
		// filed packet is dropped without it.
		pkt[4] = static_cast<uint8_t>((yu & 0xFFu) | 0x02u);
		pkt[5] = static_cast<uint8_t>((su >> 8) & 0xFFu);
		pkt[6] = static_cast<uint8_t>(su & 0xFFu);
		pkt[7] = static_cast<uint8_t>((pu >> 8) & 0xFFu);
		pkt[8] = static_cast<uint8_t>(pu & 0xFFu);
		pkt[9] = static_cast<uint8_t>((ru >> 8) & 0xFFu);
		pkt[10] = static_cast<uint8_t>(ru & 0xFFu);
	} else {
		// Puck yaw is unused (centered); KP2 sweeps the X axis and KP3
		// sweeps the Y axis (±1024 each), unheld axes stay centered.
		// Puck bytes 9-10 are buttons/pad, not a third axis (HPD
		// layout): sweeping axis data there reads as phantom button
		// presses.
		const uint16_t xu = sweep_puck_x
		        ? vfx1_trk_bias(32768 +
		                        static_cast<int32_t>(1024 * sin(ph2)))
		        : 0x8000u;
		const uint16_t yu = sweep_puck_y
		        ? vfx1_trk_bias(32768 +
		                        static_cast<int32_t>(1024 * cos(ph2)))
		        : 0x8000u;
		pkt[3] = 0x80;
		// Byte[4] bit 1 = dispatcher validity gate (VFX1.md 5f);
		// centered yaw reads 0x8002 either way.
		pkt[4] = 0x02;
		pkt[5] = static_cast<uint8_t>((xu >> 8) & 0xFFu);
		pkt[6] = static_cast<uint8_t>(xu & 0xFFu);
		pkt[7] = static_cast<uint8_t>((yu >> 8) & 0xFFu);
		pkt[8] = static_cast<uint8_t>(yu & 0xFFu);
		pkt[9] = static_cast<uint8_t>(puck_btn);
		pkt[10] = 0x00;
	}
	pkt[11] = vfx1_bus_xor(pkt, 11);
}

static void vfx1_bus_tracking(const int base, const int dev)
{
	uint8_t pkt[12];
	vfx1_build_tracking(base, dev, pkt);
	vfx1_bus_queue(base, pkt, sizeof(pkt));
	++vfx1_trk_n[base];
}

// Live TSR report refresh (P3). The resident driver never re-reads
// the bus after boot, so INT33 REPORT consumers (games, VRMOUSE,
// VFX1DIAG live screens) get frozen boot-captured data. The demuxer
// files every tracking packet into per-device slots (device slot N at
// data:[0xAEC + 0x42*N], freshness flags at [0xAE4+N]/[0x838+N];
// HMD = slot 2, puck = slot 4) — the same wire bytes the bus would
// have delivered. This refreshes those slots (plus flags) on a 100ms
// timer with freshly built live packets, which is observationally
// identical to the TSR having polled the bus itself. Gated on a
// signature-checked lock so it can never write unrelated RAM:
// the INT33 vector must point at the TSR dispatcher prologue
// (pushf + cmp cs:[0x107],3), the data seg must come from its
// cs:[0x103] cell and differ from code/BIOS, and the slots must read
// as zeros (pre-traffic) or valid checksummed tracking.
static bool     vfx1_tsr_locked   = false;
static uint16_t vfx1_tsr_code_seg = 0;
static uint16_t vfx1_tsr_data_seg = 0;
static bool     vfx1_tsr_timer_on = false;
// SETVFX1 flag-poll tracking (feeds the mirror in vfx1_tsr_tick).
static uint16_t vfx1_setvfx1_ds  = 0;
static uint32_t vfx1_flagpoll_ms = 0;

static uint32_t vfx1_tsr_linear(const uint16_t seg, const uint16_t off)
{
	return static_cast<uint32_t>(seg) * 16u + off;
}

static bool vfx1_tsr_slot_bytes(const uint16_t data_seg,
                                const uint16_t slot, uint8_t b[12])
{
	uint32_t p = vfx1_tsr_linear(data_seg,
	                             static_cast<uint16_t>(0xAECu +
	                                                   0x42u * slot));
	for (int i = 0; i < 12; ++i) {
		b[i] = mem_readb(p + static_cast<uint32_t>(i));
	}
	return true;
}

static bool vfx1_tsr_slot_zero(const uint16_t data_seg,
                               const uint16_t slot)
{
	uint8_t b[12];
	vfx1_tsr_slot_bytes(data_seg, slot, b);
	for (int i = 0; i < 12; ++i) {
		if (b[i] != 0u) {
			return false;
		}
	}
	return true;
}

static bool vfx1_tsr_slot_live(const uint16_t data_seg,
                               const uint16_t slot, const uint8_t src)
{
	uint8_t b[12];
	vfx1_tsr_slot_bytes(data_seg, slot, b);
	if (b[0] != 0x50u || b[1] != src || b[2] != 0x08u) {
		return false;
	}
	return vfx1_bus_xor(b, 11) == b[11];
}

static bool vfx1_tsr_slot_ok(const uint16_t data_seg, const uint16_t slot,
                             const uint8_t src)
{
	return vfx1_tsr_slot_zero(data_seg, slot) ||
	       vfx1_tsr_slot_live(data_seg, slot, src);
}

static void vfx1_tsr_probe()
{
	if (vfx1_tsr_locked) {
		return;
	}
	// Transient code seg from the VIP ISR vector (stable per setup);
	// its [0x103] cell holds the DOS-allocated data seg (written by
	// init, valid while the transient area is intact, i.e. during
	// boot while traffic runs).
	const uint16_t tseg = mem_readw(0x36u);
	// INT33 vector -> TSR resident code seg (abort on BIOS/unhooked).
	const uint16_t code = mem_readw(0xCEu);
	static uint16_t probe_last_code = 0;
	static int      probe_n         = 0;
	if (code != probe_last_code) {
		probe_last_code = code;
		probe_n         = 0; // verbose burst after each handoff
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: TSR probe int33 vector seg=%04x (tseg %04x)",
			        code,
			        tseg);
		}
	}
	if (tseg == 0u || tseg >= 0xF000u || code == 0u ||
	    code >= 0xF000u) {
		++probe_n;
		if ((probe_n < 8 || probe_n % 600 == 1) &&
		    vfx1_logging_enabled()) {
			LOG_MSG("VFX1: TSR probe waiting (ivt0d=%04x ivt33=%04x)",
			        tseg,
			        code);
		}
		return;
	}
	// Dispatcher prologue signature, pattern-scanned over the
	// resident code seg (its resident offset is layout-dependent).
	static constexpr uint8_t sig[6] = {0x9Cu, 0x2Eu, 0x83u,
	                                   0x3Eu, 0x07u, 0x01u};
	const uint32_t cbase = vfx1_tsr_linear(code, 0u);
	bool sig_found       = false;
	uint32_t sig_off     = 0;
	for (uint32_t o = 0; o + 6 <= 0x10000u; o += 1u) {
		bool hit = true;
		for (int i = 0; i < 6; ++i) {
			if (mem_readb(cbase + o +
			              static_cast<uint32_t>(i)) != sig[i]) {
				hit = false;
				break;
			}
		}
		if (hit) {
			sig_found = true;
			sig_off   = o;
			break;
		}
	}
	if (!sig_found) {
		++probe_n;
		if ((probe_n < 8 || probe_n % 600 == 1) &&
		    vfx1_logging_enabled()) {
			LOG_MSG("VFX1: TSR probe no dispatcher in %04x",
			        code);
		}
		return;
	}
	// Data seg candidates: the transient init cell (01e7:0x103,
	// written by init, valid until transient reuse) preferred; the
	// resident cell (code:0x103) as fallback (tiny-model shares it
	// with code). Slot CONTENT decides: all-zero pre-traffic BSS
	// (packets + freshness flags) or valid checksummed tracking.
	const uint16_t trans =
	        mem_readw(vfx1_tsr_linear(0x01E7u, 0x103u));
	const uint16_t res =
	        mem_readw(vfx1_tsr_linear(code, 0x103u));
	const uint16_t cands[2] = {trans, res};
	const char* const cand_names[2] = {"trans", "res"};
	bool locked_now = false;
	for (int c = 0; c < 2 && !locked_now; ++c) {
		const uint16_t data = cands[c];
		if (data < 0x1000u || data >= 0xF000u) {
			continue;
		}
		if (!vfx1_tsr_slot_ok(data, 2, 0x02u) ||
		    !vfx1_tsr_slot_ok(data, 4, 0x04u)) {
			continue;
		}
		// Freshness flags must be zero pre-traffic too (same
		// zeroed BSS range as the slots).
		static constexpr uint16_t flag_offs[4] = {0xAE6u,
		                                          0xAE8u,
		                                          0x83Au,
		                                          0x83Cu};
		bool flags_zero = true;
		for (int f = 0; f < 4; ++f) {
			if (mem_readb(vfx1_tsr_linear(data,
			                              flag_offs[f])) != 0u) {
				flags_zero = false;
				break;
			}
		}
		const bool slots_zero =
		        vfx1_tsr_slot_zero(data, 2) &&
		        vfx1_tsr_slot_zero(data, 4);
		const bool packets_live =
		        vfx1_tsr_slot_live(data, 2, 0x02u) &&
		        vfx1_tsr_slot_live(data, 4, 0x04u);
		if (!((slots_zero && flags_zero) || packets_live)) {
			continue;
		}
		vfx1_tsr_code_seg = code;
		vfx1_tsr_data_seg = data;
		vfx1_tsr_locked   = true;
		locked_now        = true;
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: TSR report slots locked (%s=%04x data=%04x dispatch=%04x)",
			        cand_names[c],
			        cands[c],
			        data,
			        sig_off);
			// DIAG one-shot: other valid tracking packets in the
			// data seg (REPORT copy-buffers?) besides the slots.
			const uint32_t dbase = vfx1_tsr_linear(data, 0u);
			int found            = 0;
			for (uint32_t o = 0;
			     o + 12 <= 0x10000u && found < 8;
			     ++o) {
				if (o == 0xB70u || o == 0xBFCu) {
					continue;
				}
				uint8_t q[12];
				for (int i = 0; i < 12; ++i) {
					q[i] = mem_readb(
					        dbase +
					        static_cast<uint32_t>(i));
				}
				if ((q[0] != 0x50u) ||
				    (q[1] != 0x02u && q[1] != 0x04u) ||
				    q[2] != 0x08u ||
				    vfx1_bus_xor(q, 11) != q[11]) {
					continue;
				}
				LOG_MSG("VFX1: TSR tracking copy src=%02x at %04x:%04x",
				        q[1],
				        data,
				        o);
				++found;
			}
		}
	}
	if (!locked_now) {
		++probe_n;
		if ((probe_n < 8 || probe_n % 600 == 1) &&
		    vfx1_logging_enabled()) {
			LOG_MSG("VFX1: TSR probe slots not ready (trans %04x res %04x)",
			        trans,
			        res);
		}
		return;
	}
}

static void vfx1_tsr_refresh()
{
	if (!vfx1_tsr_locked) {
		return;
	}
	const uint16_t data = vfx1_tsr_data_seg;
	uint8_t pkt[12];
	vfx1_build_tracking(0, 0, pkt);
	uint32_t hmd = vfx1_tsr_linear(data, 0xB70u); // 0xAEC+0x42*2
	for (int i = 0; i < 12; ++i) {
		mem_writeb(hmd + static_cast<uint32_t>(i), pkt[i]);
	}
	vfx1_build_tracking(0, 1, pkt);
	uint32_t puck = vfx1_tsr_linear(data, 0xBFCu); // 0xAEC+0x42*4
	for (int i = 0; i < 12; ++i) {
		mem_writeb(puck + static_cast<uint32_t>(i), pkt[i]);
	}
	// Freshness flags, as the demuxer sets them when filing.
	mem_writeb(vfx1_tsr_linear(data, 0xAE6u), 1u); // 0xAE4+2
	mem_writeb(vfx1_tsr_linear(data, 0xAE8u), 1u); // 0xAE4+4
	mem_writeb(vfx1_tsr_linear(data, 0x83Au), 1u); // 0x838+2
	mem_writeb(vfx1_tsr_linear(data, 0x83Cu), 1u); // 0x838+4
}

static void vfx1_tsr_tick(uint32_t)
{
	vfx1_tsr_probe();
	vfx1_tsr_refresh();
	// NOTE: no forced interrupt pulse here. FIFO level never raises
	// (command edges only), and the TSRZeros enumerator advances
	// polled; periodic pulses over-counted the exact-1 IRQ test.
	// DIAG: SETVFX1 data seg via its relocated `mov ax,imm16`
	// (070c:0AE4) + its per-device [0xA2E+idx] flags, on change.
	// Also feeds the flag mirror below.
	{
		static uint16_t last_ds = 0;
		static uint8_t last_fl[8] = {};
		static bool first = true;
		const uint16_t ds =
		        mem_readw(vfx1_tsr_linear(0x070Cu, 0x0AE4u));
		vfx1_setvfx1_ds = ds;
		bool changed = first || ds != last_ds;
		uint8_t fl[8] = {};
		if (ds >= 0x1000u && ds < 0xF000u) {
			for (int i = 0; i < 8; ++i) {
				fl[i] = mem_readb(vfx1_tsr_linear(
				        ds,
				        static_cast<uint16_t>(0xA2Eu +
				                              i)));
				if (!changed && fl[i] != last_fl[i]) {
					changed = true;
				}
			}
		}
		if (changed && vfx1_logging_enabled()) {
			LOG_MSG("VFX1: SETVFX1 ds=%04x A2E=[%02x %02x %02x %02x %02x %02x %02x %02x]",
			        ds,
			        fl[0],
			        fl[1],
			        fl[2],
			        fl[3],
			        fl[4],
			        fl[5],
			        fl[6],
			        fl[7]);
			last_ds = ds;
			for (int i = 0; i < 8; ++i) {
				last_fl[i] = fl[i];
			}
			first = false;
		}
	}
	// Flag mirror for the SETVFX1 sequencer: it polls [0xA2E+idx]
	// (070c:0145 site below) for a dispatcher-set found-flag that
	// never comes (its dispatcher inputs were starved by races it
	// cannot win). While its flag-poll is active, mirror found for
	// HMD (2) + puck (4) plus the companion [0xA22] bits the
	// dispatcher sets with them. The devices ARE present and
	// streaming (validated bus content), so this supplements truth
	// it cannot observe, like the TSR slot refresh above.
	if (vfx1_setvfx1_ds >= 0x1000u && vfx1_setvfx1_ds < 0xF000u &&
	    PIC_Ticks - vfx1_flagpoll_ms < 5000u) {
		const uint32_t fbase =
		        vfx1_tsr_linear(vfx1_setvfx1_ds, 0xA2Eu);
		mem_writeb(fbase + 2u, 1u);
		mem_writeb(fbase + 4u, 1u);
		const uint32_t gbase =
		        vfx1_tsr_linear(vfx1_setvfx1_ds, 0xA22u);
		mem_writeb(gbase + 2u, 1u);
		mem_writeb(gbase + 4u, 1u);
	}
	// NOTE: no forced interrupt pulse here. FIFO level never raises
	// (command edges only), and the TSRZeros enumerator advances
	// polled; periodic pulses over-counted the exact-1 IRQ test.
	PIC_AddEvent(vfx1_tsr_tick, 100.0, 0);
}

static void vfx1_tsr_start()
{
	if (vfx1_tsr_timer_on) {
		return;
	}
	vfx1_tsr_timer_on = true;
	PIC_AddEvent(vfx1_tsr_tick, 100.0, 0);
}

// Keep a few tracking packets available; compact consumed prefix so the
// stream can run forever. Unarmed traffic never sees these bytes (serve
// and EMPTY bits stay legacy-gated), so idle checks and handshakes are
// unaffected.
static void vfx1_bus_maintain(const int base)
{
	auto& b = vfx1_bus[base];
	vfx1_tsr_start(); // 100ms report-slot refresh (locks on TSR)
	// Pre-seed one HMD + one puck ID reply per stage-0 episode (never
	// refilled mid-transfer): the TSR's demuxer files whatever its
	// early reads catch, but its final broadcast IDREQ routinely
	// arrives after its last RX call, so those replies would rot
	// unread. Seeded replies are already filed when the orchestrator
	// checks, like hardware where replies precede the check.
	if (b.stage == 0 && !b.armed && !b.id_seeded) {
		vfx1_bus_idreply(base, b.hmd);
		vfx1_bus_idreply(base, b.puck);
		b.id_seeded = true;
	}
	// ID keep-alive while pre-assignment: the framed reader eats
	// one-shot replies instantly (back-to-back 132B transfers), so a
	// slow polling validator never sees one. Keep one HMD + one puck
	// ID reply in flight (refilled when fully consumed); the flush
	// still terminates via the quench, and re-filing is idempotent
	// for the demuxers.
	if (b.stage == 0 && !b.armed) {
		auto has_id = [&](const VipBusDev& d) {
			for (size_t i = b.rx_pos; i + 33 <= b.rx.size();
			     ++i) {
				if (b.rx[i] == 0x50 && b.rx[i + 1] == 0x6E &&
				    b.rx[i + 2] == 0x9D &&
				    b.rx[i + 3] == 0xE1 &&
				    memcmp(b.rx.data() + i + 20,
				           d.name, 8) == 0) {
					return true;
				}
			}
			return false;
		};
		if (!has_id(b.hmd)) {
			vfx1_bus_idreply(base, b.hmd);
		}
		if (!has_id(b.puck)) {
			vfx1_bus_idreply(base, b.puck);
		}
	}
	if (b.stage == 0 && !b.armed && b.att_seeded < 8) {
		// Pre-IDREQ: the demuxer must also see ATTENTION (it sets the
		// flag the orchestrator waits on before sending IDREQ). Keep
		// one copy in flight; the firmware consumes and re-triggers.
		// Budgeted (mid-transfer refills would defeat drain
		// completion: the RX exit needs the queue to actually drain),
		// and never refilled mid-transfer, so a quiet bus still
		// terminates every RX call.
		static constexpr uint8_t at[] = {0x50, 0x6E, 0x82,
		                                 0xE0, 0x00, 0x5C};
		bool found = false;
		for (size_t i = b.rx_pos; !found && i + sizeof(at) <= b.rx.size();
		     ++i) {
			found = memcmp(b.rx.data() + i, at, sizeof(at)) == 0;
		}
		if (!found) {
			vfx1_bus_queue(base, at, sizeof(at));
			vfx1_bus_queue(base, at, sizeof(at));
			b.att_seeded += 2;
		}
	}
	// Tracking top-up: instant (a full backlog, like the always-
	// streaming real DSP as the host sees it), gated on host
	// reporting and never mid-transfer (each RX call still drains to
	// its completion). The TSR's polled enumerator needs full reads
	// to advance (short/dry reads make it skip phases); the SETVFX1
	// flush loops are count-bounded downstream so they terminate
	// regardless.
	while (!b.armed && b.reporting && b.rx.size() - b.rx_pos < 256) {
		if (b.rx.size() > 4096) {
			break; // firmware stopped reading; don't grow
		}
		vfx1_bus_tracking(base, 0);
		vfx1_bus_tracking(base, 1);
	}
	if (b.rx_pos > 512) {
		b.rx.erase(b.rx.begin(), b.rx.begin() + b.rx_pos);
		b.rx_pos = 0;
	}
}

// New transfer takes the bus: entry-shape +0x8 write (bit0 set, bits 3+6
// clear, as the receiver head emits) arms serving; a bit0-clear write
// (TX takes the bus) disarms. A 0x40-bearing write is the 0x48-style ack
// and consumes a pending completion.
static void vfx1_bus_control(const int base, const uint8_t value)
{
	auto& b = vfx1_bus[base];
	if ((value & 0x49u) == 0x01u) {
		b.armed = true;
		// New transfer takes the bus: drop any stale completion from
		// a previous one (the VIP deasserts on takeover). The ack path
		// below clears it after a genuine completion.
		vfx1_bus_req[base] = false;
		// Quench cadence: every 8th armed transfer sees a dry bus
		// (serves nothing, consumes nothing) so drain-until-dry
		// loops (SETVFX1's flush, which retries RX while nonzero)
		// terminate within ~8 transfers instead of spinning forever
		// on an always-full backlog. All other transfers (and all
		// unarmed traffic) are unaffected; the backlog is preserved
		// for the next transfer. Speed-invariant (counted, not timed).
		++b.xfer;
		b.quenched = (b.xfer % 8u == 7u);
	} else if ((value & 0x01u) == 0u) {
		b.armed = false;
		b.quenched = false;
	}
	if ((value & 0x40u) != 0u) {
		vfx1_bus_req[base] = false;
	}
	if ((value & 0x49u) == 0x49u) {
		// RX exit-shape (|0x48 onto the armed cl): the transfer is
		// over, release the bus so the next maintain refills for the
		// next transfer instead of starving it quiet.
		b.armed = false;
		b.quenched = false;
	}
}

static bool vfx1_bus_in_clear_pulse(const int base)
{
	return vfx1_resp_pending[base] && vfx1_resp_grace[base] == 0 &&
	       vfx1_resp_clearreads[base] < Vfx1ClearReads;
}

static bool vfx1_bus_framed(const int base, size_t len)
{
	// Short packets must start within a few writes after a +0x8 control
	// write, as the 0x28EF sender heads every packet (long ASSIGN is
	// exempt: serials plus checksum cannot occur by chance).
	return vfx1_bus[base].since_ctrl >= static_cast<int>(len) &&
	       vfx1_bus[base].since_ctrl - static_cast<int>(len) <= 8;
}

// Detect host bus packets on the +0x9 write stream. All matches are
// checksum-verified (plus serials for ASSIGN), so DSP firmware bytes in
// the same stream cannot trip them.
static void vfx1_bus_tx(const int base, const uint8_t value)
{
	auto& b = vfx1_bus[base];
	if (value == 0xAAu) {
		// Bus reset filler: 0x200-strong runs mean bus engine (re)init.
		if (++b.aa_run == 64) {
			b.stage = 0;
			b.rx.clear();
			b.rx_pos = 0;
			b.txwin.clear();
			b.armed = false;
			b.quenched = false;
			b.reporting = false;
			b.att_seeded = 0;
			b.id_seeded = false;
			b.att_logged = false;
			vfx1_bus_req[base] = false;
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: VIP bus base%d: reset (AA run), back to ATTENTION",
				        base);
			}
		}
	} else {
		b.aa_run = 0;
	}
	b.txwin.push_back(value);
	if (b.txwin.size() > 64) {
		b.txwin.erase(b.txwin.begin(),
		              b.txwin.begin() + (b.txwin.size() - 64));
	}
	const auto& w = b.txwin;
	const size_t n = w.size();
	// ID request: 6E 50 81 F1 4E (XOR-verified, framed).
	if (n >= 5 && w[n - 5] == 0x6E && w[n - 4] == 0x50 &&
	    w[n - 3] == 0x81 && w[n - 2] == 0xF1 &&
	    (w[n - 5] ^ w[n - 4] ^ w[n - 3] ^ w[n - 2]) == w[n - 1]) {
		if (!vfx1_bus_framed(base, 5)) {
			return;
		}
		b.rx.clear();
		b.rx_pos = 0;
		b.stage = 1;
		vfx1_bus_idreply(base, b.hmd);
		vfx1_bus_idreply(base, b.puck);
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP bus base%d: host IDREQ, ID replies queued (HMD+puck)",
			        base);
			// DIAG: is the VIP ISR hooked? (INT 0x0D = IRQ5.)
			LOG_MSG("VFX1: VIP IRQ5 vector=%04x:%04x",
			        mem_readw(0x36u),
			        mem_readw(0x34u));
		}
		// The TSR's orchestrator advances on filed replies; open the
		// line and kick so a pending edge can vector while IF is set.
		PIC_SetIRQMask(static_cast<uint32_t>(vfx1_irq()), false);
		vfx1_raise_irq(base);
		return;
	}
	// Directed ID request: <addr> 50 81 F1 <chk> (XOR-verified,
	// framed). SETVFX1's TEST VFX1/PUCK probes each candidate device
	// address after its REPORTING sweep and waits for that device's ID
	// reply; broadcast-only detection starved it forever. Queue the
	// matching device's reply without disturbing the bus (no clear,
	// no stage change); unknown addresses get both, as broadcast.
	if (n >= 5 && w[n - 5] != 0x6Eu && w[n - 4] == 0x50 &&
	    w[n - 3] == 0x81 && w[n - 2] == 0xF1 &&
	    (w[n - 5] ^ w[n - 4] ^ w[n - 3] ^ w[n - 2]) == w[n - 1]) {
		if (!vfx1_bus_framed(base, 5)) {
			return;
		}
		if (w[n - 5] == 0x02u) {
			vfx1_bus_idreply(base, b.hmd);
		} else if (w[n - 5] == 0x04u) {
			vfx1_bus_idreply(base, b.puck);
		} else {
			vfx1_bus_idreply(base, b.hmd);
			vfx1_bus_idreply(base, b.puck);
		}
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP bus base%d: host directed IDREQ addr %u, ID reply queued",
			        base,
			        w[n - 5]);
			LOG_MSG("VFX1: VIP IRQ5 vector=%04x:%04x",
			        mem_readw(0x36u),
			        mem_readw(0x34u));
		}
		PIC_SetIRQMask(static_cast<uint32_t>(vfx1_irq()), false);
		vfx1_raise_irq(base);
		return;
	}
	// Assign address: 6E 50 9E F2 42 fw(7) FORTE___ name(8) ser(4) addr
	// chk (34 bytes; a 33-byte variant is also tried). Serials, strings
	// and XOR checksum must all verify.
	for (int layout = 0; layout < 2; ++layout) {
		const size_t total = layout == 0 ? 34u : 33u;
		if (n < total) {
			continue;
		}
		const size_t at = n - total;
		const size_t ab = at + total - 2; // addr offset
		if (w[at] != 0x6E || w[at + 1] != 0x50 ||
		    w[at + 2] != 0x9E || w[at + 3] != 0xF2 ||
		    w[at + 4] != 0x42 ||
		    vfx1_bus_xor(w.data() + at, total - 1) !=
		            w[at + total - 1]) {
			continue;
		}
		for (VipBusDev* d : {&b.hmd, &b.puck}) {
			if (memcmp(w.data() + at + 5, d->fwver, 7) == 0 &&
			    memcmp(w.data() + at + 12, "FORTE___", 8) == 0 &&
			    memcmp(w.data() + at + 20, d->name, 8) == 0 &&
			    memcmp(w.data() + at + 28, d->serial, 4) == 0) {
				d->assigned = w[ab];
				if (b.hmd.assigned != 0u &&
				    b.puck.assigned != 0u) {
					b.stage = 2;
				}
				if (vfx1_logging_enabled()) {
					LOG_MSG("VFX1: VIP bus base%d: assign %s -> addr %u (stage %d)",
					        base,
					        d->name,
					        d->assigned,
					        b.stage);
				}
				return;
			}
		}
	}
	// Power (F6) / reporting (F5) / presence poll (F7):
	// <addr> 50 8x F6/F5/F7 .. chk. F7 (presence) and F6 (power)
	// get a status reply (the 0x1F62 probe and the power phase wait
	// on it); F5 needs none (streaming is the response).
	if (n >= 6 && vfx1_bus_framed(base, 6) && w[n - 5] == 0x50 &&
	    (w[n - 4] == 0x82 || w[n - 4] == 0x81) &&
	    (w[n - 3] == 0xF6 || w[n - 3] == 0xF5 || w[n - 3] == 0xF7) &&
	    vfx1_bus_xor(w.data() + n - 6, 5) == w[n - 1]) {
		if (w[n - 3] == 0xF5) {
			// Host enabled tracking reports: the DSP may
			// stream from here on (quiet until then).
			b.reporting = true;
		} else {
			// 50 51 82 C2 04 04 chk: [1]==0x51 files as
			// device 1 ([0xAE5] presence for the probe);
			// [3]==0xC2 selects the status branch; [4]&4
			// and [5]&4 mark it valid ([0x1AD]=1). Minimal
			// bits otherwise. POWER is HMD-only (the puck
			// has no power management); POLL answers for
			// any address.
			const uint8_t addr = w[n - 6];
			const bool is_puck_addr =
			        (b.puck.assigned != 0u)
			                ? (addr == b.puck.assigned)
			                : (addr == 0x04u);
			if (w[n - 3] == 0xF7 || !is_puck_addr) {
				static constexpr uint8_t rp[] = {
				        0x50, 0x51, 0x82,
				        0xC2, 0x04, 0x04};
				uint8_t pkt[7];
				memcpy(pkt, rp, sizeof(rp));
				pkt[6] = vfx1_bus_xor(pkt, 6);
				vfx1_bus_queue(base, pkt, sizeof(pkt));
			}
		}
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP bus base%d: host %s for addr %u",
			        base,
			        w[n - 3] == 0xF6   ? "POWER"
			        : w[n - 3] == 0xF5 ? "REPORTING"
			                           : "POLL",
			        w[n - 6]);
		}
	}
}

void VFX1_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	using enum Property::Changeable::Value;

	LOG_MSG("VFX1: responder build %s",
	        VFX1_BUILD_ID);

	auto section = conf->AddSection("vfx1");

	auto pbool = section->AddBool("vfx1_enable", Always, true);
	pbool->SetHelp(
	        "Master switch for all VFX-1 VIP-card emulation ('on' by\n"
	        "default). When off, nothing answers regardless of the other\n"
	        "keys below (safe for non-VFX-1 sessions sharing this config).\n"
	        "When on, 'vfx1_respond' still gates the actual answering.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	pbool = section->AddBool("vfx1_respond", Always, false);
	pbool->SetHelp(
	        "Answer VFX-1 VIP-card detection ('off' by default): reads from\n"
	        "an unclaimed VIP base port return the last value written there\n"
	        "(echo latch), and the ID registers answer HWID 0Dh with\n"
	        "non-FF hardware versions, so card-detection routines like\n"
	        "SETVFX1's baseport test can succeed. Claimed ports keep their\n"
	        "real handlers; nothing else is emulated. Requires\n"
	        "'vfx1_enable'.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto pint = section->AddInt("vfx1_irq", Always, 7);
	pint->SetHelp(
	        "IRQ line the emulated VFX-1 VIP card raises on ('7' by\n"
	        "default, matching the usual VIPPORT jumper). Must match the\n"
	        "IRQ in the guest VIPPORT setting. The card raises it when\n"
	        "command traffic completes; the guest EOIs lower it again.\n"
	        "Only used while 'vfx1_respond' is on. Valid range from 0\n"
	        "to 15 (card accepts 5, 7, 10, 11, 12, 15).\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pint->SetMinMax(0, 15);

	pbool = section->AddBool("vfx1_log", Always, true);
	pbool->SetHelp(
	        "Log VFX-1 VIP-card detection failures ('on' by default):\n"
	        "responder-off touches, echo/HWID/CFGSTATUS rejects, IRQCFG\n"
	        "readback divergence, status-waiter spins, FIFO-dry reads,\n"
	        "unserviced IRQs and VIP-IRQ masking on ports 260h, 280h,\n"
	        "2A0h and 2C0h. Healthy traffic stays silent.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	pbool = section->AddBool("vfx1_log_all", Always, false);
	pbool->SetHelp(
	        "Log every VFX-1 VIP port access with CS:IP and no dedup\n"
	        "('off' by default). Identical repeats still flip emulated\n"
	        "device state, so use this when the failure-point log\n"
	        "contradicts itself. Very verbose: read dosbox-xr.log, not\n"
	        "the window.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	pbool = section->AddBool("vfx1_driver", Always, false);
	pbool->SetHelp(
	        "Internal INT 33h VFX-1 driver ('off' by default): answers\n"
	        "the SENSE + tracker API (GET_DEVICE_DATA and ZERO) from live\n"
	        "head/puck state, so games like EF2000 track without loading\n"
	        "VFX1.COM. Do NOT load VFX1.COM together with this: a resident\n"
	        "TSR hooks over it and serves frozen boot-captured data.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
}

// One-shot notice when the pre-[vfx1] keys are still set under [xr]:
// they no longer do anything.
static void vfx1_note_moved_keys()
{
	static bool noted = false;
	if (noted) {
		return;
	}
	const auto* xr = get_section("xr");
	if (!xr) {
		return;
	}
	const bool stale = xr->GetBool("xr_vip_respond") ||
	                   xr->GetInt("xr_vip_irq") != 7 ||
	                   !xr->GetBool("xr_log_vip") ||
	                   xr->GetBool("xr_log_vip_full");
	if (stale) {
		noted = true;
		LOG_MSG("VFX1: xr_vip_respond/xr_vip_irq/xr_log_vip/xr_log_vip_full under [xr] are retired; use [vfx1] vfx1_enable/vfx1_respond/vfx1_irq/vfx1_log/vfx1_log_all");
	}
}

static bool vfx1_respond_enabled()
{
	// Fail-closed: never answer hardware unasked. The master switch
	// gates the responder key.
	const auto* section = get_section("vfx1");
	if (!section) {
		return false;
	}
	vfx1_note_moved_keys();
	if (!section->GetBool("vfx1_enable")) {
		return false;
	}
	return section->GetBool("vfx1_respond");
}

// VIP interrupt request line (VIPPORT IRQ, 7 by default). Raised when
// the emulated engine finishes command traffic (see latch_write):
// the driver waits on completion IRQs with ~40 ms timeouts, so a
// silent engine fails every init stage. Lowered only on EOI: status
// reads must not cancel a pending IRQ before the wait starts.
// Valid IRQs per SDK: 5, 7, 10, 11, 12, 15 (from SET VIPPORT=2x0 Iy)
static int vfx1_irq()
{
	const auto* section = get_section("vfx1");
	int irq             = 7;
	if (section) {
		irq = section->GetInt("vfx1_irq");
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

static bool vfx1_driver_enabled()
{
	const auto* section = get_section("vfx1");
	return section && section->GetBool("vfx1_driver");
}

// SET_CONFIGURATION reporting state (per-device data enable).
static bool vfx1_drv_den_hmd  = true;
static bool vfx1_drv_den_puck = true;

static void vfx1_drv_put(uint32_t phys, const uint8_t* p, size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		mem_writeb(phys + static_cast<uint32_t>(i), p[i]);
	}
}

static void vfx1_drv_put16(uint32_t phys, int v)
{
	mem_writeb(phys, static_cast<uint8_t>(v & 0xFF));
	mem_writeb(phys + 1u,
	           static_cast<uint8_t>((v >> 8) & 0xFF));
}

// GET_CONFIGURATION chain (225 bytes total: 15+98+44+32+34+2; count 6).
// Layouts per DOSSDK VFX1.H (cfpack structs); reporting optics are
// documented guesses (see code).
static int vfx1_drv_config(const uint32_t base)
{
	uint32_t p   = base;
	uint8_t  pkt[100];
	size_t   i = 0;
	// SYSTEM_CPKT (15).
	i = 0;
	pkt[i++] = 15;
	pkt[i++] = 1; // SYSI
	pkt[i++] = 0;
	pkt[i++] = 0; // dcls/dnum
	pkt[i++] = 0; // sstat
	pkt[i++] = 1;
	pkt[i++] = 2; // vmin/vmaj
	pkt[i++] = 'V';
	pkt[i++] = 'F';
	pkt[i++] = 'X';
	pkt[i++] = '1'; // vipid
	pkt[i++] = 0x00;
	pkt[i++] = 0x01; // dfsz = 256
	pkt[i++] = 0x00;
	pkt[i++] = 0x00; // dummy
	vfx1_drv_put(p, pkt, i);
	p += static_cast<uint32_t>(i);
	// VENDOR_CPKT (98).
	i = 0;
	pkt[i++] = 98;
	pkt[i++] = 2; // VNDI
	const char* const prdm = "Forte Technologies, Inc.";
	const char* const prdn = "VFX1 CyberPuck";
	size_t k = 0;
	for (; k < 32 && prdm[k] != 0; ++k) {
		pkt[i++] = static_cast<uint8_t>(prdm[k]);
	}
	while (k++ < 32) {
		pkt[i++] = 0;
	}
	k = 0;
	for (; k < 32 && prdn[k] != 0; ++k) {
		pkt[i++] = static_cast<uint8_t>(prdn[k]);
	}
	while (k++ < 32) {
		pkt[i++] = 0;
	}
	static constexpr uint8_t hmd_ser[4] = {0xA1, 0xB2, 0xC3, 0xD4};
	for (k = 0; k < 4; ++k) {
		pkt[i++] = hmd_ser[k];
	}
	for (; k < 16; ++k) {
		pkt[i++] = 0;
	}
	static constexpr uint8_t puck_ser[4] = {0x12, 0x34, 0x56, 0x78};
	for (k = 0; k < 4; ++k) {
		pkt[i++] = puck_ser[k];
	}
	for (; k < 16; ++k) {
		pkt[i++] = 0;
	}
	vfx1_drv_put(p, pkt, i);
	p += static_cast<uint32_t>(i);
	// HMS_CPKT (44).
	i = 0;
	pkt[i++] = 44;
	pkt[i++] = 4; // HMS
	pkt[i++] = 2;
	pkt[i++] = 0; // dcls/dnum
	pkt[i++] = 0x00; // hmsf (enabled)
	pkt[i++] = 0x00; // hmss
	pkt[i++] = 1;
	pkt[i++] = 2; // vmin/vmaj
	pkt[i++] = 0x38; // hmsc: color+stereo+tracker
	pkt[i++] = 1; // dtyp LCD
	pkt[i++] = 0xE8;
	pkt[i++] = 0x03; // vmag 1000 (1.0x)
	pkt[i++] = 0x68;
	pkt[i++] = 0x01; // hfov 360 (90 deg)
	pkt[i++] = 0x0E;
	pkt[i++] = 0x01; // vfov 270 (67.5 deg)
	pkt[i++] = 127; // ipd 63.5mm
	pkt[i++] = 0;
	pkt[i++] = 0;
	pkt[i++] = 0; // hoao/voao/oar
	pkt[i++] = 60; // mrr Hz
	pkt[i++] = 0; // cvm
	pkt[i++] = 0; // reserved
	pkt[i++] = 10; // vmsp
	static constexpr uint16_t vmodes[10] = {0x13,  0x101, 0x103,
	                                        0x105, 0x107, 0x111,
	                                        0x114, 0x117, 0x11A,
	                                        0x11D};
	for (k = 0; k < 10; ++k) {
		pkt[i++] = static_cast<uint8_t>(vmodes[k] & 0xFFu);
		pkt[i++] = static_cast<uint8_t>((vmodes[k] >> 8) &
		                                0xFFu);
	}
	vfx1_drv_put(p, pkt, i);
	p += static_cast<uint32_t>(i);
	// HTD_CPKT (32): yaw 0-359 wrap, pitch/roll +-90, 1 deg steps.
	i = 0;
	pkt[i++] = 32;
	pkt[i++] = 6; // HTD
	pkt[i++] = 3;
	pkt[i++] = 0; // dcls/dnum
	pkt[i++] = 0x00; // htf (enabled)
	pkt[i++] = 0x00; // hts
	pkt[i++] = 1;
	pkt[i++] = 2; // vmin/vmaj
	pkt[i++] = 0x67;
	pkt[i++] = 0x01; // ywcd.maxv = 359
	pkt[i++] = 0x00;
	pkt[i++] = 0x00; // ywcd.minv = 0
	pkt[i++] = 0x68;
	pkt[i++] = 0x01; // ywcd.steps = 360
	pkt[i++] = 0x8E; // ywcd.rcf: wrap+zpsr+rdg+den
	pkt[i++] = 0x00; // ywcd.dum
	pkt[i++] = 0x5A;
	pkt[i++] = 0x00; // ptcd.maxv = 90
	pkt[i++] = 0xA6;
	pkt[i++] = 0xFF; // ptcd.minv = -90
	pkt[i++] = 0xB4;
	pkt[i++] = 0x00; // ptcd.steps = 180
	pkt[i++] = 0x8C; // ptcd.rcf: zpsr+rdg+den
	pkt[i++] = 0x00; // ptcd.dum
	pkt[i++] = 0x5A;
	pkt[i++] = 0x00; // rlcd.maxv = 90
	pkt[i++] = 0xA6;
	pkt[i++] = 0xFF; // rlcd.minv = -90
	pkt[i++] = 0xB4;
	pkt[i++] = 0x00; // rlcd.steps = 180
	pkt[i++] = 0x8C; // rlcd.rcf
	pkt[i++] = 0x00; // rlcd.dum
	vfx1_drv_put(p, pkt, i);
	p += static_cast<uint32_t>(i);
	// HPD_CPKT (34): same axes + 3-button descriptor.
	i = 0;
	pkt[i++] = 34;
	pkt[i++] = 7; // HPD
	pkt[i++] = 4;
	pkt[i++] = 0; // dcls/dnum
	pkt[i++] = 0x00; // hhpf
	pkt[i++] = 0x00; // hhps
	pkt[i++] = 1;
	pkt[i++] = 2; // vmin/vmaj
	pkt[i++] = 0x67;
	pkt[i++] = 0x01;
	pkt[i++] = 0x00;
	pkt[i++] = 0x00;
	pkt[i++] = 0x68;
	pkt[i++] = 0x01;
	pkt[i++] = 0x8E;
	pkt[i++] = 0x00; // ywcd
	pkt[i++] = 0x5A;
	pkt[i++] = 0x00;
	pkt[i++] = 0xA6;
	pkt[i++] = 0xFF;
	pkt[i++] = 0xB4;
	pkt[i++] = 0x00;
	pkt[i++] = 0x8C;
	pkt[i++] = 0x00; // ptcd
	pkt[i++] = 0x5A;
	pkt[i++] = 0x00;
	pkt[i++] = 0xA6;
	pkt[i++] = 0xFF;
	pkt[i++] = 0xB4;
	pkt[i++] = 0x00;
	pkt[i++] = 0x8C;
	pkt[i++] = 0x00; // rlcd
	pkt[i++] = 3; // btcd.nbtn = 3
	pkt[i++] = 0x80; // btcd: den
	vfx1_drv_put(p, pkt, i);
	p += static_cast<uint32_t>(i);
	// NULLP terminator.
	uint8_t term[2] = {2, 0};
	vfx1_drv_put(p, term, sizeof(term));
	return 6;
}
// Internal INT 33h driver live state (degrees; yaw wraps 0-359).
// Zero offsets per device (SET_ZERO captures current, RESET_ZERO
// restores). Numpad range sweeps only: KP7/8/9 hold sweeps the HMD
// yaw/pitch/roll, KP2/KP3 hold sweeps the puck X/Y axes, KP4/5/6 hold
// the puck buttons. Idle (nothing held) is centered. No real-hardware
// motion.
static int16_t vfx1_drv_zero_hmd[3]  = {};
static int16_t vfx1_drv_zero_puck[3] = {};
static int     vfx1_drv_sweep        = 0;

static void vfx1_drv_angles(const int cls, int& yaw, int& pitch,
                            int& roll, unsigned int& btn)
{
	yaw = 0;
	pitch = 0;
	roll  = 0;
	btn   = 0;
	vfx1_drv_sweep = (vfx1_drv_sweep + 1) % 360;
	const double sw =
	        static_cast<double>(vfx1_drv_sweep) * 3.141592653589793 /
	        180.0;
#if C_OPENXR
	if (cls == 3) {
		if (sdl_xr_test_hmd[0] != 0.0f) { // KP7: yaw sweep 0-359
			yaw = (vfx1_drv_sweep - vfx1_drv_zero_hmd[0] % 360 +
			       360) %
			      360;
		} else {
			yaw = (360 - vfx1_drv_zero_hmd[0] % 360) % 360;
		}
		if (sdl_xr_test_hmd[1] != 0.0f) { // KP8: pitch sweep +-90
			pitch = static_cast<int>(90.0 * sin(sw)) -
			        vfx1_drv_zero_hmd[1];
		} else {
			pitch = -vfx1_drv_zero_hmd[1];
		}
		if (sdl_xr_test_hmd[2] != 0.0f) { // KP9: roll sweep +-90
			roll = static_cast<int>(90.0 * sin(sw + 1.5707963)) -
			       vfx1_drv_zero_hmd[2];
		} else {
			roll = -vfx1_drv_zero_hmd[2];
		}
	} else if (cls == 4) {
		// KP2/KP3: X/Y axes sweep +-90 each; yaw unused. KP4/5/6:
		// buttons.
		if (sdl_xr_test_puck[0] != 0.0f) {
			pitch = static_cast<int>(90.0 * sin(sw)) -
			        vfx1_drv_zero_puck[1];
		} else {
			pitch = -vfx1_drv_zero_puck[1];
		}
		if (sdl_xr_test_puck[1] != 0.0f) {
			roll = static_cast<int>(90.0 * sin(sw + 1.5707963)) -
			       vfx1_drv_zero_puck[2];
		} else {
			roll = -vfx1_drv_zero_puck[2];
		}
		btn = sdl_xr_test_puckbtn & 0x07u;
	}
#else
	if (cls == 4) {
		yaw   = 0;
		pitch = -vfx1_drv_zero_puck[1];
		roll  = -vfx1_drv_zero_puck[2];
	} else {
		yaw   = (360 - vfx1_drv_zero_hmd[0] % 360) % 360;
		pitch = -vfx1_drv_zero_hmd[1];
		roll  = -vfx1_drv_zero_hmd[2];
	}
#endif
}

// SET_CONFIGURATION: walk the guest buffer (psiz/ptyp chain),
// adopt per-device data-enable bits (HTD: any axis RTCF den;
// HPD: button-descriptor den). Defaults stay on so REPORT works
// even if the app never configures.
static void vfx1_drv_set_config(const uint32_t base)
{
	uint32_t off = 0;
	for (int guard = 0; guard < 16; ++guard) {
		const uint8_t psiz = mem_readb(base + off);
		const uint8_t ptyp = mem_readb(base + off + 1u);
		if (psiz < 2u || psiz > 128u) {
			break;
		}
		if (ptyp == 6) { // HTD
			bool den = false;
			for (int a = 0; a < 3; ++a) {
				const uint32_t r =
				        base + off + 8u +
				        static_cast<uint32_t>(a * 8) + 6u;
				if ((mem_readb(r) & 0x80u) != 0u) {
					den = true;
				}
			}
			vfx1_drv_den_hmd = den;
		} else if (ptyp == 7) { // HPD
			vfx1_drv_den_puck =
			        (mem_readb(base + off + 33u) & 0x80u) !=
			        0u;
		} else if (ptyp == 0) { // NULLP terminator
			break;
		}
		off += psiz;
	}
}

// REPORT: HTD structs (yaw/pitch/roll shorts) then HPD structs
// (+buttons byte), matching the config order apps lay out. Count =
// devices reported.
static int vfx1_drv_report(const uint32_t base)
{
	uint32_t p   = base;
	int      cnt = 0;
	if (vfx1_drv_den_hmd) {
		int yaw = 0, pitch = 0, roll = 0;
		unsigned int btn = 0;
		vfx1_drv_angles(3, yaw, pitch, roll, btn);
		vfx1_drv_put16(p, yaw);
		vfx1_drv_put16(p + 2u, pitch);
		vfx1_drv_put16(p + 4u, roll);
		p += 6u;
		++cnt;
	}
	if (vfx1_drv_den_puck) {
		int yaw = 0, pitch = 0, roll = 0;
		unsigned int btn = 0;
		vfx1_drv_angles(4, yaw, pitch, roll, btn);
		vfx1_drv_put16(p, yaw);
		vfx1_drv_put16(p + 2u, pitch);
		vfx1_drv_put16(p + 4u, roll);
		mem_writeb(p + 6u, static_cast<uint8_t>(btn & 0xFFu));
		p += 7u;
		++cnt;
	}
	return cnt;
}

bool vfx1_int33_handle()
{
	const uint16_t ax   = reg_ax;
	const uint8_t  mask = static_cast<uint8_t>(ax >> 8);
	const uint8_t  fn   = static_cast<uint8_t>(ax & 0xFFu);
	if (mask < 0x60u || mask > 0x70u) {
		return false;
	}
	if (!vfx1_driver_enabled()) {
		// One-shot: INT33 VFX-1 call with the driver off (falls
		// through to the mouse driver).
		static bool noted = false;
		if (!noted && vfx1_logging_enabled()) {
			noted = true;
			LOG_MSG("VFX1: INT33 mask=%02x fn=%02x with vfx1_driver off (enable it, no TSR)",
			        mask,
			        fn);
		}
		return false;
	}
	// DIAG: prove games reach us (mask/fn per call).
	if (vfx1_logging_enabled()) {
		LOG_MSG("VFX1: INT33 mask=%02x fn=%02x bx=%04x cx=%04x dx=%04x es=%04x di=%04x si=%04x",
		        mask,
		        fn,
		        reg_bx,
		        reg_cx,
		        reg_dx,
		        SegValue(es),
		        reg_di,
		        reg_si);
	}
	switch (fn) {
	case 0x03: { // GET_CONFIGURATION: ES:DX buffer in, CX = count,
		     // AX = error out.
		const uint32_t base =
		        SegPhys(es) + static_cast<uint32_t>(reg_dx);
		const int cnt = vfx1_drv_config(base);
		reg_cx        = static_cast<uint16_t>(cnt);
		reg_ax        = 0;
		break;
	}
	case 0x04: { // SET_CONFIGURATION: ES:DX buffer in, AX = error.
		const uint32_t base =
		        SegPhys(es) + static_cast<uint32_t>(reg_dx);
		vfx1_drv_set_config(base);
		reg_ax = 0;
		break;
	}
	case 0x05: { // REPORT: ES:DX buffer in, CX = packets, AX = err.
		const uint32_t base =
		        SegPhys(es) + static_cast<uint32_t>(reg_dx);
		const int cnt = vfx1_drv_report(base);
		reg_cx        = static_cast<uint16_t>(cnt);
		reg_ax        = 0;
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: INT33 REPORT count=%d", cnt);
		}
		break;
	}
	case 0x70: { // GET_DEVICE_DATA: BX=1|(cls<<8) in; BX/CX/DX =
		     // yaw/pitch/roll, AX = err<<8|buttons out.
		const int cls = (reg_bx >> 8) & 0xFF;
		if (cls != 2 && cls != 3 && cls != 4) {
			reg_ax = 0x0100; // error 1, no buttons
			break;
		}
		int yaw = 0, pitch = 0, roll = 0;
		unsigned int btn = 0;
		vfx1_drv_angles(cls, yaw, pitch, roll, btn);
		reg_bx = static_cast<uint16_t>(yaw);
		reg_cx = static_cast<uint16_t>(pitch);
		reg_dx = static_cast<uint16_t>(roll);
		reg_ax = static_cast<uint16_t>(btn & 0xFFu); // err 0
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: INT33 GET_DATA cls=%d yaw=%d pitch=%d roll=%d btn=%u",
			        cls,
			        yaw,
			        pitch,
			        roll,
			        btn);
		}
		break;
	}
	case 0x08: // SET_ZERO: capture current as zero.
	case 0x07: { // RESET_ZERO: restore default zero.
		const int cls = (reg_bx >> 8) & 0xFF;
		if (fn == 0x08 && (cls == 3 || cls == 4)) {
			int yaw = 0, pitch = 0, roll = 0;
			unsigned int btn = 0;
			vfx1_drv_angles(cls, yaw, pitch, roll, btn);
			// Store pre-zero values (angles() already
			// subtracts the old zero, so add back).
			if (cls == 3) {
				vfx1_drv_zero_hmd[0] =
				        static_cast<int16_t>(
				                yaw +
				                vfx1_drv_zero_hmd[0]);
				vfx1_drv_zero_hmd[1] =
				        static_cast<int16_t>(
				                pitch +
				                vfx1_drv_zero_hmd[1]);
				vfx1_drv_zero_hmd[2] =
				        static_cast<int16_t>(
				                roll +
				                vfx1_drv_zero_hmd[2]);
			} else {
				vfx1_drv_zero_puck[1] =
				        static_cast<int16_t>(
				                pitch +
				                vfx1_drv_zero_puck[1]);
				vfx1_drv_zero_puck[2] =
				        static_cast<int16_t>(
				                roll +
				                vfx1_drv_zero_puck[2]);
			}
		} else if (fn == 0x07) {
			for (int i = 0; i < 3; ++i) {
				vfx1_drv_zero_hmd[i]  = 0;
				vfx1_drv_zero_puck[i] = 0;
			}
		}
		reg_ax = 0;
		break;
	}
	default:
		// TEST_DEVICES / SET_VIDEO_PAGE / SET_VIDEO and friends:
		// accepted silently for now (TODO).
		break;
	}
	return true;
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

void VFX1_NotifyEOI(const int irq)
{
	// Only our own line matters: timer (and other) EOIs must not
	// wipe a pending VIP edge (that starved every ISR forever).
	if (irq != vfx1_irq()) {
		return;
	}
	vfx1_raise_no_eoi = 0;
	// Level: stay asserted while bus bytes remain (the ISR drains
	// then EOIs; re-fire on a full backlog is correct hardware
	// behavior and converges as readers drain).
	for (int base = 0; base < 4; ++base) {
		if (vfx1_bus_rx_pending(base)) {
			PIC_ActivateIRQ(static_cast<uint8_t>(vfx1_irq()));
			return;
		}
	}
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
	if (offset == 0x8) {
		vfx1_bus_control(base, value);
		vfx1_bus[base].since_ctrl = 0;
	}
	if (offset == 0x9) {
		++vfx1_bus[base].since_ctrl;
		vfx1_bus_tx(base, value);
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
			if (offset == 0x8 || offset == 0x9) {
				vfx1_bus_maintain(base);
			}
			if (offset == 0x9) {
				// Bus RX transfer: serve device bytes ahead of the
				// echo replay, inside a firmware RX transfer
				// (armed by an entry-shape +0x8 write) OR to an
				// unarmed reader once the host enabled reporting
				// (the VIP streams; bare reads get FIFO bytes on
				// real hardware). Unarmed serving needs reporting
				// (pre-REPORTING traffic, incl. the 0x4806 idle
				// check and the DSP firmware-verify echo replay,
				// keeps legacy bytes).
				const bool framed =
				        vfx1_bus[base].armed &&
				        !vfx1_bus[base].quenched;
				const bool streaming =
				        !vfx1_bus[base].armed &&
				        vfx1_bus[base].reporting;
				if ((framed || streaming) &&
				    vfx1_bus_rx_pending(base)) {
					vfx1_resp_data[base] =
					        vfx1_bus_rx_next(base);
					// The switch below only reports resp_data
					// for latched ports; a pure reader that
					// never wrote +0x9 would otherwise see
					// open-bus 0xFF forever.
					vfx1_reg_valid[base][0x9] = true;
				} else {
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
				// SETVFX1 flag-poll site (its sequencer spinning on
				// [0xA2E+idx] reads +0x0 here; seg observed stable
				// across setups): timestamp it for the flag mirror
				// in vfx1_tsr_tick.
				if (offset == 0x0 && SegValue(cs) == 0x070Cu &&
				    reg_ip == 0x0145u) {
					vfx1_flagpoll_ms = PIC_Ticks;
				}
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
					// Armed transfers own bit 5 (data-ready =
					// queue pending, drained = clear so the RX
					// exit sees completion); the legacy idle
					// bit must not hold it set mid-transfer or
					// the firmware RX spins forever.
					const bool legacy_set =
					        vfx1_resp_ready[base] &&
					        !vfx1_bus[base].armed;
					if (legacy_set) {
						value |= 0x20u;
					} else {
						if (vfx1_logging_full() &&
						    !vfx1_bus[base].armed) {
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
							// Bus RX pending reads EMPTY-set inside
							// an armed transfer (firmware reads bytes
							// while SET); unarmed traffic keeps
							// legacy bits so idle checks (which
							// require exact values) are unaffected,
							// unless the host enabled reporting
							// (streaming: bare reads are data-ready
							// exactly when bytes are pending).
							// Quenched transfers read dry. The CLEAR
							// pulse still wins so TX handshake waits
							// keep their window.
							const bool framed =
							        vfx1_bus[base].armed &&
							        !vfx1_bus[base]
							                .quenched;
							const bool stream_ready =
							        !vfx1_bus[base].armed &&
							        vfx1_bus[base]
							                .reporting;
							if ((framed || stream_ready) &&
							    vfx1_bus_rx_pending(base) &&
							    !vfx1_bus_in_clear_pulse(base)) {
								value |= 0x20u;
							}
					}
						// AB2PCREQ (bit 2) is VIP-driven transfer
						// completion, not latch echo: the firmware
						// RX exit requires it set to finish, and
						// refuses to start while set.
					value &= ~0x04u;
					// EMPTY is live while armed (the real VIP
					// idles it set on a dry bus: the RX entry
					// then returns 0 and a drained transfer
					// exits, instead of spinning on polls);
					// unarmed keeps legacy sticky bits so idle
					// checks see exact values. A quenched
					// transfer counts as dry (see control).
					if (vfx1_bus_req[base] ||
					    (vfx1_bus[base].armed &&
					     (!vfx1_bus_rx_pending(base) ||
					      vfx1_bus[base].quenched))) {
						value |= 0x04u;
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
	// Yield ports owned by an active SU-2000 (its NET 0x280 collides
	// with VIP base 1): answering ETS's NET probes with VIP bytes and
	// unmasking IRQ5 mid-run deterministically crashes it.
	if (SU2000_ClaimsPort(port)) {
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
			LOG_MSG("VFX1: VIP base%d touched while [vfx1] vfx1_enable/vfx1_respond is off (reads float 0xFF: firmware reports 'base port not detected')",
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
