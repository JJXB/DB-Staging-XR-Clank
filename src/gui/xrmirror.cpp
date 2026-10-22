// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Optional desktop "headset mirror" window. It is a separate SDL
// window so the normal DOSBox window keeps showing the desktop view;
// this one shows the left/right eye images that were submitted to the
// headset, side by side.

#include "gui/xrmirror.h"

#include <algorithm>
#include <cstring>

#include "config/setup.h"
#include "misc/support.h"

#if C_OPENXR

namespace {

SDL_Window* mirror_window       = nullptr;
SDL_Renderer* mirror_renderer   = nullptr;
SDL_Texture* mirror_texture[2]  = {};
int mirror_texture_w            = 0;
int mirror_texture_h            = 0;

// Right-clicking the mirror body cycles both eyes -> left -> right.
enum class MirrorView { Both, Left, Right };
MirrorView mirror_view = MirrorView::Both;

void destroy_window()
{
	for (auto& texture : mirror_texture) {
		if (texture) {
			SDL_DestroyTexture(texture);
			texture = nullptr;
		}
	}
	mirror_texture_w = 0;
	mirror_texture_h = 0;

	if (mirror_renderer) {
		SDL_DestroyRenderer(mirror_renderer);
		mirror_renderer = nullptr;
	}
	if (mirror_window) {
		SDL_DestroyWindow(mirror_window);
		mirror_window = nullptr;
	}
}

bool ensure_window()
{
	if (mirror_window && mirror_renderer) {
		return true;
	}
	destroy_window();

	mirror_window = SDL_CreateWindow("DOSBox-XR Headset Mirror",
	                                 960,
	                                 540,
	                                 SDL_WINDOW_RESIZABLE |
	                                         SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!mirror_window) {
		LOG_WARNING("OpenXR: headset mirror window creation failed: %s",
		            SDL_GetError());
		return false;
	}

	// Do not inherit the driver the main window was forced onto (the
	// Vulkan/D3D renderers leave SDL_HINT_RENDER_DRIVER set): a second
	// renderer on the same Vulkan device presents an empty window.
	SDL_ResetHint(SDL_HINT_RENDER_DRIVER);
#if defined(_WIN32)
	mirror_renderer = SDL_CreateRenderer(mirror_window, "direct3d11");
#endif
	if (!mirror_renderer) {
		mirror_renderer = SDL_CreateRenderer(mirror_window,
		                                    SDL_SOFTWARE_RENDERER);
	}
	if (!mirror_renderer) {
		mirror_renderer = SDL_CreateRenderer(mirror_window, nullptr);
	}
	if (!mirror_renderer) {
		LOG_WARNING("OpenXR: headset mirror renderer creation failed: %s",
		            SDL_GetError());
		destroy_window();
		return false;
	}
	LOG_MSG("OpenXR: headset mirror renderer '%s'",
	        SDL_GetRendererName(mirror_renderer));
	mirror_view = MirrorView::Both;
	// The main present path paces us; never block on the mirror.
	SDL_SetRenderVSync(mirror_renderer, 0);
	SDL_ShowWindow(mirror_window);
	return true;
}

bool ensure_textures(const int w_px, const int h_px)
{
	if (w_px <= 0 || h_px <= 0) {
		return false;
	}
	if (mirror_texture[0] && mirror_texture[1] &&
	    mirror_texture_w == w_px && mirror_texture_h == h_px) {
		return true;
	}
	for (auto& texture : mirror_texture) {
		if (texture) {
			SDL_DestroyTexture(texture);
			texture = nullptr;
		}
	}
	for (auto& texture : mirror_texture) {
		texture = SDL_CreateTexture(mirror_renderer,
		                            SDL_PIXELFORMAT_ARGB8888,
		                            SDL_TEXTUREACCESS_STREAMING,
		                            w_px,
		                            h_px);
		if (!texture) {
			LOG_WARNING("OpenXR: headset mirror texture creation "
			            "failed: %s",
			            SDL_GetError());
			destroy_window();
			return false;
		}
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
	}
	mirror_texture_w = w_px;
	mirror_texture_h = h_px;
	return true;
}

void upload_eye(SDL_Texture* texture, const uint32_t* pixels,
                const int w_px, const int h_px)
{
	void* dst      = nullptr;
	int dst_pitch  = 0;
	if (!SDL_LockTexture(texture, nullptr, &dst, &dst_pitch)) {
		return;
	}
	// Eye buffers are bottom-up (the upload convention shared with
	// the headset eye textures); SDL textures are top-down. Force the
	// alpha byte opaque: some backends leave it at zero in the source
	// frame, which would blend the whole mirror away.
	for (int y = 0; y < h_px; ++y) {
		auto* dst_row = reinterpret_cast<uint32_t*>(
		        static_cast<uint8_t*>(dst) +
		        static_cast<size_t>(y) * dst_pitch);
		const auto* src_row =
		        pixels + static_cast<size_t>(h_px - 1 - y) * w_px;
		for (int x = 0; x < w_px; ++x) {
			dst_row[x] = src_row[x] | 0xFF000000u;
		}
	}
	SDL_UnlockTexture(texture);
}

void present_pair()
{
	if (!mirror_renderer || !mirror_texture[0] || !mirror_texture[1]) {
		return;
	}
	int out_w = 0, out_h = 0;
	if (!SDL_GetRenderOutputSize(mirror_renderer, &out_w, &out_h) ||
	    out_w <= 0 || out_h <= 0) {
		return;
	}
	SDL_SetRenderDrawColor(mirror_renderer, 0, 0, 0, 255);
	SDL_RenderClear(mirror_renderer);

	const float out_w_f = static_cast<float>(out_w);
	const float out_h_f = static_cast<float>(out_h);

	if (mirror_view == MirrorView::Both) {
		const float half_w = out_w_f / 2.0f;
		const float scale  = std::min(
		        half_w / static_cast<float>(mirror_texture_w),
		        out_h_f / static_cast<float>(mirror_texture_h));
		const float draw_w = static_cast<float>(mirror_texture_w) * scale;
		const float draw_h = static_cast<float>(mirror_texture_h) * scale;
		const float y      = (out_h_f - draw_h) / 2.0f;

		const SDL_FRect left_rect = {(half_w - draw_w) / 2.0f,
		                             y,
		                             draw_w,
		                             draw_h};
		const SDL_FRect right_rect = {half_w + (half_w - draw_w) / 2.0f,
		                              y,
		                              draw_w,
		                              draw_h};
		SDL_RenderTexture(
		        mirror_renderer, mirror_texture[0], nullptr, &left_rect);
		SDL_RenderTexture(
		        mirror_renderer, mirror_texture[1], nullptr, &right_rect);
	} else {
		const int eye = (mirror_view == MirrorView::Left) ? 0 : 1;
		const float scale = std::min(
		        out_w_f / static_cast<float>(mirror_texture_w),
		        out_h_f / static_cast<float>(mirror_texture_h));
		const float draw_w = static_cast<float>(mirror_texture_w) * scale;
		const float draw_h = static_cast<float>(mirror_texture_h) * scale;
		const SDL_FRect dst = {(out_w_f - draw_w) / 2.0f,
		                       (out_h_f - draw_h) / 2.0f,
		                       draw_w,
		                       draw_h};
		SDL_RenderTexture(
		        mirror_renderer, mirror_texture[eye], nullptr, &dst);
	}
	SDL_RenderPresent(mirror_renderer);
}

} // namespace

