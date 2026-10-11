// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// OpenXR session core (see xr.h): instance, session, spaces,
// swapchains, views, projections, frame loop, events, recovery, the
// [xr] section accessor + config, and composition-layer placement.

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
#include "gui/osd.h"
#include "gui/xrinput.h"
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
// OpenXR state
XrInstance              sdl_xr_instance      = XR_NULL_HANDLE;
XrSystemId              sdl_xr_systemId      = XR_NULL_SYSTEM_ID;
XrSession               sdl_xr_session       = XR_NULL_HANDLE;
XrSpace                 sdl_xr_app_space     = XR_NULL_HANDLE;
XrSwapchain             sdl_xr_swapchain     = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_swapchain_images;

// ---- moved to gui/osd.cpp: kbd/osd globals (was sdl_gui.cpp:122-172) ----


// ---- moved to gui/osd.cpp: menu/ray/size globals (was sdl_gui.cpp:173-199) ----


// ---- moved to gui/xrstereo.cpp: eye/depth/autodepth/voodoo buffers (was sdl_gui.cpp:200-216) ----

// Bounded compositor wait shared by the swapchain acquire paths: an
// unresponsive compositor must stall the emulator thread for at most
// this long (Meta runtimes reject release-before-wait outright).
static constexpr XrDuration OpenXrSwapchainWaitTimeoutNs = 10'000'000; // 10 ms
int64_t                 sdl_xr_swapchain_format = 0;
uint32_t                sdl_xr_swapchain_width  = 0;
uint32_t                sdl_xr_swapchain_height = 0;
static uint32_t                sdl_xr_swapchain_index  = 0;
// Index of the acquired swapchain image the mirror blit targets, or
// -1 when no XR frame is in flight.
int                     sdl_xr_active_image     = -1;
// Picture size is the xr_picture_size setting (percent of the eye
// texture); the zoom shortcuts adjust it live.
// Per-frame timing accumulators for slowdown diagnosis (microseconds,
// logged as averages every 600 submitted frames).
static uint64_t sdl_xr_stat_frames       = 0;
static uint64_t sdl_xr_stat_wait_us      = 0;
static uint64_t sdl_xr_stat_total_us     = 0;
static uint64_t sdl_xr_stat_wait_max_us  = 0;
static uint64_t sdl_xr_stat_locate_us    = 0;
static uint64_t sdl_xr_stat_acquire_us   = 0;
uint64_t         sdl_xr_stat_mirror_us    = 0;
uint64_t sdl_xr_stat_blit_us      = 0;
static uint64_t sdl_xr_stat_submit_us    = 0;
static int64_t  sdl_xr_frame_t0_us       = 0;
// World-locked screen anchor: when locked, the submitted layer keeps
// this pose instead of following the headset, so the picture stays
// fixed in the room while you move.
bool    sdl_xr_world_locked = false;
bool    sdl_xr_has_lock     = false;
XrPosef sdl_xr_locked_pose[2] = {};
XrFovf  sdl_xr_locked_fov[2]  = {};
// Diagnostic: submit empty frames (no layers) to isolate compositor
// cost of our layer content vs. submit pacing overhead.
bool    sdl_xr_submit_blank = false;
// Graceful-loss handling: last failure (for once-per-cause logging),
// next retry time, and retry cooldown for hot-plug/reconnect.
static XrResult  sdl_xr_last_error  = XR_SUCCESS;
static int64_t   sdl_xr_retry_at_us = 0;
static constexpr int64_t OpenXrRetryCooldownUs = 10'000'000; // 10 s
// Fallback bookkeeping: stop trying XR for this run after repeated
// bring-up failures that never produced a working session. A session
// that ran at least once keeps retrying forever (likely transient).
bool      sdl_xr_disabled    = false;
static bool      sdl_xr_ever_ran    = false;
static int       sdl_xr_init_fails  = 0;
static constexpr int OpenXrMaxInitAttempts = 6;
XrView                  sdl_xr_views[2]      = {};
static XrViewConfigurationView sdl_xr_config_views[2] = {};
static XrViewConfigurationType sdl_xr_view_config_type =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
static XrFovf                  sdl_xr_eye_fov[2]    = {};
static float                   sdl_xr_proj_mats[2][16] = {};
static XrCompositionLayerProjection     sdl_xr_proj_layer       = {};
static XrCompositionLayerProjectionView sdl_xr_proj_layer_views[2] = {};
bool                    sdl_xr_initialized   = false;
bool                    sdl_xr_session_ready = false;
static XrSessionState          sdl_xr_session_state = XR_SESSION_STATE_UNKNOWN;
uint32_t                sdl_xr_view_count    = 0;

// Head pose proxy (filled by openxr_update_head_tracking, below).
XrPosef sdl_xr_head_pose       = {};
bool    sdl_xr_head_pose_valid = false;

// Forward declarations for the loss-handling helpers defined below
// (needed by the session/frame functions above them; cleanup is
// declared non-static in gui/xr.h).
static void openxr_teardown_session();
static bool openxr_note_failure(const char* what, const XrResult result);

static constexpr float OpenXrNearZ = 0.1f;
static constexpr float OpenXrFarZ  = 100.0f;

SectionProp* get_xr_section()
{
	auto section = get_section("xr");
	assert(section);
	return section;
}
// Screen anchor for pointer mapping: the world-locked anchor when
// locked, otherwise the live head pose.
bool openxr_use_world_lock()
{
	// Picture placement master switch: with xr_headset_picture_lock
	// on (default) the screen is glued to the headset; off puts it in
	// 3D space once a lock pose exists (recenter or lock toggle).
	if (get_xr_section()->GetBool("xr_headset_picture_lock")) {
		return false;
	}
	return sdl_xr_world_locked && sdl_xr_has_lock;
}

bool openxr_screen_anchor(XrPosef& anchor_out)
{
	if (openxr_use_world_lock()) {
		anchor_out.position.x =
		        (sdl_xr_locked_pose[0].position.x +
		         sdl_xr_locked_pose[1].position.x) *
		        0.5f;
		anchor_out.position.y =
		        (sdl_xr_locked_pose[0].position.y +
		         sdl_xr_locked_pose[1].position.y) *
		        0.5f;
		anchor_out.position.z =
		        (sdl_xr_locked_pose[0].position.z +
		         sdl_xr_locked_pose[1].position.z) *
		        0.5f;
		anchor_out.orientation = sdl_xr_locked_pose[0].orientation;
		return true;
	}
	if (sdl_xr_head_pose_valid) {
		anchor_out = sdl_xr_head_pose;
		return true;
	}
	return false;
}

// Screen-plane geometry shared by pointer mapping and the keyboard
// quad: anchor pose plus half extents at the configured distance.
bool openxr_screen_plane(XrPosef& anchor_out, float& half_w_out,
                                float& half_h_out, float& dist_out)
{
	if (!openxr_screen_anchor(anchor_out)) {
		return false;
	}
	const int dist_cm = get_xr_section()->GetInt("xr_screen_distance");
	const float dist_m =
	        ((dist_cm < 50 ? 50 : (dist_cm > 500 ? 500 : dist_cm)) / 100.0f);

	// Plane half-extents from the pointing-side eye FOV.
	const XrFovf& fov = sdl_xr_eye_fov[1];
	const float tan_h = std::max(std::fabs(tanf(fov.angleLeft)),
	                             std::fabs(tanf(fov.angleRight)));
	const float tan_v = std::max(std::fabs(tanf(fov.angleUp)),
	                             std::fabs(tanf(fov.angleDown)));
	if (tan_h <= 0.0f || tan_v <= 0.0f) {
		return false;
	}
	half_w_out = dist_m * tan_h;
	half_h_out = dist_m * tan_v;
	dist_out   = dist_m;
	return true;
}

// Overlay distance in meters from an xr_*_distance option (cm, clamped
// like the screen distance).
float openxr_layer_dist(const char* opt_name)
{
	int dist_cm = get_xr_section()->GetInt(opt_name);
	if (dist_cm < 50) {
		dist_cm = 50;
	}
	if (dist_cm > 500) {
		dist_cm = 500;
	}
	return dist_cm / 100.0f;
}
// Column-major OpenGL-style projection from an OpenXR FOV.
// Matches XrMatrix4x4_CreateProjectionFov in the OpenXR SDK.
static void openxr_fov_to_projection(const XrFovf& fov, float near_z,
                                     float far_z, float out_m[16])
{
	const float l = tanf(fov.angleLeft);
	const float r = tanf(fov.angleRight);
	const float u = tanf(fov.angleUp);
	const float d = tanf(fov.angleDown);

	const float w = r - l;
	const float h = u - d;

	out_m[0]  = 2.0f / w;
	out_m[1]  = 0.0f;
	out_m[2]  = 0.0f;
	out_m[3]  = 0.0f;

	out_m[4]  = 0.0f;
	out_m[5]  = 2.0f / h;
	out_m[6]  = 0.0f;
	out_m[7]  = 0.0f;

	out_m[8]  = (r + l) / w;
	out_m[9]  = (u + d) / h;
	out_m[10] = -(far_z + near_z) / (far_z - near_z);
	out_m[11] = -1.0f;

	out_m[12] = 0.0f;
	out_m[13] = 0.0f;
	out_m[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
	out_m[15] = 0.0f;
}

static void openxr_update_projections()
{
	for (uint32_t i = 0; i < sdl_xr_view_count && i < 2; ++i) {
		openxr_fov_to_projection(sdl_xr_eye_fov[i],
		                         OpenXrNearZ,
		                         OpenXrFarZ,
		                         sdl_xr_proj_mats[i]);
	}
}

static bool openxr_setup_view_configuration()
{
	uint32_t view_config_type_count = 0;
	XrResult result = xrEnumerateViewConfigurations(sdl_xr_instance,
	                                               sdl_xr_systemId,
	                                               0,
	                                               &view_config_type_count,
	                                               nullptr);
	if (result != XR_SUCCESS || view_config_type_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate view configurations: %d",
		        result);
		return false;
	}

	std::vector<XrViewConfigurationType> view_configs(view_config_type_count);
	result = xrEnumerateViewConfigurations(sdl_xr_instance,
	                                      sdl_xr_systemId,
	                                      view_config_type_count,
	                                      &view_config_type_count,
	                                      view_configs.data());
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate view configurations: %d",
		        result);
		return false;
	}

	bool found_stereo = false;
	for (auto config_type : view_configs) {
		if (config_type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
			sdl_xr_view_config_type = config_type;
			found_stereo            = true;
			break;
		}
	}
	if (!found_stereo) {
		LOG_ERR("OpenXR: Primary stereo view configuration not supported");
		return false;
	}
	LOG_MSG("OpenXR: Primary stereo view configuration supported");

	uint32_t view_count = 0;
	result = xrEnumerateViewConfigurationViews(sdl_xr_instance,
	                                          sdl_xr_systemId,
	                                          sdl_xr_view_config_type,
	                                          0,
	                                          &view_count,
	                                          nullptr);
	if (result != XR_SUCCESS || view_count == 0 || view_count > 2) {
		LOG_ERR("OpenXR: Unexpected view count: %u (result %d)",
		        view_count,
		        result);
		return false;
	}

	sdl_xr_view_count = view_count;

	std::vector<XrViewConfigurationView> view_views(view_count);
	for (auto& v : view_views) {
		v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		v.next = nullptr;
	}
	result = xrEnumerateViewConfigurationViews(sdl_xr_instance,
	                                          sdl_xr_systemId,
	                                          sdl_xr_view_config_type,
	                                          view_count,
	                                          &view_count,
	                                          view_views.data());
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate view configuration views: %d",
		        result);
		return false;
	}

	for (uint32_t i = 0; i < view_count; ++i) {
		sdl_xr_config_views[i] = view_views[i];
		LOG_MSG("OpenXR: Eye %u recommended resolution: %ux%u (samples: %u)",
		        i,
		        view_views[i].recommendedImageRectWidth,
		        view_views[i].recommendedImageRectHeight,
		        view_views[i].recommendedSwapchainSampleCount);

		// Init runtime XrView structs; pose/FOV are filled per-frame
		// by xrLocateViews().
		sdl_xr_views[i].type = XR_TYPE_VIEW;
		sdl_xr_views[i].next = nullptr;
	}

	// Seed projections with a symmetric ~90-degree default FOV until the
	// first xrLocateViews provides real per-eye FOVs.
	for (uint32_t i = 0; i < view_count; ++i) {
		sdl_xr_eye_fov[i].angleLeft  = -0.785398f;
		sdl_xr_eye_fov[i].angleRight = 0.785398f;
		sdl_xr_eye_fov[i].angleUp    = 0.785398f;
		sdl_xr_eye_fov[i].angleDown  = -0.785398f;
	}
	openxr_update_projections();

	return true;
}

#if defined(_WIN32)
static XrGraphicsBindingOpenGLWin32KHR sdl_xr_gfx_binding_win32 = {};
#elif defined(__linux__)
static XrGraphicsBindingOpenGLXlibKHR    sdl_xr_gfx_binding_xlib    = {};
static XrGraphicsBindingOpenGLWaylandKHR sdl_xr_gfx_binding_wayland = {};
#endif

