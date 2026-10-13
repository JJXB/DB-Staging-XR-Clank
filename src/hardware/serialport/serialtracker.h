// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_SERIALTRACKER_H
#define DOSBOX_SERIALTRACKER_H

#include "serialport.h"

#include <deque>
#include <string>

#include "gui/trackers.h"

enum class TrackerMode {
	Iglasses,
	CyberMaxx,
};

class CSerialTracker final : public CSerial {
public:
	CSerialTracker(const uint8_t port_idx, CommandLine* cmd,
	               const TrackerMode tracker_mode);
	~CSerialTracker() override;

	void setRTSDTR(bool rts, bool dtr) override;
	void setRTS(bool val) override;
	void setDTR(bool val) override;

	void updatePortConfig(uint16_t divider, uint8_t lcr) override;
	void updateMSR() override;
	void transmitByte(uint8_t val, bool first) override;
	void setBreak(bool value) override;
	void handleUpperEvent(uint16_t type) override;

private:
	void queueBytes(const uint8_t* data, size_t len);
	void queueString(const char* data, size_t len);
	void getLiveYpr(float& yaw_deg, float& pitch_deg,
	                float& roll_deg);
	void refillCyberMaxx();
	void handleCyberMaxxGuestByte(uint8_t val);
	void emitCyberMaxxBinary();
	void refillIglassesContinuous();
	void handleIglassesGuestByte(uint8_t val);
	void handleIglassesCommand(const std::string& cmd);
	void emitIglassesPacket();
	void scheduleRx(float delay_ms);
	void setReady(bool ready);

	const TrackerMode mode = TrackerMode::Iglasses;
	const int port_num     = 0;

	TrackerIglassesState ig_state = {};
	std::string ig_cmd            = {};
	bool ig_streaming             = false; // continuous ('C') stream active
	// CyberMaxx binary poll mode (Descent -maxxtrack): set by any
	// A/C/F/G/S command byte; while set, the ASCII stream stays off
	// and each 'S' emits one 6-byte packet. DTR drop clears it.
	bool cm_binary = false;
	// One-shot diagnostics: guest line programming + first TX bytes,
	// so a log shows whether the guest opened the port and talked.
	uint16_t last_divider = 0;
	uint8_t last_lcr      = 0;
	unsigned int tx_logged = 0;
	std::deque<uint8_t> out_q     = {};

	bool dtr_on = false;
	bool rts_on = false;

	uint32_t synth_n = 0;
};

#endif // DOSBOX_SERIALTRACKER_H
