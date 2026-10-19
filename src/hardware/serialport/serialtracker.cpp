// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Serial head-tracker devices (see IGLASSES.md sections 3-5 and
// VMaxxCyMaxx.md section 6): protocol core lives in gui/trackers.h,
// this layer is the CSerial hookup feeding live XR pose (synthetic
// sweep fallback) to DOS games such as VR Slingshot (VRS.EXE).

#include "serialtracker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "shell/command_line.h"

#if C_OPENXR
bool openxr_head_quat(float& x, float& y, float& z, float& w);
#endif

static constexpr float kPiF = 3.14159265358979323846f;

CSerialTracker::CSerialTracker(const uint8_t port_idx, CommandLine* cmd,
                               const TrackerMode tracker_mode)
        : CSerial(port_idx, cmd),
          mode(tracker_mode),
          port_num(port_idx + 1)
{
	TRACKER_Iglasses_Reset(ig_state);
	ig_cmd.reserve(64);

	CSerial::Init_Registers();
	setReady(true);
	if (mode == TrackerMode::CyberMaxx) {
		LOG_MSG("TRACKER (COM%d): Using CyberMaxx tracker "
		        "(ASCII Y/P/R stream, binary poll on A/C/F/G/S)",
		        port_num);
		// Continuous stream: kick the RX drip.
		scheduleRx(bytetime > 0.0f ? bytetime : 1.0f);
	} else {
		LOG_MSG("TRACKER (COM%d): Using i-glasses! command/packet device",
		        port_num);
	}

	InstallationSuccessful = true;
}

CSerialTracker::~CSerialTracker()
{
	removeEvent(SERIAL_TX_EVENT);
	removeEvent(SERIAL_THR_EVENT);
	removeEvent(SERIAL_RX_EVENT);
	LOG_MSG("TRACKER (COM%d): Disconnected", port_num);
}

void CSerialTracker::setReady(const bool ready)
{
	setRI(false);
	setDSR(ready);
	setCD(ready);
	setCTS(ready);
}

void CSerialTracker::scheduleRx(const float delay_ms)
{
	float d = delay_ms;
	if (!(d > 0.0f)) {
		d = 1.0f;
	}
	setEvent(SERIAL_RX_EVENT, d);
}

void CSerialTracker::queueBytes(const uint8_t* data, const size_t len)
{
	if (!data) {
		return;
	}
	for (size_t i = 0; i < len; ++i) {
		if (out_q.size() < 512) {
			out_q.push_back(data[i]);
		} else {
			break;
		}
	}
}

void CSerialTracker::queueString(const char* data, const size_t len)
{
	queueBytes(reinterpret_cast<const uint8_t*>(data), len);
}

void CSerialTracker::getLiveYpr(float& yaw_deg, float& pitch_deg,
                                float& roll_deg)
{
#if C_OPENXR
	float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
	if (openxr_head_quat(qx, qy, qz, qw)) {
		const float t = std::max(-1.0f,
		                         std::min(1.0f,
		                                  2.0f * (qw * qx - qy * qz)));
		const float pitch = asinf(t);
		const float yaw = atan2f(2.0f * (qw * qy + qx * qz),
		                         1.0f - 2.0f * (qx * qx + qy * qy));
		float roll = atan2f(2.0f * (qw * qz + qx * qy),
		                    1.0f - 2.0f * (qx * qx + qz * qz));
		roll = std::max(-1.5707964f, std::min(1.5707964f, roll));
		yaw_deg   = yaw * 180.0f / kPiF;
		pitch_deg = pitch * 180.0f / kPiF;
		roll_deg  = roll * 180.0f / kPiF;
		return;
	}
#endif
	// Synthetic sweep fallback (no HMD pose): slow yaw circle with
	// gentle pitch/roll so games always see motion.
	const float n = static_cast<float>(synth_n);
	yaw_deg   = fmodf(n * 1.0f, 360.0f);
	pitch_deg = 8.0f * sinf(n * 0.05f);
	roll_deg  = 8.0f * cosf(n * 0.03f);
}

