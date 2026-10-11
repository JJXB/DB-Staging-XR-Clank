// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// CyberShades stereo input mode (see cybershades.md, sections 3-4):
// frame-sequential decode for Chinon CyberShades titles (TEST.EXE,
// Descent with CyberShades support). Each captured DOS frame carries
// one full eye view; it drives that eye's buffer while the other eye
// holds its last frame.

#include "gui/cyshades.h"

#include <string>

#include "config/setup.h"
#include "gui/xr.h"
#include "hardware/port.h"
#include "misc/support.h"

void CYSHADES_SubmitFrame(std::vector<uint32_t>& eye0,
                          std::vector<uint32_t>& eye1,
                          const std::vector<uint32_t>& frame)
{
	if (frame.empty() || eye0.size() != frame.size() ||
	    eye1.size() != frame.size()) {
		return;
	}
	static uint64_t cy_parity = 0;
	static uint8_t cy_shutter_last = 0x00;
	static int cy_shutter_source   = 0;
	static bool cy_shutter_seen    = false;

	const auto* section = get_xr_section();
	const std::string shutter_mode =
	        section->GetString("xr_shutter_sync");
	const bool eye_swap = section->GetBool("xr_eye_swap");

	uint8_t shutter     = 0;
	int source          = 0;
	const bool shutter_fresh =
	        STEREO3D_ConsumeCyShutter(shutter, source);
	if (shutter_fresh) {
		cy_shutter_last   = shutter;
		cy_shutter_source = source;
		cy_shutter_seen   = true;
	}
	const bool use_shutter = (shutter_mode == "lpt" && cy_shutter_seen) ||
	                         (shutter_mode != "parity" && shutter_fresh);
	int layer = 0;
	if (use_shutter) {
		static bool shutter_logged = false;
		if (!shutter_logged) {
			shutter_logged = true;
			LOG_MSG("OpenXR: CyberShades shutter sync active "
			        "(LPT bit 2 / COM RTS drive eyes)");
		}
		// CyberShades polarity (cybershades.md section 3): the LPT
		// path shows VSleft (0xFF, data bit D2 set) with the LEFT
		// view and VSright (0xFB, D2 clear) with the RIGHT view --
		// inverted relative to the generic 'alternating' mode. The
		// COM path shows SER_D_LEFT (0x01) with left and SER_D_RIGHT
		// (0x03, RTS set) with right, so RTS set reads as RIGHT.
		const bool left = (cy_shutter_source == 1)
		                          ? ((cy_shutter_last & 0x02) == 0)
		                          : ((cy_shutter_last & 0x04) != 0);
		layer = ((left != eye_swap) ? 0 : 1);
	} else {
		layer = static_cast<int>((cy_parity ^ (eye_swap ? 1u : 0u)) &
		                         1u);
		++cy_parity;
	}
	if (layer == 0) {
		eye0 = frame;
	} else {
		eye1 = frame;
	}
}
