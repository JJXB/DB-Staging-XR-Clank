// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Head-tracker wire protocols (see trackers.h, VMaxxCyMaxx.md section
// 6, IGLASSES.md sections 3-5).

#include "gui/trackers.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// M_PI is not provided by MSVC's <cmath> without _USE_MATH_DEFINES;
// use a local constant instead.
static constexpr float kPi = 3.14159265358979323846f;

int TRACKER_CyberMaxx_FormatLine(float yaw_deg, float pitch_deg,
                                 float roll_deg, char* out,
                                 size_t out_size)
{
	if (!out || out_size < 18) {
		return 0;
	}
	// Wrap yaw into [0, 360) like the compass-style sensor readout.
	float yaw = fmodf(yaw_deg, 360.0f);
	if (yaw < 0.0f) {
		yaw += 360.0f;
	}
	// Clamp pitch/roll into the printable field (sensor range is
	// about +/-45 deg; the game parses wider without harm).
	if (pitch_deg > 99.9f) {
		pitch_deg = 99.9f;
	} else if (pitch_deg < -99.9f) {
		pitch_deg = -99.9f;
	}
	if (roll_deg > 99.9f) {
		roll_deg = 99.9f;
	} else if (roll_deg < -99.9f) {
		roll_deg = -99.9f;
	}
	// %05.1f: "084.7"; %+05.1f: "-03.5" / "+04.9" (matches the
	// `Y084.7P-03.5R+04.9` example in VMaxxCyMaxx.md section 6).
	const int n = snprintf(out, out_size, "Y%05.1fP%+05.1fR%+05.1f\r",
	                       static_cast<double>(yaw),
	                       static_cast<double>(pitch_deg),
	                       static_cast<double>(roll_deg));
	if (n < 0 || static_cast<size_t>(n) >= out_size) {
		return 0;
	}
	return n;
}

void TRACKER_Iglasses_Reset(TrackerIglassesState& st)
{
	st.data_mode  = 1;
	st.send_mode  = 'P';
	st.format     = 'B';
	// Filters stay where the Tracker Manager left them (the real
	// device remembers its last mode across power-off).
}

// 60-byte `!V` string (IGLASSES.md section 3, SDK TRACKER_VER_LEN):
// `M<16>P<16>T<8>Hxxx.xxxFxxx.xxx` + `O`. Hardware/firmware 001.003
// (first release with the Euler yaw-sign fix, so hosts need no remap).
static const char kIglassesVersion[] =
        "MVirtual i-O.    P00000001499.000,Ttracker H001.003F001.003O";

int TRACKER_Iglasses_HandleCommand(TrackerIglassesState& st, const char* cmd,
                                   size_t cmd_len, char* out,
                                   size_t out_size)
{
	if (!cmd || !out || out_size < 2) {
		return -1;
	}
	// Bare "S" (no CR): polled-data request / stream start. No reply;
	// the caller emits a data packet (BuildCooked/BuildEuler).
	if (cmd_len == 1 && cmd[0] == 'S') {
		return 0;
	}
	// Bare "!" + CR: stop a continuous stream (resetTracker recovery).
	if (cmd_len == 1 && cmd[0] == '!') {
		out[0] = 'O';
		return 1;
	}
	if (cmd_len == 0 || cmd[0] != '!') {
		out[0] = 'E';
		return 1;
	}
	// "!R": reset to cooked/polled/binary.
	if (cmd_len == 2 && cmd[1] == 'R') {
		TRACKER_Iglasses_Reset(st);
		out[0] = 'O';
		return 1;
	}
	// "!V": version string (60 bytes incl. trailing O).
	if (cmd_len == 2 && cmd[1] == 'V') {
		const size_t vlen = sizeof(kIglassesVersion) - 1;
		if (out_size < vlen + 1) {
			return -1;
		}
		memcpy(out, kIglassesVersion, vlen);
		return static_cast<int>(vlen);
	}
	// "!M....": mode select. Minimal grammar for the commands real
	// apps send (`!M1,P,B`, `!M2,P,B`, `!M1,P,B,f,f`,
	// `!M3,...` mouse): data digit, ',', send char, ',', format char.
	// Anything else (incl. memory R/W `!ZR`/`!ZW`, calibration `!MC`)
	// replies 'E' — apps treat those as optional (DEVKIT1 notes).
	if (cmd_len >= 6 && cmd[1] == 'M') {
		const char d = cmd[2];
		if (d >= '0' && d <= '3' && cmd[3] == ',' &&
		    (cmd[4] == 'P' || cmd[4] == 'C' || cmd[4] == '0' ||
		     cmd[4] == '1') &&
		    cmd[5] == ',') {
			char f = (cmd_len > 6) ? cmd[6] : 'B';
			if (f != 'A' && f != 'B') {
				out[0] = 'E';
				return 1;
			}
			st.data_mode = d - '0';
			st.send_mode = cmd[4];
			st.format    = f;
			// Optional ",magfilt,grvfilter[,mouse...]" tail: accept
			// and ignore (filters stay host-managed).
			out[0] = 'O';
			return 1;
		}
		out[0] = 'E';
		return 1;
	}
	out[0] = 'E';
	return 1;
}

