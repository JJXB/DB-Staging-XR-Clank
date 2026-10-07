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
	if (xr_osd_tex) {
		glDeleteTextures(1, &xr_osd_tex);
		xr_osd_tex = 0;
	}
	xr_osd_tex_w = 0;
	xr_osd_tex_h = 0;
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
	shader_pipeline->Render(vao);

#if C_OPENXR
	// Copy the just-rendered mirror image into the XR swapchain while
	// the backbuffer is still valid (pre-swap).
	OPENXR_BlitMirrorToSwapchain();
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
// Minimal 5x7 bitmap font for overlay key caps (X = ink, . = empty).
// Covers key labels only: digits, lowercase, comma/period, and the
// capitals used by the wide-key abbreviations (B, E, S).
struct XrFontGlyph {
	char        ch;
	const char* rows[7];
};

static const XrFontGlyph xr_font[] = {
        {'0', {"01110", "10001", "10011", "10101", "11001", "10001", "01110"}},
        {'1', {"00100", "01100", "00100", "00100", "00100", "00100", "01110"}},
        {'2', {"01110", "10001", "00001", "00110", "01000", "10000", "11111"}},
        {'3', {"11111", "00010", "00100", "00010", "00001", "10001", "01110"}},
        {'4', {"00010", "00110", "01010", "10010", "11111", "00010", "00010"}},
        {'5', {"11111", "10000", "11110", "00001", "00001", "10001", "01110"}},
        {'6', {"00110", "01000", "10000", "11110", "10001", "10001", "01110"}},
        {'7', {"11111", "00001", "00010", "00100", "01000", "01000", "01000"}},
        {'8', {"01110", "10001", "10001", "01110", "10001", "10001", "01110"}},
        {'9', {"01110", "10001", "10001", "01111", "00001", "00010", "01100"}},
        {'a', {"00000", "00000", "01110", "00001", "01111", "10001", "01111"}},
        {'b', {"10000", "10000", "10110", "11001", "10001", "10001", "10110"}},
        {'c', {"00000", "00000", "01110", "10000", "10000", "10001", "01110"}},
        {'d', {"00001", "00001", "01101", "11011", "10011", "10011", "01111"}},
        {'e', {"00000", "00000", "01110", "10001", "11111", "10000", "01110"}},
        {'f', {"00110", "01001", "01000", "11100", "01000", "01000", "01000"}},
        {'g', {"00000", "00000", "01111", "10001", "01111", "00001", "01110"}},
        {'h', {"10000", "10000", "10110", "11001", "10001", "10001", "10001"}},
        {'i', {"00100", "00000", "01100", "00100", "00100", "00100", "01110"}},
        {'j', {"00010", "00000", "00110", "00010", "00010", "10010", "01100"}},
        {'k', {"10000", "10000", "10010", "10100", "11000", "10100", "10010"}},
        {'l', {"01100", "00100", "00100", "00100", "00100", "00100", "01110"}},
        {'m', {"00000", "00000", "11010", "10101", "10101", "10101", "10101"}},
        {'n', {"00000", "00000", "10110", "11001", "10001", "10001", "10001"}},
        {'o', {"00000", "00000", "01110", "10001", "10001", "10001", "01110"}},
        {'p', {"00000", "00000", "10110", "11001", "10110", "10000", "10000"}},
        {'q', {"00000", "00000", "01101", "11011", "01111", "00001", "00001"}},
        {'r', {"00000", "00000", "10111", "11000", "10000", "10000", "10000"}},
        {'s', {"00000", "00000", "01111", "10000", "01110", "00001", "11110"}},
        {'t', {"01000", "01000", "11100", "01000", "01000", "01001", "00110"}},
        {'u', {"00000", "00000", "10001", "10001", "10001", "10001", "01110"}},
        {'v', {"00000", "00000", "10001", "10001", "10001", "01010", "00100"}},
        {'w', {"00000", "00000", "10001", "10001", "10101", "10101", "01010"}},
        {'x', {"00000", "00000", "10001", "10001", "01110", "10001", "10001"}},
        {'y', {"00000", "00000", "10001", "10001", "01111", "00001", "01110"}},
        {'z', {"00000", "00000", "11111", "00010", "00100", "01000", "11111"}},
        {',', {"00000", "00000", "00000", "00000", "01100", "00100", "01000"}},
        {'.', {"00000", "00000", "00000", "00000", "00000", "01100", "01100"}},
        {'/', {"00001", "00010", "00010", "00100", "01000", "01000", "10000"}},
        {'B', {"11110", "10001", "10001", "11110", "10001", "10001", "11110"}},
        {'E', {"11111", "10000", "10000", "11110", "10000", "10000", "11111"}},
        {'S', {"01111", "10000", "10000", "01110", "00001", "00001", "11110"}},
        {'C', {"01110", "10001", "10000", "10000", "10000", "10001", "01110"}},
        {'A', {"01110", "10001", "10001", "11111", "10001", "10001", "10001"}},
        {'T', {"11111", "00100", "00100", "00100", "00100", "00100", "00100"}},
        {'D', {"11110", "10001", "10001", "10001", "10001", "10001", "11110"}},
        {'F', {"11111", "10000", "10000", "11110", "10000", "10000", "10000"}},
        {'G', {"01110", "10001", "10000", "10111", "10001", "10001", "01111"}},
        {'H', {"10001", "10001", "10001", "11111", "10001", "10001", "10001"}},
        {'I', {"01110", "00100", "00100", "00100", "00100", "00100", "01110"}},
        {'J', {"00111", "00010", "00010", "00010", "00010", "10010", "01100"}},
        {'K', {"10001", "10010", "10100", "11000", "10100", "10010", "10001"}},
        {'L', {"10000", "10000", "10000", "10000", "10000", "10000", "11111"}},
        {'M', {"10001", "11011", "10101", "10101", "10001", "10001", "10001"}},
        {'N', {"10001", "11001", "10101", "10011", "10001", "10001", "10001"}},
        {'O', {"01110", "10001", "10001", "10001", "10001", "10001", "01110"}},
        {'P', {"11110", "10001", "10001", "11110", "10000", "10000", "10000"}},
        {'Q', {"01110", "10001", "10001", "10001", "10101", "10010", "01101"}},
        {'R', {"11110", "10001", "10001", "11110", "10100", "10010", "10001"}},
        {'U', {"10001", "10001", "10001", "10001", "10001", "10001", "01110"}},
        {'V', {"10001", "10001", "10001", "10001", "10001", "01010", "00100"}},
        {'W', {"10001", "10001", "10001", "10101", "10101", "11011", "10001"}},
        {'X', {"10001", "10001", "01010", "00100", "01010", "10001", "10001"}},
        {'Y', {"10001", "10001", "01010", "00100", "00100", "00100", "00100"}},
        {'Z', {"11111", "00001", "00010", "00100", "01000", "10000", "11111"}},
        {':', {"00000", "01100", "01100", "00000", "01100", "01100", "00000"}},
        {'-', {"00000", "00000", "00000", "11111", "00000", "00000", "00000"}},
        {'(', {"00010", "00100", "01000", "01000", "01000", "00100", "00010"}},
        {')', {"01000", "00100", "00010", "00010", "00010", "00100", "01000"}},
        {'+', {"00000", "00100", "00100", "11111", "00100", "00100", "00000"}},
        {'%', {"11001", "11010", "00010", "00100", "01000", "01011", "10011"}},
        {'=', {"00000", "00000", "11111", "00000", "11111", "00000", "00000"}},
        {'^', {"00100", "01110", "10101", "00100", "00100", "00000", "00000"}},
        {'<', {"00010", "00100", "01000", "10000", "01000", "00100", "00010"}},
        {'>', {"01000", "00100", "00010", "00001", "00010", "00100", "01000"}},
        {' ', {"00000", "00000", "00000", "00000", "00000", "00000", "00000"}},
        {'_', {"00000", "00000", "00000", "00000", "00000", "00000", "11111"}},
};

