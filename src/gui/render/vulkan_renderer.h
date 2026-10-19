// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_VULKAN_RENDERER_H
#define DOSBOX_VULKAN_RENDERER_H

#include "render_backend.h"

#include <string>
#include <vector>

#include "gui/private/common.h"
#include "gui/render/render.h"

#include "dosbox_config.h"
#include "utils/rect.h"

// must be included after dosbox_config.h
#include <SDL3/SDL.h>

class VulkanSurface {
public:
	void LockSurface();
	void UnlockSurface();
	SDL_Surface* surface = nullptr;

private:
	bool is_locked = false;
};

// Vulkan-backed 2D renderer.
//
// Implemented on top of SDL3's 2D renderer forced onto a Vulkan-based
// driver ("vulkan", falling back to "gpu" with SDL_GPU_DRIVER=vulkan).
// This keeps the implementation small and portable while the pixels
// genuinely go through Vulkan on the host. No shader support in this
// first version (same as SdlRenderer); shaders stay an OpenGL-only
// feature until a Vulkan shader pipeline lands.
//
// Frame handling mirrors SdlRenderer: a CPU "work-in-progress"
// framebuffer plus a latched "last" frame uploaded to a streaming
// texture on PrepareFrame().
class VulkanRenderer : public RenderBackend {
public:
	VulkanRenderer(const int x, const int y, const int width,
	               const int height,
	               const SDL_WindowFlags sdl_window_flags,
	               TextureFilterMode texture_filter_mode);

	~VulkanRenderer() override;

	SDL_Window* GetWindow() override;

	DosBox::Rect GetCanvasSizeInPixels() override;

	void NotifyViewportSizeChanged(const DosBox::Rect draw_rect_px) override;

	void NotifyRenderSizeChanged(const int new_render_width_px,
	                             const int new_render_height_px) override;

	void NotifyVideoModeChanged(const VideoMode& video_mode) override;

	SetShaderResult SetShader(const std::string& symbolic_shader_descriptor) override;

	void ForceReloadCurrentShader() override;

	ShaderInfo GetCurrentShaderInfo() override;
	ShaderPreset GetCurrentShaderPreset() override;

	std::string GetCurrentSymbolicShaderDescriptor() override;
	ShaderDescriptor GetCurrentShaderDescriptor() override;

	void StartFrame(uint32_t*& pixels_out, int& pitch_out) override;
	void EndFrame() override;

	void PrepareFrame() override;
	void PresentFrame() override;

	void SetVsync(const bool is_enabled) override;

	void SetColorSpace(const ColorSpace color_space) override;
	void SetImageAdjustmentSettings(const ImageAdjustmentSettings& settings) override;
	void EnableImageAdjustments(const bool enable) override;
	void SetDeditheringStrength(const float strength) override;

	RenderedImage ReadPixelsPostShader(const DosBox::Rect output_rect_px) override;

	uint32_t MakePixel(const uint8_t red, const uint8_t green,
	                   const uint8_t blue) override;

	// Name of the SDL render driver actually in use (e.g. "vulkan",
	// "gpu"). Empty before successful initialisation.
	std::string GetSdlDriverName() const;

	// CPU copy of the last presented game frame (BGRX words, top-down
	// rows) with its dimensions. Used by screenshots/video capture
	// fallbacks and as the future CPU upload source for OpenXR eye
	// textures once a Vulkan XR path exists. Returns false when no
	// frame is available yet.
	bool GetGameFrame(std::vector<uint32_t>& pixels_out, int& w_px_out,
	                  int& h_px_out) const;

	// prevent copying
	VulkanRenderer(const VulkanRenderer&) = delete;
	// prevent assignment
	VulkanRenderer& operator=(const VulkanRenderer&) = delete;

private:
	bool InitRenderer();

	SDL_Window* window     = {};
	SDL_Renderer* renderer = {};

	// The current framebuffer we render the emulated video output into
	// (contains the "work-in-progress" next frame).
	//
	// The framebuffers contain 32-bit pixel data stored as a sequence of
	// four packed 8-bit values in BGRX byte order (that's in memory order,
	// so byte N is B, byte N+1 is G, byte N+2 is R).
	//
	VulkanSurface curr_framebuf = {};

	// Contains the last fully rendered frame, waiting to be presented.
	VulkanSurface last_framebuf = {};

	// True if the last framebuffer has been updated since the last present
	bool last_framebuf_dirty = false;

	SDL_Texture* texture = {};

	TextureFilterMode texture_filter_mode = TextureFilterMode::Bilinear;
};

#endif // DOSBOX_VULKAN_RENDERER_H
