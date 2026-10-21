// SPDX-FileCopyrightText:  2025-2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "opengl_renderer.h"

#if C_OPENGL

#include "gui/private/common.h"
#include "private/auto_shader_switcher.h"
#include "private/shader_manager.h"

#include "capture/capture.h"
#include "dosbox_config.h"
#include "misc/support.h"
#include "misc/video.h"
#include "gui/render/xr_raster.h"
#include "utils/checks.h"
#include "utils/math_utils.h"

// Glad must be included before SDL
#include "glad/gl.h"

// must be included after dosbox_config.h
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>

#if defined(MACOSX) 
#include "macos_colorspace.h"
#endif

CHECK_NARROWING();

#if C_OPENXR
// Defined in gui/sdl_gui.cpp: copies the mirror backbuffer into the
// active OpenXR swapchain image when an XR frame is in flight.
void OPENXR_BlitMirrorToSwapchain();
// Defined in gui/osd.cpp: desktop-mirror toasts. pending() is a cheap
// gate (no raster work); composite() blends into the pending
// framebuffer and returns true when pixels were drawn.
bool openxr_desk_osd_pending();
bool openxr_composite_desk_osd();
#endif

// #define DEBUG_OPENGL
// #define USE_DEBUG_CONTEXT

// A safe wrapper around that returns the default result on failure
static const char* safe_gl_get_string(const GLenum requested_name,
                                      const char* default_result = "")
{
	// the result points to a static string but can be null
	const auto result = glGetString(requested_name);

	// the default, however, needs to be valid
	assert(default_result);

	return result ? reinterpret_cast<const char*>(result) : default_result;
}

OpenGlRenderer::OpenGlRenderer(const int x, const int y, const int width,
                               const int height, SDL_WindowFlags sdl_window_flags)
{
	window = CreateSdlWindow(x, y, width, height, sdl_window_flags);
	if (!window) {
		const auto msg = format_str("OPENGL: Error creating window: %s",
		                            SDL_GetError());
		LOG_ERR("%s", msg.c_str());
		throw std::runtime_error(msg);
	}

	if (!InitRenderer()) {
		SDL_DestroyWindow(window);

		const auto msg = format_str("OPENGL: Error creating renderer");
		LOG_ERR("%s", msg.c_str());
		throw std::runtime_error(msg);
	}
}

SDL_Window* OpenGlRenderer::CreateSdlWindow(const int x, const int y,
                                            const int width, const int height,
                                            const SDL_WindowFlags sdl_window_flags)
{
	// Request 24-bits framebuffer, don't care about depth buffer
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#ifdef USE_DEBUG_CONTEXT
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);

	// Request an OpenGL 4.3 core profile context (debug output became a
	// core part of OpenGL since version 4.3). Note the macOS doesn't seem
	// to support debug contexts at all.
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#else
	// Request an OpenGL 3.3 core profile context
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

	// Request an OpenGL-ready window
	SDL_WindowFlags flags = sdl_window_flags;
	flags |= SDL_WINDOW_OPENGL;

	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
	
	// For window flags you should use separate window creation properties,
	// but for easier migration from SDL2 you can use the following:
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, flags);
	auto window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);

#if defined(MACOSX) 
	// From "Best Practices for Color Management in OS X and iOS", chapter
	// "Non-Color Managed Frameworks, OpenGL - Explicit Color Management
	// Example":
	// 
	//   OpenGL is not color managed. As a consequence, it might require
	//   additional effort to devise solutions to specific color problems
	//   you may encounter when using it. The fundamental problem is OpenGL
	//   has one set of assumptions, and the display buffer has another.
	// 
	// Ref:
	// https://developer.apple.com/library/archive/technotes/tn2313/_index.html#//apple_ref/doc/uid/DTS40014694-CH1-NONCOLORMANAGEDFRAMEWORKS-OPENGL___EXPLICIT_COLOR_MANAGEMENT_EXAMPLE
	// 
	// SDL3 tags the window's colorspace with sRGBColorSpace. This is
	// hardcoded, but what we want is to use the Display P3 colour space
	// instead internally, tag the window with displayP3ColorSpace, then let
	// the system-wide colour management feature of macOS do the rest (i.e.,
	// converting the Display P3 image data to whatever colour space is set in
	// the system settings).
	setDisplayP3ColorSpace(window);
#endif

	return window;
}

#ifdef USE_DEBUG_CONTEXT
void glDebugOutput(GLenum source, GLenum type, unsigned int id, GLenum severity,
                   GLsizei length, const char* message, const void* userParam)
{
	LOG_DEBUG("OPENGL: %s", message);
}
#endif

bool OpenGlRenderer::InitRenderer()
{
	context = SDL_GL_CreateContext(window);
	if (!context) {
		LOG_ERR("SDL: Error creating OpenGL context: %s", SDL_GetError());
		return false;
	}

	gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);

#ifdef USE_DEBUG_CONTEXT
	GLint flags;
	glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
	if (flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(glDebugOutput, nullptr);
		glDebugMessageControl(
		        GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
	} else {
		LOG_ERR("OPENGL: Could not initialise debug context");
	}
#endif

	GLint size = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);

	max_texture_size_px = size;

	LOG_INFO("OPENGL: Version: %s, renderer: %s, GLSL version: %s, vendor: %s",
			 safe_gl_get_string(GL_VERSION, "unknown"),
	         safe_gl_get_string(GL_RENDERER, "unknown"),
	         safe_gl_get_string(GL_SHADING_LANGUAGE_VERSION, "unknown"),
	         safe_gl_get_string(GL_VENDOR, "unknown"));

	// Vertex data of a single oversized triangle encompassing the viewport
	// Lower left
	vertex_data[0] = -1.0f;
	vertex_data[1] = -1.0f;

	// Lower right
	vertex_data[2] = 3.0f;
	vertex_data[3] = -1.0f;

	// Upper left
	vertex_data[4] = -1.0f;
	vertex_data[5] = 3.0f;

	// Create VBO & VAO
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data.data(), GL_STATIC_DRAW);

	glVertexAttribPointer(0,        // attribute index,
	                      2,        // num components (vec2),
	                      GL_FLOAT, // component data type,
	                      GL_FALSE, // do not normalise fixed point,
	                      0,        // data stride (0=packed),
	                      static_cast<GLvoid*>(nullptr));

	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

	// We don't care about depth testing; we'll just apply the shaders in 2D
	// space
	glDisable(GL_DEPTH_TEST);

	shader_pipeline = std::make_unique<ShaderPipeline>();

	return true;
}

