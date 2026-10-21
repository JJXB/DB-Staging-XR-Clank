// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "d3d12_renderer.h"

#include <cstring>
#include <memory>

#include <SDL3/SDL.h>

#include "gui/private/common.h"
#if C_OPENXR
#include "gui/osd.h"
#endif

#include "capture/capture.h"
#include "config/setup.h"
#include "misc/video.h"
#include "utils/checks.h"
#include "utils/math_utils.h"

CHECK_NARROWING();

#if C_OPENXR
// Defined in gui/xrstereo.cpp: copies the game frame into the active
// OpenXR swapchain image when an XR frame is in flight.
void OPENXR_BlitMirrorToSwapchain();
#endif

static constexpr SDL_PixelFormat D3D12PixelFormat = SDL_PIXELFORMAT_XRGB8888;

// The single SDL render driver this backend runs on.
static constexpr const char* D3DDriverName = "direct3d12";

D3D12Renderer::D3D12Renderer(const int x, const int y,
                               const int width, const int height,
                               const SDL_WindowFlags sdl_window_flags,
                               TextureFilterMode texture_filter_mode)
        : texture_filter_mode(texture_filter_mode)
{
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, DOSBOX_NAME);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);

	// For window flags you should use separate window creation properties,
	// but for easier migration from SDL2 you can use the following:
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, sdl_window_flags);
	window = SDL_CreateWindowWithProperties(props);

	SDL_DestroyProperties(props);

	if (!window) {
		const auto msg = format_str("D3D12: Error creating window: %s",
		                            SDL_GetError());
		LOG_ERR("%s", msg.c_str());
		throw std::runtime_error(msg);
	}

	if (!InitRenderer()) {
		SDL_DestroyWindow(window);
		window = {};

		const auto msg = "D3D12: Error creating Direct3D 12-backed SDL renderer "
		                 "(no 'direct3d12' render driver available)";
		LOG_ERR("%s", msg);
		throw std::runtime_error(msg);
	}
}

bool D3D12Renderer::InitRenderer()
{
	if (!SDL_SetHint(SDL_HINT_RENDER_DRIVER, D3DDriverName)) {
		LOG_WARNING("D3D12: Error setting '%s' SDL render driver hint: %s",
	            D3DDriverName,
	            SDL_GetError());
	}

	renderer = SDL_CreateRenderer(window, nullptr);
	if (!renderer) {
		LOG_ERR("D3D12: No 'direct3d12' SDL render driver found; "
	        "list SDL render drivers via the debug log");
		const auto count = SDL_GetNumRenderDrivers();
		for (int i = 0; i < count; ++i) {
			LOG_MSG("D3D12: Available SDL render driver: '%s'",
		        SDL_GetRenderDriver(i));
		}
		if (!SDL_ResetHint(SDL_HINT_RENDER_DRIVER)) {
			LOG_WARNING("D3D12: Error resetting SDL render driver hint: %s",
	            SDL_GetError());
		}
		return false;
	}

	LOG_MSG("D3D12: Using '%s' SDL render driver",
	        SDL_GetRendererName(renderer));

	if (!SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE)) {
		LOG_ERR("D3D12: Error setting render clear color: %s", SDL_GetError());
	}

	return true;
}

D3D12Renderer::~D3D12Renderer()
{
	if (renderer) {
		// Frees associated textures automatically.
		SDL_DestroyRenderer(renderer);
		renderer = {};
		texture  = {};
	}
	if (curr_framebuf.surface) {
		SDL_DestroySurface(curr_framebuf.surface);
		curr_framebuf = {};
	}
	if (last_framebuf.surface) {
		SDL_DestroySurface(last_framebuf.surface);
		last_framebuf = {};
	}
	if (window) {
		SDL_DestroyWindow(window);
		window = {};
	}
}

SDL_Window* D3D12Renderer::GetWindow()
{
	return window;
}

DosBox::Rect D3D12Renderer::GetCanvasSizeInPixels()
{
	SDL_Rect canvas_size_px = {};
	SDL_GetWindowSizeInPixels(window, &canvas_size_px.w, &canvas_size_px.h);

	const auto r = to_rect(canvas_size_px);
	assert(r.HasPositiveSize());

	return r;
}

void D3D12Renderer::NotifyViewportSizeChanged(const DosBox::Rect draw_rect_px)
{
	const auto sdl_draw_rect_px = to_sdl_rect(draw_rect_px);

	if (!SDL_SetRenderViewport(renderer, &sdl_draw_rect_px)) {
		LOG_ERR("D3D12: Error setting viewport: %s", SDL_GetError());
	}
}

