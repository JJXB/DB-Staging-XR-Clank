// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Stereo image handling (see xrstereo.h): eye buffers, depth-view
// synthesis, autodepth estimation, Voodoo depth capture, and the
// headset/mirror stereo blit.

#include "private/sdl_gui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <string>

#if C_OPENXR
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#if defined(_WIN32)
// openxr_platform.h needs the Win32/GDI/COM types visible first.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#endif

#if C_XR_VULKAN
// vulkan.h must precede the OpenXR platform header so the Vk types
// used by XR_KHR_vulkan_enable are visible.
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#if C_XR_D3D11 || C_XR_D3D12
// d3d11.h/d3d12.h must precede the OpenXR platform header so the D3D
// types used by XR_KHR_D3D11_enable/XR_KHR_D3D12_enable are visible.
#include <dxgi.h>
#include <d3d11.h>
#include <d3d12.h>
#if C_XR_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#if C_XR_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#if defined(_WIN32)
#define XR_USE_PLATFORM_WIN32
#elif defined(__linux__)
#define XR_USE_PLATFORM_XLIB
#define XR_USE_PLATFORM_WAYLAND
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

#include "private/common.h"

#include "audio/mixer.h"
#include "capture/capture.h"
#include "config/config.h"
#include "config/setup.h"
#include "cpu/cpu.h"
#include "dosbox.h"
#include "gui/mapper.h"
#include "gui/render/opengl_renderer.h"
#include "gui/render/sdl_renderer.h"
#include "gui/render/vulkan_renderer.h"
#include "gui/render/d3d11_renderer.h"
#include "gui/render/d3d12_renderer.h"
#include "gui/xrvulkan.h"
#include "gui/xrd3d11.h"
#include "gui/xrd3d12.h"
#include "gui/titlebar.h"
#include "gui/cyshades.h"
#include "gui/cybermaxx.h"
#include "gui/iglasses.h"
#include "gui/xr.h"
#include "gui/xrinput.h"
#include "gui/xrmirror.h"
#include "gui/xrstereo.h"
#include "hardware/input/keyboard.h"
#include "hardware/input/joystick.h"
#include "hardware/input/mouse.h"
#include "hardware/port.h"
#include "hardware/timer.h"
#include "hardware/video/vga.h"
#include "misc/cross.h"
#include "misc/notifications.h"
#include "misc/support.h"
#include "misc/video.h"
#include "utils/checks.h"
#include "utils/env_utils.h"
#include "utils/math_utils.h"
#include "utils/rect.h"
#include "utils/string_utils.h"

// must be included after dosbox_config.h
#include <SDL3/SDL.h>

CHECK_NARROWING();

#if C_OPENXR
// Retained per-eye images (32-bit BGRA) shared with the Voodoo depth
// hook below. Filled either by the stereo decoders in the blit path or
// by XR_VoodooDepthFrame; dimensions tracked alongside.
static std::vector<uint32_t> xr_eye_buf[2];
static int xr_eye_buf_w = 0, xr_eye_buf_h = 0;
// Set by the Voodoo depth hook when it fills the buffers; consumed
// (cleared) by the next depth-mode blit. Stale buffers read as mono.
bool sdl_xr_depth_valid = false;
// Scratch maps for depth synthesis: estimated (autodepth) normalized
// depth plus the Voodoo path's converted color/normalized depth.
static std::vector<float> xr_autodepth_map;
static int xr_autodepth_map_w = 0, xr_autodepth_map_h = 0;
static std::vector<uint32_t> xr_voodoo_argb;
static std::vector<float> xr_voodoo_norm;
static int xr_voodoo_buf_w = 0, xr_voodoo_buf_h = 0;

// Shared depth-stereo synthesizer: builds left/right views from color
// + depth with a backward-mapped shift (stretched edges at depth
// discontinuities). depth_raw is sampled through depth_scale/bias so
// the caller can fold its normalization (and optional inversion) in;
// null reads as flat (mono-equivalent output). src_top_down adapts
// top-down sources (Voodoo FBI) to the bottom-up eye buffers. Both
// eyes are produced in one pixel pass, sharing the depth read and the
// shift math. Fully overwrites both.
static void xr_synthesize_depth_views(const uint32_t* color_argb,
                                      const float* depth_raw,
                                      const int w, const int h,
                                      const float max_shift,
                                      const bool show_depth,
                                      const bool flat,
                                      const bool src_top_down,
                                      const float depth_scale,
                                      const float depth_bias)
{
	if (!color_argb || w <= 0 || h <= 0) {
		return;
	}
	if (xr_eye_buf_w != w || xr_eye_buf_h != h) {
		xr_eye_buf[0].assign(static_cast<size_t>(w) * h, 0xFF000000u);
		xr_eye_buf[1].assign(static_cast<size_t>(w) * h, 0xFF000000u);
		xr_eye_buf_w = w;
		xr_eye_buf_h = h;
	}
	auto* dst_l = xr_eye_buf[0].data();
	auto* dst_r = xr_eye_buf[1].data();

	if (show_depth) {
		// Debug view: raw normalized depth, no shift.
		for (int y = 0; y < h; ++y) {
			const auto* d_row = depth_raw
			        ? depth_raw + static_cast<size_t>(y) * w
			        : nullptr;
			const int out_y = src_top_down ? (h - 1 - y) : y;
			auto* l_row     = dst_l + static_cast<size_t>(out_y) * w;
			auto* r_row     = dst_r + static_cast<size_t>(out_y) * w;
			for (int x = 0; x < w; ++x) {
				uint32_t g = 0;
				if (!d_row) {
					g = 0; // no depth buffer
				} else if (flat) {
					g = 128; // flat depth
				} else {
					float dn = d_row[x] * depth_scale +
					           depth_bias;
					dn = dn < 0.0f ? 0.0f
					     : (dn > 1.0f ? 1.0f : dn);
					g = static_cast<uint32_t>(dn * 255.0f);
				}
				const uint32_t p = 0xFF000000u | (g << 16) |
				                   (g << 8) | g;
				l_row[x] = p;
				r_row[x] = p;
			}
		}
		return;
	}

	for (int y = 0; y < h; ++y) {
		const auto* c_row = color_argb + static_cast<size_t>(y) * w;
		const auto* d_row = depth_raw
		        ? depth_raw + static_cast<size_t>(y) * w
		        : nullptr;
		const int out_y = src_top_down ? (h - 1 - y) : y;
		auto* l_row     = dst_l + static_cast<size_t>(out_y) * w;
		auto* r_row     = dst_r + static_cast<size_t>(out_y) * w;
		for (int x = 0; x < w; ++x) {
			float dn = 0.0f;
			if (d_row) {
				dn = d_row[x] * depth_scale + depth_bias;
				dn = dn < 0.0f ? 0.0f
				     : (dn > 1.0f ? 1.0f : dn);
			}
			// Symmetric rounding for both eyes: the old
			// '+0.5f then truncate' only rounded positive
			// shifts correctly.
			const int shift = iroundf(dn * max_shift);
			const int sx_l  = std::clamp(x + shift, 0, w - 1);
			const int sx_r  = std::clamp(x - shift, 0, w - 1);
			l_row[x]        = c_row[sx_l];
			r_row[x]        = c_row[sx_r];
		}
	}
}

