// SPDX-FileCopyrightText:  2020-2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "private/sdl_gui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sys/types.h>
#include <unistd.h>

#if C_OPENXR
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
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

#if C_DEBUGGER
#include "debugger/debugger.h"
#include "debugger/debugger_inc.h"
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

// #define DEBUG_WINDOW_EVENTS

template <typename... Args>
void log_window_event([[maybe_unused]] const char* message,
                      [[maybe_unused]] const Args&... args) noexcept
{
#ifdef DEBUG_WINDOW_EVENTS
	LOG_DEBUG(message, args...);
#endif
}

SDL_Block sdl;

#if C_OPENXR
// OpenXR state
static XrInstance              sdl_xr_instance      = XR_NULL_HANDLE;
static XrSystemId              sdl_xr_systemId      = XR_NULL_SYSTEM_ID;
static XrSession               sdl_xr_session       = XR_NULL_HANDLE;
static XrSpace                 sdl_xr_app_space     = XR_NULL_HANDLE;
static XrSwapchain             sdl_xr_swapchain     = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_swapchain_images;
// Dedicated swapchain for the overlay-keyboard quad layer (static
// content, filled once) and its enumerated images.
static XrSwapchain             sdl_xr_kbd_swapchain = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_kbd_images;
static constexpr int OpenXrKeyboardTexW = 1000;
static constexpr int OpenXrKeyboardTexH = 500;
// Notification overlay: dedicated swapchain, texture size, per-message
// lifetime, and the last few messages (newest last).
static XrSwapchain             sdl_xr_osd_swapchain = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageOpenGLKHR> sdl_xr_osd_images;
static constexpr int OpenXrOsdTexW = 1024;
static constexpr int OpenXrOsdTexH = 256;
static constexpr int64_t OpenXrOsdTimeoutUs = 4'000'000; // 4 s
struct XrOsdLine {
	std::string text;
	int64_t     time_us = 0;
};
static std::deque<XrOsdLine> sdl_xr_osd;
// Overlay content lifecycle: setup only creates/enumerates the
// keyboard/OSD swapchains; pixels go in lazily from the first frame
// that needs them (strict runtimes reject acquire/release outside a
// running wait/begin frame).
static bool sdl_xr_kbd_filled = false;
static bool sdl_xr_osd_dirty  = true;
// Hover key last seen (-1 = none, -2 = nothing seen yet); the texture
// epoch bumps on change so designated images refresh.
static int sdl_xr_kbd_hover_built = -2;
// Trigger-held key last seen (same encoding); its letter lights up.
static int sdl_xr_kbd_pressed_built = -2;
// Current keyboard swapchain dims (follow xr_keyboard_key_scale;
// swapchain is rebuilt live when they no longer match).
static int sdl_xr_kbd_tex_w = 0;
static int sdl_xr_kbd_tex_h = 0;
// Fresh-by-construction overlays: the image handed to the compositor
// is copied at designate time whenever its epoch lags the texture
// epoch (bulk fills can't rely on acquire order within a frame, so a
// same-image acquire loop can leave stale outlines behind).
static GLuint sdl_xr_kbd_tex = 0;
static uint64_t sdl_xr_kbd_tex_epoch = 1;
static std::vector<uint64_t> sdl_xr_kbd_img_epoch;
static GLuint sdl_xr_osd_tex = 0;
static uint64_t sdl_xr_osd_tex_epoch = 1;
static std::vector<uint64_t> sdl_xr_osd_img_epoch;
// Retained per-eye images (32-bit BGRA) shared with the Voodoo depth
// hook below. Filled either by the stereo decoders in the blit path or
// by XR_VoodooDepthFrame; dimensions tracked alongside.
static std::vector<uint32_t> xr_eye_buf[2];
static int xr_eye_buf_w = 0, xr_eye_buf_h = 0;
// Set by the Voodoo depth hook when it fills the buffers; consumed
// (cleared) by the next depth-mode blit. Stale buffers read as mono.
static bool sdl_xr_depth_valid = false;
// Scratch maps for depth synthesis: estimated (autodepth) normalized
// depth plus the Voodoo path's converted color/normalized depth.
static std::vector<float> xr_autodepth_map;
static int xr_autodepth_map_w = 0, xr_autodepth_map_h = 0;
static std::vector<uint32_t> xr_voodoo_argb;
static std::vector<float> xr_voodoo_norm;
static int xr_voodoo_buf_w = 0, xr_voodoo_buf_h = 0;
// Bounded compositor wait shared by the swapchain acquire paths: an
// unresponsive compositor must stall the emulator thread for at most
// this long (Meta runtimes reject release-before-wait outright).
static constexpr XrDuration OpenXrSwapchainWaitTimeoutNs = 10'000'000; // 10 ms
static int64_t                 sdl_xr_swapchain_format = 0;
static uint32_t                sdl_xr_swapchain_width  = 0;
static uint32_t                sdl_xr_swapchain_height = 0;
static uint32_t                sdl_xr_swapchain_index  = 0;
// Index of the acquired swapchain image the mirror blit targets, or
// -1 when no XR frame is in flight.
static int                     sdl_xr_active_image     = -1;
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
static uint64_t sdl_xr_stat_mirror_us    = 0;
static uint64_t sdl_xr_stat_blit_us      = 0;
static uint64_t sdl_xr_stat_submit_us    = 0;
static int64_t  sdl_xr_frame_t0_us       = 0;
// World-locked screen anchor: when locked, the submitted layer keeps
// this pose instead of following the headset, so the picture stays
// fixed in the room while you move.
static bool    sdl_xr_world_locked = false;
static bool    sdl_xr_has_lock     = false;
static XrPosef sdl_xr_locked_pose[2] = {};
static XrFovf  sdl_xr_locked_fov[2]  = {};
// Diagnostic: submit empty frames (no layers) to isolate compositor
// cost of our layer content vs. submit pacing overhead.
static bool    sdl_xr_submit_blank = false;
// Graceful-loss handling: last failure (for once-per-cause logging),
// next retry time, and retry cooldown for hot-plug/reconnect.
static XrResult  sdl_xr_last_error  = XR_SUCCESS;
static int64_t   sdl_xr_retry_at_us = 0;
static constexpr int64_t OpenXrRetryCooldownUs = 10'000'000; // 10 s
// Fallback bookkeeping: stop trying XR for this run after repeated
// bring-up failures that never produced a working session. A session
// that ran at least once keeps retrying forever (likely transient).
static bool      sdl_xr_disabled    = false;
static bool      sdl_xr_ever_ran    = false;
static int       sdl_xr_init_fails  = 0;
static constexpr int OpenXrMaxInitAttempts = 6;
static XrView                  sdl_xr_views[2]      = {};
static XrViewConfigurationView sdl_xr_config_views[2] = {};
static XrViewConfigurationType sdl_xr_view_config_type =
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
static XrFovf                  sdl_xr_eye_fov[2]    = {};
static float                   sdl_xr_proj_mats[2][16] = {};
static XrCompositionLayerProjection     sdl_xr_proj_layer       = {};
static XrCompositionLayerProjectionView sdl_xr_proj_layer_views[2] = {};
static bool                    sdl_xr_initialized   = false;
static bool                    sdl_xr_session_ready = false;
static XrSessionState          sdl_xr_session_state = XR_SESSION_STATE_UNKNOWN;
static uint32_t                sdl_xr_view_count    = 0;

// Head pose proxy (filled by openxr_update_head_tracking, below).
static XrPosef sdl_xr_head_pose       = {};
static bool    sdl_xr_head_pose_valid = false;

// Forward declarations for the loss-handling helpers defined below
// (needed by the session/frame functions above them).
static void cleanup_openxr();
static void openxr_teardown_session();
static bool openxr_note_failure(const char* what, const XrResult result);

static constexpr float OpenXrNearZ = 0.1f;
static constexpr float OpenXrFarZ  = 100.0f;

static SectionProp* get_xr_section()
{
	auto section = get_section("xr");
	assert(section);
	return section;
}

// Key-cell scale: texture resolution and panel size grow with it so
// keys stay crisp. Returns the multiplier (1.0 = 100%).
static float openxr_keyboard_key_scale()
{
	return std::clamp(get_xr_section()->GetInt("xr_keyboard_key_scale") /
	                          100.0f,
	                  0.5f,
	                  2.0f);
}

// Runtime keyboard texture dims (base 1000x500 scaled by key scale).
static void openxr_keyboard_tex_size(int& w_out, int& h_out)
{
	const float s = openxr_keyboard_key_scale();
	w_out = static_cast<int>(OpenXrKeyboardTexW * s + 0.5f);
	h_out = static_cast<int>(OpenXrKeyboardTexH * s + 0.5f);
}

// Log + show an in-headset notification (defined with the OSD machinery
// below, called by the mapper handlers further down).
void OPENXR_Notify(const std::string& message);

// ---------------------------------------------------------------------------
// Motion controllers, emulated-mouse control, and overlay keyboard
// ---------------------------------------------------------------------------
// Hand index convention throughout: 0 = left, 1 = right.
static XrActionSet sdl_xr_action_set   = XR_NULL_HANDLE;
static XrAction    sdl_xr_aim_action   = XR_NULL_HANDLE; // pose input
static XrAction    sdl_xr_click_action = XR_NULL_HANDLE; // boolean input
static XrAction    sdl_xr_grip_action  = XR_NULL_HANDLE; // float input
static XrAction    sdl_xr_stick_action = XR_NULL_HANDLE; // vec2 left thumbstick
static XrAction    sdl_xr_joya_action  = XR_NULL_HANDLE; // boolean right A
static XrAction    sdl_xr_joyb_action  = XR_NULL_HANDLE; // boolean right B
static XrSpace     sdl_xr_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static XrPath      sdl_xr_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
static bool        sdl_xr_actions_attached = false;

struct XrHandState {
	bool     pose_valid       = false;
	XrPosef  pose             = {};
	bool     click            = false;
	bool     click_changed    = false;
	float    grip             = 0.0f;
	bool     grip_pressed     = false;
	bool     grip_was_pressed = false;
};
static int sdl_xr_input_mode = 2; // 0=keyboard, 1=mouse, 2=neither (off)
static XrHandState sdl_xr_hand[2] = {};

// Emulated DOS joystick (stick 1) via the left controller, cycled by
// left-grip press: 0=analog, 1=hat (8-way digital), 2=off.
static int  sdl_xr_joy_mode       = 2;
static float sdl_xr_joy_x         = 0.0f;
static float sdl_xr_joy_y         = 0.0f;
static bool  sdl_xr_joy_btn[2]    = {false, false}; // right A, B

// Emulated-mouse control via the right controller (toggle).
static bool  sdl_xr_controller_mouse      = false;
static bool  sdl_xr_mouse_has_last       = false;
static float sdl_xr_mouse_last_x         = 0.0f;
static float sdl_xr_mouse_last_y         = 0.0f;
static bool  sdl_xr_mouse_button_held[2] = {false, false}; // L, R

// Overlay keyboard state (toggle). Rendering the key caps is a later
// step; layout, hit-testing, and key injection work now.
static bool sdl_xr_keyboard_visible = false;
static int  sdl_xr_keyboard_pressed = -1; // key index held via trigger

struct XrVec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

static XrVec3 xr_quat_rotate(const XrQuaternionf& q, const XrVec3& v)
{
	// v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
	const XrVec3 qv = {q.x, q.y, q.z};
	XrVec3 t = {qv.y * v.z - qv.z * v.y + q.w * v.x,
	            qv.z * v.x - qv.x * v.z + q.w * v.y,
	            qv.x * v.y - qv.y * v.x + q.w * v.z};
	XrVec3 out = {v.x + 2.0f * (qv.y * t.z - qv.z * t.y),
	              v.y + 2.0f * (qv.z * t.x - qv.x * t.z),
	              v.z + 2.0f * (qv.x * t.y - qv.y * t.x)};
	return out;
}