// Resolve the OpenXR graphics binding from the live SDL window + GL
// context. Returns a pointer to static binding storage (valid until
// process exit) or nullptr when unavailable/unsupported.
static const void* openxr_get_graphics_binding()
{
#if !C_OPENGL
	LOG_ERR("OpenXR: OpenGL backend required for XR graphics binding");
	return nullptr;
#else
	if (!sdl.window) {
		LOG_ERR("OpenXR: No SDL window for graphics binding");
		return nullptr;
	}
	if (sdl.render_backend_type != RenderBackendType::OpenGl ||
	    !sdl.renderer) {
		LOG_ERR("OpenXR: OpenGL render backend required for XR graphics binding");
		return nullptr;
	}
	auto* gl_renderer = static_cast<OpenGlRenderer*>(sdl.renderer.get());
	SDL_GLContext gl_ctx = gl_renderer->GetGLContext();
	if (!gl_ctx) {
		gl_ctx = SDL_GL_GetCurrentContext();
	}
	if (!gl_ctx) {
		LOG_ERR("OpenXR: No OpenGL context for graphics binding");
		return nullptr;
	}

	// SDL 3.2+ exposes native handles via window properties
	// (SDL_syswm.h was removed upstream).
	const SDL_PropertiesID props = SDL_GetWindowProperties(sdl.window);
	if (!props) {
		LOG_ERR("OpenXR: SDL_GetWindowProperties failed: %s", SDL_GetError());
		return nullptr;
	}

#if defined(_WIN32)
	HDC hdc = static_cast<HDC>(SDL_GetPointerProperty(
	        props, SDL_PROP_WINDOW_WIN32_HDC_POINTER, nullptr));
	if (!hdc) {
		LOG_ERR("OpenXR: No HDC for Win32 GL binding");
		return nullptr;
	}
	sdl_xr_gfx_binding_win32.type  = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
	sdl_xr_gfx_binding_win32.next  = nullptr;
	sdl_xr_gfx_binding_win32.hDC   = hdc;
	sdl_xr_gfx_binding_win32.hGLRC = reinterpret_cast<HGLRC>(gl_ctx);
	return &sdl_xr_gfx_binding_win32;

#elif defined(__linux__)
	void* wl_display = SDL_GetPointerProperty(
	        props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
	if (wl_display) {
		sdl_xr_gfx_binding_wayland.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_WAYLAND_KHR;
		sdl_xr_gfx_binding_wayland.next = nullptr;
		sdl_xr_gfx_binding_wayland.display =
		        static_cast<struct wl_display*>(wl_display);
		return &sdl_xr_gfx_binding_wayland;
	}
	Display* dpy = static_cast<Display*>(SDL_GetPointerProperty(
	        props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
	const Window win = static_cast<Window>(SDL_GetNumberProperty(
	        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
	if (!dpy || !win) {
		LOG_ERR("OpenXR: No X11 display/window for GL binding");
		return nullptr;
	}
	// The Xlib binding needs the FBConfig matching SDL's visual.
	XWindowAttributes attrs = {};
	if (!XGetWindowAttributes(dpy, win, &attrs) || !attrs.visual) {
		LOG_ERR("OpenXR: XGetWindowAttributes failed for GL binding");
		return nullptr;
	}
	const int visualid = attrs.visual->visualid;
	const int screen   = static_cast<int>(SDL_GetNumberProperty(
                props,
                SDL_PROP_WINDOW_X11_SCREEN_NUMBER,
                static_cast<Uint64>(DefaultScreen(dpy))));
	int nconfigs       = 0;
	GLXFBConfig* configs =
	        glXChooseFBConfig(dpy, screen, nullptr, &nconfigs);
	GLXFBConfig chosen = nullptr;
	for (int i = 0; i < nconfigs; ++i) {
		int vid = 0;
		glXGetFBConfigAttrib(dpy, configs[i], GLX_VISUAL_ID, &vid);
		if (vid == visualid) {
			chosen = configs[i];
			break;
		}
	}
	if (!chosen) {
		LOG_ERR("OpenXR: No GLXFBConfig matches SDL visual for binding");
		if (configs) {
			XFree(configs);
		}
		return nullptr;
	}
	sdl_xr_gfx_binding_xlib.type        = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR;
	sdl_xr_gfx_binding_xlib.next        = nullptr;
	sdl_xr_gfx_binding_xlib.xDisplay    = dpy;
	sdl_xr_gfx_binding_xlib.visualid    = static_cast<uint32_t>(visualid);
	sdl_xr_gfx_binding_xlib.glxFBConfig = chosen;
	sdl_xr_gfx_binding_xlib.glxDrawable = win;
	sdl_xr_gfx_binding_xlib.glxContext  = reinterpret_cast<GLXContext>(gl_ctx);
	XFree(configs);
	return &sdl_xr_gfx_binding_xlib;

#else
	LOG_ERR("OpenXR: No GL graphics binding for this platform yet");
	return nullptr;
#endif
#endif // C_OPENGL
}

static bool openxr_check_graphics_requirements()
{
	// XR_KHR_opengl_enable mandates this call after xrCreateInstance
	// and before xrCreateSession; runtimes (e.g. SteamVR) fail
	// session creation with GRAPHICS_REQUIREMENTS_CALL_MISSING
	// otherwise.
	PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetRequirements = nullptr;
	XrResult result = xrGetInstanceProcAddr(
	        sdl_xr_instance,
	        "xrGetOpenGLGraphicsRequirementsKHR",
	        reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetRequirements));
	if (result != XR_SUCCESS || !pfnGetRequirements) {
		LOG_ERR("OpenXR: xrGetOpenGLGraphicsRequirementsKHR unavailable: %d",
		        result);
		return false;
	}

	XrGraphicsRequirementsOpenGLKHR req = {};
	req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
	result = pfnGetRequirements(sdl_xr_instance, sdl_xr_systemId, &req);
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrGetOpenGLGraphicsRequirementsKHR failed: %d",
		        result);
		return false;
	}

	LOG_MSG("OpenXR: GL requirements min %u.%u, max %u.%u",
	        XR_VERSION_MAJOR(req.minApiVersionSupported),
	        XR_VERSION_MINOR(req.minApiVersionSupported),
	        XR_VERSION_MAJOR(req.maxApiVersionSupported),
	        XR_VERSION_MINOR(req.maxApiVersionSupported));
	return true;
}

static bool openxr_create_session()
{
	const void* binding = openxr_get_graphics_binding();
	if (!binding) {
		LOG_ERR("OpenXR: Cannot create session without graphics binding");
		return false;
	}
	XrSessionCreateInfo session_info = {};
	session_info.type        = XR_TYPE_SESSION_CREATE_INFO;
	session_info.next        = binding;
	session_info.createFlags = 0;
	session_info.systemId    = sdl_xr_systemId;

	const XrResult result = xrCreateSession(sdl_xr_instance,
	                                        &session_info,
	                                        &sdl_xr_session);
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrCreateSession failed: %d", result);
		sdl_xr_session = XR_NULL_HANDLE;
		return false;
	}

	LOG_MSG("OpenXR: Session created successfully");
	sdl_xr_session_ready = false;

	// Motion-controller input is optional: the session works without it.
	if (!openxr_attach_input()) {
		LOG_MSG("OpenXR: Continuing without motion controllers");
	}
	return true;
}

static bool openxr_create_reference_space()
{
	if (sdl_xr_session == XR_NULL_HANDLE) {
		return false;
	}
	XrReferenceSpaceCreateInfo space_info = {};
	space_info.type                = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	space_info.referenceSpaceType  = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
	space_info.poseInReferenceSpace.position    = {0.0f, 0.0f, 0.0f};

	const XrResult result = xrCreateReferenceSpace(sdl_xr_session,
	                                              &space_info,
	                                              &sdl_xr_app_space);
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrCreateReferenceSpace failed: %d", result);
		return false;
	}
	return true;
}

static bool openxr_begin_session()
{
	if (sdl_xr_session == XR_NULL_HANDLE) {
		return false;
	}
	XrSessionBeginInfo begin_info = {};
	begin_info.type = XR_TYPE_SESSION_BEGIN_INFO;
	begin_info.primaryViewConfigurationType = sdl_xr_view_config_type;

	const XrResult result = xrBeginSession(sdl_xr_session, &begin_info);
	if (result != XR_SUCCESS) {
		return openxr_note_failure("xrBeginSession", result);
	}
	sdl_xr_session_ready = true;
	sdl_xr_ever_ran      = true;
	LOG_MSG("OpenXR: Session begun (view config %d)",
	        static_cast<int>(sdl_xr_view_config_type));
	return true;
}

static bool openxr_create_swapchain()
{
	if (sdl_xr_session == XR_NULL_HANDLE || sdl_xr_view_count == 0) {
		return false;
	}

	// 1. Pick a swapchain format supported by the runtime.
	uint32_t format_count = 0;
	XrResult result = xrEnumerateSwapchainFormats(sdl_xr_session,
	                                            0,
	                                            &format_count,
	                                            nullptr);
	if (result != XR_SUCCESS || format_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate swapchain formats: %d",
		        result);
		return false;
	}
	std::vector<int64_t> formats(format_count);
	result = xrEnumerateSwapchainFormats(sdl_xr_session,
	                                    format_count,
	                                    &format_count,
	                                    formats.data());
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate swapchain formats: %d",
		        result);
		return false;
	}
	// Prefer sRGB RGBA8 for direct DOS framebuffer display; fall back
	// to the runtime's first choice.
	sdl_xr_swapchain_format = formats[0];
	for (const auto fmt : formats) {
		if (fmt == 0x8C43) { // GL_SRGB8_ALPHA8
			sdl_xr_swapchain_format = fmt;
			break;
		}
	}

	// 2. Size from the view configuration (per-eye recommended size).
	// Single swapchain with arraySize == view count; eye N renders into
	// slice N.
	sdl_xr_swapchain_width  = sdl_xr_config_views[0].recommendedImageRectWidth;
	sdl_xr_swapchain_height = sdl_xr_config_views[0].recommendedImageRectHeight;

	XrSwapchainCreateInfo swapchain_info = {};
	swapchain_info.type       = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	swapchain_info.createFlags = 0;
	swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	swapchain_info.format     = sdl_xr_swapchain_format;
	swapchain_info.sampleCount = 1;
	swapchain_info.width      = sdl_xr_swapchain_width;
	swapchain_info.height     = sdl_xr_swapchain_height;
	swapchain_info.faceCount  = 1;
	swapchain_info.arraySize  = sdl_xr_view_count;
	swapchain_info.mipCount   = 1;

	result = xrCreateSwapchain(sdl_xr_session,
	                          &swapchain_info,
	                          &sdl_xr_swapchain);
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrCreateSwapchain failed: %d", result);
		sdl_xr_swapchain = XR_NULL_HANDLE;
		return false;
	}

	// 3. Enumerate the GL texture images backing the swapchain.
	uint32_t image_count = 0;
	result = xrEnumerateSwapchainImages(sdl_xr_swapchain,
	                                   0,
	                                   &image_count,
	                                   nullptr);
	if (result != XR_SUCCESS || image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate swapchain images: %d",
		        result);
		return false;
	}
	sdl_xr_swapchain_images.resize(image_count);
	for (auto& img : sdl_xr_swapchain_images) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		img.next = nullptr;
	}
	result = xrEnumerateSwapchainImages(
	        sdl_xr_swapchain,
	        image_count,
	        &image_count,
	        reinterpret_cast<XrSwapchainImageBaseHeader*>(
	                sdl_xr_swapchain_images.data()));
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate swapchain images: %d",
		        result);
		sdl_xr_swapchain_images.clear();
		return false;
	}

	LOG_MSG("OpenXR: Swapchain created: %ux%u x%u, format %lld, %u images",
	        sdl_xr_swapchain_width,
	        sdl_xr_swapchain_height,
	        sdl_xr_view_count,
	        static_cast<long long>(sdl_xr_swapchain_format),
	        image_count);
	return true;
}
static bool openxr_locate_views(XrTime display_time)
{
	if (sdl_xr_session == XR_NULL_HANDLE ||
	    sdl_xr_app_space == XR_NULL_HANDLE || sdl_xr_view_count == 0) {
		return false;
	}
	for (uint32_t i = 0; i < sdl_xr_view_count && i < 2; ++i) {
		sdl_xr_views[i].type = XR_TYPE_VIEW;
		sdl_xr_views[i].next = nullptr;
	}

	XrViewLocateInfo locate_info = {};
	locate_info.type                 = XR_TYPE_VIEW_LOCATE_INFO;
	locate_info.viewConfigurationType = sdl_xr_view_config_type;
	locate_info.displayTime          = display_time;
	locate_info.space                = sdl_xr_app_space;

	XrViewState view_state = {};
	view_state.type = XR_TYPE_VIEW_STATE;

	uint32_t view_count = 0;
	const XrResult result = xrLocateViews(sdl_xr_session,
	                                     &locate_info,
	                                     &view_state,
	                                     sdl_xr_view_count,
	                                     &view_count,
	                                     sdl_xr_views);
	if (result != XR_SUCCESS || view_count != sdl_xr_view_count) {
		// Tracking loss is routine (occluded sensors, idle HMD):
		// log once per cause, skip the frame, keep the mirror going.
		return openxr_note_failure("xrLocateViews", result);
	}

	// Refresh per-eye FOV + projections from the tracked poses.
	for (uint32_t i = 0; i < view_count && i < 2; ++i) {
		sdl_xr_eye_fov[i] = sdl_xr_views[i].fov;
	}
	openxr_update_projections();
	return true;
}