OpenGlRenderer::~OpenGlRenderer()
{
	SDL_GL_ResetAttributes();

	glDeleteVertexArrays(1, &vao);
	glDeleteBuffers(1, &vbo);

	if (input_texture.texture) {
		glDeleteTextures(1, &input_texture.texture);
		input_texture.texture = 0;
	}

#if C_OPENXR
	for (int i = 0; i < 2; ++i) {
		if (xr_scratch_tex[i]) {
			glDeleteTextures(1, &xr_scratch_tex[i]);
			xr_scratch_tex[i] = 0;
		}
	}
	if (xr_read_fbo) {
		glDeleteFramebuffers(1, &xr_read_fbo);
		xr_read_fbo = 0;
	}
	if (xr_kb_tex) {
		glDeleteTextures(1, &xr_kb_tex);
		xr_kb_tex = 0;
	}
	xr_kb_tex_w = 0;
	xr_kb_tex_h = 0;
	if (xr_wheel_tex) {
		glDeleteTextures(1, &xr_wheel_tex);
		xr_wheel_tex = 0;
	}
	xr_wheel_tex_w = 0;
	xr_wheel_tex_h = 0;
	if (xr_raw_tex) {
		glDeleteTextures(1, &xr_raw_tex);
		xr_raw_tex = 0;
	}
	xr_raw_tex_w = 0;
	xr_raw_tex_h = 0;
#endif

	// ShaderPipeline destructor makes some gl calls.
	// Do that here before we delete the context.
	shader_pipeline = {};

	if (context) {
		SDL_GL_DestroyContext(context);
		context = {};
	}
	if (window) {
		SDL_DestroyWindow(window);
		window = {};
	}
}

SDL_Window* OpenGlRenderer::GetWindow()
{
	return window;
}

SDL_GLContext OpenGlRenderer::GetGLContext()
{
	return context;
}

DosBox::Rect OpenGlRenderer::GetCanvasSizeInPixels()
{
	SDL_Rect canvas_size_px = {};
	SDL_GetWindowSizeInPixels(window, &canvas_size_px.w, &canvas_size_px.h);

	const auto r = to_rect(canvas_size_px);
	assert(r.HasPositiveSize());

	return r;
}

void OpenGlRenderer::NotifyViewportSizeChanged(const DosBox::Rect viewport_size_px)
{
	// If the viewport size has changed, the canvas size might have
	// changed too.
	const auto canvas_size_px = GetCanvasSizeInPixels();

	// We always expect a valid canvas
	assert(!canvas_size_px.IsEmpty());

	curr_viewport_size_px = viewport_size_px;

	// The video mode hasn't changed, but the ShaderManager call expects it.
	const auto video_mode = VGA_GetCurrentVideoMode();

	if (video_mode.width <= 0 && video_mode.height <= 0) {
		// On Windows at least, this method gets called before the video
		// mode is initialised if fullscreen is enabled in the config.
		// The problem is that the exact invocation order of the
		// graphics init code is still somewhat platform dependent as
		// certain actions are triggered in response to windowing events
		// which are not 100% standardised across platforms in SDL2.
		// SDL3 promises more cross-platform consistency in this regard,
		// so let's hope that will get us closer to the "test once, run
		// anywhere" paradigm.
		//
		// It's rather tricky to solve this in a more elegant way, so
		// this solution will do for now. This method will get called a
		// second time after the video mode has been initialised which
		// won't be a no-op.
		return;
	}

	const auto new_descriptor = AutoShaderSwitcher::GetInstance().NotifyRenderParametersChanged(
	        curr_shader_descriptor, canvas_size_px, video_mode);

	HandleShaderAndPresetChangeViaNotify(new_descriptor);

	shader_pipeline->NotifyViewportSizeChanged(curr_viewport_size_px);
}

void OpenGlRenderer::HandleShaderAndPresetChangeViaNotify(const ShaderDescriptor& new_descriptor)
{
	using enum OpenGlRenderer::SetShaderResult;

	switch (MaybeSwitchShaderAndPreset(curr_shader_descriptor, new_descriptor)) {
	case Ok: curr_shader_descriptor = new_descriptor; break;
	case PresetError:
		LOG_ERR("RENDER: Error loading shader preset '%s'; using default preset",
		        new_descriptor.ToString().c_str());

		curr_shader_descriptor = new_descriptor;
		curr_shader_descriptor.preset_name.clear();
		break;

	case ShaderError:
		LOG_ERR("RENDER: Error setting shader '%s'",
		        new_descriptor.ToString().c_str());
		break;

	default: assertm(false, "Invalid SetShaderResult value");
	}
}

void OpenGlRenderer::NotifyRenderSizeChanged(const int new_render_width_px,
                                             const int new_render_height_px)
{
	MaybeUpdateRenderSize(new_render_width_px, new_render_height_px);
}

void OpenGlRenderer::MaybeUpdateRenderSize(const int new_render_width_px,
                                           const int new_render_height_px)
{
	// At startup we set the shader before receiving the first
	// `UpdateRenderSize()` call.  This will result in
	// `MaybeSwitchShaderAndPreset()` calling `UpdateRenderSize()` with zero
	// render dimensions.
	//
	// It's hard to solve this in a more "elegant" way, so the simplest
	// solution is just to bail out in the zero dimension case and let the
	// subsequent `UpdateRenderSize()` call set up the correct render
	// dimensions.
	//
	if (new_render_width_px == 0 && new_render_height_px == 0) {
		// no-op
		return;
	}

	// Size hasn't changed, don't recreate the texture
	if (new_render_width_px == input_texture.width &&
	    new_render_height_px == input_texture.height) {
		// no-op
		return;
	}

	if (new_render_width_px > max_texture_size_px ||
	    new_render_height_px > max_texture_size_px) {

		LOG_ERR("OPENGL: No support for texture size of %dx%d pixels",
		        new_render_width_px,
		        new_render_height_px);
		return;
	}

	input_texture.width  = new_render_width_px;
	input_texture.height = new_render_height_px;

	RecreateInputTexture();

	shader_pipeline->NotifyRenderSizeChanged(input_texture.width,
	                                         input_texture.height,
	                                         input_texture.texture);
}