static float xr_dot(const XrVec3& a, const XrVec3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static bool openxr_create_input_actions()
{
	if (sdl_xr_instance == XR_NULL_HANDLE) {
		return false;
	}
	if (sdl_xr_action_set != XR_NULL_HANDLE) {
		return true; // already set up (e.g. re-init)
	}

	XrActionSetCreateInfo set_info = {};
	set_info.type     = XR_TYPE_ACTION_SET_CREATE_INFO;
	set_info.priority = 0;
	snprintf(set_info.actionSetName,
	         sizeof(set_info.actionSetName),
	         "dosbox_xr");
	snprintf(set_info.localizedActionSetName,
	         sizeof(set_info.localizedActionSetName),
	         "DOSBox Staging XR input");
	if (xrCreateActionSet(sdl_xr_instance, &set_info, &sdl_xr_action_set) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: xrCreateActionSet failed");
		sdl_xr_action_set = XR_NULL_HANDLE;
		return false;
	}

	if (xrStringToPath(sdl_xr_instance,
	                   "/user/hand/left",
	                   &sdl_xr_hand_path[0]) != XR_SUCCESS ||
	    xrStringToPath(sdl_xr_instance,
	                   "/user/hand/right",
	                   &sdl_xr_hand_path[1]) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to resolve hand subaction paths");
		return false;
	}

	XrActionCreateInfo aim_info = {};
	aim_info.type                 = XR_TYPE_ACTION_CREATE_INFO;
	aim_info.actionType           = XR_ACTION_TYPE_POSE_INPUT;
	aim_info.countSubactionPaths  = 2;
	aim_info.subactionPaths       = sdl_xr_hand_path;
	snprintf(aim_info.actionName, sizeof(aim_info.actionName), "aim");
	snprintf(aim_info.localizedActionName,
	         sizeof(aim_info.localizedActionName),
	         "Aim pose");
	if (xrCreateAction(sdl_xr_action_set, &aim_info, &sdl_xr_aim_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create aim action");
		return false;
	}

	XrActionCreateInfo click_info = {};
	click_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	click_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
	click_info.countSubactionPaths = 2;
	click_info.subactionPaths      = sdl_xr_hand_path;
	snprintf(click_info.actionName, sizeof(click_info.actionName), "click");
	snprintf(click_info.localizedActionName,
	         sizeof(click_info.localizedActionName),
	         "Trigger click");
	if (xrCreateAction(sdl_xr_action_set, &click_info, &sdl_xr_click_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create click action");
		return false;
	}

	XrActionCreateInfo grip_info = {};
	grip_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	grip_info.actionType          = XR_ACTION_TYPE_FLOAT_INPUT;
	grip_info.countSubactionPaths = 2;
	grip_info.subactionPaths      = sdl_xr_hand_path;
	snprintf(grip_info.actionName, sizeof(grip_info.actionName), "grip");
	snprintf(grip_info.localizedActionName,
	         sizeof(grip_info.localizedActionName),
	         "Grip squeeze");
	if (xrCreateAction(sdl_xr_action_set, &grip_info, &sdl_xr_grip_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create grip action");
		sdl_xr_grip_action = XR_NULL_HANDLE;
		// Non-fatal: session runs, grip cycle just stays unavailable.
	}

	// Left thumbstick (emulated DOS joystick axes). Non-fatal like grip.
	XrActionCreateInfo stick_info = {};
	stick_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	stick_info.actionType          = XR_ACTION_TYPE_VECTOR2F_INPUT;
	stick_info.countSubactionPaths = 1;
	stick_info.subactionPaths      = &sdl_xr_hand_path[0];
	snprintf(stick_info.actionName, sizeof(stick_info.actionName), "stick");
	snprintf(stick_info.localizedActionName,
	         sizeof(stick_info.localizedActionName),
	         "Left thumbstick");
	if (xrCreateAction(sdl_xr_action_set, &stick_info, &sdl_xr_stick_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create stick action");
		sdl_xr_stick_action = XR_NULL_HANDLE;
	}

	// Right A/B buttons (emulated DOS joystick buttons 1/2). Non-fatal.
	XrActionCreateInfo joya_info = {};
	joya_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	joya_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
	joya_info.countSubactionPaths = 1;
	joya_info.subactionPaths      = &sdl_xr_hand_path[1];
	snprintf(joya_info.actionName, sizeof(joya_info.actionName), "joya");
	snprintf(joya_info.localizedActionName,
	         sizeof(joya_info.localizedActionName),
	         "Right A button");
	if (xrCreateAction(sdl_xr_action_set, &joya_info, &sdl_xr_joya_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create joy-A action");
		sdl_xr_joya_action = XR_NULL_HANDLE;
	}
	XrActionCreateInfo joyb_info = {};
	joyb_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	joyb_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
	joyb_info.countSubactionPaths = 1;
	joyb_info.subactionPaths      = &sdl_xr_hand_path[1];
	snprintf(joyb_info.actionName, sizeof(joyb_info.actionName), "joyb");
	snprintf(joyb_info.localizedActionName,
	         sizeof(joyb_info.localizedActionName),
	         "Right B button");
	if (xrCreateAction(sdl_xr_action_set, &joyb_info, &sdl_xr_joyb_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create joy-B action");
		sdl_xr_joyb_action = XR_NULL_HANDLE;
	}

	// Suggested bindings per interaction profile (best effort: a
	// runtime without the profile simply ignores it).
	struct ProfileBindings {
		const char* profile;
		const char* click_path[2];
		const char* squeeze_path[2]; // nullptr where the profile has none
		const char* stick_path;   // left vec2, nullptr where none
		const char* buttona_path; // right A, nullptr where none
		const char* buttonb_path; // right B, nullptr where none
	};
	static const ProfileBindings profiles[] = {
	        {"/interaction_profiles/khr/simple_controller",
	         {"/user/hand/left/input/select/click",
	          "/user/hand/right/input/select/click"},
	         {nullptr, nullptr}, // no grip input on simple controllers
	         nullptr,
	         nullptr,
	         nullptr},
	        {"/interaction_profiles/oculus/touch_controller",
	         {"/user/hand/left/input/trigger/touch",
	          "/user/hand/right/input/trigger/touch"},
	         {"/user/hand/left/input/squeeze/value",
	          "/user/hand/right/input/squeeze/value"},
	         "/user/hand/left/input/thumbstick",
	         "/user/hand/right/input/a/click",
	         "/user/hand/right/input/b/click"},
	        {"/interaction_profiles/valve/index_controller",
	         {"/user/hand/left/input/trigger/click",
	          "/user/hand/right/input/trigger/click"},
	         {"/user/hand/left/input/squeeze/value",
	          "/user/hand/right/input/squeeze/value"},
	         "/user/hand/left/input/thumbstick",
	         "/user/hand/right/input/a/click",
	         "/user/hand/right/input/b/click"},
	        {"/interaction_profiles/microsoft/motion_controller",
	         {"/user/hand/left/input/trigger/click",
	          "/user/hand/right/input/trigger/click"},
	         {"/user/hand/left/input/squeeze/value",
	          "/user/hand/right/input/squeeze/value"},
	         "/user/hand/left/input/thumbstick",
	         nullptr, // no A button; trackpad click is button 2
	         "/user/hand/right/input/trackpad/click"},
	};
	static const char* const aim_path[2] = {
	        "/user/hand/left/input/aim/pose",
	        "/user/hand/right/input/aim/pose",
	};

	for (const auto& p : profiles) {
		XrPath profile = XR_NULL_PATH;
		if (xrStringToPath(sdl_xr_instance, p.profile, &profile) !=
		    XR_SUCCESS) {
			continue;
		}
		std::vector<XrActionSuggestedBinding> bindings;
		bool ok = true;
		for (int hand = 0; hand < 2 && ok; ++hand) {
			XrPath aim = XR_NULL_PATH, click = XR_NULL_PATH;
			ok = (xrStringToPath(sdl_xr_instance,
			                     aim_path[hand],
			                     &aim) == XR_SUCCESS) &&
			     (xrStringToPath(sdl_xr_instance,
			                     p.click_path[hand],
			                     &click) == XR_SUCCESS);
			if (ok) {
				bindings.push_back({sdl_xr_aim_action, aim});
				bindings.push_back({sdl_xr_click_action, click});
			}
			if (ok && sdl_xr_grip_action != XR_NULL_HANDLE &&
			    p.squeeze_path[hand] != nullptr) {
				XrPath squeeze = XR_NULL_PATH;
				if (xrStringToPath(sdl_xr_instance,
				                   p.squeeze_path[hand],
				                   &squeeze) == XR_SUCCESS) {
					bindings.push_back({sdl_xr_grip_action, squeeze});
				}
			}
		}
		if (ok && sdl_xr_stick_action != XR_NULL_HANDLE &&
		    p.stick_path != nullptr) {
			XrPath stick = XR_NULL_PATH;
			if (xrStringToPath(sdl_xr_instance,
			                   p.stick_path,
			                   &stick) == XR_SUCCESS) {
				bindings.push_back({sdl_xr_stick_action, stick});
			}
		}
		if (ok && sdl_xr_joya_action != XR_NULL_HANDLE &&
		    p.buttona_path != nullptr) {
			XrPath btna = XR_NULL_PATH;
			if (xrStringToPath(sdl_xr_instance,
			                   p.buttona_path,
			                   &btna) == XR_SUCCESS) {
				bindings.push_back({sdl_xr_joya_action, btna});
			}
		}
		if (ok && sdl_xr_joyb_action != XR_NULL_HANDLE &&
		    p.buttonb_path != nullptr) {
			XrPath btnb = XR_NULL_PATH;
			if (xrStringToPath(sdl_xr_instance,
			                   p.buttonb_path,
			                   &btnb) == XR_SUCCESS) {
				bindings.push_back({sdl_xr_joyb_action, btnb});
			}
		}
		if (!ok || bindings.empty()) {
			continue;
		}
		XrInteractionProfileSuggestedBinding suggested = {};
		suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
		suggested.interactionProfile      = profile;
		suggested.countSuggestedBindings  = static_cast<uint32_t>(
		        bindings.size());
		suggested.suggestedBindings       = bindings.data();
		if (xrSuggestInteractionProfileBindings(sdl_xr_instance,
		                                       &suggested) != XR_SUCCESS) {
			LOG_MSG("OpenXR: No suggested bindings for %s", p.profile);
		}
	}
	return true;
}

static bool openxr_attach_input()
{
	if (sdl_xr_session == XR_NULL_HANDLE ||
	    sdl_xr_action_set == XR_NULL_HANDLE) {
		return false;
	}
	if (sdl_xr_actions_attached) {
		return true;
	}
	XrSessionActionSetsAttachInfo attach = {};
	attach.type             = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	attach.countActionSets  = 1;
	attach.actionSets       = &sdl_xr_action_set;
	if (xrAttachSessionActionSets(sdl_xr_session, &attach) != XR_SUCCESS) {
		LOG_ERR("OpenXR: xrAttachSessionActionSets failed");
		return false;
	}
	sdl_xr_actions_attached = true;

	for (int hand = 0; hand < 2; ++hand) {
		if (sdl_xr_aim_space[hand] != XR_NULL_HANDLE) {
			continue;
		}
		XrActionSpaceCreateInfo space_info = {};
		space_info.type            = XR_TYPE_ACTION_SPACE_CREATE_INFO;
		space_info.action          = sdl_xr_aim_action;
		space_info.subactionPath   = sdl_xr_hand_path[hand];
		space_info.poseInActionSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
		space_info.poseInActionSpace.position    = {0.0f, 0.0f, 0.0f};
		if (xrCreateActionSpace(sdl_xr_session,
		                        &space_info,
		                        &sdl_xr_aim_space[hand]) != XR_SUCCESS) {
			LOG_ERR("OpenXR: Failed to create aim space for hand %d",
			        hand);
		}
	}
	return true;
}

static void openxr_update_input(const XrTime display_time)
{
	if (sdl_xr_session == XR_NULL_HANDLE || !sdl_xr_actions_attached) {
		return;
	}
	XrActiveActionSet active = {sdl_xr_action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync   = {};
	sync.type                  = XR_TYPE_ACTIONS_SYNC_INFO;
	sync.countActiveActionSets = 1;
	sync.activeActionSets      = &active;
	if (xrSyncActions(sdl_xr_session, &sync) != XR_SUCCESS) {
		return;
	}
	for (int hand = 0; hand < 2; ++hand) {
		auto& dst = sdl_xr_hand[hand];
		if (sdl_xr_aim_space[hand] != XR_NULL_HANDLE) {
			XrSpaceLocation loc = {};
			loc.type            = XR_TYPE_SPACE_LOCATION;
			if (xrLocateSpace(sdl_xr_aim_space[hand],
			                  sdl_xr_app_space,
			                  display_time,
			                  &loc) == XR_SUCCESS) {
				const bool pos_valid =
				        (loc.locationFlags &
				         XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
				const bool ori_valid =
				        (loc.locationFlags &
				         XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
				dst.pose_valid = (pos_valid && ori_valid);
				if (dst.pose_valid) {
					dst.pose = loc.pose;
				}
			} else {
				dst.pose_valid = false;
			}
		}
		XrActionStateGetInfo get_info = {};
		get_info.type          = XR_TYPE_ACTION_STATE_GET_INFO;
		get_info.action        = sdl_xr_click_action;
		get_info.subactionPath = sdl_xr_hand_path[hand];
		XrActionStateBoolean state = {};
		state.type                 = XR_TYPE_ACTION_STATE_BOOLEAN;
		if (xrGetActionStateBoolean(sdl_xr_session,
		                            &get_info,
		                            &state) == XR_SUCCESS &&
		    state.isActive) {
			dst.click_changed = (state.currentState != dst.click);
			dst.click         = state.currentState;
		} else {
			dst.click_changed = false;
		}
		if (sdl_xr_grip_action != XR_NULL_HANDLE) {
			XrActionStateGetInfo grip_get = {};
			grip_get.type          = XR_TYPE_ACTION_STATE_GET_INFO;
			grip_get.action        = sdl_xr_grip_action;
			grip_get.subactionPath = sdl_xr_hand_path[hand];
			XrActionStateFloat grip_state = {};
			grip_state.type               = XR_TYPE_ACTION_STATE_FLOAT;
			if (xrGetActionStateFloat(sdl_xr_session,
			                          &grip_get,
			                          &grip_state) == XR_SUCCESS &&
			    grip_state.isActive) {
				dst.grip = grip_state.currentState;
			}
		}
		const bool pressed_now = (dst.grip > 0.5f);
		dst.grip_pressed       = (pressed_now && !dst.grip_was_pressed);
		dst.grip_was_pressed   = pressed_now;
	}
	// Left thumbstick + right A/B for the emulated DOS joystick.
	if (sdl_xr_stick_action != XR_NULL_HANDLE) {
		XrActionStateGetInfo stick_get = {};
		stick_get.type          = XR_TYPE_ACTION_STATE_GET_INFO;
		stick_get.action        = sdl_xr_stick_action;
		stick_get.subactionPath = sdl_xr_hand_path[0];
		XrActionStateVector2f stick_state = {};
		stick_state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
		if (xrGetActionStateVector2f(sdl_xr_session,
		                            &stick_get,
		                            &stick_state) == XR_SUCCESS &&
		    stick_state.isActive) {
			sdl_xr_joy_x = stick_state.currentState.x;
			sdl_xr_joy_y = stick_state.currentState.y;
		}
	}
	for (int b = 0; b < 2; ++b) {
		const XrAction btn_action =
		        (b == 0) ? sdl_xr_joya_action : sdl_xr_joyb_action;
		if (btn_action == XR_NULL_HANDLE) {
			continue;
		}
		XrActionStateGetInfo btn_get = {};
		btn_get.type          = XR_TYPE_ACTION_STATE_GET_INFO;
		btn_get.action        = btn_action;
		btn_get.subactionPath = sdl_xr_hand_path[1];
		XrActionStateBoolean btn_state = {};
		btn_state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
		if (xrGetActionStateBoolean(sdl_xr_session,
		                            &btn_get,
		                            &btn_state) == XR_SUCCESS &&
		    btn_state.isActive) {
			sdl_xr_joy_btn[b] = btn_state.currentState;
		}
	}
}

// Screen anchor for pointer mapping: the world-locked anchor when
// locked, otherwise the live head pose.
static bool openxr_screen_anchor(XrPosef& anchor_out)
{
	if (sdl_xr_world_locked && sdl_xr_has_lock) {
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
static bool openxr_screen_plane(XrPosef& anchor_out, float& half_w_out,
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

// Intersect a hand's aim ray with the virtual screen plane. Returns
// ray origin/direction, hit point, and plane UV (u right, v up, 0..1),
// or false when unusable.
static bool openxr_hand_ray(const int hand, XrVec3& from_out, XrVec3& dir_out,
                            XrVec3& hit_out, float& u_out, float& v_out)
{
	if (hand < 0 || hand > 1 || !sdl_xr_hand[hand].pose_valid) {
		return false;
	}
	XrPosef anchor = {};
	float half_w = 0.0f, half_h = 0.0f, dist_m = 0.0f;
	if (!openxr_screen_plane(anchor, half_w, half_h, dist_m)) {
		return false;
	}

	const XrVec3 fwd = xr_quat_rotate(anchor.orientation, {0.0f, 0.0f, -1.0f});
	const XrVec3 right = xr_quat_rotate(anchor.orientation, {1.0f, 0.0f, 0.0f});
	const XrVec3 up    = xr_quat_rotate(anchor.orientation, {0.0f, 1.0f, 0.0f});
	const XrVec3 center = {anchor.position.x + fwd.x * dist_m,
	                       anchor.position.y + fwd.y * dist_m,
	                       anchor.position.z + fwd.z * dist_m};

	const auto& hp  = sdl_xr_hand[hand].pose;
	const XrVec3 o = {hp.position.x, hp.position.y, hp.position.z};
	const XrVec3 dir = xr_quat_rotate(hp.orientation, {0.0f, 0.0f, -1.0f});
	const XrVec3 n   = {-fwd.x, -fwd.y, -fwd.z};

	const float denom = xr_dot(dir, n);
	if (denom > -1e-6f && denom < 1e-6f) {
		return false; // ray parallel to the plane
	}
	const XrVec3 diff = {center.x - o.x, center.y - o.y, center.z - o.z};
	const float t     = xr_dot(diff, n) / denom;
	if (t <= 0.0f) {
		return false; // plane is behind the controller
	}
	const XrVec3 hit = {o.x + dir.x * t, o.y + dir.y * t, o.z + dir.z * t};
	const XrVec3 rel = {hit.x - center.x, hit.y - center.y, hit.z - center.z};

	from_out = o;
	dir_out  = dir;
	hit_out  = hit;
	u_out = xr_dot(rel, right) / half_w * 0.5f + 0.5f;
	v_out = xr_dot(rel, up) / half_h * 0.5f + 0.5f;
	return true;
}

static bool openxr_hand_plane_uv(const int hand, float& u_out, float& v_out)
{
	XrVec3 from = {}, dir = {}, hit = {};
	return openxr_hand_ray(hand, from, dir, hit, u_out, v_out);
}

// Column-major helpers for the pointer-ray transform.
static void xr_quat_to_mat3(const XrQuaternionf& q, float m[9])
{
	const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	m[0] = 1.0f - 2.0f * (yy + zz);
	m[1] = 2.0f * (xy + wz);
	m[2] = 2.0f * (xz - wy);
	m[3] = 2.0f * (xy - wz);
	m[4] = 1.0f - 2.0f * (xx + zz);
	m[5] = 2.0f * (yz + wx);
	m[6] = 2.0f * (xz + wy);
	m[7] = 2.0f * (yz - wx);
	m[8] = 1.0f - 2.0f * (xx + yy);
}

static void xr_mul44(const float a[16], const float b[16], float out[16])
{
	// out = a * b, column-major.
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
			                 a[8 + r] * b[c * 4 + 2] +
			                 a[12 + r] * b[c * 4 + 3];
		}
	}
}

// Model-view-projection for an eye: projection from the current FOV,
// view as the inverse of the submitted eye pose (locked anchor when
// world-locked, live pose otherwise).
static void openxr_ray_mvp(const uint32_t eye, float out_mvp[16])
{
	const bool use_lock = (sdl_xr_world_locked && sdl_xr_has_lock);
	const XrPosef& pose =
	        use_lock ? sdl_xr_locked_pose[eye] : sdl_xr_views[eye].pose;

	float rot[9] = {};
	xr_quat_to_mat3(pose.orientation, rot);
	// Inverse rigid transform, column-major view matrix.
	float view[16] = {};
	view[0]        = rot[0];
	view[1]        = rot[3];
	view[2]        = rot[6];
	view[3]        = 0.0f;
	view[4]        = rot[1];
	view[5]        = rot[4];
	view[6]        = rot[7];
	view[7]        = 0.0f;
	view[8]        = rot[2];
	view[9]        = rot[5];
	view[10]       = rot[8];
	view[11]       = 0.0f;
	view[12]       = -(rot[0] * pose.position.x + rot[3] * pose.position.y +
	                   rot[6] * pose.position.z);
	view[13]       = -(rot[1] * pose.position.x + rot[4] * pose.position.y +
	                   rot[7] * pose.position.z);
	view[14]       = -(rot[2] * pose.position.x + rot[5] * pose.position.y +
	                   rot[8] * pose.position.z);
	view[15]       = 1.0f;

	xr_mul44(sdl_xr_proj_mats[eye], view, out_mvp);
}

static void openxr_release_mouse_buttons()
{
	if (sdl_xr_mouse_button_held[0]) {
		MOUSE_EventButton(MouseButtonId::Left, false);
		sdl_xr_mouse_button_held[0] = false;
	}
	if (sdl_xr_mouse_button_held[1]) {
		MOUSE_EventButton(MouseButtonId::Right, false);
		sdl_xr_mouse_button_held[1] = false;
	}
	sdl_xr_mouse_has_last = false;
}

static void openxr_drive_mouse()
{
	if (!sdl_xr_controller_mouse) {
		return;
	}
	// Buttons follow trigger edges even when the ray misses the screen.
	if (sdl_xr_hand[1].click_changed) {
		MOUSE_EventButton(MouseButtonId::Left, sdl_xr_hand[1].click);
		sdl_xr_mouse_button_held[0] = sdl_xr_hand[1].click;
	}
	if (sdl_xr_hand[0].click_changed) {
		MOUSE_EventButton(MouseButtonId::Right, sdl_xr_hand[0].click);
		sdl_xr_mouse_button_held[1] = sdl_xr_hand[0].click;
	}

	float u = 0.0f, v = 0.0f;
	if (!openxr_hand_plane_uv(1, u, v)) {
		sdl_xr_mouse_has_last = false;
		return;
	}
	if (u < 0.0f) {
		u = 0.0f;
	}
	if (u > 1.0f) {
		u = 1.0f;
	}
	if (v < 0.0f) {
		v = 0.0f;
	}
	if (v > 1.0f) {
		v = 1.0f;
	}
	const auto& r  = sdl.draw.draw_rect_px;
	const float ax = r.x + u * r.w;
	const float ay = r.y + (1.0f - v) * r.h;
	if (sdl_xr_mouse_has_last) {
		MOUSE_EventMoved(ax - sdl_xr_mouse_last_x,
		                 ay - sdl_xr_mouse_last_y,
		                 ax,
		                 ay);
	}
	sdl_xr_mouse_last_x   = ax;
	sdl_xr_mouse_last_y   = ay;
	sdl_xr_mouse_has_last = true;
}

// Overlay keyboard layout: label, emulated key, row, width units.
// 12 units per row; the overlay covers the bottom 45% of the screen
// plane (v measured upward from the bottom).
struct XrKeyDef {
	const char* label;
	KBD_KEYS    code;
	uint8_t     row;
	uint8_t     units;
};

static constexpr XrKeyDef xr_key_defs[] = {
        {"Esc", KBD_esc, 0, 2},
        {"1", KBD_1, 0, 1}, {"2", KBD_2, 0, 1}, {"3", KBD_3, 0, 1},
        {"4", KBD_4, 0, 1}, {"5", KBD_5, 0, 1}, {"6", KBD_6, 0, 1},
        {"7", KBD_7, 0, 1}, {"8", KBD_8, 0, 1}, {"9", KBD_9, 0, 1},
        {"0", KBD_0, 0, 1},
        {"Tab", KBD_tab, 1, 2},
        {"q", KBD_q, 1, 1}, {"w", KBD_w, 1, 1}, {"e", KBD_e, 1, 1},
        {"r", KBD_r, 1, 1}, {"t", KBD_t, 1, 1}, {"y", KBD_y, 1, 1},
        {"u", KBD_u, 1, 1}, {"i", KBD_i, 1, 1}, {"o", KBD_o, 1, 1},
        {"p", KBD_p, 1, 1},
        {"Ctrl", KBD_leftctrl, 2, 2},
        {"a", KBD_a, 2, 1}, {"s", KBD_s, 2, 1}, {"d", KBD_d, 2, 1},
        {"f", KBD_f, 2, 1}, {"g", KBD_g, 2, 1}, {"h", KBD_h, 2, 1},
        {"j", KBD_j, 2, 1}, {"k", KBD_k, 2, 1}, {"l", KBD_l, 2, 1},
        {"Bksp", KBD_backspace, 2, 1},
        {"Alt", KBD_leftalt, 3, 2},
        {"z", KBD_z, 3, 1}, {"x", KBD_x, 3, 1},
        {"c", KBD_c, 3, 1}, {"v", KBD_v, 3, 1}, {"b", KBD_b, 3, 1},
        {"n", KBD_n, 3, 1}, {"m", KBD_m, 3, 1}, {",", KBD_comma, 3, 1},
        {".", KBD_period, 3, 1}, {"/", KBD_slash, 3, 1},
        {"Spc", KBD_space, 4, 6},
        {"<", KBD_left, 4, 1}, {"^", KBD_up, 4, 1},
        {"v", KBD_down, 4, 1}, {">", KBD_right, 4, 1},
        {"Ent", KBD_enter, 4, 2},
};

struct XrKeyRect {
	const XrKeyDef* def;
	float           x0, x1, v0, v1;
};

// Overlay keyboard geometry: 5 rows of 0.09 in plane-UV filling the
// bottom band (v 0..BandTop) of the screen plane. The keyboard texture
// spans exactly this band, so texture labels map v/BandTop across its
// full height (no wasted empty panel).
static constexpr float OpenXrKeyboardBandTop = 0.45f;

static const std::vector<XrKeyRect>& openxr_keyboard_rects()
{
	static std::vector<XrKeyRect> rects;
	if (!rects.empty()) {
		return rects;
	}
	float cursor[5] = {};
	for (const auto& def : xr_key_defs) {
		const float x0 = cursor[def.row] / 12.0f;
		cursor[def.row] += def.units;
		const float x1 = cursor[def.row] / 12.0f;
		const float v1 = OpenXrKeyboardBandTop - def.row * 0.09f;
		rects.push_back({&def, x0, x1, v1 - 0.09f, v1});
	}
	return rects;
}

static int openxr_keyboard_hover()
{
	float u = 0.0f, v = 0.0f;
	if (!sdl_xr_keyboard_visible || !openxr_hand_plane_uv(1, u, v)) {
		return -1;
	}
	// The visual is the full-width band (u 0..1, v 0..0.45) scaled
	// about its centre and shifted by the user offsets: map the hit
	// back into key space so pointing follows xr_keyboard_scale,
	// xr_keyboard_key_scale, and xr_keyboard_offset_*.
	const float s = std::clamp(get_xr_section()->GetInt(
	                                   "xr_keyboard_scale") /
	                                   100.0f,
	                           0.25f,
	                           2.0f) *
	                openxr_keyboard_key_scale();
	const float ox = std::clamp(get_xr_section()->GetInt(
	                                    "xr_keyboard_offset_x") /
	                                    100.0f,
	                            -1.0f,
	                            1.0f);
	const float oy = std::clamp(get_xr_section()->GetInt(
	                                    "xr_keyboard_offset_y") /
	                                    100.0f,
	                            -1.0f,
	                            1.0f);
	const float ku = (u - (0.5f + ox * 0.5f)) / s + 0.5f;
	const float kv = (v - (0.225f + oy * 0.5f)) / s + 0.225f;
	if (kv < 0.0f || kv > OpenXrKeyboardBandTop) {
		return -1;
	}
	const auto& rects = openxr_keyboard_rects();
	for (size_t i = 0; i < rects.size(); ++i) {
		if (ku >= rects[i].x0 && ku < rects[i].x1 &&
		    kv >= rects[i].v0 && kv < rects[i].v1) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

static void openxr_drive_keyboard()
{
	if (!sdl_xr_keyboard_visible) {
		return;
	}
	const auto& hand = sdl_xr_hand[1];
	if (!hand.click_changed) {
		return;
	}
	if (hand.click) {
		// Trigger pressed: press the hovered key, if any.
		const int hovered = openxr_keyboard_hover();
		if (hovered >= 0) {
			const auto& rects = openxr_keyboard_rects();
			KEYBOARD_AddKey(rects[hovered].def->code, true);
			sdl_xr_keyboard_pressed = hovered;
			LOG_MSG("OpenXR: Key '%s' pressed",
			        rects[hovered].def->label);
		}
	} else if (sdl_xr_keyboard_pressed >= 0) {
		// Trigger released: release the held key.
		const auto& rects = openxr_keyboard_rects();
		if (sdl_xr_keyboard_pressed < static_cast<int>(rects.size())) {
			KEYBOARD_AddKey(rects[sdl_xr_keyboard_pressed].def->code,
			                false);
		}
		sdl_xr_keyboard_pressed = -1;
	}
}

// Right-grip press cycles keyboard overlay -> mouse input -> neither.
// Absolute presets: keeps working even if the individual Ctrl+M /
// Ctrl+K toggles were used in between.
static void openxr_drive_input_mode()
{
	if (!sdl_xr_hand[1].grip_pressed) {
		return;
	}
	sdl_xr_input_mode = (sdl_xr_input_mode + 1) % 3;

	sdl_xr_keyboard_visible = (sdl_xr_input_mode == 0);
	sdl_xr_controller_mouse = (sdl_xr_input_mode == 1);
	if (!sdl_xr_keyboard_visible && sdl_xr_keyboard_pressed >= 0) {
		const auto& rects = openxr_keyboard_rects();
		if (sdl_xr_keyboard_pressed < static_cast<int>(rects.size())) {
			KEYBOARD_AddKey(rects[sdl_xr_keyboard_pressed].def->code,
			                false);
		}
		sdl_xr_keyboard_pressed = -1;
	}
	if (!sdl_xr_controller_mouse) {
		openxr_release_mouse_buttons();
	} else {
		sdl_xr_mouse_has_last = false;
	}
	static const char* const names[] = {"keyboard", "mouse", "off"};
	OPENXR_Notify(format_str("Input mode: %s", names[sdl_xr_input_mode]));
}

// Left-grip press cycles the emulated DOS joystick (stick 1): left
// thumbstick drives the axes (analog, then 8-way digital "hat") and
// right A/B are DOS buttons 1/2. DOS can only see stick 1 at port
// 0x201 when [joystick] joysticktype exposes it (e.g. 2axis).
static void openxr_drive_joystick()
{
	if (sdl_xr_hand[0].grip_pressed) {
		sdl_xr_joy_mode = (sdl_xr_joy_mode + 1) % 3;
		static const char* const joy_names[] = {"analog", "hat", "off"};
		OPENXR_Notify(format_str("XR joystick: %s",
		                         joy_names[sdl_xr_joy_mode]));
		// (Re)centre on every transition so no direction sticks.
		JOYSTICK_Move_X(0, 0);
		JOYSTICK_Move_Y(0, 0);
		JOYSTICK_Button(0, 0, false);
		JOYSTICK_Button(0, 1, false);
		if (sdl_xr_joy_mode != 2) {
			JOYSTICK_Enable(0, true);
			if (!JOYSTICK_IsAccessible(0)) {
				LOG_MSG("OpenXR: XR joystick on but stick 1 is not visible to DOS; set joysticktype=2axis (or 4axis)");
				OPENXR_Notify(
				        "XR joystick needs joysticktype=2axis");
			}
		}
	}
	if (sdl_xr_joy_mode == 2) {
		return;
	}
	float x = sdl_xr_joy_x;
	float y = -sdl_xr_joy_y; // thumbstick up (away) is DOS up
	if (sdl_xr_joy_mode == 1) {
		// Hat: snap each axis to full deflection past the threshold.
		x = (x > 0.5f) ? 1.0f : ((x < -0.5f) ? -1.0f : 0.0f);
		y = (y > 0.5f) ? 1.0f : ((y < -0.5f) ? -1.0f : 0.0f);
	}
	JOYSTICK_Move_X(0, static_cast<int16_t>(x * 32767.0f));
	JOYSTICK_Move_Y(0, static_cast<int16_t>(y * 32767.0f));
	JOYSTICK_Button(0, 0, sdl_xr_joy_btn[0]);
	JOYSTICK_Button(0, 1, sdl_xr_joy_btn[1]);
}

static void xr_controller_mouse_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	sdl_xr_controller_mouse = !sdl_xr_controller_mouse;
	if (!sdl_xr_controller_mouse) {
		openxr_release_mouse_buttons();
	} else {
		sdl_xr_mouse_has_last = false;
	}
	OPENXR_Notify(format_str("Controller mouse control %s",
	                         sdl_xr_controller_mouse
	                                 ? "on (right hand aims, triggers click)"
	                                 : "off (host mouse)"));
}

static void xr_ray_toggle_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool shown = !get_xr_section()->GetBool("xr_ray");
	set_section_property_value("xr", "xr_ray", shown ? "on" : "off");
	OPENXR_Notify(format_str("Pointer ray %s", shown ? "on" : "off"));
}

static void xr_keyboard_toggle_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	sdl_xr_keyboard_visible = !sdl_xr_keyboard_visible;
	if (!sdl_xr_keyboard_visible && sdl_xr_keyboard_pressed >= 0) {
		const auto& rects = openxr_keyboard_rects();
		if (sdl_xr_keyboard_pressed < static_cast<int>(rects.size())) {
			KEYBOARD_AddKey(rects[sdl_xr_keyboard_pressed].def->code,
			                false);
		}
		sdl_xr_keyboard_pressed = -1;
	}
	OPENXR_Notify(format_str("Overlay keyboard %s",
	                         sdl_xr_keyboard_visible ? "shown" : "hidden"));
}

// Host mouse events defer to the right controller while it drives the
// emulated mouse.
static bool openxr_controller_mouse_active()
{
	return sdl_xr_controller_mouse && sdl_xr_session_ready;
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

// (Re)builds the keyboard texture for the given hover outline and
// pressed letter, returning it (0 on failure). Images are (re)filled
// from it at designate time; see the freshness epochs in submit.
static GLuint openxr_refresh_keyboard_texture(const int hover_index,
                                              const int pressed_index)
{
	if (sdl_xr_kbd_swapchain == XR_NULL_HANDLE || sdl.renderer == nullptr ||
	    sdl.render_backend_type != RenderBackendType::OpenGl) {
		return false;
	}
	auto* gl_renderer = static_cast<OpenGlRenderer*>(sdl.renderer.get());

	// Key labels from the single layout table (band UV mapped across
	// the full texture height, y down from the top).
	const auto& rects = openxr_keyboard_rects();
	int tex_w = 0, tex_h = 0;
	openxr_keyboard_tex_size(tex_w, tex_h);
	std::vector<OpenGlRenderer::XrKeyLabel> labels;
	labels.reserve(rects.size());
	for (const auto& r : rects) {
		labels.push_back({r.def->label,
		                  static_cast<int>(r.x0 * tex_w),
		                  static_cast<int>((1.0f - r.v1 /
		                                             OpenXrKeyboardBandTop) *
		                                   tex_h),
		                  static_cast<int>(r.x1 * tex_w),
		                  static_cast<int>((1.0f - r.v0 /
		                                             OpenXrKeyboardBandTop) *
		                                   tex_h),
		                  r.def->units > 1});
	}
	const GLuint kb_tex = gl_renderer->GetXrKeyboardTexture(labels.data(),
	                                                       labels.size(),
	                                                       tex_w,
	                                                       tex_h,
	                                                       hover_index,
	                                                       pressed_index);
	return kb_tex;
}

static bool openxr_setup_osd_layer()
{
	if (sdl_xr_session == XR_NULL_HANDLE) {
		return false;
	}
	if (sdl_xr_osd_swapchain != XR_NULL_HANDLE) {
		return true; // already built
	}
	if (sdl_xr_swapchain_format == 0) {
		return false; // main swapchain decides the format
	}

	XrSwapchainCreateInfo info = {};
	info.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                   XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format      = sdl_xr_swapchain_format;
	info.sampleCount = 1;
	info.width       = OpenXrOsdTexW;
	info.height      = OpenXrOsdTexH;
	info.faceCount   = 1;
	info.arraySize   = 1;
	info.mipCount    = 1;

	if (xrCreateSwapchain(sdl_xr_session, &info, &sdl_xr_osd_swapchain) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create OSD swapchain");
		sdl_xr_osd_swapchain = XR_NULL_HANDLE;
		return false;
	}

	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(sdl_xr_osd_swapchain,
	                              0,
	                              &image_count,
	                              nullptr) != XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate OSD swapchain images");
		return false;
	}
	sdl_xr_osd_images.resize(image_count);
	for (auto& img : sdl_xr_osd_images) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		img.next = nullptr;
	}
	if (xrEnumerateSwapchainImages(
	            sdl_xr_osd_swapchain,
	            image_count,
	            &image_count,
	            reinterpret_cast<XrSwapchainImageBaseHeader*>(
	                    sdl_xr_osd_images.data())) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate OSD swapchain images");
		sdl_xr_osd_images.clear();
		return false;
	}
	LOG_MSG("OpenXR: OSD layer ready (%dx%d, %u images)",
	        OpenXrOsdTexW,
	        OpenXrOsdTexH,
	        image_count);
	sdl_xr_osd_img_epoch.assign(image_count, 0);
	return true;
}

static bool openxr_setup_keyboard_layer()
{
	if (sdl_xr_session == XR_NULL_HANDLE) {
		return false;
	}
	if (sdl_xr_kbd_swapchain != XR_NULL_HANDLE) {
		return true; // already built
	}
	if (sdl_xr_swapchain_format == 0) {
		return false; // main swapchain decides the format
	}

	XrSwapchainCreateInfo info = {};
	info.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
	                   XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format      = sdl_xr_swapchain_format;
	info.sampleCount = 1;
	int kbd_tex_w = 0, kbd_tex_h = 0;
	openxr_keyboard_tex_size(kbd_tex_w, kbd_tex_h);
	info.width       = kbd_tex_w;
	info.height      = kbd_tex_h;
	info.faceCount   = 1;
	info.arraySize   = 1;
	info.mipCount    = 1;

	if (xrCreateSwapchain(sdl_xr_session, &info, &sdl_xr_kbd_swapchain) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create keyboard swapchain");
		sdl_xr_kbd_swapchain = XR_NULL_HANDLE;
		return false;
	}

	uint32_t image_count = 0;
	if (xrEnumerateSwapchainImages(sdl_xr_kbd_swapchain,
	                              0,
	                              &image_count,
	                              nullptr) != XR_SUCCESS ||
	    image_count == 0) {
		LOG_ERR("OpenXR: Failed to enumerate keyboard swapchain images");
		return false;
	}
	sdl_xr_kbd_images.resize(image_count);
	for (auto& img : sdl_xr_kbd_images) {
		img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
		img.next = nullptr;
	}
	if (xrEnumerateSwapchainImages(
	            sdl_xr_kbd_swapchain,
	            image_count,
	            &image_count,
	            reinterpret_cast<XrSwapchainImageBaseHeader*>(
	                    sdl_xr_kbd_images.data())) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to enumerate keyboard swapchain images");
		sdl_xr_kbd_images.clear();
		return false;
	}

	// Content is filled lazily from the first frame that shows the
	// keyboard; acquiring a swapchain image outside a running frame
	// may be rejected by strict runtimes.
	sdl_xr_kbd_tex_w = kbd_tex_w;
	sdl_xr_kbd_tex_h = kbd_tex_h;
	sdl_xr_kbd_img_epoch.assign(image_count, 0);
	LOG_MSG("OpenXR: Keyboard layer ready (%dx%d, %u images)",
	        kbd_tex_w,
	        kbd_tex_h,
	        image_count);
	return true;
}

// Rebuilds the notification texture from queued messages and returns
// it (0 on failure). Images are (re)filled from it at designate time;
// see the freshness epochs in the submit path.
static GLuint openxr_refresh_osd_texture()
{
	if (sdl_xr_osd_swapchain == XR_NULL_HANDLE || sdl.renderer == nullptr ||
	    sdl.render_backend_type != RenderBackendType::OpenGl) {
		return 0;
	}
	auto* gl_renderer = static_cast<OpenGlRenderer*>(sdl.renderer.get());

	std::vector<const char*> lines;
	lines.reserve(sdl_xr_osd.size());
	for (auto it = sdl_xr_osd.rbegin(); it != sdl_xr_osd.rend(); ++it) {
		lines.push_back(it->text.c_str());
	}
	const GLuint osd_tex = gl_renderer->UpdateXrOsdTexture(lines.data(),
	                                                      lines.size(),
	                                                      OpenXrOsdTexW,
	                                                      OpenXrOsdTexH);
	return osd_tex;
}

// Log + show an in-headset notification for XR toggles and setting
// changes. Quietly drops the overlay part when disabled or too early.
void OPENXR_Notify(const std::string& message)
{
	LOG_MSG("OpenXR: %s", message.c_str());
	if (!get_xr_section()->GetBool("xr_osd")) {
		return;
	}
	sdl_xr_osd.push_back({message, GetTicksUs()});
	while (sdl_xr_osd.size() > 4) {
		sdl_xr_osd.pop_front();
	}
	// Mark dirty; the next submitted frame rebuilds + fills (swapchain
	// I/O here can violate the runtime's frame call order).
	sdl_xr_osd_dirty = true;
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
		const bool use_lock = (sdl_xr_world_locked && sdl_xr_has_lock);
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
		// Rebuild the texture when the hover or pressed key changed
		// (the renderer caches it otherwise); images refresh
		// from it below.
		const int hover   = openxr_keyboard_hover();
		const int pressed = sdl_xr_keyboard_pressed;
		if (hover != sdl_xr_kbd_hover_built ||
		    pressed != sdl_xr_kbd_pressed_built) {
			sdl_xr_kbd_hover_built   = hover;
			sdl_xr_kbd_pressed_built = pressed;
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
			quad.subImage.swapchain = sdl_xr_kbd_swapchain;
			quad.subImage.imageRect = {
			        {0, 0},
			        {static_cast<int32_t>(kbd_tex_w),
			         static_cast<int32_t>(kbd_tex_h)}};
			quad.subImage.imageArrayIndex = 0;
			quad.pose.orientation         = anchor.orientation;
			quad.pose.position = {anchor.position.x +
			                              fwd.x * (dist_m + 0.02f) -
			                              up.x * (0.55f * half_h) +
			                              right.x * kbd_ox +
			                              up.x * kbd_oy,
			                      anchor.position.y +
			                              fwd.y * (dist_m + 0.02f) -
			                              up.y * (0.55f * half_h) +
			                              right.y * kbd_ox +
			                              up.y * kbd_oy,
			                      anchor.position.z +
			                              fwd.z * (dist_m + 0.02f) -
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
	if (render_layers && get_xr_section()->GetBool("xr_osd") &&
	    sdl_xr_osd_swapchain != XR_NULL_HANDLE && !sdl_xr_osd.empty()) {
		const auto now_us = GetTicksUs();
		while (!sdl_xr_osd.empty() &&
		       now_us - sdl_xr_osd.front().time_us > OpenXrOsdTimeoutUs) {
			sdl_xr_osd.pop_front();
		}
		// Rebuild the texture when a new message arrived since the
		// last submit; images refresh from it below.
		if (!sdl_xr_osd.empty() && sdl_xr_osd_dirty) {
			const GLuint osd_tex = openxr_refresh_osd_texture();
			if (osd_tex != 0) {
				sdl_xr_osd_tex   = osd_tex;
				sdl_xr_osd_dirty = false;
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
				        std::clamp(get_xr_section()->GetInt(
				                           "xr_osd_offset_x") /
				                           100.0f,
				                   -1.0f,
				                   1.0f) *
				        osd_half_w;
				const float osd_oy =
				        std::clamp(get_xr_section()->GetInt(
				                           "xr_osd_offset_y") /
				                           100.0f,
				                   -1.0f,
				                   1.0f) *
				        osd_half_h;
				osd_quad.type          = XR_TYPE_COMPOSITION_LAYER_QUAD;
				osd_quad.space         = sdl_xr_app_space;
				osd_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				osd_quad.subImage.swapchain = sdl_xr_osd_swapchain;
				osd_quad.subImage.imageRect = {
				        {0, 0},
				        {static_cast<int32_t>(OpenXrOsdTexW),
				         static_cast<int32_t>(OpenXrOsdTexH)}};
				osd_quad.subImage.imageArrayIndex = 0;
				osd_quad.pose.orientation = osd_anchor.orientation;
				osd_quad.pose.position = {
				        osd_anchor.position.x +
				                osd_fwd.x * (osd_dist + 0.02f) +
				                osd_up.x * (0.75f * osd_half_h) +
				                osd_right.x * osd_ox +
				                osd_up.x * osd_oy,
				        osd_anchor.position.y +
				                osd_fwd.y * (osd_dist + 0.02f) +
				                osd_up.y * (0.75f * osd_half_h) +
				                osd_right.y * osd_ox +
				                osd_up.y * osd_oy,
				        osd_anchor.position.z +
				                osd_fwd.z * (osd_dist + 0.02f) +
				                osd_up.z * (0.75f * osd_half_h) +
				                osd_right.z * osd_ox +
				                osd_up.z * osd_oy};
				const float osd_s =
				        std::clamp(get_xr_section()->GetInt(
				                           "xr_osd_scale") /
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

	const XrCompositionLayerBaseHeader* layers[3] = {};

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
	if (sdl_xr_kbd_swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(sdl_xr_kbd_swapchain);
		sdl_xr_kbd_swapchain = XR_NULL_HANDLE;
	}
	sdl_xr_kbd_images.clear();
	sdl_xr_kbd_filled = false;
	sdl_xr_kbd_hover_built = -2;
	sdl_xr_kbd_pressed_built = -2;
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
		sdl_xr_hand[hand].click         = false;
		sdl_xr_hand[hand].click_changed = false;
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

static void init_openxr()
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
static void openxr_update_head_tracking()
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
static void openxr_maybe_recover()
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
static bool openxr_frame_begin(XrTime& display_time_out)
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
	openxr_drive_mouse();
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

static void openxr_frame_end(const XrTime display_time)
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
                                   const int h, float* depth_norm)
{
	const std::string method =
	        get_xr_section()->GetString("xr_autodepth_method");
	const bool use_grad = (method != "luma");
	const bool use_luma = (method != "gradient");
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
			} else {
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

#if C_NEURAL
// ---------------------------------------------------------------------------
// Neural monocular depth ('neural' autodepth submode)
// ---------------------------------------------------------------------------
// MiDaS v2.1 small (256x256 input, normalization baked into the model)
// via a runtime-loaded ONNX Runtime: no build-time dependency, nothing
// links against ORT. Files live in <config>/xrdepth/ (filenames kept in
// sync with dos/programs/xrdepth.cpp):
//   midas_v21_small_256.onnx  (~66 MB model)
//   onnxruntime.dll           (plus onnxruntime_providers_shared.dll, if any)
// A background worker runs inference about once a second; the blit path
// reuses the latest map and falls back to the heuristic cues when none
// is ready, so neural mode degrades gracefully instead of stalling.
//
// ABI notes: the declarations below mirror onnxruntime_c_api.h v1.30.0
// (verified member-by-member against the upstream header) for the prefix
// through ReleaseValue; unneeded slots are padding (every member is one
// pointer). OrtApi is append-only across versions, so requesting API v13
// keeps every used slot aligned on any runtime >= 1.13.
#if defined(_WIN32)
#define XR_ORT_CALL __stdcall
#else
#define XR_ORT_CALL
#endif
#if defined(_WIN32)
using XrOrtChar = wchar_t;
#else
using XrOrtChar = char;
#endif

struct OrtEnv;
struct OrtSession;
struct OrtSessionOptions;
struct OrtRunOptions;
struct OrtValue;
struct OrtTypeInfo;
struct OrtTensorTypeAndShapeInfo;
struct OrtMemoryInfo;
struct OrtAllocator;
struct OrtStatus;

enum XrOrtErrorCode { XR_ORT_OK = 0 };
enum XrOrtLoggingLevel {
	XR_ORT_LOGGING_VERBOSE = 0,
	XR_ORT_LOGGING_INFO    = 1,
	XR_ORT_LOGGING_WARNING = 2,
	XR_ORT_LOGGING_ERROR   = 3,
	XR_ORT_LOGGING_FATAL   = 4
};
enum XrOrtGraphOptimizationLevel {
	XR_ORT_DISABLE_ALL = 0,
	XR_ORT_ENABLE_BASIC = 1,
	XR_ORT_ENABLE_EXTENDED = 2,
	XR_ORT_ENABLE_ALL = 99
};
enum XrOrtAllocatorType { XR_ORT_DEVICE_ALLOCATOR = 0 };
enum XrOrtMemType { XR_ORT_MEM_DEFAULT = 0 };
enum XrOrtTensorElementDataType {
	XR_ORT_ELEM_UNDEFINED = 0,
	XR_ORT_ELEM_FLOAT = 1
};

struct XrOrtApiBase {
	const struct XrOrtApi* (XR_ORT_CALL* GetApi)(uint32_t version) noexcept;
	const char* (XR_ORT_CALL* GetVersionString)(void) noexcept;
};

struct XrOrtApi {
	void* CreateStatus_; // 1
	XrOrtErrorCode(XR_ORT_CALL* GetErrorCode)(const OrtStatus* status) noexcept; // 2
	const char*(XR_ORT_CALL* GetErrorMessage)(const OrtStatus* status) noexcept; // 3
	OrtStatus*(XR_ORT_CALL* CreateEnv)(XrOrtLoggingLevel log_severity_level,
	                                   const char* logid,
	                                   OrtEnv** out) noexcept; // 4
	void* CreateEnvWithCustomLogger_; // 5
	void* EnableTelemetryEvents_; // 6
	void* DisableTelemetryEvents_; // 7
	OrtStatus*(XR_ORT_CALL* CreateSession)(const OrtEnv* env,
	                                       const XrOrtChar* model_path,
	                                       const OrtSessionOptions* options,
	                                       OrtSession** out) noexcept; // 8
	void* CreateSessionFromArray_; // 9
	OrtStatus*(XR_ORT_CALL* Run)(OrtSession* session,
	                             const OrtRunOptions* run_options,
	                             const char* const* input_names,
	                             const OrtValue* const* inputs,
	                             size_t input_len,
	                             const char* const* output_names,
	                             size_t output_names_len,
	                             OrtValue** outputs) noexcept; // 10
	OrtStatus*(XR_ORT_CALL* CreateSessionOptions)(OrtSessionOptions** options) noexcept; // 11
	void* SetOptimizedModelFilePath_; // 12
	void* CloneSessionOptions_; // 13
	void* SetSessionExecutionMode_; // 14
	void* EnableProfiling_; // 15
	void* DisableProfiling_; // 16
	void* EnableMemPattern_; // 17
	void* DisableMemPattern_; // 18
	void* EnableCpuMemArena_; // 19
	void* DisableCpuMemArena_; // 20
	void* SetSessionLogId_; // 21
	void* SetSessionLogVerbosityLevel_; // 22
	void* SetSessionLogSeverityLevel_; // 23
	OrtStatus*(XR_ORT_CALL* SetSessionGraphOptimizationLevel)(OrtSessionOptions* options,
	                                                          XrOrtGraphOptimizationLevel graph_optimization_level) noexcept; // 24
	OrtStatus*(XR_ORT_CALL* SetIntraOpNumThreads)(OrtSessionOptions* options,
	                                              int intra_op_num_threads) noexcept; // 25
	void* SetInterOpNumThreads_; // 26
	void* CreateCustomOpDomain_; // 27
	void* CustomOpDomain_Add_; // 28
	void* AddCustomOpDomain_; // 29
	void* RegisterCustomOpsLibrary_; // 30
	OrtStatus*(XR_ORT_CALL* SessionGetInputCount)(const OrtSession* session,
	                                              size_t* out) noexcept; // 31
	OrtStatus*(XR_ORT_CALL* SessionGetOutputCount)(const OrtSession* session,
	                                               size_t* out) noexcept; // 32
	void* SessionGetOverridableInitializerCount_; // 33
	OrtStatus*(XR_ORT_CALL* SessionGetInputTypeInfo)(const OrtSession* session,
	                                                 size_t index,
	                                                 OrtTypeInfo** type_info) noexcept; // 34
	OrtStatus*(XR_ORT_CALL* SessionGetOutputTypeInfo)(const OrtSession* session,
	                                                  size_t index,
	                                                  OrtTypeInfo** type_info) noexcept; // 35
	void* SessionGetOverridableInitializerTypeInfo_; // 36
	OrtStatus*(XR_ORT_CALL* SessionGetInputName)(const OrtSession* session,
	                                             size_t index,
	                                             OrtAllocator* allocator,
	                                             char** value) noexcept; // 37
	OrtStatus*(XR_ORT_CALL* SessionGetOutputName)(const OrtSession* session,
	                                              size_t index,
	                                              OrtAllocator* allocator,
	                                              char** value) noexcept; // 38
	void* SessionGetOverridableInitializerName_; // 39
	void* CreateRunOptions_; // 40
	void* RunOptionsSetRunLogVerbosityLevel_; // 41
	void* RunOptionsSetRunLogSeverityLevel_; // 42
	void* RunOptionsSetRunTag_; // 43
	void* RunOptionsGetRunLogVerbosityLevel_; // 44
	void* RunOptionsGetRunLogSeverityLevel_; // 45
	void* RunOptionsGetRunTag_; // 46
	void* RunOptionsSetTerminate_; // 47
	void* RunOptionsUnsetTerminate_; // 48
	void* CreateTensorAsOrtValue_; // 49
	OrtStatus*(XR_ORT_CALL* CreateTensorWithDataAsOrtValue)(const OrtMemoryInfo* info,
	                                                        void* p_data,
	                                                        size_t p_data_len,
	                                                        const int64_t* shape,
	                                                        size_t shape_len,
	                                                        XrOrtTensorElementDataType type,
	                                                        OrtValue** out) noexcept; // 50
	void* IsTensor_; // 51
	OrtStatus*(XR_ORT_CALL* GetTensorMutableData)(OrtValue* value,
	                                              void** out) noexcept; // 52
	void* FillStringTensor_; // 53
	void* GetStringTensorDataLength_; // 54
	void* GetStringTensorContent_; // 55
	OrtStatus*(XR_ORT_CALL* CastTypeInfoToTensorInfo)(const OrtTypeInfo* type_info,
	                                                  const OrtTensorTypeAndShapeInfo** out) noexcept; // 56
	void* GetOnnxTypeFromTypeInfo_; // 57
	void* CreateTensorTypeAndShapeInfo_; // 58
	void* SetTensorElementType_; // 59
	void* SetDimensions_; // 60
	OrtStatus*(XR_ORT_CALL* GetTensorElementType)(const OrtTensorTypeAndShapeInfo* info,
	                                              XrOrtTensorElementDataType* out) noexcept; // 61
	OrtStatus*(XR_ORT_CALL* GetDimensionsCount)(const OrtTensorTypeAndShapeInfo* info,
	                                            size_t* out) noexcept; // 62
	OrtStatus*(XR_ORT_CALL* GetDimensions)(const OrtTensorTypeAndShapeInfo* info,
	                                       int64_t* dim_values,
	                                       size_t dim_values_length) noexcept; // 63
	void* GetSymbolicDimensions_; // 64
	void* GetTensorShapeElementCount_; // 65
	void* GetTensorTypeAndShape_; // 66
	void* GetTypeInfo_; // 67
	void* GetValueType_; // 68
	void* CreateMemoryInfo_; // 69
	OrtStatus*(XR_ORT_CALL* CreateCpuMemoryInfo)(XrOrtAllocatorType type,
	                                             XrOrtMemType mem_type,
	                                             OrtMemoryInfo** out) noexcept; // 70
	void* CompareMemoryInfo_; // 71
	void* MemoryInfoGetName_; // 72
	void* MemoryInfoGetId_; // 73
	void* MemoryInfoGetMemType_; // 74
	void* MemoryInfoGetType_; // 75
	void* AllocatorAlloc_; // 76
	OrtStatus*(XR_ORT_CALL* AllocatorFree)(OrtAllocator* ort_allocator,
	                                       void* p) noexcept; // 77
	void* AllocatorGetInfo_; // 78
	OrtStatus*(XR_ORT_CALL* GetAllocatorWithDefaultOptions)(OrtAllocator** out) noexcept; // 79
	void* AddFreeDimensionOverride_; // 80
	void* GetValue_; // 81
	void* GetValueCount_; // 82
	void* CreateValue_; // 83
	void* CreateOpaqueValue_; // 84
	void* GetOpaqueValue_; // 85
	void* KernelInfoGetAttribute_float_; // 86
	void* KernelInfoGetAttribute_int64_; // 87
	void* KernelInfoGetAttribute_string_; // 88
	void* KernelContext_GetInputCount_; // 89
	void* KernelContext_GetOutputCount_; // 90
	void* KernelContext_GetInput_; // 91
	void* KernelContext_GetOutput_; // 92
	void* ReleaseEnv_; // 93 (process-lifetime objects are never released)
	void (XR_ORT_CALL* ReleaseStatus)(OrtStatus* input); // 94
	void* ReleaseMemoryInfo_; // 95
	void* ReleaseSession_; // 96
	void (XR_ORT_CALL* ReleaseValue)(OrtValue* input); // 97
};

using XrOrtGetApiBaseFn =
        const XrOrtApiBase* (XR_ORT_CALL*)(void) noexcept;

// Neural depth runtime state (process-lifetime; the worker owns the
// ORT session and only idles when neural mode is off).
struct XrNeuralState {
#if defined(_WIN32)
	HMODULE lib = nullptr;
#else
	void* lib = nullptr;
#endif
	const XrOrtApi* ort           = nullptr;
	OrtEnv* env                   = nullptr;
	OrtSession* session           = nullptr;
	OrtSessionOptions* options    = nullptr;
	OrtMemoryInfo* mem            = nullptr;
	OrtAllocator* alloc           = nullptr;
	std::string input_name;
	std::string output_name;
	int64_t in_w = 0, in_h = 0; // model input dims (NCHW, N=C=1ch? no: N=1,C=3)
	int64_t out_w = 0, out_h = 0; // model output plane dims
	std::vector<float> in_tensor; // 1*3*in_h*in_w, RGB [0,1]
	std::thread worker;
	std::mutex mtx;
	std::condition_variable cv;
	bool stop_requested  = false;
	bool worker_running  = false;
	bool load_done       = false; // loader ran (success or terminal state below)
	bool ready           = false; // session usable
	std::string error; // last loader error for status/toasts
	bool has_job  = false;
	bool job_busy = false;
	std::vector<uint32_t> job_frame; // bottom-up ARGB32 source frame
	int job_w = 0, job_h = 0;
	std::vector<float> map; // published model-space inverse-depth map
	int map_w = 0, map_h = 0;
	uint64_t map_epoch = 0;
	int64_t last_attempt_us = 0;
	int64_t last_submit_us  = 0;
	bool toast_loading = false;
	bool toast_active  = false;
	bool toast_missing_needed = false; // worker asks main thread to warn
	bool toast_missing_done   = false;
};

static XrNeuralState xr_neural;
static std::once_flag xr_neural_stop_flag;

// Keep in sync with dos/programs/xrdepth.cpp.
static std_fs::path xr_neural_dir()
{
	return get_config_dir() / "xrdepth";
}
static std_fs::path xr_neural_model_path()
{
	return xr_neural_dir() / "midas_v21_small_256.onnx";
}
static std_fs::path xr_neural_dll_path()
{
#if defined(_WIN32)
	return xr_neural_dir() / "onnxruntime.dll";
#elif defined(__linux__)
	return xr_neural_dir() / "libonnxruntime.so";
#elif defined(__APPLE__)
	return xr_neural_dir() / "libonnxruntime.dylib";
#else
	return std_fs::path();
#endif
}

// Report an ORT failure (logs the message, releases the status).
static bool xr_ort_check(OrtStatus* st, const char* what)
{
	if (!st) {
		return true;
	}
	const char* msg = "?";
	if (xr_neural.ort) {
		msg = xr_neural.ort->GetErrorMessage(st);
	}
	LOG_ERR("OpenXR: Neural depth %s failed: %s", what, msg);
	xr_neural.ort->ReleaseStatus(st);
	return false;
}

static void* xr_ort_load_library(const std_fs::path& path)
{
#if defined(_WIN32)
	// The DLL's own directory must resolve its sibling dependencies.
	wchar_t prev_dir[MAX_PATH] = {};
	const DWORD prev_len = GetDllDirectoryW(ARRAYSIZE(prev_dir), prev_dir);
	const std::wstring dir = path.parent_path().wstring();
	SetDllDirectoryW(dir.c_str());
	HMODULE mod = LoadLibraryW(path.wstring().c_str());
	if (prev_len > 0) {
		SetDllDirectoryW(prev_dir);
	} else {
		SetDllDirectoryW(nullptr);
	}
	return mod;
#elif defined(__linux__) || defined(__APPLE__)
	return dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#else
	(void)path;
	return nullptr;
#endif
}

// Full session setup on the worker thread (slow: multi-second model
// load). Returns true when inference is usable.
static bool xr_neural_load()
{
	auto& s = xr_neural;
	if (s.ready) {
		return true;
	}
	const auto dll_path   = xr_neural_dll_path();
	const auto model_path = xr_neural_model_path();
	if (dll_path.empty() || !std_fs::exists(dll_path) ||
	    !std_fs::exists(model_path)) {
		s.error = "files missing (run XRDEPTH DOWNLOAD)";
		return false;
	}
	s.lib = static_cast<decltype(s.lib)>(
	        xr_ort_load_library(dll_path));
	if (!s.lib) {
		s.error = "could not load onnxruntime library";
		return false;
	}
#if defined(_WIN32)
	auto* get_base = reinterpret_cast<XrOrtGetApiBaseFn>(
	        GetProcAddress(s.lib, "OrtGetApiBase"));
#else
	auto* get_base = reinterpret_cast<XrOrtGetApiBaseFn>(
	        dlsym(s.lib, "OrtGetApiBase"));
#endif
	if (!get_base) {
		s.error = "OrtGetApiBase not exported";
		return false;
	}
	const XrOrtApiBase* base = get_base();
	if (!base || !base->GetApi) {
		s.error = "bad OrtApiBase";
		return false;
	}
	// API v13: every function below exists there; the struct prefix
	// matches any runtime >= 1.13 (append-only ABI).
	s.ort = base->GetApi(13);
	if (!s.ort) {
		s.error = "ORT API v13 unsupported by this runtime";
		return false;
	}
	LOG_MSG("OpenXR: Neural depth runtime %s", base->GetVersionString());

	auto* ort = s.ort;
	if (!xr_ort_check(ort->CreateEnv(XR_ORT_LOGGING_WARNING,
	                                "dosbox-xr",
	                                &s.env),
	                  "CreateEnv") ||
	    !xr_ort_check(ort->CreateSessionOptions(&s.options),
	                  "CreateSessionOptions")) {
		s.error = "env/options setup failed";
		return false;
	}
	// Best effort; defaults apply when the runtime ignores them.
	xr_ort_check(ort->SetSessionGraphOptimizationLevel(
	                     s.options, XR_ORT_ENABLE_ALL),
	             "graph optimization");
	unsigned threads = std::thread::hardware_concurrency();
	if (threads < 1) {
		threads = 2;
	}
	if (threads > 4) {
		threads = 4;
	}
	xr_ort_check(ort->SetIntraOpNumThreads(s.options,
	                                       static_cast<int>(threads)),
	             "thread count");
#if defined(_WIN32)
	const std::wstring wmodel = model_path.wstring();
	if (!xr_ort_check(ort->CreateSession(s.env,
	                                    wmodel.c_str(),
	                                    s.options,
	                                    &s.session),
	                  "CreateSession")) {
#else
	if (!xr_ort_check(ort->CreateSession(s.env,
	                                    model_path.string().c_str(),
	                                    s.options,
	                                    &s.session),
	                  "CreateSession")) {
#endif
		s.error = "model load failed (bad or corrupt file?)";
		return false;
	}
	size_t in_count = 0, out_count = 0;
	if (!xr_ort_check(ort->SessionGetInputCount(s.session, &in_count),
	                  "input count") ||
	    !xr_ort_check(ort->SessionGetOutputCount(s.session, &out_count),
	                  "output count") ||
	    in_count < 1 || out_count < 1) {
		s.error = "model has no inputs/outputs";
		return false;
	}
	if (!xr_ort_check(ort->GetAllocatorWithDefaultOptions(&s.alloc),
	                  "default allocator") ||
	    !xr_ort_check(ort->CreateCpuMemoryInfo(XR_ORT_DEVICE_ALLOCATOR,
	                                           XR_ORT_MEM_DEFAULT,
	                                           &s.mem),
	                  "cpu memory info")) {
		s.error = "allocator setup failed";
		return false;
	}
	// Input: expect float NCHW with C=3.
	char* in_name = nullptr;
	OrtTypeInfo* in_type = nullptr;
	bool ok = xr_ort_check(ort->SessionGetInputName(s.session,
	                                                0,
	                                                s.alloc,
	                                                &in_name),
	                       "input name") &&
	          xr_ort_check(ort->SessionGetInputTypeInfo(s.session,
	                                                     0,
	                                                     &in_type),
	                       "input type");
	if (ok && in_name) {
		s.input_name = in_name;
	}
	if (in_name) {
		ort->AllocatorFree(s.alloc, in_name);
	}
	const OrtTensorTypeAndShapeInfo* in_shape = nullptr;
	XrOrtTensorElementDataType in_elem = XR_ORT_ELEM_UNDEFINED;
	size_t in_rank = 0;
	int64_t in_dims[8] = {};
	ok = ok &&
	     xr_ort_check(ort->CastTypeInfoToTensorInfo(in_type, &in_shape),
	                  "input shape cast") &&
	     xr_ort_check(ort->GetTensorElementType(in_shape, &in_elem),
	                  "input element type") &&
	     xr_ort_check(ort->GetDimensionsCount(in_shape, &in_rank),
	                  "input rank") &&
	     (in_rank == 4) &&
	     xr_ort_check(ort->GetDimensions(in_shape, in_dims, in_rank),
	                  "input dims");
	if (!ok || in_elem != XR_ORT_ELEM_FLOAT || in_dims[0] != 1 ||
	    in_dims[1] != 3 || in_dims[2] <= 0 || in_dims[3] <= 0 ||
	    in_dims[2] > 1024 || in_dims[3] > 1024) {
		s.error = "unexpected model input (want float 1x3xHxW)";
		return false;
	}
	s.in_h = in_dims[2];
	s.in_w = in_dims[3];
	// Output: float map; last two dims are the plane (rank 4/3/2 ok).
	char* out_name = nullptr;
	OrtTypeInfo* out_type = nullptr;
	ok = xr_ort_check(ort->SessionGetOutputName(s.session,
	                                            0,
	                                            s.alloc,
	                                            &out_name),
	                  "output name") &&
	     xr_ort_check(ort->SessionGetOutputTypeInfo(s.session,
	                                                0,
	                                                &out_type),
	                  "output type");
	if (ok && out_name) {
		s.output_name = out_name;
	}
	if (out_name) {
		ort->AllocatorFree(s.alloc, out_name);
	}
	const OrtTensorTypeAndShapeInfo* out_shape = nullptr;
	XrOrtTensorElementDataType out_elem = XR_ORT_ELEM_UNDEFINED;
	size_t out_rank = 0;
	int64_t out_dims[8] = {};
	ok = ok &&
	     xr_ort_check(ort->CastTypeInfoToTensorInfo(out_type, &out_shape),
	                  "output shape cast") &&
	     xr_ort_check(ort->GetTensorElementType(out_shape, &out_elem),
	                  "output element type") &&
	     xr_ort_check(ort->GetDimensionsCount(out_shape, &out_rank),
	                  "output rank") &&
	     (out_rank >= 2 && out_rank <= 4) &&
	     xr_ort_check(ort->GetDimensions(out_shape, out_dims, out_rank),
	                  "output dims");
	// NOTE: TypeInfo/MemoryInfo/SessionOptions leak by design (a few
	// small objects per process; only Status and per-run Values are
	// released). Sessions/objects are process-lifetime: the worker is
	// joined at exit and the OS reclaims the rest.
	if (!ok || out_elem != XR_ORT_ELEM_FLOAT) {
		s.error = "unexpected model output (want float map)";
		return false;
	}
	s.out_h = out_dims[out_rank - 2];
	s.out_w = out_dims[out_rank - 1];
	if (s.out_h <= 0 || s.out_w <= 0 || s.out_h > 2048 ||
	    s.out_w > 2048) {
		s.error = "bogus model output dims";
		return false;
	}
	s.in_tensor.assign(static_cast<size_t>(3) * s.in_h * s.in_w, 0.0f);
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		s.map.assign(static_cast<size_t>(s.out_h) * s.out_w, 0.0f);
		s.map_w = static_cast<int>(s.out_w);
		s.map_h = static_cast<int>(s.out_h);
	}
	s.ready = true;
	LOG_MSG("OpenXR: Neural depth ready (in %s %lldx%lld, out %lldx%lld)",
	        s.input_name.c_str(),
	        (long long)s.in_w,
	        (long long)s.in_h,
	        (long long)s.out_w,
	        (long long)s.out_h);
	return true;
}

// Bilinear-ish downscale: bottom-up ARGB32 to top-down RGB float [0,1]
// (MiDaS-small normalization is baked into the model file).
static void xr_neural_preprocess(const uint32_t* frame, int fw, int fh,
                                 float* dst, int dw, int dh)
{
	for (int y = 0; y < dh; ++y) {
		// Model row 0 is the top: sample the frame bottom-up flipped.
		const float src_y = (y + 0.5f) * fh / dh;
		int y0 = static_cast<int>(fh - src_y - 0.5f);
		if (y0 < 0) {
			y0 = 0;
		}
		if (y0 >= fh) {
			y0 = fh - 1;
		}
		const auto* row = frame + static_cast<size_t>(y0) * fw;
		for (int x = 0; x < dw; ++x) {
			const float src_x = (x + 0.5f) * fw / dw;
			int x0 = static_cast<int>(src_x - 0.5f);
			if (x0 < 0) {
				x0 = 0;
			}
			if (x0 >= fw) {
				x0 = fw - 1;
			}
			const uint32_t p = row[x0];
			const size_t base = static_cast<size_t>(y) * dw + x;
			dst[base] = ((p >> 16) & 0xFFu) / 255.0f;
			dst[static_cast<size_t>(dw) * dh + base] =
			        ((p >> 8) & 0xFFu) / 255.0f;
			dst[static_cast<size_t>(2) * dw * dh + base] =
			        (p & 0xFFu) / 255.0f;
		}
	}
}

static void xr_neural_worker_main()
{
	auto& s = xr_neural;
	// (Re)try the slow model load until it works or we are stopped,
	// so files downloaded mid-session are picked up with no restart.
	for (;;) {
		if (xr_neural_load()) {
			break;
		}
		{
			std::unique_lock<std::mutex> lock(s.mtx);
			s.load_done = true;
			if (!s.toast_missing_done) {
				s.toast_missing_done   = true;
				s.toast_missing_needed = true;
			}
			s.cv.wait_for(lock,
			              std::chrono::seconds(5),
			              [&] { return s.stop_requested; });
			if (s.stop_requested) {
				return;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		s.load_done = true;
		s.cv.notify_all();
	}
	auto* ort = s.ort;
	const int64_t in_shape[4] = {1, 3, s.in_h, s.in_w};
	const char* in_names[1]  = {s.input_name.c_str()};
	const char* out_names[1] = {s.output_name.c_str()};
	for (;;) {
		std::vector<uint32_t> frame;
		int fw = 0, fh = 0;
		{
			std::unique_lock<std::mutex> lock(s.mtx);
			s.cv.wait(lock, [&] {
				return s.stop_requested || s.has_job;
			});
			if (s.stop_requested) {
				return;
			}
			frame.swap(s.job_frame);
			fw         = s.job_w;
			fh         = s.job_h;
			s.has_job  = false;
			s.job_busy = true;
		}
		if (fw <= 0 || fh <= 0 || frame.empty()) {
			std::lock_guard<std::mutex> lock(s.mtx);
			s.job_busy = false;
			continue;
		}
		xr_neural_preprocess(frame.data(),
		                     fw,
		                     fh,
		                     s.in_tensor.data(),
		                     static_cast<int>(s.in_w),
		                     static_cast<int>(s.in_h));
		OrtValue* in_val  = nullptr;
		OrtValue* out_val = nullptr;
		bool run_ok =
		        xr_ort_check(ort->CreateTensorWithDataAsOrtValue(
		                             s.mem,
		                             s.in_tensor.data(),
		                             s.in_tensor.size() * sizeof(float),
		                             in_shape,
		                             4,
		                             XR_ORT_ELEM_FLOAT,
		                             &in_val),
		                     "input tensor") &&
		        xr_ort_check(ort->Run(s.session,
		                              nullptr,
		                              in_names,
		                              const_cast<const OrtValue**>(&in_val),
		                              1,
		                              out_names,
		                              1,
		                              &out_val),
		                     "inference");
		if (in_val) {
			ort->ReleaseValue(in_val);
		}
		if (run_ok && out_val) {
			void* out_data = nullptr;
			if (xr_ort_check(ort->GetTensorMutableData(out_val,
			                                           &out_data),
			                 "output data") &&
			    out_data) {
				const auto* f = static_cast<const float*>(out_data);
				std::lock_guard<std::mutex> lock(s.mtx);
				const size_t n =
				        std::min(s.map.size(),
				                 static_cast<size_t>(s.out_h) *
				                         s.out_w);
				for (size_t i = 0; i < n; ++i) {
					s.map[i] = f[i];
				}
				++s.map_epoch;
			}
			ort->ReleaseValue(out_val);
		}
		{
			std::lock_guard<std::mutex> lock(s.mtx);
			s.job_busy = false;
		}
	}
}

static void xr_neural_stop_at_exit()
{
	auto& s = xr_neural;
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		s.stop_requested = true;
	}
	s.cv.notify_all();
	if (s.worker.joinable()) {
		s.worker.join();
	}
#if defined(_WIN32)
	if (s.lib) {
		FreeLibrary(s.lib);
		s.lib = nullptr;
	}
#elif defined(__linux__) || defined(__APPLE__)
	if (s.lib) {
		dlclose(s.lib);
		s.lib = nullptr;
	}
#endif
}

// Hand the latest frame to the worker (throttled); starts the worker
// (and its async model load) on first neural use. Never blocks.
static void xr_neural_submit_frame(const uint32_t* frame, int w, int h)
{
	auto& s = xr_neural;
	if (!s.worker_running) {
		s.worker_running = true;
		std::call_once(xr_neural_stop_flag, [] {
			std::atexit(xr_neural_stop_at_exit);
		});
		try {
			s.worker = std::thread(xr_neural_worker_main);
		} catch (const std::exception& e) {
			LOG_ERR("OpenXR: Neural depth thread failed: %s", e.what());
			s.worker_running = false;
			return;
		}
		if (!s.toast_loading) {
			s.toast_loading = true;
			OPENXR_Notify("Neural depth: loading model...");
		}
	}
	int interval_ms = get_xr_section()->GetInt("xr_neural_interval_ms");
	if (interval_ms < 100) {
		interval_ms = 100;
	}
	if (interval_ms > 5000) {
		interval_ms = 5000;
	}
	const int64_t now_us = GetTicksUs();
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		// Don't pile up: the worker always has the latest queued
		// frame waiting when it is free.
		if (s.has_job || s.job_busy) {
			return;
		}
		if (s.ready && now_us - s.last_submit_us <
		                static_cast<int64_t>(interval_ms) * 1000) {
			return;
		}
		s.last_submit_us = now_us;
		s.job_frame.assign(frame, frame + static_cast<size_t>(w) * h);
		s.job_w   = w;
		s.job_h   = h;
		s.has_job = true;
	}
	s.cv.notify_one();
}

// Main-thread side of the worker's missing-files warning (the worker
// must never touch the OSD queue itself).
static void xr_neural_poll_missing_toast()
{
	auto& s = xr_neural;
	bool needed = false;
	{
		std::lock_guard<std::mutex> lock(s.mtx);
		if (s.toast_missing_needed) {
			s.toast_missing_needed = false;
			needed                 = true;
		}
	}
	if (needed) {
		OPENXR_Notify("Neural depth needs files: run XRDEPTH DOWNLOAD");
	}
}

// Upsample the published model map (nearest) into a frame-sized,
// normalized depth map (1 = near). Returns false when no map is ready
// (caller falls back to the heuristic cues); flat when spread is nil.
static bool xr_neural_fill_map(float* depth_norm, int w, int h, bool& flat)
{
	auto& s = xr_neural;
	std::lock_guard<std::mutex> lock(s.mtx);
	if (!s.ready || s.map_epoch == 0 || s.map_w <= 0 || s.map_h <= 0 ||
	    s.map.size() != static_cast<size_t>(s.map_w) * s.map_h) {
		return false;
	}
	const bool invert = get_xr_section()->GetBool("xr_autodepth_invert");
	float dmin = std::numeric_limits<float>::max();
	float dmax = std::numeric_limits<float>::lowest();
	for (int y = 0; y < h; ++y) {
		// Model row 0 is the top; frame rows run bottom-up.
		const int my = (h - 1 - y) * s.map_h / h;
		const auto* mrow = s.map.data() + static_cast<size_t>(my) *
		                                        s.map_w;
		auto* drow = depth_norm + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; ++x) {
			const int mx = x * s.map_w / w;
			const float v = mrow[mx];
			drow[x]      = v;
			if (v < dmin) {
				dmin = v;
			}
			if (v > dmax) {
				dmax = v;
			}
		}
	}
	if (dmax <= dmin) {
		for (int i = 0; i < w * h; ++i) {
			depth_norm[i] = 0.0f;
		}
		flat = true;
		return true;
	}
	const float inv_range = 1.0f / (dmax - dmin);
	for (int i = 0; i < w * h; ++i) {
		// MiDaS outputs inverse depth: near reads high already.
		const float d = (depth_norm[i] - dmin) * inv_range;
		depth_norm[i] = invert ? (1.0f - d) : d;
	}
	flat = false;
	if (!s.toast_active) {
		s.toast_active = true;
		OPENXR_Notify("Neural depth active");
	}
	return true;
}
#endif // C_NEURAL

// Called by OpenGlRenderer::PresentFrame() after rendering the mirror
// image and before the buffer swap, while the backbuffer is valid.
// Copies the DOS draw area into every eye slice of the acquired
// swapchain image. No-op when no XR frame is in flight.
void OPENXR_BlitMirrorToSwapchain()
{
	if (sdl_xr_active_image < 0 || sdl_xr_swapchain_images.empty() ||
	    sdl_xr_view_count == 0) {
		return;
	}
	if (sdl.render_backend_type != RenderBackendType::OpenGl ||
	    !sdl.renderer) {
		return;
	}
	const uint32_t image_index = static_cast<uint32_t>(sdl_xr_active_image);
	if (image_index >= sdl_xr_swapchain_images.size()) {
		return;
	}
	// uint32_t (not GLuint): keeps this TU compiling when C_OPENGL
	// is off, as glad types are unavailable there.
	const auto dst_tex = sdl_xr_swapchain_images[image_index].image;
	if (!dst_tex) {
		return;
	}

	const int eye_w = static_cast<int>(sdl_xr_swapchain_width);
	const int eye_h = static_cast<int>(sdl_xr_swapchain_height);
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

	const std::string stereo_mode =
	        get_xr_section()->GetString("xr_stereo_input");
	const bool eye_swap = get_xr_section()->GetBool("xr_eye_swap");
	const int xr_disparity = get_xr_section()->GetInt("xr_disparity");
	const bool is_anaglyph    = (stereo_mode == "anaglyph");
	const bool is_alternating = (stereo_mode == "alternating");
	const bool is_split       = (stereo_mode == "interleaved" ||
                               stereo_mode == "top-bottom" ||
                               stereo_mode == "sbs");
	const bool is_depth       = (stereo_mode == "depth");
	const bool is_autodepth   = (stereo_mode == "autodepth");

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
	     !is_autodepth) ||
	    (is_depth && !sdl_xr_depth_valid)) {
		// Mono fast path: same picture in both eyes, pure GPU blit.
		// Also the fallback when depth mode has no fresh depth frame
		// (e.g. non-Voodoo content): the mirror already shows it.
		for (uint32_t eye = 0; eye < sdl_xr_view_count && eye < 2; ++eye) {
			gl_renderer->BlitBackbufferToTexture(dst_tex,
			                                     static_cast<int>(eye),
			                                     sdl.draw.draw_rect_px,
			                                     eye_dst_rect(eye,
			                                                  fit_rect),
			                                     eye_w,
			                                     eye_h);
		}
	} else if (is_depth) {
		// Depth-synthesized views from the Voodoo hook.
		for (uint32_t eye = 0; eye < sdl_xr_view_count && eye < 2; ++eye) {
			gl_renderer->UploadAndBlitEye(static_cast<int>(eye),
			                              xr_eye_buf[eye].data(),
			                              xr_eye_buf_w,
			                              xr_eye_buf_h,
			                              dst_tex,
			                              static_cast<int>(eye),
			                              eye_dst_rect(eye, fit_rect),
			                              eye_w,
			                              eye_h);
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
					xr_autodepth_map_w = cw;
					xr_autodepth_map_h = ch;
				}
				// Neural submode: reuse the latest worker map when
				// ready, else the heuristic cues for this frame.
				bool use_neural_map = false;
				bool flat           = false;
#if C_NEURAL
				if (get_xr_section()->GetString(
				            "xr_autodepth_method") == "neural") {
					xr_neural_submit_frame(frame.data(), cw, ch);
					xr_neural_poll_missing_toast();
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
#endif
				if (!use_neural_map) {
					flat = xr_estimate_depth_auto(
					        frame.data(),
					        cw,
					        ch,
					        xr_autodepth_map.data());
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
				     eye < sdl_xr_view_count && eye < 2;
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

	// Pointer ray overlay (per eye, over the finished eye image).
	if (get_xr_section()->GetBool("xr_ray") &&
	    (sdl_xr_controller_mouse || sdl_xr_keyboard_visible)) {
		XrVec3 ray_from = {}, ray_dir = {}, ray_hit = {};
		float ray_u = 0.0f, ray_v = 0.0f;
		if (openxr_hand_ray(1, ray_from, ray_dir, ray_hit, ray_u, ray_v)) {
			(void)ray_dir;
			(void)ray_u;
			(void)ray_v;
			const float from_xyz[3] = {ray_from.x, ray_from.y, ray_from.z};
			const float to_xyz[3]   = {ray_hit.x, ray_hit.y, ray_hit.z};
			for (uint32_t eye = 0;
			     eye < sdl_xr_view_count && eye < 2;
			     ++eye) {
				float mvp[16] = {};
				openxr_ray_mvp(eye, mvp);
				gl_renderer->DrawXrRayLine(mvp,
				                          from_xyz,
				                          to_xyz,
				                          dst_tex,
				                          static_cast<int>(eye),
				                          eye_w,
				                          eye_h);
			}
		}
	}

	const auto blit_us = GetTicksDiff(GetTicksUs(), blit_t0_us);
	if (blit_us > 0) {
		sdl_xr_stat_blit_us += static_cast<uint64_t>(blit_us);
	}
}

static void cleanup_openxr()
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
	if (sdl_xr_stick_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_stick_action);
		sdl_xr_stick_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_joya_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_joya_action);
		sdl_xr_joya_action = XR_NULL_HANDLE;
	}
	if (sdl_xr_joyb_action != XR_NULL_HANDLE) {
		xrDestroyAction(sdl_xr_joyb_action);
		sdl_xr_joyb_action = XR_NULL_HANDLE;
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
#endif

static SDL_Point minimum_window_size = {640, 480};

DosBox::Rect to_rect(const SDL_Rect r)
{
	return {r.x, r.y, r.w, r.h};
}

SDL_Rect to_sdl_rect(const DosBox::Rect& r)
{
	return {iroundf(r.x), iroundf(r.y), iroundf(r.w), iroundf(r.h)};
}

#if C_DEBUGGER

static bool is_debugger_event(const SDL_Event& event)
{
	if (!dbg.win_main) {
		return false;
	}

	const auto dbg_window_id   = SDL_GetWindowID(dbg.win_main);
	const auto event_window    = SDL_GetWindowFromEvent(&event);
	const auto event_window_id = SDL_GetWindowID(event_window);

	return event_window_id == dbg_window_id;
}
#endif

// Detects if we're running within a desktop environment (or window manager).
bool GFX_HaveDesktopEnvironment()
{
// On BSD and Linux, it's possible that the user is running directly on the
// console without a windowing environment. For example, SDL can directly
// interface with the host's OpenGL/GLES drivers, the console's frame buffer, or
// the Raspberry Pi's DISPMANX driver.
//
#if defined(BSD) || defined(LINUX)
	// The presence of any of the following variables set by either the
	// login manager, display manager, or window manager itself is
	// sufficient evidence to say the user has a desktop session.
	//
	// References:
	// https://www.freedesktop.org/software/systemd/man/pam_systemd.html#desktop=
	// https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#recognized-keys
	// https://askubuntu.com/questions/72549/how-to-determine-which-window-manager-and-desktop-environment-is-running
	// https://unix.stackexchange.com/questions/116539/how-to-detect-the-desktop-environment-in-a-bash-script

	static bool already_checked          = false;
	static bool have_desktop_environment = false;

	if (!already_checked) {
		constexpr const char* EnvVars[] = {"XDG_CURRENT_DESKTOP",
		                                   "XDG_SESSION_DESKTOP",
		                                   "DESKTOP_SESSION",
		                                   "GDMSESSION"};

		auto get_variable = [](const char* variable) {
			return std::getenv(variable);
		};
		have_desktop_environment = std::any_of(std::begin(EnvVars),
		                                       std::end(EnvVars),
		                                       get_variable);

		already_checked = true;
	}

	return have_desktop_environment;
#else
	// Assume we have a desktop environment on all other systems
	return true;
#endif
}

SDL_Window* GFX_GetWindow()
{
	return sdl.window;
}

double GFX_GetHostRefreshRate()
{
	assert(sdl.window);

	const auto display_in_use = SDL_GetDisplayForWindow(sdl.window);

	constexpr auto DefaultHostRefreshRateHz = 60;

	if (display_in_use == 0) {
		LOG_ERR("SDL: Could not get the current display ID: %s",
		        SDL_GetError());
		return DefaultHostRefreshRateHz;
	}

	const auto* mode = SDL_GetCurrentDisplayMode(display_in_use);
	if (!mode) {
		LOG_ERR("SDL: Could not get the current display mode: %s",
		        SDL_GetError());
		return DefaultHostRefreshRateHz;
	}
	if (mode->refresh_rate < RefreshRateMin) {
		LOG_WARNING("SDL: Got a strange refresh rate of %g Hz",
		            mode->refresh_rate);
		return DefaultHostRefreshRateHz;
	}

	assert(mode->refresh_rate >= RefreshRateMin);
	return mode->refresh_rate;
}

static void validate_vsync_and_presentation_mode_settings()
{
	const std::string vsync_pref = get_sdl_section()->GetString("vsync");

	const std::string presentation_mode_pref = get_sdl_section()->GetString(
	        "presentation_mode");

	if (presentation_mode_pref == "dos-rate" &&
	    (has_true(vsync_pref) || vsync_pref == "fullscreen-only")) {

		NOTIFY_DisplayWarning(Notification::Source::Console,
		                      "DISPLAY",
		                      "DISPLAY_INVALID_VSYNC_SETTING",
		                      vsync_pref.c_str());

		set_section_property_value("sdl", "vsync", "off");
	}
}

// Reset and populate the vsync settings from the config. This is called
// on-demand after startup and on output mode changes (e.g., switching from
// the 'texture' backend to 'opengl').
//
static void configure_vsync()
{
	const std::string vsync_pref = get_sdl_section()->GetString("vsync");

	if (has_false(vsync_pref)) {
		sdl.vsync.windowed   = false;
		sdl.vsync.fullscreen = false;

	} else if (has_true(vsync_pref)) {
		sdl.vsync.windowed   = true;
		sdl.vsync.fullscreen = true;

	} else {
		assert(vsync_pref == "fullscreen-only");

		sdl.vsync.windowed   = false;
		sdl.vsync.fullscreen = true;
	}
}

static void configure_presentation_mode()
{
	using enum PresentationMode;

	const std::string presentation_mode_pref = get_sdl_section()->GetString(
	        "presentation_mode");

	if (presentation_mode_pref == "dos-rate") {
		sdl.presentation.windowed_mode   = DosRate;
		sdl.presentation.fullscreen_mode = DosRate;

	} else if (presentation_mode_pref == "host-rate") {
		sdl.presentation.windowed_mode   = HostRate;
		sdl.presentation.fullscreen_mode = HostRate;

	} else {
		assert(presentation_mode_pref == "auto");

		sdl.presentation.windowed_mode = sdl.vsync.windowed ? HostRate
		                                                    : DosRate;

		sdl.presentation.fullscreen_mode = sdl.vsync.fullscreen ? HostRate
		                                                        : DosRate;
	}
}

static void configure_renderer()
{
	const std::string output = get_sdl_section()->GetString("output");

	if (output == "texture") {
		sdl.render_backend_type = RenderBackendType::Sdl;
		sdl.texture_filter_mode = TextureFilterMode::Bilinear;

	} else if (output == "texturenb") {
		sdl.render_backend_type = RenderBackendType::Sdl;
		sdl.texture_filter_mode = TextureFilterMode::NearestNeighbour;

#if C_OPENGL
	} else if (output == "opengl") {
		sdl.render_backend_type = RenderBackendType::OpenGl;
#endif

	} else if (output == "openxr") {
#if C_OPENGL
		// OpenXR sessions bind to the live OpenGL context
		// (XR_KHR_opengl_enable), so the mirror window runs on the
		// OpenGL backend to provide it.
		sdl.render_backend_type = RenderBackendType::OpenGl;
#else
		sdl.render_backend_type = RenderBackendType::Sdl;
		sdl.texture_filter_mode = TextureFilterMode::Bilinear;
#endif
		LOG_INFO("SDL: Using OpenXR output mode with mirror window");
	}
}

void GFX_RequestExit(const bool pressed)
{
	if (pressed) {
		DOSBOX_RequestShutdown();
		LOG_DEBUG("SDL: Exit requested");
	}
}

// Thin wrapper around the FSM. The state machine + `paused_tick()` in
// `dosbox.cpp` own the actual pause loop; here we just translate the
// hotkey press into the right transition.
//
[[maybe_unused]] static void pause_emulation(bool pressed)
{
	if (!pressed) {
		return;
	}

	// Toggle on the requested state, not the actual one. During the
	// pending fade-out window `DOSBOX_IsPaused()` is still false, so
	// keying off it would map a second press to another (no-op) pause
	// request. Keying off the request lets a quick second press cancel
	// the pause during the fade, and guarantees the hotkey can always
	// exit a pending pause even if the fade never completes (e.g. the
	// mixer thread wedged on a stalled audio device).
	if (DOSBOX_IsPauseRequested()) {
		DOSBOX_RequestUserResume();
	} else {
		DOSBOX_RequestUserPause();
	}

	// Titlebar refresh happens inside the pause FSM on the
	// `Paused`/`Running` transition.
}

bool GFX_IsPaused()
{
	return DOSBOX_IsPaused();
}

void GFX_Stop()
{
	if (sdl.draw.updating_framebuffer) {
		GFX_EndUpdate();
	}
	sdl.draw.active = false;
}

void GFX_ResetScreen()
{
	GFX_Stop();
	if (sdl.draw.callback) {
		(sdl.draw.callback)(GFX_CallbackReset);
	}

	CPU_ResetAutoAdjust();

	// The callback's `update_viewport()` may have triggered an auto-shader
	// switch (e.g. resize crossed the scan-doubling threshold while the
	// mapper was open). Push the new shader's `force_single_scan` into
	// `vga.draw.scan_doubling_allowed` here so the `VGA_SetupDrawing()` call
	// below sees it, otherwise `render.src.height` will stay wrong.
	//
	// This matters even while paused: `RENDER_RescaleLastFrame()` below reads
	// `render.src.height` as its destination height for the height
	// doubling/halving of the last latched frame. Stale height will mean
	// stride mismatch, resulting in garbage output.
	RENDER_SetScanAndPixelDoubling();

	VGA_SetupDrawing(0);
	GFX_Start();

	// While paused, we are not producing fresh frames, so the just recreated
	// renderer would draw a blank or stale stretched framebuffer until
	// resume. Drive a synthetic rescale of the latched last completed source
	// frame at the new dimensions, then present immediately so the held frame
	// shows correctly after viewport dimension updates (resizing the window,
	// fullscreen/windowed toggle, etc.)
	//
	if (DOSBOX_IsPaused()) {
		RENDER_RescaleLastFrame();
		GFX_MaybePresentFrame();
	}
}

static bool is_vsync_enabled()
{
	return sdl.is_fullscreen ? sdl.vsync.fullscreen : sdl.vsync.windowed;
}

PresentationMode GFX_GetPresentationMode()
{
	return sdl.is_fullscreen ? sdl.presentation.fullscreen_mode
	                         : sdl.presentation.windowed_mode;
}

DosBox::Rect GFX_CalcDrawRectInPixels(const DosBox::Rect& canvas_size_px)
{
	const DosBox::Rect render_size_px = {sdl.draw.render_width_px,
	                                     sdl.draw.render_height_px};

	const auto r = RENDER_CalcDrawRectInPixels(canvas_size_px,
	                                           render_size_px,
	                                           sdl.draw.render_pixel_aspect_ratio);

	return {iroundf(r.x), iroundf(r.y), iroundf(r.w), iroundf(r.h)};
}

static void maybe_log_presentation_and_vsync_mode()
{
	static std::optional<PresentationMode> last_presentation_mode = {};
	static std::optional<bool> last_vsync_enabled                 = {};

	const auto presentation_mode = GFX_GetPresentationMode();
	const auto vsync_enabled     = is_vsync_enabled();

	if (presentation_mode == last_presentation_mode &&
	    vsync_enabled == last_vsync_enabled) {
		return;
	}

	const auto presentation_rate = [&]() -> std::string {
		switch (presentation_mode) {
		case PresentationMode::DosRate: return "DOS rate";

		case PresentationMode::HostRate:
			return format_str("%2.5g Hz host rate",
			                  GFX_GetHostRefreshRate());

		default: assertm(false, "Invalid PresentationMode"); return "";
		}
	}();

	LOG_MSG("DISPLAY: Presenting at %s %s vsync",
	        presentation_rate.c_str(),
	        (vsync_enabled ? "with" : "without"));

	last_presentation_mode = presentation_mode;
	last_vsync_enabled     = vsync_enabled;
}

static void maybe_log_display_properties()
{
	assert(sdl.renderer);
	assert(sdl.draw.render_width_px > 0 && sdl.draw.render_height_px > 0);

	static DosBox::Rect last_draw_size_px = {};

	const auto canvas_size_px = sdl.renderer->GetCanvasSizeInPixels();
	const auto draw_size_px   = GFX_CalcDrawRectInPixels(canvas_size_px);

	assert(draw_size_px.HasPositiveSize());

	const auto refresh_rate = VGA_GetRefreshRate();

	if (sdl.maybe_video_mode) {
		const auto video_mode = *sdl.maybe_video_mode;

		static VideoMode last_video_mode               = {};
		static double last_refresh_rate                = 0.0;
		static PresentationMode last_presentation_mode = {};
		static bool last_width_was_doubled             = false;
		static bool last_height_was_doubled            = false;
		static Fraction last_pixel_aspect_ratio        = {};

		if (last_video_mode != video_mode ||
		    last_refresh_rate != refresh_rate ||
		    last_presentation_mode != GFX_GetPresentationMode() ||
		    last_draw_size_px.w != draw_size_px.w ||
		    last_draw_size_px.h != draw_size_px.h ||
		    last_width_was_doubled != sdl.draw.width_was_doubled ||
		    last_height_was_doubled != sdl.draw.height_was_doubled ||
		    last_pixel_aspect_ratio != sdl.draw.render_pixel_aspect_ratio) {

			const auto& par = video_mode.pixel_aspect_ratio;

			LOG_MSG("DISPLAY: %s at %2.5g Hz, scaled to %dx%d pixels "
			        "with 1:%1.6g (%d:%d) pixel aspect ratio",
			        to_string(video_mode).c_str(),
			        refresh_rate,
			        iroundf(draw_size_px.w),
			        iroundf(draw_size_px.h),
			        par.Inverse().ToDouble(),
			        static_cast<int32_t>(par.Num()),
			        static_cast<int32_t>(par.Denom()));

			maybe_log_presentation_and_vsync_mode();

			last_video_mode         = video_mode;
			last_refresh_rate       = refresh_rate;
			last_presentation_mode  = GFX_GetPresentationMode();
			last_draw_size_px       = draw_size_px;
			last_width_was_doubled  = sdl.draw.width_was_doubled;
			last_height_was_doubled = sdl.draw.height_was_doubled;
			last_pixel_aspect_ratio = sdl.draw.render_pixel_aspect_ratio;
		}

	} else {
		if (last_draw_size_px.w != draw_size_px.w ||
		    last_draw_size_px.h != draw_size_px.h) {

			LOG_MSG("DISPLAY: Unknown video mode at %2.5g Hz, "
			        "scaled to %dx%d pixels",
			        refresh_rate,
			        iroundf(draw_size_px.w),
			        iroundf(draw_size_px.h));

			maybe_log_presentation_and_vsync_mode();

			last_draw_size_px = draw_size_px;
		}
	}

#if 0
	const auto scale_x = static_cast<double>(draw_size_px.w) / render_width_px;
	const auto scale_y = static_cast<double>(draw_size_px.h) / render_height_px;

	const auto one_per_render_pixel_aspect = scale_y / scale_x;

	LOG_MSG("DISPLAY: render_width_px: %d, render_height_px: %d, "
	        "render pixel aspect ratio: 1:%1.3g",
	        render_width_px,
	        render_height_px,
	        one_per_render_pixel_aspect);
#endif
}

static void setup_presentation_mode()
{
	auto update_frame_time = [](const double rate_hz) {
		assert(rate_hz > 0);

		const auto frame_time_ms       = 1000.0 / rate_hz;
		sdl.presentation.frame_time_us = ifloor(frame_time_ms * 1000.0);
	};

	switch (GFX_GetPresentationMode()) {
	case PresentationMode::DosRate: {
		update_frame_time(VGA_GetRefreshRate());

		// In 'dos-rate' mode, we just present the frame whenever it's
		// ready, so the duration of the window doesn't matter if it's
		// large enough to allow for the frame time jitter.
		//
		sdl.presentation.early_present_window_us = sdl.presentation.frame_time_us;
	} break;

	case PresentationMode::HostRate: {
		update_frame_time(GFX_GetHostRefreshRate());

		// The primary use case for the 'host-rate' mode is a fixed
		// refresh rate monitor running at 60 Hz with vsync enabled
		// (with vsync off, we might as well just use 'dos-rate'). In
		// this scenario, we need to present the frame a bit before the
		// vsync happens, otherwise we'd "miss the train" and would have
		// to wait for an extra frame period. This would increase
		// latency and possibly cause audio glitches because it's a
		// blocking wait, so it's better to be a bit generous with the
		// time window.
		//
		// This value was determined by experimentation on our supported
		// OSes. We might turn this a config setting if there's enough
		// evidence that no single single value works well on all
		// systems, but so far it seems to do the job.
		//
		sdl.presentation.early_present_window_us = 3000;
	} break;
	}

	sdl.presentation.last_present_time_us = 0;
}

static void notify_new_mouse_screen_params()
{
	if (sdl.draw.draw_rect_px.w <= 0 || sdl.draw.draw_rect_px.h <= 0) {
		// Filter out unusual parameters, which can be the result
		// of window minimized due to ALT+TAB, for example
		return;
	}

	MouseScreenParams params = {};

	// It is important to scale not just the size of the rectangle but also
	// its starting point by the inverse of the DPI scale factor.
	params.draw_rect = to_rect(sdl.draw.draw_rect_px).Copy().Scale(1.0f / sdl.dpi_scale);

	float abs_x = 0.0f;
	float abs_y = 0.0f;
	SDL_GetMouseState(&abs_x, &abs_y);

	params.x_abs = abs_x;
	params.y_abs = abs_y;

	params.is_fullscreen    = sdl.is_fullscreen;
	int num_displays = 0;
	SDL_GetDisplays(&num_displays);
	params.is_multi_display = (num_displays > 1);

	MOUSE_NewScreenParams(params);
}

static bool is_aspect_ratio_correction_enabled()
{
	return (RENDER_GetAspectRatioCorrectionMode() ==
	        AspectRatioCorrectionMode::Auto);
}

static void set_minimum_window_size()
{
	assert(sdl.window);

	// TODO This only works for 320x200 games. We cannot make hardcoded
	// assumptions about aspect ratios in general, e.g. the pixel aspect
	// ratio is 1:1 for 640x480 games both with 'aspect = on' and 'aspect =
	// off'.
	const auto minimum_height = (is_aspect_ratio_correction_enabled() ? 480 : 400);

	constexpr auto MinimumWidth = 640;

	minimum_window_size = {iround(MinimumWidth), iround(minimum_height)};

	// The SDL documentation is incorrect; this will set the minimum window
	// size in logical units, not pixels.
	if (!SDL_SetWindowMinimumSize(sdl.window,
	                         minimum_window_size.x,
	                         minimum_window_size.y)) {
		LOG_WARNING("SDL: Failed to set window minimum size: %s", SDL_GetError());
	}

	// LOG_INFO("SDL: Updated window minimum size to %dx%d", width, height);
}

static void check_and_handle_dpi_change(SDL_Window* sdl_window,
                                        [[maybe_unused]] const int _new_width = 0)
{
	assert(sdl_window);

	const auto new_dpi_scale = SDL_GetWindowDisplayScale(sdl_window);

	if (std::abs(new_dpi_scale - sdl.dpi_scale) < FLT_EPSILON) {
		log_window_event("SDL: DPI scale hasn't changed (still %g)",
		                 sdl.dpi_scale);
		return;
	}

	log_window_event("SDL: DPI scale updated from %g to %g",
	                 sdl.dpi_scale,
	                 new_dpi_scale);

	sdl.dpi_scale = new_dpi_scale;
}

static void set_window_transparency()
{
	assert(sdl.window);

	const auto transparency = get_sdl_section()->GetInt("window_transparency");
	const auto alpha = static_cast<float>(100 - transparency) / 100.0f;

	if (!SDL_SetWindowOpacity(sdl.window, alpha)) {
		LOG_WARNING("SDL: Failed to set window opacity: %s", SDL_GetError());
	}
}

static void set_window_decorations()
{
	assert(sdl.window);

	if (!SDL_SetWindowBordered(sdl.window,
	                      get_sdl_section()->GetBool("window_decorations"))) {
		LOG_WARNING("SDL: Failed to set window border: %s", SDL_GetError());
	}
}

static void enter_fullscreen()
{
	assert(sdl.window);

	sdl.is_fullscreen = true;

	if (sdl.fullscreen.mode == FullscreenMode::ForcedBorderless) {

		// "Emulate" SDL's built-in borderless fullscreen mode by
		// turning off window decorations and resizing the window to
		// cover the entire desktop. But this would trigger exclusive
		// fullscreen on Windows so we'd be no better off -- the trick
		// is to size the window one pixel wider than the desktop so
		// fullscreen optimisation won't kick in.
		//
		SDL_Rect display_bounds = {};
		if (!SDL_GetDisplayBounds(sdl.display_number, &display_bounds)) {
			LOG_WARNING("SDL: Failed to get display bounds: %s", SDL_GetError());
			return;
		}

		SDL_GetWindowSize(sdl.window,
		                  &sdl.fullscreen.prev_window.width,
		                  &sdl.fullscreen.prev_window.height);

		SDL_GetWindowPosition(sdl.window,
		                      &sdl.fullscreen.prev_window.x_pos,
		                      &sdl.fullscreen.prev_window.y_pos);

		SDL_SetWindowBordered(sdl.window, false);
		SDL_SetWindowResizable(sdl.window, false);
		if (!SDL_SetWindowPosition(sdl.window, 0, 0)) {
			LOG_WARNING("SDL: Failed to set window position: %s", SDL_GetError());
		}

		if (!SDL_SetWindowSize(sdl.window,
		                  display_bounds.w + 1,
		                  display_bounds.h)) {
			LOG_WARNING("SDL: Failed to set window size: %s", SDL_GetError());
		}

		// Disable transparency in fullscreen mode
		SDL_SetWindowOpacity(sdl.window, 1.0f);

		maybe_log_display_properties();

	} else {
		const auto fullscreen = (sdl.fullscreen.mode == FullscreenMode::Standard);

		if (!SDL_SetWindowFullscreen(sdl.window, fullscreen)) {
			LOG_WARNING("SDL: Failed to set fullscreen: %s", SDL_GetError());
		}
	}

	// We need to disable transparency in fullscreen on macOS
	SDL_SetWindowOpacity(sdl.window, 1.0f);
}

static void exit_fullscreen()
{
	assert(sdl.window);

	sdl.is_fullscreen = false;

	if (sdl.fullscreen.mode == FullscreenMode::ForcedBorderless) {
		// Restore the previous window state when exiting our "fake"
		// borderless fullscreen mode.
		//
		set_window_decorations();

		SDL_SetWindowResizable(sdl.window, true);

		if (!SDL_SetWindowSize(sdl.window,
		                  sdl.fullscreen.prev_window.width,
		                  sdl.fullscreen.prev_window.height)) {
			LOG_WARNING("SDL: Failed to set window size: %s", SDL_GetError());
		}

		if (!SDL_SetWindowPosition(sdl.window,
		                      sdl.fullscreen.prev_window.x_pos,
		                      sdl.fullscreen.prev_window.y_pos)) {
			LOG_WARNING("SDL: Failed to set window position: %s", SDL_GetError());
		}

		set_window_transparency();

		maybe_log_display_properties();

	} else {
		if (!SDL_SetWindowFullscreen(sdl.window, false)) {
			LOG_WARNING("SDL: Failed to exit fullscreen: %s", SDL_GetError());
		}

		// On macOS, SDL_SetWindowSize() and SDL_SetWindowPosition()
		// calls in fullscreen mode are no-ops, so we need to set the
		// potentially changed window size and position when exiting
		// fullscreen mode.
		SDL_SetWindowSize(sdl.window, sdl.windowed.width, sdl.windowed.height);

		SDL_SetWindowPosition(sdl.window,
		                      sdl.windowed.x_pos,
		                      sdl.windowed.y_pos);
	}

	// We need to disable transparency in fullscreen on macOS
	set_window_transparency();

	set_window_decorations();
}

DosBox::Rect GFX_GetCanvasSizeInPixels()
{
	assert(sdl.renderer);
	return sdl.renderer->GetCanvasSizeInPixels();
}

RenderBackend* GFX_GetRenderer()
{
	assert(sdl.renderer);
	return sdl.renderer.get();
}

RenderBackendType GFX_GetRenderBackendType()
{
	return sdl.render_backend_type;
}

static SDL_Rect get_desktop_size()
{
	SDL_Rect desktop = {};
	assert(sdl.display_number > 0);

	if (!SDL_GetDisplayBounds(sdl.display_number, &desktop)) {
		LOG_ERR("SDL: Could not get display bounds for display number %d: %s", sdl.display_number, SDL_GetError());
		// Return a safe default
		desktop.w = 640;
		desktop.h = 480;
		return desktop;
	}

	// Deduct the border decorations from the desktop size
	int top    = 0;
	int left   = 0;
	int bottom = 0;
	int right  = 0;

	if (SDL_GetWindowBordersSize(sdl.window, &top, &left, &bottom, &right)) {
		// If SDL_GetWindowBordersSize succeeds, deduct borders
		desktop.w -= (left + right);
		desktop.h -= (top + bottom);
	}

	assert(desktop.w >= minimum_window_size.x);
	assert(desktop.h >= minimum_window_size.y);
	return desktop;
}

DosBox::Rect GFX_GetDesktopSize()
{
	return to_rect(get_desktop_size());
}

DosBox::Rect GFX_GetViewportSizeInPixels()
{
	assert(sdl.renderer);

	const auto canvas_size_px = sdl.renderer->GetCanvasSizeInPixels();

	return RENDER_CalcRestrictedViewportSizeInPixels(canvas_size_px);
}

float GFX_GetDpiScaleFactor()
{
	return sdl.dpi_scale;
}

static bool is_using_kmsdrm_driver()
{
	assert(SDL_WasInit(SDL_INIT_VIDEO));

	const auto driver = SDL_GetCurrentVideoDriver();
	if (!driver) {
		return false;
	}

	std::string driver_str = driver;
	lowcase(driver_str);
	return driver_str == "kmsdrm";
}

static bool check_kmsdrm_setting()
{
	// Do we have read access to the event subsystem
	if (auto f = fopen("/dev/input/event0", "r"); f) {
		fclose(f);
		return true;
	}

	// We're using KMSDRM, but we don't have read access to the event
	// subsystem
	return false;
}

[[maybe_unused]] static bool operator!=(const SDL_Point lhs, const SDL_Point rhs)
{
	return lhs.x != rhs.x || lhs.y != rhs.y;
}

static void update_viewport()
{
	assert(sdl.renderer);

	const auto canvas_size_px = sdl.renderer->GetCanvasSizeInPixels();

	auto notify_viewport_size_changed = [&]() {
		const auto draw_rect_px = GFX_CalcDrawRectInPixels(canvas_size_px);
		sdl.draw.draw_rect_px = to_sdl_rect(draw_rect_px);

		sdl.renderer->NotifyViewportSizeChanged(draw_rect_px);
	};

	// TODO come up with a better design for the adaptive shader switching
	//
	// The normal flow when using adaptive CRT shaders is that we call the
	// `NotifyViewportSize()` method of the render backend, which then
	// handles shader preset switching based on the current restricted
	// viewport size. That works well when only changing the shader *preset*
	// is required because all CRT shaders enforce integer scaling. But
	// there's an edge case when we switch from a CRT shader to the 'sharp'
	// shader:
	//
	// 1. When we calculate the draw area with `GFX_CalcDrawRectInPixels()`,
	// that calls `RENDER_CalcDrawRectInPixels()`, which in turn queries the
	// current shader in 'auto' integer scaling mode by calling
	// `GetCurrentShaderInfo()` of the render backend. The problem is the
	// current shader is *still* the CRT shader at this point, which
	// enforces integer scaling. Thus we'll get the draw area restricted by
	// integer scaling based on the currently active CRT shader, *not* the
	// full viewport area which we'd expect for the 'sharp' shader to work
	// correclty (as it doesn't enforce integer scaling; it should fit the
	// image to the viewport).
	//
	// 2. This results in a restricted draw area being set, and the shader
	// switching only happens *after that*. The end result is the 'sharp'
	// shader is effectively integer scaled based on the integer scaling
	// imposed by the previously active the CRT shader.
	//
	// ---
	//
	// We have a chicken-and-egg problem here; the root cause is that
	// `NotifyViewportSize()` of the render backend takes the *restricted*
	// draw area size, not the full viewport size. Ideally, it should take
	// the unrestricted full viewport size, then it should perform the
	// shader switching and the restricted viewport size calculation
	// (potentially) in the correct order. That's a job for the render
	// backend.
	//
	// For now, calling `NotifyViewportSizeChanged()` a second time if the
	// current shader has been auto-switched to another shader fixes the
	// problem, but it's a bit of a bandaid solution.
	//
	// The proper fix will require a major redesign:
	//
	// - The interaction between ShaderManager and the RenderBackend is a
	//   bit messy at the moment. ShaderManager should probably only do
	//   shader and preset loading and nothing else, then we should have a
	//   separate ShaderAutoSwitcher class to separate the two different
	//   concerns.
	//
	// - The integer scaling handling should be moved into another utility
	//   class out of render.cpp, then the render backend should use this to
	//   restrict the viewport size if the current shader requires it (as
	//   opposed to sdl_gui.cpp and ShaderManager both calling RENDER_*
	//   methods that perform viewport size restriction). Only the render
	//   backend should "drive" the integer scaling restriction handling
	//   when it's notified of a viewport size change, then everybody else
	//   should just ask the render backend for the current resticted
	//   viewport size.
	//
	const auto curr_shader = sdl.renderer->GetCurrentShaderInfo();

	notify_viewport_size_changed();

	const auto new_shader     = sdl.renderer->GetCurrentShaderInfo();
	const auto shader_changed = (curr_shader.name != new_shader.name);

	if (shader_changed) {
		notify_viewport_size_changed();
	}
}

void GFX_SetSize(const int render_width_px, const int render_height_px,
                 const Fraction& render_pixel_aspect_ratio,
                 const bool double_width, const bool double_height,
                 const VideoMode& video_mode, GFX_Callback_t callback)
{
	assert(sdl.renderer);

	if (sdl.draw.updating_framebuffer) {
		GFX_EndUpdate();
	}

	GFX_Stop();
	// The rendering objects are recreated below with new sizes, after which
	// frame rendering is re-engaged with the output-type specific calls.

	sdl.draw.render_width_px           = render_width_px;
	sdl.draw.render_height_px          = render_height_px;
	sdl.draw.width_was_doubled         = double_width;
	sdl.draw.height_was_doubled        = double_height;
	sdl.draw.render_pixel_aspect_ratio = render_pixel_aspect_ratio;

	sdl.maybe_video_mode = video_mode;

	sdl.draw.callback = callback;

	sdl.renderer->NotifyRenderSizeChanged(sdl.draw.render_width_px,
	                                      sdl.draw.render_height_px);
	update_viewport();
	setup_presentation_mode();

	// Ensure mouse emulation knows the current parameters
	notify_new_mouse_screen_params();

	maybe_log_display_properties();

	GFX_Start();
}

void GFX_CenterMouse()
{
	assert(sdl.renderer);
	assert(sdl.window);

	int width  = 0;
	int height = 0;

	SDL_GetWindowSize(sdl.window, &width, &height);

	SDL_WarpMouseInWindow(sdl.window, static_cast<float>(width) / 2.0f, static_cast<float>(height) / 2.0f);
}

void GFX_SetMouseRawInput([[maybe_unused]] const bool requested_raw_input)
{
	// TODO Raw mouse input is unsupported (and not needed) under SDL3. If this
	// remains the case, remove the associated config option and related code.
}

static void set_keyboard_capture()
{
	assert(sdl.window);

	const auto pref = get_sdl_section()->GetString("keyboard_capture");

	// In 'auto' mode, the keyboard follows the mouse
	const auto capture_keyboard = (pref == "auto") ? sdl.is_mouse_captured
	                                               : has_true(pref);

	if (!SDL_SetWindowKeyboardGrab(sdl.window, capture_keyboard)) {
		LOG_WARNING("SDL: Failed to set keyboard grab: %s", SDL_GetError());
	}
}

void GFX_SetMouseCapture(const bool requested_capture)
{
	if (!SDL_SetWindowRelativeMouseMode(sdl.window, requested_capture)) {
		SDL_ShowCursor();

		E_Exit("SDL: Error %s relative mode",
		       requested_capture ? "putting the mouse into"
		                         : "taking the mouse out of");
	}

	sdl.is_mouse_captured = requested_capture;

	set_keyboard_capture();
}

void GFX_SetMouseVisibility(const bool requested_visible)
{
	if (requested_visible) {
		if (!SDL_ShowCursor()) {
			LOG_WARNING("SDL: Error making mouse cursor visible");
		}
	} else {
		if (!SDL_HideCursor()) {
			LOG_WARNING("SDL: Error making mouse cursor invisible");
		}
	}
}

static void focus_input()
{
	assert(sdl.window);

	// Ensure we have input focus when in fullscreen
	if (!sdl.is_fullscreen) {
		return;
	}
	// Do we already have focus?
	if (SDL_GetWindowFlags(sdl.window) & SDL_WINDOW_INPUT_FOCUS) {
		return;
	}

	// If not, raise-and-focus to prevent stranding the window
	if (!SDL_RaiseWindow(sdl.window)) {
		LOG_WARNING("SDL: Failed to raise window: %s", SDL_GetError());
	}
	if (!SDL_RaiseWindow(sdl.window)) {
		LOG_WARNING("SDL: Failed to set window input focus: %s", SDL_GetError());
	}
}

static void toggle_fullscreen()
{
	assert(sdl.renderer);

	// Record the window's current canvas size if we're departing window-mode
	if (!sdl.is_fullscreen) {
		sdl.windowed.canvas_size = to_sdl_rect(
		        sdl.renderer->GetCanvasSizeInPixels());
	}

	if (sdl.is_fullscreen) {
		exit_fullscreen();
	} else {
		enter_fullscreen();
	}

	set_section_property_value("sdl", "fullscreen", sdl.is_fullscreen ? "on" : "off");

	focus_input();
	setup_presentation_mode();

	maybe_log_display_properties();
}

static void toggle_fullscreen_handler(bool pressed)
{
	if (pressed) {
		toggle_fullscreen();
	}
}

// The function should return a writeable buffer for the VGA emulation to
// render the current framebuffer image into. The buffer was sized for the
// current DOS video mode by a preceding `GFX_SetSize()` call.
//
// `pitch` is the number of bytes used to store a single row of pixel data
// (can be larger than the actual image width in bytes).
//
// The renderer (`render.cpp`) calls this iff the contents of the current
// emulated VGA framebuffer has changed from the last frame (even it's just a
// single pixel), otherwise running the scalers and submitting the new frame
// to the render backend can be optimised away (common scenario in DOS games
// with low frame rates and mostly static screens, e.g., most RPG, adventure,
// and strategy titles).
//
// So we have two scenarios:
//
// 1. Current frame is the same as the previous one:
//
//    - `GFX_StartUpdate()` is NOT called for this frame.
//    - `GFX_EndUpdate()` IS called; `sdl.draw.updating_framebuffer` is FALSE.
//
// 2. Current frame contains changes since the previous one:
//
//    - `GFX_StartUpdate()` IS called for this frame.
//    - `GFX_EndUpdate()` IS called; `sdl.draw.updating_framebuffer` is TRUE.
//
bool GFX_StartUpdate(uint32_t*& pixels, int& pitch)
{
	assert(sdl.renderer);

	if (!sdl.draw.active || sdl.draw.updating_framebuffer) {
		return false;
	}

	sdl.renderer->StartFrame(pixels, pitch);

	sdl.draw.updating_framebuffer = true;
	return true;
}

// Called at the end of each frame at the emulated DOS rate, *regardless* of
// whether contents of the framebuffer have changed or not compared to the
// prevoius frame.
void GFX_EndUpdate()
{
	assert(sdl.renderer);

	if (sdl.draw.updating_framebuffer) {
		// `sdl.draw.updating_framebuffer` is true when the contents of
		// the framebuffer has been changed compared to the previous
		// frame.
		//
		// We're making a copy of the framebuffer as we might present it
		// a bit later in 'host-rate' mode, otherwise the VGA emulation
		// could partially overwrite it by the time we present it (this
		// would introduce tearing even with vsync enabled!)
		//
		// Also, we're not updating the texture here yet because if
		// frames are skiped due to host vs DOS refresh mismatch, we
		// don't want to upload the texture for the skipped frames.
		//
		sdl.renderer->EndFrame();
	}

	if (GFX_GetPresentationMode() == PresentationMode::DosRate) {

		// In 'dos-rate' presentation mode, we present the frames as
		// soon as they're ready. This caters for the VRR monitor use
		// case where effectively our present rate controls the refresh
		// rate of the monitor.
		//
		// The `GFX_EndUpdate` is called at the end of each frame, so at
		// regular intervals close to the refresh rate of the emulated
		// DOS video mode. There is some jitter in the 1-5 ms range, but
		// the timing seems to work well enough in practice on VRR
		// monitors (we can certainly achieve 100% smooth scrolling on
		// better VRR displays).
		//
		// However, this jitter might cause flicker and less than
		// perfect smooth scrolling on some VRR monitor & driver
		// combinations. We should try to get as close as possible to
		// the DOS rate in the future, down to microsecond accuracy
		// (e.g., by tightening the timing accuracy of the PIC timers as
		// VGA updates are timed by abusing the emulated PIC timers,
		// then we might also need to do some additional sleep & busy
		// waiting before present to hit the exact time).
		//
		// Updating the new texture to the GPU also takes some non-zero
		// time, so we'll probably need to introduce an extra fixed
		// latency to account for this delay, and possibly make
		// adjustments in the audio emulation layer to keep the video
		// and audio in perfect sync.
		//
		GFX_MaybePresentFrame();
	}

	// 'host-rate' present is handled in `normal_loop()` in `dosbox.cpp` in
	// a "cooperative-multitasking" fashion at the end of each emulated 1ms
	// tick.
	sdl.draw.updating_framebuffer = false;
}

uint32_t GFX_MakePixel(const uint8_t red, const uint8_t green, const uint8_t blue)
{
	assert(sdl.renderer);

	return sdl.renderer->MakePixel(red, green, blue);
}

void GFX_Start()
{
	sdl.draw.active = true;
}

static void gui_destroy()
{
	GFX_Stop();

	if (sdl.draw.callback) {
		(sdl.draw.callback)(GFX_CallbackStop);
	}
}

void GFX_Destroy()
{
	gui_destroy();
	MAPPER_Destroy();
}

static SDL_Point refine_window_size(const SDL_Point size,
                                    const bool wants_aspect_ratio_correction)
{
	// TODO This only works for 320x200 games. We cannot make hardcoded
	// assumptions about aspect ratios in general, e.g. the pixel aspect
	// ratio is 1:1 for 640x480 games both with 'aspect = on' and 'aspect =
	// off'.
	constexpr SDL_Point RatiosForStretchedPixels = {4, 3};
	constexpr SDL_Point RatiosForSquarePixels    = {8, 5};

	const auto image_aspect = wants_aspect_ratio_correction
	                                ? RatiosForStretchedPixels
	                                : RatiosForSquarePixels;

	const auto window_aspect = static_cast<double>(size.x) / size.y;

	const auto game_aspect = static_cast<double>(image_aspect.x) /
	                         image_aspect.y;

	// Window is wider than the emulated image, so constrain horizonally
	if (window_aspect > game_aspect) {
		const int x = ceil_sdivide(size.y * image_aspect.x, image_aspect.y);
		return {x, size.y};
	} else {
		// Window is narrower than the emulated image, so constrain
		// vertically
		const int y = ceil_sdivide(size.x * image_aspect.y, image_aspect.x);
		return {size.x, y};
	}

	return minimum_window_size;
}

static void maybe_limit_requested_resolution(int& w, int& h,
                                             const char* size_description)
{
	const auto desktop = get_desktop_size();
	if (w <= desktop.w && h <= desktop.h) {
		return;
	}

	bool was_limited = false;

	// Add any driver / platform / operating system limits in succession:

	// SDL KMSDRM limitations
	if (is_using_kmsdrm_driver()) {
		w = desktop.w;
		h = desktop.h;

		was_limited = true;

		LOG_WARNING("DISPLAY: Limiting '%s' resolution to %dx%d to avoid kmsdrm issues",
		            size_description,
		            w,
		            h);
	}

	if (!was_limited) {
		// TODO shouldn't we log the display resolution in physical
		// pixels instead?
		LOG_INFO(
		        "DISPLAY: Accepted '%s' resolution %dx%d despite exceeding "
		        "the %dx%d display",
		        size_description,
		        w,
		        h,
		        desktop.w,
		        desktop.h);
	}
}

static SDL_Point parse_window_resolution_from_conf(const std::string& pref)
{
	int w = 0;
	int h = 0;

	const bool was_parsed = sscanf(pref.c_str(), "%dx%d", &w, &h) == 2;

	const bool is_valid = (w >= minimum_window_size.x &&
	                       h >= minimum_window_size.y);

	if (was_parsed && is_valid) {
		maybe_limit_requested_resolution(w, h, "window");
		return {w, h};
	}

	// TODO convert to notification
	LOG_WARNING(
	        "DISPLAY: Invalid 'window_size' setting: '%s', "
	        "using 'default'",
	        pref.c_str());

	return minimum_window_size;
}

static SDL_Point window_bounds_from_label(const std::string& pref,
                                          const SDL_Rect desktop)
{
	constexpr int SmallPercent  = 50;
	constexpr int MediumPercent = 74;
	constexpr int LargePercent  = 90;

	const int percent = [&] {
		if (pref.starts_with('s')) {
			return SmallPercent;

		} else if (pref.starts_with('m') || pref == "default" ||
		           pref.empty()) {
			return MediumPercent;

		} else if (pref.starts_with('l')) {
			return LargePercent;

		} else if (pref == "desktop") {
			return 100;

		} else {
			// TODO convert to notification
			LOG_WARNING(
			        "DISPLAY: Invalid 'window_size' setting: '%s', "
			        "using 'default'",
			        pref.c_str());
			return MediumPercent;
		}
	}();

	const int w = ceil_sdivide(desktop.w * percent, 100);
	const int h = ceil_sdivide(desktop.h * percent, 100);

	return {w, h};
}

static SDL_Point clamp_to_minimum_window_dimensions(SDL_Point size)
{
	const auto w = std::max(size.x, minimum_window_size.x);
	const auto h = std::max(size.y, minimum_window_size.y);
	return {w, h};
}

static std::optional<SDL_Point> parse_window_position_conf(const std::string& window_position_val)
{
	if (window_position_val == "auto") {
		return {};
	}

	int x, y;
	const auto was_parsed = (sscanf(window_position_val.c_str(), "%d,%d", &x, &y) ==
	                         2);
	if (!was_parsed) {
		// TODO convert to notification
		LOG_WARNING(
		        "DISPLAY: Invalid 'window_position' setting: '%s'. "
		        "Must be in X,Y format, using 'auto'.",
		        window_position_val.c_str());
		return {};
	}

	const auto desktop = get_desktop_size();

	const bool is_out_of_bounds = x < 0 || x > desktop.w || y < 0 ||
	                              y > desktop.h;
	if (is_out_of_bounds) {
		// TODO convert to notification
		LOG_WARNING(
		        "DISPLAY: Invalid 'window_position' setting: '%s'. "
		        "Requested position is outside the bounds of the %dx%d "
		        "desktop, using 'auto'.",
		        window_position_val.c_str(),
		        desktop.w,
		        desktop.h);
		return {};
	}

	return SDL_Point{x, y};
}

static void save_window_position(const int x, const int y)
{
	if (sdl.fullscreen.mode == FullscreenMode::ForcedBorderless) {
		sdl.fullscreen.prev_window.x_pos = x;
		sdl.fullscreen.prev_window.y_pos = y;
	} else {
		sdl.windowed.x_pos = x;
		sdl.windowed.y_pos = y;
	}
}

static void save_default_window_position()
{
	if (sdl.fullscreen.mode == FullscreenMode::ForcedBorderless) {
		sdl.fullscreen.prev_window.x_pos = SDL_WINDOWPOS_UNDEFINED_DISPLAY(
		        sdl.display_number);

		sdl.fullscreen.prev_window.y_pos = SDL_WINDOWPOS_UNDEFINED_DISPLAY(
		        sdl.display_number);
	} else {
		sdl.windowed.x_pos = SDL_WINDOWPOS_UNDEFINED_DISPLAY(sdl.display_number);

		sdl.windowed.y_pos = SDL_WINDOWPOS_UNDEFINED_DISPLAY(sdl.display_number);
	}
}

// Writes to the window-size member should be done via this function
static void save_window_size(const int w, const int h)
{
	assert(w > 0 && h > 0);

	// `sdl.window` size stores the user-configured window size. During
	// runtime, the actual SDL window size might differ from this depending
	// on the aspect ratio, window DPI, or manual resizing.
	sdl.windowed.width  = w;
	sdl.windowed.height = h;

	// Initialize the window's canvas size if it hasn't yet been set.
	if (sdl.windowed.canvas_size.w <= 0 || sdl.windowed.canvas_size.h <= 0) {
		sdl.windowed.canvas_size.w = w;
		sdl.windowed.canvas_size.h = h;
	}

	set_section_property_value("sdl", "window_size", format_str("%dx%d", w, h));
}

void GFX_SaveCurrentWindowSizeAndPosition()
{
	if (sdl.is_fullscreen) {
		return;
	}

	SDL_Rect r = {};

	SDL_GetWindowPosition(sdl.window, &r.x, &r.y);
	SDL_GetWindowSize(sdl.window, &r.w, &r.h);

	save_window_position(r.x, r.y);
	save_window_size(r.w, r.h);
}

static void handle_window_size_pref_after_config_load()
{
	assert(control);

	constexpr auto SectionName    = "sdl";
	constexpr auto LegacyPrefName = "windowresolution";
	constexpr auto NewPrefName    = "window_size";

	const auto legacy_pref_order = control->GetSettingParseOrder(SectionName,
	                                                             LegacyPrefName);

	const auto new_pref_order = control->GetSettingParseOrder(SectionName,
	                                                          NewPrefName);

	if (legacy_pref_order > new_pref_order) {
		const auto legacy_pref = get_sdl_section()->GetString(LegacyPrefName);

		// Move setting from the legacy setting into the new one
		set_section_property_value(SectionName, NewPrefName, legacy_pref);
	}

	set_section_property_value(SectionName, LegacyPrefName, "");
}

// Takes in:
//  - The user's window_size setting: default, WxH, small, medium, large,
//    desktop, or an invalid setting.
//  - If aspect correction is requested.
//
// This function returns a refined size and additionally populates the
// following struct members:
//
//  - 'sdl.window', with the refined size.
//
static void configure_window_size()
{
	const auto window_size_pref = get_sdl_section()->GetString("window_size");

	// Get the coarse resolution from the users setting, and adjust
	// refined scaling mode if an exact resolution is desired.
	SDL_Point coarse_size = minimum_window_size;

	const auto use_exact_window_resolution = window_size_pref.find('x') !=
	                                         std::string::npos;

	if (use_exact_window_resolution) {
		coarse_size = parse_window_resolution_from_conf(window_size_pref);
	} else {
		const auto desktop = get_desktop_size();

		coarse_size = window_bounds_from_label(window_size_pref, desktop);
	}

	// Refine the coarse resolution and save it in the SDL struct.
	auto refined_size = coarse_size;

	if (use_exact_window_resolution) {
		refined_size = clamp_to_minimum_window_dimensions(coarse_size);
	} else {
		refined_size = refine_window_size(coarse_size,
		                                  is_aspect_ratio_correction_enabled());
	}

	assert(refined_size.x <= UINT16_MAX && refined_size.y <= UINT16_MAX);

	save_window_size(refined_size.x, refined_size.y);

	// Let the user know the resulting window properties
	LOG_MSG("DISPLAY: Using %dx%d window size in windowed mode on display-%d",
	        refined_size.x,
	        refined_size.y,
	        sdl.display_number);
}

static void set_window_size()
{
	if (sdl.fullscreen.mode == FullscreenMode::ForcedBorderless &&
	    sdl.is_fullscreen) {

		sdl.fullscreen.prev_window.width = sdl.windowed.width;

		sdl.fullscreen.prev_window.height = sdl.windowed.height;
	} else {
		SDL_SetWindowSize(sdl.window, sdl.windowed.width, sdl.windowed.height);
	}
}

static void save_window_position_from_conf()
{
	if (const auto pos = parse_window_position_conf(
	            get_sdl_section()->GetString("window_position"));
	    pos) {

		save_window_position(pos->x, pos->y);
	} else {
		save_default_window_position();
	}
}

TextureFilterMode GFX_GetTextureFilterMode()
{
	return sdl.texture_filter_mode;
}

static SDL_WindowFlags get_sdl_window_flags()
{
	SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

	if (!get_sdl_section()->GetBool("window_decorations")) {
		flags |= SDL_WINDOW_BORDERLESS;
	}

	if (sdl.is_fullscreen) {
		switch (sdl.fullscreen.mode) {
		case FullscreenMode::Standard:
			flags |= SDL_WINDOW_FULLSCREEN;
			break;
		case FullscreenMode::ForcedBorderless:
			// no-op
			break;
		default: assertm(false, "Invalid FullscreenMode");
		}
	}

	return flags;
}

static RenderBackend* create_renderer()
{
#if C_OPENGL
	if (sdl.render_backend_type == RenderBackendType::OpenGl) {
		try {
			return new OpenGlRenderer(sdl.windowed.x_pos,
			                          sdl.windowed.y_pos,
			                          sdl.windowed.width,
			                          sdl.windowed.height,
			                          get_sdl_window_flags());

		} catch (const std::runtime_error& ex) {
			LOG_WARNING(
			        "OPENGL: Error initialising OpenGL renderer, "
			        "falling back to SDL renderer");

			// GL attributes are global and can affect SDL's texture
			// renderer as it can use OpenGL internally as a
			// backend. This is done in OpenGlRenderer's destructor
			// but we caught an exception during construction so the
			// destructor will not be run here.
			SDL_GL_ResetAttributes();

			sdl.render_backend_type = RenderBackendType::Sdl;
			set_section_property_value("sdl", "output", "texture");
		}
	}
#endif

	if (sdl.render_backend_type == RenderBackendType::Sdl) {
		try {
			std::string render_driver = get_sdl_section()->GetString(
			        "texture_renderer");
			lowcase(render_driver);

			return new SdlRenderer(sdl.windowed.x_pos,
			                       sdl.windowed.y_pos,
			                       sdl.windowed.width,
			                       sdl.windowed.height,
			                       get_sdl_window_flags(),
			                       render_driver,
			                       sdl.texture_filter_mode);

		} catch (const std::runtime_error& ex) {
			E_Exit("SDL: Could not initialize SDL render backend");
		}
	}

	assert(false);
	return nullptr;
}

// Window-inactive coordinator. Single entry point for everything that
// should happen when the host window loses focus.
static void on_window_inactive()
{
	MOUSE_NotifyWindowActive(false);

	if (sdl.mute_when_inactive) {
		// No-op if the user has manually muted
		MIXER_RequestAutoMute();
	}
	if (sdl.pause_when_inactive) {
		KEYBOARD_ClrBuffer();
		DOSBOX_RequestAutoPause();
	}
}

// Window-active coordinator.
//
// `focus_gained` is true for the FOCUS_GAINED SDL window event and false for
// WINDOW_RESTORED" and WINDOW_EXPOSED. Only FOCUS_GAINED re-applies host-side
// state (keyboard grab, mouse-active, and auto-unmute).
//
// Pause auto-resume runs on all three events, so a minimized/uncovered window
// comes back up.
//
static void on_window_active(const bool focus_gained)
{
	if (sdl.pause_when_inactive) {
		DOSBOX_RequestAutoResume();

		// ALT can stick on focus loss -- `paused_tick` doesn't
		// process key events normally, so release on the "user is
		// back" edge regardless of which active-event got us here.
		KEYBOARD_AddKey(KBD_leftalt, false);
		KEYBOARD_AddKey(KBD_rightalt, false);
	}
	if (!focus_gained) {
		return;
	}

	MOUSE_NotifyWindowActive(true);

	if (sdl.mute_when_inactive) {
		// No-op if the user has manually muted -- the mute FSM
		// preserves UserMuted across focus changes.
		MIXER_RequestAutoUnmute();
	}

	// At least on some platforms grabbing the keyboard has to be repeated
	// each time we regain focus
	set_keyboard_capture();
}

static void restart_hotkey_handler([[maybe_unused]] bool pressed)
{
	DOSBOX_Restart();
}

static void configure_fullscreen_mode()
{
	const auto section = get_sdl_section();

	sdl.is_fullscreen = control->arguments.fullscreen ||
	                    section->GetBool("fullscreen");

	const auto fullscreen_mode_pref = section->GetString("fullscreen_mode");

	if (fullscreen_mode_pref == "standard") {
		sdl.fullscreen.mode = FullscreenMode::Standard;
	} else if (fullscreen_mode_pref == "forced-borderless") {
		sdl.fullscreen.mode = FullscreenMode::ForcedBorderless;
	}
}

static void configure_display()
{
	const int display = get_sdl_section()->GetInt("display");

	int num_displays = 0;
	SDL_DisplayID *displays = SDL_GetDisplays(&num_displays);
	if ((display >= 0) && (display < num_displays)) {
		sdl.display_number = displays[display];
		assert(sdl.display_number != 0);
	} else {
		// TODO convert to notification
		LOG_WARNING("SDL: Display number out of bounds, using display 0");
		sdl.display_number = displays[0];
	}
}

static void set_allow_screensaver()
{
	const std::string screensaver = get_sdl_section()->GetString("screensaver");
	if (screensaver == "allow") {
		if (!SDL_EnableScreenSaver()) {
			LOG_WARNING("SDL: Failed to enable screensaver: %s", SDL_GetError());
		}
	} else {
		if (!SDL_DisableScreenSaver()) {
			LOG_WARNING("SDL: Failed to disable screensaver: %s", SDL_GetError());
		}
	}
}

static void configure_pause_and_mute_when_inactive()
{
	sdl.pause_when_inactive = get_sdl_section()->GetBool("pause_when_inactive");

	sdl.mute_when_inactive = (!sdl.pause_when_inactive) &&
	                         get_sdl_section()->GetBool("mute_when_inactive");
}

static void set_sdl_hints()
{
	// Seamless mouse integration feels more 'seamless' if mouse
	// clicks on unfocused windows are passed to the guest.
	SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

	// We have a keyboard shortcut to exit the fullscreen mode,
	// so we don't necessary need the ALT+TAB shortcut
	SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");

#if defined SDL_HINT_APP_NAME
	// For KDE 6 volume applet and PipeWire audio driver; further
	// SetHint calls have no effect in the GUI, only the first
	// advertised name is used.
	SDL_SetHint(SDL_HINT_APP_NAME, DOSBOX_NAME);
#endif

#if defined SDL_HINT_AUDIO_DEVICE_STREAM_NAME
	// Useful for 'pw-top' and possibly other PipeWire CLI tools.
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, DOSBOX_NAME);
#endif
}

#if C_OPENXR
static void xr_image_scale_adjust(const bool zoom_in, const bool pressed)
{
	if (!pressed) {
		return;
	}
	int size_pct = get_xr_section()->GetInt("xr_picture_size");
	size_pct += (zoom_in ? 10 : -10);
	if (size_pct > 100) {
		size_pct = 100;
	}
	if (size_pct < 20) {
		size_pct = 20;
	}
	set_section_property_value("xr", "xr_picture_size", format_str("%d", size_pct));
	OPENXR_Notify(format_str("Picture size %d%%", size_pct));
}

static void xr_zoom_in_handler(bool pressed)
{
	xr_image_scale_adjust(true, pressed);
}

static void xr_zoom_out_handler(bool pressed)
{
	xr_image_scale_adjust(false, pressed);
}

static void xr_capture_lock_pose()
{
	if (sdl_xr_view_count == 0) {
		LOG_MSG("OpenXR: Recenter ignored, no tracked views yet");
		return;
	}
	for (uint32_t i = 0; i < sdl_xr_view_count && i < 2; ++i) {
		sdl_xr_locked_pose[i] = sdl_xr_views[i].pose;
		sdl_xr_locked_fov[i]  = sdl_xr_views[i].fov;
	}
	sdl_xr_has_lock = true;
}

static void xr_recenter_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	// Anchor the screen where you are looking right now and lock it
	// there.
	xr_capture_lock_pose();
	if (sdl_xr_has_lock) {
		sdl_xr_world_locked = true;
		OPENXR_Notify("Screen recentered and locked");
	}
}

static void xr_lock_toggle_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	if (!sdl_xr_world_locked && !sdl_xr_has_lock) {
		// First enable: anchor where the screen is right now.
		xr_capture_lock_pose();
	}
	sdl_xr_world_locked = !sdl_xr_world_locked;
	OPENXR_Notify(format_str("Screen position lock %s",
	                         sdl_xr_world_locked ? "on" : "off"));
}

static void xr_blank_test_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	sdl_xr_submit_blank = !sdl_xr_submit_blank;
	OPENXR_Notify(format_str("Blank-frame submit test %s",
	                         sdl_xr_submit_blank ? "on (HMD goes dark)"
	                                             : "off"));
}

static void xr_stereo_mode_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	static const char* const modes[] = {"mono",
	                                          "anaglyph",
	                                          "alternating",
	                                          "interleaved",
	                                          "top-bottom",
	                                          "sbs",
	                                          "depth",
	                                          "autodepth"};
	static constexpr int num_modes = sizeof(modes) / sizeof(modes[0]);
	const std::string curr =
	        get_xr_section()->GetString("xr_stereo_input");
	int next = 0;
	for (int i = 0; i < num_modes; ++i) {
		if (curr == modes[i]) {
			next = (i + 1) % num_modes;
			break;
		}
	}
	set_section_property_value("xr", "xr_stereo_input", modes[next]);
	OPENXR_Notify(format_str("Stereo input mode: %s", modes[next]));
}

// Last non-mono stereo mode, for the mono/last toggle below.
static std::string sdl_xr_last_stereo_mode;

// Mono/last-stereo toggle: from any stereo mode, remember it and drop
// to mono; from mono, restore the remembered mode (stays mono when
// nothing was remembered yet).
static void xr_stereo_toggle_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const std::string curr =
	        get_xr_section()->GetString("xr_stereo_input");
	if (curr == "mono") {
		if (!sdl_xr_last_stereo_mode.empty() &&
		    sdl_xr_last_stereo_mode != "mono") {
			set_section_property_value("xr",
			                           "xr_stereo_input",
			                           sdl_xr_last_stereo_mode.c_str());
			OPENXR_Notify(format_str("Stereo input mode: %s",
			                         sdl_xr_last_stereo_mode.c_str()));
		} else {
			OPENXR_Notify("Stereo input mode: mono (no previous mode)");
		}
		return;
	}
	sdl_xr_last_stereo_mode = curr;
	set_section_property_value("xr", "xr_stereo_input", "mono");
	OPENXR_Notify(format_str("Stereo input mode: mono (was %s)",
	                         curr.c_str()));
}

static void xr_autodepth_method_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	static const char* const methods[] = {"blend",
	                                      "gradient",
	                                      "luma"
#if C_NEURAL
	                                      ,
	                                      "neural"
#endif
	                                      };
	static constexpr int num_methods = sizeof(methods) / sizeof(methods[0]);
	const std::string curr =
	        get_xr_section()->GetString("xr_autodepth_method");
	int next = 0;
	for (int i = 0; i < num_methods; ++i) {
		if (curr == methods[i]) {
			next = (i + 1) % num_methods;
			break;
		}
	}
	set_section_property_value("xr", "xr_autodepth_method", methods[next]);
	OPENXR_Notify(format_str("Autodepth method: %s", methods[next]));
}

static void xr_disparity_adjust(const bool increase, const bool pressed)
{
	if (!pressed) {
		return;
	}
	int d = get_xr_section()->GetInt("xr_disparity");
	d += (increase ? 4 : -4);
	if (d > 128) {
		d = 128;
	}
	if (d < -128) {
		d = -128;
	}
	set_section_property_value("xr", "xr_disparity", format_str("%d", d));
	OPENXR_Notify(format_str("Stereo disparity %+d px (%s)",
	                         d,
	                         (d == 0 ? "aligned"
	                                : (d > 0 ? "closer" : "farther"))));
}

static void xr_disparity_down_handler(bool pressed)
{
	xr_disparity_adjust(false, pressed);
}

static void xr_disparity_up_handler(bool pressed)
{
	xr_disparity_adjust(true, pressed);
}

static void xr_depth_strength_adjust(const bool increase, const bool pressed)
{
	if (!pressed) {
		return;
	}
	int pct = get_xr_section()->GetInt("xr_depth_strength");
	pct += (increase ? 2 : -2);
	if (pct > 50) {
		pct = 50;
	}
	if (pct < 0) {
		pct = 0;
	}
	set_section_property_value("xr", "xr_depth_strength", format_str("%d", pct));
	OPENXR_Notify(format_str("Depth strength %d%%%s",
	                         pct,
	                         (pct == 0 ? " (flat)" : "")));
}

static void xr_depth_strength_down_handler(bool pressed)
{
	xr_depth_strength_adjust(false, pressed);
}

static void xr_depth_strength_up_handler(bool pressed)
{
	xr_depth_strength_adjust(true, pressed);
}

static void xr_divisor_adjust(const bool increase, const bool pressed)
{
	if (!pressed) {
		return;
	}
	int d = get_xr_section()->GetInt("xr_submit_divisor");
	d += (increase ? 1 : -1);
	if (d > 8) {
		d = 8;
	}
	if (d < 1) {
		d = 1;
	}
	set_section_property_value("xr", "xr_submit_divisor", format_str("%d", d));
	OPENXR_Notify(format_str("Submit divisor %d (%s)",
	                         d,
	                         (d == 1 ? "every frame" : "reduced rate")));
}

static void xr_divisor_down_handler(bool pressed)
{
	xr_divisor_adjust(false, pressed);
}

static void xr_divisor_up_handler(bool pressed)
{
	xr_divisor_adjust(true, pressed);
}

static void xr_dubois_toggle_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool use_dubois = (get_xr_section()->GetString("xr_anaglyph_method") !=
	                         "dubois");
	set_section_property_value("xr",
	                           "xr_anaglyph_method",
	                           use_dubois ? "dubois" : "simple");
	OPENXR_Notify(format_str("Anaglyph method: %s",
	                         use_dubois ? "dubois (least-squares)"
	                                    : "simple (channel extract)"));
}

static void xr_eye_swap_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool swapped = !get_xr_section()->GetBool("xr_eye_swap");
	set_section_property_value("xr", "xr_eye_swap", swapped ? "on" : "off");
	OPENXR_Notify(format_str("Eye swap %s", swapped ? "on" : "off"));
}

static void xr_flip_vertical_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool flipped = !get_xr_section()->GetBool("xr_flip_vertical");
	set_section_property_value("xr", "xr_flip_vertical", flipped ? "on" : "off");
	OPENXR_Notify(format_str("Vertical flip %s", flipped ? "on" : "off"));
}

static void xr_depth_view_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool shown = !get_xr_section()->GetBool("xr_show_depth");
	set_section_property_value("xr", "xr_show_depth", shown ? "on" : "off");
	OPENXR_Notify(format_str("Depth buffer view %s", shown ? "on" : "off"));
}

static void xr_shutter_sync_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	static const char* const modes[] = {"auto", "parity", "lpt"};
	static constexpr int num_modes = sizeof(modes) / sizeof(modes[0]);
	const std::string curr =
	        get_xr_section()->GetString("xr_shutter_sync");
	int next = 0;
	for (int i = 0; i < num_modes; ++i) {
		if (curr == modes[i]) {
			next = (i + 1) % num_modes;
			break;
		}
	}
	set_section_property_value("xr", "xr_shutter_sync", modes[next]);
	OPENXR_Notify(format_str("Shutter sync: %s", modes[next]));
}
#endif

static void add_default_sdl_section_mapper_bindings()
{
	MAPPER_AddHandler(MAPPER_Run, SDL_SCANCODE_F1, PRIMARY_MOD, "mapper", "Mapper");

	MAPPER_AddHandler(GFX_RequestExit, SDL_SCANCODE_F9, PRIMARY_MOD, "shutdown", "Shutdown");

	MAPPER_AddHandler(toggle_fullscreen_handler,
	                  SDL_SCANCODE_RETURN,
	                  MMOD2,
	                  "fullscr",
	                  "Fullscreen");
	MAPPER_AddHandler(restart_hotkey_handler,
	                  SDL_SCANCODE_HOME,
	                  PRIMARY_MOD | MMOD2,
	                  "restart",
	                  "Restart");

	MAPPER_AddHandler(MOUSE_ToggleUserCapture,
	                  SDL_SCANCODE_F10,
	                  PRIMARY_MOD,
	                  "capmouse",
	                  "Cap Mouse");

#if C_OPENXR
	MAPPER_AddHandler(xr_zoom_in_handler,
	                  SDL_SCANCODE_EQUALS,
	                  PRIMARY_MOD,
	                  "xr_zoomin",
	                  "XR Zoom In");
	MAPPER_AddHandler(xr_zoom_out_handler,
	                  SDL_SCANCODE_MINUS,
	                  PRIMARY_MOD,
	                  "xr_zoomout",
	                  "XR Zoom Out");
	MAPPER_AddHandler(xr_recenter_handler,
	                  SDL_SCANCODE_0,
	                  PRIMARY_MOD,
	                  "xr_recenter",
	                  "XR Recenter");
	MAPPER_AddHandler(xr_lock_toggle_handler,
	                  SDL_SCANCODE_L,
	                  PRIMARY_MOD,
	                  "xr_lock",
	                  "XR Toggle Lock");
	MAPPER_AddHandler(xr_blank_test_handler,
	                  SDL_SCANCODE_B,
	                  PRIMARY_MOD,
	                  "xr_blank",
	                  "XR Blank Test");
	MAPPER_AddHandler(xr_stereo_mode_handler,
	                  SDL_SCANCODE_E,
	                  PRIMARY_MOD,
	                  "xr_stereomode",
	                  "XR Stereo Mode");
	MAPPER_AddHandler(xr_stereo_toggle_handler,
	                  SDL_SCANCODE_T,
	                  PRIMARY_MOD,
	                  "xr_stereotoggle",
	                  "XR Stereo Toggle");
	MAPPER_AddHandler(xr_disparity_down_handler,
	                  SDL_SCANCODE_COMMA,
	                  PRIMARY_MOD,
	                  "xr_disparity_down",
	                  "XR Disparity -");
	MAPPER_AddHandler(xr_disparity_up_handler,
	                  SDL_SCANCODE_PERIOD,
	                  PRIMARY_MOD,
	                  "xr_disparity_up",
	                  "XR Disparity +");
	MAPPER_AddHandler(xr_depth_strength_down_handler,
	                  SDL_SCANCODE_SEMICOLON,
	                  PRIMARY_MOD,
	                  "xr_depth_strength_down",
	                  "XR Depth -");
	MAPPER_AddHandler(xr_depth_strength_up_handler,
	                  SDL_SCANCODE_APOSTROPHE,
	                  PRIMARY_MOD,
	                  "xr_depth_strength_up",
	                  "XR Depth +");
	MAPPER_AddHandler(xr_dubois_toggle_handler,
	                  SDL_SCANCODE_D,
	                  PRIMARY_MOD,
	                  "xr_dubois",
	                  "XR Anaglyph Method");
	MAPPER_AddHandler(xr_shutter_sync_handler,
	                  SDL_SCANCODE_S,
	                  PRIMARY_MOD,
	                  "xr_shuttersync",
	                  "XR Shutter Sync");
	MAPPER_AddHandler(xr_eye_swap_handler,
	                  SDL_SCANCODE_W,
	                  PRIMARY_MOD,
	                  "xr_eyeswap",
	                  "XR Eye Swap");
	MAPPER_AddHandler(xr_flip_vertical_handler,
	                  SDL_SCANCODE_V,
	                  PRIMARY_MOD,
	                  "xr_flip_vertical",
	                  "XR Vertical Flip");
	MAPPER_AddHandler(xr_depth_view_handler,
	                  SDL_SCANCODE_G,
	                  PRIMARY_MOD,
	                  "xr_depth_view",
	                  "XR Depth View");
	MAPPER_AddHandler(xr_autodepth_method_handler,
	                  SDL_SCANCODE_A,
	                  PRIMARY_MOD,
	                  "xr_autodepth_method",
	                  "XR Autodepth");
	MAPPER_AddHandler(xr_divisor_down_handler,
	                  SDL_SCANCODE_LEFTBRACKET,
	                  PRIMARY_MOD,
	                  "xr_divisor_down",
	                  "XR Divisor -");
	MAPPER_AddHandler(xr_divisor_up_handler,
	                  SDL_SCANCODE_RIGHTBRACKET,
	                  PRIMARY_MOD,
	                  "xr_divisor_up",
	                  "XR Divisor +");
	MAPPER_AddHandler(xr_controller_mouse_handler,
	                  SDL_SCANCODE_M,
	                  PRIMARY_MOD,
	                  "xr_mouse",
	                  "XR Mouse");
	MAPPER_AddHandler(xr_keyboard_toggle_handler,
	                  SDL_SCANCODE_K,
	                  PRIMARY_MOD,
	                  "xr_keyboard",
	                  "XR Keyboard");
	MAPPER_AddHandler(xr_ray_toggle_handler,
	                  SDL_SCANCODE_R,
	                  PRIMARY_MOD,
	                  "xr_ray",
	                  "XR Ray");
#endif

#if C_DEBUGGER
	// Pause binds with activate-debugger

#elif defined(MACOSX)
	// Pause/unpause is hardcoded to Command+P on macOS
	MAPPER_AddHandler(&pause_emulation, SDL_SCANCODE_P, PRIMARY_MOD, "pause", "Pause Emu.");
#else
	// Pause/unpause is hardcoded to Alt+Pause on Window & Linux
	MAPPER_AddHandler(&pause_emulation, SDL_SCANCODE_PAUSE, MMOD2, "pause", "Pause Emu.");
#endif
}

#ifdef MACOSX
// Check if a path is a .dosbox document package (directory with .dosbox extension)
static bool is_dosbox_package(const std::string& path)
{
	if (path.empty()) {
		return false;
	}

	std::error_code ec;
	std_fs::path pkg_path(path);

	// Check if path exists and is a directory
	if (!std_fs::is_directory(pkg_path, ec)) {
		return false;
	}

	// Check if it has .dosbox extension
	const auto extension = pkg_path.extension().string();
	return iequals(extension, ".dosbox");
}

// Handle .dosbox package drops from Finder
static void handle_macos_dosbox_package_drop(const std::string& dropped_file_path)
{
	LOG_MSG("CONFIG: Received dropped file via SDL_EVENT_DROP_FILE: '%s'",
	        dropped_file_path.c_str());

	// Check if it's a .dosbox package
	if (!is_dosbox_package(dropped_file_path)) {
		LOG_WARNING("CONFIG: Dropped file is not a .dosbox package, ignoring");
		return;
	}

	LOG_MSG("CONFIG: Detected .dosbox package");

	// Convert to absolute path
	std::error_code ec;
	std_fs::path pkg_path = std_fs::absolute(dropped_file_path, ec);
	if (ec) {
		LOG_WARNING("CONFIG: Failed to convert package path to absolute");
		return;
	}

	// Build new startup parameters with expanded package
	auto new_params = control->startup_params;

	// Add --working-dir to set the package directory
	new_params.emplace_back("--working-dir");
	new_params.emplace_back(pkg_path.string());

	// Use the standard restart mechanism to launch with expanded package
	LOG_MSG("CONFIG: Restarting with expanded package arguments");
	DOSBOX_Restart(new_params);
}
#endif

void GFX_InitSdl()
{
	set_sdl_hints();

#if C_OPENXR
	// OpenXR initialization
	sdl_xr_initialized = false;
	sdl_xr_instance = XR_NULL_HANDLE;
	sdl_xr_session = XR_NULL_HANDLE;
	sdl_xr_swapchain = XR_NULL_HANDLE;
#endif

	// Initialise SDL
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		E_Exit("SDL: Failed to init SDL video and timer: %s", SDL_GetError());
	}

	if (is_using_kmsdrm_driver() && !check_kmsdrm_setting()) {
		E_Exit("SDL: /dev/input/event0 is not readable, quitting early to prevent TTY input lockup.\n"
		       "Please run: 'sudo usermod -aG input $(whoami)', then re-login and try again.");
	}

	// Register custom SDL events
	sdl.start_event_id = SDL_RegisterEvents(enum_val(DosBoxSdlEvent::NumEvents));
	// SDL3 returns 0 on failure (SDL2 returned (Uint32)-1).
	if (sdl.start_event_id == 0) {
		E_Exit("SDL: Error allocating event IDs");
	}

	// Log runtime SDL version
	const auto sdl_version = SDL_GetVersion();

	LOG_MSG("SDL: Version %d.%d.%d initialised",
	        SDL_VERSIONNUM_MAJOR(sdl_version),
	        SDL_VERSIONNUM_MINOR(sdl_version),
	        SDL_VERSIONNUM_MICRO(sdl_version));
	LOG_MSG("SDL: %s video initialised", SDL_GetCurrentVideoDriver());

#ifdef MACOSX
	// Check for .dosbox document packages dropped from Finder
	// (double-click to open or drag-and-drop onto the app icon)

	// Sleep briefly to allow the OS time to queue the drop event before we
	// poll.
	SDL_Delay(100);

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
			const std::string dropped_file_path = event.drop.data;
			handle_macos_dosbox_package_drop(dropped_file_path);
		}
	}
#endif
}

