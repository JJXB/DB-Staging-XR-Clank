// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Head-tracker wire protocols (see VMaxxCyMaxx.md section 6 and
// IGLASSES.md sections 3-5): pure protocol core with no DOSBox
// dependencies, so the serial-device layer (or tests) can drive it
// without pulling in the XR session.

#ifndef DOSBOX_TRACKERS_H
#define DOSBOX_TRACKERS_H

#include <cstddef>
#include <cstdint>

// VictorMaxx CyberMaxx tracker (VMaxxCyMaxx.md section 6): continuous
// ASCII lines at 19.2 kbaud, `YyawPpitchRroll` with one decimal, e.g.
// `Y084.7P-03.5R+04.9`. Yaw wraps to [0, 360); pitch/roll are signed
// (sensor range about +/-45 deg, field wider). Formats one line
// terminated with '\r' into out (must hold >= 18 bytes); returns the
// line length, or 0 when out is null/too small.
int TRACKER_CyberMaxx_FormatLine(float yaw_deg, float pitch_deg,
                                 float roll_deg, char* out,
                                 size_t out_size);

// Virtual i-O i-glasses! tracker (IGLASSES.md sections 3-5, vendor
// SDK SRC/TRACKER1/TRACKER.C): command-driven polled/binary device.
// Data modes 0 = raw, 1 = cooked (VRS's `!M1,P,B`), 2 = Euler
// (micro-Euler 8-byte); format 'A' is ASCII-hex debug, 'B' binary.
// Send 'P' = polled (`S` per packet), 'C' = continuous stream.
struct TrackerIglassesState {
	int data_mode = 1;   // 0 raw, 1 cooked, 2 euler
	char send_mode = 'P'; // 'P' polled, 'C' continuous
	char format = 'B';    // 'B' binary, 'A' ascii-hex
	int mag_filter = 3;
	int grv_filter = 3;
};

// Resets to the `!R` default: cooked, polled, binary (filters kept).
void TRACKER_Iglasses_Reset(TrackerIglassesState& st);

// Handles one guest command line (bytes up to but excluding the
// terminating '\r', except bare "S" which has no '\r'). Writes the
// device reply into out (must hold >= 64 bytes; `!V` needs 60):
// "O"/"E" for most commands, the 60-byte version string for `!V`,
// empty (0) for `S` — the caller then emits a data packet below.
// Returns reply length, or -1 on unknown command ('E' written).
int TRACKER_Iglasses_HandleCommand(TrackerIglassesState& st, const char* cmd,
                                   size_t cmd_len, char* out,
                                   size_t out_size);

// Builds one binary data packet from host orientation. Degrees use the
// SDK convention (+yaw = left turn, +pitch = up, +roll = left tilt;
// yaw wraps to [-180, 180] before quantizing; +16384 = 180 deg).
// Cooked packet (mode 1, VRS): 12 bytes,
// [255][XH][XL][YH][YL][ZH][ZL][PH][PL][RH][RL][sum 0..10].
// The magnetometer XYZ triple is synthesized from yaw (unit XZ vector
// scaled to +/-16384); pitch/roll quantize directly. Euler packet
// (mode 2): 8 bytes, [255][YH][YL][PH][PL][RH][RL][sum 0..6].
// Returns packet length (12/8), or 0 when out is null/too small.
int TRACKER_Iglasses_BuildCooked(float yaw_deg, float pitch_deg,
                                 float roll_deg, uint8_t* out,
                                 size_t out_size);
int TRACKER_Iglasses_BuildEuler(float yaw_deg, float pitch_deg,
                                float roll_deg, uint8_t* out,
                                size_t out_size);

#endif