void D3D12Renderer::NotifyRenderSizeChanged(const int render_width_px,
                                             const int render_height_px)
{
	if (texture) {
		SDL_DestroyTexture(texture);
		texture = {};
	}

	texture = SDL_CreateTexture(renderer,
	                            D3D12PixelFormat,
	                            SDL_TEXTUREACCESS_STREAMING,
	                            render_width_px,
	                            render_height_px);
	if (!texture) {
		LOG_ERR("D3D12: Error creating SDL texture: %s", SDL_GetError());
		return;
	}

	switch (texture_filter_mode) {
	case TextureFilterMode::NearestNeighbour:
		if (!SDL_SetTextureScaleMode(texture,
		                            SDL_ScaleMode::SDL_SCALEMODE_NEAREST)) {
			LOG_ERR("D3D12: Error setting texture filtering mode: %s",
			        SDL_GetError());
		}
		break;

	case TextureFilterMode::Bilinear:
		if (!SDL_SetTextureScaleMode(texture,
		                            SDL_ScaleMode::SDL_SCALEMODE_LINEAR)) {
			LOG_ERR("D3D12: Error setting texture filtering mode: %s",
			        SDL_GetError());
		}
		break;

	default: assertm(false, "Invalid TextureFilterMode"); return;
	}

	if (curr_framebuf.surface) {
		SDL_DestroySurface(curr_framebuf.surface);
		curr_framebuf = {};
	}
	if (last_framebuf.surface) {
		SDL_DestroySurface(last_framebuf.surface);
		last_framebuf = {};
	}

	curr_framebuf.surface = SDL_CreateSurface(render_width_px,
	                                          render_height_px,
	                                          D3D12PixelFormat);

	last_framebuf.surface = SDL_CreateSurface(render_width_px,
	                                          render_height_px,
	                                          D3D12PixelFormat);

	if (!curr_framebuf.surface || !last_framebuf.surface) {
		SDL_DestroyTexture(texture);
		texture = {};
		LOG_ERR("D3D12: Error creating input surface: %s", SDL_GetError());
		return;
	}
}

D3D12Renderer::SetShaderResult D3D12Renderer::SetShader(
        [[maybe_unused]] const std::string& symbolic_shader_descriptor)
{
	// No shader support yet; always report success.
	//
	// If we didn't, the rendering backend agnostic fallback mechanism would
	// fail and we'd hard exit. Direct3D 12 shader support
	// is a planned follow-up; until then the OpenGL backend remains the
	// shader-capable option.
	return SetShaderResult::Ok;
}

void D3D12Renderer::NotifyVideoModeChanged(
        [[maybe_unused]] const VideoMode& video_mode)
{
	// no shader support
	return;
}

void D3D12Renderer::ForceReloadCurrentShader()
{
	// no shader support
	return;
}

ShaderInfo D3D12Renderer::GetCurrentShaderInfo()
{
	// no shader support
	return {};
}

ShaderPreset D3D12Renderer::GetCurrentShaderPreset()
{
	// no shader support
	return {};
}

std::string D3D12Renderer::GetCurrentSymbolicShaderDescriptor()
{
	// no shader support
	return {};
}

ShaderDescriptor D3D12Renderer::GetCurrentShaderDescriptor()
{
	// no-op (no shader support)
	return {};
}

void D3D12Renderer::StartFrame(uint32_t*& pixels_out, int& pitch_out)
{
	assert(curr_framebuf.surface);

	// Some surfaces must be locked for direct access
	curr_framebuf.LockSurface();

	pixels_out = static_cast<uint32_t*>(curr_framebuf.surface->pixels);
	pitch_out  = curr_framebuf.surface->pitch;
}

void D3D12Renderer::EndFrame()
{
	assert(curr_framebuf.surface);
	assert(last_framebuf.surface);

	last_framebuf.LockSurface();

	// We need to copy the buffers. We can't just swap them because the VGA
	// emulation only writes the changed pixels to the framebuffer in each
	// frame.
	std::memcpy(last_framebuf.surface->pixels,
	            curr_framebuf.surface->pixels,
	            (curr_framebuf.surface->h * curr_framebuf.surface->pitch));

	last_framebuf_dirty = true;
}

void D3D12Renderer::PrepareFrame()
{
	assert(texture);
	assert(last_framebuf.surface);

	last_framebuf.UnlockSurface();

	if (last_framebuf_dirty) {
		SDL_UpdateTexture(texture,
		                  nullptr, // entire texture
		                  last_framebuf.surface->pixels,
		                  last_framebuf.surface->pitch);

		last_framebuf_dirty = false;
	}
}

void D3D12Renderer::PresentFrame()
{
	assert(texture);

	SDL_RenderClear(renderer);
	SDL_RenderTexture(renderer, texture, nullptr, nullptr);

#if C_OPENXR
	// Upload the game frame into the XR swapchain while the presented
	// pixels are current (no-op without an XR frame in flight).
	OPENXR_BlitMirrorToSwapchain();
#endif

#if C_OPENXR
	// Desktop toast overlay; every SDL-presented backend shares it.
	openxr_composite_desk_osd_sdl(renderer);
#endif

	if (CAPTURE_IsCapturingPostRenderImage()) {
		// SDL_RenderPresent() blocks until the queued Direct3D work
		// using the backbuffer has finished, so we're guaranteed to read the
		// contents of the up-to-date backbuffer here right before the
		// buffer swap.
		//
		GFX_CaptureRenderedImage();
	}

	SDL_RenderPresent(renderer);
}

