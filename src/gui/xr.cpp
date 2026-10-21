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
#include "gui/titlebar.h"
#include "gui/xr.h"
#include "gui/xrstereo.h"
#include "gui/xrvulkan.h"
#include "gui/xrd3d11.h"
#include "gui/xrd3d12.h"
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
std::vector<XrGfxImage> sdl_xr_swapchain_images;

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

// In-headset mapper panel: a dedicated single-slice swapchain fed from
// the mapper's SDL_RenderReadPixels capture while the mapper UI owns
// the window (the normal frame loop is paused).
static XrSwapchain             sdl_xr_mapper_swapchain = XR_NULL_HANDLE;
static std::vector<XrGfxImage> sdl_xr_mapper_images;
static int                     sdl_xr_mapper_tex_w = 0;
static int                     sdl_xr_mapper_tex_h = 0;
static std::vector<uint32_t>   sdl_xr_mapper_cpu;
static int                     sdl_xr_mapper_cpu_w = 0;
static int                     sdl_xr_mapper_cpu_h = 0;
static bool                    sdl_xr_mapper_filled = false;
static bool                    sdl_xr_mapper_active = false;
static uint64_t                sdl_xr_mapper_epoch = 1;
static std::vector<uint64_t>   sdl_xr_mapper_img_epoch;
static void*                   sdl_xr_mapper_game_gl_context = nullptr;
static void*                   sdl_xr_mapper_ui_gl_context   = nullptr;

// Mapper panel placement, shared by the layer submit and the ray-based
// pointer so aiming hits exactly what the panel draws.
struct XrMapperQuad {
	XrPosef pose = {};
	XrVec3  fwd = {}, right = {}, up = {};
	float   w = 0.0f, h = 0.0f;
	int     tex_w = 0, tex_h = 0;
};

static bool openxr_mapper_quad_geometry(XrMapperQuad& quad)
{
	quad.tex_w = sdl_xr_mapper_cpu_w;
	quad.tex_h = sdl_xr_mapper_cpu_h;
	if (quad.tex_w <= 0 || quad.tex_h <= 0) {
		return false;
	}
	XrPosef anchor = {};
	float half_w = 0.0f, half_h = 0.0f, dist_m = 0.0f;
	if (!openxr_screen_plane(anchor, half_w, half_h, dist_m)) {
		return false;
	}
	quad.fwd = xr_quat_rotate(anchor.orientation, {0.0f, 0.0f, -1.0f});
	quad.right = xr_quat_rotate(anchor.orientation, {1.0f, 0.0f, 0.0f});
	quad.up = xr_quat_rotate(anchor.orientation, {0.0f, 1.0f, 0.0f});
	const float aspect = static_cast<float>(quad.tex_w) / quad.tex_h;
	const float mapper_scale = std::clamp(
	        get_xr_section()->GetInt("xr_mapper_scale") / 100.0f,
	        0.20f,
	        1.00f);
	quad.w = 2.0f * half_w * mapper_scale;
	quad.h = quad.w / aspect;
	if (quad.h > 2.0f * half_h * mapper_scale) {
		quad.h = 2.0f * half_h * mapper_scale;
		quad.w = quad.h * aspect;
	}
	quad.pose.orientation = anchor.orientation;
	quad.pose.position    = {anchor.position.x + quad.fwd.x * dist_m,
	                        anchor.position.y + quad.fwd.y * dist_m,
	                        anchor.position.z + quad.fwd.z * dist_m};
	return (quad.w > 0.0f && quad.h > 0.0f);
}

static bool openxr_mapper_ray_uv(const int hand, XrMapperQuad& quad,
                                 float& u_out, float& v_out)
{
	if (hand < 0 || hand > 1 || !sdl_xr_hand[hand].pose_valid) {
		return false;
	}
	if (!openxr_mapper_quad_geometry(quad)) {
		return false;
	}
	const auto& hp = sdl_xr_hand[hand].pose;
	const XrVec3 origin = {hp.position.x, hp.position.y, hp.position.z};
	const XrVec3 dir = xr_quat_rotate(hp.orientation, {0.0f, 0.0f, -1.0f});
	const XrVec3 center = {quad.pose.position.x,
	                       quad.pose.position.y,
	                       quad.pose.position.z};
	XrVec3 from = {}, ray_dir = {}, hit = {};
	float u = 0.0f, v = 0.0f;
	if (!openxr_ray_plane_hit(origin,
	                          dir,
	                          center,
	                          quad.fwd,
	                          quad.right,
	                          quad.up,
	                          quad.w * 0.5f,
	                          quad.h * 0.5f,
	                          from,
	                          ray_dir,
	                          hit,
	                          u,
	                          v)) {
		return false;
	}
	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
		return false;
	}
	u_out = u;
	v_out = v;
	return true;
}

static bool sdl_xr_mapper_mouse_down  = false;
static bool sdl_xr_mapper_mouse_valid = false;
static int  sdl_xr_mapper_mouse_x     = 0;
static int  sdl_xr_mapper_mouse_y     = 0;

// Feed the mapper's SDL mouse handling from the motion controllers:
// the aim ray moves the pointer over the mapper panel, and the trigger
// is the left button. Real mouse input keeps working alongside it.
static void openxr_mapper_drive_pointer()
{
	if (!sdl_xr_mapper_active || !sdl_xr_session_ready) {
		return;
	}
	SDL_Window* window = GFX_GetWindow();
	if (window == nullptr) {
		return;
	}
	int win_w = 0, win_h = 0;
	if (!SDL_GetWindowSize(window, &win_w, &win_h) || win_w <= 0 ||
	    win_h <= 0) {
		return;
	}
	const SDL_WindowID win_id = SDL_GetWindowID(window);

	XrMapperQuad quad = {};
	int hand = -1;
	float u = 0.0f, v = 0.0f;
	// Prefer a hand that is clicking (so a press lands where it aims),
	// otherwise follow the right hand.
	for (int h = 1; h >= 0 && hand < 0; --h) {
		if (!sdl_xr_hand[h].click) {
			continue;
		}
		float hu = 0.0f, hv = 0.0f;
		if (openxr_mapper_ray_uv(h, quad, hu, hv)) {
			hand = h;
			u    = hu;
			v    = hv;
		}
	}
	for (int h = 1; h >= 0 && hand < 0; --h) {
		float hu = 0.0f, hv = 0.0f;
		if (openxr_mapper_ray_uv(h, quad, hu, hv)) {
			hand = h;
			u    = hu;
			v    = hv;
		}
	}
	if (hand >= 0) {
		const int x = std::clamp(static_cast<int>(std::lround(u * win_w)),
		                         0,
		                         win_w - 1);
		const int y = std::clamp(
		        static_cast<int>(std::lround((1.0f - v) * win_h)),
		        0,
		        win_h - 1);
		if (!sdl_xr_mapper_mouse_valid || x != sdl_xr_mapper_mouse_x ||
		    y != sdl_xr_mapper_mouse_y) {
			SDL_Event event = {};
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = win_id;
			event.motion.which    = 0;
			event.motion.state =
			        sdl_xr_mapper_mouse_down ? SDL_BUTTON_LMASK : 0;
			event.motion.x = static_cast<float>(x);
			event.motion.y = static_cast<float>(y);
			if (sdl_xr_mapper_mouse_valid) {
				event.motion.xrel = static_cast<float>(
				        x - sdl_xr_mapper_mouse_x);
				event.motion.yrel = static_cast<float>(
				        y - sdl_xr_mapper_mouse_y);
			}
			sdl_xr_mapper_mouse_x     = x;
			sdl_xr_mapper_mouse_y     = y;
			sdl_xr_mapper_mouse_valid = true;
			SDL_PushEvent(&event);
		}
	}
	// While the mapper waits for a bind, the trigger belongs to the
	// capture, not to the UI. A press must start on the panel, but
	// stays held if the aim drifts off it.
	const bool trigger = sdl_xr_hand[0].click || sdl_xr_hand[1].click;
	const bool down    = !MAPPER_IsAddBindArmed() &&
	                  (sdl_xr_mapper_mouse_down
	                           ? trigger
	                           : (hand >= 0 && sdl_xr_hand[hand].click));
	if (down != sdl_xr_mapper_mouse_down &&
	    (down || sdl_xr_mapper_mouse_valid)) {
		SDL_Event event = {};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN
		                  : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = win_id;
		event.button.which    = 0;
		event.button.button   = SDL_BUTTON_LEFT;
		event.button.down     = down;
		event.button.clicks   = 1;
		event.button.x        = static_cast<float>(sdl_xr_mapper_mouse_x);
		event.button.y        = static_cast<float>(sdl_xr_mapper_mouse_y);
		sdl_xr_mapper_mouse_down = down;
		SDL_PushEvent(&event);
	}
}
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