void OpenGlRenderer::RecreateInputTexture()
{
	if (input_texture.texture) {
		glDeleteTextures(1, &input_texture.texture);
	}

	glGenTextures(1, &input_texture.texture);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, input_texture.texture);

	// Wrap mode is set on the bound sampler object in the shader
	// pipeline, per the `#pragma wrap_modeN` of each shader pass input.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// Just create the texture; we'll copy the image data later with
	// `glTexSubImage2D()`
	//
	glTexImage2D(GL_TEXTURE_2D,
	             0,                    // mimap level (0 = base image)
	             GL_RGBA8,             // internal format
	             input_texture.width,  // width
	             input_texture.height, // height
	             0,                    // border (must be always 0)
	             GL_BGRA,              // pixel data format
	             GL_UNSIGNED_BYTE,     // pixel data type
	             nullptr               // pointer to image data
	);

	glBindTexture(GL_TEXTURE_2D, 0);

	// Allocate host memory buffers for the texture data. The video card
	// emulation will write to these buffers, then we'll copy the data to
	// the texture in GPU memory with `glTexSubImage2D()` before presenting
	// the frame.
	const auto pitch_pixels = input_texture.width;
	const auto num_pixels   = static_cast<size_t>(pitch_pixels) *
	                        input_texture.height;

	curr_framebuf.resize(num_pixels);
	last_framebuf.resize(num_pixels);

	constexpr auto BytesPerPixel = sizeof(uint32_t);
	const auto pitch_bytes       = pitch_pixels * BytesPerPixel;

	input_texture.pitch = check_cast<int>(pitch_bytes);
}

void OpenGlRenderer::StartFrame(uint32_t*& pixels_out, int& pitch_out)
{
	assert(!curr_framebuf.empty());

	pixels_out = curr_framebuf.data();
	if (pixels_out == nullptr) {
		return;
	}
	pitch_out = input_texture.pitch;
}

void OpenGlRenderer::EndFrame()
{
	assert(!curr_framebuf.empty());
	assert(!last_framebuf.empty());

	// We need to copy the buffers. We can't just swap them because the VGA
	// emulation only writes the changed pixels to the framebuffer in each
	// frame.

	last_framebuf       = curr_framebuf;
	last_framebuf_dirty = true;
}

void OpenGlRenderer::PrepareFrame()
{
	assert(!last_framebuf.empty());

	if (last_framebuf_dirty) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, input_texture.texture);

		glTexSubImage2D(GL_TEXTURE_2D,
		                0, // mimap level (0 = base image)
		                0, // x offset
		                0, // y offset
		                input_texture.width,  // width
		                input_texture.height, // height
		                GL_BGRA,              // pixel data format
		                GL_UNSIGNED_INT_8_8_8_8_REV, // pixel data type
		                last_framebuf.data() // pointer to image data
		);

		glBindTexture(GL_TEXTURE_2D, 0);

		last_framebuf_dirty = false;
	}
}

void OpenGlRenderer::PresentFrame()
{
#if C_OPENXR
	// Desktop toasts stay out of the XR picture: stage a clean-frame
	// restore only when toasts may draw (cheap gate, no raster work).
	// The copy lives only for toast-visible presents.
	bool desk_saved = false;
	std::vector<uint32_t> clean_framebuf;
	if (openxr_desk_osd_pending()) {
		clean_framebuf = last_framebuf;
		desk_saved     = true;
	}
#endif

	shader_pipeline->Render(vao);

#if C_OPENXR
	// Copy the just-rendered mirror image into the XR swapchain while
	// the backbuffer is still valid (pre-swap). This sees the clean
	// frame: desktop toasts are composited below, after this blit.
	OPENXR_BlitMirrorToSwapchain();

	// Desktop-only toasts, after the XR blit so the headset video
	// (mono blit, depth/autodepth readbacks, eye uploads) never sees
	// them. Re-upload + re-render so the desktop swap shows them,
	// then restore the clean frame so repeated presents of a static
	// image don't accumulate blends and the next XR blit stays clean.
	if (desk_saved && openxr_composite_desk_osd()) {
		// The toast blend changed last_framebuf in place; force the
		// upload so the extra render actually shows it.
		last_framebuf_dirty = true;
		PrepareFrame();
		shader_pipeline->Render(vao);
		last_framebuf       = std::move(clean_framebuf);
		last_framebuf_dirty = true;
	}
#endif

	// Optionally capture frame
	if (CAPTURE_IsCapturingPostRenderImage()) {
		// glReadPixels() implicitly blocks until all pipelined
		// rendering commands have finished, so we're guaranteed to
		// read the contents of the up-to-date backbuffer here right
		// before the buffer swap.
		//
		GFX_CaptureRenderedImage();
	}

	// Present frame
	SDL_GL_SwapWindow(window);
}