void GFX_InitAndStartGui()
{
	// Configure initial settings
	configure_pause_and_mute_when_inactive();

	configure_fullscreen_mode();
	configure_display();

	validate_vsync_and_presentation_mode_settings();
	configure_vsync();
	configure_presentation_mode();
	configure_renderer();

	save_window_position_from_conf();

	handle_window_size_pref_after_config_load();
	configure_window_size();

	sdl.draw.render_width_px  = minimum_window_size.x;
	sdl.draw.render_height_px = minimum_window_size.y;

	// Create rendering backend and application window
	sdl.renderer = std::unique_ptr<RenderBackend>(create_renderer());
	assert(sdl.renderer);

	sdl.window = sdl.renderer->GetWindow();
	assert(sdl.window);

#ifdef MACOSX
	// The window is not always brought to the foreground after startup with
	// SDL 2.32.10 on macOS, hence this workaround. Both the OpenGL and SDL
	// texture renderers are affected.
	//
	// SDL on Windows and Linux seems to always raise the window after
	// creation.
	//
	// SDL issues:
	//
	// - https://github.com/libsdl-org/SDL/issues/14701
	// - https://github.com/libsdl-org/SDL/issues/13920
	//
	SDL_RaiseWindow(sdl.window);

	// Setting the SDL_WINDOW_BORDERLESS flag on window creation doesn't
	// work on macOS.
	//
	// TODO Remove workaround when the SDL issue
	// https://github.com/libsdl-org/SDL/issues/6172 is resolved.
	//
	set_window_decorations();
#endif

	set_minimum_window_size();

	// Assume focus on startup
	constexpr auto FocusGained = true;
	on_window_active(FocusGained);

	RENDER_SetShaderWithFallback();

	sdl.renderer->SetVsync(is_vsync_enabled());

	set_window_transparency();

	check_and_handle_dpi_change(sdl.window);
	set_allow_screensaver();

	add_default_sdl_section_mapper_bindings();

	// Notify MOUSE subsystem that it can start now
	MOUSE_NotifyReadyGFX();

	TITLEBAR_ReadConfig();

	if (sdl.is_fullscreen &&
	    sdl.fullscreen.mode == FullscreenMode::ForcedBorderless) {
		enter_fullscreen();
	}

	RENDER_Init();

#if C_OPENXR
	// Start OpenXR after the renderer exists so the graphics binding
	// can hook the live window + GL context. The mirror window keeps
	// rendering through the normal path regardless.
	if (get_sdl_section()->GetString("output") == "openxr") {
		init_openxr();
	}
#endif
}

