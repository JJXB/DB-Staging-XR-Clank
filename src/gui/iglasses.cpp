// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Virtual i-O i-glasses! stereo input mode (see IGLASSES.md, sections
// 8-9): scanline-interleaved decode for i-glasses! titles. The incoming
// signal weaves the two eye views into alternate scanlines (first
// scanline = left eye at the 15.75-31.5 kHz line rate), so each eye
// gets a bob-deinterlaced half built from a single field. Each eye is
// always self-consistent (no within-eye doubling); static content
// coincides in both eyes, motion shows the field offset exactly as the
// headset panels would. No phase to track, so nothing can drift.

#include "gui/iglasses.h"

#include <cstring>

#include "config/setup.h"
#include "gui/xr.h"

void IGLASSES_SubmitFrame(std::vector<uint32_t>& eye0,
                          std::vector<uint32_t>& eye1,
                          const std::vector<uint32_t>& frame, const int w,
                          const int h)
{
	if (w <= 0 || h <= 0 || frame.empty()) {
		return;
	}
	const size_t px = static_cast<size_t>(w) * static_cast<size_t>(h);
	if (frame.size() != px || eye0.size() != px || eye1.size() != px) {
		return;
	}
	const bool eye_swap = get_xr_section()->GetBool("xr_eye_swap");
	const int fi = eye_swap ? 1 : 0;
	const int si = 1 - fi;
	auto& dst_f  = (fi == 0) ? eye0 : eye1;
	auto& dst_s  = (si == 0) ? eye0 : eye1;
	const auto row_bytes = static_cast<size_t>(w) * sizeof(uint32_t);
	for (int y = 0; y < h; ++y) {
		int src_fy = y & ~1; // even field row (first scanline = first eye)
		int src_sy = y | 1;  // odd field row
		if (src_sy >= h) {
			src_sy = y;
		}
		std::memcpy(dst_f.data() + static_cast<size_t>(y) *
		                                  static_cast<size_t>(w),
		            frame.data() + static_cast<size_t>(src_fy) *
		                                   static_cast<size_t>(w),
		            row_bytes);
		std::memcpy(dst_s.data() + static_cast<size_t>(y) *
		                                  static_cast<size_t>(w),
		            frame.data() + static_cast<size_t>(src_sy) *
		                                   static_cast<size_t>(w),
		            row_bytes);
	}
}