#if C_OPENXR
GLuint OpenGlRenderer::GetXrKeyboardTexture(const XrKeyLabel* keys,
                                           const size_t num_keys,
                                           const int tex_w_px,
                                           const int tex_h_px,
                                           const int hover_index,
                                           const int pressed_index,
                                           const float opacity)
{
	if (!keys || num_keys == 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return 0;
	}
	if (xr_kb_tex && hover_index == xr_kb_hover_built &&
	    pressed_index == xr_kb_pressed_built &&
	    opacity == xr_kb_opacity_built && tex_w_px == xr_kb_tex_w &&
	    tex_h_px == xr_kb_tex_h) {
		return xr_kb_tex; // layout is static; rebuild on state change
	}
	if (xr_kb_tex) {
		glDeleteTextures(1, &xr_kb_tex);
		xr_kb_tex = 0;
	}
	std::vector<uint32_t> img;
	if (!XrBuildKeyboardRaster(keys,
	                           num_keys,
	                           tex_w_px,
	                           tex_h_px,
	                           hover_index,
	                           pressed_index,
	                           opacity,
	                           img)) {
		return 0;
	}

	glGenTextures(1, &xr_kb_tex);
	glBindTexture(GL_TEXTURE_2D, xr_kb_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	// Opacity was already baked into the raster by the builder.
	glTexImage2D(GL_TEXTURE_2D,
	             0, // mip level
	             GL_RGBA8,
	             tex_w_px,
	             tex_h_px,
	             0, // border
	             GL_RGBA,
	             GL_UNSIGNED_BYTE,
	             img.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	xr_kb_tex_w = tex_w_px;
	xr_kb_tex_h = tex_h_px;
	xr_kb_hover_built = hover_index;
	xr_kb_pressed_built = pressed_index;
	xr_kb_opacity_built = opacity;

	return xr_kb_tex;
}

GLuint OpenGlRenderer::UpdateXrWheelTexture(const char* const* labels,
                                            int count, int highlight,
                                            int tex_w_px, int tex_h_px,
                                            float opacity)
{
	if (!labels || count <= 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return 0;
	}
	std::vector<uint32_t> img;
	if (!XrBuildWheelRaster(labels,
	                        count,
	                        highlight,
	                        tex_w_px,
	                        tex_h_px,
	                        opacity,
	                        img)) {
		return 0;
	}

	if (xr_wheel_tex &&
	    (xr_wheel_tex_w != tex_w_px || xr_wheel_tex_h != tex_h_px)) {
		glDeleteTextures(1, &xr_wheel_tex);
		xr_wheel_tex   = 0;
		xr_wheel_tex_w = 0;
		xr_wheel_tex_h = 0;
	}
	if (!xr_wheel_tex) {
		glGenTextures(1, &xr_wheel_tex);
	}
	glBindTexture(GL_TEXTURE_2D, xr_wheel_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D,
	             0, // mip level
	             GL_RGBA8,
	             tex_w_px,
	             tex_h_px,
	             0, // border
	             GL_RGBA,
	             GL_UNSIGNED_BYTE,
	             img.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	xr_wheel_tex_w = tex_w_px;
	xr_wheel_tex_h = tex_h_px;
	return xr_wheel_tex;
}

GLuint OpenGlRenderer::UpdateXrRawTexture(const uint32_t* px,
                                          int tex_w_px, int tex_h_px)
{
	if (!px || tex_w_px <= 0 || tex_h_px <= 0) {
		return 0;
	}
	if (xr_raw_tex &&
	    (xr_raw_tex_w != tex_w_px || xr_raw_tex_h != tex_h_px)) {
		glDeleteTextures(1, &xr_raw_tex);
		xr_raw_tex   = 0;
		xr_raw_tex_w = 0;
		xr_raw_tex_h = 0;
	}
	if (!xr_raw_tex) {
		glGenTextures(1, &xr_raw_tex);
	}
	glBindTexture(GL_TEXTURE_2D, xr_raw_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D,
	             0, // mip level
	             GL_RGBA8,
	             tex_w_px,
	             tex_h_px,
	             0, // border
	             GL_RGBA,
	             GL_UNSIGNED_BYTE,
	             px);
	glBindTexture(GL_TEXTURE_2D, 0);

	xr_raw_tex_w = tex_w_px;
	xr_raw_tex_h = tex_h_px;
	return xr_raw_tex;
}

void OpenGlRenderer::CompositeDeskOsd(const uint32_t* osd_px, int osd_w,
                                      int osd_h, float opacity,
                                      float desk_scale)
{
	if (!osd_px || osd_w <= 0 || osd_h <= 0 || last_framebuf.empty()) {
		return;
	}
	const int fw = input_texture.width;
	const int fh = input_texture.height;
	if (fw <= 0 || fh <= 0 ||
	    static_cast<size_t>(fw) * fh > last_framebuf.size()) {
		return;
	}
	float ta = opacity;
	if (ta < 0.0f) {
		ta = 0.0f;
	}
	if (ta > 1.0f) {
		ta = 1.0f;
	}
	if (ta <= 0.0f) {
		return;
	}
	// Top-centre at the scaled size (default two-thirds width, fonts
	// scale with the quad; framebuffer row 0 is the top). Uniformly
	// shrunk only if it would exceed the frame.
	if (desk_scale < 0.25f) {
		desk_scale = 0.25f;
	}
	if (desk_scale > 2.0f) {
		desk_scale = 2.0f;
	}
	int tw = static_cast<int>(fw * 2 / 3 * desk_scale);
	int th = (osd_w > 0) ? tw * osd_h / osd_w : 0;
	if (tw > fw || th > fh / 2) {
		const float f = std::min(static_cast<float>(fw) / tw,
		                         static_cast<float>(fh / 2) / th);
		tw = static_cast<int>(tw * f);
		th = static_cast<int>(th * f);
	}
	if (tw <= 0 || th <= 0) {
		return;
	}
	const int tx0 = (fw - tw) / 2;
	const int ty0 = fh * 4 / 100;
	if (tx0 < 0 || ty0 < 0 || tx0 + tw > fw || ty0 + th > fh) {
		return;
	}
	for (int y = 0; y < th; ++y) {
		// Nearest sample; source rows stored bottom-up.
		const int sy = y * osd_h / th;
		const uint32_t* srow =
		        osd_px + static_cast<size_t>(osd_h - 1 - sy) * osd_w;
		uint32_t* drow = last_framebuf.data() +
		                 static_cast<size_t>(ty0 + y) * fw + tx0;
		for (int x = 0; x < tw; ++x) {
			const uint32_t sp  = srow[x * osd_w / tw];
			const uint32_t sa8 = (sp >> 24);
			if (sa8 == 0) {
				continue;
			}
			const float a  = sa8 / 255.0f * ta;
			const float ia = 1.0f - a;
			// Source bytes R,G,B,A; dest bytes B,G,R,A.
			const float sr = (sp & 0xFFu);
			const float sg = (sp >> 8) & 0xFFu;
			const float sb = (sp >> 16) & 0xFFu;
			const uint32_t dp = drow[x];
			const float db = (dp & 0xFFu);
			const float dg = (dp >> 8) & 0xFFu;
			const float dr = (dp >> 16) & 0xFFu;
			const auto ob = static_cast<uint32_t>(db * ia + sb * a +
			                                      0.5f);
			const auto og = static_cast<uint32_t>(dg * ia + sg * a +
			                                      0.5f);
			const auto or_ = static_cast<uint32_t>(dr * ia + sr * a +
			                                       0.5f);
			drow[x] = 0xFF000000u | (or_ << 16) | (og << 8) | ob;
		}
	}
	last_framebuf_dirty = true;
}

GLuint OpenGlRenderer::GetXrRayTexture(const uint32_t rgb,
                                       const uint32_t opacity_pct)
{
	const bool rebuild = xr_ray_tex &&
	                     (rgb != xr_ray_rgb ||
	                      opacity_pct != xr_ray_alpha_pct);
	if (xr_ray_tex && !rebuild) {
		return xr_ray_tex;
	}
	constexpr int W = 128, H = 16;
	std::vector<uint32_t> img;
	XrBuildRayRaster(img,
	                 W,
	                 H,
	                 rgb,
	                 static_cast<float>(opacity_pct) / 100.0f);

	if (!xr_ray_tex) {
		glGenTextures(1, &xr_ray_tex);
	}
	glBindTexture(GL_TEXTURE_2D, xr_ray_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D,
	             0, // mip level
	             GL_RGBA8,
	             W,
	             H,
	             0, // border
	             GL_RGBA,
	             GL_UNSIGNED_BYTE,
	             img.data());
	glBindTexture(GL_TEXTURE_2D, 0);
	xr_ray_rgb       = rgb;
	xr_ray_alpha_pct = opacity_pct;
	return xr_ray_tex;
}

GLuint OpenGlRenderer::GetXrBorderTexture()
{
	if (xr_border_tex) {
		return xr_border_tex;
	}
	constexpr int W = 256, H = 256;
	std::vector<uint32_t> img;
	XrBuildPanelBorderRaster(img, W, H);

	glGenTextures(1, &xr_border_tex);
	glBindTexture(GL_TEXTURE_2D, xr_border_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D,
	             0, // mip level
	             GL_RGBA8,
	             W,
	             H,
	             0, // border
	             GL_RGBA,
	             GL_UNSIGNED_BYTE,
	             img.data());
	glBindTexture(GL_TEXTURE_2D, 0);
	return xr_border_tex;
}

GLuint OpenGlRenderer::UpdateXrMenuTexture(const XrMenuRow* rows,
                                           int num_rows, int tex_w_px,
                                           int tex_h_px, float font_mult,
                                           int cols)
{
	if (!rows || num_rows <= 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return 0;
	}
	std::vector<uint32_t> img;
	if (!XrBuildMenuRaster(rows,
	                          num_rows,
	                          tex_w_px,
	                          tex_h_px,
	                          font_mult,
	                          cols,
	                          img)) {
		return 0;
	}

	if (xr_menu_tex &&
	    (xr_menu_tex_w != tex_w_px || xr_menu_tex_h != tex_h_px)) {
		glDeleteTextures(1, &xr_menu_tex);
		xr_menu_tex   = 0;
		xr_menu_tex_w = 0;
		xr_menu_tex_h = 0;
	}
	if (!xr_menu_tex) {
		glGenTextures(1, &xr_menu_tex);
	}
	glBindTexture(GL_TEXTURE_2D, xr_menu_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D,
	             0, // mip level
	             GL_RGBA8,
	             tex_w_px,
	             tex_h_px,
	             0, // border
	             GL_RGBA,
	             GL_UNSIGNED_BYTE,
	             img.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	xr_menu_tex_w = tex_w_px;
	xr_menu_tex_h = tex_h_px;
	return xr_menu_tex;
}

void OpenGlRenderer::DrawXrRayLine(const float mvp[16],
                                    const float from_xyz[3],
                                    const float to_xyz[3],
                                    GLuint dst_tex_array, int dst_layer,
                                    int w_px, int h_px)
{
	if (!mvp || !from_xyz || !to_xyz || !dst_tex_array || dst_layer < 0 ||
	    w_px <= 0 || h_px <= 0) {
		return;
	}

	static GLuint prog = 0, vbo = 0, vao = 0, fbo = 0;
	static bool prog_ok = false, prog_tried = false;
	if (!prog_tried) {
		prog_tried = true;
		const char* vsrc =
		        "#version 330 core\n"
		        "layout(location=0) in vec3 a_pos;\n"
		        "uniform mat4 u_mvp;\n"
		        "void main(){gl_Position = u_mvp*vec4(a_pos,1.0);}\n";
		const char* fsrc =
		        "#version 330 core\n"
		        "out vec4 o_col;\n"
		        "uniform vec4 u_color;\n"
		        "void main(){o_col = u_color;}\n";
		const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(vs, 1, &vsrc, nullptr);
		glCompileShader(vs);
		GLint compiled = 0;
		glGetShaderiv(vs, GL_COMPILE_STATUS, &compiled);
		const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(fs, 1, &fsrc, nullptr);
		glCompileShader(fs);
		GLint fcompiled = 0;
		glGetShaderiv(fs, GL_COMPILE_STATUS, &fcompiled);
		if (compiled && fcompiled) {
			prog = glCreateProgram();
			glAttachShader(prog, vs);
			glAttachShader(prog, fs);
			glLinkProgram(prog);
			GLint linked = 0;
			glGetProgramiv(prog, GL_LINK_STATUS, &linked);
			prog_ok = (linked != 0);
		}
		glDeleteShader(vs);
		glDeleteShader(fs);
		if (!prog_ok) {
			LOG_ERR("OPENGL: Failed to build XR ray shader");
			return;
		}
		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
		glGenFramebuffers(1, &fbo);
	}
	if (!prog_ok) {
		return;
	}

	GLint prev_prog = 0, prev_array = 0, prev_vao = 0;
	GLint prev_draw = 0, prev_read = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	GLint prev_viewport[4] = {};
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean srgb_was_enabled = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER,
	                          GL_COLOR_ATTACHMENT0,
	                          dst_tex_array,
	                          0, // mip level
	                          dst_layer);
	if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) !=
	    GL_FRAMEBUFFER_COMPLETE) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			LOG_ERR("OPENGL: XR ray FBO incomplete, skipping pointer ray");
		}
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
		return;
	}

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_SCISSOR_TEST);
	glViewport(0, 0, w_px, h_px);

	glUseProgram(prog);
	const GLint mvp_loc = glGetUniformLocation(prog, "u_mvp");
	glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, mvp);
	const GLint col_loc = glGetUniformLocation(prog, "u_color");
	glUniform4f(col_loc, 0.0f, 1.0f, 1.0f, 1.0f);

	const float verts[6] = {from_xyz[0], from_xyz[1], from_xyz[2],
	                        to_xyz[0],   to_xyz[1],   to_xyz[2]};
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	glDrawArrays(GL_LINES, 0, 2);

	glUseProgram(prev_prog);
	glBindVertexArray(prev_vao);
	glBindBuffer(GL_ARRAY_BUFFER, prev_array);
	if (depth_was_enabled) {
		glEnable(GL_DEPTH_TEST);
	}
	if (srgb_was_enabled) {
		glEnable(GL_FRAMEBUFFER_SRGB);
	}
	if (scissor_was_enabled) {
		glEnable(GL_SCISSOR_TEST);
	}
	glViewport(prev_viewport[0],
	           prev_viewport[1],
	           prev_viewport[2],
	           prev_viewport[3]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
}

