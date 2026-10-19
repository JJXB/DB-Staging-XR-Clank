// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_OPENGL_RENDERER_H
#define DOSBOX_OPENGL_RENDERER_H

#include "render_backend.h"

#if C_OPENGL

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "private/shader_common.h"
#include "private/shader_pipeline.h"

#include "dosbox_config.h"
#include "gui/render/xr_raster.h"
#include "misc/video.h"
#include "utils/rect.h"

// Glad must be included before SDL
#include "glad/gl.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

class OpenGlRenderer : public RenderBackend {

public:
	OpenGlRenderer(const int x, const int y, const int width,
	               const int height, SDL_WindowFlags sdl_window_flags);

	~OpenGlRenderer() override;

	SDL_Window* GetWindow() override;

	// Return the underlying SDL OpenGL context (for OpenXR graphics
	// binding). Valid after construction, null if creation failed.
	SDL_GLContext GetGLContext();

	DosBox::Rect GetCanvasSizeInPixels() override;

	void NotifyViewportSizeChanged(const DosBox::Rect viewport_size) override;

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

#if C_OPENXR
	// Copy the just-rendered backbuffer image (SDL pixel rect,
	// top-left origin) into a sub-rectangle of a layer of a 2D-array
	// texture. The whole layer is cleared to black first so a
	// shrunken picture gets a black border instead of garbage. Must
	// be called after rendering and before buffer swap, with the GL
	// context current.
	void BlitBackbufferToTexture(GLuint dst_tex, int dst_layer,
	                             SDL_Rect src_rect_px,
	                             SDL_Rect dst_rect_px,
	                             int dst_full_w_px, int dst_full_h_px);

	// Read a pixel rect from the backbuffer (BGRA byte order).
	// Must be called pre-swap with the GL context current. Returns an
	// empty vector on error.
	std::vector<uint32_t> ReadBackbufferRect(SDL_Rect rect_px);

	// Copy of the pre-scaler game frame (top-down rows at integer game
	// resolution, pre-shader, OSD-free at XR-blit time) with its dims.
	// Scanline-interleaved headset modes decode from this instead of
	// the window-fitted backbuffer, whose fractional scale, shader
	// filtering, and bottom-up readback order destroy the fields.
	// Returns an empty vector when no frame is available yet.
	std::vector<uint32_t> ReadGameFrame(int& w_px, int& h_px);

	// Upload CPU BGRA pixels into the retained scratch texture of an
	// eye slot, then blit-scaled into a sub-rectangle of a layer of a
	// 2D-array texture (whole layer cleared to black first).
	void UploadAndBlitEye(int eye_slot, const uint32_t* pixels,
	                      int w_px, int h_px, GLuint dst_tex_array,
	                      int dst_layer, SDL_Rect dst_rect_px,
	                      int eye_w_px, int eye_h_px);

	// Demo/verification helper: upload two CPU BGRA eye images and
	// draw them side-by-side into the window backbuffer (each half
	// aspect-fitted with black bars). Must be called after rendering
	// and before buffer swap, with the GL context current.
	void BlitStereoPairToMirror(const uint32_t* left_pixels,
	                            const uint32_t* right_pixels,
	                            int w_px, int h_px);

	// Overlay-keyboard label rows (XrKeyLabel, see render/xr_raster.h).
	// Build (once per hover/pressed/opacity state) and return the
	// overlay-keyboard RGBA texture. Rebuilds when hover_index,
	// pressed_index, or opacity changes: the hovered key gets an
	// outline, the pressed key (trigger held) gets a lit letter (-1
	// draws neither), and opacity (0..1) scales the texture alpha so
	// the compositor can blend it over the scene behind.
	GLuint GetXrKeyboardTexture(const XrKeyLabel* keys, size_t num_keys,
	                            int tex_w_px, int tex_h_px,
	                            int hover_index, int pressed_index,
	                            float opacity);