// Voodoo depth-stereo synthesizer, called from the Voodoo emulation
// once per frame before the image is handed to the renderer. Converts
// the 16-bit color + depth buffers, then shares the shift core above.
// Runs only in 'depth' stereo mode; other modes ignore the call.
void XR_VoodooDepthFrame(const uint32_t width, const uint32_t height,
                         const uint32_t rowpixels,
                         const uint16_t* color_rgb565,
                         const uint16_t* depth)
{
	if (!color_rgb565 || width == 0 || height == 0 || width > 4096 ||
	    height > 4096 || rowpixels < width) {
		return;
	}
	if (get_xr_section()->GetString("xr_stereo_input") != "depth") {
		return;
	}

	const int w      = static_cast<int>(width);
	const int h      = static_cast<int>(height);
	const int stride = static_cast<int>(rowpixels);

	// Normalize depth over the frame. Flat or absent depth yields zero
	// shift (mono-equivalent output).
	uint16_t zmin = 0xFFFFu, zmax = 0u;
	if (depth) {
		for (int y = 0; y < h; ++y) {
			const auto* row = depth + static_cast<size_t>(y) * stride;
			for (int x = 0; x < w; ++x) {
				const auto z = row[x];
				if (z < zmin) {
					zmin = z;
				}
				if (z > zmax) {
					zmax = z;
				}
			}
		}
	}
	int pct = get_xr_section()->GetInt("xr_depth_strength");
	if (pct < 0) {
		pct = 0;
	}
	if (pct > 50) {
		pct = 50;
	}
	const float inv_range = (depth && zmax > zmin)
	        ? (1.0f / static_cast<float>(zmax - zmin))
	        : 0.0f;
	const float max_shift = (pct / 100.0f) * w * 0.5f;
	const bool show_depth =
	        get_xr_section()->GetBool("xr_show_depth");
	// FBI rows run top-down while the eye buffers run bottom-up, hence
	// the default flip (xr_flip_vertical corrects bottom-up titles).
	const bool flip_v = get_xr_section()->GetBool("xr_flip_vertical");
	const bool flat   = (!depth || zmax <= zmin);

	if (xr_voodoo_buf_w != w || xr_voodoo_buf_h != h) {
		xr_voodoo_argb.assign(static_cast<size_t>(w) * h,
		                      0xFF000000u);
		xr_voodoo_norm.assign(static_cast<size_t>(w) * h, 0.0f);
		xr_voodoo_buf_w = w;
		xr_voodoo_buf_h = h;
	}
	for (int y = 0; y < h; ++y) {
		const auto* c_row = color_rgb565 + static_cast<size_t>(y) *
		                                        stride;
		const auto* z_row = depth
		        ? depth + static_cast<size_t>(y) * stride
		        : nullptr;
		auto* a_row = xr_voodoo_argb.data() + static_cast<size_t>(y) *
		                                                w;
		auto* n_row = xr_voodoo_norm.data() + static_cast<size_t>(y) *
		                                              w;
		for (int x = 0; x < w; ++x) {
			const uint32_t c  = c_row[x];
			const uint32_t r5 = (c >> 11) & 31u;
			const uint32_t g6 = (c >> 5) & 63u;
			const uint32_t b5 = c & 31u;
			const uint32_t r8 = (r5 * 255u + 15u) / 31u;
			const uint32_t g8 = (g6 * 255u + 31u) / 63u;
			const uint32_t b8 = (b5 * 255u + 15u) / 31u;
			a_row[x] = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
			n_row[x] = z_row
			        ? (zmax - z_row[x]) * inv_range
			        : 0.0f;
		}
	}
	xr_synthesize_depth_views(xr_voodoo_argb.data(),
	                          depth ? xr_voodoo_norm.data() : nullptr,
	                          w,
	                          h,
	                          max_shift,
	                          show_depth,
	                          flat,
	                          !flip_v,
	                          1.0f,
	                          0.0f);
	sdl_xr_depth_valid = true;
}

// Shared 0.299/0.587/0.114 luma in 8-bit fixed point.
static inline uint8_t xr_autodepth_luma8(const uint32_t p)
{
	return static_cast<uint8_t>((77u * ((p >> 16) & 0xFFu) +
	                             150u * ((p >> 8) & 0xFFu) +
	                             29u * (p & 0xFFu)) >>
	                            8);
}

// Robust normalization range for a 0..1 depth map: 1% tails are
// clipped so single outliers cannot compress the usable range, and a
// narrow distribution reads as flat instead of being amplified into
// full parallax. Returns true when flat (lo/hi are zeroed).
static bool xr_depth_range_from_hist(const uint32_t* hist, const int pixels,
                                     float& lo_out, float& hi_out)
{
	if (!hist || pixels <= 0) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	const int tail = pixels / 100;
	int acc        = 0;
	int lo_bin     = 0;
	for (int b = 0; b < 256; ++b) {
		acc += static_cast<int>(hist[b]);
		if (acc > tail) {
			lo_bin = b;
			break;
		}
	}
	acc = 0;
	int hi_bin = 255;
	for (int b = 255; b >= 0; --b) {
		acc += static_cast<int>(hist[b]);
		if (acc > tail) {
			hi_bin = b;
			break;
		}
	}
	// Below ~1.5% of the range is noise.
	if (hi_bin - lo_bin <= 4) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	lo_out = static_cast<float>(lo_bin) / 255.0f;
	hi_out = static_cast<float>(hi_bin) / 255.0f;
	return false;
}

// Same range calculation for a materialized 0..1 map.
static bool xr_depth_percentile_range(const float* map, const size_t n,
                                      float& lo_out, float& hi_out)
{
	if (!map || n == 0) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	uint32_t hist[256] = {};
	for (size_t i = 0; i < n; ++i) {
		float d = map[i];
		d       = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
		uint32_t bin = static_cast<uint32_t>(d * 255.0f + 0.5f);
		if (bin > 255u) {
			bin = 255u;
		}
		++hist[bin];
	}
	return xr_depth_range_from_hist(hist, static_cast<int>(n), lo_out,
	                                hi_out);
}

// Heuristic monocular depth for the 'autodepth' stereo mode: no depth
// buffer exists for 2D content, so near/far is estimated from the
// frame itself. frame is bottom-up ARGB32 (0xAARRGGBB, row 0 = screen
// bottom); the raw map runs 1 = near to 0 = far. lo_out/hi_out return
// a robust range (1st/99th percentile) for the caller to normalize
// with, so a single bright HUD pixel cannot flatten the depth. Cues: a
// vertical ground-plane gradient (screen bottom reads as near) and
// luminance (bright reads as near). Returns true when flat. The caller
// folds lo_out/hi_out (and inversion) into the synthesizer's
// depth_scale/depth_bias, avoiding a separate normalization pass.
static bool xr_estimate_depth_auto(const uint32_t* frame, const int w,
                                   const int h, float* depth_raw,
                                   const bool use_grad, const bool use_luma,
                                   float& lo_out, float& hi_out)
{
	if (!depth_raw || w <= 0 || h <= 0) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	if (use_grad && !use_luma) {
		// The cue is a pure function of the row, so the frame pixels
		// are not needed: fill a 0..1 ramp and keep the full range.
		const float y_denom = (h > 1) ? 1.0f / (h - 1) : 0.0f;
		for (int y = 0; y < h; ++y) {
			const float d = 1.0f - y * y_denom;
			auto* d_row = depth_raw + static_cast<size_t>(y) * w;
			for (int x = 0; x < w; ++x) {
				d_row[x] = d;
			}
		}
		lo_out = 0.0f;
		hi_out = (h > 1) ? 1.0f : 0.0f;
		return h <= 1;
	}
	if (!frame) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}

	// Blend or luma-only weights. Keeping them out of the pixel loop
	// removes a per-pixel cue branch (gradient-only returned above).
	const float w_luma     = use_grad ? 0.5f : 1.0f;
	const float w_grad     = use_grad ? 0.5f : 0.0f;
	const float luma_scale = w_luma / 255.0f;
	const float y_denom    = (h > 1) ? 1.0f / (h - 1) : 0.0f;

	// 8-bit histogram for the robust normalization range.
	uint32_t hist[256] = {};
	for (int y = 0; y < h; ++y) {
		const auto* row = frame + static_cast<size_t>(y) * w;
		auto* d_row     = depth_raw + static_cast<size_t>(y) * w;
		const float grad = (1.0f - y * y_denom) * w_grad;
		for (int x = 0; x < w; ++x) {
			const uint32_t luma8 = xr_autodepth_luma8(row[x]);
			const float d = grad + luma8 * luma_scale;
			d_row[x]      = d;
			uint32_t bin  = static_cast<uint32_t>(d * 255.0f +
			                                      0.5f);
			if (bin > 255u) {
				bin = 255u;
			}
			++hist[bin];
		}
	}

	const int pixels = w * h;
	const bool flat  = xr_depth_range_from_hist(hist, pixels, lo_out,
	                                            hi_out);
	if (flat) {
		std::fill_n(depth_raw, static_cast<size_t>(pixels), 0.0f);
	}
	return flat;
}

// Detail/edge-energy cue: textured regions read as near, flat sky /
// walls / backgrounds read as far. Gradients come from 8-bit luma,
// then a separable [1 2 1] blur keeps the shift field from shimmering
// on single-pixel noise.
static bool xr_estimate_depth_detail(const uint32_t* frame, const int w,
                                     const int h, float* depth_raw,
                                     float& lo_out, float& hi_out)
{
	if (!frame || !depth_raw || w <= 0 || h <= 0) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	const size_t n        = static_cast<size_t>(w) * static_cast<size_t>(h);
	const size_t row_size = static_cast<size_t>(w);

	static std::vector<uint8_t> rows;
	static std::vector<float> aux;
	if (rows.size() < row_size * 3) {
		rows.resize(row_size * 3);
	}
	if (aux.size() < n) {
		aux.resize(n);
	}
	auto* prev = rows.data();
	auto* cur  = rows.data() + row_size;
	auto* next = rows.data() + row_size * 2;

	const auto fill_row = [&](const int y, uint8_t* dst) {
		const auto* src = frame + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			dst[x] = xr_autodepth_luma8(src[x]);
		}
	};
	fill_row(0, cur);
	std::memcpy(prev, cur, row_size);
	if (h > 1) {
		fill_row(1, next);
	} else {
		std::memcpy(next, cur, row_size);
	}

	for (int y = 0; y < h; ++y) {
		auto* d_row = depth_raw + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			const int xl = (x > 0) ? (x - 1) : x;
			const int xr = (x < w - 1) ? (x + 1) : x;
			const int gx = static_cast<int>(cur[xr]) -
			               static_cast<int>(cur[xl]);
			const int gy = static_cast<int>(next[x]) -
			               static_cast<int>(prev[x]);
			d_row[x] = (std::abs(gx) + std::abs(gy)) *
			           (1.0f / 510.0f);
		}
		if (y + 1 < h) {
			uint8_t* tmp = prev;
			prev         = cur;
			cur          = next;
			next         = tmp;
			if (y + 2 < h) {
				fill_row(y + 2, next);
			} else {
				std::memcpy(next, cur, row_size);
			}
		}
	}

	// Separable [1 2 1] blur: horizontal into aux, vertical back into
	// depth_raw.
	for (int y = 0; y < h; ++y) {
		const auto* src = depth_raw + static_cast<size_t>(y) * w;
		auto* dst       = aux.data() + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			const float l = (x > 0) ? src[x - 1] : src[x];
			const float r = (x < w - 1) ? src[x + 1] : src[x];
			dst[x] = 0.25f * l + 0.5f * src[x] + 0.25f * r;
		}
	}
	for (int y = 0; y < h; ++y) {
		const auto* src = aux.data() + static_cast<size_t>(y) * w;
		const auto* up  = (y > 0) ? src - w : src;
		const auto* dn  = (y < h - 1) ? src + w : src;
		auto* dst       = depth_raw + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			dst[x] = 0.25f * up[x] + 0.5f * src[x] + 0.25f * dn[x];
		}
	}
	return xr_depth_percentile_range(depth_raw, n, lo_out, hi_out);
}