static bool openxr_acquire_swapchain_image()
{
	if (sdl_xr_swapchain == XR_NULL_HANDLE ||
	    sdl_xr_swapchain_images.empty()) {
		return false;
	}

	XrSwapchainImageAcquireInfo acquire_info = {};
	acquire_info.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
	uint32_t image_index = 0;
	XrResult result = xrAcquireSwapchainImage(sdl_xr_swapchain,
	                                         &acquire_info,
	                                         &image_index);
	if (result != XR_SUCCESS) {
		return openxr_note_failure("xrAcquireSwapchainImage", result);
	}

	// Bounded wait: an unresponsive compositor (idle HMD, dashboard
	// open) must stall the emulator thread for at most this long.
	// When every image is still owned by the compositor we skip the
	// blit; the caller submits an empty frame to keep the wait/begin
	// pairing intact.
	XrSwapchainImageWaitInfo wait_info = {};
	wait_info.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	wait_info.timeout = OpenXrSwapchainWaitTimeoutNs;
	result = xrWaitSwapchainImage(sdl_xr_swapchain, &wait_info);
	if (result == XR_TIMEOUT_EXPIRED) {
		sdl_xr_active_image = -1;
		return false;
	}
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrWaitSwapchainImage failed: %d", result);
		return false;
	}

	sdl_xr_swapchain_index = image_index;
	sdl_xr_active_image    = static_cast<int>(image_index);
	return true;
}

static bool openxr_release_swapchain_image()
{
	sdl_xr_active_image = -1;
	if (sdl_xr_swapchain == XR_NULL_HANDLE) {
		return false;
	}
	XrSwapchainImageReleaseInfo release_info = {};
	release_info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	const XrResult result = xrReleaseSwapchainImage(sdl_xr_swapchain,
	                                               &release_info);
	if (result != XR_SUCCESS) {
		return openxr_note_failure("xrReleaseSwapchainImage", result);
	}
	return true;
}