void OpenGlRenderer::CopyTextureToLayer(GLuint src_tex, int w_px, int h_px,
                                        GLuint dst_tex, int dst_layer,
                                        bool dst_is_array)
{
	if (!src_tex || !dst_tex || w_px <= 0 || h_px <= 0 ||
	    dst_layer < 0) {
		return;
	}
	if (!xr_read_fbo) {
		glGenFramebuffers(1, &xr_read_fbo);
	}

	GLint prev_read = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, xr_read_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
	                       GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D,
	                       src_tex,
	                       0);
	if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) !=
	    GL_FRAMEBUFFER_COMPLETE) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			LOG_ERR("OPENGL: XR layer copy FBO incomplete, skipping");
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
		return;
	}
	if (dst_is_array) {
		// The copy target is whatever array texture is bound (not
		// a parameter), so bind the destination explicitly.
		GLint prev_tex_array = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prev_tex_array);
		glBindTexture(GL_TEXTURE_2D_ARRAY, dst_tex);
		glCopyTexSubImage3D(GL_TEXTURE_2D_ARRAY,
		                    0, // mip level
		                    0, // x offset
		                    0, // y offset
		                    dst_layer,
		                    0, // x
		                    0, // y
		                    w_px,
		                    h_px);
		glBindTexture(GL_TEXTURE_2D_ARRAY, prev_tex_array);
	} else {
		GLint prev_tex = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
		glBindTexture(GL_TEXTURE_2D, dst_tex);
		glCopyTexSubImage2D(GL_TEXTURE_2D,
		                    0, // mip level
		                    0, // x offset
		                    0, // y offset
		                    0, // x
		                    0, // y
		                    w_px,
		                    h_px);
		glBindTexture(GL_TEXTURE_2D, prev_tex);
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
}