void XR_MirrorWindowUpdate(const uint32_t* eye_left, const uint32_t* eye_right,
                           const int w_px, const int h_px)
{
	const auto* section = get_section("xr");
	if (!section || !section->GetBool("xr_mirror_window")) {
		destroy_window();
		return;
	}
	if (!eye_left || !eye_right || w_px <= 0 || h_px <= 0) {
		return;
	}
	if (!ensure_window() || !ensure_textures(w_px, h_px)) {
		return;
	}
	static bool logged_first_frame = false;
	if (!logged_first_frame) {
		logged_first_frame = true;
		const auto count = static_cast<size_t>(w_px) *
		                   static_cast<size_t>(h_px);
		size_t nonzero = 0;
		for (size_t i = 0; i < count; ++i) {
			if (eye_left[i] & 0x00FFFFFFu) {
				++nonzero;
			}
		}
		LOG_MSG("OpenXR: headset mirror first frame %dx%d, %zu/%zu "
		        "non-black pixels",
		        w_px,
		        h_px,
		        nonzero,
		        count);
	}
	upload_eye(mirror_texture[0], eye_left, w_px, h_px);
	upload_eye(mirror_texture[1], eye_right, w_px, h_px);
	present_pair();
}

bool XR_MirrorWindowHandleEvent(const SDL_Event& event)
{
	if (!mirror_window) {
		return false;
	}
	const auto mirror_id = SDL_GetWindowID(mirror_window);
	if (!mirror_id) {
		return false;
	}

	if (event.type >= SDL_EVENT_WINDOW_FIRST &&
	    event.type <= SDL_EVENT_WINDOW_LAST) {
		if (event.window.windowID != mirror_id) {
			return false;
		}
		if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
			LOG_MSG("OpenXR: headset mirror window closed");
			set_section_property_value("xr",
			                           "xr_mirror_window",
			                           "false");
			destroy_window();
		}
		// Resize/expose/move are handled by the SDL renderer.
		return true;
	}

	// Keep keyboard/mouse events aimed at the passive mirror window
	// from reaching the emulated machine.
	Uint32 event_window_id = 0;
	switch (event.type) {
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
		event_window_id = event.key.windowID;
		break;
	case SDL_EVENT_TEXT_INPUT:
	case SDL_EVENT_TEXT_EDITING:
		event_window_id = event.text.windowID;
		break;
	case SDL_EVENT_MOUSE_MOTION:
		event_window_id = event.motion.windowID;
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		event_window_id = event.button.windowID;
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		event_window_id = event.wheel.windowID;
		break;
	default: return false;
	}
	if (event_window_id != mirror_id) {
		return false;
	}

	// Right-click in the window body cycles the displayed view.
	if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
	    event.button.button == SDL_BUTTON_RIGHT) {
		switch (mirror_view) {
		case MirrorView::Both: mirror_view = MirrorView::Left; break;
		case MirrorView::Left: mirror_view = MirrorView::Right; break;
		case MirrorView::Right: mirror_view = MirrorView::Both; break;
		}
		const char* const label = (mirror_view == MirrorView::Both)
		                                ? "both eyes"
		                                : (mirror_view == MirrorView::Left)
		                                          ? "left eye"
		                                          : "right eye";
		LOG_MSG("OpenXR: headset mirror showing %s", label);
	}
	return true;
}

void XR_MirrorWindowShutdown()
{
	destroy_window();
}

#endif // C_OPENXR