static const char* const* xr_find_glyph(const char c)
{
	for (const auto& g : xr_font) {
		if (g.ch == c) {
			return g.rows;
		}
	}
	return nullptr;
}

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

	// Memory order R,G,B,A packed little-endian (0xAABBGGRR).
	// Mid-grey panel so the overlay reads against a black void.
	constexpr uint32_t BgColor      = 0xFF80726B;
	constexpr uint32_t KeyFill      = 0xFF382C26;
	constexpr uint32_t KeyFillWide  = 0xFF1A2F3A;
	constexpr uint32_t KeyBorder    = 0xFF6A5046;
	constexpr uint32_t KeyText      = 0xFFE6E6E6;
	constexpr uint32_t KeyTextWide  = 0xFFFFD479;
	constexpr uint32_t KeyTextPressed = 0xFF7FD4FF; // trigger-held letter

	std::vector<uint32_t> img(static_cast<size_t>(tex_w_px) *
	                                  static_cast<size_t>(tex_h_px),
	                          BgColor);

	auto set_px = [&](const int x, const int y, const uint32_t color) {
		if (x < 0 || y < 0 || x >= tex_w_px || y >= tex_h_px) {
			return;
		}
		// GL uploads memory row 0 as the texture bottom, while the
		// layout coords run y-down from the top: store flipped so
		// the keys read upright in the headset.
		img[(static_cast<size_t>(tex_h_px - 1 - y)) *
		            static_cast<size_t>(tex_w_px) +
		    static_cast<size_t>(x)] = color;
	};

	for (size_t k = 0; k < num_keys; ++k) {
		const auto& key = keys[k];
		if (key.x1 <= key.x0 || key.y1 <= key.y0) {
			continue;
		}
		const uint32_t fill = key.highlight ? KeyFillWide : KeyFill;
		// The pressed key (trigger held) lights its letter; hover
		// never touches letters, outline only.
		const uint32_t text =
		        (static_cast<int>(k) == pressed_index)
		                ? KeyTextPressed
		                : (key.highlight ? KeyTextWide : KeyText);
		for (int y = key.y0; y < key.y1; ++y) {
			for (int x = key.x0; x < key.x1; ++x) {
				set_px(x, y, fill);
			}
		}
		// 2px border.
		for (int x = key.x0; x < key.x1; ++x) {
			for (int b = 0; b < 2; ++b) {
				set_px(x, key.y0 + b, KeyBorder);
				set_px(x, key.y1 - 1 - b, KeyBorder);
			}
		}
		for (int y = key.y0; y < key.y1; ++y) {
			for (int b = 0; b < 2; ++b) {
				set_px(key.x0 + b, y, KeyBorder);
				set_px(key.x1 - 1 - b, y, KeyBorder);
			}
		}
		// Centered label.
		int len = 0;
		while (key.text[len] != '\0') {
			++len;
		}
		if (len > 0) {
			const int avail_w = (key.x1 - key.x0) - 16;
			const int avail_h = (key.y1 - key.y0) - 20;
			int scale         = avail_w / (6 * len - 1);
			const int vscale  = avail_h / 7;
			if (vscale < scale) {
				scale = vscale;
			}
			if (scale >= 1) {
				const int tw = scale * (6 * len - 1);
				const int th = scale * 7;
				int tx       = key.x0 + ((key.x1 - key.x0) - tw) / 2;
				const int ty = key.y0 + ((key.y1 - key.y0) - th) / 2;
				for (int ci = 0; ci < len; ++ci) {
					const char* const* rows = xr_find_glyph(
					        key.text[ci]);
					if (!rows) {
						tx += scale * 6;
						continue;
					}
					for (int gy = 0; gy < 7; ++gy) {
						for (int gx = 0; gx < 5; ++gx) {
							if (rows[gy][gx] != '1') {
								continue;
							}
							for (int sy = 0; sy < scale;
							     ++sy) {
								for (int sx = 0; sx < scale;
								     ++sx) {
									set_px(tx + gx * scale + sx,
									       ty + gy * scale + sy,
									       text);
								}
							}
						}
					}
					tx += scale * 6;
				}
			}
		}
	}
	// Bright outline around the hovered key (if any), drawn inside its
	// rect so neighbours are untouched.
	if (hover_index >= 0 &&
	    static_cast<size_t>(hover_index) < num_keys) {
		constexpr uint32_t KeyHover = 0xFF7FD4FF;
		const auto& hk = keys[hover_index];
		if (hk.x1 > hk.x0 && hk.y1 > hk.y0) {
			for (int x = hk.x0; x < hk.x1; ++x) {
				for (int b = 0; b < 3; ++b) {
					set_px(x, hk.y0 + b, KeyHover);
					set_px(x, hk.y1 - 1 - b, KeyHover);
				}
			}
			for (int y = hk.y0; y < hk.y1; ++y) {
				for (int b = 0; b < 3; ++b) {
					set_px(hk.x0 + b, y, KeyHover);
					set_px(hk.x1 - 1 - b, y, KeyHover);
				}
			}
		}
	}

	glGenTextures(1, &xr_kb_tex);
	glBindTexture(GL_TEXTURE_2D, xr_kb_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	// Opacity for compositor blending (xr_keyboard_opacity): scale the
	// texture alpha uniformly; colors stay straight (non-premultiplied).
	if (opacity < 1.0f) {
		const uint32_t alpha =
		        static_cast<uint32_t>(opacity * 255.0f + 0.5f);
		for (auto& px : img) {
			px = (px & 0x00FFFFFFu) | (alpha << 24);
		}
	}
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

	size_t inked = 0;
	for (const auto px : img) {
		if (px != BgColor) {
			++inked;
		}
	}
	// One-time log; rebuilds happen on every hover change.
	static bool build_logged = false;
	if (!build_logged) {
		build_logged = true;
		LOG_MSG("OPENGL: Built XR keyboard texture %dx%d (%zu key pixels)",
		        tex_w_px,
		        tex_h_px,
		        inked);
	}

	xr_kb_tex_w = tex_w_px;
	xr_kb_tex_h = tex_h_px;
	xr_kb_hover_built = hover_index;
	xr_kb_pressed_built = pressed_index;
	xr_kb_opacity_built = opacity;

	return xr_kb_tex;
}