static void notify_sdl_setting_updated(SectionProp& section,
                                       const std::string& prop_name)
{
	assert(sdl.renderer);
	assert(sdl.window);

	if (prop_name == "fullscreen") {
		auto fullscreen_requested = section.GetBool("fullscreen");

		if (sdl.is_fullscreen && !fullscreen_requested) {
			exit_fullscreen();
		} else if (!sdl.is_fullscreen && fullscreen_requested) {
			enter_fullscreen();
		}

	} else if (prop_name == "fullscreen_mode") {
		const auto was_in_fullscreen = sdl.is_fullscreen;
		if (sdl.is_fullscreen) {
			exit_fullscreen();
		}

		configure_fullscreen_mode();

		if (was_in_fullscreen) {
			enter_fullscreen();
		}

	} else if (prop_name == "keyboard_capture") {
		set_keyboard_capture();

	} else if (prop_name == "mapperfile") {
		MAPPER_BindKeys(&section);

	} else if (prop_name == "mute_when_inactive") {
		configure_pause_and_mute_when_inactive();

	} else if (prop_name == "pause_when_inactive") {
		configure_pause_and_mute_when_inactive();

	} else if (prop_name == "presentation_mode") {
		validate_vsync_and_presentation_mode_settings();
		configure_vsync();
		configure_presentation_mode();
		GFX_ResetScreen();

	} else if (prop_name == "screensaver") {
		set_allow_screensaver();

	} else if (prop_name == "vsync") {
		validate_vsync_and_presentation_mode_settings();
		configure_vsync();
		configure_presentation_mode();

		sdl.renderer->SetVsync(is_vsync_enabled());
		maybe_log_presentation_and_vsync_mode();
		GFX_ResetScreen();

	} else if (prop_name == "window_decorations") {
		set_window_decorations();

#if C_OPENGL && defined(MACOSX)
		update_viewport();
		RENDER_SetScanAndPixelDoubling();
		GFX_ResetScreen();
#endif

	} else if (prop_name == "window_position") {
		save_window_position_from_conf();

		if (!sdl.is_fullscreen) {
			SDL_SetWindowPosition(sdl.window,
			                      sdl.windowed.x_pos,
			                      sdl.windowed.y_pos);
		}

	} else if (prop_name == "window_size") {
		configure_window_size();
		set_window_size();

	} else if (prop_name == "windowresolution") {
		// Move setting from the legacy setting into the new one
		constexpr auto LegacyPrefName = "windowresolution";
		constexpr auto NewPrefName    = "window_size";

		const auto legacy_pref = get_sdl_section()->GetString(LegacyPrefName);

		set_section_property_value("sdl", LegacyPrefName, "");
		set_section_property_value("sdl", NewPrefName, legacy_pref);

		configure_window_size();
		set_window_size();

	} else if (prop_name == "window_titlebar") {
		TITLEBAR_ReadConfig();

	} else if (prop_name == "window_transparency") {
		if (!sdl.is_fullscreen) {
			set_window_transparency();
		}

	} else {
		LOG_WARNING("SDL: Runtime change unhandled for property: '%s'",
		            prop_name.c_str());
	}
}