static bool openxr_submit_frame(XrTime display_time, bool should_render)
{
	if (sdl_xr_session == XR_NULL_HANDLE) {
		return false;
	}

	const XrRect2Di full_rect = {{0, 0},
	                             {static_cast<int32_t>(sdl_xr_swapchain_width),
	                              static_cast<int32_t>(sdl_xr_swapchain_height)}};

	for (uint32_t i = 0; i < sdl_xr_view_count && i < 2; ++i) {
		auto& v              = sdl_xr_proj_layer_views[i];
		v.type               = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		v.next               = nullptr;
		// A locked screen keeps its captured pose so the picture
		// stays put in the room; otherwise the layer follows the
		// headset every frame.
		const bool use_lock = openxr_use_world_lock();
		v.pose = use_lock ? sdl_xr_locked_pose[i] : sdl_xr_views[i].pose;
		v.fov  = use_lock ? sdl_xr_locked_fov[i] : sdl_xr_views[i].fov;
		v.subImage.swapchain = sdl_xr_swapchain;
		v.subImage.imageRect = full_rect;
		v.subImage.imageArrayIndex = i;
	}

	sdl_xr_proj_layer.type      = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	sdl_xr_proj_layer.next      = nullptr;
	sdl_xr_proj_layer.layerFlags = 0;
	sdl_xr_proj_layer.space     = sdl_xr_app_space;
	sdl_xr_proj_layer.viewCount = sdl_xr_view_count;
	sdl_xr_proj_layer.views     = sdl_xr_proj_layer_views;

	// Optional keyboard quad on top of the projection layer. The quad
	// covers exactly the screen-plane band the hit-testing uses, so
	// visuals and interaction stay consistent.
	const bool render_layers = (should_render && !sdl_xr_submit_blank);

	XrCompositionLayerQuad quad = {};
	bool show_keyboard          = false;
	// Live key-scale changes resize the texture: rebuild the layer so
	// the swapchain matches.
	int kbd_tex_w = 0, kbd_tex_h = 0;
	openxr_keyboard_tex_size(kbd_tex_w, kbd_tex_h);
	if (sdl_xr_kbd_swapchain != XR_NULL_HANDLE &&
	    (kbd_tex_w != sdl_xr_kbd_tex_w || kbd_tex_h != sdl_xr_kbd_tex_h)) {
		xrDestroySwapchain(sdl_xr_kbd_swapchain);
		sdl_xr_kbd_swapchain = XR_NULL_HANDLE;
		sdl_xr_kbd_images.clear();
		sdl_xr_kbd_filled      = false;
		sdl_xr_kbd_hover_built = -2;
	sdl_xr_kbd_pressed_built = -2;
		sdl_xr_kbd_opacity_built = -1.0f;
		sdl_xr_kbd_tex_w = 0;
		sdl_xr_kbd_tex_h = 0;
		++sdl_xr_kbd_tex_epoch; // new images must refresh
		LOG_MSG("OpenXR: Keyboard layer resized to %dx%d",
		        kbd_tex_w,
		        kbd_tex_h);
	}
	if (sdl_xr_kbd_swapchain == XR_NULL_HANDLE) {
		openxr_setup_keyboard_layer(); // best effort; fill follows
	}
	if (sdl_xr_keyboard_visible &&
	    sdl_xr_kbd_swapchain != XR_NULL_HANDLE &&
	    sdl.renderer != nullptr &&
	    sdl.render_backend_type == RenderBackendType::OpenGl) {
		// Rebuild the texture when the hover, pressed key, or opacity
		// changed (the renderer caches it otherwise); images refresh
		// from it below.
		const int hover   = openxr_keyboard_hover();
		const int pressed = sdl_xr_keyboard_pressed;
		const float opacity = openxr_keyboard_opacity();
		if (hover != sdl_xr_kbd_hover_built ||
		    pressed != sdl_xr_kbd_pressed_built ||
		    opacity != sdl_xr_kbd_opacity_built) {
			sdl_xr_kbd_hover_built   = hover;
			sdl_xr_kbd_pressed_built = pressed;
			sdl_xr_kbd_opacity_built = opacity;
			++sdl_xr_kbd_tex_epoch;
		}
		const GLuint kb_tex =
		        openxr_refresh_keyboard_texture(hover, pressed);
		if (kb_tex != 0) {
			sdl_xr_kbd_tex    = kb_tex;
			sdl_xr_kbd_filled = true;
		}
	}
	if (render_layers && sdl_xr_keyboard_visible &&
	    sdl_xr_kbd_swapchain != XR_NULL_HANDLE && sdl_xr_kbd_filled) {
		// Content is static: re-designate an image each frame.
		XrSwapchainImageAcquireInfo acquire = {};
		acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
		uint32_t index = 0;
		XrSwapchainImageReleaseInfo release = {};
		release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
		XrPosef anchor = {};
		float half_w = 0.0f, half_h = 0.0f, dist_m = 0.0f;
		XrSwapchainImageWaitInfo kbd_wait = {};
		kbd_wait.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
		kbd_wait.timeout = OpenXrSwapchainWaitTimeoutNs;
		if (xrAcquireSwapchainImage(sdl_xr_kbd_swapchain,
		                            &acquire,
		                            &index) == XR_SUCCESS &&
		    xrWaitSwapchainImage(sdl_xr_kbd_swapchain, &kbd_wait) ==
		            XR_SUCCESS) {
			// Fresh-by-construction: copy the current texture into
			// the designated image when stale, so every presented
			// image carries current content.
			if (sdl_xr_kbd_img_epoch.size() !=
			    sdl_xr_kbd_images.size()) {
				sdl_xr_kbd_img_epoch.assign(sdl_xr_kbd_images.size(),
				                            0);
			}
			if (index < sdl_xr_kbd_img_epoch.size() &&
			    sdl_xr_kbd_img_epoch[index] != sdl_xr_kbd_tex_epoch &&
			    sdl.renderer != nullptr &&
			    sdl.render_backend_type == RenderBackendType::OpenGl) {
				auto* gl_renderer = static_cast<OpenGlRenderer*>(
				        sdl.renderer.get());
				gl_renderer->CopyTextureToLayer(sdl_xr_kbd_tex,
				                                kbd_tex_w,
				                                kbd_tex_h,
				                                sdl_xr_kbd_images[index]
				                                        .image,
				                                0,
				                                false); // arraySize 1
				sdl_xr_kbd_img_epoch[index] = sdl_xr_kbd_tex_epoch;
			}
		if (xrReleaseSwapchainImage(sdl_xr_kbd_swapchain,
		                            &release) == XR_SUCCESS &&
		    openxr_screen_plane(anchor, half_w, half_h, dist_m)) {
			// Screen distance only sizes the plane here; the quad uses
			// its own xr_kb_distance (dist_m is this block's plane arg).
			(void)dist_m;
			const XrVec3 fwd = xr_quat_rotate(anchor.orientation,
			                                  {0.0f, 0.0f, -1.0f});
			const XrVec3 up = xr_quat_rotate(anchor.orientation,
			                                 {0.0f, 1.0f, 0.0f});
			const XrVec3 right = xr_quat_rotate(anchor.orientation,
			                                    {1.0f, 0.0f, 0.0f});
			// Band v in [0, 0.45] -> center v = 0.225, height 0.45
			// of the full plane height; epsilon toward the viewer.
			// User offsets shift the band along the plane axes
			// (percent of the half extents; see xr_keyboard_offset_*).
			const float kbd_ox =
			        std::clamp(get_xr_section()->GetInt(
			                           "xr_keyboard_offset_x") /
			                           100.0f,
			                   -1.0f,
			                   1.0f) *
			        half_w;
			const float kbd_oy =
			        std::clamp(get_xr_section()->GetInt(
			                           "xr_keyboard_offset_y") /
			                           100.0f,
			                   -1.0f,
			                   1.0f) *
			        half_h;
			quad.type           = XR_TYPE_COMPOSITION_LAYER_QUAD;
			quad.space          = sdl_xr_app_space;
			quad.eyeVisibility  = XR_EYE_VISIBILITY_BOTH;
			// Blend on the texture alpha so xr_keyboard_opacity can
			// fade the panel over the scene behind (opaque at 100%).
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			quad.subImage.swapchain = sdl_xr_kbd_swapchain;
			quad.subImage.imageRect = {
			        {0, 0},
			        {static_cast<int32_t>(kbd_tex_w),
			         static_cast<int32_t>(kbd_tex_h)}};
			quad.subImage.imageArrayIndex = 0;
			quad.pose.orientation         = anchor.orientation;
			// Own distance (xr_kb_distance), nudged off the screen
			// plane so layers never z-fight.
			const float kbd_dist =
			        openxr_layer_dist("xr_kb_distance") + 0.02f;
			quad.pose.position = {anchor.position.x +
			                              fwd.x * kbd_dist -
			                              up.x * (0.55f * half_h) +
			                              right.x * kbd_ox +
			                              up.x * kbd_oy,
			                      anchor.position.y +
			                              fwd.y * kbd_dist -
			                              up.y * (0.55f * half_h) +
			                              right.y * kbd_ox +
			                              up.y * kbd_oy,
			                      anchor.position.z +
			                              fwd.z * kbd_dist -
			                              up.z * (0.55f * half_h) +
			                              right.z * kbd_ox +
			                              up.z * kbd_oy};
			// User scales about the band centre; key scale also grows
			// the panel (texture and quad stay proportional).
			const float kbd_s =
			        std::clamp(get_xr_section()->GetInt(
			                           "xr_keyboard_scale") /
			                           100.0f,
			                   0.25f,
			                   2.0f) *
			        openxr_keyboard_key_scale();
			quad.size = {2.0f * half_w * kbd_s, 0.9f * half_h * kbd_s};
			show_keyboard = true;
			static bool kb_submit_logged = false;
			if (!kb_submit_logged) {
				kb_submit_logged = true;
				LOG_MSG("OpenXR: Keyboard quad submitting at "
				        "(%0.2f, %0.2f, %0.2f) size %.2fx%.2f",
				        quad.pose.position.x,
				        quad.pose.position.y,
				        quad.pose.position.z,
				        quad.size.width,
				        quad.size.height);
			}
		}
		}
	}

	// Notification overlay on top: prune expired messages, then show
	// the rest in a quad above the DOS picture.
	XrCompositionLayerQuad osd_quad = {};
	bool show_osd                   = false;
	if (render_layers && get_osd_section()->GetBool("osd_xr") &&
	    sdl_xr_osd_swapchain != XR_NULL_HANDLE && !sdl_xr_osd.empty()) {
		openxr_osd_prune();
		// Rebuild the texture when messages arrived, the opacity
		// changed, or the desktop composite rewrote it full-alpha;
		// images refresh from it below.
		const float osd_opacity = openxr_osd_opacity();
		if (!sdl_xr_osd.empty() &&
		    (sdl_xr_osd_dirty ||
		     osd_opacity != sdl_xr_osd_opacity_built ||
		     sdl_xr_osd_desk_touched)) {
			const GLuint osd_tex =
			        openxr_refresh_osd_texture(osd_opacity);
			if (osd_tex != 0) {
				sdl_xr_osd_tex           = osd_tex;
				sdl_xr_osd_dirty         = false;
				sdl_xr_osd_opacity_built = osd_opacity;
				sdl_xr_osd_desk_touched  = false;
				++sdl_xr_osd_tex_epoch;
			}
		}
		if (!sdl_xr_osd.empty() && !sdl_xr_osd_dirty) {
			XrSwapchainImageAcquireInfo osd_acquire = {};
			osd_acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
			uint32_t osd_index = 0;
			XrSwapchainImageWaitInfo osd_wait = {};
			osd_wait.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
			osd_wait.timeout = OpenXrSwapchainWaitTimeoutNs;
			XrSwapchainImageReleaseInfo osd_release = {};
			osd_release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
			XrPosef osd_anchor = {};
			float osd_half_w = 0.0f, osd_half_h = 0.0f, osd_dist = 0.0f;
			if (xrAcquireSwapchainImage(sdl_xr_osd_swapchain,
			                            &osd_acquire,
			                            &osd_index) == XR_SUCCESS &&
			    xrWaitSwapchainImage(sdl_xr_osd_swapchain,
			                         &osd_wait) == XR_SUCCESS) {
				// Fresh-by-construction: copy the current texture
				// into the designated image when stale.
				if (sdl_xr_osd_img_epoch.size() !=
				    sdl_xr_osd_images.size()) {
					sdl_xr_osd_img_epoch.assign(
					        sdl_xr_osd_images.size(),
					        0);
				}
				if (osd_index < sdl_xr_osd_img_epoch.size() &&
				    sdl_xr_osd_img_epoch[osd_index] !=
				            sdl_xr_osd_tex_epoch &&
				    sdl.renderer != nullptr &&
				    sdl.render_backend_type ==
				            RenderBackendType::OpenGl) {
					auto* gl_renderer =
					        static_cast<OpenGlRenderer*>(
					                sdl.renderer.get());
					gl_renderer->CopyTextureToLayer(
					        sdl_xr_osd_tex,
					        OpenXrOsdTexW,
					        OpenXrOsdTexH,
					        sdl_xr_osd_images[osd_index].image,
					        0,
					        false); // arraySize 1
					sdl_xr_osd_img_epoch[osd_index] =
					        sdl_xr_osd_tex_epoch;
				}
				if (xrReleaseSwapchainImage(sdl_xr_osd_swapchain,
				                            &osd_release) == XR_SUCCESS &&
				    openxr_screen_plane(osd_anchor,
				                        osd_half_w,
				                        osd_half_h,
				                        osd_dist)) {
				// Screen distance only sizes the plane here; the quad
				// uses its own osd_xr_distance (osd_dist is this
				// block's plane arg).
				(void)osd_dist;
				const XrVec3 osd_fwd = xr_quat_rotate(
				        osd_anchor.orientation,
				        {0.0f, 0.0f, -1.0f});
				const XrVec3 osd_up = xr_quat_rotate(
				        osd_anchor.orientation,
				        {0.0f, 1.0f, 0.0f});
				const XrVec3 osd_right = xr_quat_rotate(
				        osd_anchor.orientation,
				        {1.0f, 0.0f, 0.0f});
				// User offsets: percent of the plane half-extents
				// along its right/up axes, relative to the default
				// spot above the picture.
				const float osd_ox =
				        std::clamp(get_osd_section()->GetInt(
				                           "osd_xr_offset_x") /
				                           100.0f,
				                   -1.0f,
				                   1.0f) *
				        osd_half_w;
				const float osd_oy =
				        std::clamp(get_osd_section()->GetInt(
				                           "osd_xr_offset_y") /
				                           100.0f,
				                   -1.0f,
				                   1.0f) *
				        osd_half_h;
				osd_quad.type          = XR_TYPE_COMPOSITION_LAYER_QUAD;
				osd_quad.space         = sdl_xr_app_space;
				osd_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				// Blend on the texture alpha only when faded below
				// opaque (osd_xr_opacity), so 100% renders exactly
				// as before.
				if (openxr_osd_opacity() < 1.0f) {
					osd_quad.layerFlags =
					        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				}
				osd_quad.subImage.swapchain = sdl_xr_osd_swapchain;
				osd_quad.subImage.imageRect = {
				        {0, 0},
				        {static_cast<int32_t>(OpenXrOsdTexW),
				         static_cast<int32_t>(OpenXrOsdTexH)}};
				osd_quad.subImage.imageArrayIndex = 0;
				osd_quad.pose.orientation = osd_anchor.orientation;
				// Own distance (osd_xr_distance), nudged off the
				// screen plane so layers never z-fight.
				const float osd_layer_dist =
				        openxr_layer_dist("osd_xr_distance") + 0.02f;
				osd_quad.pose.position = {
				        osd_anchor.position.x +
				                osd_fwd.x * osd_layer_dist +
				                osd_up.x * (0.75f * osd_half_h) +
				                osd_right.x * osd_ox +
				                osd_up.x * osd_oy,
				        osd_anchor.position.y +
				                osd_fwd.y * osd_layer_dist +
				                osd_up.y * (0.75f * osd_half_h) +
				                osd_right.y * osd_ox +
				                osd_up.y * osd_oy,
				        osd_anchor.position.z +
				                osd_fwd.z * osd_layer_dist +
				                osd_up.z * (0.75f * osd_half_h) +
				                osd_right.z * osd_ox +
				                osd_up.z * osd_oy};
				const float osd_s =
				        std::clamp(get_osd_section()->GetInt(
				                           "osd_xr_scale") /
				                           100.0f,
				                   0.25f,
				                   2.0f);
				osd_quad.size = {1.2f * osd_half_w * osd_s,
				                 0.3f * osd_half_w * osd_s};
				show_osd = true;
				}
			}
		}
	}

	// VR menu on top: square panel above the keyboard band, rebuilt
	// only when its rows change (values, hover, or typed name).
	XrCompositionLayerQuad menu_quad = {};
	bool show_menu                   = false;
	if (sdl_xr_menu_swapchain == XR_NULL_HANDLE) {
		openxr_setup_menu_layer(); // best effort; fill follows
	}
	if (render_layers && sdl_xr_menu_visible &&
	    sdl_xr_menu_swapchain != XR_NULL_HANDLE &&
	    sdl.renderer != nullptr &&
	    sdl.render_backend_type == RenderBackendType::OpenGl) {
		openxr_menu_build_rows();
		const std::string menu_sig = openxr_menu_signature();
		if (menu_sig != sdl_xr_menu_built_sig) {
			const GLuint menu_tex = openxr_refresh_menu_texture();
			if (menu_tex != 0) {
				sdl_xr_menu_tex   = menu_tex;
				sdl_xr_menu_filled = true;
				sdl_xr_menu_built_sig = menu_sig;
				++sdl_xr_menu_tex_epoch;
			}
		}
	}
	if (render_layers && sdl_xr_menu_visible &&
	    sdl_xr_menu_swapchain != XR_NULL_HANDLE && sdl_xr_menu_filled) {
		XrSwapchainImageAcquireInfo menu_acquire = {};
		menu_acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
		uint32_t menu_index = 0;
		XrSwapchainImageWaitInfo menu_wait = {};
		menu_wait.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
		menu_wait.timeout = OpenXrSwapchainWaitTimeoutNs;
		XrSwapchainImageReleaseInfo menu_release = {};
		menu_release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
		XrPosef menu_anchor = {};
		float menu_half_w = 0.0f, menu_half_h = 0.0f, menu_dist = 0.0f;
		if (xrAcquireSwapchainImage(sdl_xr_menu_swapchain,
		                            &menu_acquire,
		                            &menu_index) == XR_SUCCESS &&
		    xrWaitSwapchainImage(sdl_xr_menu_swapchain,
		                         &menu_wait) == XR_SUCCESS) {
			// Fresh-by-construction: copy the current texture
			// into the designated image when stale.
			if (sdl_xr_menu_img_epoch.size() !=
			    sdl_xr_menu_images.size()) {
				sdl_xr_menu_img_epoch.assign(
				        sdl_xr_menu_images.size(),
				        0);
			}
			if (menu_index < sdl_xr_menu_img_epoch.size() &&
			    sdl_xr_menu_img_epoch[menu_index] !=
			            sdl_xr_menu_tex_epoch &&
			    sdl.renderer != nullptr &&
			    sdl.render_backend_type ==
			            RenderBackendType::OpenGl) {
				auto* gl_renderer =
				        static_cast<OpenGlRenderer*>(
				                sdl.renderer.get());
				gl_renderer->CopyTextureToLayer(
				        sdl_xr_menu_tex,
				        OpenXrMenuTexW,
				        OpenXrMenuTexH,
				        sdl_xr_menu_images[menu_index].image,
				        0,
				        false); // arraySize 1
				sdl_xr_menu_img_epoch[menu_index] =
				        sdl_xr_menu_tex_epoch;
			}
			if (xrReleaseSwapchainImage(sdl_xr_menu_swapchain,
			                            &menu_release) == XR_SUCCESS &&
			    openxr_screen_plane(menu_anchor,
			                        menu_half_w,
			                        menu_half_h,
			                        menu_dist)) {
				// Screen distance only sizes the plane here; the
				// quad uses the keyboard distance (menu_dist is
				// this block's plane arg).
				(void)menu_dist;
				const XrVec3 menu_fwd = xr_quat_rotate(
				        menu_anchor.orientation,
				        {0.0f, 0.0f, -1.0f});
				const XrVec3 menu_up = xr_quat_rotate(
				        menu_anchor.orientation,
				        {0.0f, 1.0f, 0.0f});
				menu_quad.type          = XR_TYPE_COMPOSITION_LAYER_QUAD;
				menu_quad.space         = sdl_xr_app_space;
				menu_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				menu_quad.subImage.swapchain = sdl_xr_menu_swapchain;
				menu_quad.subImage.imageRect = {
				        {0, 0},
				        {static_cast<int32_t>(OpenXrMenuTexW),
				         static_cast<int32_t>(OpenXrMenuTexH)}};
				menu_quad.subImage.imageArrayIndex = 0;
				menu_quad.pose.orientation = menu_anchor.orientation;
				// Own distance (xr_menu_distance), nudged nearer
				// than the keyboard/OSD offsets so the menu wins
				// any overlap; same layout math as the hit-test.
				float panel_size = 0.0f, panel_up = 0.0f;
				float panel_vc = 0.0f, panel_vhalf = 0.0f;
				openxr_menu_layout(menu_half_h,
				                   panel_size,
				                   panel_up,
				                   panel_vc,
				                   panel_vhalf);
				(void)panel_vc;
				(void)panel_vhalf;
				const float menu_layer_dist =
				        openxr_layer_dist("xr_menu_distance") +
				        0.03f;
				menu_quad.pose.position = {
				        menu_anchor.position.x +
				                menu_fwd.x * menu_layer_dist +
				                menu_up.x * panel_up,
				        menu_anchor.position.y +
				                menu_fwd.y * menu_layer_dist +
				                menu_up.y * panel_up,
				        menu_anchor.position.z +
				                menu_fwd.z * menu_layer_dist +
				                menu_up.z * panel_up};
				menu_quad.size = {panel_size, panel_size};
				show_menu = true;
			}
		}
	}

	// Pointer-ray ribbons, one quad per hand, submitted after every
	// other quad so the rays stay visible over the keyboard, OSD,
	// and menu. Both eyes see each ribbon in true 3D, so a single
	// shared quad per hand is stereo-correct.
	XrCompositionLayerQuad ray_quad[2] = {};
	bool show_ray[2]                   = {false, false};
	if (sdl_xr_ray_swapchain == XR_NULL_HANDLE) {
		openxr_setup_ray_layer(); // best effort; fill follows
	}
	if (get_xr_section()->GetBool("xr_ray") &&
	    (sdl_xr_mouse_mode != 0 || sdl_xr_keyboard_visible ||
	     sdl_xr_menu_visible) &&
	    sdl_xr_ray_swapchain != XR_NULL_HANDLE &&
	    sdl.renderer != nullptr &&
	    sdl.render_backend_type == RenderBackendType::OpenGl) {
		// Static gradient content: refresh the texture pointer when
		// the renderer rebuilds it, then fill stale images below.
		const GLuint ray_tex = openxr_refresh_ray_texture();
		if (ray_tex != 0 && ray_tex != sdl_xr_ray_tex) {
			sdl_xr_ray_tex = ray_tex;
			++sdl_xr_ray_tex_epoch;
		}
		if (ray_tex != 0) {
			sdl_xr_ray_filled = true;
		}
	}
	if (render_layers && sdl_xr_ray_filled &&
	    get_xr_section()->GetBool("xr_ray") &&
	    (sdl_xr_mouse_mode != 0 || sdl_xr_keyboard_visible ||
	     sdl_xr_menu_visible)) {
		for (int hand = 0; hand < 2; ++hand) {
			XrPosef ribbon = {};
			float ribbon_len = 0.0f;
			if (!openxr_ray_ribbon(hand, ribbon, ribbon_len)) {
				continue;
			}
			XrSwapchainImageAcquireInfo ray_acquire = {};
			ray_acquire.type =
			        XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
			uint32_t ray_index = 0;
			XrSwapchainImageWaitInfo ray_wait = {};
			ray_wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
			ray_wait.timeout = OpenXrSwapchainWaitTimeoutNs;
			XrSwapchainImageReleaseInfo ray_release = {};
			ray_release.type =
			        XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
			if (xrAcquireSwapchainImage(sdl_xr_ray_swapchain,
			                            &ray_acquire,
			                            &ray_index) != XR_SUCCESS ||
			    xrWaitSwapchainImage(sdl_xr_ray_swapchain,
			                         &ray_wait) != XR_SUCCESS) {
				continue;
			}
			if (sdl_xr_ray_img_epoch.size() !=
			    sdl_xr_ray_images.size()) {
				sdl_xr_ray_img_epoch.assign(
				        sdl_xr_ray_images.size(),
				        0);
			}
			if (ray_index < sdl_xr_ray_img_epoch.size() &&
			    sdl_xr_ray_img_epoch[ray_index] !=
			            sdl_xr_ray_tex_epoch &&
			    sdl.renderer != nullptr &&
			    sdl.render_backend_type ==
			            RenderBackendType::OpenGl) {
				auto* gl_renderer =
				        static_cast<OpenGlRenderer*>(
				                sdl.renderer.get());
				gl_renderer->CopyTextureToLayer(
				        sdl_xr_ray_tex,
				        OpenXrRayTexW,
				        OpenXrRayTexH,
				        sdl_xr_ray_images[ray_index].image,
				        0,
				        false); // arraySize 1
				sdl_xr_ray_img_epoch[ray_index] =
				        sdl_xr_ray_tex_epoch;
			}
			if (xrReleaseSwapchainImage(sdl_xr_ray_swapchain,
			                            &ray_release) != XR_SUCCESS) {
				continue;
			}
			ray_quad[hand].type          = XR_TYPE_COMPOSITION_LAYER_QUAD;
			ray_quad[hand].space         = sdl_xr_app_space;
			ray_quad[hand].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			ray_quad[hand].layerFlags =
			        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			ray_quad[hand].subImage.swapchain = sdl_xr_ray_swapchain;
			ray_quad[hand].subImage.imageRect = {
			        {0, 0},
			        {static_cast<int32_t>(OpenXrRayTexW),
			         static_cast<int32_t>(OpenXrRayTexH)}};
			ray_quad[hand].subImage.imageArrayIndex = 0;
			ray_quad[hand].pose = ribbon;
			ray_quad[hand].size = {ribbon_len, OpenXrRayWidthM};
			show_ray[hand] = true;
		}
	}

	const XrCompositionLayerBaseHeader* layers[6] = {};

	XrFrameEndInfo end_info = {};
	end_info.type                 = XR_TYPE_FRAME_END_INFO;
	end_info.displayTime          = display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	uint32_t layer_count          = 0;
	if (render_layers) {
		layers[layer_count++] = reinterpret_cast<
		        const XrCompositionLayerBaseHeader*>(&sdl_xr_proj_layer);
		if (show_keyboard) {
			layers[layer_count++] = reinterpret_cast<
			        const XrCompositionLayerBaseHeader*>(&quad);
		}
		if (show_osd) {
			layers[layer_count++] = reinterpret_cast<
			        const XrCompositionLayerBaseHeader*>(&osd_quad);
		}
		if (show_menu) {
			layers[layer_count++] = reinterpret_cast<
			        const XrCompositionLayerBaseHeader*>(&menu_quad);
		}
		for (int hand = 0; hand < 2; ++hand) {
			if (show_ray[hand]) {
				layers[layer_count++] = reinterpret_cast<
				        const XrCompositionLayerBaseHeader*>(
				        &ray_quad[hand]);
			}
		}
	}
	end_info.layerCount = layer_count;
	end_info.layers     = render_layers ? layers : nullptr;

	const XrResult result = xrEndFrame(sdl_xr_session, &end_info);
	if (result != XR_SUCCESS) {
		return openxr_note_failure("xrEndFrame", result);
	}
	return true;
}
// Tears down everything bound to a dead session but keeps the
// instance, so a new session can be created on retry.
static void openxr_teardown_session()
{
	if (sdl_xr_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_swapchain);
		sdl_xr_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_swapchain_images.clear();
	sdl_xr_swapchain_format = 0;
	sdl_xr_swapchain_width  = 0;
	sdl_xr_swapchain_height = 0;
	sdl_xr_active_image     = -1;

	// HUD layers (swapchains, images, flags) teardown lives in
	// gui/osd.cpp now.
	osd_teardown_layers();

	if (sdl_xr_app_space != XR_NULL_HANDLE) {
		xrDestroySpace(sdl_xr_app_space);
		sdl_xr_app_space = XR_NULL_HANDLE;
	}
	for (int hand = 0; hand < 2; ++hand) {
		if (sdl_xr_aim_space[hand] != XR_NULL_HANDLE) {
			xrDestroySpace(sdl_xr_aim_space[hand]);
			sdl_xr_aim_space[hand] = XR_NULL_HANDLE;
		}
		sdl_xr_hand[hand].pose_valid    = false;
		sdl_xr_hand[hand].aim_result    = XR_SUCCESS;
		sdl_xr_hand[hand].aim_flags     = 0;
		sdl_xr_hand[hand].click         = false;
		sdl_xr_hand[hand].click_changed = false;
		sdl_xr_hand[hand].click_ui      = false;
		sdl_xr_hand[hand].click_ui_changed = false;
		sdl_xr_hand[hand].menu          = false;
		sdl_xr_hand[hand].menu_changed  = false;
	}
	openxr_release_mouse_buttons();
	if (sdl_xr_session != XR_NULL_HANDLE) {
		xrDestroySession(sdl_xr_session);
		sdl_xr_session = XR_NULL_HANDLE;
	}
	sdl_xr_view_count      = 0;
	sdl_xr_session_ready   = false;
	sdl_xr_session_state   = XR_SESSION_STATE_UNKNOWN;
	sdl_xr_head_pose_valid = false;
	// Force re-attach (and aim-space re-creation) on the next session:
	// attaching is per-session, and the destroyed aim spaces above must
	// be recreated, otherwise input stays dead after a restart.
	sdl_xr_actions_attached = false;
	sdl_xr_has_lock        = false;
	sdl_xr_retry_at_us     = GetTicksUs() + OpenXrRetryCooldownUs;
}

