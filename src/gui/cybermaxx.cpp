// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// VictorMaxx CyberMaxx stereo input mode (see VMaxxCyMaxx.md, sections
// 7-8): scanline-interleaved decode for CyberMaxx titles. The display
// board routes alternate incoming scanlines to the left vs. right TFT
// panel (line N to one panel, line N+1 to the other at the 15.734 kHz
// line rate), so each eye is one half-height field. The fields are
// extracted at half height and the GPU upscales them on upload (same
// path as the generic 'interleaved' mode). The input frame is the
// pre-scaler game frame (exact integer rows, top-down, pre-shader):
// a render scaler may still have line-doubled it (L0,L0,R0,R0...),
// which a sparse row-pair comparison detects so de-doubling precedes
// de-weaving — otherwise even/odd rows would mix both eyes into each
// eye. Eye selection follows 'xr_eye_swap'.

#include "gui/cybermaxx.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "config/setup.h"
#include "gui/xr.h"

// Sum of absolute per-channel (R/G/B) differences, alpha ignored.
static long row_pixel_diff(const uint32_t a, const uint32_t b)
{
	const long dr = static_cast<long>((a >> 16) & 0xFFu) -
	                static_cast<long>((b >> 16) & 0xFFu);
	const long dg = static_cast<long>((a >> 8) & 0xFFu) -
	                static_cast<long>((b >> 8) & 0xFFu);
	const long db = static_cast<long>(a & 0xFFu) -
	                static_cast<long>(b & 0xFFu);
	const long ar = dr < 0 ? -dr : dr;
	const long ag = dg < 0 ? -dg : dg;
	const long ab = db < 0 ? -db : db;
	return ar + ag + ab;
}

// True when the capture repeats every row (line-doubled scaler
// output). Samples a centered band of adjacent row pairs: in a
// doubled capture the in-pair rows match while the cross-pair step
// (the actual L/R field change) does not. Flat bands (no cross-pair
// change) are not decisive and are skipped, so solid-colour frames
// cannot force either verdict (both decodes agree there anyway).
static bool capture_is_doubled(const uint32_t* frame, const int w,
                               const int h)
{
	if (!frame || w <= 0 || h < 8) {
		return false;
	}
	const int step_x = (w > 64) ? (w / 32) : 1;
	int identical = 0, decisive = 0;
	for (int p = 0; p < 8; ++p) {
		const int y = h / 4 + p * 2;
		if (y + 3 >= h) {
			break;
		}
		const uint32_t* r0 = frame + static_cast<size_t>(y) *
		                                   static_cast<size_t>(w);
		const uint32_t* r1 = r0 + static_cast<size_t>(w);
		const uint32_t* r2 = r1 + static_cast<size_t>(w);
		long diff_in = 0, diff_cross = 0;
		for (int x = 0; x < w; x += step_x) {
			diff_in += row_pixel_diff(r0[x], r1[x]);
			diff_cross += row_pixel_diff(r1[x], r2[x]);
		}
		if (diff_cross == 0) {
			continue;
		}
		++decisive;
		// Exact match, or an order of magnitude closer than the
		// field step (covers filtered/bilinear doublers).
		if (diff_in == 0 || diff_in * 8 <= diff_cross) {
			++identical;
		}
	}
	return decisive >= 2 && identical == decisive;
}

void CYBERMAXX_SubmitFrame(std::vector<uint32_t>& eye0,
                           std::vector<uint32_t>& eye1,
                           const std::vector<uint32_t>& frame, const int w,
                           const int h)
{
	if (w <= 0 || h <= 0 || frame.empty()) {
		return;
	}
	const size_t px = static_cast<size_t>(w) * static_cast<size_t>(h);
	if (frame.size() != px) {
		return;
	}
	// Eye buffers are half-height fields (w x h/2, integer division).
	const int eh        = h / 2;
	const size_t eye_px = static_cast<size_t>(w) *
	                      static_cast<size_t>(eh);
	if (eh < 1 || eye0.size() != eye_px || eye1.size() != eye_px) {
		return;
	}
	const bool eye_swap = get_xr_section()->GetBool("xr_eye_swap");
	const int fi = eye_swap ? 1 : 0;
	const int si = 1 - fi;
	auto& dst_f  = (fi == 0) ? eye0 : eye1;
	auto& dst_s  = (si == 0) ? eye0 : eye1;
	const auto row_bytes = static_cast<size_t>(w) * sizeof(uint32_t);
	const uint32_t* src  = frame.data();
	// Eye buffers run bottom-up (the uploader's glReadPixels-order
	// convention) while the game frame is top-down, so field row
	// (eh-1-y) lands on eye row y.
	if (!capture_is_doubled(src, w, h)) {
		// Native interleave: even rows drive one eye, odd the other.
		for (int y = 0; y < eh; ++y) {
			const int i = eh - 1 - y;
			std::memcpy(dst_f.data() + static_cast<size_t>(y) *
			                                  static_cast<size_t>(w),
			            src + static_cast<size_t>(2 * i) *
			                        static_cast<size_t>(w),
			            row_bytes);
			std::memcpy(dst_s.data() + static_cast<size_t>(y) *
			                                  static_cast<size_t>(w),
			            src + (static_cast<size_t>(2 * i) + 1) *
			                        static_cast<size_t>(w),
			            row_bytes);
		}
		return;
	}
	// Line-doubled capture (L0,L0,R0,R0...): de-double, then de-weave.
	// Each game row lands twice so the half-height buffers stay full.
	for (int y = 0; y < eh; ++y) {
		const int bs = ((eh - 1 - y) & ~1) * 2;
		std::memcpy(dst_f.data() + static_cast<size_t>(y) *
		                                  static_cast<size_t>(w),
		            src + static_cast<size_t>(bs) *
		                        static_cast<size_t>(w),
		            row_bytes);
		std::memcpy(dst_s.data() + static_cast<size_t>(y) *
		                                  static_cast<size_t>(w),
		            src + static_cast<size_t>(bs + 2) *
		                        static_cast<size_t>(w),
		            row_bytes);
	}
}