static void handle_mouse_motion(SDL_MouseMotionEvent* motion)
{
	MOUSE_EventMoved(motion->xrel,
	                 motion->yrel,
	                 motion->x,
	                 motion->y);
}

static void handle_mouse_wheel(SDL_MouseWheelEvent* wheel)
{
	const auto tmp = (wheel->direction == SDL_MOUSEWHEEL_NORMAL) ? -wheel->y
	                                                             : wheel->y;
	MOUSE_EventWheel(tmp);
}

static void handle_mouse_button(SDL_MouseButtonEvent* button)
{
	auto notify_button = [](const uint8_t button, const bool pressed) {
		// clang-format off
		switch (button) {
		case SDL_BUTTON_LEFT:   MOUSE_EventButton(MouseButtonId::Left,   pressed); break;
		case SDL_BUTTON_RIGHT:  MOUSE_EventButton(MouseButtonId::Right,  pressed); break;
		case SDL_BUTTON_MIDDLE: MOUSE_EventButton(MouseButtonId::Middle, pressed); break;
		case SDL_BUTTON_X1:     MOUSE_EventButton(MouseButtonId::Extra1, pressed); break;
		case SDL_BUTTON_X2:     MOUSE_EventButton(MouseButtonId::Extra2, pressed); break;
		}
		// clang-format on
	};
	assert(button);
	notify_button(button->button, button->down);
}