// Center saliency: a smooth radial ramp, near at the middle of the
// picture and far at the edges. Content-independent, but useful for
// galleries, menus, and titles where the action is centred.
static bool xr_estimate_depth_center(const int w, const int h,
                                     float* depth_raw, float& lo_out,
                                     float& hi_out)
{
	if (!depth_raw || w <= 0 || h <= 0) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	const float cx     = (w - 1) * 0.5f;
	const float cy     = (h - 1) * 0.5f;
	const float inv_rx = 1.0f / std::max(1.0f, cx);
	const float inv_ry = 1.0f / std::max(1.0f, cy);
	for (int y = 0; y < h; ++y) {
		const float dy = (y - cy) * inv_ry;
		auto* row      = depth_raw + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			const float dx = (x - cx) * inv_rx;
			const float r  = std::sqrt(dx * dx + dy * dy);
			row[x] = (r >= 1.0f) ? 0.0f : (1.0f - r);
		}
	}
	lo_out = 0.0f;
	hi_out = 1.0f;
	return false;
}

// Layer segmentation: flood-fills similarly coloured regions, then
// treats small regions as uniform planes (pop-up book) while large
// regions keep a graded vertical background. This gives sprites/UI a
// crisp depth layer without turning flat art into a noisy per-pixel
// depth field.
struct XrAutodepthLayer {
	int64_t area  = 0;
	int64_t sum_y = 0;
};

static bool xr_estimate_depth_layers(const uint32_t* frame, const int w,
                                     const int h, float* depth_raw,
                                     float& lo_out, float& hi_out)
{
	if (!frame || !depth_raw || w <= 0 || h <= 0) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);

	static std::vector<int32_t> labels;
	static std::vector<int32_t> stack_x;
	static std::vector<int32_t> stack_y;
	static std::vector<XrAutodepthLayer> layers;
	static std::vector<float> layer_depth;
	static std::vector<float> layer_weight;

	labels.assign(n, -1);
	stack_x.clear();
	stack_y.clear();
	layers.clear();
	stack_x.reserve(4096);
	stack_y.reserve(4096);
	layers.reserve(256);

	constexpr int kColorTolerance = 20;
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const int idx = y * w + x;
			if (labels[idx] >= 0) {
				continue;
			}
			const uint32_t seed = frame[idx];
			const int label     = static_cast<int>(layers.size());
			labels[idx]         = label;
			stack_x.push_back(x);
			stack_y.push_back(y);

			XrAutodepthLayer stat = {};
			const auto push_if    = [&](const int nx, const int ny) {
				if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
					return;
				}
				const int j = ny * w + nx;
				if (labels[j] >= 0) {
					return;
				}
				const uint32_t c = frame[j];
				const int dr = static_cast<int>((c >> 16) & 0xFFu) -
				               static_cast<int>((seed >> 16) & 0xFFu);
				const int dg = static_cast<int>((c >> 8) & 0xFFu) -
				               static_cast<int>((seed >> 8) & 0xFFu);
				const int db = static_cast<int>(c & 0xFFu) -
				               static_cast<int>(seed & 0xFFu);
				if (std::abs(dr) > kColorTolerance ||
				    std::abs(dg) > kColorTolerance ||
				    std::abs(db) > kColorTolerance) {
					return;
				}
				labels[j] = label;
				stack_x.push_back(nx);
				stack_y.push_back(ny);
			};
			while (!stack_x.empty()) {
				const int px = stack_x.back();
				const int py = stack_y.back();
				stack_x.pop_back();
				stack_y.pop_back();
				++stat.area;
				stat.sum_y += py;
				push_if(px - 1, py);
				push_if(px + 1, py);
				push_if(px, py - 1);
				push_if(px, py + 1);
			}
			layers.push_back(stat);
		}
	}

	layer_depth.resize(layers.size());
	layer_weight.resize(layers.size());
	const float inv_h = (h > 1) ? 1.0f / (h - 1) : 0.0f;
	const float inv_n = 1.0f / static_cast<float>(n);
	for (size_t i = 0; i < layers.size(); ++i) {
		const float area_norm = static_cast<float>(layers[i].area) *
		                        inv_n;
		const float avg_y = (layers[i].area > 0)
		        ? static_cast<float>(layers[i].sum_y) /
		                  static_cast<float>(layers[i].area)
		        : 0.0f;
		const float y_norm      = avg_y * inv_h;
		const float small_bonus = 1.0f - std::clamp(area_norm / 0.06f,
		                                            0.0f,
		                                            1.0f);
		layer_depth[i] = std::clamp(1.0f - y_norm +
		                                    0.25f * small_bonus,
		                            0.0f,
		                            1.0f);
		layer_weight[i] = 1.0f -
		                  std::clamp(area_norm / 0.15f, 0.0f, 1.0f);
	}
	for (int y = 0; y < h; ++y) {
		const float base = 1.0f - y * inv_h;
		auto* row        = depth_raw + static_cast<size_t>(y) * w;
		const auto* lab  = labels.data() + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			const int l = lab[x];
			row[x] = base * (1.0f - layer_weight[l]) +
			         layer_depth[l] * layer_weight[l];
		}
	}
	return xr_depth_percentile_range(depth_raw, n, lo_out, hi_out);
}

// Background subtraction: a slow luma background model marks changed
// pixels as near. Static cameras (HUD-heavy 2D games, fixed views)
// work best; a pan or scene cut restarts the model and reads as mono
// for that frame.
static bool xr_estimate_depth_background(const uint32_t* frame, const int w,
                                         const int h, float* depth_raw,
                                         float& lo_out, float& hi_out,
                                         const bool reset)
{
	if (!frame || !depth_raw || w <= 0 || h <= 0) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);

	static std::vector<uint8_t> bg;
	static int bg_w      = 0;
	static int bg_h      = 0;
	static bool bg_valid = false;

	if (reset || !bg_valid || bg_w != w || bg_h != h) {
		bg.resize(n);
		for (size_t i = 0; i < n; ++i) {
			bg[i] = xr_autodepth_luma8(frame[i]);
		}
		bg_w     = w;
		bg_h     = h;
		bg_valid = true;
		std::fill_n(depth_raw, n, 0.0f);
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}

	constexpr int kDiffLo   = 8;
	constexpr int kDiffSpan = 64;
	constexpr int kChanged  = 16;
	int changed             = 0;
	for (int y = 0; y < h; ++y) {
		auto* row    = depth_raw + static_cast<size_t>(y) * w;
		const auto* src = frame + static_cast<size_t>(y) * w;
		auto* bg_row = bg.data() + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			const int luma = xr_autodepth_luma8(src[x]);
			const int diff = std::abs(luma -
			                          static_cast<int>(bg_row[x]));
			float d = static_cast<float>(diff - kDiffLo) /
			          static_cast<float>(kDiffSpan);
			d       = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
			row[x]  = d;
			if (diff > kChanged) {
				++changed;
			}
			// Faster adaptation for background pixels, slower for
			// moving ones so a passing sprite leaves only a short
			// trail.
			const int shift = (diff <= kChanged) ? 4 : 6;
			const int cur   = static_cast<int>(bg_row[x]);
			if (luma > cur) {
				bg_row[x] = static_cast<uint8_t>(
				        cur + ((luma - cur + (1 << shift) - 1) >>
				               shift));
			} else if (luma < cur) {
				bg_row[x] = static_cast<uint8_t>(
				        cur - ((cur - luma + (1 << shift) - 1) >>
				               shift));
			}
		}
	}
	// Camera pan / scene cut: restart and read as mono.
	if (changed * 2 > static_cast<int>(n)) {
		for (size_t i = 0; i < n; ++i) {
			bg[i] = xr_autodepth_luma8(frame[i]);
		}
		std::fill_n(depth_raw, n, 0.0f);
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	return xr_depth_percentile_range(depth_raw, n, lo_out, hi_out);
}

