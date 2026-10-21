// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hardware/vfx1.h"

#include "dosbox.h"

#include "cpu/callback.h"
#include "cpu/registers.h"

#include "hardware/su2000.h"
#include "port.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
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
constexpr uint8_t Vfx1WatchedIrqMaster =
        (1u << 2) | (1u << 5) | (1u << 7);
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
// Diagnostics: guest handlers for the valid VIP IRQ choices.
static void vfx1_log_irq_vectors(const int base);
// Last host TX per base (PIC_Ticks ms), bounding the boot-kick below.
static uint32_t vfx1_bus_tx_ms[4] = {};
// Forward: hands-free sweep key, defined with the [vfx1] config below
// but read by the tracking builders above it.
static bool vfx1_test_auto();
// Forward: live-headset switch, defined with the [vfx1] config below
// but read by the tracking builders above it.
static bool vfx1_live_track();
// Forward: live-CyberPuck switch, defined with the [vfx1] config below
// but read by the tracking builders above it.
static bool vfx1_live_puck();
// Forward: reported versions, defined with the [vfx1] config below
// but read by the bus builders above it.
static int vfx1_hwv_low();
static int vfx1_hwv_hi();
static int vfx1_cfg_major();
static int vfx1_cfg_minor();
static void vfx1_fwver_get(const bool hmd, char out7[7]);
// Forward: INT33 tracker angles, defined below but read by the
// callback-client mirror above it.
static void vfx1_drv_angles(const int cls, int& yaw, int& pitch,
                            int& roll, unsigned int& btn);
// Numpad range-sweep testing inputs (mapper hotkeys, momentary): KP7/8/9
// hold sweeps the HMD yaw/pitch/roll, KP2 hold sweeps the puck axes,
// KP4/5/6 hold the puck Top/Middle/Bottom buttons. Only present in
// OpenXR builds (the mapper hotkeys are XR-gated); non-XR builds stay
// centered. With [vfx1] vfx1_live_track on, the real headset
// orientation is chained in as the HMD default (the numpad holds still
// override per axis) and the headset pose also counts as active
// tracking; see vfx1_head_yaw_pitch_roll().
#if C_OPENXR
extern float        sdl_xr_test_hmd[3];
extern float        sdl_xr_test_puck[2];
extern unsigned int sdl_xr_test_puckbtn;
// Live CyberPuck state (right-controller "Puck" role); used only
// while [vfx1] vfx1_live_puck is on.
extern bool         sdl_xr_puck_active;
extern float        sdl_xr_puck_x;
extern float        sdl_xr_puck_y;
extern unsigned int sdl_xr_puck_btn;
// Live XR headset orientation (plain floats across the module
// boundary; false when no valid pose). Used only while
// [vfx1] vfx1_live_track is on.
extern bool openxr_head_quat(float& x, float& y, float& z, float& w);
#endif
static constexpr int Vfx1CompleteReads = 10; // echo reads before completion
// Build stamp: bump on responder changes so logs identify the code.
#define VFX1_BUILD_ID "2026-10-10/grace5000+echo+fed+irqfix+cfgready+fpmon+irqunmask+irqtrig1+backclear+dcbias+puckbtn+busatt+rxdrain+rxempty+streamdrip+rxirq+dirid+idaddr+pucklive+headlive+idtruth+isrkick+fullbus+tsrmem+irqlevel+isrpulse+pollreply+irqedge+irqcmd+idseed+rxquench+streamread+validbit+eoiprecise+idkeep+flagmirror+int33drv+int33cfg+numpadonly+puckxy+int33sense+cfgbytes+testauto+cbaxes+cbpush+read93+bootkick+dirquiet+postkick+verkeys+setvideo+fixscale+irq1shot+pollseg+dirdefaddr+dirboth+dirstock+idkeepfix+kickmask+livekick+idledry+livetrack+invroll+abempoll+tsrprobe11+tsrirq+tsrsvc+irqcascade+irqvec+htdslot+rptval+probe8+livepuck"
// Bases with responders installed (unclaimed ports only).
static bool vfx1_installed[4] = {};
// Ports the responder itself installed (to tell prior claimants apart).
static bool vfx1_port_installed[Vfx1NumSlots] = {};

// True while the guest has the VIP IRQ line masked in its PIC. The TSR
// boot/post kicks below exist only to unstick a guest that masked its
// line and never re-opened it (VFX1.COM's orchestrator); we unmask the
// line ourselves at install and SETVFX1's TEST / the internal INT33
// driver leave it unmasked, so gating on this keeps the kicks quiet
// there (they were injecting spurious IRQs mid-detection).
static bool vfx1_irq_line_masked()
{
	const int irq = vfx1_irq();
	const int pic = (irq < 8) ? 0 : 1;
	// IRQ 8-15 reach the CPU only through the master's cascade
	// (IRQ 2). A guest that masks IRQ 2 blocks the VIP line even
	// when the slave mask bit is clear.
	if (irq >= 8 && (PIC_GetIMR(0) & (1u << 2)) != 0) {
		return true;
	}
	return (PIC_GetIMR(pic) & (1u << (irq & 7))) != 0;
}

// Clear the VIP line's slave mask and, for IRQ 8-15, the master
// cascade mask. Guest PIC setup often leaves IRQ 2 masked; the VIP
// completion interrupt then pends forever (no ISR, no EOI).
static void vfx1_unmask_irq_line()
{
	const int irq = vfx1_irq();
	PIC_SetIRQMask(static_cast<uint32_t>(irq), false);
	if (irq >= 8) {
		PIC_SetIRQMask(2u, false);
	}
}

// True while the user is driving the VFX-1 tracker with the numpad
// sweep holds (or vfx1_test_auto is on), or while a live headset pose
// is being chained in (vfx1_live_track). Live tracking must then be
// streamed AND interrupt-signalled exactly like the real DSP: the
// guest ISRs (SETVFX1's TEST menus, VFX1.COM's demuxer) only read the
// bus in response to the VIP interrupt, and we otherwise raise it only
// on command edges, so without this the stream sits undrained.
static bool vfx1_track_active()
{
	if (vfx1_test_auto()) {
		return true;
	}
#if C_OPENXR
	if (vfx1_live_track()) {
		float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
		if (openxr_head_quat(qx, qy, qz, qw)) {
			return true;
		}
	}
	if (vfx1_live_puck() && sdl_xr_puck_active) {
		return true;
	}
	for (int i = 0; i < 3; ++i) {
		if (sdl_xr_test_hmd[i] != 0.0f) {
			return true;
		}
	}
	for (int i = 0; i < 2; ++i) {
		if (sdl_xr_test_puck[i] != 0.0f) {
			return true;
		}
	}
	if (sdl_xr_test_puckbtn != 0u) {
		return true;
	}
#endif
	return false;
}

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
// IRQCFG mode-1 single-shot test interrupt (SETVFX1's exact-1 IRQ test):
// while this is set, the first EOI does NOT re-assert the level, so the
// test yields exactly one delivered interrupt even with a pending RX
// backlog the test ISR never drains (see VFX1_NotifyEOI).
static bool vfx1_irq_test_pulse = false;

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
	const char* name; // 8 chars, e.g. "VFX1101_"
	uint8_t serial[4];
	uint8_t assigned = 0; // 0 = still on default 0x6E
	// Factory bus address the device answers directed IDREQs from
	// until the host assigns it (SETVFX1's TEST sweeps candidates
	// 2,4,6,8,10; real HMD=2, puck=4). 0 = answers nothing.
	uint8_t default_addr = 0;
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
	VipBusDev hmd = {"VFX1101_", {0xA1, 0xB2, 0xC3, 0xD4}, 0, 0x02};
	VipBusDev puck = {"PUCK102_", {0x12, 0x34, 0x56, 0x78}, 0, 0x04};
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
	char fwver[7];
	vfx1_fwver_get(&d == &vfx1_bus[base].hmd, fwver);
	memcpy(pkt + i, fwver, 7);
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

// Live headset orientation as yaw/pitch/roll (radians). False unless
// [vfx1] vfx1_live_track is on and a valid head pose is available
// (no XR session / no headset). Yaw is full-circle, pitch/roll +-90.
static bool vfx1_head_yaw_pitch_roll(float& yaw, float& pitch, float& roll)
{
	if (!vfx1_live_track()) {
		return false;
	}
#if C_OPENXR
	float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
	if (!openxr_head_quat(qx, qy, qz, qw)) {
		return false;
	}
	pitch = asinf(std::max(-1.0f,
	                       std::min(1.0f, 2.0f * (qw * qx - qy * qz))));
	yaw = atan2f(2.0f * (qw * qy + qx * qz),
	             1.0f - 2.0f * (qx * qx + qy * qy));
	// Roll is inverted relative to the headset pose convention (head
	// tilt up the wrong way otherwise).
	roll = -atan2f(2.0f * (qw * qz + qx * qy),
	               1.0f - 2.0f * (qx * qx + qz * qz));
	roll = std::max(-1.5707964f, std::min(1.5707964f, roll));
	return true;
#else
	(void)yaw;
	(void)pitch;
	(void)roll;
	return false;
#endif
}