void GFX_LosingFocus()
{
	MAPPER_LosingFocus();
}

bool GFX_IsFullscreen()
{
	return sdl.is_fullscreen;
}

static bool is_window_event(const SDL_Event& event)
{
	return (event.type >= SDL_EVENT_WINDOW_FIRST &&
	        event.type <= SDL_EVENT_WINDOW_LAST);
}

static bool is_user_event(const SDL_Event& event)
{
	const auto start_id = sdl.start_event_id;
	const auto end_id   = start_id + enum_val(DosBoxSdlEvent::NumEvents);

	return (event.common.type >= start_id && event.common.type < end_id);
}

static void handle_user_event(const SDL_Event& event)
{
	const auto id = event.common.type - sdl.start_event_id;

	switch (static_cast<DosBoxSdlEvent>(id)) {
	case DosBoxSdlEvent::RefreshAnimatedTitle:
		TITLEBAR_RefreshAnimatedTitle();
		break;

	default: assert(false);
	}
}

int GFX_GetUserSdlEventId(DosBoxSdlEvent event)
{
	return sdl.start_event_id + enum_val(event);
}

static bool handle_sdl_windowevent(const SDL_Event& event)
{
	switch (event.type) {
	case SDL_EVENT_WINDOW_RESTORED: {
		log_window_event("SDL: Window has been restored");

		// We may need to re-create a texture and more on Android.
		// Another case: Update surface while using X11.
		//
		GFX_ResetScreen();

#if C_OPENGL && defined(MACOSX)
		// TODO check if this workaround is still needed

		log_window_event("SDL: Reset macOS's GL viewport after window-restore");

		if (sdl.render_backend_type == RenderBackendType::OpenGl) {
			update_viewport();
		}
#endif
		focus_input();

		constexpr auto FocusGained = false;
		on_window_active(FocusGained);
		return true;
	}

	case SDL_EVENT_WINDOW_RESIZED: {
		// Window dimensions in logical coordinates
		const auto width  = event.window.data1;
		const auto height = event.window.data2;

		log_window_event("SDL: Window has been resized to %dx%d", width, height);

		static int last_width  = 0;
		static int last_height = 0;

		// SDL_EVENT_WINDOW_RESIZED events are sent twice when resizing
		// the window, but maybe_log_display_properties() will only
		// output a log entry if the image dimensions have actually
		// changed.
		maybe_log_display_properties();

		if (!sdl.is_fullscreen) {
			save_window_size(width, height);
		}

		if (width != last_width && height != last_height) {
			maybe_log_display_properties();

			// Needed for aspect & viewport mode combinations where
			// the pixel aspect ratio or viewport size is sized
			// relatively to the window size.
			GFX_ResetScreen();

			last_width  = width;
			last_height = height;
		}
		return true;
	}

	case SDL_EVENT_WINDOW_FOCUS_GAINED:
		log_window_event("SDL: Window has gained keyboard focus");
		[[fallthrough]];

	case SDL_EVENT_WINDOW_EXPOSED: {
		log_window_event("SDL: Window has been exposed and should be redrawn");

		// TODO: below is not consistently true :( seems incorrect on
		// KDE and sometimes on MATE
		//
		// Note that on Windows/Linux-X11/Wayland/macOS, event is fired
		// after toggling between full vs windowed modes. However this
		// is never fired on the Raspberry Pi (when rendering to the
		// Framebuffer); therefore we rely on the FOCUS_GAINED event to
		// catch window startup and size toggles.

		const bool focus_gained = (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
		on_window_active(focus_gained);

		if (sdl.draw.callback) {
			sdl.draw.callback(GFX_CallbackRedraw);
		}
		focus_input();
		return true;
	}

	case SDL_EVENT_WINDOW_FOCUS_LOST:
		log_window_event("SDL: Window has lost keyboard focus");

		// `GFX_LosingFocus()` deactivates mapper events, which can
		// enqueue key-up scancodes; `on_window_inactive()`'s
		// `KEYBOARD_ClrBuffer` must run after that so pause starts
		// with an empty keyboard buffer.
		GFX_LosingFocus();
		on_window_inactive();
		return false;

	case SDL_EVENT_WINDOW_MOUSE_ENTER:
		log_window_event("SDL: Window has gained mouse focus");
		return true;

	case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		log_window_event("SDL: Window has lost mouse focus");
		return true;

	case SDL_EVENT_WINDOW_SHOWN:
		log_window_event("SDL: Window has been shown");
		return true;

	case SDL_EVENT_WINDOW_HIDDEN:
		log_window_event("SDL: Window has been hidden");
		return true;

	case SDL_EVENT_WINDOW_MOVED: {
		const auto x = event.window.data1;
		const auto y = event.window.data2;

		log_window_event("SDL: Window has been moved to %d, %d", x, y);

#if C_OPENGL && defined(MACOSX)
		// TODO This workaround is still needed on macOS 15.6. We'll be
		// able to remove it once we always set the viewport to covert
		// the full window (supporting overlay images and the OSD will
		// necessitate this).
		//
		if (sdl.render_backend_type == RenderBackendType::OpenGl) {
			update_viewport();
		}
#endif
		// We don't allow negative values for 'window_position', so this
		// is the best we can do to keep things in sync.
		const auto new_x = std::max(x, 0);
		const auto new_y = std::max(y, 0);

		if (!sdl.is_fullscreen) {
			save_window_position(new_x, new_y);

			set_section_property_value("sdl",
			                           "window_position",
			                           format_str("%d,%d", new_x, new_y));
		}
		return true;
	}

	case SDL_EVENT_WINDOW_DISPLAY_CHANGED: {
		const auto new_display_number = event.window.data1;
		log_window_event("SDL: Window has been moved to display %d",
		                 new_display_number);

		// New display might have a different resolution and DPI scaling
		// set, so recalculate that and set viewport
		check_and_handle_dpi_change(sdl.window);

		sdl.display_number = new_display_number;

		update_viewport();
		RENDER_SetScanAndPixelDoubling();
		GFX_ResetScreen();

		notify_new_mouse_screen_params();
		return true;
	}

	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
		log_window_event("SDL: The window size has changed");

		// The window size has changed either as a result of an API call
		// or through the system or user changing the window size.
		const auto new_width = event.window.data1;

		check_and_handle_dpi_change(sdl.window, new_width);
		update_viewport();
		RENDER_SetScanAndPixelDoubling();
		GFX_ResetScreen();

		notify_new_mouse_screen_params();
		return true;
	}

	case SDL_EVENT_WINDOW_MINIMIZED:
		log_window_event("SDL: Window has been minimized");

		on_window_inactive();
		return false;

	case SDL_EVENT_WINDOW_MAXIMIZED:
		log_window_event("SDL: Window has been maximized");
		return true;

	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		log_window_event(
		        "SDL: The window manager requests that the window be closed");

		GFX_RequestExit(true);
		return false;

	// case SDL_WINDOWEVENT_TAKE_FOCUS:
	// 	log_window_event("SDL: Window is being offered a focus");

		// 	focus_input();
		// 	on_window_active(/* focus_gained = */ true);
		// 	return true;

	case SDL_EVENT_WINDOW_HIT_TEST:
		log_window_event(
		        "SDL: Window had a hit test that wasn't SDL_HITTEST_NORMAL");
		return true;

	default: return false;
	}
	return false;
}