// Optical flow: block matching on a 4x-downsampled luma. A dominant
// global translation (panning) is removed, leaving independently
// moving content and radial (forward-motion) expansion as the depth
// cue. Works best with camera motion or moving sprites; a cut or a
// mostly-unmatched frame restarts the reference and reads as mono.
static bool xr_estimate_depth_flow(const uint32_t* frame, const int w,
                                   const int h, float* depth_raw,
                                   float& lo_out, float& hi_out,
                                   const bool reset)
{
	if (!frame || !depth_raw || w <= 0 || h <= 0) {
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}
	constexpr int kDown   = 4;
	constexpr int kBlock  = 4;
	constexpr int kSearch = 5;
	const int lw          = w / kDown;
	const int lh          = h / kDown;
	if (lw < kBlock || lh < kBlock) {
		std::fill_n(depth_raw, static_cast<size_t>(w) * h, 0.0f);
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}

	static std::vector<uint8_t> cur;
	static std::vector<uint8_t> prev;
	static int flow_lw      = 0;
	static int flow_lh      = 0;
	static bool flow_valid  = false;
	static std::vector<float> block_depth;
	static std::vector<int8_t> block_dx;
	static std::vector<int8_t> block_dy;
	static std::vector<uint8_t> block_ok;

	const size_t low_n = static_cast<size_t>(lw) * lh;
	if (flow_lw != lw || flow_lh != lh) {
		cur.resize(low_n);
		prev.resize(low_n);
		flow_lw    = lw;
		flow_lh    = lh;
		flow_valid = false;
	}
	for (int ly = 0; ly < lh; ++ly) {
		const int y0 = ly * kDown;
		auto* dst    = cur.data() + static_cast<size_t>(ly) * lw;
		for (int lx = 0; lx < lw; ++lx) {
			const int x0 = lx * kDown;
			uint32_t sum = 0;
			for (int dy = 0; dy < kDown; ++dy) {
				const auto* src = frame +
				        static_cast<size_t>(y0 + dy) * w + x0;
				for (int dx = 0; dx < kDown; ++dx) {
					sum += xr_autodepth_luma8(src[dx]);
				}
			}
			dst[lx] = static_cast<uint8_t>(sum >> 4);
		}
	}
	if (reset || !flow_valid) {
		std::swap(prev, cur);
		flow_valid = true;
		std::fill_n(depth_raw, static_cast<size_t>(w) * h, 0.0f);
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}

	const int bw     = lw / kBlock;
	const int bh     = lh / kBlock;
	const int blocks = bw * bh;
	block_dx.assign(blocks, 0);
	block_dy.assign(blocks, 0);
	block_ok.assign(blocks, 0);
	block_depth.assign(blocks, 0.0f);

	const int side = 2 * kSearch + 1;
	int hist[side * side] = {};
	int reliable          = 0;
	const int kSadMax     = kBlock * kBlock * 24;

	for (int by = 0; by < bh; ++by) {
		const int y0   = by * kBlock;
		const int sy_lo = std::max(-kSearch, -y0);
		const int sy_hi = std::min(kSearch, lh - kBlock - y0);
		for (int bx = 0; bx < bw; ++bx) {
			const int x0    = bx * kBlock;
			const int sx_lo = std::max(-kSearch, -x0);
			const int sx_hi = std::min(kSearch, lw - kBlock - x0);
			int best_sad    = kSadMax + 1;
			int best_dx     = 0;
			int best_dy     = 0;
			for (int dy = sy_lo; dy <= sy_hi; ++dy) {
				for (int dx = sx_lo; dx <= sx_hi; ++dx) {
					int sad = 0;
					// SAD only grows, so stop once this
					// candidate cannot beat the best.
					for (int yy = 0;
					     yy < kBlock && sad < best_sad;
					     ++yy) {
						const auto* a = cur.data() +
						        static_cast<size_t>(y0 + yy) * lw + x0;
						const auto* b = prev.data() +
						        static_cast<size_t>(y0 + yy + dy) * lw +
						        (x0 + dx);
						for (int xx = 0; xx < kBlock; ++xx) {
							sad += std::abs(
							        static_cast<int>(a[xx]) -
							        static_cast<int>(b[xx]));
						}
					}
					if (sad < best_sad) {
						best_sad = sad;
						best_dx  = dx;
						best_dy  = dy;
					}
				}
			}
			const int bi = by * bw + bx;
			if (best_sad <= kSadMax) {
				block_dx[bi] = static_cast<int8_t>(best_dx);
				block_dy[bi] = static_cast<int8_t>(best_dy);
				block_ok[bi] = 1;
				++reliable;
				++hist[(best_dy + kSearch) * side +
				       (best_dx + kSearch)];
			}
		}
	}
	if (reliable * 5 < blocks) {
		std::swap(prev, cur);
		std::fill_n(depth_raw, static_cast<size_t>(w) * h, 0.0f);
		lo_out = 0.0f;
		hi_out = 0.0f;
		return true;
	}

	int mode_count = 0;
	int mode_idx   = 0;
	for (int i = 0; i < side * side; ++i) {
		if (hist[i] > mode_count) {
			mode_count = hist[i];
			mode_idx   = i;
		}
	}
	const int zero_idx = kSearch * side + kSearch;
	int gdx            = 0;
	int gdy            = 0;
	// Keep the reference at zero when half the frame is static (a
	// fixed camera with a big moving object must not turn the
	// background into the foreground). Otherwise remove a dominant
	// translation (a pan or a scrolling background), which leaves
	// independently moving sprites as near. Radial forward motion
	// has no dominant bin, so its expansion stays intact as the cue.
	if (hist[zero_idx] * 2 < reliable && mode_idx != zero_idx &&
	    mode_count * 2 > reliable) {
		gdy = mode_idx / side - kSearch;
		gdx = mode_idx % side - kSearch;
	}
	const float inv_search = 1.0f / static_cast<float>(kSearch);
	for (int bi = 0; bi < blocks; ++bi) {
		if (!block_ok[bi]) {
			continue;
		}
		const int rx = static_cast<int>(block_dx[bi]) - gdx;
		const int ry = static_cast<int>(block_dy[bi]) - gdy;
		const float mag = std::sqrt(static_cast<float>(rx * rx +
		                                               ry * ry)) *
		                  inv_search;
		block_depth[bi] = mag > 1.0f ? 1.0f : mag;
	}

	// Bilinear upsample of the block-depth grid.
	const float scale = 1.0f / static_cast<float>(kDown * kBlock);
	for (int y = 0; y < h; ++y) {
		const float fy = (y + 0.5f) * scale - 0.5f;
		int y0         = static_cast<int>(std::floor(fy));
		const float ty = fy - static_cast<float>(y0);
		int y1         = y0 + 1;
		y0             = std::clamp(y0, 0, bh - 1);
		y1             = std::clamp(y1, 0, bh - 1);
		auto* dst      = depth_raw + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			const float fx = (x + 0.5f) * scale - 0.5f;
			int x0         = static_cast<int>(std::floor(fx));
			const float tx = fx - static_cast<float>(x0);
			int x1         = x0 + 1;
			x0             = std::clamp(x0, 0, bw - 1);
			x1             = std::clamp(x1, 0, bw - 1);
			const float v00 = block_depth[y0 * bw + x0];
			const float v10 = block_depth[y0 * bw + x1];
			const float v01 = block_depth[y1 * bw + x0];
			const float v11 = block_depth[y1 * bw + x1];
			const float top = v00 + (v10 - v00) * tx;
			const float bot = v01 + (v11 - v01) * tx;
			dst[x]          = top + (bot - top) * ty;
		}
	}
	std::swap(prev, cur);
	return xr_depth_percentile_range(depth_raw,
	                                 static_cast<size_t>(w) * h,
	                                 lo_out,
	                                 hi_out);
}

// In-place vertical flip of a w*h pixel buffer (GL backbuffer
// readbacks are bottom-up; the Vulkan game frame is top-down, so it
// is flipped once to feed the shared decode below with identical
// eyes/fields on both backends).
static void xr_flip_rows(std::vector<uint32_t>& px, const int w_px,
                         const int h_px)
{
	if (w_px <= 0 || h_px <= 0 ||
	    px.size() != static_cast<size_t>(w_px) * h_px) {
		return;
	}
	for (int y = 0; y < h_px / 2; ++y) {
		auto* a = px.data() + static_cast<size_t>(y) * w_px;
		auto* b = px.data() + static_cast<size_t>(h_px - 1 - y) * w_px;
		for (int x = 0; x < w_px; ++x) {
			const uint32_t t = a[x];
			a[x]             = b[x];
			b[x]             = t;
		}
	}
}

