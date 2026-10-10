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
#include "gui/titlebar.h"
#include "gui/xr.h"
#include "gui/xrinput.h"
#include "gui/xrneural.h"
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
// Scratch for the neural half of fused modes (same dims as above).
static std::vector<float> xr_autodepth_aux;
static std::vector<uint32_t> xr_voodoo_argb;
static std::vector<float> xr_voodoo_norm;
static int xr_voodoo_buf_w = 0, xr_voodoo_buf_h = 0;

// Shared depth-stereo synthesizer: builds left/right views from color
// + normalized depth with a backward-mapped shift (stretched edges at
// depth discontinuities). depth_norm runs 1 = near (full max_shift)
// to 0 = far, sharing color_argb's row order; null reads as flat
// (mono-equivalent output). src_top_down adapts top-down sources
// (Voodoo FBI) to the bottom-up eye buffers. Fully overwrites both.
static void xr_synthesize_depth_views(const uint32_t* color_argb,
                                      const float* depth_norm,
                                      const int w, const int h,
                                      const float max_shift,
                                      const bool show_depth,
                                      const bool flat,
                                      const bool src_top_down)
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
	for (int e = 0; e < 2; ++e) {
		const float sign = (e == 0) ? 1.0f : -1.0f;
		auto* dst        = xr_eye_buf[e].data();
		for (int y = 0; y < h; ++y) {
			const auto* c_row = color_argb +
			                    static_cast<size_t>(y) * w;
			const auto* d_row = depth_norm
			        ? depth_norm + static_cast<size_t>(y) * w
			        : nullptr;
			const int out_y = src_top_down ? (h - 1 - y) : y;
			auto* d_row_out = dst + static_cast<size_t>(out_y) * w;
			for (int x = 0; x < w; ++x) {
				const float dn = d_row ? d_row[x] : 0.0f;
				if (show_depth) {
					// Debug view: raw normalized depth, no shift.
					uint32_t g = 0;
					if (!d_row) {
						g = 0; // no depth buffer
					} else if (flat) {
						g = 128; // flat depth
					} else {
						g = static_cast<uint32_t>(dn * 255.0f);
					}
					d_row_out[x] = 0xFF000000u | (g << 16) |
					               (g << 8) | g;
					continue;
				}
				int sx = static_cast<int>(x + sign * dn * max_shift +
				                          0.5f);
				if (sx < 0) {
					sx = 0;
				} else if (sx >= w) {
					sx = w - 1;
				}
				d_row_out[x] = c_row[sx];
			}
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
	                          !flip_v);
	sdl_xr_depth_valid = true;
}

// Heuristic monocular depth for the 'autodepth' stereo mode: no depth
// buffer exists for 2D content, so near/far is estimated from the
// frame itself. frame is bottom-up ARGB32 (0xAARRGGBB, row 0 = screen
// bottom); depth_norm runs 1 = near to 0 = far. Cues: a vertical
// ground-plane gradient (screen bottom reads as near) and luminance
// (bright reads as near), normalized per frame like the Voodoo path so
// flat frames collapse to mono. Returns true when flat.
static bool xr_estimate_depth_auto(const uint32_t* frame, const int w,
                                   const int h, float* depth_norm,
                                   const bool use_grad, const bool use_luma)
{
	const bool invert =
	        get_xr_section()->GetBool("xr_autodepth_invert");
	const float y_denom = (h > 1) ? 1.0f / (h - 1) : 0.0f;

	float dmin = std::numeric_limits<float>::max();
	float dmax = std::numeric_limits<float>::lowest();
	for (int y = 0; y < h; ++y) {
		const auto* row = frame + static_cast<size_t>(y) * w;
		auto* d_row = depth_norm + static_cast<size_t>(y) * w;
		const float grad = 1.0f - y * y_denom; // bottom-up: row 0 near
		for (int x = 0; x < w; ++x) {
			const uint32_t p = row[x];
			const float r = static_cast<float>((p >> 16) & 0xFFu);
			const float g = static_cast<float>((p >> 8) & 0xFFu);
			const float b = static_cast<float>(p & 0xFFu);
			const float luma = (0.299f * r + 0.587f * g +
			                    0.114f * b) /
			                   255.0f;
			float d = 0.0f;
			if (use_grad && use_luma) {
				d = 0.5f * grad + 0.5f * luma;
			} else if (use_grad) {
				d = grad;
			} else if (use_luma) {
				d = luma;
			}
			d_row[x] = d;
			if (d < dmin) {
				dmin = d;
			}
			if (d > dmax) {
				dmax = d;
			}
		}
	}
	if (dmax <= dmin) {
		for (int i = 0; i < w * h; ++i) {
			depth_norm[i] = 0.0f;
		}
		return true;
	}
	const float inv_range = 1.0f / (dmax - dmin);
	for (int i = 0; i < w * h; ++i) {
		float d = (depth_norm[i] - dmin) * inv_range;
		depth_norm[i] = invert ? (1.0f - d) : d;
	}
	return false;
}