static void adjust_ticks_after_present_frame(int64_t elapsed_us)
{
	static int64_t cumulative_time_rendered_us = 0;
	cumulative_time_rendered_us += elapsed_us;

	constexpr auto MicrosInMillisecond = 1000;

	if (cumulative_time_rendered_us >= MicrosInMillisecond) {
		// 1 tick == 1 millisecond
		const auto cumulative_ticks_rendered = cumulative_time_rendered_us /
		                                       MicrosInMillisecond;

		DOSBOX_SetTicksDone(DOSBOX_GetTicksDone() - cumulative_ticks_rendered);

		// Keep the fractional microseconds part
		cumulative_time_rendered_us %= MicrosInMillisecond;
	}
}

void GFX_CaptureRenderedImage()
{
	assert(sdl.renderer);

	// The draw rect can extends beyond the bounds of the window or the
	// screen in fullscreen when we're "zooming into" the DOS content in
	// `relative` viewport mode. But rendered captures should always capture
	// what we see on the screen, so only the visible part of the enlarged
	// image. Therefore, we need to clip the draw rect to the bounds of the
	// canvas (the total visible area of the window or screen), and only
	// capture the resulting output rectangle.

	auto canvas_rect_px = sdl.renderer->GetCanvasSizeInPixels();
	canvas_rect_px.x    = 0.0f;
	canvas_rect_px.y    = 0.0f;

	const auto output_rect_px = canvas_rect_px.Copy().Intersect(
	        to_rect(sdl.draw.draw_rect_px));

	auto image = sdl.renderer->ReadPixelsPostShader(output_rect_px);

	assert(sdl.maybe_video_mode);
	image.params.video_mode = *sdl.maybe_video_mode;

	CAPTURE_AddPostRenderImage(image);
}