XrGfxBackend openxr_gfx_backend()
{
#if C_XR_VULKAN
	// The Vulkan session needs our own VkDevice (the SDL Vulkan
	// renderer cannot share its handles). The device is created
	// during bring-up; the loader probe decides hot-path
	// availability (quiet, so recovery polling never spams).
	if (sdl.render_backend_type == RenderBackendType::Vulkan &&
	    xrvk_have_loader()) {
		return XrGfxBackend::Vulkan;
	}
#endif
#if C_XR_D3D11
	// D3D11 is inbox (no loader probe needed); the device is created
	// during bring-up like Vulkan's.
	if (sdl.render_backend_type == RenderBackendType::D3D11) {
		return XrGfxBackend::D3D11;
	}
#endif
#if C_XR_D3D12
	if (sdl.render_backend_type == RenderBackendType::D3D12) {
		return XrGfxBackend::D3D12;
	}
#endif
	if (sdl.render_backend_type == RenderBackendType::OpenGl) {
		return XrGfxBackend::OpenGl;
	}
	return XrGfxBackend::None;
}

bool openxr_enumerate_images(XrSwapchain swapchain, const char* label,
                             std::vector<XrGfxImage>& out)
{
	out.clear();
	if (swapchain == XR_NULL_HANDLE) {
		return false;
	}
	if (openxr_gfx_backend() == XrGfxBackend::Vulkan) {
#if C_XR_VULKAN
		return xrvk_enumerate(swapchain, label, out);
#else
		return false;
#endif
	}
	if (openxr_gfx_backend() == XrGfxBackend::D3D11) {
#if C_XR_D3D11
		return xrd11_enumerate(swapchain, label, out);
#else
		return false;
#endif
	}
	if (openxr_gfx_backend() == XrGfxBackend::D3D12) {
#if C_XR_D3D12
		return xrd12_enumerate(swapchain, label, out);
#else
		return false;
#endif
	}
	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr) !=
	            XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate %s swapchain images", label);
		return false;
	}
	std::vector<XrSwapchainImageOpenGLKHR> tmp(image_count);
	for (auto& img : tmp) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		img.next = nullptr;
	}
	if (xrEnumerateSwapchainImages(
	            swapchain,
	            image_count,
	            &image_count,
	            reinterpret_cast<XrSwapchainImageBaseHeader*>(
	                    tmp.data())) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate %s swapchain images", label);
		return false;
	}
	out.resize(image_count);
	for (uint32_t i = 0; i < image_count; ++i) {
		out[i].type   = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		out[i].handle = tmp[i].image;
	}
	LOG_MSG("OpenXR: %s swapchain ready (%u GL textures)", label,
	        image_count);
	return true;
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

// Grip-adjustable panel layouts (see xr.h). Loaded from [xr] the first
// time a frame needs them; grips write back on release so placement
// survives a restart.
XrPanelLayout sdl_xr_layout_screen;
XrPanelLayout sdl_xr_layout_keyboard;
XrPanelLayout sdl_xr_layout_menu;

static bool sdl_xr_layout_loaded = false;

void openxr_layout_load()
{
	if (sdl_xr_layout_loaded) {
		return;
	}
	sdl_xr_layout_loaded = true;
	auto* section         = get_xr_section();

	sdl_xr_layout_screen.offset_x = std::clamp(
	        section->GetInt("xr_screen_offset_x") / 100.0f, -1.0f, 1.0f);
	sdl_xr_layout_screen.offset_y = std::clamp(
	        section->GetInt("xr_screen_offset_y") / 100.0f, -1.0f, 1.0f);
	sdl_xr_layout_screen.scale = std::clamp(
	        section->GetInt("xr_screen_zoom") / 100.0f, 0.25f, 3.0f);

	sdl_xr_layout_keyboard.offset_x = std::clamp(
	        section->GetInt("xr_keyboard_offset_x") / 100.0f, -1.0f, 1.0f);
	sdl_xr_layout_keyboard.offset_y = std::clamp(
	        section->GetInt("xr_keyboard_offset_y") / 100.0f, -1.0f, 1.0f);
	sdl_xr_layout_keyboard.scale = std::clamp(
	        section->GetInt("xr_keyboard_scale") / 100.0f, 0.25f, 2.0f);

	sdl_xr_layout_menu.offset_x = std::clamp(
	        section->GetInt("xr_menu_offset_x") / 100.0f, -1.0f, 1.0f);
	sdl_xr_layout_menu.offset_y = std::clamp(
	        section->GetInt("xr_menu_offset_y") / 100.0f, -1.0f, 1.0f);
	sdl_xr_layout_menu.scale = std::clamp(
	        section->GetInt("xr_menu_scale") / 100.0f, 0.25f, 2.0f);
}

void openxr_layout_save(const int panel)
{
	auto save = [](const char* name, const float value) {
		set_section_property_value("xr",
		                           name,
		                           std::to_string(static_cast<int>(
		                                   std::lround(value))));
	};
	switch (panel) {
	case 0:
		save("xr_screen_offset_x", sdl_xr_layout_screen.offset_x * 100.0f);
		save("xr_screen_offset_y", sdl_xr_layout_screen.offset_y * 100.0f);
		save("xr_screen_zoom", sdl_xr_layout_screen.scale * 100.0f);
		break;
	case 1:
		save("xr_keyboard_offset_x",
		     sdl_xr_layout_keyboard.offset_x * 100.0f);
		save("xr_keyboard_offset_y",
		     sdl_xr_layout_keyboard.offset_y * 100.0f);
		save("xr_keyboard_scale", sdl_xr_layout_keyboard.scale * 100.0f);
		break;
	case 2:
		save("xr_menu_offset_x", sdl_xr_layout_menu.offset_x * 100.0f);
		save("xr_menu_offset_y", sdl_xr_layout_menu.offset_y * 100.0f);
		save("xr_menu_scale", sdl_xr_layout_menu.scale * 100.0f);
		break;
	default: break;
	}
}

// Screen-plane geometry shared by pointer mapping and the keyboard
// quad: anchor pose plus half extents at the configured distance.
bool openxr_screen_plane(XrPosef& anchor_out, float& half_w_out,
                                float& half_h_out, float& dist_out)
{
	openxr_layout_load();
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
	// Base extents (unscaled) drive the grip offsets, so moving and
	// resizing stay independent. The scale grows the plane about its
	// centre; keyboard/menu placement follows the scaled extents.
	const float base_half_w = dist_m * tan_h;
	const float base_half_h = dist_m * tan_v;
	half_w_out = base_half_w * sdl_xr_layout_screen.scale;
	half_h_out = base_half_h * sdl_xr_layout_screen.scale;

	const XrVec3 right = xr_quat_rotate(anchor_out.orientation,
	                                    {1.0f, 0.0f, 0.0f});
	const XrVec3 up = xr_quat_rotate(anchor_out.orientation,
	                                 {0.0f, 1.0f, 0.0f});
	const float ox = sdl_xr_layout_screen.offset_x * base_half_w;
	const float oy = sdl_xr_layout_screen.offset_y * base_half_h;
	anchor_out.position.x += right.x * ox + up.x * oy;
	anchor_out.position.y += right.y * ox + up.y * oy;
	anchor_out.position.z += right.z * ox + up.z * oy;

	dist_out = dist_m;
	return true;
}

// World-space pose and size of the pictured area of the screen,
// cropped to the blitted picture rect so it can float as a panel.
bool openxr_screen_panel(XrPosef& pose_out, float& width_out,
                         float& height_out)
{
	XrPosef anchor = {};
	float half_w = 0.0f, half_h = 0.0f, dist_m = 0.0f;
	if (!openxr_screen_plane(anchor, half_w, half_h, dist_m)) {
		return false;
	}
	int px = 0, py = 0, pw = 0, ph = 0;
	if (!OPENXR_GetPictureRect(0, px, py, pw, ph) ||
	    sdl_xr_swapchain_width == 0 || sdl_xr_swapchain_height == 0) {
		return false;
	}
	(void)px;
	(void)py;
	const XrVec3 fwd = xr_quat_rotate(anchor.orientation,
	                                  {0.0f, 0.0f, -1.0f});
	pose_out.orientation = anchor.orientation;
	pose_out.position    = {anchor.position.x + fwd.x * dist_m,
	                        anchor.position.y + fwd.y * dist_m,
	                        anchor.position.z + fwd.z * dist_m};
	width_out  = 2.0f * half_w *
	             (static_cast<float>(pw) / sdl_xr_swapchain_width);
	height_out = 2.0f * half_h *
	             (static_cast<float>(ph) / sdl_xr_swapchain_height);
	return (width_out > 0.0f && height_out > 0.0f);
}