// A headset pulled mid-session (or a dying runtime) must neither spam
// the log at 70 Hz nor keep calling into a dead runtime: log each
// distinct failure once, tear down what's dead, and schedule a retry
// so reconnection recovers automatically. Returns false.
static bool openxr_note_failure(const char* what, const XrResult result)
{
	if (result != sdl_xr_last_error) {
		LOG_ERR("OpenXR: %s failed: %d", what, result);
		sdl_xr_last_error = result;
	}
	if (result == XR_ERROR_SESSION_LOST) {
		openxr_teardown_session();
	} else if (result == XR_ERROR_INSTANCE_LOST ||
	           result == XR_ERROR_RUNTIME_FAILURE) {
		cleanup_openxr();
		sdl_xr_retry_at_us = GetTicksUs() + OpenXrRetryCooldownUs;
	}
	return false;
}

void init_openxr()
{
	if (sdl_xr_instance != XR_NULL_HANDLE) {
		// Already brought up; session work happens in recovery.
		return;
	}

	XrResult result;

	// OpenXR instance creation. XR_KHR_opengl_enable is required later
	// for session creation with an OpenGL binding.
	static const char* const extensions[] = {"XR_KHR_opengl_enable"};

	XrInstanceCreateInfo instance_create_info = {};
	instance_create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	instance_create_info.enabledExtensionCount = 1;
	instance_create_info.enabledExtensionNames = extensions;
	snprintf(instance_create_info.applicationInfo.applicationName,
	         sizeof(instance_create_info.applicationInfo.applicationName),
	         "DOSBox Staging");
	instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

	result = xrCreateInstance(&instance_create_info, &sdl_xr_instance);
	if (result == XR_ERROR_API_VERSION_UNSUPPORTED) {
		// Some runtimes (e.g. older Meta builds) only accept 1.0.
		// Everything we use exists in 1.0, so retry with that.
		LOG_MSG("OpenXR: Runtime rejected API 1.1, retrying with 1.0");
		instance_create_info.applicationInfo.apiVersion =
		        XR_MAKE_VERSION(1, 0, 0);
		result = xrCreateInstance(&instance_create_info, &sdl_xr_instance);
	}
	if (result != XR_SUCCESS) {
		openxr_note_failure("xrCreateInstance", result);
		return;
	}
	// Identify the runtime (Meta, SteamVR, ...): input behavior
	// differs per runtime, so this belongs in every diagnostic log.
	{
		XrInstanceProperties props = {};
		props.type = XR_TYPE_INSTANCE_PROPERTIES;
		if (xrGetInstanceProperties(sdl_xr_instance, &props) ==
		    XR_SUCCESS) {
			LOG_MSG("OpenXR: Runtime '%s' version %d.%d.%d",
			        props.runtimeName,
			        XR_VERSION_MAJOR(props.runtimeVersion),
			        XR_VERSION_MINOR(props.runtimeVersion),
			        XR_VERSION_PATCH(props.runtimeVersion));
		}
	}

	// 1. Function pointers / Instance Proc Addr verification
	PFN_xrGetInstanceProcAddr pfnGetInstanceProcAddr = nullptr;
	result = xrGetInstanceProcAddr(sdl_xr_instance,
	                              "xrGetInstanceProcAddr",
	                              (PFN_xrVoidFunction*)&pfnGetInstanceProcAddr);
	if (result != XR_SUCCESS) {
		openxr_note_failure("xrGetInstanceProcAddr", result);
		return;
	}
	if (!pfnGetInstanceProcAddr) {
		LOG_ERR("OpenXR: xrGetInstanceProcAddr returned null");
		return;
	}

	// Motion-controller input (optional; the session runs without it).
	openxr_create_input_actions();

	// 2. Device Enumeration: Get System ID for HMD form factor
	XrSystemGetInfo system_get_info = {};
	system_get_info.type       = XR_TYPE_SYSTEM_GET_INFO;
	system_get_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	result = xrGetSystem(sdl_xr_instance, &system_get_info, &sdl_xr_systemId);
	if (result != XR_SUCCESS) {
		openxr_note_failure("xrGetSystem (no headset attached?)", result);
		return;
	}

	// Get system properties
	XrSystemProperties system_properties = {};
	system_properties.type = XR_TYPE_SYSTEM_PROPERTIES;
	result = xrGetSystemProperties(sdl_xr_instance,
	                              sdl_xr_systemId,
	                              &system_properties);
	if (result == XR_SUCCESS) {
		LOG_MSG("OpenXR: Headset system name: %s (Vendor ID: %u)",
		        system_properties.systemName,
		        system_properties.vendorId);
		LOG_MSG("OpenXR: Max swapchain image width: %u, height: %u, layers: %u",
		        system_properties.graphicsProperties.maxSwapchainImageWidth,
		        system_properties.graphicsProperties.maxSwapchainImageHeight,
		        system_properties.graphicsProperties.maxLayerCount);
	}

	// 3. View configuration (resolution per eye, view count)
	if (!openxr_setup_view_configuration()) {
		return;
	}

	// 4. Graphics requirements (mandatory before session creation)
	if (!openxr_check_graphics_requirements()) {
		return;
	}

	// 5. Session creation with the SDL GL graphics binding
	if (!openxr_create_session()) {
		return;
	}

	// 6. Reference space + begin session
	if (!openxr_create_reference_space()) {
		return;
	}
	if (!openxr_begin_session()) {
		return;
	}

	// 7. Swapchain (images backing the per-eye render targets)
	if (!openxr_create_swapchain()) {
		// Don't limp on with a begun session but no render targets;
		// tear down so recovery rebuilds cleanly.
		openxr_teardown_session();
		return;
	}

	// 8. Keyboard overlay layer (optional; session runs without it).
	openxr_setup_keyboard_layer();

	// 9. Notification overlay layer (optional; session runs without it).
	openxr_setup_osd_layer();

	sdl_xr_initialized = true;
	sdl_xr_last_error  = XR_SUCCESS; // re-arm error logging
}
// Headset tracking: derive a head proxy pose from the located eye
// poses (midpoint position, averaged + normalized orientation).
// (sdl_xr_head_pose / _valid live with the state block above.)
void openxr_update_head_tracking()
{
	if (sdl_xr_view_count == 0) {
		sdl_xr_head_pose_valid = false;
		return;
	}
	const auto& p0 = sdl_xr_views[0].pose;
	if (sdl_xr_view_count == 1) {
		sdl_xr_head_pose        = p0;
		sdl_xr_head_pose_valid  = true;
		return;
	}
	const auto& p1 = sdl_xr_views[1].pose;

	sdl_xr_head_pose.position.x = (p0.position.x + p1.position.x) * 0.5f;
	sdl_xr_head_pose.position.y = (p0.position.y + p1.position.y) * 0.5f;
	sdl_xr_head_pose.position.z = (p0.position.z + p1.position.z) * 0.5f;

	auto q0 = p0.orientation;
	auto q1 = p1.orientation;
	const float dot = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z +
	                  q0.w * q1.w;
	if (dot < 0.0f) {
		q1.x = -q1.x;
		q1.y = -q1.y;
		q1.z = -q1.z;
		q1.w = -q1.w;
	}
	XrQuaternionf avg = {q0.x + q1.x, q0.y + q1.y, q0.z + q1.z, q0.w + q1.w};
	const float len = sqrtf(avg.x * avg.x + avg.y * avg.y +
	                        avg.z * avg.z + avg.w * avg.w);
	if (len > 1e-6f) {
		avg.x /= len;
		avg.y /= len;
		avg.z /= len;
		avg.w /= len;
		sdl_xr_head_pose.orientation = avg;
	} else {
		sdl_xr_head_pose.orientation = q0;
	}
	sdl_xr_head_pose_valid = true;

	static int log_counter = 0;
	if (++log_counter % 600 == 0 &&
	    get_xr_section()->GetBool("xr_log_framestats")) {
		LOG_MSG("OpenXR: Head pose pos (%0.2f, %0.2f, %0.2f)",
		        sdl_xr_head_pose.position.x,
		        sdl_xr_head_pose.position.y,
		        sdl_xr_head_pose.position.z);
	}
}