void GFX_MaybePresentFrame()
{
	assert(sdl.renderer);

	const auto start_us = GetTicksUs();

	// Always present the frame if we want to capture the next
	// rendered frame, regardless of the presentation mode. This is
	// necessary to keep the contents of rendered and raw/upscaled
	// screenshots in sync (so they capture the exact same frame) in
	// multi-output image capture modes.
	const auto force_present = CAPTURE_IsCapturingPostRenderImage();

	const auto curr_frame_time_us =
	        GetTicksDiff(start_us, sdl.presentation.last_present_time_us);

	const auto present_window_start_us = sdl.presentation.frame_time_us -
	                                     sdl.presentation.early_present_window_us;

	if (force_present || (curr_frame_time_us >= present_window_start_us)) {
#if C_OPENXR
		int64_t xr_overhead_us = 0;
#endif

		if (sdl.draw.active) {
#if C_OPENXR
			// Start the XR frame first (throttles to headset vsync);
			// the mirror blit into the swapchain happens pre-swap
			// inside PresentFrame, then the XR frame is submitted.
			// `output` is OnlyAtStart, so cache it; the divisor is
			// live (adjustable via shortcut) and read every frame.
			static const bool use_openxr =
			        (get_sdl_section()->GetString("output") == "openxr");
			const int xr_divisor_raw =
			        get_xr_section()->GetInt("xr_submit_divisor");
			const int xr_divisor = (xr_divisor_raw < 1) ? 1
			                       : ((xr_divisor_raw > 8) ? 8
			                                             : xr_divisor_raw);
			static uint64_t xr_present_count = 0;
			XrTime xr_display_time = 0;
			const auto xr_t0       = GetTicksUs();
			bool xr_frame          = false;
			// Alternating shutter content carries one eye per DOS
			// frame, so it always submits every frame (ignores
			// xr_submit_divisor, which would freeze one eye).
			const int eff_divisor =
			        (use_openxr &&
			         get_xr_section()->GetString("xr_stereo_input") ==
			                 "alternating")
			                ? 1
			                : xr_divisor;
			if (use_openxr && !sdl_xr_disabled) {
				if (!sdl_xr_session_ready) {
					openxr_maybe_recover();
				}
				xr_frame =
				        (sdl_xr_initialized &&
				         (xr_present_count++ %
				          static_cast<uint64_t>(eff_divisor) ==
				          0))
				                ? openxr_frame_begin(xr_display_time)
				                : false;
			}
			xr_overhead_us += GetTicksDiff(GetTicksUs(), xr_t0);
#endif
#if C_OPENXR
			const auto mirror_t0_us = GetTicksUs();
#endif
			sdl.renderer->PrepareFrame();
			sdl.renderer->PresentFrame();
#if C_OPENXR
			if (xr_frame) {
				// Attribute mirror cost to submitted frames only so
				// the average stays comparable across divisors.
				const auto mirror_us = GetTicksDiff(GetTicksUs(),
				                                    mirror_t0_us);
				if (mirror_us > 0) {
					sdl_xr_stat_mirror_us +=
					        static_cast<uint64_t>(mirror_us);
				}
				const auto xr_t1 = GetTicksUs();
				openxr_frame_end(xr_display_time);
				xr_overhead_us += GetTicksDiff(GetTicksUs(), xr_t1);
			}
#endif
		}

		const auto end_us = GetTicksUs();
#if 0
		LOG_TRACE("DISPLAY: present took %2.4f ms", 0.001 * GetTicksDiff(end_us, start_us));

		const auto measured_frame_time_us = GetTicksDiff(
			end_us, sdl.presentation.last_present_time_us);

		LOG_TRACE("DISPLAY: frame time: %2.4f ms", 0.001 * measured_frame_time_us);

		if (measured_frame_time_us >
			sdl.presentation.frame_time_us * 1.5) {
			LOG_WARNING("DISPLAY: missed vsync (long frame)");
		}
#endif
		sdl.presentation.last_present_time_us = end_us;

		// Adjust "ticks done" counter by the time it took to present
		// the frame. XR headset waits are excluded: blocking on the
		// compositor is not mirror-present cost and must not slow the
		// emulated machine down.
		auto present_cost_us = GetTicksDiff(end_us, start_us);
#if C_OPENXR
		present_cost_us -= xr_overhead_us;
		if (present_cost_us < 0) {
			present_cost_us = 0;
		}
#endif
		adjust_ticks_after_present_frame(present_cost_us);
	}

}

// Returns:
//   true  - event loop can keep running
//   false - event loop wants to quit
bool GFX_PollAndHandleEvents()
{
	SDL_Event event = {};

	static auto last_check_joystick = GetTicks();
	auto current_check_joystick     = GetTicks();

	if (GetTicksDiff(current_check_joystick, last_check_joystick) > 20) {
		last_check_joystick = current_check_joystick;

		if (MAPPER_IsUsingJoysticks()) {
			SDL_UpdateJoysticks();
		}
		MAPPER_UpdateJoysticks();
	}

	while (SDL_PollEvent(&event)) {
		
#if C_DEBUGGER
		if (is_debugger_event(event)) {
			if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
			    !DEBUG_IsDebugging()) {
				SDL_HideWindow(dbg.win_main);
				continue;
			}
			DebuggerInputEvent qe;
			qe.ev = event;
			if (event.type == SDL_EVENT_TEXT_INPUT && event.text.text) {
				qe.text = event.text.text;
			}
			debugger_event_queue.push(std::move(qe));
			continue;
		}
#endif
		if (is_user_event(event)) {
			handle_user_event(event);
			continue;
		}

		if (is_window_event(event)) {
			if (handle_sdl_windowevent(event)) {
				continue;
			}
		}

		switch(event.type) {
		case SDL_EVENT_DISPLAY_ADDED:
		case SDL_EVENT_DISPLAY_REMOVED:
			notify_new_mouse_screen_params();
			break;

		case SDL_EVENT_MOUSE_MOTION:
			if (!openxr_controller_mouse_active()) {
				handle_mouse_motion(&event.motion);
			}
			break;
		case SDL_EVENT_MOUSE_WHEEL: handle_mouse_wheel(&event.wheel); break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (!openxr_controller_mouse_active()) {
				handle_mouse_button(&event.button);
			}
			break;

		case SDL_EVENT_QUIT: GFX_RequestExit(true); break;
		default: MAPPER_CheckEvent(&event);
		}
	}
	return !DOSBOX_IsShutdownRequested();
}

static std::vector<std::string> get_sdl_texture_renderers()
{
	const int n = SDL_GetNumRenderDrivers();

	std::vector<std::string> drivers;
	drivers.reserve(n + 1);
	drivers.emplace_back("auto");

	const char *name;

	for (int i = 0; i < n; i++) {
		name = SDL_GetRenderDriver(i);
		if (!name) {
			continue;
		}
		drivers.emplace_back(name);
	}
	return drivers;
}

static void register_sdl_text_messages()
{
	MSG_Add("DISPLAY_INVALID_VSYNC_SETTING",
	        "Invalid [color=light-green]'vsync'[reset] setting: [color=white]'%s'[reset];\n"
	        "vsync cannot be enabled in [color=white]'dos-rate'[reset] presentation mode, "
	        "using [color=white]'off'[reset]");
}

static void init_sdl_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

#if C_OPENGL
	const std::string default_output = "opengl";
#else
	const std::string default_output = "texture";
#endif
	auto pstring = section.AddString("output", OnlyAtStart, default_output.c_str());

	pstring->SetOptionHelp(
	        "opengl_default",
	        "Rendering backend to use for graphics output ('opengl' by default). Only the\n"
	        "'opengl' backend has shader support and is thus the preferred option. The\n"
	        "'texture' backend is only provided as a last resort fallback if OpenGL is not\n"
	        "available or the OpenGL driver is not Core Profile 3.3 compliant. Possible\n"
	        "values:\n");

	pstring->SetOptionHelp("texture_default",
	                       "Rendering backend to use for graphics output ('texture' by default).");

	pstring->SetOptionHelp("opengl",
	                       "  opengl:     OpenGL backend with shader support (default).");
	pstring->SetOptionHelp("texture",
	                       "  texture:    SDL's texture backend with bilinear interpolation.");
pstring->SetOptionHelp("texturenb",
	                       "  texturenb:  SDL's texture backend with nearest-neighbour interpolation\n"
	                       "              (no bilinear).");
	pstring->SetOptionHelp("openxr",
	                       "  openxr:     OpenXR backend with mirror window support.");
#if C_OPENGL
	pstring->SetDeprecatedWithAlternateValue("surface", "opengl");
	pstring->SetDeprecatedWithAlternateValue("openglpp", "opengl");
	pstring->SetDeprecatedWithAlternateValue("openglnb", "opengl");
#else
	pstring->SetDeprecatedWithAlternateValue("surface", "texture");
#endif
	pstring->SetDeprecatedWithAlternateValue("texturepp", "texture");

	pstring->SetValues({
#if C_OPENGL
		"opengl",
#endif
		"texture",
		"texturenb",
		"openxr",
	});
	pstring->SetEnabledOptions({
#if C_OPENGL
		"opengl_default",
		"opengl",
#else
		"texture_default",
		"texture",
		"texturenb",
		"openxr",
#endif
	});

	pstring = section.AddString("texture_renderer", OnlyAtStart, "auto");
	pstring->SetHelp(
	        "Render driver to use in 'texture' output mode ('auto' by default).\n"
	        "Use 'texture_renderer = auto' for an automatic choice.");
	pstring->SetValues(get_sdl_texture_renderers());

	auto pint = section.AddInt("display", OnlyAtStart, 0);
	pint->SetHelp(
	        "Number of display to use; values depend on OS and user "
	        "settings (0 by default).");

	auto pbool = section.AddBool("fullscreen", Always, false);
	pbool->SetHelp("Start in fullscreen mode ('off' by default).");

	pstring = section.AddString("fullresolution", Deprecated, "");
	pstring->SetHelp(
	        "The [color=light-green]'fullresolution'[reset] setting has been removed.\n"
	        "Please use [color=light-green]'fullscreen_mode'[reset] instead.");

	pstring = section.AddString("fullscreen_mode", Always, "standard");
	pstring->SetHelp(
	        "Set fullscreen mode ('standard' by default). Possible values:\n");

	pstring->SetOptionHelp("standard",
	                       "  standard:           Use the standard fullscreen mode of your operating system\n"
	                       "                      (default).\n");

	pstring->SetOptionHelp(
	        "forced-borderless",
	        "  forced-borderless:  Force borderless fullscreen operation if your graphics\n"
	        "                      card driver decides to disable fullscreen optimisation\n"
	        "                      on Windows, resulting in exclusive fullscreen. Forcing\n"
	        "                      borderless mode might result in decreased performance\n"
	        "                      and slightly worse frame pacing (e.g., scrolling in 2D\n"
	        "                      games not appearing perfectly smooth).");
	pstring->SetValues({
	        "standard",
#ifdef WIN32
	        "forced-borderless",
#endif
	});

	pstring->SetDeprecatedWithAlternateValue("desktop", "standard");

	pstring = section.AddString("windowresolution", DeprecatedButAllowed, "");
	pstring->SetHelp(
	        "The [color=light-green]'windowresolution'[reset] setting is deprecated but still accepted;\n"
	        "please use [color=light-green]'window_size'[reset] instead.");

	pstring = section.AddString("window_size", Always, "default");
	pstring->SetHelp(
	        "Set initial window size for windowed mode ('default' by default). You can still\n"
	        "resize the window after startup. Possible values:\n"
	        "\n"
	        "  default:   Select the best option based on your environment and other\n"
	        "             factors (such as whether aspect ratio correction is enabled).\n"
	        "\n"
	        "  small, medium, large (s, m, l):\n"
	        "             Size the window relative to the desktop.\n"
	        "\n"
	        "  WxH:       Specify window size in WxH format in logical units (e.g.,\n"
	        "             1024x768). The values be multiplied by the OS-level DPI scaling to\n"
	        "             get the window size in pixels.\n"
	        "\n"
	        "Note: If you want to use pixel coordinates instead and ignore DPI scaling, set\n"
	        "      the SDL_WINDOWS_DPI_SCALING environment variable to 0.");

	pstring = section.AddString("window_position", Always, "auto");
	pstring->SetHelp(
	        "Set initial window position for windowed mode ('auto' by default).\n"
	        "Possible values:\n"
	        "\n"
	        "  auto:      Let the window manager decide the position (default).\n"
	        "\n"
	        "  X,Y:       Set window position in X,Y format in logical units (e.g., 250,100).\n"
	        "             0,0 is the top-left corner of the screen. The values will be\n"
	        "             multiplied by the OS-level DPI scaling to get the window position\n"
	        "             in pixels.\n"
	        "\n"
	        "Note: If you want to use pixel coordinates instead and ignore DPI scaling, set\n"
	        "      the SDL_WINDOWS_DPI_SCALING environment variable to 0.");

	pbool = section.AddBool("window_decorations", Always, true);
	pbool->SetHelp("Enable window decorations in windowed mode ('on' by default).");

	TITLEBAR_AddConfigSettings();

	pint = section.AddInt("transparency", Deprecated, 0);
	pint->SetHelp("Renamed to [color=light-green]'window_transparency'[reset].");

	pint = section.AddInt("window_transparency", Always, 0);
	pint->SetHelp(
	        "Set the transparency of the DOSBox Staging window (0 by default).\n"
	        "Valid range from 0 (no transparency) to 90 (high transparency).");
	pint->SetMinMax(0, 90);

	pstring = section.AddString("max_resolution", Deprecated, "");
	pstring->SetHelp(
	        "Moved to [color=light-cyan][render][reset] section "
	        "and renamed to [color=light-green]'viewport'[reset].");

	pstring = section.AddString("viewport_resolution", Deprecated, "");
	pstring->SetHelp(
	        "Moved to [color=light-cyan][render][reset] section "
	        "and renamed to [color=light-green]'viewport'[reset].");

	pstring = section.AddString("vsync", Always, "off");
	pstring->SetHelp(
	        "Set the host video driver's vertical synchronization (vsync) mode.\n"
	        "Possible values:\n"
	        "\n"
	        "  off:              Disable vsync in both windowed and fullscreen mode\n"
	        "                    (default). This is the best option on variable refresh rate\n"
	        "                    (VRR) monitors running in VRR mode to get perfect frame\n"
	        "                    pacing, no tearing, and low input lag. On fixed refresh rate\n"
	        "                    monitors (or VRR monitors in fixed refresh mode), disabling\n"
	        "                    vsync might cause visible tearing in fast-paced games.\n"
	        "\n"
	        "  on:               Enable vsync in both windowed and fullscreen mode. This can\n"
	        "                    prevent tearing in fast-paced games but will increase input\n"
	        "                    lag. Vsync is only available with 'host-rate' presentation\n"
	        "                    (see 'presentation_mode').\n"
	        "\n"
	        "  fullscreen-only:  Enable vsync in fullscreen mode only. This might be useful\n"
	        "                    if your operating system enforces vsync in windowed mode and\n"
	        "                    the 'on' setting causes audio glitches or other issues in\n"
	        "                    windowed mode only. Vsync is only available with 'host-rate'\n"
	        "                    presentation (see 'presentation_mode').\n"
	        "\n"
	        "Notes:\n"
	        "  - For perfectly smooth scrolling in 2D games (e.g., in Pinball Dreams\n"
	        "    and Epic Pinball), you'll need a VRR monitor running in VRR mode and 'vsync'\n"
	        "    disabled. The scrolling in 70 Hz VGA games will always appear juddery on\n"
	        "    60 Hz fixed refresh rate monitors even with vsync enabled.\n"
	        "\n"
	        "  - Usually, you'll only get perfectly smooth 2D scrolling in fullscreen mode,\n"
	        "    even on a VRR monitor.\n"
	        "\n"
	        "  - For the best results, disable all frame cappers and global vsync overrides\n"
	        "    in your video driver settings.");
	pstring->SetValues({"off", "on", "fullscreen-only"});

	pstring = section.AddString("presentation_mode", Always, "auto");
	pstring->SetHelp(
	        "Set the frame presentation mode ('auto' by default). Possible values:\n"
	        "\n"
	        "  auto:       Use 'host-rate' if 'vsync' is enabled, otherwise use 'dos-rate'\n"
	        "              (default). See 'vsync' for further details.\n"
	        "\n"
	        "  dos-rate:   Present frames at the refresh rate of the emulated DOS video mode.\n"
	        "              This is the best option on variable refresh rate (VRR) monitors.\n"
	        "              'vsync' is not availabe with 'dos-rate' presentation.\n"
	        "\n"
	        "  host-rate:  Present frames at the refresh rate of the host display. Use this\n"
	        "              with 'vsync' enabled on fixed refresh rate monitors for fast-paced\n"
	        "              games where tearing is a problem. 'host-rate' combined with\n"
	        "              'vsync' disabled can be a good workaround on systems that always\n"
	        "              enforce blocking vsync at the OS level (e.g., forced 60 Hz vsync\n"
	        "              could cause problems with VGA games presenting frames at 70 Hz).");
	pstring->SetValues({"auto", "dos-rate", "host-rate"});

	auto pmulti = section.AddMultiVal("capture_mouse", Deprecated, ",");
	pmulti->SetHelp(
	        "Moved to [color=light-cyan][mouse][reset] section and "
	        "renamed to [color=light-green]'mouse_capture'[reset].");

	pmulti = section.AddMultiVal("sensitivity", Deprecated, ",");
	pmulti->SetHelp(
	        "Moved to [color=light-cyan][mouse][reset] section and "
	        "renamed to [color=light-green]'mouse_sensitivity'[reset].");

	pbool = section.AddBool("raw_mouse_input", Deprecated, false);
	pbool->SetHelp(
	        "Moved to [color=light-cyan][mouse][reset] section and "
	        "renamed to [color=light-green]'mouse_raw_input'[reset].");

	pbool = section.AddBool("waitonerror", Deprecated, true);
	pbool->SetHelp("The [color=light-green]'waitonerror'[reset] setting has been removed.");

	pstring = section.AddString("priority", Deprecated, "");
	pstring->SetHelp(
	        "The [color=light-green]'priority'[reset] setting has been removed.");

	pbool = section.AddBool("mute_when_inactive", Always, false);
	pbool->SetHelp("Mute the sound when the window is inactive ('off' by default).");

	pbool = section.AddBool("pause_when_inactive", Always, false);
	pbool->SetHelp("Pause emulation when the window is inactive ('off' by default).");

	pstring = section.AddString("keyboard_capture", Always, "off");
	pstring->SetValues({"auto", "on", "off"});
	pstring->SetHelp(
	        "Capture system keyboard shortcuts ('off' by default). When captured, most system\n"
	        "shortcuts such as Alt+Tab are sent to DOSBox Staging instead of the host OS.\n"
	        "This is useful for Windows 3.1 and some DOS programs with unchangeable keyboard\n"
	        "shortcuts that conflict with system shortcuts. Possible values:\n"
	        "\n"
	        "  auto:  Capture system shortcuts only while the mouse is captured. Releasing\n"
	        "         the mouse gives the shortcuts back to the host OS.\n"
	        "\n"
	        "  on:    Always capture system shortcuts.\n"
	        "\n"
	        "  off:   Never capture system shortcuts (default).");

	pstring = section.AddPath("mapperfile", Always, MAPPERFILE);
	pstring->SetHelp(
	        "Path to the mapper file ('mapper-sdl2-XYZ.map' by default, where XYZ is the\n"
	        "current version). Pre-configured maps are bundled in 'resources/mapperfiles'.\n"
	        "These can be loaded by name, e.g., with 'mapperfile = xbox/xenon2.map'.\n"
	        "\n"
	        "Note: The '--erasemapper' command line option only deletes the default mapper\n"
	        "      file.");

	pstring = section.AddString("screensaver", Always, "auto");
	pstring->SetHelp(
	        "Use 'allow' or 'block' to override the SDL_VIDEO_ALLOW_SCREENSAVER environment\n"
	        "variable which usually blocks the OS screensaver while the emulator is\n"
	        "running ('auto' by default).");
	pstring->SetValues({"auto", "allow", "block"});
}

static void init_xr_config_settings(SectionProp& section)
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
	        "Note: 'first eye' is left unless 'xr_eye_swap' is on. Can be\n"
	        "changed while running; takes effect on the next frame.\n"
	        "The Ctrl+E / Ctrl+T shortcuts cycle modes / toggle mono live.");
	pstring_xr->SetValues({"mono",
	                       "anaglyph",
	                       "alternating",
	                       "interleaved",
	                       "top-bottom",
	                       "sbs",
	                       "depth",
	                       "autodepth"});

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
#endif
	        "Note: Can be changed while running; takes effect on the next frame.\n"
	        "The Ctrl+A shortcut cycles the methods live.");
	pstring_xr_auto->SetValues({"blend",
	                             "gradient",
	                             "luma"
#if C_NEURAL
	                             ,
	                             "neural"
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
	        "higher CPU cost. Valid range from 100 to 5000.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
	pint_xr_neural->SetMinMax(100, 5000);
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
	        "Swap left/right eyes for the 'anaglyph' and 'alternating' stereo\n"
	        "input modes ('off' by default). Use this if depth looks inverted.\n"
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
	        "Eye source for the 'alternating' stereo input mode ('auto' by\n"
	        "default). Possible values:\n"
	        "\n"
	        "  auto:    Follow the game's LPT1 shutter bit (data bit 2) when\n"
	        "           it wrote one for the current frame, otherwise fall\n"
	        "           back to frame parity (default).\n"
	        "\n"
	        "  parity:  Always use blind frame parity, ignoring LPT writes.\n"
	        "\n"
	        "  lpt:     Always use the latest LPT1 data bit 2 value, even if\n"
	        "           stale.\n"
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

	auto pbool_xr_osd = section.AddBool("xr_osd", Always, true);
	pbool_xr_osd->SetHelp(
	        "Show an in-headset notification overlay for XR toggles and\n"
	        "setting changes ('on' by default). Messages fade after a few\n"
	        "seconds.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");

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

	auto pint_xr_osd_scale = section.AddInt("xr_osd_scale", Always, 100);
	pint_xr_osd_scale->SetHelp(
	        "Size of the in-headset notification overlay as a percentage of\n"
	        "its default size ('100' by default). Scaled about its centre\n"
	        "above the DOS picture. Valid range from 25 to 200.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_osd_scale->SetMinMax(25, 200);

	auto pint_xr_osd_x = section.AddInt("xr_osd_offset_x", Always, 0);
	pint_xr_osd_x->SetHelp(
	        "Horizontal position offset of the in-headset notification\n"
	        "overlay as a percentage of the screen half-width ('0' by\n"
	        "default, meaning centred above the DOS picture). Positive\n"
	        "moves right, negative moves left. Valid range from -100\n"
	        "to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_osd_x->SetMinMax(-100, 100);

	auto pint_xr_osd_y = section.AddInt("xr_osd_offset_y", Always, 0);
	pint_xr_osd_y->SetHelp(
	        "Vertical position offset of the in-headset notification\n"
	        "overlay as a percentage of the screen half-height ('0' by\n"
	        "default, meaning above the DOS picture). Positive moves up,\n"
	        "negative moves down. Valid range from -100 to 100.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");
	pint_xr_osd_y->SetMinMax(-100, 100);

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

	auto pbool_xr_ray = section.AddBool("xr_ray", Always, true);
	pbool_xr_ray->SetHelp(
	        "Draw a pointer ray from the right controller to the screen\n"
	        "('on' by default). Visible while controller mouse control or\n"
	        "the overlay keyboard is active.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect on the next frame.");

	auto pbool_xr_log_stats = section.AddBool("xr_log_framestats",
	                                          Always,
	                                          false);
	pbool_xr_log_stats->SetHelp(
	        "Log OpenXR per-frame timing statistics and head-pose updates\n"
	        "('off' by default). Useful when diagnosing headset performance.\n"
	        "\n"
	        "Note: Can be changed while running; takes effect immediately.");
}

void GFX_AddConfigSection()
{
	auto section = control->AddSection("sdl");
	section->AddUpdateHandler(notify_sdl_setting_updated);

	init_sdl_config_settings(*section);

	auto xr_section = control->AddSection("xr");
	init_xr_config_settings(*xr_section);

	TITLEBAR_AddMessages();
	register_sdl_text_messages();
}

void GFX_Quit()
{
#if !C_DEBUGGER
#if C_OPENXR
	// Tear down XR session/swapchains before the GL context goes away.
	cleanup_openxr();
#endif
	// Renderer must be destoryed before SDL_Quit() is called.
	// Otherwise we can get segfaults and sadness.
	sdl.renderer = {};
	sdl.window   = nullptr;

	SDL_Quit();
#endif
}