void CSerialTracker::refillCyberMaxx()
{
	float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
	getLiveYpr(yaw, pitch, roll);
	++synth_n;
	char line[32] = {};
	const int n = TRACKER_CyberMaxx_FormatLine(yaw, pitch, roll, line,
	                                           sizeof(line));
	if (n > 0) {
		queueString(line, static_cast<size_t>(n));
	}
}

static int16_t clamp_s16(const long v)
{
	if (v > 32767) {
		return 32767;
	}
	if (v < -32768) {
		return -32768;
	}
	return static_cast<int16_t>(v);
}

// Binary poll packet (Descent MAIN/VICTOR.C:267…275
// victor_read_headset_raw, consumed by MAIN/KCONFIG.C read_head_tracker,
// whose yaw wrap logic expects [0, 65536)): 6 bytes big-endian, yaw as
// UNSIGNED compass angle (65536 units = 360 deg, matching the ASCII
// Y/P/R wrap and the magnetic-heading sensor), pitch/roll centered on
// 16384 (the host computes -(p - 16384)). Emitted on each 'S' once any
// A/C/F/G/S byte selected binary mode.
void CSerialTracker::emitCyberMaxxBinary()
{
	float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
	getLiveYpr(yaw, pitch, roll);
	++synth_n;
	// Descent heads turn the opposite way to the headset's yaw and
	// pitch, while roll matches, so invert yaw/pitch only.
	yaw   = -yaw;
	pitch = -pitch;
	while (yaw >= 360.0f) {
		yaw -= 360.0f;
	}
	while (yaw < 0.0f) {
		yaw += 360.0f;
	}
	const uint16_t y = static_cast<uint16_t>(
	        lroundf(yaw / 360.0f * 65536.0f) & 0xFFFF);
	const int16_t p = clamp_s16(
	        lroundf(16384.0f - pitch / 360.0f * 65536.0f));
	const int16_t r = clamp_s16(
	        lroundf(16384.0f - roll / 360.0f * 65536.0f));
	const uint8_t pkt[6] = {
	        static_cast<uint8_t>((y >> 8) & 0xFF),
	        static_cast<uint8_t>(y & 0xFF),
	        static_cast<uint8_t>((p >> 8) & 0xFF),
	        static_cast<uint8_t>(p & 0xFF),
	        static_cast<uint8_t>((r >> 8) & 0xFF),
	        static_cast<uint8_t>(r & 0xFF),
	};
	queueBytes(pkt, sizeof(pkt));
}

// CyberMaxx guest bytes (Descent MAIN/VICTOR.C:222…229): single chars
// 'A' (absolute), 'C' (high accuracy), 'F' (binary), 'G' (sample on
// request), then 'S' per packet. No replies — the host flushes RX
// after init anyway. Any command byte selects binary poll mode and
// stops the ASCII stream (DTR drop restores it).
void CSerialTracker::handleCyberMaxxGuestByte(const uint8_t val)
{
	switch (val) {
	case 'A':
	case 'C':
	case 'F':
	case 'G':
		if (!cm_binary) {
			cm_binary = true;
			// Drop any pre-switch ASCII bytes: the host
			// flushes RX after init and expects binary.
			out_q.clear();
			LOG_MSG("TRACKER (COM%d): CyberMaxx binary poll mode",
			        port_num);
		}
		return;
	case 'S':
		if (!cm_binary) {
			cm_binary = true;
			LOG_MSG("TRACKER (COM%d): CyberMaxx binary poll mode",
			        port_num);
		}
		emitCyberMaxxBinary();
		scheduleRx(bytetime > 0.0f ? bytetime : 1.0f);
		return;
	default:
		break;
	}
}