GLuint OpenGlRenderer::UpdateXrOsdTexture(const char* const* lines,
                                           size_t num_lines, int tex_w_px,
                                           int tex_h_px)
{
	if (!lines || num_lines == 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return 0;
	}
	if (num_lines > 4) {
		num_lines = 4;
	}

	// Straight-alpha RGBA: transparent background, opaque line boxes.
	constexpr uint32_t BgClear  = 0x00000000;
	constexpr uint32_t BoxFill   = 0xCC100C0A;
	constexpr uint32_t BoxBorder = 0xFF6A5046;
	constexpr uint32_t OsdText   = 0xFFE6E6E6;

	std::vector<uint32_t> img(static_cast<size_t>(tex_w_px) *
	                                  static_cast<size_t>(tex_h_px),
	                          BgClear);

	auto set_px = [&](const int x, const int y, const uint32_t color) {
		if (x < 0 || y < 0 || x >= tex_w_px || y >= tex_h_px) {
			return;
		}
		// Same bottom-origin storage as the keyboard rasterizer:
		// memory row 0 is the texture bottom.
		img[(static_cast<size_t>(tex_h_px - 1 - y)) *
		            static_cast<size_t>(tex_w_px) +
		    static_cast<size_t>(x)] = color;
	};

	const int row_h = tex_h_px / static_cast<int>(num_lines);
	for (size_t li = 0; li < num_lines; ++li) {
		const int y0 = static_cast<int>(li) * row_h;
		const int y1 = y0 + row_h - 6; // gap between bars
		for (int y = y0; y < y1; ++y) {
			for (int x = 0; x < tex_w_px; ++x) {
				set_px(x, y, BoxFill);
			}
		}
		for (int x = 0; x < tex_w_px; ++x) {
			set_px(x, y0, BoxBorder);
			set_px(x, y0 + 1, BoxBorder);
			set_px(x, y1 - 1, BoxBorder);
			set_px(x, y1 - 2, BoxBorder);
		}

		int len = 0;
		while (lines[li][len] != '\0') {
			++len;
		}
		if (len == 0) {
			continue;
		}
		const int avail_w = tex_w_px - 32;
		const int avail_h = (y1 - y0) - 20;
		int scale         = avail_w / (6 * len - 1);
		const int vscale  = avail_h / 7;
		if (vscale < scale) {
			scale = vscale;
		}
		if (scale < 1) {
			continue;
		}
		const int tw = scale * (6 * len - 1);
		int tx       = 16;
		if (tw < avail_w) {
			tx += (avail_w - tw) / 2;
		}
		const int ty = y0 + ((y1 - y0) - scale * 7) / 2;
		for (int ci = 0; ci < len; ++ci) {
			const char* const* rows = xr_find_glyph(lines[li][ci]);
			if (rows) {
				for (int gy = 0; gy < 7; ++gy) {
					for (int gx = 0; gx < 5; ++gx) {
						if (rows[gy][gx] != '1') {
							continue;
						}
						for (int sy = 0; sy < scale; ++sy) {
							for (int sx = 0; sx < scale; ++sx) {
								set_px(tx + gx * scale + sx,
								       ty + gy * scale + sy,
								       OsdText);
							}
						}
					}
				}
			}
			tx += scale * 6;
		}
	}

	if (xr_osd_tex && (xr_osd_tex_w != tex_w_px || xr_osd_tex_h != tex_h_px)) {
		glDeleteTextures(1, &xr_osd_tex);
		xr_osd_tex   = 0;
		xr_osd_tex_w = 0;
		xr_osd_tex_h = 0;
	}
	if (!xr_osd_tex) {
		glGenTextures(1, &xr_osd_tex);
	}
	glBindTexture(GL_TEXTURE_2D, xr_osd_tex);
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

	xr_osd_tex_w = tex_w_px;
	xr_osd_tex_h = tex_h_px;
	return xr_osd_tex;
}