// Overlay distance in meters from an *_distance option (cm, clamped
// like the screen distance). Most live in [xr]; the toast distance is
// in [osd], so callers can pass the section that owns the option.
float openxr_layer_dist(const char* opt_name, SectionProp* section)
{
	if (section == nullptr) {
		section = get_xr_section();
	}
	int dist_cm = section->GetInt(opt_name);
	if (dist_cm < 50) {
		dist_cm = 50;
	}
	if (dist_cm > 500) {
		dist_cm = 500;
	}
	return dist_cm / 100.0f;
}

// World-space geometry of the overlay keyboard band (the same quad the
// submit path places). Used for aim-ray hit-testing so key selection
// follows the keyboard's own plane instead of the screen plane at a
// different distance.
bool openxr_keyboard_quad(XrPosef& pose_out, float& width_out,
                          float& height_out)
{
	XrPosef anchor = {};
	float half_w = 0.0f, half_h = 0.0f, dist_m = 0.0f;
	if (!openxr_screen_plane(anchor, half_w, half_h, dist_m)) {
		return false;
	}
	openxr_layout_load();
	const XrVec3 fwd = xr_quat_rotate(anchor.orientation,
	                                  {0.0f, 0.0f, -1.0f});
	const XrVec3 up = xr_quat_rotate(anchor.orientation,
	                                 {0.0f, 1.0f, 0.0f});
	const XrVec3 right = xr_quat_rotate(anchor.orientation,
	                                    {1.0f, 0.0f, 0.0f});
	const float ox = std::clamp(sdl_xr_layout_keyboard.offset_x,
	                            -1.0f,
	                            1.0f) *
	                 half_w;
	const float oy = std::clamp(sdl_xr_layout_keyboard.offset_y,
	                            -1.0f,
	                            1.0f) *
	                 half_h;
	const float kd = openxr_layer_dist("xr_kb_distance") + 0.02f;
	pose_out.orientation = anchor.orientation;
	pose_out.position    = {anchor.position.x + fwd.x * kd -
	                                up.x * (0.55f * half_h) + right.x * ox +
	                                up.x * oy,
	                        anchor.position.y + fwd.y * kd -
	                                up.y * (0.55f * half_h) + right.y * ox +
	                                up.y * oy,
	                        anchor.position.z + fwd.z * kd -
	                                up.z * (0.55f * half_h) + right.z * ox +
	                                up.z * oy};
	const float s = std::clamp(sdl_xr_layout_keyboard.scale, 0.25f, 2.0f) *
	                openxr_keyboard_key_scale();
	width_out  = 2.0f * half_w * s;
	height_out = 0.9f * half_h * s;
	return (width_out > 0.0f && height_out > 0.0f);
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
#if C_XR_VULKAN
static XrGraphicsBindingVulkanKHR sdl_xr_gfx_binding_vulkan = {};
#endif
#if C_XR_D3D11
static XrGraphicsBindingD3D11KHR sdl_xr_gfx_binding_d3d11 = {};
#endif
#if C_XR_D3D12
static XrGraphicsBindingD3D12KHR sdl_xr_gfx_binding_d3d12 = {};
#endif

// Resolve the OpenXR graphics binding (GL from the live SDL window +
// context, Vulkan from our own VkDevice). Returns a pointer to static
// binding storage (valid until process exit) or nullptr when
// unavailable/unsupported.
static const void* openxr_get_graphics_binding()
{
#if C_XR_VULKAN
	if (sdl.render_backend_type == RenderBackendType::Vulkan) {
		// Created by the requirements step; the device survives
		// session rebuilds and is only destroyed by cleanup_openxr().
		if (!xrvk_is_ready()) {
			LOG_ERR("OpenXR: No Vulkan device for XR graphics binding");
			return nullptr;
		}
		return &sdl_xr_gfx_binding_vulkan;
	}
#endif
#if C_XR_D3D11
	if (sdl.render_backend_type == RenderBackendType::D3D11) {
		if (!xrd11_is_ready()) {
			LOG_ERR("OpenXR: No D3D11 device for XR graphics binding");
			return nullptr;
		}
		return &sdl_xr_gfx_binding_d3d11;
	}
#endif
#if C_XR_D3D12
	if (sdl.render_backend_type == RenderBackendType::D3D12) {
		if (!xrd12_is_ready()) {
			LOG_ERR("OpenXR: No D3D12 device for XR graphics binding");
			return nullptr;
		}
		return &sdl_xr_gfx_binding_d3d12;
	}
#endif
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
#if C_XR_VULKAN
	if (sdl.render_backend_type == RenderBackendType::Vulkan) {
		// The Vulkan requirements query runs inside; the VkDevice
		// is created here too, and the binding it fills is consumed
		// by session creation below.
		return xrvk_create(sdl_xr_instance, sdl_xr_systemId,
		                   &sdl_xr_gfx_binding_vulkan);
	}
#endif
#if C_XR_D3D11
	if (sdl.render_backend_type == RenderBackendType::D3D11) {
		// Same shape: requirements inside, device created here.
		return xrd11_create(sdl_xr_instance, sdl_xr_systemId,
		                    &sdl_xr_gfx_binding_d3d11);
	}
#endif
#if C_XR_D3D12
	if (sdl.render_backend_type == RenderBackendType::D3D12) {
		return xrd12_create(sdl_xr_instance, sdl_xr_systemId,
		                    &sdl_xr_gfx_binding_d3d12);
	}
#endif
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
	if (openxr_gfx_backend() == XrGfxBackend::Vulkan) {
#if C_XR_VULKAN
		sdl_xr_swapchain_format =
		        xrvk_pick_format(formats.data(), format_count);
#else
		sdl_xr_swapchain_format = 0;
#endif
	} else if (openxr_gfx_backend() == XrGfxBackend::D3D11) {
#if C_XR_D3D11
		sdl_xr_swapchain_format =
		        xrd11_pick_format(formats.data(), format_count);
#else
		sdl_xr_swapchain_format = 0;
#endif
	} else if (openxr_gfx_backend() == XrGfxBackend::D3D12) {
#if C_XR_D3D12
		sdl_xr_swapchain_format =
		        xrd12_pick_format(formats.data(), format_count);
#else
		sdl_xr_swapchain_format = 0;
#endif
	} else {
		sdl_xr_swapchain_format = formats[0];
		for (const auto fmt : formats) {
			if (fmt == 0x8C43) { // GL_SRGB8_ALPHA8
				sdl_xr_swapchain_format = fmt;
				break;
			}
		}
	}
	if (sdl_xr_swapchain_format == 0) {
		LOG_ERR("OpenXR: No usable swapchain format");
		return false;
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
	if (openxr_gfx_backend() == XrGfxBackend::Vulkan) {
		// Transfer-dst for the CPU staging uploads (see xrvulkan).
		swapchain_info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	}
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

	// 3. Enumerate the images backing the swapchain (GL textures or
	// VkImages depending on the backend).
	if (!openxr_enumerate_images(sdl_xr_swapchain,
	                             "swapchain",
	                             sdl_xr_swapchain_images)) {
		return false;
	}

	LOG_MSG("OpenXR: Swapchain created: %ux%u x%u, format %lld, %u images",
	        sdl_xr_swapchain_width,
	        sdl_xr_swapchain_height,
	        sdl_xr_view_count,
	        static_cast<long long>(sdl_xr_swapchain_format),
	        static_cast<unsigned int>(sdl_xr_swapchain_images.size()));
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
	// Nothing was acquired (mapper mode, timeout, or a failed frame):
	// releasing anyway is a call-order violation.
	if (sdl_xr_active_image < 0) {
		return true;
	}
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

static bool openxr_setup_mapper_layer(const int w_px, const int h_px)
{
	if (sdl_xr_session == XR_NULL_HANDLE || sdl_xr_swapchain_format == 0 ||
	    w_px <= 0 || h_px <= 0) {
		return false;
	}
	if (sdl_xr_mapper_swapchain != XR_NULL_HANDLE &&
	    sdl_xr_mapper_tex_w == w_px && sdl_xr_mapper_tex_h == h_px) {
		return true; // already built at this size
	}
	if (sdl_xr_mapper_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_mapper_swapchain);
		sdl_xr_mapper_swapchain = XR_NULL_HANDLE;
		sdl_xr_mapper_images.clear();
		sdl_xr_mapper_img_epoch.clear();
		sdl_xr_mapper_filled = false;
		++sdl_xr_mapper_epoch; // new images must refresh
	}

	XrSwapchainCreateInfo info = {};
	info.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                   XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	if (openxr_gfx_backend() == XrGfxBackend::Vulkan) {
		info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	}
	info.format      = sdl_xr_swapchain_format;
	info.sampleCount = 1;
	info.width       = w_px;
	info.height      = h_px;
	info.faceCount   = 1;
	info.arraySize   = 1;
	info.mipCount    = 1;

	if (xrCreateSwapchain(sdl_xr_session, &info, &sdl_xr_mapper_swapchain) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create mapper swapchain");
		sdl_xr_mapper_swapchain = XR_NULL_HANDLE;
		return false;
	}

	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(sdl_xr_mapper_swapchain,
	                               0,
	                               &image_count,
	                               nullptr) != XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate mapper swapchain images");
		return false;
	}
	if (!openxr_enumerate_images(sdl_xr_mapper_swapchain,
	                             "mapper",
	                             sdl_xr_mapper_images)) {
		return false;
	}

	sdl_xr_mapper_tex_w = w_px;
	sdl_xr_mapper_tex_h = h_px;
	sdl_xr_mapper_img_epoch.assign(image_count, 0);
	LOG_MSG("OpenXR: Mapper layer ready (%dx%d, %u images)",
	        w_px,
	        h_px,
	        image_count);
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
	    openxr_gfx_backend() != XrGfxBackend::None) {
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
			    openxr_gfx_backend() != XrGfxBackend::None) {
				bool filled = false;
				if (openxr_gfx_backend() ==
				    XrGfxBackend::OpenGl) {
					auto* gl_renderer =
					        static_cast<OpenGlRenderer*>(
					                sdl.renderer.get());
					gl_renderer->CopyTextureToLayer(
					        sdl_xr_kbd_tex,
					        kbd_tex_w,
					        kbd_tex_h,
					        static_cast<GLuint>(
					                sdl_xr_kbd_images[index]
					                        .handle),
					        0,
					        false); // arraySize 1
					filled = true;
				}
#if C_XR_VULKAN
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::Vulkan) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_kbd_cpu_raster(cpu_w,
					                              cpu_h);
					filled = !cpu.empty() &&
					         xrvk_upload_full(
					                 sdl_xr_kbd_images[index]
					                         .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
#if C_XR_D3D11
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D11) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_kbd_cpu_raster(cpu_w,
					                              cpu_h);
					filled = !cpu.empty() &&
					         xrd11_upload_full(
					                 sdl_xr_kbd_images[index]
					                         .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
#if C_XR_D3D12
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D12) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_kbd_cpu_raster(cpu_w,
					                              cpu_h);
					filled = !cpu.empty() &&
					         xrd12_upload_full(
					                 sdl_xr_kbd_images[index]
					                         .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
				if (filled) {
					sdl_xr_kbd_img_epoch[index] =
					        sdl_xr_kbd_tex_epoch;
				}
			}
		if (xrReleaseSwapchainImage(sdl_xr_kbd_swapchain,
		                            &release) == XR_SUCCESS) {
			XrPosef kbd_pose = {};
			float kbd_w = 0.0f, kbd_h = 0.0f;
			if (openxr_keyboard_quad(kbd_pose, kbd_w, kbd_h)) {
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.space = sdl_xr_app_space;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				// Blend on the texture alpha so xr_keyboard_opacity
				// can fade the panel (opaque at 100%).
				quad.layerFlags =
				        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				quad.subImage.swapchain = sdl_xr_kbd_swapchain;
				quad.subImage.imageRect = {
				        {0, 0},
				        {static_cast<int32_t>(kbd_tex_w),
				         static_cast<int32_t>(kbd_tex_h)}};
				quad.subImage.imageArrayIndex = 0;
				quad.pose = kbd_pose;
				quad.size = {kbd_w, kbd_h};
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
	}

	// Notification overlay on top: the toast raster is shared with the
	// desktop path; swapchain images refresh when its generation moves.
	XrCompositionLayerQuad osd_quad = {};
	bool show_osd                   = false;
	if (render_layers && get_osd_section()->GetBool("osd_xr") &&
	    sdl_xr_osd_swapchain != XR_NULL_HANDLE && !sdl_xr_osd.empty()) {
		const uint32_t* osd_px = nullptr;
		int osd_w = 0, osd_h = 0;
		uint64_t osd_gen = 0;
		if (openxr_osd_upload_raster(openxr_osd_opacity(),
		                             osd_px,
		                             osd_w,
		                             osd_h,
		                             osd_gen)) {
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
				if (sdl_xr_osd_img_gen.size() !=
				    sdl_xr_osd_images.size()) {
					sdl_xr_osd_img_gen.assign(
					        sdl_xr_osd_images.size(),
					        0);
				}
				if (osd_index < sdl_xr_osd_img_gen.size() &&
				    sdl_xr_osd_img_gen[osd_index] != osd_gen &&
				    sdl.renderer != nullptr &&
				    openxr_gfx_backend() !=
				            XrGfxBackend::None) {
					bool filled = false;
					if (openxr_gfx_backend() ==
					    XrGfxBackend::OpenGl) {
						auto* gl_renderer =
						        static_cast<OpenGlRenderer*>(
						                sdl.renderer.get());
						const GLuint osd_tex =
						        gl_renderer->UpdateXrRawTexture(
						                osd_px,
						                osd_w,
						                osd_h);
						if (osd_tex != 0) {
							gl_renderer->CopyTextureToLayer(
							        osd_tex,
							        osd_w,
							        osd_h,
							        static_cast<GLuint>(
							                sdl_xr_osd_images
							                        [osd_index]
							                                .handle),
							        0,
							        false); // arraySize 1
							filled = true;
						}
					}
#if C_XR_VULKAN
					else if (openxr_gfx_backend() ==
					         XrGfxBackend::Vulkan) {
						filled = xrvk_upload_full(
						        sdl_xr_osd_images[osd_index]
						                .handle,
						        0,
						        osd_w,
						        osd_h,
						        osd_px);
					}
#endif
#if C_XR_D3D11
					else if (openxr_gfx_backend() ==
					         XrGfxBackend::D3D11) {
						filled = xrd11_upload_full(
						        sdl_xr_osd_images[osd_index]
						                .handle,
						        0,
						        osd_w,
						        osd_h,
						        osd_px);
					}
#endif
#if C_XR_D3D12
					else if (openxr_gfx_backend() ==
					         XrGfxBackend::D3D12) {
						filled = xrd12_upload_full(
						        sdl_xr_osd_images[osd_index]
						                .handle,
						        0,
						        osd_w,
						        osd_h,
						        osd_px);
					}
#endif
					if (filled) {
						sdl_xr_osd_img_gen[osd_index] =
						        osd_gen;
					}
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
				        openxr_layer_dist("osd_xr_distance",
				                          get_osd_section()) +
				        0.02f;
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
				                   0.05f,
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
	    openxr_gfx_backend() != XrGfxBackend::None) {
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
			    openxr_gfx_backend() !=
			            XrGfxBackend::None) {
				bool filled = false;
				if (openxr_gfx_backend() ==
				    XrGfxBackend::OpenGl) {
					auto* gl_renderer =
					        static_cast<OpenGlRenderer*>(
					                sdl.renderer.get());
					gl_renderer->CopyTextureToLayer(
					        sdl_xr_menu_tex,
					        OpenXrMenuTexW,
					        OpenXrMenuTexH,
					        static_cast<GLuint>(
					                sdl_xr_menu_images
					                        [menu_index]
					                                .handle),
					        0,
					        false); // arraySize 1
					filled = true;
				}
#if C_XR_VULKAN
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::Vulkan) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_menu_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrvk_upload_full(
					                 sdl_xr_menu_images
					                         [menu_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
#if C_XR_D3D11
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D11) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_menu_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrd11_upload_full(
					                 sdl_xr_menu_images
					                         [menu_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
#if C_XR_D3D12
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D12) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_menu_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrd12_upload_full(
					                 sdl_xr_menu_images
					                         [menu_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
				if (filled) {
					sdl_xr_menu_img_epoch[menu_index] =
					        sdl_xr_menu_tex_epoch;
				}
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
				const XrVec3 menu_right = xr_quat_rotate(
				        menu_anchor.orientation,
				        {1.0f, 0.0f, 0.0f});
				const float menu_ox = std::clamp(
				        sdl_xr_layout_menu.offset_x, -1.0f, 1.0f);
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
				float panel_w = 0.0f, panel_h = 0.0f;
				float panel_up = 0.0f;
				float panel_vc = 0.0f, panel_vhalf = 0.0f;
				openxr_menu_layout(menu_half_h,
				                   panel_w,
				                   panel_h,
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
				                menu_up.x * panel_up +
				                menu_right.x * (menu_ox * menu_half_w),
				        menu_anchor.position.y +
				                menu_fwd.y * menu_layer_dist +
				                menu_up.y * panel_up +
				                menu_right.y * (menu_ox * menu_half_w),
				        menu_anchor.position.z +
				                menu_fwd.z * menu_layer_dist +
				                menu_up.z * panel_up +
				                menu_right.z * (menu_ox * menu_half_w)};
				menu_quad.size = {panel_w, panel_h};
				show_menu = true;
			}
		}
	}

	// Radial input-mode wheel: a square panel riding the active hand's
	// aim pose, rebuilt only when the highlighted wedge changes.
	XrCompositionLayerQuad wheel_quad = {};
	bool show_wheel                   = false;
	if (sdl_xr_wheel_swapchain == XR_NULL_HANDLE) {
		openxr_setup_wheel_layer(); // best effort; fill follows
	}
	if (render_layers && openxr_wheel_open() &&
	    sdl_xr_wheel_swapchain != XR_NULL_HANDLE &&
	    sdl.renderer != nullptr &&
	    openxr_gfx_backend() != XrGfxBackend::None) {
		const int wheel_hand = openxr_wheel_active_hand();
		const int wheel_hl   = openxr_wheel_highlight();
		const std::string wheel_sig =
		        openxr_wheel_signature(wheel_hand, wheel_hl);
		if (wheel_sig != sdl_xr_wheel_built_sig) {
			const GLuint wheel_tex =
			        openxr_refresh_wheel_texture(wheel_hand,
			                                     wheel_hl);
			if (wheel_tex != 0) {
				sdl_xr_wheel_tex = wheel_tex;
				sdl_xr_wheel_built_sig = wheel_sig;
				++sdl_xr_wheel_tex_epoch;
			}
		}
	}
	if (render_layers && openxr_wheel_open() &&
	    sdl_xr_wheel_swapchain != XR_NULL_HANDLE &&
	    sdl_xr_hand[openxr_wheel_active_hand()].pose_valid) {
		const int wheel_hand = openxr_wheel_active_hand();
		XrSwapchainImageAcquireInfo wheel_acquire = {};
		wheel_acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
		uint32_t wheel_index = 0;
		XrSwapchainImageWaitInfo wheel_wait = {};
		wheel_wait.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
		wheel_wait.timeout = OpenXrSwapchainWaitTimeoutNs;
		XrSwapchainImageReleaseInfo wheel_release = {};
		wheel_release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
		if (xrAcquireSwapchainImage(sdl_xr_wheel_swapchain,
		                            &wheel_acquire,
		                            &wheel_index) == XR_SUCCESS &&
		    xrWaitSwapchainImage(sdl_xr_wheel_swapchain,
		                         &wheel_wait) == XR_SUCCESS) {
			if (sdl_xr_wheel_img_epoch.size() !=
			    sdl_xr_wheel_images.size()) {
				sdl_xr_wheel_img_epoch.assign(
				        sdl_xr_wheel_images.size(), 0);
			}
			if (wheel_index < sdl_xr_wheel_img_epoch.size() &&
			    sdl_xr_wheel_img_epoch[wheel_index] !=
			            sdl_xr_wheel_tex_epoch &&
			    sdl.renderer != nullptr &&
			    openxr_gfx_backend() != XrGfxBackend::None) {
				bool filled = false;
				if (openxr_gfx_backend() ==
				    XrGfxBackend::OpenGl) {
					auto* gl_renderer =
					        static_cast<OpenGlRenderer*>(
					                sdl.renderer.get());
					gl_renderer->CopyTextureToLayer(
					        sdl_xr_wheel_tex,
					        OpenXrWheelTexW,
					        OpenXrWheelTexH,
					        static_cast<GLuint>(
					                sdl_xr_wheel_images
					                        [wheel_index]
					                                .handle),
					        0,
					        false); // arraySize 1
					filled = true;
				}
#if C_XR_VULKAN
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::Vulkan) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_wheel_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrvk_upload_full(
					                 sdl_xr_wheel_images
					                         [wheel_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
#if C_XR_D3D11
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D11) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_wheel_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrd11_upload_full(
					                 sdl_xr_wheel_images
					                         [wheel_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
#if C_XR_D3D12
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D12) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_wheel_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrd12_upload_full(
					                 sdl_xr_wheel_images
					                         [wheel_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
				if (filled) {
					sdl_xr_wheel_img_epoch[wheel_index] =
					        sdl_xr_wheel_tex_epoch;
				}
			}
			if (xrReleaseSwapchainImage(sdl_xr_wheel_swapchain,
			                            &wheel_release) ==
			    XR_SUCCESS) {
				const XrPosef hp =
				        sdl_xr_hand[wheel_hand].pose;
				const XrVec3 wfwd = xr_quat_rotate(
				        hp.orientation,
				        {0.0f, 0.0f, -1.0f});
				wheel_quad.type =
				        XR_TYPE_COMPOSITION_LAYER_QUAD;
				wheel_quad.space = sdl_xr_app_space;
				wheel_quad.eyeVisibility =
				        XR_EYE_VISIBILITY_BOTH;
				wheel_quad.layerFlags =
				        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				wheel_quad.subImage.swapchain =
				        sdl_xr_wheel_swapchain;
				wheel_quad.subImage.imageRect = {
				        {0, 0},
				        {static_cast<int32_t>(OpenXrWheelTexW),
				         static_cast<int32_t>(OpenXrWheelTexH)}};
				wheel_quad.subImage.imageArrayIndex = 0;
				wheel_quad.pose.orientation = hp.orientation;
				// Held a little in front of the aim origin so the
				// panel clears the controller body.
				wheel_quad.pose.position = {
				        hp.position.x +
				                wfwd.x * OpenXrWheelDistM,
				        hp.position.y +
				                wfwd.y * OpenXrWheelDistM,
				        hp.position.z +
				                wfwd.z * OpenXrWheelDistM};
				wheel_quad.size = {OpenXrWheelSizeM,
				                   OpenXrWheelSizeM};
				show_wheel = true;
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
	    openxr_gfx_backend() != XrGfxBackend::None) {
		// Static gradient content: refresh the texture pointer when
		// the renderer rebuilds it, then fill stale images below.
		// The Vulkan path fills from the retained CPU raster
		// instead (refresh returns 1 once built).
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
			    openxr_gfx_backend() !=
			            XrGfxBackend::None) {
				bool filled = false;
				if (openxr_gfx_backend() ==
				    XrGfxBackend::OpenGl) {
					auto* gl_renderer =
					        static_cast<OpenGlRenderer*>(
					                sdl.renderer.get());
					gl_renderer->CopyTextureToLayer(
					        sdl_xr_ray_tex,
					        OpenXrRayTexW,
					        OpenXrRayTexH,
					        static_cast<GLuint>(
					                sdl_xr_ray_images
					                        [ray_index]
					                                .handle),
					        0,
					        false); // arraySize 1
					filled = true;
				}
#if C_XR_VULKAN
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::Vulkan) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_ray_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrvk_upload_full(
					                 sdl_xr_ray_images
					                         [ray_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
#if C_XR_D3D11
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D11) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_ray_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrd11_upload_full(
					                 sdl_xr_ray_images
					                         [ray_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
#if C_XR_D3D12
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D12) {
					int cpu_w = 0, cpu_h = 0;
					const auto& cpu =
					        openxr_ray_cpu_raster(
					                cpu_w, cpu_h);
					filled = !cpu.empty() &&
					         xrd12_upload_full(
					                 sdl_xr_ray_images
					                         [ray_index]
					                                 .handle,
					                 0, cpu_w, cpu_h,
					                 cpu.data());
				}
#endif
				if (filled) {
					sdl_xr_ray_img_epoch[ray_index] =
					        sdl_xr_ray_tex_epoch;
				}
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

	// Screen as a world-space panel: submit the picture crop as one
	// quad per eye instead of the full projection, so grips can move
	// and resize it like the other HUD layers. Split/interleaved
	// stereo modes keep the projection (their eye slices are not a
	// plain picture rect).
	XrCompositionLayerQuad screen_quads[2] = {};
	bool use_screen_panel                  = false;
	if (render_layers && get_xr_section()->GetBool("xr_screen_panel")) {
		const std::string stereo_mode =
		        get_xr_section()->GetString("xr_stereo_input");
		const bool split_mode = (stereo_mode == "interleaved" ||
		                         stereo_mode == "top-bottom" ||
		                         stereo_mode == "sbs" ||
		                         stereo_mode == "cybermaxx" ||
		                         stereo_mode == "iglasses");
		XrPosef panel_pose = {};
		float panel_w = 0.0f, panel_h = 0.0f;
		if (!split_mode &&
		    openxr_screen_panel(panel_pose, panel_w, panel_h)) {
			bool rects_ok = true;
			for (int eye = 0; eye < 2; ++eye) {
				int px = 0, py = 0, pw = 0, ph = 0;
				if (!OPENXR_GetPictureRect(eye, px, py, pw, ph)) {
					rects_ok = false;
					break;
				}
				XrCompositionLayerQuad& q = screen_quads[eye];
				q.type          = XR_TYPE_COMPOSITION_LAYER_QUAD;
				q.space         = sdl_xr_app_space;
				q.eyeVisibility = (eye == 0)
				                          ? XR_EYE_VISIBILITY_LEFT
				                          : XR_EYE_VISIBILITY_RIGHT;
				q.subImage.swapchain = sdl_xr_swapchain;
				q.subImage.imageRect = {{px, py}, {pw, ph}};
				q.subImage.imageArrayIndex =
				        static_cast<uint32_t>(eye);
				q.pose = panel_pose;
				q.size = {panel_w, panel_h};
			}
			use_screen_panel = rects_ok;
		}
	}

	// In-headset mapper: while the mapper UI owns the window, the
	// scene layers are replaced by a single quad carrying the
	// captured UI pixels.
	XrCompositionLayerQuad mapper_quad = {};
	bool show_mapper                   = false;
	if (render_layers && sdl_xr_mapper_active &&
	    sdl_xr_mapper_swapchain != XR_NULL_HANDLE &&
	    !sdl_xr_mapper_cpu.empty()) {
		XrSwapchainImageAcquireInfo acquire = {};
		acquire.type    = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
		uint32_t index  = 0;
		XrSwapchainImageWaitInfo map_wait = {};
		map_wait.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
		map_wait.timeout = OpenXrSwapchainWaitTimeoutNs;
		XrSwapchainImageReleaseInfo release = {};
		release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;

		if (xrAcquireSwapchainImage(sdl_xr_mapper_swapchain,
		                            &acquire,
		                            &index) == XR_SUCCESS &&
		    xrWaitSwapchainImage(sdl_xr_mapper_swapchain, &map_wait) ==
		            XR_SUCCESS) {
			if (sdl_xr_mapper_img_epoch.size() !=
			    sdl_xr_mapper_images.size()) {
				sdl_xr_mapper_img_epoch.assign(
				        sdl_xr_mapper_images.size(), 0);
			}
			const int w_px = sdl_xr_mapper_cpu_w;
			const int h_px = sdl_xr_mapper_cpu_h;
			if (index < sdl_xr_mapper_img_epoch.size() &&
			    sdl_xr_mapper_img_epoch[index] != sdl_xr_mapper_epoch &&
			    w_px > 0 && h_px > 0) {
				bool filled = false;
				if (openxr_gfx_backend() ==
				    XrGfxBackend::OpenGl) {
					auto* gl_renderer =
					        static_cast<OpenGlRenderer*>(
					                sdl.renderer.get());
					if (gl_renderer) {
						const GLuint tex =
						        gl_renderer->UpdateXrRawTexture(
						                sdl_xr_mapper_cpu.data(),
						                w_px,
						                h_px);
						if (tex) {
							gl_renderer->CopyTextureToLayer(
							        tex,
							        w_px,
							        h_px,
							        static_cast<GLuint>(
							                sdl_xr_mapper_images[index]
							                        .handle),
							        0,
							        false);
							filled = true;
						}
					}
				}
#if C_XR_VULKAN
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::Vulkan) {
					filled = xrvk_upload_full(
					        sdl_xr_mapper_images[index].handle,
					        0,
					        w_px,
					        h_px,
					        sdl_xr_mapper_cpu.data());
				}
#endif
#if C_XR_D3D11
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D11) {
					filled = xrd11_upload_full(
					        sdl_xr_mapper_images[index].handle,
					        0,
					        w_px,
					        h_px,
					        sdl_xr_mapper_cpu.data());
				}
#endif
#if C_XR_D3D12
				else if (openxr_gfx_backend() ==
				         XrGfxBackend::D3D12) {
					filled = xrd12_upload_full(
					        sdl_xr_mapper_images[index].handle,
					        0,
					        w_px,
					        h_px,
					        sdl_xr_mapper_cpu.data());
				}
#endif
				if (filled) {
					sdl_xr_mapper_img_epoch[index] =
					        sdl_xr_mapper_epoch;
					sdl_xr_mapper_filled = true;
				}
			}
			xrReleaseSwapchainImage(sdl_xr_mapper_swapchain, &release);

			bool image_ready = sdl_xr_mapper_filled;
			if (index < sdl_xr_mapper_img_epoch.size() &&
			    sdl_xr_mapper_img_epoch[index] == sdl_xr_mapper_epoch) {
				image_ready = true;
			}
			XrMapperQuad mapper_geom = {};
			if (image_ready &&
			    openxr_mapper_quad_geometry(mapper_geom)) {
				mapper_quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				mapper_quad.space = sdl_xr_app_space;
				mapper_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				mapper_quad.subImage.swapchain =
				        sdl_xr_mapper_swapchain;
				mapper_quad.subImage.imageRect = {
				        {0, 0},
				        {mapper_geom.tex_w, mapper_geom.tex_h}};
				mapper_quad.subImage.imageArrayIndex = 0;
				mapper_quad.pose = mapper_geom.pose;
				mapper_quad.size = {mapper_geom.w, mapper_geom.h};
				show_mapper      = true;
			}
		}
	}

	const XrCompositionLayerBaseHeader* layers[10] = {};

	XrFrameEndInfo end_info = {};
	end_info.type                 = XR_TYPE_FRAME_END_INFO;
	end_info.displayTime          = display_time;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	uint32_t layer_count          = 0;
	if (render_layers) {
		if (show_mapper) {
			layers[layer_count++] = reinterpret_cast<
			        const XrCompositionLayerBaseHeader*>(&mapper_quad);
		} else {
			if (use_screen_panel) {
				for (int eye = 0; eye < 2; ++eye) {
					layers[layer_count++] = reinterpret_cast<
					        const XrCompositionLayerBaseHeader*>(
					        &screen_quads[eye]);
				}
			} else {
				layers[layer_count++] = reinterpret_cast<
				        const XrCompositionLayerBaseHeader*>(
				        &sdl_xr_proj_layer);
			}
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
			if (show_wheel) {
				layers[layer_count++] = reinterpret_cast<
				        const XrCompositionLayerBaseHeader*>(&wheel_quad);
			}
			for (int hand = 0; hand < 2; ++hand) {
				if (show_ray[hand]) {
					layers[layer_count++] = reinterpret_cast<
					        const XrCompositionLayerBaseHeader*>(
					        &ray_quad[hand]);
				}
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

	if (sdl_xr_mapper_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_mapper_swapchain);
		sdl_xr_mapper_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_mapper_images.clear();
	sdl_xr_mapper_img_epoch.clear();
	sdl_xr_mapper_cpu.clear();
	sdl_xr_mapper_cpu_w  = 0;
	sdl_xr_mapper_cpu_h  = 0;
	sdl_xr_mapper_tex_w  = 0;
	sdl_xr_mapper_tex_h  = 0;
	sdl_xr_mapper_filled = false;
	sdl_xr_mapper_active = false;

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

	{
		const bool want_gl =
		        (sdl.render_backend_type == RenderBackendType::OpenGl);
#if C_XR_VULKAN
		const bool want_vk =
		        (sdl.render_backend_type == RenderBackendType::Vulkan);
#else
		constexpr bool want_vk = false;
#endif
#if C_XR_D3D11
		const bool want_d3d11 =
		        (sdl.render_backend_type == RenderBackendType::D3D11);
#else
		constexpr bool want_d3d11 = false;
#endif
#if C_XR_D3D12
		const bool want_d3d12 =
		        (sdl.render_backend_type == RenderBackendType::D3D12);
#else
		constexpr bool want_d3d12 = false;
#endif
		if (!want_gl && !want_vk && !want_d3d11 && !want_d3d12) {
			// No XR graphics binding on this backend (the enable
			// path already logged it); stay down without burning
			// retries.
			return;
		}
	}

	XrResult result;

	// OpenXR instance creation. The graphics extension matching the
	// desktop backend is required later for session creation.
	static const char* const extensions_gl[] = {"XR_KHR_opengl_enable"};
#if C_XR_VULKAN
	static const char* const extensions_vk[] = {"XR_KHR_vulkan_enable"};
#endif
#if C_XR_D3D11
	static const char* const extensions_d3d11[] = {"XR_KHR_D3D11_enable"};
#endif
#if C_XR_D3D12
	static const char* const extensions_d3d12[] = {"XR_KHR_D3D12_enable"};
#endif
	const char* const* extensions = extensions_gl;
#if C_XR_VULKAN
	if (sdl.render_backend_type == RenderBackendType::Vulkan) {
		extensions = extensions_vk;
	}
#endif
#if C_XR_D3D11
	if (sdl.render_backend_type == RenderBackendType::D3D11) {
		extensions = extensions_d3d11;
	}
#endif
#if C_XR_D3D12
	if (sdl.render_backend_type == RenderBackendType::D3D12) {
		extensions = extensions_d3d12;
	}
#endif

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

#if C_OPENXR
// VFX1 bus: live headset orientation for HMD tracking reports.
// Plain floats cross the module boundary (no OpenXR types); false
// when no valid pose is available (caller falls back to synthetic).
bool openxr_head_quat(float& x, float& y, float& z, float& w)
{
	if (!sdl_xr_head_pose_valid) {
		return false;
	}
	x = sdl_xr_head_pose.orientation.x;
	y = sdl_xr_head_pose.orientation.y;
	z = sdl_xr_head_pose.orientation.z;
	w = sdl_xr_head_pose.orientation.w;
	return true;
}
#endif

// Permanently disables XR for this run (until xr_enabled is toggled
// off/on again). The desktop renderer is untouched: `output` now
// belongs to the desktop backend alone, so there is nothing to fall
// back to. Used after repeated bring-up failures that never produced
// a working session.
static void openxr_give_up_and_fallback()
{
	LOG_MSG("OpenXR: Unavailable after %d attempts, giving up for this run "
	        "(toggle [xr] xr_enabled off/on to retry); desktop rendering continues",
	        OpenXrMaxInitAttempts);
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
	if (!sdl_xr_mapper_active) {
		openxr_drive_panels();
		openxr_drive_menu();
		openxr_drive_mouse();
		openxr_drive_head_mouse();
		openxr_drive_keyboard();
		openxr_drive_wheel();
		openxr_drive_joystick();
		openxr_drive_puck();
	}

	// Mapper mode presents its own layer and never fills the
	// projection swapchain, so leave that image alone.
	if (sdl_xr_mapper_active) {
		return true;
	}

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
void openxr_mapper_begin(void* game_gl_context, void* mapper_gl_context)
{
	sdl_xr_mapper_game_gl_context = game_gl_context;
	sdl_xr_mapper_ui_gl_context   = mapper_gl_context;
	sdl_xr_mapper_active          = (sdl_xr_session != XR_NULL_HANDLE);
	sdl_xr_mapper_mouse_down      = false;
	sdl_xr_mapper_mouse_valid     = false;
}

void openxr_mapper_capture(SDL_Renderer* renderer)
{
	if (!renderer) {
		return;
	}
	SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
	if (!shot) {
		LOG_WARNING("OpenXR: Mapper readback failed: %s", SDL_GetError());
		return;
	}
	SDL_Surface* surface = SDL_ConvertSurface(shot,
	                                          SDL_PIXELFORMAT_ARGB8888);
	SDL_DestroySurface(shot);
	if (!surface) {
		return;
	}
	const int w_px = surface->w;
	const int h_px = surface->h;
	// Keep the per-frame software readback sane on very large windows.
	constexpr int MaxCaptureDim = 2048;
	if (w_px <= 0 || h_px <= 0 || w_px > MaxCaptureDim ||
	    h_px > MaxCaptureDim || !SDL_LockSurface(surface)) {
		SDL_DestroySurface(surface);
		return;
	}

	// The XR uploaders want RGBA byte order with row 0 at the bottom
	// (0xAABBGGRR words); SDL hands us ARGB top-down.
	const auto* src = static_cast<const uint32_t*>(surface->pixels);
	const int src_pitch = surface->pitch / static_cast<int>(sizeof(uint32_t));
	sdl_xr_mapper_cpu.resize(static_cast<size_t>(w_px) * h_px);
	for (int y = 0; y < h_px; ++y) {
		const uint32_t* row = src +
		                      static_cast<size_t>(h_px - 1 - y) * src_pitch;
		uint32_t* dst = sdl_xr_mapper_cpu.data() +
		                static_cast<size_t>(y) * w_px;
		for (int x = 0; x < w_px; ++x) {
			const uint32_t p = row[x];
			dst[x] = (p & 0xff00ff00u) | ((p & 0x00ff0000u) >> 16) |
			         ((p & 0x000000ffu) << 16);
		}
	}
	SDL_UnlockSurface(surface);
	SDL_DestroySurface(surface);

	sdl_xr_mapper_cpu_w = w_px;
	sdl_xr_mapper_cpu_h = h_px;
	++sdl_xr_mapper_epoch;
}

void openxr_mapper_frame([[maybe_unused]] SDL_Renderer* renderer)
{
	if (!sdl_xr_mapper_active || !sdl_xr_session_ready) {
		return;
	}
	if (sdl_xr_mapper_cpu_w > 0 && sdl_xr_mapper_cpu_h > 0) {
		openxr_setup_mapper_layer(sdl_xr_mapper_cpu_w,
		                          sdl_xr_mapper_cpu_h);
	}

	XrTime display_time = 0;
	if (!openxr_frame_begin(display_time)) {
		return;
	}

	// Motion controllers drive the mapper pointer and left button.
	openxr_mapper_drive_pointer();

	// XR swapchain images belong to the game's GL context, but the
	// mapper currently has its own context current. Switch for the
	// upload (inside submit) and back for the next mapper draw.
	bool switched = false;
#if C_OPENGL
	if (openxr_gfx_backend() == XrGfxBackend::OpenGl &&
	    sdl_xr_mapper_game_gl_context != nullptr && sdl.window != nullptr) {
		switched = SDL_GL_MakeCurrent(
		        sdl.window, static_cast<SDL_GLContext>(
		                            sdl_xr_mapper_game_gl_context));
	}
#endif
	openxr_frame_end(display_time);
#if C_OPENGL
	if (switched) {
		SDL_GL_MakeCurrent(sdl.window,
		                   static_cast<SDL_GLContext>(
		                           sdl_xr_mapper_ui_gl_context));
	}
#endif
}

void openxr_mapper_end()
{
	sdl_xr_mapper_active          = false;
	sdl_xr_mapper_mouse_down      = false;
	sdl_xr_mapper_mouse_valid     = false;
	sdl_xr_mapper_game_gl_context = nullptr;
	sdl_xr_mapper_ui_gl_context   = nullptr;
	if (sdl_xr_mapper_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_mapper_swapchain);
		sdl_xr_mapper_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_mapper_images.clear();
	sdl_xr_mapper_img_epoch.clear();
	sdl_xr_mapper_cpu.clear();
	sdl_xr_mapper_cpu_w        = 0;
	sdl_xr_mapper_cpu_h        = 0;
	sdl_xr_mapper_tex_w        = 0;
	sdl_xr_mapper_tex_h        = 0;
	sdl_xr_mapper_filled       = false;
	sdl_xr_mapper_epoch        = 1;
}

void cleanup_openxr()
{
	// End an active session before tearing down spaces/swapchains.
	openxr_end_session();
	openxr_mapper_end();
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
	openxr_osd_reset();
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
#if C_XR_VULKAN
	xrvk_shutdown();
#endif
#if C_XR_D3D11
	xrd11_shutdown();
#endif
#if C_XR_D3D12
	xrd12_shutdown();
#endif
}
// Master switch for the headset side of XR: session bring-up,
// per-frame submission, controller input, and overlay layers all key
// off the live [xr] xr_enabled value (plus the OpenGL backend the XR
// graphics binding needs). Called at startup, from the [xr] section
// update handler (DOS `config -set`), and directly by the VR-menu
// toggle, the mapper hotkey, and XRCFG preset loads (programmatic
// property sets don't run section update handlers).
void openxr_set_enabled(const bool on)
{
	auto* section = get_xr_section();
	if (section->GetBool("xr_enabled") != on) {
		// Keeps the stored value (and later XRCFG saves) in sync
		// with the actual session state. Programmatic sets don't
		// run update handlers, so this can't recurse.
		set_section_property_value("xr", "xr_enabled", on ? "true" : "false");
	}
	if (on) {
#if C_XR_VULKAN
		if (sdl.render_backend_type == RenderBackendType::Vulkan &&
		    !xrvk_available()) {
			LOG_ERR("OpenXR: Headset output on 'output=vulkan' needs "
			        "a Vulkan driver installed; desktop rendering "
			        "continues without the headset");
			return;
		}
#else
		if (sdl.render_backend_type == RenderBackendType::Vulkan) {
			LOG_ERR("OpenXR: Vulkan binding not built (no Vulkan "
			        "headers at compile time); use 'output=opengl' "
			        "for the headset");
			return;
		}
#endif
		if (sdl.render_backend_type != RenderBackendType::OpenGl &&
		    sdl.render_backend_type != RenderBackendType::Vulkan &&
		    sdl.render_backend_type != RenderBackendType::D3D11 &&
		    sdl.render_backend_type != RenderBackendType::D3D12) {
			LOG_ERR("OpenXR: Headset output needs 'output=opengl', "
			        "'output=vulkan', 'output=d3d11', or "
			        "'output=d3d12'; desktop rendering continues "
			        "without the headset");
			return;
		}
		if (sdl_xr_instance != XR_NULL_HANDLE) {
			return; // already up; recovery runs in the frame loop
		}
		// Fresh attempt (also after a give-up): clear the
		// bring-up failure state so recovery retries immediately.
		sdl_xr_disabled    = false;
		sdl_xr_init_fails  = 0;
		sdl_xr_retry_at_us = GetTicksUs();
		init_openxr();
	} else {
		if (sdl_xr_instance == XR_NULL_HANDLE) {
			return; // already down
		}
		sdl_xr_init_fails = 0;
		cleanup_openxr();
		LOG_MSG("OpenXR: Headset output disabled; desktop rendering continues");
	}
}

// Live [xr] key changes (DOS `config -set xr ...`): only xr_enabled
// needs action, the rest is polled every frame.
void notify_xr_setting_updated(SectionProp& section, const std::string& prop_name)
{
	if (prop_name == "xr_enabled") {
		openxr_set_enabled(section.GetBool("xr_enabled"));
	}
}

void init_xr_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto pbool_xr_on = section.AddBool("xr_enabled", Always, false);
	pbool_xr_on->SetHelp(
	        "Enable the VR headset output (OpenXR session, headset layers,\n"
	        "and controller input). Works alongside any [sdl] 'output' backend\n"
	        "for the desktop window; the headset itself needs 'output=opengl',\n"
	        "'output=vulkan', 'output=d3d11', or 'output=d3d12' (other backends\n"
	        "keep rendering the desktop, with a log notice). Off by default.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.\n"
	        "The VR-menu 'XR enabled' row and the 'XR on/off' mapper hotkey\n"
	        "toggle it live.");

	auto pint_xr = section.AddInt("xr_submit_divisor", Always, 1);
	pint_xr->SetHelp(
	        "Submit every Nth frame to the VR headset when XR is enabled\n"
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
        "  cybermaxx:   Scanline-interleaved decode for VictorMaxx\n"
        "                CyberMaxx titles: the signal weaves the two eye\n"
        "                views into alternate scanlines (line N to one\n"
        "                panel, line N+1 to the other at the 15.734 kHz\n"
        "                line rate), so each eye is one half-height field\n"
        "                (line-doubled captures are detected and\n"
        "                de-doubled first). Static content coincides\n"
        "                in both eyes; motion shows the field offset as the\n"
        "                headset panels would.\n"
        "\n"
        "  iglasses:     Scanline-interleaved decode for Virtual i-O\n"
        "                i-glasses! titles: one non-interlaced frame\n"
        "                carries two eye views on alternate scanlines\n"
        "                (first scanline is the left eye: 320x400 holds\n"
        "                two 320x200 views), so each eye is one\n"
        "                half-height field (line-doubled captures are\n"
        "                detected and de-doubled first).\n"
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
                       "cybermaxx",
                       "iglasses"});

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
	        "Note: Can be changed while running; takes effect on the next frame.\n"
	        "The Ctrl+Alt+E shortcut cycles the methods live.");
	pstring_xr_auto->SetValues({"blend",
	                             "gradient",
	                             "luma"
	                             });

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

	auto pint_xr_scr_x = section.AddInt("xr_screen_offset_x", Always, 0);
	pint_xr_scr_x->SetHelp(
	        "Horizontal position of the VR screen panel as a percentage\n"
	        "of the screen half-width ('0' by default). Positive moves\n"
	        "right. Also adjusted live by gripping the screen with the\n"
	        "left controller. Valid range from -100 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_scr_x->SetMinMax(-100, 100);

	auto pint_xr_scr_y = section.AddInt("xr_screen_offset_y", Always, 0);
	pint_xr_scr_y->SetHelp(
	        "Vertical position of the VR screen panel as a percentage\n"
	        "of the screen half-height ('0' by default). Positive moves\n"
	        "up. Also adjusted live by gripping the screen with the left\n"
	        "controller. Valid range from -100 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_scr_y->SetMinMax(-100, 100);

	auto pint_xr_scr_zoom = section.AddInt("xr_screen_zoom", Always, 100);
	pint_xr_scr_zoom->SetHelp(
	        "Size of the VR screen panel as a percentage of its default\n"
	        "size ('100' by default). Resizing the screen also moves the\n"
	        "keyboard and menu with it. Also adjusted live by gripping\n"
	        "the screen with the right controller. Valid range from 25\n"
	        "to 300.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_scr_zoom->SetMinMax(25, 300);

	auto pbool_xr_scr_panel = section.AddBool("xr_screen_panel",
	                                          Always,
	                                          true);
	pbool_xr_scr_panel->SetHelp(
	        "Render the DOS picture as a floating panel in 3D space\n"
	        "('on' by default) so it can be moved and resized with the\n"
	        "controllers. When off, the picture fills the headset views\n"
	        "as before. Split and interleaved stereo modes always use the\n"
	        "full-view projection.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

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

	auto pbool_xr_panel_grab = section.AddBool("xr_panel_grab",
	                                           Always,
	                                           true);
	pbool_xr_panel_grab->SetHelp(
	        "Grab and reposition VR panels with the controller grips\n"
	        "('on' by default). Point at the screen, overlay keyboard, or\n"
	        "menu and hold the left grip to move it, or the right grip to\n"
	        "resize it. The new placement is saved to this config on\n"
	        "release. While the controller-aim mouse is on, the right\n"
	        "grip recenters the mouse instead of resizing (the current\n"
	        "aim becomes the screen centre). When off, the grips do\n"
	        "nothing.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pint_xr_mapper_scale = section.AddInt("xr_mapper_scale",
	                                          Always,
	                                          60);
	pint_xr_mapper_scale->SetHelp(
	        "Size of the mapper panel in the VR headset as a percentage\n"
	        "of the screen plane ('60' by default). Valid range from 20\n"
	        "to 100. Lower it if the mapper fills too much of the view.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_mapper_scale->SetMinMax(20, 100);

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

	// NOTE: the VFX-1 VIP keys used to live here (xr_vip_respond,
	// xr_vip_irq, xr_log_vip, xr_log_vip_full); they moved to the
	// [vfx1] section (vfx1_enable/vfx1_respond/vfx1_irq/vfx1_log/
	// vfx1_log_all) with a master switch. The emulator logs a
	// one-shot notice if the old keys are still set.

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

	auto pint_xr_menu_x = section.AddInt("xr_menu_offset_x", Always, 0);
	pint_xr_menu_x->SetHelp(
	        "Horizontal position offset of the in-headset VR menu as a\n"
	        "percentage of the screen half-width ('0' by default).\n"
	        "Positive moves right, negative left. Pointing and trigger\n"
	        "clicks follow the menu. Also adjusted live by gripping the\n"
	        "menu with the left controller. Valid range from -100 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_menu_x->SetMinMax(-100, 100);

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

	auto pint_xr_menu_cols = section.AddInt("xr_menu_columns", Always, 0);
	pint_xr_menu_cols->SetHelp(
	        "Number of columns the in-headset VR menu uses to lay its\n"
	        "sections out as a grid of cards ('0' by default). 0 = auto:\n"
	        "two columns, or one when the menu has a single section. 1\n"
	        "stacks every section in a single column; 2 or 3 force that\n"
	        "many columns (clamped to the section count). The desktop\n"
	        "mirror menu honours the same setting, except its auto value\n"
	        "is one column (that panel is tall and narrow). Valid range\n"
	        "from 0 to 3.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_menu_cols->SetMinMax(0, 3);

	auto pbool_xr_motion = section.AddBool("xr_motion_input", Always, true);
	pbool_xr_motion->SetHelp(
	        "Feed the left motion-controller thumbstick into the emulated\n"
	        "DOS inputs ('on' by default). When off, the left stick is\n"
	        "inert no matter which left-wheel mode is selected, handy if\n"
	        "the stick drifts or you only want the right hand active.\n"
	        "Even when on, the stick only reaches DOS while the left grip\n"
	        "is held (a dead-man hold): releasing the grip centres the\n"
	        "axis and releases the stick's buttons and preset keys.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

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
	                                  Deprecated,
	                                  25);
	pint_xr_hold->SetHelp(
	        "Deprecated and unused. Emulated mouse buttons now follow the\n"
	        "controller triggers directly (hold = down, release = up), so\n"
	        "a minimum press length is no longer applied. Kept so old\n"
	        "config files keep loading.\n"
	        "\n"
	        "Note: No effect.");
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

	section.AddUpdateHandler(notify_xr_setting_updated);
}
#endif // C_OPENXR