// Called by OpenGlRenderer::PresentFrame() after rendering the mirror
// image and before the buffer swap, while the backbuffer is valid.
// Copies the DOS draw area into every eye slice of the acquired
// swapchain image. No-op when no XR frame is in flight.
void OPENXR_BlitMirrorToSwapchain()
{
	const std::string stereo_mode =
	        get_xr_section()->GetString("xr_stereo_input");
	const bool have_swapchain = (sdl_xr_active_image >= 0 &&
	                             !sdl_xr_swapchain_images.empty() &&
	                             sdl_xr_view_count > 0);
	// Headset-free stereo preview: with no XR frame in flight, still
	// decode into the eye buffers for the desktop mirror when the
	// mirror-stereo preview is on (Ctrl+O).
	const bool want_preview = (!have_swapchain && sdl_xr_mirror_stereo &&
	                           stereo_mode != "mono");
	if (!have_swapchain && !want_preview) {
		return;
	}
	if (sdl.render_backend_type != RenderBackendType::OpenGl ||
	    !sdl.renderer) {
		return;
	}
	// uint32_t (not GLuint): keeps this TU compiling when C_OPENGL
	// is off, as glad types are unavailable there.
	uint32_t dst_tex  = 0;
	int eye_w = 1, eye_h = 1;
	if (have_swapchain) {
		const uint32_t image_index =
		        static_cast<uint32_t>(sdl_xr_active_image);
		if (image_index >= sdl_xr_swapchain_images.size()) {
			return;
		}
		dst_tex = sdl_xr_swapchain_images[image_index].image;
		if (!dst_tex) {
			return;
		}
		eye_w = static_cast<int>(sdl_xr_swapchain_width);
		eye_h = static_cast<int>(sdl_xr_swapchain_height);
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
	// geometry in the headset (letterboxed). Packed split modes keep
	// the stretched picture rect (their eye buffers are halved views).
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

	auto* gl_renderer = static_cast<OpenGlRenderer*>(sdl.renderer.get());
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
	const bool is_cybermaxx   = (stereo_mode == "cybermaxx");

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

	if ((!is_anaglyph && !is_alternating && !is_split && !is_depth &&
	     !is_autodepth && !is_cybermaxx) ||
	    (is_depth && !sdl_xr_depth_valid)) {
		// Mono fast path: same picture in both eyes, pure GPU blit.
		// Also the fallback when depth mode has no fresh depth frame
		// (e.g. non-Voodoo content): the mirror already shows it.
		// Skipped without a swapchain (preview mono needs no work:
		// the desktop already shows the picture).
		if (have_swapchain) {
			for (uint32_t eye = 0;
			     eye < sdl_xr_view_count && eye < 2;
			     ++eye) {
				gl_renderer->BlitBackbufferToTexture(
				        dst_tex,
				        static_cast<int>(eye),
				        sdl.draw.draw_rect_px,
				        eye_dst_rect(eye, fit_rect),
				        eye_w,
				        eye_h);
			}
		}
	} else if (is_depth) {
		// Depth-synthesized views from the Voodoo hook.
		if (have_swapchain) {
			for (uint32_t eye = 0;
			     eye < sdl_xr_view_count && eye < 2;
			     ++eye) {
				gl_renderer->UploadAndBlitEye(
				        static_cast<int>(eye),
				        xr_eye_buf[eye].data(),
				        xr_eye_buf_w,
				        xr_eye_buf_h,
				        dst_tex,
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

		if (cw > 0 && ch > 0) {
			const auto frame = gl_renderer->ReadBackbufferRect(
			        {cx, cy, cw, ch});
			const size_t frame_px = static_cast<size_t>(cw) *
			                        static_cast<size_t>(ch);
			if (frame.size() == frame_px) {
				if (xr_autodepth_map_w != cw ||
				    xr_autodepth_map_h != ch) {
					xr_autodepth_map.assign(frame_px, 0.0f);
					xr_autodepth_aux.assign(frame_px, 0.0f);
					xr_autodepth_map_w = cw;
					xr_autodepth_map_h = ch;
				}
				// Resolve the estimate: pure neural, neural fused
				// with one heuristic cue, or pure heuristic. A
				// missing neural map degrades to the cue alone.
				const std::string auto_method =
				        get_xr_section()->GetString(
				                "xr_autodepth_method");
				const bool want_neural =
				        xr_autodepth_wants_neural(auto_method);
				const bool want_fusion =
				        xr_autodepth_is_fusion(auto_method);
				bool cue_grad = true, cue_luma = true;
				if (auto_method == "gradient" ||
				    auto_method == "neural+gradient") {
					cue_luma = false;
				} else if (auto_method == "luma" ||
				           auto_method == "neural+luma") {
					cue_grad = false;
				}
				bool use_neural_map = false;
				bool flat           = false;
#if C_NEURAL
				if (want_neural) {
					xr_neural_submit_frame(frame.data(), cw, ch);
					xr_neural_poll_missing_toast();
					if (want_fusion) {
						bool neural_flat = false;
						if (xr_neural_fill_map(
						            xr_autodepth_aux.data(),
						            cw,
						            ch,
						            neural_flat)) {
							bool cue_flat =
							        xr_estimate_depth_auto(
							                frame.data(),
							                cw,
							                ch,
							                xr_autodepth_map.data(),
							                cue_grad,
							                cue_luma);
							for (size_t i = 0; i < frame_px;
							     ++i) {
								xr_autodepth_map[i] =
								        0.5f *
								                xr_autodepth_map[i] +
								        0.5f *
								                xr_autodepth_aux[i];
							}
							flat = cue_flat && neural_flat;
							use_neural_map = true;
						}
					} else {
						bool neural_flat = false;
						use_neural_map = xr_neural_fill_map(
						        xr_autodepth_map.data(),
						        cw,
						        ch,
						        neural_flat);
						if (use_neural_map) {
							flat = neural_flat;
						}
					}
				}
#endif
				if (!use_neural_map) {
					flat = xr_estimate_depth_auto(
					        frame.data(),
					        cw,
					        ch,
					        xr_autodepth_map.data(),
					        cue_grad,
					        cue_luma);
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
				xr_synthesize_depth_views(frame.data(),
				                          xr_autodepth_map.data(),
				                          cw,
				                          ch,
				                          max_shift,
				                          show_depth,
				                          flat,
				                          false);
				if (have_swapchain) {
					for (uint32_t eye = 0;
					     eye < sdl_xr_view_count && eye < 2;
					     ++eye) {
						gl_renderer->UploadAndBlitEye(
						        static_cast<int>(eye),
						        xr_eye_buf[eye].data(),
						        xr_eye_buf_w,
						        xr_eye_buf_h,
						        dst_tex,
						        static_cast<int>(eye),
						        eye_dst_rect(eye, fit_rect),
						        eye_w,
						        eye_h);
					}
				}
			}
		}
	} else {
		// Stereo path: read back the draw area once, then produce
		// per-eye images on the CPU and upload them for GPU upscaling.
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

			// Per-eye dimensions: split modes halve one axis.
			int ew = cw, eh = ch;
			if (is_split) {
				if (stereo_mode == "sbs") {
					ew = cw / 2;
				} else { // interleaved + top-bottom
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

			const auto frame = gl_renderer->ReadBackbufferRect(
			        {cx, cy, cw, ch});

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
			} else if (is_cybermaxx) {
				// Field-sequential bob decode (CyberMaxx test mode):
				// the signal carries alternating temporal fields in
				// even/odd rows, so each eye gets a bob-deinterlaced
				// half built from a single field. Each eye is always
				// self-consistent (no within-eye doubling); static
				// content coincides in both eyes, motion shows the
				// field offset exactly as the shutters would. No
				// phase to track, so nothing can drift.
				const int fi = eye_swap ? 1 : 0;
				const int si = 1 - fi;
				auto& dst_f  = xr_eye_buf[fi];
				auto& dst_s  = xr_eye_buf[si];
				for (int y = 0; y < eh; ++y) {
					const int src_fy =
					        y & ~1; // even field row
					int src_sy = y | 1; // odd field row
					if (src_sy >= ch) {
						src_sy = y;
					}
					std::memcpy(dst_f.data() +
					                    static_cast<size_t>(y) *
					                            static_cast<size_t>(
					                                    ew),
					            frame.data() +
					                    static_cast<size_t>(src_fy) *
					                            static_cast<size_t>(
					                                    cw),
					            static_cast<size_t>(ew) *
					                    sizeof(uint32_t));
					std::memcpy(dst_s.data() +
					                    static_cast<size_t>(y) *
					                            static_cast<size_t>(
					                                    ew),
					            frame.data() +
					                    static_cast<size_t>(src_sy) *
					                            static_cast<size_t>(
					                                    cw),
					            static_cast<size_t>(ew) *
					                    sizeof(uint32_t));
				}
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
					gl_renderer->UploadAndBlitEye(
					        static_cast<int>(eye),
					        xr_eye_buf[eye].data(),
					        ew,
					        eh,
					        dst_tex,
					        static_cast<int>(eye),
					        eye_dst_rect(eye,
					                     is_split ? dst_rect
					                              : fit_rect),
					        eye_w,
					        eye_h);
				}
			}
		}
	}

	// 2D stereo-decode demo in the mirror window: side-by-side eye
	// views from the decoded buffers (any non-mono mode; depth shows
	// the last frame when no fresh Voodoo frame arrived, which is what
	// the depth preview is for).
	if (sdl_xr_mirror_stereo && stereo_mode != "mono" &&
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