	// (Re)builds the notification-overlay texture from lines of text
	// (newest first, up to 4 shown). opacity (0..1) scales the
	// texture alpha for compositor blending. Returns the texture
	// (0 on failure).
	GLuint UpdateXrOsdTexture(const char* const* lines, size_t num_lines,
	                          int tex_w_px, int tex_h_px, float opacity);

	// CPU copy of the last OSD raster for the desktop-mirror
	// composite below (straight-alpha 0xAABBGGRR words, row 0 is the
	// texture bottom, like the uploaded texture). Valid until the
	// next UpdateXrOsdTexture call.
	void GetXrOsdRaster(const uint32_t*& px_out, int& w_out, int& h_out);

	// Blends an OSD raster (same layout as GetXrOsdRaster) over the
	// top-centre of the pending framebuffer. opacity 0..1 (1 = opaque
	// toast); desk_scale multiplies the default two-thirds-width size.
	// Marks the framebuffer dirty.
	void CompositeDeskOsd(const uint32_t* osd_px, int osd_w, int osd_h,
	                      float opacity, float desk_scale);

	// (Re)builds the radial input-mode wheel texture from `count`
	// wedge labels with `highlight` painted brighter. Always rebuilds
	// (callers rebuild only when the selection changes). Returns the
	// texture (0 on failure).
	GLuint UpdateXrWheelTexture(const char* const* labels, int count,
	                            int highlight, int tex_w_px, int tex_h_px,
	                            float opacity);

	// Small shared gradient texture for the 3D pointer-ray ribbons
	// (bright core fading at the edges, straight alpha). Built once,
	// returned cached afterwards (0 on failure).
	GLuint GetXrRayTexture();

	// In-headset menu rows (XrMenuRow, see render/xr_raster.h).
	// (Re)builds the menu-overlay texture from rows. Always rebuilds
	// (callers rebuild on state change only). font_mult scales the
	// glyph size (1.0 = fit the row height); cols is the requested
	// section-grid column count (0 = default). Returns 0 on failure.
	GLuint UpdateXrMenuTexture(const XrMenuRow* rows, int num_rows,
	                           int tex_w_px, int tex_h_px,
	                           float font_mult, int cols);

	// Raw copy of a 2D texture into a same-size destination: one
	// layer of a 2D-array texture when dst_is_array, otherwise a
	// plain 2D texture (single-image swapchains come back as
	// TEXTURE_2D, not TEXTURE_2D_ARRAY).
	void CopyTextureToLayer(GLuint src_tex, int w_px, int h_px,
	                        GLuint dst_tex, int dst_layer,
	                        bool dst_is_array);

	// Draw a 1px pointer ray (world-space segment, column-major MVP)
	// into one layer of a 2D-array texture.
	void DrawXrRayLine(const float mvp[16], const float from_xyz[3],
	                   const float to_xyz[3], GLuint dst_tex_array,
	                   int dst_layer, int w_px, int h_px);
#endif

	void SetVsync(const bool is_enabled) override;

	void SetColorSpace(const ColorSpace color_space) override;
	void EnableImageAdjustments(const bool enable) override;
	void SetImageAdjustmentSettings(const ImageAdjustmentSettings& settings) override;
	void SetDeditheringStrength(const float strength) override;

	RenderedImage ReadPixelsPostShader(const DosBox::Rect output_rect_px) override;

	uint32_t MakePixel(const uint8_t red, const uint8_t green,
	                   const uint8_t blue) override;

	// prevent copying
	OpenGlRenderer(const OpenGlRenderer&) = delete;
	// prevent assignment
	OpenGlRenderer& operator=(const OpenGlRenderer&) = delete;

private:
	SDL_Window* CreateSdlWindow(const int x, const int y, const int width,
	                            const int height,
	                            const SDL_WindowFlags sdl_window_flags);

	bool InitRenderer();
	void RecreateInputTexture();

	void MaybeUpdateRenderSize(const int new_render_width_px,
	                           const int new_render_height_px);

	SetShaderResult SetShaderInternal(const std::string& symbolic_shader_descriptor);