void CSerialTracker::emitIglassesPacket()
{
	float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
	getLiveYpr(yaw, pitch, roll);
	++synth_n;

	if (ig_state.data_mode == 2) {
		uint8_t pkt[8] = {};
		const int n = TRACKER_Iglasses_BuildEuler(yaw, pitch, roll,
		                                          pkt, sizeof(pkt));
		if (n <= 0) {
			return;
		}
		if (ig_state.format == 'A') {
			// ASCII-hex debug rendering: "XX " per byte + CR.
			char hex[8 * 3 + 2] = {};
			int pos             = 0;
			for (int i = 0; i < n; ++i) {
				pos += snprintf(hex + pos, sizeof(hex) - pos,
				                "%02X ", pkt[i]);
				if (pos < 0 ||
				    static_cast<size_t>(pos) >= sizeof(hex)) {
					return;
				}
			}
			hex[pos++] = '\r';
			queueString(hex, static_cast<size_t>(pos));
		} else {
			queueBytes(pkt, static_cast<size_t>(n));
		}
		return;
	}
	if (ig_state.data_mode == 3) {
		// Mouse emulation (1200 bps Microsoft packets) is not
		// implemented; tracker games use modes 1/2.
		return;
	}
	// Modes 0 (raw) and 1 (cooked) share the 12-byte layout.
	uint8_t pkt[12] = {};
	const int n = TRACKER_Iglasses_BuildCooked(yaw, pitch, roll, pkt,
	                                           sizeof(pkt));
	if (n <= 0) {
		return;
	}
	if (ig_state.format == 'A') {
		char hex[12 * 3 + 2] = {};
		int pos              = 0;
		for (int i = 0; i < n; ++i) {
			pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ",
			                pkt[i]);
			if (pos < 0 ||
			    static_cast<size_t>(pos) >= sizeof(hex)) {
				return;
			}
		}
		hex[pos++] = '\r';
		queueString(hex, static_cast<size_t>(pos));
	} else {
		queueBytes(pkt, static_cast<size_t>(n));
	}
}

void CSerialTracker::refillIglassesContinuous()
{
	if (!ig_streaming || ig_state.send_mode == 'P') {
		return;
	}
	// Continuous ('C') stream: one packet per drip.
	emitIglassesPacket();
}

void CSerialTracker::handleIglassesCommand(const std::string& cmd)
{
	char reply[64] = {};
	const int rlen = TRACKER_Iglasses_HandleCommand(
	        ig_state, cmd.c_str(), cmd.size(), reply, sizeof(reply));
	if (rlen > 0) {
		queueString(reply, static_cast<size_t>(rlen));
	} else if (rlen == 0) {
		// Bare "S": emit one data packet now; in continuous mode
		// it also (re)starts the stream.
		if (ig_state.send_mode == 'C') {
			ig_streaming = true;
		}
		emitIglassesPacket();
		return;
	}
	// Track streaming state across mode commands.
	if (!cmd.empty() && cmd[0] == '!') {
		if (cmd.size() == 1) {
			// "!" stops a continuous stream.
			ig_streaming = false;
		} else if (cmd.size() >= 2 && cmd[1] == 'R') {
			ig_streaming = false;
		} else if (cmd.size() >= 6 && cmd[1] == 'M') {
			if (ig_state.send_mode == 'C') {
				// Stream starts on the next 'S'; also
				// start now so hosts that never send
				// 'S' still see data.
				ig_streaming = true;
				emitIglassesPacket();
			} else {
				ig_streaming = false;
			}
		}
	}
}

void CSerialTracker::handleIglassesGuestByte(const uint8_t val)
{
	if (val == 'S' && ig_cmd.empty()) {
		// Polled-data request / stream start (no CR).
		handleIglassesCommand("S");
		scheduleRx(bytetime > 0.0f ? bytetime : 1.0f);
		return;
	}
	if (val == '\r') {
		handleIglassesCommand(ig_cmd);
		ig_cmd.clear();
		scheduleRx(bytetime > 0.0f ? bytetime : 1.0f);
		return;
	}
	if (val == '\n' || val == 0) {
		return;
	}
	if (ig_cmd.size() < 60) {
		ig_cmd.push_back(static_cast<char>(val));
	} else {
		// Overflow: reset to the recovery handshake.
		ig_cmd.clear();
	}
}