// Permanently disables XR for this run and switches back to plain
// desktop rendering (the same backend the mirror already uses, so
// nothing is recreated). Used after repeated bring-up failures that
// never produced a working session.
static void openxr_give_up_and_fallback()
{
#if C_OPENGL
	constexpr const char* fallback_output = "opengl";
#else
	constexpr const char* fallback_output = "texture";
#endif
	LOG_MSG("OpenXR: Unavailable after %d attempts, falling back to '%s' output for this run (restart to retry XR)",
	        OpenXrMaxInitAttempts,
	        fallback_output);
	set_section_property_value("sdl", "output", fallback_output);
	cleanup_openxr();
	sdl_xr_disabled = true;
}

// Periodic recovery: hot-plugs a headset connected after startup,
// and rebuilds after session/instance loss. Rate-limited, and cheap
// (two comparisons) when everything is healthy. Gives up permanently
// if bring-up never succeeds and no session ever ran.
void openxr_maybe_recover()
{
	if (sdl_xr_session_ready && sdl_xr_swapchain != XR_NULL_HANDLE) {
		return; // fully operational
	}
	const auto now_us = GetTicksUs();
	if (now_us < sdl_xr_retry_at_us) {
		return;
	}
	sdl_xr_retry_at_us = now_us + OpenXrRetryCooldownUs;

	bool ok = false;
	if (sdl_xr_instance == XR_NULL_HANDLE ||
	    sdl_xr_systemId == XR_NULL_SYSTEM_ID) {
		// Cold start or headset was absent: fresh bring-up (also
		// re-probes a headset plugged in after startup).
		if (sdl_xr_last_error == XR_SUCCESS) {
			LOG_MSG("OpenXR: Trying initialization...");
		}
		if (sdl_xr_instance != XR_NULL_HANDLE) {
			cleanup_openxr();
		}
		init_openxr();
		ok = (sdl_xr_session_ready &&
		      sdl_xr_swapchain != XR_NULL_HANDLE);
	} else {
		// Instance (and system) survived: rebuild the session stack
		// from a clean slate, including the view configuration.
		LOG_MSG("OpenXR: Re-creating session...");
		openxr_teardown_session();
		ok = (openxr_setup_view_configuration() &&
		      openxr_create_session() &&
		      openxr_create_reference_space() &&
		      openxr_begin_session() && openxr_create_swapchain());
		if (ok) {
			sdl_xr_initialized = true;
			sdl_xr_last_error  = XR_SUCCESS;
			openxr_setup_keyboard_layer();
			openxr_setup_osd_layer();
		}
	}

	if (ok) {
		sdl_xr_init_fails = 0;
	} else if (!sdl_xr_ever_ran &&
	           ++sdl_xr_init_fails >= OpenXrMaxInitAttempts) {
		openxr_give_up_and_fallback();
	}
}

static const char* openxr_session_state_name(const XrSessionState state)
{
	switch (state) {
	case XR_SESSION_STATE_UNKNOWN:      return "UNKNOWN";
	case XR_SESSION_STATE_IDLE:         return "IDLE";
	case XR_SESSION_STATE_READY:        return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
	case XR_SESSION_STATE_STOPPING:     return "STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
	case XR_SESSION_STATE_EXITING:      return "EXITING";
	default:                            return "?";
	}
}

// Session event pump: handles READY (begin), STOPPING (end),
// LOSS_PENDING / EXITING (session is dead: tear down + schedule
// retry). Call once per frame.
static void openxr_poll_events()
{
	if (sdl_xr_instance == XR_NULL_HANDLE) {
		return;
	}
	XrEventDataBuffer event = {};
	event.type              = XR_TYPE_EVENT_DATA_BUFFER;

	while (xrPollEvent(sdl_xr_instance, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto* changed =
			        reinterpret_cast<const XrEventDataSessionStateChanged*>(
			                &event);
			if (changed->session == sdl_xr_session) {
				if (changed->state != sdl_xr_session_state) {
					LOG_MSG("OpenXR: Session state %s -> %s",
					        openxr_session_state_name(
					                sdl_xr_session_state),
					        openxr_session_state_name(changed->state));
					sdl_xr_session_state = changed->state;
				}
				switch (changed->state) {
				case XR_SESSION_STATE_READY:
					if (!sdl_xr_session_ready) {
						openxr_begin_session();
					}
					break;
				case XR_SESSION_STATE_STOPPING:
					if (sdl_xr_session_ready) {
						const auto r = xrEndSession(sdl_xr_session);
						sdl_xr_session_ready = false;
						if (r != XR_SUCCESS) {
							openxr_note_failure("xrEndSession", r);
						}
					}
					break;
				case XR_SESSION_STATE_LOSS_PENDING:
				case XR_SESSION_STATE_EXITING:
					// Session handle is dead: destroy it now so we
					// stop calling into it, and retry on cooldown.
					openxr_teardown_session();
					break;
				default: break;
				}
			}
		}
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
	// XR_EVENT_UNAVAILABLE just means the queue is empty.
}

// Frame timing & sync: throttle to the headset vsync and begin the
// frame. predictedDisplayTime drives xrLocateViews + xrEndFrame.
static bool openxr_wait_begin_frame(XrFrameState& out_frame_state)
{
	if (sdl_xr_session == XR_NULL_HANDLE) {
		return false;
	}
	XrFrameWaitInfo wait_info = {};
	wait_info.type            = XR_TYPE_FRAME_WAIT_INFO;
	out_frame_state.type      = XR_TYPE_FRAME_STATE;

	XrResult result = xrWaitFrame(sdl_xr_session,
	                             &wait_info,
	                             &out_frame_state);
	if (result != XR_SUCCESS) {
		return openxr_note_failure("xrWaitFrame", result);
	}
	XrFrameBeginInfo begin_info = {};
	begin_info.type             = XR_TYPE_FRAME_BEGIN_INFO;
	result = xrBeginFrame(sdl_xr_session, &begin_info);
	if (result != XR_SUCCESS) {
		return openxr_note_failure("xrBeginFrame", result);
	}
	return true;
}

static bool openxr_end_session()
{
	if (sdl_xr_session == XR_NULL_HANDLE || !sdl_xr_session_ready) {
		return true;
	}
	const XrResult result = xrEndSession(sdl_xr_session);
	sdl_xr_session_ready  = false;
	if (result != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrEndSession failed: %d", result);
		return false;
	}
	return true;
}

// Starts an XR frame: events -> wait/begin -> locate -> acquire.
// Returns true when the caller should present the mirror (the blit
// into the swapchain happens pre-swap inside PresentFrame) and then
// call openxr_frame_end(). Submits an empty frame and returns false
// when throttled or on failure.
bool openxr_frame_begin(XrTime& display_time_out)
{
	openxr_poll_events();

	if (!sdl_xr_session_ready) {
		return false;
	}

	// Drive frames while the session can accept them. Frames (even
	// empty ones, when shouldRender is false) are how the runtime
	// promotes the session to visibility, so READY and SYNCHRONIZED
	// must still run; only dead/stopping states are skipped. This
	// keeps us out of xrWaitFrame when nothing can come of it, where
	// runtimes may stall instead of failing fast.
	switch (sdl_xr_session_state) {
	case XR_SESSION_STATE_READY:
	case XR_SESSION_STATE_SYNCHRONIZED:
	case XR_SESSION_STATE_VISIBLE:
	case XR_SESSION_STATE_FOCUSED:
		break;
	default:
		return false;
	}

	sdl_xr_frame_t0_us = GetTicksUs();

	XrFrameState frame_state = {};
	frame_state.type          = XR_TYPE_FRAME_STATE;
	const auto wait_t0_us = GetTicksUs();
	if (!openxr_wait_begin_frame(frame_state)) {
		return false;
	}
	const auto wait_us = GetTicksDiff(GetTicksUs(), wait_t0_us);
	if (wait_us > 0) {
		sdl_xr_stat_wait_us += static_cast<uint64_t>(wait_us);
		if (static_cast<uint64_t>(wait_us) > sdl_xr_stat_wait_max_us) {
			sdl_xr_stat_wait_max_us = static_cast<uint64_t>(wait_us);
		}
	}

	display_time_out = frame_state.predictedDisplayTime;

	if (!frame_state.shouldRender) {
		// Throttled / invisible: submit empty frame to keep timing.
		openxr_submit_frame(frame_state.predictedDisplayTime, false);
		return false;
	}

	const auto locate_t0_us = GetTicksUs();
	if (!openxr_locate_views(frame_state.predictedDisplayTime)) {
		openxr_submit_frame(frame_state.predictedDisplayTime, false);
		return false;
	}
	const auto locate_us = GetTicksDiff(GetTicksUs(), locate_t0_us);
	if (locate_us > 0) {
		sdl_xr_stat_locate_us += static_cast<uint64_t>(locate_us);
	}
	openxr_update_head_tracking();

	openxr_update_input(frame_state.predictedDisplayTime);
	openxr_drive_menu();
	openxr_drive_mouse();
	openxr_drive_head_mouse();
	openxr_drive_keyboard();
	openxr_drive_input_mode();
	openxr_drive_joystick();

	const auto acquire_t0_us = GetTicksUs();
	if (!openxr_acquire_swapchain_image()) {
		openxr_submit_frame(frame_state.predictedDisplayTime, false);
		return false;
	}
	const auto acquire_us = GetTicksDiff(GetTicksUs(), acquire_t0_us);
	if (acquire_us > 0) {
		sdl_xr_stat_acquire_us += static_cast<uint64_t>(acquire_us);
	}
	return true;
}

void openxr_frame_end(const XrTime display_time)
{
	const auto submit_t0_us = GetTicksUs();
	openxr_release_swapchain_image();
	openxr_submit_frame(display_time, true);
	const auto submit_us = GetTicksDiff(GetTicksUs(), submit_t0_us);
	if (submit_us > 0) {
		sdl_xr_stat_submit_us += static_cast<uint64_t>(submit_us);
	}

	const auto total_us = GetTicksDiff(GetTicksUs(), sdl_xr_frame_t0_us);
	if (total_us > 0) {
		sdl_xr_stat_total_us += static_cast<uint64_t>(total_us);
	}
	if (get_xr_section()->GetBool("xr_log_framestats") &&
	    (++sdl_xr_stat_frames % 600 == 0)) {
		const double n = static_cast<double>(sdl_xr_stat_frames);
		LOG_MSG("OpenXR: Frame stats over %llu frames (ms): wait avg %.2f "
		        "max %.2f | locate %.2f | acquire %.2f | mirror %.2f | "
		        "blit %.2f | submit %.2f | total %.2f",
		        sdl_xr_stat_frames,
		        sdl_xr_stat_wait_us / n / 1000.0,
		        sdl_xr_stat_wait_max_us / 1000.0,
		        sdl_xr_stat_locate_us / n / 1000.0,
		        sdl_xr_stat_acquire_us / n / 1000.0,
		        sdl_xr_stat_mirror_us / n / 1000.0,
		        sdl_xr_stat_blit_us / n / 1000.0,
		        sdl_xr_stat_submit_us / n / 1000.0,
		        sdl_xr_stat_total_us / n / 1000.0);
		sdl_xr_stat_frames      = 0;
		sdl_xr_stat_wait_us     = 0;
		sdl_xr_stat_total_us    = 0;
		sdl_xr_stat_wait_max_us = 0;
		sdl_xr_stat_locate_us   = 0;
		sdl_xr_stat_acquire_us  = 0;
		sdl_xr_stat_mirror_us   = 0;
		sdl_xr_stat_blit_us     = 0;
		sdl_xr_stat_submit_us   = 0;
	}
}
void cleanup_openxr()
{
	// End an active session before tearing down spaces/swapchains.
	openxr_end_session();
	if (sdl_xr_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_swapchain);
		sdl_xr_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_swapchain_images.clear();
	sdl_xr_swapchain_format = 0;
	sdl_xr_swapchain_width  = 0;
	sdl_xr_swapchain_height = 0;
	if (sdl_xr_kbd_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_kbd_swapchain);
		sdl_xr_kbd_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_kbd_images.clear();
	sdl_xr_kbd_filled = false;
	sdl_xr_kbd_hover_built = -2;
	sdl_xr_kbd_pressed_built = -2;
	sdl_xr_kbd_opacity_built = -1.0f;
	sdl_xr_kbd_tex_w = 0;
	sdl_xr_kbd_tex_h = 0;
	sdl_xr_kbd_img_epoch.clear();
	if (sdl_xr_osd_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_osd_swapchain);
		sdl_xr_osd_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_osd_images.clear();
	sdl_xr_osd_dirty = true;
	sdl_xr_osd_img_epoch.clear();
	if (sdl_xr_menu_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_menu_swapchain);
		sdl_xr_menu_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_menu_images.clear();
	sdl_xr_menu_filled = false;
	sdl_xr_menu_img_epoch.clear();
	sdl_xr_menu_built_sig.clear();
	if (sdl_xr_ray_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_ray_swapchain);
		sdl_xr_ray_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_ray_images.clear();
	sdl_xr_ray_filled = false;
	sdl_xr_ray_img_epoch.clear();
	if (sdl_xr_app_space != XR_NULL_HANDLE) {
		xrDestroySpace(sdl_xr_app_space);
		sdl_xr_app_space = XR_NULL_HANDLE;
	}
	if (sdl_xr_session != XR_NULL_HANDLE) {
		xrDestroySession(sdl_xr_session);
		sdl_xr_session = XR_NULL_HANDLE;
	}
	if (sdl_xr_aim_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_aim_action);
		sdl_xr_aim_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_click_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_click_action);
		sdl_xr_click_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_grip_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_grip_action);
		sdl_xr_grip_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_trigvalue_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_trigvalue_action);
		sdl_xr_trigvalue_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_stick_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_stick_action);
		sdl_xr_stick_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_rstick_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_rstick_action);
		sdl_xr_rstick_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_joya_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_joya_action);
		sdl_xr_joya_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_joyb_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_joyb_action);
		sdl_xr_joyb_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_joyx_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_joyx_action);
		sdl_xr_joyx_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_joyy_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_joyy_action);
		sdl_xr_joyy_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_menu_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_menu_action);
		sdl_xr_menu_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_action_set != XR_NULL_HANDLE) {
		xrDestroyActionSet(sdl_xr_action_set);
		sdl_xr_action_set = XR_NULL_HANDLE;
	}
	sdl_xr_actions_attached = false;
	if (sdl_xr_instance != XR_NULL_HANDLE) {
		xrDestroyInstance(sdl_xr_instance);
		sdl_xr_instance = XR_NULL_HANDLE;
	}
	sdl_xr_systemId      = XR_NULL_SYSTEM_ID;
	sdl_xr_view_count    = 0;
	sdl_xr_session_ready = false;
	sdl_xr_session_state = XR_SESSION_STATE_UNKNOWN;
	sdl_xr_initialized   = false;
}
void init_xr_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pint_xr = section.AddInt("xr_submit_divisor", Always, 1);
	pint_xr->SetHelp(
	        "Submit every Nth frame to the VR headset in 'openxr' output mode\n"
	        "(1 by default, meaning every frame). Higher values lower the headset\n"
	        "refresh rate but reduce overhead when the OpenXR runtime is slow to\n"
	        "accept frames. Valid range from 1 to 8.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.\n"
	        "The Ctrl+[ / Ctrl+] shortcuts adjust it live.");
	pint_xr->SetMinMax(1, 8);

	auto pstring_xr = section.AddString("xr_stereo_input", Always, "mono");
	pstring_xr->SetHelp(
	        "Stereo input mode for 'openxr' output mode ('mono' by default).\n"
	        "Converts 3D content produced by the emulated machine into separate\n"
	        "left/right headset views. Possible values:\n"
	        "\n"
	        "  mono:         Same picture in both eyes (default).\n"
	        "\n"
	        "  anaglyph:     Decode red/blue (red/cyan) anaglyph frames: the red\n"
	        "                channel drives the left eye, green+blue drive the\n"
	        "                right eye (as grayscale, for viewing without\n"
	        "                anaglyph glasses).\n"
	        "\n"
	        "  alternating:  De-interleave frame-sequential (shutter glasses)\n"
	        "                content: even frames drive one eye, odd frames the\n"
	        "                other, each eye holding its last frame in between.\n"
	        "                Always submits every frame (ignores\n"
	        "                'xr_submit_divisor', which would freeze one eye).\n"
	        "\n"
	        "  cybershades:  Frame-sequential decode for Chinon CyberShades\n"
	        "                titles (e.g. TEST.EXE, Descent): like 'alternating'\n"
	        "                (one frame per eye, other eye holds its last\n"
	        "                frame), but with CyberShades shutter polarity -\n"
	        "                LPT data bit 2 set reads as the LEFT eye - and\n"
	        "                COM-port MCR sync in addition to LPT. Always\n"
	        "                submits every frame (ignores 'xr_submit_divisor').\n"
	        "\n"
	        "  interleaved:  Row-interleaved content (e.g. 320x400 with even\n"
	        "                rows for one eye and odd rows for the other).\n"
	        "                Even rows drive the first eye, odd rows the second.\n"
	        "\n"
	        "  top-bottom:   Over-under content (e.g. 320x400 with two stacked\n"
	        "                320x200 views). Top half drives the first eye,\n"
	        "                bottom half the second.\n"
	        "\n"
	        "  sbs:          Side-by-side content. Left half drives the first\n"
	        "                eye, right half the second.\n"
	        "\n"
	        "  depth:        Synthesize stereo from the 3dfx Voodoo depth\n"
	        "                buffer (for Glide games): near pixels shift apart\n"
	        "                per eye. Falls back to mono on non-Voodoo content.\n"
	        "\n"
	        "  autodepth:    Synthesize stereo from estimated 2D depth (works\n"
	        "                on any content): near/far is guessed from the\n"
	        "                frame itself (ground-plane gradient plus\n"
	        "                brightness, see 'xr_autodepth_method'), then\n"
	        "                shifted per eye like 'depth'. Flat frames read\n"
	        "                as mono.\n"
	        "\n"
	        "  cybermaxx:   Field-sequential decode for testing: the\n"
	        "                signal carries alternating temporal fields in\n"
	        "                even/odd rows, so each eye gets a\n"
	        "                bob-deinterlaced half from a single field.\n"
	        "                Static content coincides in both eyes; motion\n"
	        "                shows the field offset as the shutters would.\n"
	        "\n"
	        "Note: 'first eye' is left unless 'xr_eye_swap' is on. Can be\n"
	        "changed while running; takes effect on the next frame.\n"
	        "The Ctrl+E / Ctrl+T shortcuts cycle modes / toggle mono live.");
	pstring_xr->SetValues({"mono",
	                       "anaglyph",
	                       "alternating",
	                       "cybershades",
	                       "interleaved",
	                       "top-bottom",
	                       "sbs",
	                       "depth",
	                       "autodepth",
	                       "cybermaxx"});

	auto pstring_xr_auto = section.AddString("xr_autodepth_method",
	                                         Always,
	                                         "blend");
	pstring_xr_auto->SetHelp(
	        "Depth cues for the 'autodepth' stereo input mode ('blend' by\n"
	        "default). Possible values:\n"
	        "\n"
	        "  blend:     Screen-bottom-near gradient plus brightness.\n"
	        "\n"
	        "  gradient:  Screen bottom reads as near, top as far (best\n"
	        "             for ground-plane games: racers, flight sims).\n"
	        "\n"
	        "  luma:      Bright reads as near, dark as far (best for\n"
	        "             dark-background games: space sims, shooters).\n"
	        "\n"
#if C_NEURAL
	        "  neural:    MiDaS small neural depth via a downloaded ONNX\n"
	        "             Runtime + model (see XRDEPTH below); falls back to\n"
	        "             'blend' until the first map arrives.\n"
	        "\n"
	        "  neural+gradient / neural+luma / neural+blend:\n"
	        "             Blend the neural map 50/50 with that heuristic\n"
	        "             cue; degrade to the cue alone until maps arrive.\n"
	        "\n"
#endif
	        "Note: Can be changed while running; takes effect on the next frame.\n"
	        "The Ctrl+A shortcut cycles the methods live.");
	pstring_xr_auto->SetValues({"blend",
	                             "gradient",
	                             "luma"
#if C_NEURAL
	                             ,
	                             "neural",
	                             "neural+gradient",
	                             "neural+luma",
	                             "neural+blend"
#endif
	                             });