// Builds one 12-byte tracking report (wire format, checksummed) for
// the HMD (dev 0) or CyberPuck (dev 1). HMD axes come from the numpad
// range-sweep holds and, with [vfx1] vfx1_live_track on, the real
// headset pose (the holds win per axis; no head pose keeps the
// centered idle). Puck axes come from the numpad holds and, with
// [vfx1] vfx1_live_puck on, the right controller's "Puck" role (the
// holds still win per axis): KP7 sweeps the HMD yaw across the full
// circle, KP8 sweeps the HMD pitch (±1024), KP9 sweeps the HMD roll
// (±1024); KP2/KP3 hold sweeps the puck X/Y axes (±1024); KP4/5/6
// hold the puck Top/Middle/Bottom buttons. Idle (nothing held, no
// live pose) is a rock-static centered packet. Shared by the bus
// queue and the TSR report-slot refresh below.
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
	// vfx1_test_auto reads as all five held (hands-free sweeps).
	const bool auto_sw = vfx1_test_auto();
	const bool sweep_hmd_yaw   = auto_sw || sdl_xr_test_hmd[0] != 0.0f; // KP7
	const bool sweep_hmd_pitch = auto_sw || sdl_xr_test_hmd[1] != 0.0f; // KP8
	const bool sweep_hmd_roll  = auto_sw || sdl_xr_test_hmd[2] != 0.0f; // KP9
	const bool sweep_puck_x    = auto_sw || sdl_xr_test_puck[0] != 0.0f; // KP2
	const bool sweep_puck_y    = auto_sw || sdl_xr_test_puck[1] != 0.0f; // KP3
	const unsigned int puck_btn = sdl_xr_test_puckbtn & 0x07u; // KP4/5/6
#else
	const bool auto_sw = vfx1_test_auto();
	const bool sweep_hmd_yaw   = auto_sw;
	const bool sweep_hmd_pitch = auto_sw;
	const bool sweep_hmd_roll  = auto_sw;
	const bool sweep_puck_x    = auto_sw;
	constexpr bool sweep_puck_y    = false;
	constexpr unsigned int puck_btn = 0u;