std::vector<uint32_t> OpenGlRenderer::ReadBackbufferRect(SDL_Rect rect_px)
{
	std::vector<uint32_t> out;
	if (rect_px.w <= 0 || rect_px.h <= 0) {
		return out;
	}

	int fb_w = 0, fb_h = 0;
	SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);

	// Clamp to the drawable and bail on empty results.
	int x = rect_px.x < 0 ? 0 : rect_px.x;
	int y = rect_px.y < 0 ? 0 : rect_px.y;
	int w = rect_px.w;
	int h = rect_px.h;
	if (x + w > fb_w) {
		w = fb_w - x;
	}
	if (y + h > fb_h) {
		h = fb_h - y;
	}
	if (w <= 0 || h <= 0) {
		return out;
	}

	out.resize(static_cast<size_t>(w) * static_cast<size_t>(h));

	GLint prev_pack = 0, prev_read = 0;
	glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	// GL origin is bottom-left; returned rows run bottom-up.
	glReadPixels(x, fb_h - (y + h), w, h,
	             GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, out.data());
	glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);

	return out;
}

std::vector<uint32_t> OpenGlRenderer::ReadGameFrame(int& w_px, int& h_px)
{
	w_px = input_texture.width;
	h_px = input_texture.height;
	if (w_px <= 0 || h_px <= 0 || last_framebuf.empty() ||
	    last_framebuf.size() != static_cast<size_t>(w_px) *
	                            static_cast<size_t>(h_px)) {
		w_px = 0;
		h_px = 0;
		return {};
	}
	// By value: the caller keeps it past any later PresentFrame work.
	return last_framebuf;
}

void OpenGlRenderer::UploadAndBlitEye(int eye_slot, const uint32_t* pixels,
                                      int w_px, int h_px,
                                      GLuint dst_tex_array, int dst_layer,
                                      SDL_Rect dst_rect_px,
                                      int eye_w_px, int eye_h_px)
{
	if (!pixels || w_px <= 0 || h_px <= 0 || eye_slot < 0 ||
	    eye_slot > 1 || !dst_tex_array || dst_layer < 0 ||
	    eye_w_px <= 0 || eye_h_px <= 0 || dst_rect_px.w <= 0 ||
	    dst_rect_px.h <= 0) {
		return;
	}

	// (Re)allocate the retained scratch texture on size changes.
	if (!xr_scratch_tex[eye_slot] || xr_scratch_w[eye_slot] != w_px ||
	    xr_scratch_h[eye_slot] != h_px) {
		if (!xr_scratch_tex[eye_slot]) {
			glGenTextures(1, &xr_scratch_tex[eye_slot]);
		}
		glBindTexture(GL_TEXTURE_2D, xr_scratch_tex[eye_slot]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D,
		             0, // mip level
		             GL_RGBA8,
		             w_px,
		             h_px,
		             0, // border
		             GL_BGRA,
		             GL_UNSIGNED_INT_8_8_8_8_REV,
		             nullptr);
		xr_scratch_w[eye_slot] = w_px;
		xr_scratch_h[eye_slot] = h_px;
	} else {
		glBindTexture(GL_TEXTURE_2D, xr_scratch_tex[eye_slot]);
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D,
	                0, // mip level
	                0, // x offset
	                0, // y offset
	                w_px,
	                h_px,
	                GL_BGRA,
	                GL_UNSIGNED_INT_8_8_8_8_REV,
	                pixels);
	glBindTexture(GL_TEXTURE_2D, 0);

	if (!xr_read_fbo) {
		glGenFramebuffers(1, &xr_read_fbo);
	}

	GLint prev_draw = 0, prev_read = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	GLint prev_viewport[4] = {};
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	const GLboolean srgb_was_enabled    = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);

	// One draw FBO for all blits; the GL context outlives the process.
	static GLuint blit_fbo = 0;
	if (!blit_fbo) {
		glGenFramebuffers(1, &blit_fbo);
	}

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blit_fbo);
	glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER,
	                          GL_COLOR_ATTACHMENT0,
	                          dst_tex_array,
	                          0, // mip level
	                          dst_layer);
	if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) !=
	    GL_FRAMEBUFFER_COMPLETE) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			LOG_ERR("OPENGL: XR stereo blit FBO incomplete, skipping HMD copy");
		}
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
		return;
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, xr_read_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
	                       GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D,
	                       xr_scratch_tex[eye_slot],
	                       0);

	glDisable(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_SCISSOR_TEST);

	// Clear the whole eye slice to black first so the border around
	// a shrunken picture is black, not uninitialized memory.
	glViewport(0, 0, eye_w_px, eye_h_px);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// Scratch rows run bottom-up (glReadPixels order); the
	// destination slice is addressed bottom-origin as well, so no
	// Y-flip is needed on either side.
	const int dx0 = dst_rect_px.x;
	const int dy0 = eye_h_px - (dst_rect_px.y + dst_rect_px.h);
	const int dx1 = dst_rect_px.x + dst_rect_px.w;
	const int dy1 = eye_h_px - dst_rect_px.y;

	glBlitFramebuffer(0, 0, w_px, h_px,
	                  dx0, dy0, dx1, dy1,
	                  GL_COLOR_BUFFER_BIT, GL_LINEAR);

	if (srgb_was_enabled) {
		glEnable(GL_FRAMEBUFFER_SRGB);
	}
	if (scissor_was_enabled) {
		glEnable(GL_SCISSOR_TEST);
	}
	glViewport(prev_viewport[0],
	           prev_viewport[1],
	           prev_viewport[2],
	           prev_viewport[3]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
}