#if C_NEURAL
	auto pint_xr_neural = section.AddInt("xr_neural_interval_ms",
	                                     Always,
	                                     750);
	pint_xr_neural->SetHelp(
	        "Neural depth refresh interval in milliseconds ('750' by\n"
	        "default): how often a new frame is sent to the background\n"
	        "inference worker. Lower values track scene changes faster at\n"
	        "higher CPU cost. Valid range from 16 to 5000 (values below\n"
	        "a model's inference time just queue every frame).\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pint_xr_neural->SetMinMax(16, 5000);

	auto pstring_xr_neural_model = section.AddString("xr_neural_model",
	                                                 Always,
	                                                 "midas");
	pstring_xr_neural_model->SetHelp(
	        "Default neural depth model loaded for the 'neural'\n"
	        "autodepth submode ('midas' by default). Possible values:\n"
	        "\n"
	        "  midas:      MiDaS v2.1 small, fast general depth.\n"
	        "\n"
	        "  da2int8:    Depth Anything V2 small, INT8 quantized\n"
	        "              (slower, sharper boundaries).\n"
	        "\n"
	        "  fastdepth:  FastDepth 224, tiny and quickest (NYU-indoor\n"
	        "              biased).\n"
	        "\n"
	        "  lightdepth: ResNet18 U-Net (NYU meters). No hosted weights\n"
	        "              file exists; convert the PyTorch checkpoint\n"
	        "              locally and place it as lightdepth.onnx.\n"
	        "\n"
	        "Only downloaded models can load (see XRDEPTH); the Ctrl+N\n"
	        "shortcut cycles those live. Applies on the next neural load.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pstring_xr_neural_model->SetValues({"midas",
	                                     "da2int8",
	                                     "fastdepth",
	                                     "lightdepth"});

	auto pint_xr_threads = section.AddInt("xr_neural_threads",
	                                      Always,
	                                      0);
	pint_xr_threads->SetHelp(
	        "ONNX Runtime inference threads for neural depth ('0' by\n"
	        "default, meaning automatic: up to 4). Fewer threads lower\n"
	        "total CPU use and leave the emulator's cores alone at the\n"
	        "cost of slower single inferences; more threads help the\n"
	        "large models (da2int8 at 518) on many-core machines.\n"
	        "Valid range from 0 to 16.\n"
	        "\n"
	        "Note: Takes effect on restart (session options apply at\n"
	        "runtime startup).");
	pint_xr_threads->SetMinMax(0, 16);