#endif
	if (dev == 0) {
		// Raw values are UNSIGNED DC-biased around 0x8000 (signed
		// zeros read as pegged extremes, "BEYOND TRACKING RANGE"), so
		// unheld axes stay centered. Precedence per axis: a held
		// numpad sweep wins, else the chained live headset pose
		// (vfx1_live_track), else centered. Live yaw is a full-circle
		// (cos,sin) pair at +-8192, pitch/roll +-1024 over +-90 deg,
		// matching the wire scales the guests accept.
		float live_yaw = 0.0f, live_pitch = 0.0f, live_roll = 0.0f;
		const bool live_hmd = vfx1_head_yaw_pitch_roll(live_yaw,
		                                               live_pitch,
		                                               live_roll);
		uint16_t yu = 0x8000u;
		uint16_t su = 0x8000u;
		if (sweep_hmd_yaw) {
			yu = vfx1_trk_bias(32768 +
			                   static_cast<int32_t>(8192 * cos(ph)));
			su = vfx1_trk_bias(32768 +
			                   static_cast<int32_t>(8192 * sin(ph)));
		} else if (live_hmd) {
			yu = vfx1_trk_bias(
			        32768 +
			        static_cast<int32_t>(8192.0f * cosf(live_yaw)));
			su = vfx1_trk_bias(
			        32768 +
			        static_cast<int32_t>(8192.0f * sinf(live_yaw)));
		}
		uint16_t pu = 0x8000u;
		if (sweep_hmd_pitch) {
			pu = vfx1_trk_bias(
			        32768 + static_cast<int32_t>(1024 * sin(ph2)));
		} else if (live_hmd) {
			pu = vfx1_trk_bias(
			        32768 +
			        static_cast<int32_t>(1024.0f * live_pitch /
			                             1.5707964f));
		}
		uint16_t ru = 0x8000u;
		if (sweep_hmd_roll) {
			ru = vfx1_trk_bias(
			        32768 + static_cast<int32_t>(1024 * cos(ph2)));
		} else if (live_hmd) {
			ru = vfx1_trk_bias(
			        32768 +
			        static_cast<int32_t>(1024.0f * live_roll /
			                             1.5707964f));
		}
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
		uint16_t xu = 0x8000u;
		uint16_t yu = 0x8000u;
		unsigned int btn = puck_btn;
		if (sweep_puck_x) {
			xu = vfx1_trk_bias(
			        32768 + static_cast<int32_t>(1024 * sin(ph2)));
		}
		if (sweep_puck_y) {
			yu = vfx1_trk_bias(
			        32768 + static_cast<int32_t>(1024 * cos(ph2)));
		}
#if C_OPENXR
		if (vfx1_live_puck() && sdl_xr_puck_active) {
			if (!sweep_puck_x) {
				xu = vfx1_trk_bias(
				        32768 + static_cast<int32_t>(
				                        sdl_xr_puck_x * 1024.0f));
			}
			if (!sweep_puck_y) {
				yu = vfx1_trk_bias(
				        32768 + static_cast<int32_t>(
				                        sdl_xr_puck_y * 1024.0f));
			}
			btn = (btn | sdl_xr_puck_btn) & 0x07u;
		}
#endif
		pkt[3] = 0x80;
		// Byte[4] bit 1 = dispatcher validity gate (VFX1.md 5f);
		// centered yaw reads 0x8002 either way.
		pkt[4] = 0x02;
		pkt[5] = static_cast<uint8_t>((xu >> 8) & 0xFFu);
		pkt[6] = static_cast<uint8_t>(xu & 0xFFu);
		pkt[7] = static_cast<uint8_t>((yu >> 8) & 0xFFu);
		pkt[8] = static_cast<uint8_t>(yu & 0xFFu);
		pkt[9] = static_cast<uint8_t>(btn);
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
static uint16_t vfx1_setvfx1_cs  = 0; // code seg seen at its flag-poll site
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
	// IVT 0x0D is only the TSR's ISR when the driver hooks the
	// classic IRQ 5/INT 0D line. VFX1.COM under vfx1_irq=11 leaves
	// INT 0D at the BIOS stub (tseg=f000), which must NOT veto the
	// slot probe: the resident [0x103] cell and the pattern scan
	// below locate the data seg without it. Only an unhooked INT33
	// vector (no TSR) takes the legacy 0xAEC fallback.
	if (code == 0u || code >= 0xF000u) {
		++probe_n;
		if ((probe_n < 8 || probe_n % 600 == 1) &&
		    vfx1_logging_enabled()) {
			LOG_MSG("VFX1: TSR probe waiting (ivt0d=%04x ivt33=%04x)",
			        tseg,
			        code);
		}
		// If IVT entries are not set up, fall back to using the
		// default TSR data segment at 0xAEC so games can still
		// receive tracking data via INT 33h.
		vfx1_tsr_data_seg = 0xAEC;
		vfx1_tsr_code_seg  = 0;
		vfx1_tsr_locked    = true;
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
	{
		static bool cands_logged = false;
		if (!cands_logged && vfx1_logging_enabled()) {
			cands_logged = true;
			LOG_MSG("VFX1: TSR data-seg candidates trans=%04x res=%04x",
			        trans,
			        res);
		}
	}
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
	vfx1_build_tracking(0, 0, pkt);
	// Descent 1 reads tracker data as HTD class 3 (slot 3), not HMD
	// class 2. File the same head pose there with an HTD source byte
	// and a matching checksum so VFX1.COM's GET_DATA cls=3 has data.
	pkt[1] = 0x03;
	pkt[11] = vfx1_bus_xor(pkt, 11);
	uint32_t htd = vfx1_tsr_linear(data, 0xBB2u); // 0xAEC+0x42*3
	for (int i = 0; i < 12; ++i) {
		mem_writeb(htd + static_cast<uint32_t>(i), pkt[i]);
	}
	vfx1_build_tracking(0, 1, pkt);
	uint32_t puck = vfx1_tsr_linear(data, 0xBFCu); // 0xAEC+0x42*4
	for (int i = 0; i < 12; ++i) {
		mem_writeb(puck + static_cast<uint32_t>(i), pkt[i]);
	}
	// Freshness flags, as the demuxer sets them when filing.
	mem_writeb(vfx1_tsr_linear(data, 0xAE6u), 1u); // 0xAE4+2
	mem_writeb(vfx1_tsr_linear(data, 0xAE7u), 1u); // 0xAE4+3
	mem_writeb(vfx1_tsr_linear(data, 0xAE8u), 1u); // 0xAE4+4
	mem_writeb(vfx1_tsr_linear(data, 0x83Au), 1u); // 0x838+2
	mem_writeb(vfx1_tsr_linear(data, 0x83Bu), 1u); // 0x838+3
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
		// SETVFX1's data seg lives in its relocated `mov ax,imm16`
		// at code offset 0xAE4; read it from whichever segment its
		// flag-poll site ran in (the load address varies per boot).
		const uint16_t ds = (vfx1_setvfx1_cs >= 0x1000u &&
		                     vfx1_setvfx1_cs < 0xF000u)
		                            ? mem_readw(vfx1_tsr_linear(
		                                      vfx1_setvfx1_cs,
		                                      0x0AE4u))
		                            : 0u;
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
	// (the IP==0x0145 +0x0 site, any load segment) for a
	// dispatcher-set found-flag that never comes (its dispatcher
	// inputs were starved by races it cannot win). While its
	// flag-poll is active, mirror found for HMD (2) + puck (4) plus
	// the companion [0xA22] bits the dispatcher sets with them. The
	// devices ARE present and streaming (validated bus content), so
	// this supplements truth it cannot observe, like the TSR slot
	// refresh above. Guard the write with a data-seg signature (the
	// VIP base is parked at [0x10C0]) so a mis-derived segment can
	// never scribble unrelated RAM.
	const uint16_t sbase =
	        (vfx1_setvfx1_ds >= 0x1000u && vfx1_setvfx1_ds < 0xF000u)
	                ? mem_readw(vfx1_tsr_linear(vfx1_setvfx1_ds,
	                                            0x10C0u))
	                : 0u;
	const bool sbase_ok = (sbase == 0x0260u || sbase == 0x0280u ||
	                       sbase == 0x02A0u || sbase == 0x02C0u);
	if (sbase_ok && PIC_Ticks - vfx1_flagpoll_ms < 5000u) {
		const uint32_t fbase =
		        vfx1_tsr_linear(vfx1_setvfx1_ds, 0xA2Eu);
		mem_writeb(fbase + 2u, 1u);
		mem_writeb(fbase + 4u, 1u);
		const uint32_t gbase =
		        vfx1_tsr_linear(vfx1_setvfx1_ds, 0xA22u);
		mem_writeb(gbase + 2u, 1u);
		mem_writeb(gbase + 4u, 1u);
	}
	// Bounded ISR kick for a stalled VFX1.COM boot (NOT a periodic
	// pulse: unconditional pulses over-counted the exact-1 IRQ test).
	// Fires ONLY while exactly one device is assigned (mid-boot) with
	// recent bus activity (<15s), AND the guest has masked the VIP
	// line: VFX1.COM masks it after its own IRQ test and never
	// re-opens it, so without this the boot stalls pre-resident (the
	// orchestrator runs inside its VIP ISR and never vectors to demux
	// the queued replies). SETVFX1's TEST also assigns devices but
	// never masks the line (we unmask it at install), so the mask
	// gate keeps the kick from injecting spurious IRQs mid-detection.
	// Quiesces on completion (both assigned), on reset (AA run clears
	// assignments), and on idle. Frozen IRQ layer untouched (no
	// change to raise/lower/EOI/status/arming semantics; this reuses
	// the command-edge raise).
	for (int base = 0; base < 4; ++base) {
		const auto& b = vfx1_bus[base];
		const bool midboot =
		        (b.hmd.assigned != 0u) != (b.puck.assigned != 0u);
		if (!midboot || !vfx1_installed[base]) {
			continue;
		}
		// Only unstick a line the guest itself masked (see helper):
		// SETVFX1's TEST / the internal driver leave it unmasked.
		if (!vfx1_irq_line_masked()) {
			continue;
		}
		if (PIC_Ticks - vfx1_bus_tx_ms[base] >= 15000u) {
			continue;
		}
		PIC_SetIRQMask(static_cast<uint32_t>(vfx1_irq()), false);
		vfx1_raise_irq(base);
		static int kick_n[4] = {};
		if (vfx1_logging_enabled() &&
		    (++kick_n[base] % 50) == 1) {
			LOG_MSG("VFX1: VIP boot-kick base%d (mid-boot, recent TX)",
			        base);
		}
	}
	// Post-enumeration stall kick: after BOTH devices are assigned
	// VFX1.COM sits foreground-idle (no port I/O, no hook, no exit -
	// observed hanging with zero reads) waiting on its VIP ISR, which
	// never vectors (masked line, no further bus arrivals to raise
	// on). Keep unmasking+pulsing while streaming is up (REPORTING),
	// the TSR probe has not locked (not resident/demuxing yet), and
	// a lifetime cap (1800 pulses ~= 3 min) is not exhausted, and the
	// guest has masked the VIP line (same gate as the boot kick).
	// Frozen-layer discipline unchanged (command-edge raise reuse).
	{
		static int post_n = 0;
		bool fresh = false;
		for (int base = 0; base < 4; ++base) {
			const auto& b = vfx1_bus[base];
			if (b.stage == 0 && b.hmd.assigned == 0u &&
			    b.puck.assigned == 0u) {
				post_n = 0; // fresh/idle bus: renew budget
				continue;
			}
			if (b.stage == 2 && b.reporting && !vfx1_tsr_locked &&
			    vfx1_installed[base] && post_n < 1800 &&
			    vfx1_bus_tx_ms[base] != 0u &&
			    vfx1_irq_line_masked()) {
				fresh = true;
			}
		}
		if (fresh) {
			++post_n;
			for (int base = 0; base < 4; ++base) {
				const auto& b = vfx1_bus[base];
				if (b.stage == 2 && b.reporting && !vfx1_tsr_locked &&
				    vfx1_installed[base] && vfx1_irq_line_masked()) {
					PIC_SetIRQMask(
					        static_cast<uint32_t>(vfx1_irq()),
					        false);
					vfx1_raise_irq(base);
				}
			}
			if (vfx1_logging_enabled() && (post_n % 50) == 1) {
				LOG_MSG("VFX1: VIP post-boot kick (%d)",
				        post_n);
			}
		}
	}
	// Live tracking kick: while a numpad sweep key is held (or
	// vfx1_test_auto is on) the card must behave like the real DSP -
	// stream tracking AND interrupt. The guest ISRs (SETVFX1's TEST
	// menus and VFX1.COM's demuxer) only read the bus in response to
	// the VIP interrupt, and we otherwise raise it only on command
	// edges, so the stream would sit undrained and nothing would move.
	// Gate on the line being UNMASKED (the guest is listening): this
	// is the opposite gate to the boot/post kicks above, which exist
	// only to unstick a guest that masked its own line. Reuses the
	// command-edge raise (frozen IRQ layer); the level EOI keeps it
	// re-firing while the top-up keeps RX non-empty, and it quiesces
	// the instant the key is released.
	if (vfx1_track_active()) {
		for (int base = 0; base < 4; ++base) {
			if (!vfx1_installed[base] ||
			    vfx1_irq_line_masked()) {
				continue;
			}
			vfx1_raise_irq(base);
		}
	}
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
		// Match on the ID-reply frame magic + device name only. Do
		// NOT key on the address byte: vfx1_bus_idreply writes the
		// host-assigned address there once a device is moved off the
		// 0x6E default, so a 0x6E-only test silently fails after the
		// first ASSIGN and the keep-alive then re-queues the same
		// reply every maintain tick. That flooded the RX queue with
		// one device's record (observed: endless "50 0c ... VFX1101_"
		// and no puck record ever reaching SETVFX1's 0875:003e ID
		// reader), so the sweep never confirmed a device and TEST
		// VFX1 stayed "VFX1/CYBER PUCK NOT ONLINE".
		auto has_id = [&](const VipBusDev& d) {
			for (size_t i = b.rx_pos; i + 33 <= b.rx.size();
			     ++i) {
				if (b.rx[i] == 0x50 && b.rx[i + 2] == 0x9D &&
				    b.rx[i + 3] == 0xE1 &&
				    b.rx[i + 4] == 0x42 &&
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
	const bool track_on = b.reporting || vfx1_track_active();
	while (!b.armed && track_on && b.rx.size() - b.rx_pos < 256) {
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
	// Last host-bus activity per base (ms): bounds the boot-kick in
	// vfx1_tsr_tick so a stale mid-boot state can't pulse forever.
	vfx1_bus_tx_ms[base] = PIC_Ticks;
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
			vfx1_log_irq_vectors(base);
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
	//
	// This is the EXACT rule the 2026-10-13 TEST-VFX1-menu build
	// shipped (D:\Emu\Dbox-build\vfx1-testvfx1-menu-20261013), i.e.
	// the last state where TEST VFX1 reached its interactive menu and
	// VFX1.COM still booted. A later "dirquiet" rewrite (answer only
	// the device assigned at the probed address, silence everything
	// else) regressed TEST VFX1 to "VFX1/CYBER PUCK NOT ONLINE":
	// both devices are still unassigned at TEST time, and the sweep
	// needs a usable reply for EVERY candidate it walks (2,4,6,8,10),
	// not just the first two. Restored verbatim here.
	if (n >= 5 && w[n - 5] != 0x6Eu && w[n - 4] == 0x50 &&
	    w[n - 3] == 0x81 && w[n - 2] == 0xF1 &&
	    (w[n - 5] ^ w[n - 4] ^ w[n - 3] ^ w[n - 2]) == w[n - 1]) {
		if (!vfx1_bus_framed(base, 5)) {
			return;
		}
		const uint8_t probed = w[n - 5];
		bool answered        = true;
		if (probed == 0x02u) {
			vfx1_bus_idreply(base, b.hmd);
		} else if (probed == 0x04u) {
			vfx1_bus_idreply(base, b.puck);
		} else {
			vfx1_bus_idreply(base, b.hmd);
			vfx1_bus_idreply(base, b.puck);
		}
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP bus base%d: host directed IDREQ addr %u%s",
			        base,
			        probed,
			        answered ? ", ID reply queued"
			                 : " (empty, silent)");
			vfx1_log_irq_vectors(base);
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
			char fwver[7];
			vfx1_fwver_get(d == &b.hmd, fwver);
			if (memcmp(w.data() + at + 5, fwver, 7) == 0 &&
			    memcmp(w.data() + at + 12, "FORTE___", 8) == 0 &&
			    memcmp(w.data() + at + 20, d->name, 8) == 0 &&
			    memcmp(w.data() + at + 28, d->serial, 4) == 0) {
				const uint8_t newaddr = w[ab];
				const uint8_t oldhmd  = b.hmd.assigned;
				const uint8_t oldpuck = b.puck.assigned;
				d->assigned = newaddr;
				if (b.hmd.assigned != 0u &&
				    b.puck.assigned != 0u) {
					b.stage = 2;
				}
			if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: VIP bus base%d: assign %s -> addr %u (was HMD %u puck %u, layout %u, stage %d)",
			        base,
			        d->name,
			        d->assigned,
			        oldhmd,
			        oldpuck,
			        static_cast<unsigned>(layout),
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
				// Fire the completion edge: the TSR's
				// presence probe (@0x1F62) and power phase
				// spin up to 0x7530 INs waiting for [0xAE5],
				// which only the ISR's demuxer sets. Without
				// this edge every probe burns its full
				// timeout (observed ~19k reads/s, multi-second
				// in-game freezes) even though the reply is
				// already queued.
				PIC_SetIRQMask(static_cast<uint32_t>(vfx1_irq()),
				               false);
				vfx1_raise_irq(base);
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

	pbool = section->AddBool("vfx1_test_auto", Always, false);
	pbool->SetHelp(
	        "Hands-free VFX-1 range sweeps ('off' by default): when on,\n"
	        "all five motion sweeps run continuously as if KP7/8/9 (HMD\n"
	        "yaw/pitch/roll) and KP2/3 (puck X/Y) were all held, for\n"
	        "unattended testing (VRMOUSE/game range checks, log\n"
	        "forensics). Puck buttons stay released. Overrides nothing:\n"
	        "explicit numpad holds work the same with it on or off.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	pbool = section->AddBool("vfx1_live_track", Always, false);
	pbool->SetHelp(
	        "Chain the XR headset into VFX-1 tracking ('off' by default):\n"
	        "when on, the real headset yaw/pitch/roll drives the emulated\n"
	        "HMD tracking reports (head-look in TEST VFX1, the internal\n"
	        "INT 33h driver, and games that read the VIP bus) instead of\n"
	        "the centered idle packet. A held numpad sweep (KP7/8/9)\n"
	        "still overrides its axis, and 'vfx1_test_auto' is unaffected.\n"
	        "Needs a running OpenXR session: without a valid head pose\n"
	        "this key does nothing.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	pbool = section->AddBool("vfx1_live_puck", Always, true);
	pbool->SetHelp(
	        "Chain the XR right controller into VFX-1 CyberPuck input\n"
	        "('on' by default): when the right-hand role 'Puck' is\n"
	        "selected from the controller mode wheel, the right thumbstick\n"
	        "drives the puck X/Y axes, the trigger is Top, B is Middle and\n"
	        "A is Bottom (in TEST CYBERPUCK, the internal INT 33h driver,\n"
	        "and games that read the VIP bus). A held numpad test (KP2/KP3\n"
	        "sweeps, KP4/5/6 buttons) still overrides per axis. The role\n"
	        "is inert while 'off'.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto pintv = section->AddInt("vfx1_hwv_low", Always, 1);
	pintv->SetHelp(
	        "Hardware version low byte answered on VIP port +0xD ('1'\n"
	        "by default). Per docs/vfx1_drv.md §5 the HWID test accepts\n"
	        "any value except 0xFF (which reads as a floating bus, so\n"
	        "the card reports 'not detected'). Match it to a real card\n"
	        "when comparing against hardware. Valid range from 0 to 255.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pintv->SetMinMax(0, 255);

	pintv = section->AddInt("vfx1_hwv_hi", Always, 1);
	pintv->SetHelp(
	        "Hardware version high byte answered on VIP port +0xE ('1'\n"
	        "by default). Same accept rule as 'vfx1_hwv_low' (anything\n"
	        "but 0xFF). Valid range from 0 to 255.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pintv->SetMinMax(0, 255);

	pintv = section->AddInt("vfx1_cfg_major", Always, 2);
	pintv->SetHelp(
	        "Major version in the INT 33h GET_CONFIGURATION packets\n"
	        "('2' by default, as SYSI/HMS/HTD/HPD vmaj). Written after\n"
	        "the minor byte per the DOSSDK VFX1.H cfpack structs.\n"
	        "Valid range from 0 to 255.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pintv->SetMinMax(0, 255);

	pintv = section->AddInt("vfx1_cfg_minor", Always, 1);
	pintv->SetHelp(
	        "Minor version in the INT 33h GET_CONFIGURATION packets\n"
	        "('1' by default, as SYSI/HMS/HTD/HPD vmin). Written before\n"
	        "the major byte per the DOSSDK VFX1.H cfpack structs.\n"
	        "Valid range from 0 to 255.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pintv->SetMinMax(0, 255);

	auto pstring = section->AddString("vfx1_fw_hmd", Always, "V1.2___");
	pstring->SetHelp(
	        "HMD firmware revision painted into bus ID replies\n"
	        "('V1.2___' by default): 7 chars, shorter values are\n"
	        "'_' padded SDK-style, longer ones truncated. The same\n"
	        "string gates assign-address matching, so both sides stay\n"
	        "consistent.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	pstring = section->AddString("vfx1_fw_puck", Always, "V1.4___");
	pstring->SetHelp(
	        "CyberPuck firmware revision painted into bus ID replies\n"
	        "('V1.4___' by default): same 7-char rules as 'vfx1_fw_hmd'.\n"
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

// Show which guest handler each valid VIP IRQ points at. The old
// diagnostic always read INT 0Dh (IRQ 5) and mislabelled every line
// as "IRQ5" even when vfx1_irq was 11; log on vector change only.
static void vfx1_log_irq_vectors(const int base)
{
	if (!vfx1_logging_enabled()) {
		return;
	}
	const int irq = vfx1_irq();
	const int irqs[] = {5, 7, 10, 11, 12, 15};
	uint32_t sig     = 2166136261u;
	for (int n : irqs) {
		const int intno = (n < 8) ? (0x08 + n) : (0x70 + n - 8);
		const uint32_t vec = static_cast<uint32_t>(intno) * 4u;
		const uint16_t off = mem_readw(vec);
		const uint16_t seg = mem_readw(vec + 2u);
		sig = (sig ^ off) * 16777619u;
		sig = (sig ^ seg) * 16777619u;
	}
	sig ^= static_cast<uint32_t>(base + 1);
	static uint32_t last_sig = 0;
	if (sig == last_sig) {
		return;
	}
	last_sig = sig;
	LOG_MSG("VFX1: VIP base%d IRQ %d; guest vectors 5=%04x:%04x 7=%04x:%04x 10=%04x:%04x 11=%04x:%04x 12=%04x:%04x 15=%04x:%04x",
	        base,
	        irq,
	        mem_readw(0x34u),
	        mem_readw(0x36u),
	        mem_readw(0x3cu),
	        mem_readw(0x3eu),
	        mem_readw(0x1c8u),
	        mem_readw(0x1cau),
	        mem_readw(0x1ccu),
	        mem_readw(0x1ceu),
	        mem_readw(0x1d0u),
	        mem_readw(0x1d2u),
	        mem_readw(0x1dcu),
	        mem_readw(0x1deu));
}

static bool vfx1_driver_enabled()
{
	const auto* section = get_section("vfx1");
	return section && section->GetBool("vfx1_driver");
}

// Hands-free sweeps (vfx1_test_auto): all five motion axes sweep as if
// held, for unattended testing. Read live (per packet) like the other
// [vfx1] keys.
static bool vfx1_test_auto()
{
	const auto* section = get_section("vfx1");
	return section && section->GetBool("vfx1_test_auto");
}

// Live-headset chaining (vfx1_live_track): feed the real XR headset
// orientation into VFX-1 HMD tracking. Read live (per packet) like the
// other [vfx1] keys; does nothing without a valid head pose.
static bool vfx1_live_track()
{
	const auto* section = get_section("vfx1");
	return section && section->GetBool("vfx1_live_track");
}

// Live-CyberPuck chaining (vfx1_live_puck): feed the right XR
// controller's "Puck" role into VFX-1 puck tracking. Read live (per
// packet) like the other [vfx1] keys; inert unless that role is
// selected from the controller mode wheel.
static bool vfx1_live_puck()
{
	const auto* section = get_section("vfx1");
	return section && section->GetBool("vfx1_live_puck");
}

// Reported versions, all live from [vfx1] keys (docs/vfx1_drv.md: the
// HWID test accepts any version bytes except 0xFF; packet/struct
// layouts follow the DOSSDK VFX1.H cfpack order). Defaults reproduce
// the long-proven values byte-for-byte.
static int vfx1_hwv_low()
{
	const auto* section = get_section("vfx1");
	return section ? section->GetInt("vfx1_hwv_low") & 0xFF : 1;
}

static int vfx1_hwv_hi()
{
	const auto* section = get_section("vfx1");
	return section ? section->GetInt("vfx1_hwv_hi") & 0xFF : 1;
}

static int vfx1_cfg_major()
{
	const auto* section = get_section("vfx1");
	return section ? section->GetInt("vfx1_cfg_major") & 0xFF : 2;
}

static int vfx1_cfg_minor()
{
	const auto* section = get_section("vfx1");
	return section ? section->GetInt("vfx1_cfg_minor") & 0xFF : 1;
}

// Firmware revision painted into bus ID replies (7 chars,
// SDK-style '_' padded, longer values truncated). Same strings gate
// assign-address matching, so both sides stay consistent.
static void vfx1_fwver_get(const bool hmd, char out7[7])
{
	const char* fallback = hmd ? "V1.2___" : "V1.4___";
	std::string v        = fallback;
	if (const auto* section = get_section("vfx1")) {
		const auto s =
		        section->GetString(hmd ? "vfx1_fw_hmd" : "vfx1_fw_puck");
		if (!s.empty()) {
			v = s;
		}
	}
	for (int i = 0; i < 7; ++i) {
		out7[i] = i < static_cast<int>(v.size())
		                  ? v[static_cast<size_t>(i)]
		                  : '_';
	}
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

// GET_CONFIGURATION chain (225 bytes total: 15+98+44+32+34+2).
// Layouts per DOSSDK VFX1.H (cfpack structs); reporting optics are
// documented guesses (see code). Returns the total BYTE count: clients
// (EXAMPLE1.C, VRMOUSE) walk `i < count` stepping by each packet's psiz
// byte, so a packet count here truncates the walk after SYSI and no
// HTD/HPD is ever found.
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
	pkt[i++] = static_cast<uint8_t>(vfx1_cfg_minor());
	pkt[i++] = static_cast<uint8_t>(vfx1_cfg_major()); // vmin/vmaj
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
	pkt[i++] = static_cast<uint8_t>(vfx1_cfg_minor());
	pkt[i++] = static_cast<uint8_t>(vfx1_cfg_major()); // vmin/vmaj
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
	pkt[i++] = static_cast<uint8_t>(vfx1_cfg_minor());
	pkt[i++] = static_cast<uint8_t>(vfx1_cfg_major()); // vmin/vmaj
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
	pkt[i++] = static_cast<uint8_t>(vfx1_cfg_minor());
	pkt[i++] = static_cast<uint8_t>(vfx1_cfg_major()); // vmin/vmaj
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
	p += static_cast<uint32_t>(sizeof(term));
	return static_cast<int>(p - base); // 225: total bytes, see above
}
// Internal INT 33h driver live state (degrees; yaw wraps 0-359).
// Zero offsets per device (SET_ZERO captures current, RESET_ZERO
// restores). Numpad range sweeps only: KP7/8/9 hold sweeps the HMD
// yaw/pitch/roll, KP2/KP3 hold sweeps the puck X/Y axes, KP4/5/6 hold
// the puck buttons (vfx1_test_auto reads as all five sweeps held).
// Idle (nothing held) is centered. No real-hardware motion.
static int16_t vfx1_drv_zero_hmd[3]  = {};
static int16_t vfx1_drv_zero_puck[3] = {};
static int     vfx1_drv_sweep        = 0;
// SET_DEVICE_DATA (fn 0x71, SenseSetVideo) HMD display mode for cls=2
// (DCHMS): 0=mono, 1=multi-buffer stereo, 2=line-sequential stereo.
// Descent 2 probes with cls=2/mode=2 and refuses to enable VFX-1 mode
// unless AX=0 comes back. SET_VIDEO_PAGE (fn 0x80) picks the page.
static uint8_t vfx1_drv_video_mode = 0;
static uint8_t vfx1_drv_video_page = 0;
// SET_CALLBACK registration (fn 0x90): client handler + data seg.
// VRMOUSE registers its INT33 hook address here at load and saves our
// previous-registration answer to restore on unload (passing 0:0 back
// clears us). Only VRMOUSE uses 0x90 (no SDK client does).
static uint16_t vfx1_drv_cb_seg  = 0;
static uint16_t vfx1_drv_cb_off  = 0;
static uint16_t vfx1_drv_cb_data = 0;
// Previous tick's puck buttons, for press/release edge detection.
static unsigned int vfx1_cb_prevbtn = 0;

// Refresh the callback client's mouse-engine cells from live angles.
// VRMOUSE's resident mouse engine ultimately consumes [0x68a] and
// [0x6f0] in its own data segment (its game-call path synthesizes the
// cursor from them), and game AX=0x0003 reads return that synthesis -
// but its refresh path only runs on driver-push invocations, which do
// not exist without this tick. Mirror the si==0 scaling exactly (x86
// idiv truncates toward zero, same as C++ /): [0x68a] = pitch/50,
// [0x6f0] = roll/(-50), [0x550] = buttons byte. Source struct: HPD
// whenever the puck is enabled (DESCENT-style profiles disable the
// headset with -XX -XY, and HPD byte 6 really is buttons), else HTD
// (with a zero buttons byte: HTD byte 6 is roll-high, which would read
// as phantom presses when negative).
static void vfx1_cb_mirror()
{
	if (vfx1_drv_cb_data < 0x1000u || vfx1_drv_cb_data >= 0xF000u) {
		return;
	}
	int hyaw = 0, hpitch = 0, hroll = 0;
	int pyaw = 0, ppitch = 0, proll = 0;
	unsigned int hbtn = 0, pbtn = 0;
	vfx1_drv_angles(3, hyaw, hpitch, hroll, hbtn);
	vfx1_drv_angles(4, pyaw, ppitch, proll, pbtn);
	// Source struct: HPD whenever the puck is enabled (its byte 6
	// really is buttons, and its pitch/roll are the puck axes), else
	// HTD (with a zero buttons byte: HTD byte 6 is roll-high, which
	// would read as phantom presses when negative). den flags do NOT
	// select here (profiles like DESCENT leave both enabled while
	// using only the puck): HPD-first keeps buttons live everywhere
	// the puck exists.
	const bool use_puck = vfx1_drv_den_puck;
	const int mpitch    = use_puck ? ppitch : hpitch;
	const int mroll     = use_puck ? proll : hroll;
	const uint32_t cell = static_cast<uint32_t>(vfx1_drv_cb_data) * 16u;
	mem_writew(cell + 0x68Au, static_cast<uint16_t>(mpitch / 50));
	mem_writew(cell + 0x6F0u, static_cast<uint16_t>(mroll / -50));
	mem_writeb(cell + 0x550u,
	           use_puck ? static_cast<uint8_t>(pbtn & 0xFFu) : 0);
	// Button press/release edges for INT33 AX=0x0005 readers (Descent
	// fire): VRMOUSE's 0x0005 handler returns per-button press counts
	// [0x6BE+2i], release counts [0x6C4+2i] and held latch [0x6D2+2i]
	// (returning + clearing the latch), but nothing maintains them -
	// its edge path only runs on driver pushes. Maintain them here
	// from the live puck buttons ( latched until the game reads).
	const unsigned int cur = pbtn & 0x07u;
	for (int i = 0; i < 3; ++i) {
		const unsigned int bit = 1u << i;
		const bool was = (vfx1_cb_prevbtn & bit) != 0u;
		const bool is  = (cur & bit) != 0u;
		if (is && !was) {
			const uint16_t pc = mem_readw(
			        cell + 0x6BEu +
			        static_cast<uint32_t>(i) * 2u);
			if (pc < 0x7FFFu) {
				mem_writew(cell + 0x6BEu +
				                   static_cast<uint32_t>(i) * 2u,
				           static_cast<uint16_t>(pc + 1u));
			}
			mem_writew(cell + 0x6D2u +
			                   static_cast<uint32_t>(i) * 2u,
			           1);
		} else if (!is && was) {
			const uint16_t rc = mem_readw(
			        cell + 0x6C4u +
			        static_cast<uint32_t>(i) * 2u);
			if (rc < 0x7FFFu) {
				mem_writew(cell + 0x6C4u +
				                   static_cast<uint32_t>(i) * 2u,
				           static_cast<uint16_t>(rc + 1u));
			}
			mem_writew(cell + 0x6D2u +
			                   static_cast<uint32_t>(i) * 2u,
			           0);
		} else if (is) {
			mem_writew(cell + 0x6D2u +
			                   static_cast<uint32_t>(i) * 2u,
			           1);
		}
	}
	vfx1_cb_prevbtn = cur;
}

// Return stub for client-callback pushes (a CBRET_STOP trap the guest
// IRET lands on).
static uint8_t vfx1_cb_stop_id = 0;
static Bitu vfx1_cb_stop_handler()
{
	return CBRET_STOP;
}
static void vfx1_cb_stop_ensure()
{
	if (vfx1_cb_stop_id != 0) {
		return;
	}
	vfx1_cb_stop_id = CALLBACK_Allocate();
	CALLBACK_Setup(vfx1_cb_stop_id,
	               &vfx1_cb_stop_handler,
	               CB_RETF,
	               "VFX1 client callback return");
}

// Push one data-ready invocation into the registered client handler
// (VRMOUSE): far-invoke it with AX = variant, using an IRET frame
// (VFX1.COM invokes with pushf+far; VRMOUSE's handler ends in IRET via
// its shared epilogue). A RETF-terminated client would unbalance one
// word; only VRMOUSE registers 0x90 in practice. Skipped in protected
// mode (real-mode TSR address) and while a push is already running.
static bool vfx1_cb_pushing = false;
static void vfx1_cb_push(const uint16_t ax_in)
{
	if (vfx1_cb_pushing || cpu.pmode) {
		return;
	}
	if (vfx1_drv_cb_seg == 0u && vfx1_drv_cb_off == 0u) {
		return;
	}
	vfx1_cb_stop_ensure();
	if (vfx1_cb_stop_id == 0) {
		return;
	}
	vfx1_cb_pushing = true;
	// Preserve the outer context across the synthetic invocation:
	// the guest handler clobbers AX, and its IRET pops our pushed
	// flags (which must carry the outer DF/TF/etc, plus IF so the
	// machine cannot wedge if entered with interrupts off).
	const uint16_t outer_ax    = reg_ax;
	const uint16_t outer_flags = static_cast<uint16_t>(reg_flags | 0x200u);
	reg_ax = ax_in;
	CPU_Push16(outer_flags);
	const RealPt stop = CALLBACK_RealPointer(vfx1_cb_stop_id);
	CPU_Push16(RealSegment(stop));
	CPU_Push16(RealOffset(stop));
	const uint32_t oldeip = reg_eip;
	const uint16_t oldcs  = SegValue(cs);
	reg_eip               = vfx1_drv_cb_off;
	SegSet16(cs, vfx1_drv_cb_seg);
	DOSBOX_RunMachine();
	reg_eip = oldeip;
	SegSet16(cs, oldcs);
	reg_ax = outer_ax;
	vfx1_cb_pushing = false;
}

// Data-ready push tick (30 Hz): mirrors fresh axes into the client's
// engine cells, then runs the cursor-synthesis variant (si==3) of its
// handler, so game AX=0x0003 reads (which return that synthesis)
// track live motion with no game traffic of their own. The abs/keys
// variant (si==1, client-private report pointers) is not pushed.
static bool     vfx1_cb_timer_on = false;
static void vfx1_cb_tick(uint32_t)
{
	if (vfx1_driver_enabled() && !cpu.pmode && !vfx1_cb_pushing &&
	    (vfx1_drv_cb_seg != 0u || vfx1_drv_cb_off != 0u)) {
		vfx1_cb_mirror();
		// PUSH DISABLED (typing): invoking the client handler runs
		// its abs/key matcher, which sprays phantom keystrokes with
		// empty/default tables (BIOS buffer floods, DOS prompt
		// unusable). The mirror alone is inert (writes only, no
		// guest code runs). Push returns once a gated design exists.
		// vfx1_cb_push(3);
	}
	PIC_AddEvent(vfx1_cb_tick, 1000.0 / 30.0, 0);
}

static void vfx1_cb_start()
{
	if (vfx1_cb_timer_on) {
		return;
	}
	vfx1_cb_timer_on = true;
	PIC_AddEvent(vfx1_cb_tick, 1000.0 / 30.0, 0);
}

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
	// vfx1_test_auto reads as all five held (hands-free sweeps).
	const bool auto_sw = vfx1_test_auto();
	// Chained live headset pose (vfx1_live_track); off by default.
	float live_yaw = 0.0f, live_pitch = 0.0f, live_roll = 0.0f;
	const bool live = vfx1_head_yaw_pitch_roll(live_yaw,
	                                           live_pitch,
	                                           live_roll);
	if (cls == 3) {
		if (auto_sw || sdl_xr_test_hmd[0] != 0.0f) { // KP7: yaw sweep 0-359
			yaw = (vfx1_drv_sweep - vfx1_drv_zero_hmd[0] % 360 +
			       360) %
			      360;
		} else if (live) {
			yaw = ((static_cast<int>(live_yaw * 57.2957795f) -
			        vfx1_drv_zero_hmd[0]) %
			           360 +
			       720) %
			      360;
		} else {
			yaw = (360 - vfx1_drv_zero_hmd[0] % 360) % 360;
		}
		if (auto_sw || sdl_xr_test_hmd[1] != 0.0f) { // KP8: pitch sweep +-90
			pitch = static_cast<int>(90.0 * sin(sw)) -
			        vfx1_drv_zero_hmd[1];
		} else if (live) {
			pitch = static_cast<int>(live_pitch * 57.2957795f) -
			        vfx1_drv_zero_hmd[1];
		} else {
			pitch = -vfx1_drv_zero_hmd[1];
		}
		if (auto_sw || sdl_xr_test_hmd[2] != 0.0f) { // KP9: roll sweep +-90
			roll = static_cast<int>(90.0 * sin(sw + 1.5707963)) -
			       vfx1_drv_zero_hmd[2];
		} else if (live) {
			roll = static_cast<int>(live_roll * 57.2957795f) -
			       vfx1_drv_zero_hmd[2];
		} else {
			roll = -vfx1_drv_zero_hmd[2];
		}
	} else if (cls == 4) {
		// KP2/KP3: X/Y axes sweep +-90 each; yaw unused. KP4/5/6:
		// buttons. With vfx1_live_puck on, the right controller's
		// "Puck" role supplies absolute X/Y (+-90) and buttons; a
		// held numpad sweep still wins per axis.
		const bool live_puck =
		        vfx1_live_puck() && sdl_xr_puck_active;
		if (auto_sw || sdl_xr_test_puck[0] != 0.0f) {
			pitch = static_cast<int>(90.0 * sin(sw)) -
			        vfx1_drv_zero_puck[1];
		} else if (live_puck) {
			pitch = static_cast<int>(sdl_xr_puck_x * 90.0f) -
			        vfx1_drv_zero_puck[1];
		} else {
			pitch = -vfx1_drv_zero_puck[1];
		}
		if (auto_sw || sdl_xr_test_puck[1] != 0.0f) {
			roll = static_cast<int>(90.0 * sin(sw + 1.5707963)) -
			       vfx1_drv_zero_puck[2];
		} else if (live_puck) {
			roll = static_cast<int>(sdl_xr_puck_y * 90.0f) -
			       vfx1_drv_zero_puck[2];
		} else {
			roll = -vfx1_drv_zero_puck[2];
		}
		btn = (sdl_xr_test_puckbtn |
		       (live_puck ? sdl_xr_puck_btn : 0u)) &
		      0x07u;
	}
#else
	const bool auto_sw = vfx1_test_auto();
	if (cls == 4) {
		yaw   = 0;
		if (auto_sw) {
			pitch = static_cast<int>(90.0 * sin(sw)) -
			        vfx1_drv_zero_puck[1];
			roll = static_cast<int>(90.0 * sin(sw + 1.5707963)) -
			       vfx1_drv_zero_puck[2];
		} else {
			pitch = -vfx1_drv_zero_puck[1];
			roll  = -vfx1_drv_zero_puck[2];
		}
	} else {
		if (auto_sw) {
			yaw = (vfx1_drv_sweep - vfx1_drv_zero_hmd[0] % 360 +
			       360) %
			      360;
			pitch = static_cast<int>(90.0 * sin(sw)) -
			        vfx1_drv_zero_hmd[1];
			roll = static_cast<int>(90.0 * sin(sw + 1.5707963)) -
			       vfx1_drv_zero_hmd[2];
		} else {
			yaw   = (360 - vfx1_drv_zero_hmd[0] % 360) % 360;
			pitch = -vfx1_drv_zero_hmd[1];
			roll  = -vfx1_drv_zero_hmd[2];
		}
	}
#endif
}

// Tracker wire units. Clients (e.g. Descent's SenseGetData:
// `*yaw = (short)(ebx & 0xffff); *yaw *= -1;` into a 16.16 `fix`)
// consume head angles as signed 16-bit counts where 65536 = 360deg
// (F1_0), i.e. +-32768 = +-180deg -- the same unit the real VFX1.COM
// normalizes into (clamp constants 0x7FFF/0x8002). vfx1_drv_angles()
// works in whole degrees internally (zero-tracking stays degree-based),
// so convert here at the wire boundary.
static int16_t vfx1_drv_deg_to_fix16(const int deg)
{
	long long v = static_cast<long long>(deg) * 65536LL +
	              (deg >= 0 ? 180LL : -180LL); // round to nearest
	v /= 360LL;
	v %= 65536LL;
	if (v > 32767LL) {
		v -= 65536LL;
	} else if (v < -32768LL) {
		v += 65536LL;
	}
	return static_cast<int16_t>(v);
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

// DIAG (temporary): probe the guest game (Flight Unlimited) for its
// head-tracking enable gate while its VFX1 driver is being polled. Flight
// is a flat LE image (vaddr == linear), so once we locate the loaded image
// by the "Vfx1" device descriptor we can read its globals directly.
static void vfx1_diag_probe_flight()
{
	static bool scanned  = false;
	static uint32_t bias = 0u;

	if (!scanned) {
		scanned              = true;
		const uint32_t total = MEM_TotalPages() << 12u;
		int hits             = 0;
		for (uint32_t a = 0u; a + 16u < total; ++a) {
			if (mem_readd(a) != 0x31786656u) { // "Vfx1"
				continue;
			}
			LOG_MSG("VFX1: PROBE vfx1str @%08x b=%02x%02x%02x%02x "
			        "%02x%02x%02x%02x %02x%02x%02x%02x d12=%02x",
			        a,
			        mem_readb(a + 0u),
			        mem_readb(a + 1u),
			        mem_readb(a + 2u),
			        mem_readb(a + 3u),
			        mem_readb(a + 4u),
			        mem_readb(a + 5u),
			        mem_readb(a + 6u),
			        mem_readb(a + 7u),
			        mem_readb(a + 8u),
			        mem_readb(a + 9u),
			        mem_readb(a + 10u),
			        mem_readb(a + 11u),
			        mem_readb(a + 12u));
			if (bias == 0u && mem_readb(a + 12u) == 0x03u) {
				bias = a - 0x141764u;
			}
			if (++hits >= 12) {
				break;
			}
		}
		if (hits == 0) {
			static const char anchor[] = "v:\\x\\src\\flite\\pilotlog.h";
			const uint32_t alen        = sizeof(anchor) - 1u;
			int ahits                  = 0;
			for (uint32_t a = 0u; a + alen < total; ++a) {
				if (mem_readb(a) !=
				    static_cast<uint8_t>(anchor[0])) {
					continue;
				}
				bool ok = true;
				for (uint32_t i = 1u; i < alen; ++i) {
					if (mem_readb(a + i) !=
					    static_cast<uint8_t>(anchor[i])) {
						ok = false;
						break;
					}
				}
				if (ok) {
					LOG_MSG("VFX1: PROBE anchor @%08x bias=%08x",
					        a,
					        a - 0x132238u);
					if (bias == 0u) {
						bias = a - 0x132238u;
					}
					if (++ahits >= 8) {
						break;
					}
				}
			}
			LOG_MSG("VFX1: PROBE anchor hits=%d", ahits);
		}
		LOG_MSG("VFX1: PROBE img_bias=%08x hits=%d total=%08x",
		        bias,
		        hits,
		        total);
	}

	const uint32_t b       = bias;
	const uint8_t  b139804 = mem_readb(b + 0x139804u);
	const uint32_t headset = mem_readd(b + 0x139E60u);
	const uint32_t devptr  = mem_readd(b + 0x13F7E4u);
	const uint32_t dev1c   = mem_readb(devptr + 0x1Cu);
	const uint32_t dev23   = mem_readb(devptr + 0x23u);
	const uint8_t  b1aee20 = mem_readb(b + 0x1AEE20u);
	const uint32_t stptr   = mem_readd(b + 0x1AEDF4u);
	const uint32_t ctl_h   = mem_readd(b + 0x13BAC4u + 0x1Du * 6u);
	const uint32_t ctl_v   = mem_readw(b + 0x13BAC8u + 0x1Du * 6u);
	const uint32_t live    = stptr ? mem_readw(stptr + 0x1Du * 2u + 9u)
	                               : 0xFFFFu;
	const uint32_t rptfn   = mem_readd(b + 0x13F794u);
	const uint32_t cached  = mem_readd(b + 0x139E64u);
	const uint32_t cyaw    = cached ? mem_readw(cached + 0x08u) : 0u;
	const uint32_t cpit    = cached ? mem_readw(cached + 0x0Au) : 0u;
	const uint32_t crol    = cached ? mem_readw(cached + 0x0Cu) : 0u;
	const uint32_t t54     = mem_readw(b + 0x215D54u);
	const uint32_t t56     = mem_readw(b + 0x215D56u);
	const uint32_t pcd0    = mem_readd(b + 0x215CD0u);
	const uint32_t d08     = mem_readd(b + 0x215D08u);
	const uint32_t port    = mem_readw(b + 0x1417B4u);
	const uint32_t pce8    = mem_readd(b + 0x215CE8u);
	const uint32_t rb0     = mem_readw(0xF940u);
	const uint32_t rb2     = mem_readw(0xF942u);
	const uint32_t rb4     = mem_readw(0xF944u);
	const uint32_t rb6     = mem_readw(0xF946u);
	const uint32_t rb8     = mem_readw(0xF948u);
	const uint32_t rba     = mem_readw(0xF94Au);
	const uint32_t rbc     = mem_readb(0xF94Cu);
	const uint32_t c0      = cached ? mem_readw(cached + 0x00u) : 0u;
	const uint32_t c2      = cached ? mem_readw(cached + 0x02u) : 0u;
	const uint32_t c4      = cached ? mem_readw(cached + 0x04u) : 0u;
	const uint32_t c6      = cached ? mem_readw(cached + 0x06u) : 0u;
	const uint32_t wptr   = mem_readd(b + 0x1AC264u);
	uint32_t v_ebp        = 0u;
	uint32_t v_ret        = 0u;
	uint32_t v_obj        = 0u;
	uint32_t v_cont       = 0u;
	uint32_t v_b2         = 0u;
	uint32_t v_cam        = 0u;
	uint32_t v_scale      = 0u;
	uint32_t v_a22b       = 0u;
	uint8_t  v_a288       = 0u;
	uint8_t  v_a289       = 0u;
	int      sc_top       = 0;
	int      sc_rep       = 0;
	int      sc_hb        = 0;
	int      sc_hbt       = 0;
	const uint32_t v_expt = b + 0x10E3Bu;
	if (wptr) {
		v_ebp  = wptr + 0x66u;
		v_ret  = mem_readd(v_ebp + 0x266u);
		v_obj  = mem_readd(wptr);
		v_cont = mem_readd(v_ebp - 0xBAu);
		v_b2   = mem_readd(v_ebp - 0xB2u);
		v_cam  = mem_readd(v_ebp - 0x60Au);
		v_scale = mem_readd(b + 0x13B524u);
		v_a22b  = mem_readd(b + 0x1AC22Bu);
		v_a288  = mem_readb(b + 0x1AC288u);
		v_a289  = mem_readb(b + 0x1AC289u);
		const uint32_t lo = (v_ebp > 0x4800u) ? (v_ebp - 0x4800u)
		                                       : 0u;
		for (uint32_t a = lo; a < v_ebp; a += 4u) {
			const uint32_t v = mem_readd(a);
			if (v == b + 0x455BDu) {
				++sc_rep;
			} else if (v == b + 0x606FCu) {
				++sc_hb;
			} else if (v == b + 0x62ED2u) {
				++sc_hbt;
			} else if (v == v_expt) {
				++sc_top;
			}
		}
	}
	const uint32_t b19f980 = mem_readd(b + 0x19F980u);
	const uint8_t  b1acb60 = mem_readb(b + 0x1ACB60u);

	LOG_MSG("VFX1: PROBE b139804=%02x headset=%08x dev=%08x dev1c=%02x "
	        "dev23=%02x ctl_h=%08x ctl_v=%04x b1aee20=%02x st=%08x live=%04x",
	        b139804,
	        headset,
	        devptr,
	        dev1c,
	        dev23,
	        ctl_h,
	        ctl_v,
	        b1aee20,
	        stptr,
	        live);
	LOG_MSG("VFX1: PROBE2 rptfn=%08x cached=%08x ypr=%04x/%04x/%04x t54=%04x "
	        "t56=%04x pcd0=%08x d08=%08x port=%04x",
	        rptfn,
	        cached,
	        cyaw,
	        cpit,
	        crol,
	        t54,
	        t56,
	        pcd0,
	        d08,
	        port);
	LOG_MSG("VFX1: PROBE3 pce8=%08x raw=%04x/%04x/%04x/%04x/%04x/%04x/%02x "
	        "cw=%04x/%04x/%04x/%04x",
	        pce8,
	        rb0,
	        rb2,
	        rb4,
	        rb6,
	        rb8,
	        rba,
	        rbc,
	        c0,
	        c2,
	        c4,
	        c6);
	LOG_MSG("VFX1: PROBE5 wptr=%08x ebp=%08x ret=%08x expt=%08x match=%d "
	        "obj=%08x cont=%08x b2=%08x cam=%08x top=%d rep=%d hb=%d hbt=%d "
	        "b19f980=%08x b1acb60=%02x a288=%02x a289=%02x a22b=%08x scale=%08x",
	        wptr,
	        v_ebp,
	        v_ret,
	        v_expt,
	        (v_ret == v_expt) ? 1 : 0,
	        v_obj,
	        v_cont,
	        v_b2,
	        v_cam,
	        sc_top,
	        sc_rep,
	        sc_hb,
	        sc_hbt,
	        b19f980,
	        b1acb60,
	        v_a288,
	        v_a289,
	        v_a22b,
	        v_scale);
}

// REPORT: HTD struct (yaw/pitch/roll shorts) then HPD struct (+buttons
// byte), always both in config order (fixed slots: clients walk CONFIG
// for presence but read REPORT positionally, so a den-gated compact
// layout would shift HPD under a disabled HTD). Count = 2.
static int vfx1_drv_report(const uint32_t base)
{
	if (vfx1_logging_enabled()) {
		vfx1_diag_probe_flight();
	}
	uint32_t p = base;
	int yaw = 0, pitch = 0, roll = 0;
	unsigned int btn = 0;
	vfx1_drv_angles(3, yaw, pitch, roll, btn);
	const int16_t htd_yaw   = vfx1_drv_deg_to_fix16(yaw);
	const int16_t htd_pitch = vfx1_drv_deg_to_fix16(pitch);
	const int16_t htd_roll  = vfx1_drv_deg_to_fix16(roll);
	vfx1_drv_put16(p, htd_yaw);
	vfx1_drv_put16(p + 2u, htd_pitch);
	vfx1_drv_put16(p + 4u, htd_roll);
	p += 6u;
	vfx1_drv_angles(4, yaw, pitch, roll, btn);
	const int16_t hpd_yaw   = vfx1_drv_deg_to_fix16(yaw);
	const int16_t hpd_pitch = vfx1_drv_deg_to_fix16(pitch);
	const int16_t hpd_roll  = vfx1_drv_deg_to_fix16(roll);
	vfx1_drv_put16(p, hpd_yaw);
	vfx1_drv_put16(p + 2u, hpd_pitch);
	vfx1_drv_put16(p + 4u, hpd_roll);
	mem_writeb(p + 6u, static_cast<uint8_t>(btn & 0xFFu));
	// DIAG (temporary): prove the tracker values handed to the guest.
	if (vfx1_logging_enabled()) {
		LOG_MSG("VFX1: REPORT htd=%d/%d/%d hpd=%d/%d/%d btn=%u base=%08x",
		        htd_yaw,
		        htd_pitch,
		        htd_roll,
		        hpd_yaw,
		        hpd_pitch,
		        hpd_roll,
		        btn & 0xFFu,
		        base);
	}
	return 2;
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
		// VFX1.COM's INT33 dispatcher covers only 03/04/05/09/
		// 0D/0E/12/10/100/101/10D-10F/153-155/13/3A and chains
		// everything else (7F SENSE, 70 GET_DATA, 71
		// SET_DEVICE_DATA, 93 READ93) to the previous vector -
		// the mouse driver, which routes it here. Serve those
		// chained calls while the TSR's resident code is locked,
		// so games still get detection/tracking with
		// vfx1_driver off; keep the gate when no TSR is loaded.
		const bool tsr_owns_int33 =
		        vfx1_tsr_locked && vfx1_tsr_code_seg != 0u;
		if (!tsr_owns_int33) {
			// One-shot: INT33 VFX-1 call with the driver off
			// (falls through to the mouse driver).
			static bool noted = false;
			if (!noted && vfx1_logging_enabled()) {
				noted = true;
				LOG_MSG("VFX1: INT33 mask=%02x fn=%02x with vfx1_driver off (enable it, no TSR)",
				        mask,
				        fn);
			}
			return false;
		}
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
	case 0x7F: { // SENSE_DRIVER_STATUS: the mask scan every client
		     // starts with (VFX1REGS SenseStatus: AX in =
		     // (mask<<8)|0x7F, AX out must be 0x7F00|mask, nibbles
		     // swapped, NOT echoed). Without this no mask ever
		     // matches and clients report "No Sense devices".
		reg_ax = static_cast<uint16_t>(0x7F00u | mask);
		break;
	}
	case 0x03: { // GET_CONFIGURATION: ES:DX buffer in, CX = total
		     // chain BYTES out (225, NOT a packet count: clients walk
		     // `i < CX` stepping by psiz), AX = error out.
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
	case 0x93: { // Tracker fast-read (Descent-vfxread private fn,
		     // VFX1.COM extension): ES:DX = caller buffer (BX = device
		     // 1 on HMD reads, SI = 3 for HTD class). Served exactly
		     // like REPORT (HTD 6B + HPD 7B, CX = packets, AX = err):
		     // Descent validates the call, then polls it per frame
		     // (SI=3/HTD reads observed live). Wrong guess here is
		     // harmless (client ignores the data, as today).
		const uint32_t base =
		        SegPhys(es) + static_cast<uint32_t>(reg_dx);
		const int cnt = vfx1_drv_report(base);
		reg_cx        = static_cast<uint16_t>(cnt);
		reg_ax        = 0;
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: INT33 READ93 count=%d", cnt);
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
		const int yaw16   = vfx1_drv_deg_to_fix16(yaw);
		const int pitch16 = vfx1_drv_deg_to_fix16(pitch);
		const int roll16  = vfx1_drv_deg_to_fix16(roll);
		reg_bx = static_cast<uint16_t>(yaw16);
		reg_cx = static_cast<uint16_t>(pitch16);
		reg_dx = static_cast<uint16_t>(roll16);
		reg_ax = static_cast<uint16_t>(btn & 0xFFu); // err 0
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: INT33 GET_DATA cls=%d yaw=%d pitch=%d roll=%d (deg %d/%d/%d) btn=%u",
			        cls,
			        yaw16,
			        pitch16,
			        roll16,
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
	case 0x90: { // SET_CALLBACK: ES:DX = client handler, BX = client
		     // data seg (VRMOUSE registers its own INT33 hook address
		     // here, then pulls REPORT itself on every invocation, so
		     // the push path is notification-only). Returns the
		     // PREVIOUS registration in ES:DX (0:0 when none), which
		     // VRMOUSE saves to restore on unload (passing 0:0 back
		     // clears us). Starts the data-ready push tick.
		const uint16_t new_seg  = SegValue(es);
		const uint16_t new_off  = reg_dx;
		const uint16_t new_data = reg_bx;
		SegSet16(es, vfx1_drv_cb_seg);
		reg_dx = vfx1_drv_cb_off;
		vfx1_drv_cb_seg  = new_seg;
		vfx1_drv_cb_off  = new_off;
		vfx1_drv_cb_data = new_data;
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: INT33 SET_CALLBACK %04x:%04x data=%04x",
			        vfx1_drv_cb_seg,
			        vfx1_drv_cb_off,
			        vfx1_drv_cb_data);
		}
		if (new_seg != 0u || new_off != 0u) {
			vfx1_cb_start();
		}
		reg_ax = 0;
		break;
	}
	case 0x71: { // SET_DEVICE_DATA (SenseSetVideo): BX = 1|(cls<<8),
		     // CX = mode. Only the HMD (cls=2/DCHMS) is a display.
		     // Mirror the TSR: store the mode and return AX=0 for
		     // modes 0-2; reject modes > 2 with AX=7. Descent 2's
		     // VFX-1 probe is exactly cls=2/mode=2, and it disables
		     // VFX-1 support ("version 2.0 or later drivers") unless
		     // AX=0 comes back, so the default (AX echoed) fails it.
		const int cls = (reg_bx >> 8) & 0xFF;
		if (cls == 2) {
			if (reg_cx > 2) {
				reg_ax = 7;
			} else {
				vfx1_drv_video_mode =
				        static_cast<uint8_t>(reg_cx);
				reg_ax = 0;
			}
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: INT33 SET_VIDEO cls=%d mode=%u -> ax=%04x",
				        cls,
				        reg_cx,
				        reg_ax);
			}
		} else {
			reg_ax = 0;
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: INT33 SET_DEVICE_DATA cls=%d mode=%u accepted",
				        cls,
				        reg_cx);
			}
		}
		break;
	}
	case 0x80: { // SET_VIDEO_PAGE: BX = 1|(cls<<8), CX = page. Only
		     // valid on the HMD (cls=2) in a multi-page stereo mode.
		     // Accept, store, AX=0.
		const int cls = (reg_bx >> 8) & 0xFF;
		if (cls == 2) {
			vfx1_drv_video_page = static_cast<uint8_t>(reg_cx);
		}
		reg_ax = 0;
		if (vfx1_logging_enabled()) {
			LOG_MSG("VFX1: INT33 SET_VIDEO_PAGE cls=%d page=%u",
			        cls,
			        reg_cx);
		}
		break;
	}
	default:
		// TEST_DEVICES / CONTINUE / GET_DATA_SIZES / SET_DATA_SIZE
		// and friends: accepted silently for now (TODO).
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
	const int irq = vfx1_irq();
	const int pic = (irq < 8) ? 0 : 1;
	const bool slave_was_masked =
	        (PIC_GetIMR(pic) & (1u << (irq & 7))) != 0;
	const bool cascade_was_masked =
	        (irq >= 8) && ((PIC_GetIMR(0) & (1u << 2)) != 0);
	vfx1_unmask_irq_line();
	// If a mask was blocking the line, the slave's IRR bit may already
	// be set from a previous raise that never reached the CPU. A plain
	// re-activate is then a no-op; lower both the slave and the cascade
	// first so the unmasked edge actually vectors.
	if (slave_was_masked || cascade_was_masked) {
		PIC_DeActivateIRQ(static_cast<uint8_t>(irq));
		if (irq >= 8) {
			PIC_DeActivateIRQ(2);
		}
		if (vfx1_logging_enabled()) {
			static uint32_t last_ms = 0;
			if (PIC_Ticks - last_ms >= 1000u) {
				LOG_MSG("VFX1: VIP IRQ %d was masked (slave=%d cascade=%d); re-pulsed after unmask",
				        irq,
				        slave_was_masked ? 1 : 0,
				        cascade_was_masked ? 1 : 0);
				last_ms = PIC_Ticks;
			}
		}
	}
	if (vfx1_logging_full()) {
		LOG_MSG("VFX1: VIP raise IRQ %d base%d",
		        irq,
		        base);
	}
	++vfx1_raise_no_eoi;
	// FP9: repeated raises with no EOI means the guest ISR never runs.
	if (vfx1_raise_no_eoi == 10 && vfx1_logging_enabled()) {
		LOG_MSG("VFX1: VIP IRQ %d raised 10x with no PIC EOI (guest ISR may not be servicing it)",
		        irq);
	}
	PIC_ActivateIRQ(static_cast<uint8_t>(irq));
}

void VFX1_NotifyEOI(const int irq)
{
	// Only our own line matters: timer (and other) EOIs must not
	// wipe a pending VIP edge (that starved every ISR forever).
	if (irq != vfx1_irq()) {
		return;
	}
	vfx1_raise_no_eoi = 0;
	// IRQCFG single-shot test interrupt (SETVFX1's exact-1 IRQ
	// test): deliver exactly one run. Re-asserting on a pending
	// backlog here would storm the test ISR (it never drains the
	// RX queue) and starve the guest -- observed as an endless
	// `IN +0x0` loop at the ISR.
	if (vfx1_irq_test_pulse) {
		vfx1_irq_test_pulse = false;
		PIC_DeActivateIRQ(static_cast<uint8_t>(vfx1_irq()));
		return;
	}
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
	if (offset == 0xA) {
		// IRQCFG test-mode write fires the card's single test
		// interrupt: SETVFX1's IRQ test hooks the vector, writes
		// IRQCFG (modes 1 then 0, combined byte = (irq & 0x7F) |
		// (mode << 4)) and waits for exactly one ISR run that only
		// EOI's and bumps a flag. Only the mode-1 (bit 4) write
		// edges: with a raise on both, the flag would land on 2 and
		// the exact-1 verdict would fail. Deliver it as a strict
		// single shot (VFX1_NotifyEOI): the test ISR never drains
		// the RX queue, so the level-style EOI re-raise would storm
		// that non-draining handler and starve the guest.
		if ((value & 0x10u) != 0u) {
			vfx1_irq_test_pulse = true;
			vfx1_raise_irq(base);
		} else {
			vfx1_irq_test_pulse = false;
		}
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
				        (vfx1_bus[base].reporting ||
				         vfx1_track_active());
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
			case 0x0d: value = static_cast<uint8_t>(vfx1_hwv_low()); break;
			case 0x0e: value = static_cast<uint8_t>(vfx1_hwv_hi()); break;
			case 0x4: value = vfx1_dfout_data[base]; break;
			case 0x0:
			case 0x8:
			case 0x9:
			case 0xB:
			case 0xF:
				// SETVFX1 flag-poll site: its sequencer spins on
				// [0xA2E+idx] reading +0x0 here (IP 0x0145,
				// segment varies with the load address). Timestamp
				// it and remember the code segment so the flag
				// mirror in vfx1_tsr_tick can find its data seg.
				if (offset == 0x0 && reg_ip == 0x0145u) {
					const uint16_t poll_cs = SegValue(cs);
					if (poll_cs >= 0x1000u &&
					    poll_cs < 0xF000u) {
						vfx1_setvfx1_cs  = poll_cs;
						vfx1_flagpoll_ms = PIC_Ticks;
					}
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
					        !vfx1_bus[base].armed &&
					        (vfx1_bus[base].reporting ||
					         vfx1_track_active());
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
						// +abempoll: during pre-reporting
						// enumeration (stage < 2) an
						// unarmed, non-reporting read
						// reports data-ready straight from
						// the live queue, so the driver's
						// orchestrator poll (VFX1.COM
						// spins on +0x8 bit5 before it
						// sends IDREQ) can advance. It
						// clears the instant the queue
						// drains, so drain-until-dry loops
						// still terminate.
						const bool enum_ready =
						        !vfx1_bus[base].armed &&
						        !vfx1_bus[base]
						                 .reporting &&
						        vfx1_bus[base].stage < 2;
						if ((framed || stream_ready ||
						     enum_ready) &&
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
		const bool cascade = (pic == 0 && bit == 2 && vip_irq >= 8);
		if (cascade) {
			if (vfx1_logging_enabled()) {
				LOG_MSG("VFX1: VIP cascade IRQ 2 %s by guest (IRQ %d delivery %s)",
				        in_use ? "unmasked" : "MASKED",
				        vip_irq,
				        in_use ? "open" : "blocked");
			}
		} else if (irq == vip_irq && !in_use) {
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