GLuint OpenGlRenderer::GetXrRayTexture()
{
	if (xr_ray_tex) {
		return xr_ray_tex;
	}
	// Memory order R,G,B,A packed little-endian (0xAABBGGRR).
	// Cyan-white core with soft side edges and a fade at both ends
	// (U runs along the ribbon); straight alpha for compositor
	// blending.
	constexpr int W = 128, H = 16;
	std::vector<uint32_t> img(static_cast<size_t>(W) * H, 0);
	for (int y = 0; y < H; ++y) {
		const float v  = (y + 0.5f) / H * 2.0f - 1.0f; // -1..1
		const float ev = std::max(0.0f, 1.0f - v * v);
		for (int x = 0; x < W; ++x) {
			const float u  = (x + 0.5f) / W;
			const float eu = std::min(1.0f,
			                          std::min(u, 1.0f - u) * 12.0f);
			const float a = ev * std::max(0.0f, eu);
			const uint32_t alpha =
			        static_cast<uint32_t>(190.0f * a + 0.5f);
			img[static_cast<size_t>(y) * W + x] =
			        (alpha << 24) | 0x00FFD47Fu; // BGR: 7F D4 FF
		}
	}
	glGenTextures(1, &xr_ray_tex);
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
	return xr_ray_tex;
}