#endif // C_NEURAL

	auto pbool_xr_autoinv = section.AddBool("xr_autodepth_invert",
	                                        Always,
	                                        false);
	pbool_xr_autoinv->SetHelp(
	        "Invert the estimated depth for the 'autodepth' stereo input\n"
	        "mode ('off' by default). Use this if near and far feel\n"
	        "swapped.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pint_xr_depth = section.AddInt("xr_depth_strength", Always, 10);
	pint_xr_depth->SetHelp(
	        "Stereo separation for the 'depth' and 'autodepth' stereo\n"
	        "input modes as a percentage of the image width (10 by\n"
	        "default): how far the nearest pixels shift apart between the\n"
	        "eyes. Higher values give stronger 3D at the cost of more\n"
	        "stretching at depth edges. Valid range from 0 (flat) to 50.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.\n"
	        "The Ctrl+; / Ctrl+' shortcuts adjust it live.");
	pint_xr_depth->SetMinMax(0, 50);

	auto pbool_xr_flip = section.AddBool("xr_flip_vertical", Always, false);
	pbool_xr_flip->SetHelp(
	        "Flip the depth-synthesized image vertically ('off' by default).\n"
	        "Corrects titles whose depth buffer runs bottom-up. Applies to\n"
	        "the 'depth' stereo input mode only.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pbool_xr_showdepth = section.AddBool("xr_show_depth", Always, false);
	pbool_xr_showdepth->SetHelp(
	        "Show the normalized depth as grayscale instead of the\n"
	        "game image ('off' by default, for debugging the 'depth' and\n"
	        "'autodepth' stereo input modes). Near is bright, far is dark,\n"
	        "flat depth is mid-gray, absent depth is black.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pbool_xr = section.AddBool("xr_eye_swap", Always, false);
	pbool_xr->SetHelp(
	        "Swap left/right eyes for the 'anaglyph', 'alternating' and\n"
	        "                'cybershades' stereo input modes ('off' by\n"
	        "                default). Use this if depth looks inverted.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pint_xr_disp = section.AddInt("xr_disparity", Always, 0);
	pint_xr_disp->SetHelp(
	        "Stereo convergence offset in pixels for 'openxr' output mode\n"
	        "(0 by default). Shifts the left and right eye pictures in opposite\n"
	        "directions to calibrate the perceived depth plane: positive values\n"
	        "converge the images (screen feels closer), negative values diverge\n"
	        "them. Applies to all stereo modes, including mono. Valid range\n"
	        "from -128 to 128.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_disp->SetMinMax(-128, 128);

	auto pstring_xr_method = section.AddString("xr_anaglyph_method",
	                                          Always,
	                                          "simple");
	pstring_xr_method->SetHelp(
	        "Anaglyph decoding method for the 'anaglyph' stereo input mode\n"
	        "('simple' by default). Possible values:\n"
	        "\n"
	        "  simple:  Red channel drives one eye, green+blue average drives\n"
	        "           the other (default). Exact for simply-encoded content,\n"
	        "           cheapest to compute.\n"
	        "\n"
	        "  dubois:  Least-squares decode using the Dubois (2001) red/cyan\n"
	        "           projection matrices. Better color separation on\n"
	        "           Dubois-encoded content, slightly higher CPU cost.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pstring_xr_method->SetValues({"simple", "dubois"});

	auto pstring_xr_shutter = section.AddString("xr_shutter_sync",
	                                            Always,
	                                            "auto");
	pstring_xr_shutter->SetHelp(
	        "Eye source for the 'alternating' and 'cybershades' stereo input\n"
	        "modes ('auto' by default). Possible values:\n"
	        "\n"
	        "  auto:    Follow the game's shutter port when it wrote one for\n"
	        "           the current frame (LPT1 data bit 2; 'cybershades'\n"
	        "           also watches LPT2/3 and the COM1-4 modem control\n"
	        "           registers), otherwise fall back to frame parity\n"
	        "           (default).\n"
	        "\n"
	        "  parity:  Always use blind frame parity, ignoring port writes.\n"
	        "\n"
	        "  lpt:     Always use the latest shutter value, even if stale.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pstring_xr_shutter->SetValues({"auto", "parity", "lpt"});

	auto pint_xr_dist = section.AddInt("xr_screen_distance", Always, 200);
	pint_xr_dist->SetHelp(
	        "Distance of the virtual screen in centimeters for motion-\n"
	        "controller pointing in 'openxr' output mode (200 by default).\n"
	        "Defines the plane that controller aim rays are intersected with\n"
	        "for the emulated mouse and overlay keyboard. Valid range from\n"
	        "50 to 500.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_dist->SetMinMax(50, 500);

	auto pbool_xr_lock = section.AddBool("xr_headset_picture_lock",
	                                     Always,
	                                     true);
	pbool_xr_lock->SetHelp(
	        "Glue the DOS picture to the headset ('on' by default). When\n"
	        "off, the picture is placed in 3D space once a lock pose exists\n"
	        "(use recenter or the position lock to place it); looking away\n"
	        "then leaves the picture behind.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pint_xr_kbdist = section.AddInt("xr_kb_distance", Always, 200);
	pint_xr_kbdist->SetHelp(
	        "Distance of the overlay keyboard in centimeters ('200' by\n"
	        "default). Independent from the screen distance. Valid range\n"
	        "from 50 to 500.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_kbdist->SetMinMax(50, 500);

	auto pint_xr_pic = section.AddInt("xr_picture_size", Always, 40);
	pint_xr_pic->SetHelp(
	        "Size of the DOS picture in the VR headset as a percentage of the\n"
	        "eye texture ('40' by default, meaning centered at 40% with a black\n"
	        "border). Valid range from 20 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.\n"
	        "The Ctrl+= / Ctrl+- shortcuts adjust it live.");
	pint_xr_pic->SetMinMax(20, 100);

	auto pbool_xr_log_lpt = section.AddBool("xr_log_lpt", Always, true);
	pbool_xr_log_lpt->SetHelp(
	        "Log LPT (parallel port) traffic for stereo/shutter-glasses\n"
	        "detection in 'openxr' output mode ('on' by default). Only logs\n"
	        "when an accessed value changes, so idle polling stays silent.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.\n"
	        "Turn off while LPT audio devices stream to avoid floods.");

	auto pbool_xr_log_com = section.AddBool("xr_log_com", Always, true);
	pbool_xr_log_com->SetHelp(
	        "Log COM (serial port) traffic for stereo/shutter-glasses\n"
	        "detection in 'openxr' output mode ('on' by default). Only logs\n"
	        "when an accessed value changes, so idle polling stays silent.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto pbool_xr_log_vip = section.AddBool("xr_log_vip", Always, true);
	pbool_xr_log_vip->SetHelp(
	        "Log VFX-1 VIP-card detection failures ('on' by default):\n"
	        "responder-off touches, echo/HWID/CFGSTATUS rejects, IRQCFG\n"
	        "readback divergence, status-waiter spins, FIFO-dry reads,\n"
	        "unserviced IRQs and VIP-IRQ masking on ports 260h, 280h,\n"
	        "2A0h and 2C0h. Healthy traffic stays silent.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto pbool_xr_log_vip_full = section.AddBool("xr_log_vip_full",
	                                             Always,
	                                             false);
	pbool_xr_log_vip_full->SetHelp(
	        "Log every VFX-1 VIP port access with CS:IP and no dedup\n"
	        "('off' by default). Identical repeats still flip emulated\n"
	        "device state, so use this when the failure-point log\n"
	        "contradicts itself. Very verbose: read dosbox-xr.log, not\n"
	        "the window.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto pbool_xr_vip = section.AddBool("xr_vip_respond", Always, false);
	pbool_xr_vip->SetHelp(
	        "Answer VFX-1 VIP-card detection ('off' by default): reads from\n"
	        "an unclaimed VIP base port return the last value written there\n"
	        "(echo latch), and the ID registers answer HWID 0Dh with\n"
	        "non-FF hardware versions, so card-detection routines like\n"
	        "SETVFX1's baseport test can succeed. Claimed ports keep their\n"
	        "real handlers; nothing else is emulated.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

	auto pint_xr_vip_irq = section.AddInt("xr_vip_irq", Always, 7);
	pint_xr_vip_irq->SetHelp(
	        "IRQ line the emulated VFX-1 VIP card raises on ('7' by\n"
	        "default, matching the usual VIPPORT jumper). Must match the\n"
	        "IRQ in the guest VIPPORT setting. The card raises it when\n"
	        "command traffic completes; the guest EOIs lower it again.\n"
	        "Only used while xr_vip_respond is on. Valid range from 0\n"
	        "to 15.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pint_xr_vip_irq->SetMinMax(0, 15);

	auto pint_xr_kbd_scale = section.AddInt("xr_keyboard_scale", Always, 100);
	pint_xr_kbd_scale->SetHelp(
	        "Size of the in-headset overlay keyboard as a percentage of its\n"
	        "default size ('100' by default). The keyboard is scaled about\n"
	        "the centre of the lower screen band; pointing and key presses\n"
	        "follow the scaled keys. Valid range from 25 to 200.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_kbd_scale->SetMinMax(25, 200);

	auto pint_xr_key_scale = section.AddInt("xr_keyboard_key_scale",
	                                        Always,
	                                        100);
	pint_xr_key_scale->SetHelp(
	        "Size of the overlay keyboard keys as a percentage of their\n"
	        "default size ('100' by default). Both the key texture and the\n"
	        "keyboard panel grow with it, so keys (and their letters) get\n"
	        "bigger while staying crisp. Combines with xr_keyboard_scale.\n"
	        "Valid range from 50 to 200.\n"
	        "\n"
	        "Note: Can be changed while running; the keyboard layer is\n"
	        "rebuilt on the next frame.");
	pint_xr_key_scale->SetMinMax(50, 200);

	auto pint_xr_kbd_x = section.AddInt("xr_keyboard_offset_x", Always, 0);
	pint_xr_kbd_x->SetHelp(
	        "Horizontal position offset of the overlay keyboard as a\n"
	        "percentage of the screen half-width ('0' by default).\n"
	        "Positive moves right, negative moves left. Pointing and key\n"
	        "presses follow the keyboard. Valid range from -100 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_kbd_x->SetMinMax(-100, 100);

	auto pint_xr_kbd_y = section.AddInt("xr_keyboard_offset_y", Always, 20);
	pint_xr_kbd_y->SetHelp(
	        "Vertical position offset of the overlay keyboard as a\n"
	        "percentage of the screen half-height ('20' by default, lifting\n"
	        "the bottom row into view). Positive moves up, negative moves\n"
	        "down. Pointing and key presses follow the keyboard. Valid\n"
	        "range from -100 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_kbd_y->SetMinMax(-100, 100);

	auto pint_xr_kbd_opacity = section.AddInt("xr_keyboard_opacity",
	                                         Always,
	                                         100);
	pint_xr_kbd_opacity->SetHelp(
	        "Opacity of the overlay keyboard as a percentage ('100' by\n"
	        "default, meaning opaque). Lower values fade the panel over\n"
	        "the menu or DOS picture behind it, so you can see through\n"
	        "while typing. Pointing and key presses are unaffected.\n"
	        "Valid range from 10 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_kbd_opacity->SetMinMax(10, 100);

	auto pint_xr_menu_scale = section.AddInt("xr_menu_scale", Always, 100);
	pint_xr_menu_scale->SetHelp(
	        "Size of the in-headset VR menu as a percentage of its\n"
	        "default size ('100' by default). The menu is scaled about\n"
	        "its centre above the DOS picture; pointing and trigger\n"
	        "clicks follow the scaled rows. Valid range from 25 to 200.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_menu_scale->SetMinMax(25, 200);

	auto pint_xr_menu_dist = section.AddInt("xr_menu_distance", Always, 200);
	pint_xr_menu_dist->SetHelp(
	        "Distance of the in-headset VR menu in centimeters ('200' by\n"
	        "default). Independent from the screen distance. Valid range\n"
	        "from 50 to 500.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_menu_dist->SetMinMax(50, 500);

	auto pint_xr_menu_y = section.AddInt("xr_menu_offset_y", Always, 0);
	pint_xr_menu_y->SetHelp(
	        "Vertical position offset of the in-headset VR menu as a\n"
	        "percentage of the screen half-height ('0' by default,\n"
	        "meaning above the overlay keyboard). Positive moves up,\n"
	        "negative moves down. Pointing and trigger clicks follow\n"
	        "the menu. Valid range from -100 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_menu_y->SetMinMax(-100, 100);

	auto pint_xr_menu_font = section.AddInt("xr_menu_font_size",
	                                       Always,
	                                       100);
	pint_xr_menu_font->SetHelp(
	        "Font size of the in-headset VR menu text as a percentage\n"
	        "of the row-fitted size ('100' by default). Larger text may\n"
	        "clip at the panel edges on long rows. Valid range from 50\n"
	        "to 200.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_menu_font->SetMinMax(50, 200);

	auto pbool_xr_ray = section.AddBool("xr_ray", Always, true);
	pbool_xr_ray->SetHelp(
	        "Draw a pointer ray from each controller to the screen\n"
	        "('on' by default). Visible while controller mouse control,\n"
	        "the overlay keyboard, or the VR menu is active. The rays are\n"
	        "submitted after every other overlay, so they stay visible\n"
	        "over the keyboard and menu.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pint_xr_trig = section.AddInt("xr_trigger_threshold", Always, 30);
	pint_xr_trig->SetHelp(
	        "How far the controller trigger must be pulled (percent) before\n"
	        "it counts as pressed ('30' by default): low enough that quick\n"
	        "taps register, with a small release hysteresis so hovering at\n"
	        "the edge cannot flutter. Higher values make the trigger less\n"
	        "sensitive (0 keeps the runtime default, which fires on mere\n"
	        "touch on some controllers). WMR motion controllers have no\n"
	        "boolean trigger input, so keep this above 0 for their trigger\n"
	        "pulls to count as clicks. Valid range from 0 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pint_xr_trig->SetMinMax(0, 100);

	auto pint_xr_hold = section.AddInt("xr_mouse_min_hold_ms",
	                                  Always,
	                                  25);
	pint_xr_hold->SetHelp(
	        "Minimum emulated mouse press length in milliseconds ('25'\n"
	        "by default). Releases arriving sooner are held until the\n"
	        "time is up, so quick trigger taps survive slow guest\n"
	        "polling instead of vanishing. Lower is snappier, higher\n"
	        "is safer; 0 releases immediately. Valid range from 0\n"
	        "to 200.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pint_xr_hold->SetMinMax(0, 200);

	auto pint_xr_uitrig = section.AddInt("xr_ui_trigger_threshold",
	                                    Always,
	                                    80);
	pint_xr_uitrig->SetHelp(
	        "How far the controller trigger must be pulled (percent) before\n"
	        "a VR menu or overlay-keyboard press counts ('80' by default).\n"
	        "Firmer than xr_trigger_threshold so resting a finger on the\n"
	        "trigger does not spray UI clicks; the DOS mouse and grip\n"
	        "chords keep the main threshold. Controllers without an\n"
	        "analog trigger fall back to the boolean click. Valid range\n"
	        "from 10 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pint_xr_uitrig->SetMinMax(10, 100);

	auto pint_xr_csense_x = section.AddInt("xr_controller_mouse_sense_x",
	                                      Always,
	                                      100);
	pint_xr_csense_x->SetHelp(
	        "Horizontal sensitivity of the controller-aimed mouse as a\n"
	        "percentage ('100' by default, meaning raw motion). Scales\n"
	        "relative movement only, so screen edges stay reachable.\n"
	        "Valid range from 10 to 400.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_csense_x->SetMinMax(10, 400);

	auto pint_xr_csense_y = section.AddInt("xr_controller_mouse_sense_y",
	                                      Always,
	                                      100);
	pint_xr_csense_y->SetHelp(
	        "Vertical sensitivity of the controller-aimed mouse as a\n"
	        "percentage ('100' by default). See xr_controller_mouse_sense_x.\n"
	        "Valid range from 10 to 400.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_csense_y->SetMinMax(10, 400);

	auto pint_xr_hsense_x = section.AddInt("xr_head_mouse_sense_x",
	                                      Always,
	                                      100);
	pint_xr_hsense_x->SetHelp(
	        "Horizontal sensitivity of the head-aimed mouse as a\n"
	        "percentage ('100' by default, meaning raw motion). Scales\n"
	        "relative movement only, so screen edges stay reachable.\n"
	        "Valid range from 10 to 400.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_hsense_x->SetMinMax(10, 400);

	auto pint_xr_hsense_y = section.AddInt("xr_head_mouse_sense_y",
	                                      Always,
	                                      100);
	pint_xr_hsense_y->SetHelp(
	        "Vertical sensitivity of the head-aimed mouse as a\n"
	        "percentage ('100' by default). See xr_head_mouse_sense_x.\n"
	        "Valid range from 10 to 400.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_hsense_y->SetMinMax(10, 400);

	auto pbool_xr_log_stats = section.AddBool("xr_log_framestats",
	                                          Always,
	                                          false);
	pbool_xr_log_stats->SetHelp(
	        "Log OpenXR per-frame timing statistics and head-pose updates\n"
	        "('off' by default). Useful when diagnosing headset performance.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
}
#endif // C_OPENXR