void OpenGlRenderer::BlitStereoPairToMirror(const uint32_t* left_pixels,
                                            const uint32_t* right_pixels,
                                            int w_px, int h_px)
{
	if (!left_pixels || !right_pixels || w_px <= 0 || h_px <= 0) {
		return;
	}
	int win_w = 0, win_h = 0;
	SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
	if (win_w <= 0 || win_h <= 0) {
		return;
	}

	// One scratch texture per half; same retained-cache pattern as the
	// eye uploads (sizes match the eye buffers, so no realloc churn).
	static GLuint mirror_tex[2] = {};
	static int mirror_w = 0, mirror_h = 0;
	if (!mirror_tex[0] || mirror_w != w_px || mirror_h != h_px) {
		for (int i = 0; i < 2; ++i) {
			if (!mirror_tex[i]) {
				glGenTextures(1, &mirror_tex[i]);
			}
			glBindTexture(GL_TEXTURE_2D, mirror_tex[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			                GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
			                GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
			                GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
			                GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D,
			             0, // mip level
			             GL_RGBA8,
			             w_px,
			             h_px,
			             0, // border
			             GL_BGRA,
			             GL_UNSIGNED_INT_8_8_8_8_REV,
			             nullptr);
		}
		mirror_w = w_px;
		mirror_h = h_px;
	}
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	const uint32_t* halves[2] = {left_pixels, right_pixels};
	for (int i = 0; i < 2; ++i) {
		glBindTexture(GL_TEXTURE_2D, mirror_tex[i]);
		glTexSubImage2D(GL_TEXTURE_2D,
		                0, // mip level
		                0, // x offset
		                0, // y offset
		                w_px,
		                h_px,
		                GL_BGRA,
		                GL_UNSIGNED_INT_8_8_8_8_REV,
		                halves[i]);
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	if (!xr_read_fbo) {
		glGenFramebuffers(1, &xr_read_fbo);
	}

	GLint prev_draw = 0, prev_read = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	GLint prev_viewport[4] = {};
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	const GLboolean srgb_was_enabled    = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);

	glDisable(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_SCISSOR_TEST);

	// Window backbuffer, bottom-origin like the eye slices: rows run
	// bottom-up on both sides, so no Y-flip is needed.
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, win_w, win_h);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// Aspect-fit each eye into its half (bars stay black).
	const int half_w  = win_w / 2;
	const int dw      = std::min(half_w, win_h * w_px / h_px);
	const int dh      = std::min(win_h, half_w * h_px / w_px);
	const int y_off   = (win_h - dh) / 2;
	const int lx_off  = (half_w - dw) / 2;
	const int rx_off  = half_w + (win_w - half_w - dw) / 2;
	const int dst_x0[2] = {lx_off, rx_off};
	for (int i = 0; i < 2; ++i) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, xr_read_fbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
		                       GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D,
		                       mirror_tex[i],
		                       0);
		glBlitFramebuffer(0, 0, w_px, h_px,
		                  dst_x0[i], y_off,
		                  dst_x0[i] + dw, y_off + dh,
		                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}

	if (srgb_was_enabled) {
		glEnable(GL_FRAMEBUFFER_SRGB);
	}
	if (scissor_was_enabled) {
		glEnable(GL_SCISSOR_TEST);
	}
	glViewport(prev_viewport[0],
	           prev_viewport[1],
	           prev_viewport[2],
	           prev_viewport[3]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
}

void OpenGlRenderer::BlitBackbufferToTexture(GLuint dst_tex, int dst_layer,
                                             SDL_Rect src_rect_px,
                                             SDL_Rect dst_rect_px,
                                             int dst_full_w_px,
                                             int dst_full_h_px)
{
	if (!dst_tex || dst_layer < 0 || dst_full_w_px <= 0 ||
	    dst_full_h_px <= 0 || dst_rect_px.w <= 0 || dst_rect_px.h <= 0 ||
	    src_rect_px.w <= 0 || src_rect_px.h <= 0) {
		return;
	}

	// One FBO for all blits; the GL context outlives the process here.
	static GLuint blit_fbo = 0;
	if (!blit_fbo) {
		glGenFramebuffers(1, &blit_fbo);
	}

	int fb_w = 0, fb_h = 0;
	SDL_GetWindowSizeInPixels(window, &fb_w, &fb_h);
	if (fb_w <= 0 || fb_h <= 0) {
		return;
	}

	GLint prev_draw = 0, prev_read = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	GLint prev_viewport[4] = {};
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	const GLboolean srgb_was_enabled    = glIsEnabled(GL_FRAMEBUFFER_SRGB);
	const GLboolean scissor_was_enabled = glIsEnabled(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blit_fbo);
	glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER,
	                          GL_COLOR_ATTACHMENT0,
	                          dst_tex,
	                          0, // mip level
	                          dst_layer);
	if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) !=
	    GL_FRAMEBUFFER_COMPLETE) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			LOG_ERR("OPENGL: XR blit FBO incomplete, skipping HMD copy");
		}
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
		return;
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	// The swapchain holds raw shader-output values; never let GL
	// re-encode them as sRGB during the copy.
	glDisable(GL_FRAMEBUFFER_SRGB);
	glDisable(GL_SCISSOR_TEST);

	// Clear the whole eye slice to black first so the border around
	// a shrunken picture is black, not uninitialized memory.
	glViewport(0, 0, dst_full_w_px, dst_full_h_px);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// SDL rects are top-left origin; GL is bottom-left.
	const int sx0 = src_rect_px.x;
	const int sy0 = fb_h - (src_rect_px.y + src_rect_px.h);
	const int sx1 = src_rect_px.x + src_rect_px.w;
	const int sy1 = fb_h - src_rect_px.y;

	const int dx0 = dst_rect_px.x;
	const int dy0 = dst_full_h_px - (dst_rect_px.y + dst_rect_px.h);
	const int dx1 = dst_rect_px.x + dst_rect_px.w;
	const int dy1 = dst_full_h_px - dst_rect_px.y;

	glBlitFramebuffer(sx0, sy0, sx1, sy1,
	                  dx0, dy0, dx1, dy1,
	                  GL_COLOR_BUFFER_BIT, GL_LINEAR);

	if (srgb_was_enabled) {
		glEnable(GL_FRAMEBUFFER_SRGB);
	}
	if (scissor_was_enabled) {
		glEnable(GL_SCISSOR_TEST);
	}
	glViewport(prev_viewport[0],
	           prev_viewport[1],
	           prev_viewport[2],
	           prev_viewport[3]);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);

	static bool logged_once = false;
	if (!logged_once) {
		logged_once = true;
		LOG_MSG("OPENGL: XR mirror blit %dx%d -> eye slice",
		        src_rect_px.w, src_rect_px.h);
	}
}
#endif

OpenGlRenderer::SetShaderResult OpenGlRenderer::SetShader(const std::string& symbolic_shader_descriptor)
{
	return SetShaderInternal(symbolic_shader_descriptor);
}

OpenGlRenderer::SetShaderResult OpenGlRenderer::SetShaderInternal(
        const std::string& new_symbolic_shader_descriptor)
{
	using enum OpenGlRenderer::SetShaderResult;

	const auto new_descriptor = AutoShaderSwitcher::GetInstance().NotifyShaderChanged(
	        new_symbolic_shader_descriptor);

	const auto result = MaybeSwitchShaderAndPreset(curr_shader_descriptor,
	                                               new_descriptor);

	switch (result) {
	case Ok: {
		curr_shader_descriptor = new_descriptor;
		curr_symbolic_shader_descriptor = new_symbolic_shader_descriptor;
	} break;

	case PresetError: {
		curr_shader_descriptor = new_descriptor;
		curr_shader_descriptor.preset_name.clear();

		constexpr auto GlslExtension = ".glsl";
		auto descriptor = ShaderDescriptor::FromString(new_symbolic_shader_descriptor,
		                                               GlslExtension);
		descriptor.preset_name.clear();

		curr_symbolic_shader_descriptor = descriptor.ToString();
	} break;

	case ShaderError: break;

	default:
		assertm(false, "Invalid SetShaderResult value");
		return ShaderError;
	}

	return result;
}