// Quantize degrees to the SDK unit (+16384 = 180 deg), wrapping yaw to
// [-180, 180] first (firmware 001.003 sign convention).
static int16_t quantize_angle(float deg)
{
	while (deg > 180.0f) {
		deg -= 360.0f;
	}
	while (deg < -180.0f) {
		deg += 360.0f;
	}
	long v = lroundf(deg / 180.0f * 16384.0f);
	if (v > 16384) {
		v = 16384;
	} else if (v < -16384) {
		v = -16384;
	}
	return static_cast<int16_t>(v);
}

int TRACKER_Iglasses_BuildCooked(float yaw_deg, float pitch_deg,
                                 float roll_deg, uint8_t* out,
                                 size_t out_size)
{
	if (!out || out_size < 12) {
		return 0;
	}
	// Synthesize the magnetometer triplet from yaw alone (unit vector
	// in the X/Z plane at full scale, matching the cooked range of
	// about +/-16384 over the Earth's field). Tilt quantizes directly
	// (+16384 = 180 deg), so host rawToEulerInt recovers the input.
	const float rad = yaw_deg * kPi / 180.0f;
	const int16_t mx = static_cast<int16_t>(
	        lroundf(cosf(rad) * 16384.0f));
	const int16_t mz = static_cast<int16_t>(
	        lroundf(sinf(rad) * 16384.0f));
	const int16_t my = 0;
	const int16_t p  = quantize_angle(pitch_deg);
	const int16_t r  = quantize_angle(roll_deg);
	out[0]  = 255;
	out[1]  = static_cast<uint8_t>((mx >> 8) & 0xFF);
	out[2]  = static_cast<uint8_t>(mx & 0xFF);
	out[3]  = static_cast<uint8_t>((my >> 8) & 0xFF);
	out[4]  = static_cast<uint8_t>(my & 0xFF);
	out[5]  = static_cast<uint8_t>((mz >> 8) & 0xFF);
	out[6]  = static_cast<uint8_t>(mz & 0xFF);
	out[7]  = static_cast<uint8_t>((p >> 8) & 0xFF);
	out[8]  = static_cast<uint8_t>(p & 0xFF);
	out[9]  = static_cast<uint8_t>((r >> 8) & 0xFF);
	out[10] = static_cast<uint8_t>(r & 0xFF);
	uint8_t sum = 0;
	for (int i = 0; i < 11; ++i) {
		sum += out[i];
	}
	out[11] = sum;
	return 12;
}

int TRACKER_Iglasses_BuildEuler(float yaw_deg, float pitch_deg,
                                float roll_deg, uint8_t* out,
                                size_t out_size)
{
	if (!out || out_size < 8) {
		return 0;
	}
	const int16_t y = quantize_angle(yaw_deg);
	const int16_t p = quantize_angle(pitch_deg);
	const int16_t r = quantize_angle(roll_deg);
	out[0] = 255;
	out[1] = static_cast<uint8_t>((y >> 8) & 0xFF);
	out[2] = static_cast<uint8_t>(y & 0xFF);
	out[3] = static_cast<uint8_t>((p >> 8) & 0xFF);
	out[4] = static_cast<uint8_t>(p & 0xFF);
	out[5] = static_cast<uint8_t>((r >> 8) & 0xFF);
	out[6] = static_cast<uint8_t>(r & 0xFF);
	uint8_t sum = 0;
	for (int i = 0; i < 7; ++i) {
		sum += out[i];
	}
	out[7] = sum;
	return 8;
}