GLuint OpenGlRenderer::UpdateXrMenuTexture(const XrMenuRow* rows,
                                           int num_rows, int tex_w_px,
                                           int tex_h_px, float font_mult)
{
	if (!rows || num_rows <= 0 || tex_w_px <= 0 || tex_h_px <= 0) {
		return 0;
	}
	// Memory order R,G,B,A packed little-endian (0xAABBGGRR), same
	// palette idiom as the keyboard/OSD builders: dark panel, light
	// text, amber headers, cyan highlight/active.
	constexpr uint32_t BgPanel  = 0xF2261C14;
	constexpr uint32_t Border   = 0xFF46506A;
	constexpr uint32_t MenuText = 0xFFE6E6E6;
	constexpr uint32_t DimText  = 0xFF868F9A;
	constexpr uint32_t Header   = 0xFF79D4FF;
	constexpr uint32_t Accent   = 0xFFFFD47F;

	std::vector<uint32_t> img(static_cast<size_t>(tex_w_px) * tex_h_px,
	                          BgPanel);
	auto set_px = [&](int x, int y, uint32_t c) {
		if (x < 0 || y < 0 || x >= tex_w_px || y >= tex_h_px) {
			return;
		}
		// Flip: raster rows are top-down, texture row 0 is bottom.
		img[static_cast<size_t>(tex_h_px - 1 - y) * tex_w_px + x] = c;
	};
	auto fill_box = [&](int x0, int y0, int x1, int y1, uint32_t c) {
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				set_px(x, y, c);
			}
		}
	};
	auto hollow_box = [&](int x0, int y0, int x1, int y1, uint32_t c) {
		for (int x = x0; x < x1; ++x) {
			set_px(x, y0, c);
			set_px(x, y1 - 1, c);
		}
		for (int y = y0; y < y1; ++y) {
			set_px(x0, y, c);
			set_px(x1 - 1, y, c);
		}
	};
	auto text_w = [](const char* s, int scale) {
		int n = 0;
		for (const char* p = s; p && *p; ++p) {
			++n;
		}
		return n ? scale * (6 * n - 1) : 0;
	};
	auto draw_text = [&](const char* s, int tx, int ty, int scale,
	                     uint32_t color) {
		for (const char* p = s; p && *p; ++p) {
			const char* const* glyph = xr_find_glyph(*p);
			if (!glyph) {
				tx += scale * 6; // missing glyph: blank advance
				continue;
			}
			for (int gy = 0; gy < 7; ++gy) {
				for (int gx = 0; gx < 5; ++gx) {
					if (glyph[gy][gx] != '1') {
						continue;
					}
					for (int sy = 0; sy < scale; ++sy) {
						for (int sx = 0; sx < scale;
						     ++sx) {
							set_px(tx + gx * scale + sx,
							       ty + gy * scale + sy,
							       color);
						}
					}
				}
			}
			tx += scale * 6;
		}
	};

	const int row_h = tex_h_px / num_rows;
	if (row_h < 12) {
		return 0;
	}
	if (font_mult < 0.5f) {
		font_mult = 0.5f;
	}
	if (font_mult > 2.0f) {
		font_mult = 2.0f;
	}
	const int pad = 12;

	for (int i = 0; i < num_rows; ++i) {
		const XrMenuRow& r = rows[i];
		const int y0       = i * row_h;
		const int y1       = (i == num_rows - 1) ? tex_h_px
		                                        : (i + 1) * row_h;
		// Fit the glyph scale to the row height, times the user
		// font size; never taller than the row itself. Pixels past
		// the texture edge clip in set_px.
		int scale = static_cast<int>((y1 - y0 - 12) * font_mult / 7);
		const int max_scale = (y1 - y0 - 4) / 7;
		if (scale < 1) {
			scale = 1;
		}
		if (scale > max_scale) {
			scale = max_scale;
		}
		if (scale < 1) {
			scale = 1;
		}
		const int ty = y0 + ((y1 - y0) - 7 * scale) / 2;

		// Hovered row gets a cyan outline.
		if (r.highlight) {
			hollow_box(2, y0 + 2, tex_w_px - 2, y1 - 2, Accent);
			hollow_box(3, y0 + 3, tex_w_px - 3, y1 - 3, Accent);
		}

		switch (r.kind) {
		case 0: { // section header: centered amber text + rule
			const int tw = text_w(r.label, scale);
			draw_text(r.label, (tex_w_px - tw) / 2, ty, scale,
			          Header);
			fill_box(pad, y1 - 3, tex_w_px - pad, y1 - 2,
			         Border);
			break;
		}
		case 5: { // centered button: [ label ]
			const std::string buf =
			        std::string("[ ") + (r.label ? r.label : "") +
			        " ]";
			const int tw = text_w(buf.c_str(), scale);
			const int tx = (tex_w_px - tw) / 2;
			// Buttons light amber on hover (vs the cyan row
			// outline) so actionable rows read distinctly.
			const uint32_t hot = r.highlight ? Header : DimText;
			hollow_box(tx - 8, y0 + 4, tx + tw + 8, y1 - 4, hot);
			draw_text(buf.c_str(), tx, ty, scale,
			          r.highlight ? Header : MenuText);
			break;
		}
		default: {
			// Left caption.
			if (r.label && *r.label) {
				draw_text(r.label, pad, ty, scale, MenuText);
			}
			if (r.kind == 1) { // toggle: right-aligned value
				const char* v  = (r.value && *r.value)
				                         ? r.value
				                         : "OFF";
				const uint32_t c = r.active ? Accent
				                            : DimText;
				const int tw = text_w(v, scale);
				draw_text(v, tex_w_px - pad - tw, ty, scale,
				          c);
			} else if (r.kind == 2 || r.kind == 3) {
				// slider/list: right-aligned "< value >"; the
				// arrows light amber on hover like buttons.
				const uint32_t arrow = r.highlight ? Header
				                                   : DimText;
				const char* v = (r.value && *r.value)
				                        ? r.value
				                        : "";
				const int vw = text_w(v, scale);
				const int aw = text_w("<", scale);
				const int gap = 2 * scale;
				int tx = tex_w_px - pad - text_w(">", scale);
				draw_text(">", tx, ty, scale, arrow);
				tx -= gap + vw;
				draw_text(v, tx, ty, scale, MenuText);
				tx -= gap + aw;
				draw_text("<", tx, ty, scale, arrow);
			} else if (r.kind == 4) {
				// text field: value after the caption.
				const int lw = text_w(r.label ? r.label
				                              : "",
				                      scale);
				const char* v = (r.value && *r.value)
				                        ? r.value
				                        : "_";
				const int fx0 = pad + lw + 3 * scale;
				const int fx1 = tex_w_px - pad;
				hollow_box(fx0 - 4, y0 + 4, fx1 + 4, y1 - 4,
				           r.highlight ? Accent : Border);
				// Clip the value to the field width.
				char clip[64];
				int maxc = (fx1 - fx0) / (6 * scale);
				if (maxc < 1) {
					maxc = 1;
				}
				if (maxc > 63) {
					maxc = 63;
				}
				int ci = 0;
				for (const char* p = v;
				     p && *p && ci < maxc; ++p, ++ci) {
					clip[ci] = *p;
				}
				clip[ci] = 0;
				draw_text(clip, fx0, ty, scale, MenuText);
			} else if (r.kind == 6 || r.kind == 7) {
				// save/load: right-aligned [ action ] button,
				// amber on hover like other buttons.
				const std::string buf =
				        std::string("[ ") +
				        (r.action ? r.action : "") + " ]";
				const int tw = text_w(buf.c_str(), scale);
				const int tx = tex_w_px - pad - tw;
				const uint32_t hot = r.highlight ? Header
				                                 : DimText;
				hollow_box(tx - 8, y0 + 4, tx + tw + 8,
				           y1 - 4, hot);
				draw_text(buf.c_str(), tx, ty, scale,
				          r.highlight ? Header
				                          : MenuText);
				// Left content: "File: name" or
				// "< preset >".
				std::string left;
				if (r.kind == 6) {
					left = std::string("File: ") +
					       ((r.value && *r.value)
					               ? r.value
					               : "_");
				} else {
					left = std::string("< ") +
					       ((r.value && *r.value)
					               ? r.value
					               : "") +
					       " >";
				}
				const int lw = text_w(r.label ? r.label
				                              : "",
				                      scale);
				draw_text(left.c_str(), pad + lw + 3 * scale, ty,
				          scale, MenuText);
			}
			break;
		}
		}
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