void OpenGlRenderer::ForceReloadCurrentShader()
{
	LOG_INFO("RENDER: Reloading current shader '%s'",
	         curr_shader_descriptor.ToString().c_str());

	const auto result = ShaderManager::GetInstance().ForceReloadShader(
	        curr_shader_descriptor);

	if (result) {
		const auto& [shader, preset] = *result;

		shader_pipeline->SetMainShader(shader);
		shader_pipeline->SetMainShaderPreset(preset);
	}
}

void OpenGlRenderer::NotifyVideoModeChanged(const VideoMode& video_mode)
{
	const auto canvas_size_px = GetCanvasSizeInPixels();

	// We always expect a valid canvas and DOS video mode
	assert(!canvas_size_px.IsEmpty());
	assert(video_mode.width > 0 && video_mode.height > 0);

	curr_video_mode = video_mode;

	// Handle shader auto-switching
	const auto new_descriptor = AutoShaderSwitcher::GetInstance().NotifyRenderParametersChanged(
	        curr_shader_descriptor, canvas_size_px, video_mode);

	HandleShaderAndPresetChangeViaNotify(new_descriptor);

	shader_pipeline->NotifyVideoModeChanged(curr_video_mode);
}

OpenGlRenderer::SetShaderResult OpenGlRenderer::MaybeSwitchShaderAndPreset(
        const ShaderDescriptor& curr_descriptor, const ShaderDescriptor& new_descriptor)
{
	const auto changed_shader = (curr_descriptor.shader_name !=
	                             new_descriptor.shader_name);

	const auto changed_preset = (curr_descriptor.preset_name !=
	                             new_descriptor.preset_name);

	using enum OpenGlRenderer::SetShaderResult;

	if (!changed_shader && !changed_preset) {
		// No change; report success
		return Ok;
	}

	if (changed_shader) {
		if (!SwitchShader(new_descriptor.shader_name)) {
			// Loading shader failed; report error
			return ShaderError;
		}
	}

	main_shader_preset = ShaderManager::GetInstance().LoadShaderPresetOrDefault(
	        new_descriptor);

	shader_pipeline->SetMainShaderPreset(main_shader_preset);

	curr_shader_descriptor.preset_name = main_shader_preset.name;

	MaybeUpdateRenderSize(input_texture.width, input_texture.height);
	return Ok;
}

bool OpenGlRenderer::SwitchShader(const std::string& shader_name)
{
	const auto maybe_shader = ShaderManager::GetInstance().LoadShader(shader_name);
	if (!maybe_shader) {
		return false;
	}

	const auto& shader = *maybe_shader;
	main_shader_info   = shader.info;

	shader_pipeline->SetMainShader(shader);
	return true;
}

void OpenGlRenderer::SetVsync(const bool is_enabled)
{
	const auto swap_interval = is_enabled ? 1 : 0;

	if (!SDL_GL_SetSwapInterval(swap_interval)) {
		// The requested swap_interval is not supported
		LOG_ERR("OPENGL: Error %s vsync: %s",
		        (is_enabled ? "enabling" : "disabling"),
		        SDL_GetError());
	}
}

void OpenGlRenderer::SetColorSpace(const ColorSpace color_space)
{
	shader_pipeline->SetColorSpace(color_space);
}

void OpenGlRenderer::EnableImageAdjustments(const bool enable)
{
	shader_pipeline->EnableImageAdjustments(enable);
}

void OpenGlRenderer::SetImageAdjustmentSettings(const ImageAdjustmentSettings& settings)
{
	shader_pipeline->SetImageAdjustmentSettings(settings);
}

void OpenGlRenderer::SetDeditheringStrength(const float strength)
{
	shader_pipeline->SetDeditheringStrength(strength);
}

ShaderInfo OpenGlRenderer::GetCurrentShaderInfo()
{
	return main_shader_info;
}

ShaderPreset OpenGlRenderer::GetCurrentShaderPreset()
{
	return main_shader_preset;
}

std::string OpenGlRenderer::GetCurrentSymbolicShaderDescriptor()
{
	return curr_symbolic_shader_descriptor;
}

ShaderDescriptor OpenGlRenderer::GetCurrentShaderDescriptor()
{
	return curr_shader_descriptor;
}

RenderedImage OpenGlRenderer::ReadPixelsPostShader(const DosBox::Rect output_rect_px)
{
	// Create new image
	RenderedImage image = {};

	image.params.width              = iroundf(output_rect_px.w);
	image.params.height             = iroundf(output_rect_px.h);
	image.params.double_width       = false;
	image.params.double_height      = false;
	image.params.pixel_aspect_ratio = {1};
	image.params.pixel_format       = PixelFormat::BGR24_ByteArray;

	constexpr auto BitsInByte = 8;

	image.pitch = image.params.width *
	              (get_bits_per_pixel(image.params.pixel_format) / BitsInByte);

	const auto image_size_bytes = check_cast<uint32_t>(image.params.height *
	                                                   image.pitch);

	image.image_data = new uint8_t[image_size_bytes];

	image.is_flipped_vertically = true;

	// Read from backbuffer
	glReadBuffer(GL_BACK);

	// Alignment is 4 by default which works fine when using the
	// GL_BGRA pixel format with glReadPixels(). We need to set it 1
	// to be able to use the GL_BGR format in order to conserve
	// memory. This should not cause any slowdowns whatsoever.
	//
	// TODO measure if this is actually true with some 4k captures and
	// potentially revert to the default 4-byte alignment
	glPixelStorei(GL_PACK_ALIGNMENT, 1);

	glReadPixels(iroundf(output_rect_px.x),
	             iroundf(output_rect_px.y),
	             image.params.width,
	             image.params.height,
	             GL_BGR,
	             GL_UNSIGNED_BYTE,
	             image.image_data);

	// Restore default
	glPixelStorei(GL_PACK_ALIGNMENT, 4);

	return image;
}

uint32_t OpenGlRenderer::MakePixel(const uint8_t red, const uint8_t green,
                                   const uint8_t blue)
{
	return ((blue << 0) | (green << 8) | (red << 16)) | (255 << 24);
}

#endif // C_OPENGL