void CSerialTracker::handleUpperEvent(const uint16_t event_type)
{
	if (event_type == SERIAL_TX_EVENT) {
		ByteTransmitted();
		return;
	}
	if (event_type == SERIAL_THR_EVENT) {
		ByteTransmitting();
		setEvent(SERIAL_TX_EVENT, bytetime);
		return;
	}
	if (event_type != SERIAL_RX_EVENT) {
		return;
	}
	// Drip one queued byte per event (baud-paced).
	if (!out_q.empty() && CanReceiveByte()) {
		receiveByte(out_q.front());
		out_q.pop_front();
	}
	if (!out_q.empty()) {
		scheduleRx(bytetime > 0.0f ? bytetime : 1.0f);
		return;
	}
	if (mode == TrackerMode::CyberMaxx) {
		if (cm_binary) {
			// Binary poll mode: replies were queued by 'S';
			// the drip above reschedules while bytes remain.
			// No auto-refill, no streaming.
			return;
		}
		if (!dtr_on && (getDTR() == false)) {
			// Host powered the device off: stay quiet but
			// keep a slow poll so resume is prompt.
			scheduleRx(50.0f);
			return;
		}
		refillCyberMaxx();
		// 19.2 kbaud line (~19 chars) ~= 10 ms per report.
		scheduleRx(10.0f);
		return;
	}
	// i-glasses!: only drip continuously in 'C' streaming mode;
	// polled replies were already queued by transmitByte.
	if (ig_streaming && ig_state.send_mode == 'C') {
		refillIglassesContinuous();
		scheduleRx(10.0f);
	}
}

void CSerialTracker::updatePortConfig(const uint16_t divider,
                                      const uint8_t lcr)
{
	// Accept any baud/frame the guest selects (i-glasses!: 1200-19200
	// 8N1; CyberMaxx: 19.2 kbaud). Timing follows bytetime. Log guest
	// reprogramming (proves the port was opened and configured).
	if (divider != last_divider || lcr != last_lcr) {
		last_divider = divider;
		last_lcr     = lcr;
		const uint32_t baud = (divider != 0u)
		        ? (115200u / divider)
		        : 115200u;
		LOG_MSG("TRACKER (COM%d): line divider=%u lcr=%02X (~%u baud)",
		        port_num, divider, lcr, baud);
	}
}

void CSerialTracker::updateMSR() {}

void CSerialTracker::transmitByte(const uint8_t val, const bool first)
{
	if (first) {
		setEvent(SERIAL_THR_EVENT, bytetime / 10);
	} else {
		setEvent(SERIAL_TX_EVENT, bytetime);
	}

	// First guest bytes, capped: proves the guest is talking and
	// shows the protocol selection (ACFG/S vs !-commands).
	if (tx_logged < 16) {
		const char c = (val >= 32 && val < 127)
		        ? static_cast<char>(val)
		        : '.';
		LOG_MSG("TRACKER (COM%d): guest TX %02X '%c'", port_num, val,
		        c);
		++tx_logged;
	}

	if (mode == TrackerMode::CyberMaxx) {
		handleCyberMaxxGuestByte(val);
		return;
	}
	handleIglassesGuestByte(val);
}

void CSerialTracker::setBreak(const bool) {}

void CSerialTracker::setRTSDTR(const bool rts, const bool dtr)
{
	setRTS(rts);
	setDTR(dtr);
}

void CSerialTracker::setRTS(const bool val)
{
	rts_on = val;
	// Device is always clear-to-send while powered.
	if (dtr_on || val) {
		setCTS(true);
	}
}

void CSerialTracker::setDTR(const bool val)
{
	const bool was = dtr_on;
	dtr_on         = val;
	if (!was && val) {
		LOG_MSG("TRACKER (COM%d): host opened (DTR on)", port_num);
	}
	if (was && !val) {
		LOG_MSG("TRACKER (COM%d): host closed (DTR off)", port_num);
	}
	if (!val) {
		// Host powered the device off: drop ready lines, reset
		// state, stop any stream.
		setReady(false);
		TRACKER_Iglasses_Reset(ig_state);
		ig_cmd.clear();
		ig_streaming = false;
		cm_binary    = false;
		out_q.clear();
		return;
	}
	setReady(true);
	if (!was && mode == TrackerMode::CyberMaxx) {
		scheduleRx(bytetime > 0.0f ? bytetime : 1.0f);
	}
}