// Backend-neutral eye upload: bottom-up BGRA CPU pixels into one
// swapchain array layer (GPU upscaling into dst_rect_px, GL
// bottom-left origin). The Vulkan path converts the rect to top-left
// origin internally.
static void xr_upload_eye(const int eye_slot, const uint32_t* pixels,
                          const int w_px, const int h_px,
                          const uint64_t dst_handle, const int dst_layer,
                          const SDL_Rect dst_rect_px, const int eye_w_px,
                          const int eye_h_px)
{
	if (openxr_gfx_backend() == XrGfxBackend::OpenGl) {
		auto* gl_renderer =
		        static_cast<OpenGlRenderer*>(sdl.renderer.get());
		gl_renderer->UploadAndBlitEye(eye_slot,
		                              pixels,
		                              w_px,
		                              h_px,
		                              static_cast<GLuint>(dst_handle),
		                              dst_layer,
		                              dst_rect_px,
		                              eye_w_px,
		                              eye_h_px);
		return;
	}
	// GL rects are bottom-left origin; Vulkan/D3D images are
	// top-left, so the row flips here.
	const int top_y = eye_h_px - (dst_rect_px.y + dst_rect_px.h);
#if C_XR_VULKAN
	if (openxr_gfx_backend() == XrGfxBackend::Vulkan) {
		xrvk_upload_fitted(dst_handle,
		                   dst_layer,
		                   eye_w_px,
		                   eye_h_px,
		                   dst_rect_px.x,
		                   top_y,
		                   dst_rect_px.w,
		                   dst_rect_px.h,
		                   pixels,
		                   w_px,
		                   h_px);
		return;
	}
#endif
#if C_XR_D3D11
	if (openxr_gfx_backend() == XrGfxBackend::D3D11) {
		xrd11_upload_fitted(dst_handle,
		                    dst_layer,
		                    eye_w_px,
		                    eye_h_px,
		                    dst_rect_px.x,
		                    top_y,
		                    dst_rect_px.w,
		                    dst_rect_px.h,
		                    pixels,
		                    w_px,
		                    h_px);
		return;
	}
#endif
#if C_XR_D3D12
	if (openxr_gfx_backend() == XrGfxBackend::D3D12) {
		xrd12_upload_fitted(dst_handle,
		                    dst_layer,
		                    eye_w_px,
		                    eye_h_px,
		                    dst_rect_px.x,
		                    top_y,
		                    dst_rect_px.w,
		                    dst_rect_px.h,
		                    pixels,
		                    w_px,
		                    h_px);
		return;
	}
#endif
}

// Pixel rect of the pictured area inside each eye slice, as last blitted.
// The screen-panel quad uses this to crop its source image so the picture
// floats as a panel without the letterbox border. Indexed [eye][x/y/w/h].
static int sdl_xr_picture_rect[2][4] = {};

bool OPENXR_GetPictureRect(const int eye, int& x_out, int& y_out,
                           int& w_out, int& h_out)
{
	if (eye < 0 || eye > 1) {
		return false;
	}
	x_out = sdl_xr_picture_rect[eye][0];
	y_out = sdl_xr_picture_rect[eye][1];
	w_out = sdl_xr_picture_rect[eye][2];
	h_out = sdl_xr_picture_rect[eye][3];
	return (w_out > 0 && h_out > 0);
}