void D3D12Renderer::SetVsync(const bool is_enabled)
{
	if (!SDL_SetRenderVSync(renderer, (is_enabled ? 1 : 0))) {
		LOG_WARNING("D3D12: Error %s vsync: %s",
		        (is_enabled ? "enabling" : "disabling"),
		        SDL_GetError());
	}
}

void D3D12Renderer::SetColorSpace([[maybe_unused]] const ColorSpace color_space)
{
	// no-op (no colour space support yet)
}

void D3D12Renderer::SetImageAdjustmentSettings(
        [[maybe_unused]] const ImageAdjustmentSettings& settings)
{
	// no-op (no image adjustment support yet)
}

void D3D12Renderer::SetDeditheringStrength([[maybe_unused]] const float strength)
{
	// no-op (no image adjustment support yet)
}

void D3D12Renderer::EnableImageAdjustments([[maybe_unused]] const bool enable)
{
	// no-op (no image adjustment support yet)
}

RenderedImage D3D12Renderer::ReadPixelsPostShader(const DosBox::Rect output_rect_px)
{
	// Create new image
	RenderedImage image = {};

	image.params.width              = iroundf(output_rect_px.w);
	image.params.height             = iroundf(output_rect_px.h);
	image.params.double_width       = false;
	image.params.double_height      = false;
	image.params.pixel_aspect_ratio = {1};
	image.params.pixel_format       = PixelFormat::BGR24_ByteArray;

	image.pitch = image.params.width *
	              (get_bits_per_pixel(image.params.pixel_format) / 8);

	const auto image_size_bytes = check_cast<uint32_t>(image.params.height *
	                                                   image.pitch);

	image.image_data   = new uint8_t[image_size_bytes];

	image.is_flipped_vertically = false;

	// See SdlRenderer::ReadPixelsPostShader for why the convert below is
	// needed: SDL3 returns pixels in the renderer's native format.
	//
	const SDL_Rect read_rect_px = to_sdl_rect(output_rect_px);

	const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
	        SDL_RenderReadPixels(renderer, &read_rect_px), SDL_DestroySurface);
	if (!surface) {
		LOG_ERR("D3D12: Error reading pixels from the renderer: %s",
		        SDL_GetError());
		return image;
	}

	// Unlike SDL2, SDL3 returns the pixels in the renderer's native format,
	// so convert them into the BGR24 destination buffer in a single step.
	if (!SDL_ConvertPixels(image.params.width,
	                       image.params.height,
	                       surface->format,
	                       surface->pixels,
	                       surface->pitch,
	                       SDL_PIXELFORMAT_BGR24,
	                       image.image_data,
	                       image.pitch)) {
		LOG_ERR("D3D12: Error converting renderer pixels: %s",
		        SDL_GetError());
	}

	return image;
}

uint32_t D3D12Renderer::MakePixel(const uint8_t red, const uint8_t green,
                                   const uint8_t blue)
{
	static_assert(D3D12PixelFormat == SDL_PIXELFORMAT_XRGB8888);
	return ((blue << 0) | (green << 8) | (red << 16)) | (255 << 24);
}

std::string D3D12Renderer::GetSdlDriverName() const
{
	if (!renderer) {
		return {};
	}
	const char* name = SDL_GetRendererName(renderer);
	return name ? name : "";
}

bool D3D12Renderer::GetGameFrame(std::vector<uint32_t>& pixels_out,
                                  int& w_px_out, int& h_px_out) const
{
	if (!last_framebuf.surface || !last_framebuf.surface->pixels) {
		return false;
	}
	const auto w = last_framebuf.surface->w;
	const auto h = last_framebuf.surface->h;
	if (w <= 0 || h <= 0) {
		return false;
	}
	const auto* src = static_cast<const uint32_t*>(last_framebuf.surface->pixels);
	// Surface pitch may include padding; copy row by row.
	const auto pitch_words = last_framebuf.surface->pitch / 4;
	pixels_out.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
	for (int y = 0; y < h; ++y) {
		std::memcpy(pixels_out.data() + static_cast<size_t>(y) * w,
		            src + static_cast<size_t>(y) * pitch_words,
		            static_cast<size_t>(w) * sizeof(uint32_t));
	}
	w_px_out = w;
	h_px_out = h;
	return true;
}

void D3D12Surface::LockSurface()
{
	assert(surface);
	if (SDL_MUSTLOCK(surface) && !is_locked) {
		SDL_LockSurface(surface);
		is_locked = true;
	}
}

void D3D12Surface::UnlockSurface()
{
	assert(surface);
	if (SDL_MUSTLOCK(surface) && is_locked) {
		SDL_UnlockSurface(surface);
		is_locked = false;
	}
}