	void HandleShaderAndPresetChangeViaNotify(const ShaderDescriptor& new_descriptor);

	SetShaderResult MaybeSwitchShaderAndPreset(const ShaderDescriptor& curr_descriptor,
	                                           const ShaderDescriptor& new_descriptor);

	bool SwitchShader(const std::string& shader_name);

	SDL_Window* window    = {};
	SDL_GLContext context = {};

	GLint max_texture_size_px = 0;

	// The current framebuffer we render the emulated video output into
	// (contains the "work-in-progress" next frame).
	//
	// The framebuffers contain 32-bit pixel data stored as a sequence of
	// four packed 8-bit values in BGRX byte order (that's in memory order,
	// so byte N is B, byte N+1 is G, byte N+2 is R).
	//
	std::vector<uint32_t> curr_framebuf = {};

	// Contains the last fully rendered frame, waiting to be presented.
	std::vector<uint32_t> last_framebuf = {};

	// True if the last framebuffer has been updated since the last present
	bool last_framebuf_dirty = false;

	struct {
		int width      = 0;
		int height     = 0;
		int pitch      = 0;
		GLuint texture = 0;
	} input_texture = {};

#if C_OPENXR
	// Scratch textures (one per eye slot) + read FBO for the stereo
	// upload path (anaglyph/alternating modes).
	GLuint xr_scratch_tex[2] = {};
	int xr_scratch_w[2]      = {};
	int xr_scratch_h[2]      = {};
	GLuint xr_read_fbo        = 0;

	// Lazily built overlay-keyboard texture (RGBA) and its size, plus
	// the hover/pressed indices it was built with (rebuilt when these
	// or the size change).
	GLuint xr_kb_tex   = 0;
	int    xr_kb_tex_w = 0;
	int    xr_kb_tex_h = 0;
	int    xr_kb_hover_built = -2;
	int    xr_kb_pressed_built = -2;
	float  xr_kb_opacity_built = -1.0f;

	// Lazily rebuilt notification-overlay RGBA texture (for the OSD).
	GLuint xr_osd_tex   = 0;
	int    xr_osd_tex_w = 0;
	int    xr_osd_tex_h = 0;
	// CPU copy of the last OSD raster (for the desktop composite).
	std::vector<uint32_t> xr_osd_img;
	int xr_osd_img_w = 0;
	int xr_osd_img_h = 0;

	// Lazily rebuilt VR-menu overlay RGBA texture.
	GLuint xr_menu_tex   = 0;
	int    xr_menu_tex_w = 0;
	int    xr_menu_tex_h = 0;

	// Shared pointer-ray ribbon texture (built once).
	GLuint xr_ray_tex = 0;

	// Lazily rebuilt radial input-mode wheel RGBA texture.
	GLuint xr_wheel_tex   = 0;
	int    xr_wheel_tex_w = 0;
	int    xr_wheel_tex_h = 0;
#endif

	// Vertex buffer object
	GLuint vbo = 0;

	// Vertex array object
	GLuint vao = 0;

	// Vertex data for an oversized triangle
	std::array<GLfloat, 2 * 3> vertex_data = {};

	// Current shader descriptor string as set by the user (e.g., if the
	// user set `crt-auto`, this will stay `crt-auto`; it won't be synced to
	// the actual shader & preset combo in use, such as
	// `crt/crt-hyllian:vga-4k`).
	//
	// Might contain the .glsl file extension if set by the user.
	//
	std::string curr_symbolic_shader_descriptor = {};

	ShaderDescriptor curr_shader_descriptor = {};

	ShaderInfo main_shader_info     = {};
	ShaderPreset main_shader_preset = {};

	DosBox::Rect curr_viewport_size_px = {};
	VideoMode curr_video_mode          = {};

	std::unique_ptr<ShaderPipeline> shader_pipeline = {};
};

#endif // C_OPENGL

#endif // DOSBOX_OPENGL_RENDERER_H