// Called by the renderer's PresentFrame() after rendering the mirror
// image and before the buffer swap, while the backbuffer is valid.
// Copies the DOS draw area into every eye slice of the acquired
// swapchain image. No-op when no XR frame is in flight.
void OPENXR_BlitMirrorToSwapchain()
{
	if (!get_xr_section()->GetBool("xr_mirror_window")) {
		XR_MirrorWindowUpdate(nullptr, nullptr, 0, 0);
	}
	const std::string stereo_mode =
	        get_xr_section()->GetString("xr_stereo_input");
	const bool have_swapchain = (sdl_xr_active_image >= 0 &&
	                             !sdl_xr_swapchain_images.empty() &&
	                             sdl_xr_view_count > 0);
	const bool mirror_window_on =
	        get_xr_section()->GetBool("xr_mirror_window") ||
	        get_xr_section()->GetBool("xr_native_sbs");
	// Headset-free stereo preview: with no XR frame in flight, still
	// decode into the eye buffers for the desktop mirror when the
	// mirror-stereo preview is on (Ctrl+Alt+T), or whenever the
	// headset-mirror / native-SBS window is enabled (no OpenXR needed).
	const bool want_preview = (!have_swapchain &&
	                           (sdl_xr_mirror_stereo || mirror_window_on) &&
	                           stereo_mode != "mono");
	if (!have_swapchain && !want_preview && !mirror_window_on) {
		return;
	}
	const bool no_xr = (openxr_gfx_backend() == XrGfxBackend::None);
	if (no_xr && !mirror_window_on) {
		return;
	}
	if (!sdl.renderer) {
		return;
	}
	const bool use_cpu = no_xr
	        ? (dynamic_cast<OpenGlRenderer*>(sdl.renderer.get()) == nullptr)
	        : (openxr_gfx_backend() != XrGfxBackend::OpenGl);
	// Neutral image handle (GLuint texture or VkImage, see XrGfxImage).
	uint64_t dst_img  = 0;
	int eye_w = 1, eye_h = 1;
	if (have_swapchain) {
		const uint32_t image_index =
		        static_cast<uint32_t>(sdl_xr_active_image);
		if (image_index >= sdl_xr_swapchain_images.size()) {
			return;
		}
		dst_img = sdl_xr_swapchain_images[image_index].handle;
		if (!dst_img) {
			return;
		}
		eye_w = static_cast<int>(sdl_xr_swapchain_width);
		eye_h = static_cast<int>(sdl_xr_swapchain_height);
	}
	// Non-GL source: the top-down game frame from the CPU-frame
	// renderer, flipped once to the bottom-up convention the decode
	// below shares with the GL backbuffer readback (only the GL
	// backend has a backbuffer to read).
	std::vector<uint32_t> cpu_frame;
	int cpu_fw = 0, cpu_fh = 0;
	if (use_cpu) {
		bool got = false;
		if (no_xr) {
			if (auto* vr = dynamic_cast<VulkanRenderer*>(
			            sdl.renderer.get())) {
				got = vr->GetGameFrame(cpu_frame, cpu_fw, cpu_fh);
			}
#if C_XR_D3D11
			else if (auto* dr = dynamic_cast<D3D11Renderer*>(
			                 sdl.renderer.get())) {
				got = dr->GetGameFrame(cpu_frame, cpu_fw, cpu_fh);
			}
#endif
#if C_XR_D3D12
			else if (auto* dr = dynamic_cast<D3D12Renderer*>(
			                 sdl.renderer.get())) {
				got = dr->GetGameFrame(cpu_frame, cpu_fw, cpu_fh);
			}
#endif
			else if (auto* sr = dynamic_cast<SdlRenderer*>(
			                 sdl.renderer.get())) {
				got = sr->GetGameFrame(cpu_frame, cpu_fw, cpu_fh);
			}
		} else {
			const auto backend = openxr_gfx_backend();
			if (backend == XrGfxBackend::Vulkan) {
				auto* vr = static_cast<VulkanRenderer*>(
				        sdl.renderer.get());
				got = vr->GetGameFrame(cpu_frame, cpu_fw, cpu_fh);
			}
#if C_XR_D3D11
			else if (backend == XrGfxBackend::D3D11) {
				auto* dr = static_cast<D3D11Renderer*>(
				        sdl.renderer.get());
				got = dr->GetGameFrame(cpu_frame, cpu_fw, cpu_fh);
			}
#endif
#if C_XR_D3D12
			else if (backend == XrGfxBackend::D3D12) {
				auto* dr = static_cast<D3D12Renderer*>(
				        sdl.renderer.get());
				got = dr->GetGameFrame(cpu_frame, cpu_fw, cpu_fh);
			}
#endif
		}
		if (got) {
			xr_flip_rows(cpu_frame, cpu_fw, cpu_fh);
		}
	}
	const int pic_pct = get_xr_section()->GetInt("xr_picture_size");
	const float pic_scale =
	        ((pic_pct < 20 ? 20 : (pic_pct > 100 ? 100 : pic_pct)) / 100.0f);
	const int pic_w = std::max(1, static_cast<int>(eye_w * pic_scale));
	const int pic_h = std::max(1, static_cast<int>(eye_h * pic_scale));
	const SDL_Rect dst_rect = {(eye_w - pic_w) / 2,
	                           (eye_h - pic_h) / 2,
	                           pic_w,
	                           pic_h};

	// Aspect-fitted picture rect: draw_rect_px already bakes in the
	// [render] 'aspect' option (corrected, square pixels, or stretch),
	// so fitting the destination to its aspect reproduces the desktop
	// geometry in the headset (letterboxed). Packed split modes and the
	// scanline-interleaved headset modes keep the stretched picture
	// rect (their eye buffers are halved views).
	SDL_Rect fit_rect = dst_rect;
	const SDL_Rect src_area = sdl.draw.draw_rect_px;
	if (src_area.w > 0 && src_area.h > 0) {
		if (pic_w * src_area.h > pic_h * src_area.w) {
			fit_rect.w = pic_h * src_area.w / src_area.h;
			fit_rect.x = (eye_w - fit_rect.w) / 2;
		} else {
			fit_rect.h = pic_w * src_area.h / src_area.w;
			fit_rect.y = (eye_h - fit_rect.h) / 2;
		}
	}

	// Null on the non-GL backends (no GL context); every use below is
	// branched on use_cpu first.
	auto* gl_renderer = use_cpu ? nullptr
	                          : static_cast<OpenGlRenderer*>(
	                                    sdl.renderer.get());
	const auto blit_t0_us = GetTicksUs();

	const bool eye_swap = get_xr_section()->GetBool("xr_eye_swap");
	const int xr_disparity = get_xr_section()->GetInt("xr_disparity");
	const bool is_anaglyph    = (stereo_mode == "anaglyph");
	const bool is_alternating = (stereo_mode == "alternating");
	const bool is_split       = (stereo_mode == "interleaved" ||
                               stereo_mode == "top-bottom" ||
                               stereo_mode == "sbs");
	const bool is_depth       = (stereo_mode == "depth");
	const bool is_autodepth   = (stereo_mode == "autodepth");
	const bool is_cybershades = (stereo_mode == "cybershades");
	const bool is_cybermaxx   = (stereo_mode == "cybermaxx");
	const bool is_iglasses   = (stereo_mode == "iglasses");
	// Scanline-interleaved headset modes: each eye is one half-height
	// field (see gui/cybermaxx.cpp, gui/iglasses.cpp).
	const bool is_line_interleaved = is_cybermaxx || is_iglasses;

	// Convergence offset: shift the eyes in opposite directions.
	// Positive converges (screen feels closer).
	const auto eye_dst_rect = [&](const uint32_t eye,
	                              const SDL_Rect& base) {
		const int shift = (eye == 0) ? (xr_disparity / 2)
		                             : -(xr_disparity / 2);
		const SDL_Rect r = {base.x + shift,
		                    base.y,
		                    base.w,
		                    base.h};
		return r;
	};

	// Remember the pictured rect (clamped to the eye) for the screen
	// panel, which crops its source image to exactly this region.
	for (int eye = 0; eye < 2; ++eye) {
		const SDL_Rect r = eye_dst_rect(static_cast<uint32_t>(eye),
		                                fit_rect);
		const int x0 = std::clamp(r.x, 0, eye_w);
		const int y0 = std::clamp(r.y, 0, eye_h);
		const int x1 = std::clamp(r.x + r.w, 0, eye_w);
		const int y1 = std::clamp(r.y + r.h, 0, eye_h);
		sdl_xr_picture_rect[eye][0] = x0;
		sdl_xr_picture_rect[eye][1] = y0;
		sdl_xr_picture_rect[eye][2] = std::max(0, x1 - x0);
		sdl_xr_picture_rect[eye][3] = std::max(0, y1 - y0);
	}

	const bool mono_path =
	        ((!is_anaglyph && !is_alternating && !is_cybershades &&
	          !is_split && !is_depth && !is_autodepth && !is_cybermaxx &&
	          !is_iglasses) ||
	         (is_depth && !sdl_xr_depth_valid));
	if (mono_path) {
		// Mono fast path: same picture in both eyes, pure GPU blit.
		// Also the fallback when depth mode has no fresh depth frame
		// (e.g. non-Voodoo content): the mirror already shows it.
		// Skipped without a swapchain (preview mono needs no work:
		// the desktop already shows the picture).
		if (have_swapchain) {
			for (uint32_t eye = 0;
			     eye < sdl_xr_view_count && eye < 2;
			     ++eye) {
				if (use_cpu) {
					// Same geometry, sourced from the game
					// frame (already bottom-up flipped).
					if (!cpu_frame.empty()) {
						xr_upload_eye(
						        static_cast<int>(eye),
						        cpu_frame.data(),
						        cpu_fw,
						        cpu_fh,
						        dst_img,
						        static_cast<int>(eye),
						        eye_dst_rect(eye,
						                     fit_rect),
						        eye_w,
						        eye_h);
					}
				} else {
					gl_renderer->BlitBackbufferToTexture(
					        static_cast<GLuint>(dst_img),
					        static_cast<int>(eye),
					        sdl.draw.draw_rect_px,
					        eye_dst_rect(eye, fit_rect),
					        eye_w,
					        eye_h);
				}
			}
		}
	} else if (is_depth) {
		// Depth-synthesized views from the Voodoo hook.
		if (have_swapchain) {
			for (uint32_t eye = 0;
			     eye < sdl_xr_view_count && eye < 2;
			     ++eye) {
				xr_upload_eye(static_cast<int>(eye),
				              xr_eye_buf[eye].data(),
				              xr_eye_buf_w,
				              xr_eye_buf_h,
				              dst_img,
				              static_cast<int>(eye),
				              eye_dst_rect(eye, fit_rect),
				              eye_w,
				              eye_h);
			}
		}
		sdl_xr_depth_valid = false;
	} else if (is_autodepth) {
		// Estimated-depth views from the 2D frame (no depth buffer).
		const SDL_Rect src = sdl.draw.draw_rect_px;
		const auto canvas  = sdl.renderer->GetCanvasSizeInPixels();
		const int fb_w     = iroundf(canvas.w);
		const int fb_h     = iroundf(canvas.h);

		int cx = src.x < 0 ? 0 : src.x;
		int cy = src.y < 0 ? 0 : src.y;
		int cw = src.w;
		int ch = src.h;
		if (cx + cw > fb_w) {
			cw = fb_w - cx;
		}
		if (cy + ch > fb_h) {
			ch = fb_h - cy;
		}

		if (use_cpu) {
			// Game frame for the estimate (bottom-up flipped
			// above); the window-fitted backbuffer rect only
			// exists on the GL backend.
			cw = cpu_fw;
			ch = cpu_fh;
		}
		if (cw > 0 && ch > 0) {
			std::vector<uint32_t> frame;
			if (use_cpu) {
				frame = std::move(cpu_frame);
			} else {
				frame = gl_renderer->ReadBackbufferRect(
				        {cx, cy, cw, ch});
			}
			const size_t frame_px = static_cast<size_t>(cw) *
			                        static_cast<size_t>(ch);
			if (frame.size() == frame_px) {
				if (xr_autodepth_map_w != cw ||
				    xr_autodepth_map_h != ch) {
					xr_autodepth_map.assign(frame_px, 0.0f);
					xr_autodepth_map_w = cw;
					xr_autodepth_map_h = ch;
				}
				// Heuristic depth estimate.
				const std::string auto_method =
				        get_xr_section()->GetString(
				                "xr_autodepth_method");
				// Temporal estimators restart when the method
				// changes (and on their own size changes).
				static std::string last_method;
				const bool temporal_reset =
				        (last_method != auto_method);
				last_method = auto_method;

				float depth_lo = 0.0f, depth_hi = 0.0f;
				bool flat      = false;
				if (auto_method == "detail") {
					flat = xr_estimate_depth_detail(
					        frame.data(),
					        cw,
					        ch,
					        xr_autodepth_map.data(),
					        depth_lo,
					        depth_hi);
				} else if (auto_method == "center") {
					flat = xr_estimate_depth_center(
					        cw,
					        ch,
					        xr_autodepth_map.data(),
					        depth_lo,
					        depth_hi);
				} else if (auto_method == "layers") {
					flat = xr_estimate_depth_layers(
					        frame.data(),
					        cw,
					        ch,
					        xr_autodepth_map.data(),
					        depth_lo,
					        depth_hi);
				} else if (auto_method == "background") {
					flat = xr_estimate_depth_background(
					        frame.data(),
					        cw,
					        ch,
					        xr_autodepth_map.data(),
					        depth_lo,
					        depth_hi,
					        temporal_reset);
				} else if (auto_method == "flow") {
					flat = xr_estimate_depth_flow(
					        frame.data(),
					        cw,
					        ch,
					        xr_autodepth_map.data(),
					        depth_lo,
					        depth_hi,
					        temporal_reset);
				} else {
					bool cue_grad = true, cue_luma = true;
					if (auto_method == "gradient") {
						cue_luma = false;
					} else if (auto_method == "luma") {
						cue_grad = false;
					}
					flat = xr_estimate_depth_auto(
					        frame.data(),
					        cw,
					        ch,
					        xr_autodepth_map.data(),
					        cue_grad,
					        cue_luma,
					        depth_lo,
					        depth_hi);
				}
				int pct = get_xr_section()->GetInt(
				        "xr_depth_strength");
				if (pct < 0) {
					pct = 0;
				}
				if (pct > 50) {
					pct = 50;
				}
				const float max_shift = (pct / 100.0f) * cw *
				                        0.5f;
				const bool show_depth =
				        get_xr_section()->GetBool("xr_show_depth");
				// Fold the percentile range (and optional
				// inversion) into the synthesizer, replacing the
				// old full-frame normalization pass.
				float depth_scale = 0.0f;
				float depth_bias  = 0.0f;
				if (!flat) {
					const float inv = 1.0f /
					                  (depth_hi - depth_lo);
					if (get_xr_section()->GetBool(
					            "xr_autodepth_invert")) {
						depth_scale = -inv;
						depth_bias  = 1.0f +
						              depth_lo * inv;
					} else {
						depth_scale = inv;
						depth_bias  = -depth_lo * inv;
					}
				}
				xr_synthesize_depth_views(frame.data(),
				                          xr_autodepth_map.data(),
				                          cw,
				                          ch,
				                          max_shift,
				                          show_depth,
				                          flat,
				                          false,
				                          depth_scale,
				                          depth_bias);
				if (have_swapchain) {
					for (uint32_t eye = 0;
					     eye < sdl_xr_view_count && eye < 2;
					     ++eye) {
						xr_upload_eye(static_cast<int>(eye),
						              xr_eye_buf[eye].data(),
						              xr_eye_buf_w,
						              xr_eye_buf_h,
						              dst_img,
						              static_cast<int>(eye),
						              eye_dst_rect(eye,
						                           fit_rect),
						              eye_w,
						              eye_h);
					}
				}
			}
		}
	} else {
		// Stereo path: per-eye images are produced on the CPU (from
		// the backbuffer draw area, or the pre-scaler game frame for
		// the scanline-interleaved headset modes) and uploaded for
		// GPU upscaling.
		const SDL_Rect src = sdl.draw.draw_rect_px;
		const auto canvas  = sdl.renderer->GetCanvasSizeInPixels();
		const int fb_w     = iroundf(canvas.w);
		const int fb_h     = iroundf(canvas.h);

		int cx = src.x < 0 ? 0 : src.x;
		int cy = src.y < 0 ? 0 : src.y;
		int cw = src.w;
		int ch = src.h;
		if (cx + cw > fb_w) {
			cw = fb_w - cx;
		}
		if (cy + ch > fb_h) {
			ch = fb_h - cy;
		}

		if (cw > 0 && ch > 0) {
			static uint64_t xr_alt_parity = 0;
			static std::string xr_eye_buf_mode;

			// Scanline-interleaved headset modes decode the
			// pre-scaler game frame (exact integer rows) instead
			// of the window-fitted backbuffer rect above. The
			// non-GL backends have no backbuffer to read, so
			// every stereo mode decodes the (flipped) game frame.
			std::vector<uint32_t> game_frame;
			if (use_cpu) {
				game_frame = cpu_frame;
				if (!game_frame.empty() && cpu_fw > 0 &&
				    cpu_fh > 0) {
					cw = cpu_fw;
					ch = cpu_fh;
					cx = 0;
					cy = 0;
				} else {
					game_frame.clear();
				}
			} else if (is_line_interleaved) {
				int gw = 0, gh = 0;
				game_frame = gl_renderer->ReadGameFrame(gw, gh);
				if (gw > 0 && gh > 0 &&
				    game_frame.size() ==
				            static_cast<size_t>(gw) *
				                    static_cast<size_t>(gh)) {
					cw = gw;
					ch = gh;
					cx = 0;
					cy = 0;
				} else {
					game_frame.clear();
				}
			}

		// Per-eye dimensions: split modes halve one axis, as do the
		// scanline-interleaved headset modes (each eye is one field).
		int ew = cw, eh = ch;
		if (is_split || is_line_interleaved) {
			if (stereo_mode == "sbs") {
				ew = cw / 2;
			} else { // interleaved + top-bottom + cybermaxx + iglasses
				eh = ch / 2;
			}
				if (ew < 1) {
					ew = 1;
				}
				if (eh < 1) {
					eh = 1;
				}
			}

			if (xr_eye_buf_w != ew || xr_eye_buf_h != eh ||
			    xr_eye_buf_mode != stereo_mode) {
				xr_eye_buf[0].assign(static_cast<size_t>(ew) *
				                             static_cast<size_t>(eh),
				                     0xFF000000u);
				xr_eye_buf[1].assign(static_cast<size_t>(ew) *
				                             static_cast<size_t>(eh),
				                     0xFF000000u);
				xr_eye_buf_w    = ew;
				xr_eye_buf_h    = eh;
				xr_eye_buf_mode = stereo_mode;
			}

			// Game-frame source for the headset modes (moved-from
			// above), backbuffer readback for everything else
			// (GL backend only).
			std::vector<uint32_t> frame;
			if (!game_frame.empty()) {
				frame = std::move(game_frame);
			} else if (!use_cpu) {
				frame = gl_renderer->ReadBackbufferRect(
				        {cx, cy, cw, ch});
			}

			const size_t frame_px = static_cast<size_t>(cw) *
			                            static_cast<size_t>(ch);
			const size_t eye_px = static_cast<size_t>(ew) *
			                      static_cast<size_t>(eh);
			if (frame.size() == frame_px &&
			    xr_eye_buf[0].size() == eye_px) {
				if (is_anaglyph) {
					const int li = eye_swap ? 1 : 0;
					const int ri = 1 - li;
					auto& dst_l  = xr_eye_buf[li];
					auto& dst_r  = xr_eye_buf[ri];
				if (get_xr_section()->GetString(
				            "xr_anaglyph_method") == "dubois") {
						// Dubois (2001) red/cyan least-squares
						// decode. Encoding is out = ML*L + MR*R
						// with the Dubois projection matrices;
						// assuming grayscale views this reduces
						// to out = l*a + r*b with
						// a = ML*(1,1,1), b = MR*(1,1,1), solved
						// here via the 2x2 normal equations.
						// (Coefficients match FFmpeg stereo3d.)
						constexpr float ax = 1.1328f;
						constexpr float ay = -0.0938f;
						constexpr float az = -0.0443f;
						constexpr float bx = -0.1351f;
						constexpr float by = 1.1743f;
						constexpr float bz = -0.0508f;
						constexpr float aa = 1.293997f;
						constexpr float ab = -0.260940f;
						constexpr float bb = 1.399813f;
						constexpr float inv_det = 1.0f / 1.743264f;
						for (size_t i = 0; i < frame.size(); ++i) {
							const uint32_t p = frame[i];
							const float R =
							        static_cast<float>(
							                (p >> 16) & 0xFFu);
							const float G =
							        static_cast<float>(
							                (p >> 8) & 0xFFu);
							const float B =
							        static_cast<float>(p & 0xFFu);
							const float adot =
							        ax * R + ay * G + az * B;
							const float bdot =
							        bx * R + by * G + bz * B;
							float l = (bb * adot - ab * bdot) *
							          inv_det;
							float r = (aa * bdot - ab * adot) *
							          inv_det;
							l = l < 0.0f ? 0.0f
							    : (l > 255.0f ? 255.0f : l);
							r = r < 0.0f ? 0.0f
							    : (r > 255.0f ? 255.0f : r);
							const uint32_t li8 =
							        static_cast<uint32_t>(l);
							const uint32_t ri8 =
							        static_cast<uint32_t>(r);
							dst_l[i] = 0xFF000000u |
							           (li8 << 16) | (li8 << 8) |
							           li8;
							dst_r[i] = 0xFF000000u |
							           (ri8 << 16) | (ri8 << 8) |
							           ri8;
						}
					} else {
						// Simple channel extraction (exact for
						// simply-encoded content): red drives
						// one eye, green+blue average drives
						// the other (grayscale, for viewing
						// without anaglyph glasses).
						for (size_t i = 0; i < frame.size();
						     ++i) {
							const uint32_t p = frame[i];
							const uint32_t r =
							        (p >> 16) & 0xFFu;
							const uint32_t g =
							        (p >> 8) & 0xFFu;
							const uint32_t b = p & 0xFFu;
							const uint32_t v = (g + b + 1u) >>
							                   1;
							dst_l[i] = 0xFF000000u |
							           (r << 16) | (r << 8) | r;
							dst_r[i] = 0xFF000000u |
							           (v << 16) | (v << 8) | v;
						}
					}
				} else if (is_alternating) {
					// Frame-sequential: this frame drives one
					// eye, the other keeps its last frame. Eye
					// source follows xr_shutter_sync: the
					// game's LPT1 data bit 2 when selected and
					// available (exact hardware sync), else blind
					// frame parity.
					const std::string shutter_mode =
					        get_xr_section()->GetString(
					                "xr_shutter_sync");
					uint8_t shutter         = 0;
					const bool shutter_fresh =
					        STEREO3D_ConsumeLptShutter(shutter);
					static uint8_t xr_shutter_last = 0x00;
					static bool xr_shutter_seen    = false;
					if (shutter_fresh) {
						xr_shutter_last = shutter;
						xr_shutter_seen = true;
					}
					int layer = 0;
					const bool use_shutter =
					        (shutter_mode == "lpt" && xr_shutter_seen) ||
					        (shutter_mode != "parity" && shutter_fresh);
					if (use_shutter) {
						static bool shutter_logged = false;
						if (!shutter_logged) {
							shutter_logged = true;
							LOG_MSG("OpenXR: LPT shutter sync active "
							        "(bit 2 drives eyes)");
						}
						const bool right =
						        (xr_shutter_last & 0x04) != 0;
						layer = ((right != eye_swap) ? 1 : 0);
					} else {
						layer = static_cast<int>(
						        (xr_alt_parity ^
						         (eye_swap ? 1u : 0u)) &
						        1u);
						++xr_alt_parity;
					}
				xr_eye_buf[layer] = frame;
			} else if (is_cybershades) {
				// Chinon CyberShades frame-sequential decode (see
				// gui/cyshades.cpp): same hold-last-frame behavior
				// as 'alternating' but with CyberShades shutter
				// polarity (LPT bit 2 set = left eye) plus COM-port
				// MCR sync.
				CYSHADES_SubmitFrame(xr_eye_buf[0],
				                     xr_eye_buf[1],
				                     frame);
		} else if (is_cybermaxx) {
			// VictorMaxx CyberMaxx scanline-interleaved
			// decode (see gui/cybermaxx.cpp): even/odd
			// rows de-woven to half-height fields (with
			// line-doubled-capture detection), GPU-upscaled
			// on upload.
			CYBERMAXX_SubmitFrame(xr_eye_buf[0],
			                     xr_eye_buf[1],
			                     frame,
			                     cw,
			                     ch,
			                     use_cpu);
		} else if (is_iglasses) {
			// Virtual i-O i-glasses! scanline-interleaved
			// decode (see gui/iglasses.cpp): even/odd
			// rows de-woven to half-height fields (first
			// scanline = left eye; line-doubled-capture
			// detection), GPU-upscaled on upload.
			IGLASSES_SubmitFrame(xr_eye_buf[0],
			                     xr_eye_buf[1],
			                     frame,
			                     cw,
			                     ch,
			                     use_cpu);
			} else {
					// Spatial split: "first" content (even rows /
					// top half / left half) drives one eye.
					const int fi = eye_swap ? 1 : 0;
					const int si = 1 - fi;
					auto& dst_f  = xr_eye_buf[fi];
					auto& dst_s  = xr_eye_buf[si];

					if (stereo_mode == "top-bottom") {
						// Contiguous halves: single copy each.
						std::memcpy(dst_f.data(),
						            frame.data(),
						            eye_px * sizeof(uint32_t));
						std::memcpy(dst_s.data(),
						            frame.data() + eye_px,
						            eye_px * sizeof(uint32_t));
					} else if (stereo_mode == "sbs") {
						for (int y = 0; y < eh; ++y) {
							const auto* src_row =
							        frame.data() +
							        static_cast<size_t>(y) *
							                static_cast<size_t>(cw);
							std::memcpy(dst_f.data() +
							                    static_cast<size_t>(y) *
							                            static_cast<size_t>(
							                                    ew),
							            src_row,
							            static_cast<size_t>(ew) *
							                    sizeof(uint32_t));
							std::memcpy(dst_s.data() +
							                    static_cast<size_t>(y) *
							                            static_cast<size_t>(
							                                    ew),
							            src_row + ew,
							            static_cast<size_t>(ew) *
							                    sizeof(uint32_t));
						}
					} else { // interleaved: even rows first eye
						for (int y = 0; y < eh; ++y) {
							std::memcpy(dst_f.data() +
							                    static_cast<size_t>(y) *
							                            static_cast<size_t>(
							                                    ew),
							            frame.data() +
							                    static_cast<size_t>(2 *
							                                        y) *
							                            static_cast<size_t>(
							                                    cw),
							            static_cast<size_t>(ew) *
							                    sizeof(uint32_t));
							std::memcpy(dst_s.data() +
							                    static_cast<size_t>(y) *
							                            static_cast<size_t>(
							                                    ew),
							            frame.data() +
							                    (static_cast<size_t>(2 *
							                                        y) +
							                     1) *
							                            static_cast<size_t>(
							                                    cw),
							            static_cast<size_t>(ew) *
							                    sizeof(uint32_t));
						}
					}
				}

				for (uint32_t eye = 0;
				     have_swapchain && eye < sdl_xr_view_count &&
				     eye < 2;
				     ++eye) {
					xr_upload_eye(static_cast<int>(eye),
					              xr_eye_buf[eye].data(),
					              ew,
					              eh,
					              dst_img,
					              static_cast<int>(eye),
				        eye_dst_rect(eye,
				                     (is_split || is_line_interleaved)
				                             ? dst_rect
				                             : fit_rect),
					              eye_w,
					              eye_h);
				}
			}
		}
	}

	// Optional second-window headset mirror: show the two eye images
	// that were just submitted. Decoded stereo modes have both eyes in
	// the retained buffers; mono (and depth with no fresh frame) uses
	// the same picture for both eyes from the game frame/backbuffer.
	// Only controlled by xr_mirror_window; xr_native_sbs renders in the
	// native window instead (see the separate native SBS block below).
	if (get_xr_section()->GetBool("xr_mirror_window")) {
		if (!mono_path && xr_eye_buf_w > 0 && xr_eye_buf_h > 0 &&
		    !xr_eye_buf[0].empty() && !xr_eye_buf[1].empty()) {
			XR_MirrorWindowUpdate(xr_eye_buf[0].data(),
			                      xr_eye_buf[1].data(),
			                      xr_eye_buf_w,
			                      xr_eye_buf_h);
		} else if (use_cpu) {
			if (!cpu_frame.empty() && cpu_fw > 0 && cpu_fh > 0) {
				XR_MirrorWindowUpdate(cpu_frame.data(),
				                      cpu_frame.data(),
				                      cpu_fw,
				                      cpu_fh);
			}
		} else if (gl_renderer) {
			const SDL_Rect src = sdl.draw.draw_rect_px;
			const auto canvas  = sdl.renderer->GetCanvasSizeInPixels();
			const int fb_w     = iroundf(canvas.w);
			const int fb_h     = iroundf(canvas.h);
			SDL_Rect rect      = {src.x < 0 ? 0 : src.x,
			                      src.y < 0 ? 0 : src.y,
			                      src.w,
			                      src.h};
			if (rect.x + rect.w > fb_w) {
				rect.w = fb_w - rect.x;
			}
			if (rect.y + rect.h > fb_h) {
				rect.h = fb_h - rect.y;
			}
			if (rect.w > 0 && rect.h > 0) {
				auto frame = gl_renderer->ReadBackbufferRect(rect);
				if (frame.size() == static_cast<size_t>(rect.w) *
				                           static_cast<size_t>(rect.h)) {
					XR_MirrorWindowUpdate(frame.data(),
					                      frame.data(),
					                      rect.w,
					                      rect.h);
				}
			}
		}
	}

	// Native side-by-side in the main window (no OpenXR runtime required):
	// composite the decoded left/right eye buffers side by side into the
	// renderer instead of the headset swapchain. Works with the SDL
	// software render backend; other backends ignore it (use the mirror
	// window there).
	if (get_xr_section()->GetBool("xr_native_sbs")) {
		const uint32_t* eye_l = nullptr;
		const uint32_t* eye_r = nullptr;
		int ew = 0, eh = 0;
		// Prefer decoded stereo eye buffers when a stereo mode ran.
		if (!mono_path && xr_eye_buf_w > 0 && xr_eye_buf_h > 0 &&
		    !xr_eye_buf[0].empty() && !xr_eye_buf[1].empty()) {
			eye_l = xr_eye_buf[0].data();
			eye_r = xr_eye_buf[1].data();
			ew = xr_eye_buf_w;
			eh = xr_eye_buf_h;
		} else if (!cpu_frame.empty() && cpu_fw > 0 && cpu_fh > 0) {
			// Mono: same frame in both eyes.
			eye_l = cpu_frame.data();
			eye_r = cpu_frame.data();
			ew = cpu_fw;
			eh = cpu_fh;
		} else if (!use_cpu && gl_renderer) {
			// GL backend, mono: read the backbuffer draw area.
			const SDL_Rect src = sdl.draw.draw_rect_px;
			const auto canvas  = sdl.renderer->GetCanvasSizeInPixels();
			const int fb_w     = iroundf(canvas.w);
			const int fb_h     = iroundf(canvas.h);
			SDL_Rect rect      = {src.x < 0 ? 0 : src.x,
			                      src.y < 0 ? 0 : src.y,
			                      src.w,
			                      src.h};
			if (rect.x + rect.w > fb_w) {
				rect.w = fb_w - rect.x;
			}
			if (rect.y + rect.h > fb_h) {
				rect.h = fb_h - rect.y;
			}
			if (rect.w > 0 && rect.h > 0) {
				std::vector<uint32_t> frame =
				        gl_renderer->ReadBackbufferRect(rect);
				if (frame.size() ==
				    static_cast<size_t>(rect.w) * rect.h) {
					// ReadBackbufferRect is top-down; flip to the
					// bottom-up eye-buffer convention.
					xr_flip_rows(frame, rect.w, rect.h);
					eye_l = frame.data();
					eye_r = frame.data();
					ew  = rect.w;
					eh  = rect.h;
				}
			}
		}
		if (eye_l && eye_r && ew > 0 && eh > 0) {
			sdl.renderer->RenderNativeSbs(eye_l, eye_r, ew, eh);
		}
	}

	// 2D stereo-decode demo in the mirror window: side-by-side eye
	// views from the decoded buffers (any non-mono mode; depth shows
	// the last frame when no fresh Voodoo frame arrived, which is what
	// the depth preview is for). GL backend only (needs GL readback
	// surfaces); the non-GL desktops simply show the DOS picture.
	if (!use_cpu && sdl_xr_mirror_stereo && stereo_mode != "mono" &&
	    xr_eye_buf_w > 0 && xr_eye_buf_h > 0 && !xr_eye_buf[0].empty() &&
	    !xr_eye_buf[1].empty()) {
		gl_renderer->BlitStereoPairToMirror(xr_eye_buf[0].data(),
		                                    xr_eye_buf[1].data(),
		                                    xr_eye_buf_w,
		                                    xr_eye_buf_h);
	}

	const auto blit_us = GetTicksDiff(GetTicksUs(), blit_t0_us);
	if (blit_us > 0) {
		sdl_xr_stat_blit_us += static_cast<uint64_t>(blit_us);
	}
}

#endif // C_OPENXR
