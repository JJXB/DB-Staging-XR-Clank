// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Motion-controller and headset input (see xrinput.h): OpenXR action
// sets, bindings, per-frame polling, controller/trigger mouse,
// head-gyro mouse, thumbstick joystick + menu navigation, chord mapper
// handlers, and shared ray math.

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
XrActionSet sdl_xr_action_set   = XR_NULL_HANDLE;
XrAction    sdl_xr_aim_action   = XR_NULL_HANDLE; // pose input
XrAction    sdl_xr_click_action = XR_NULL_HANDLE; // boolean input
XrAction    sdl_xr_grip_action  = XR_NULL_HANDLE; // float input
XrAction    sdl_xr_trigvalue_action = XR_NULL_HANDLE; // float trigger
XrAction    sdl_xr_stick_action = XR_NULL_HANDLE; // vec2 left thumbstick
XrAction    sdl_xr_rstick_action = XR_NULL_HANDLE; // vec2 right thumbstick
XrAction    sdl_xr_joya_action  = XR_NULL_HANDLE; // boolean right A
XrAction    sdl_xr_joyb_action  = XR_NULL_HANDLE; // boolean right B
XrAction    sdl_xr_joyx_action  = XR_NULL_HANDLE; // boolean left X
XrAction    sdl_xr_joyy_action  = XR_NULL_HANDLE; // boolean left Y
XrAction    sdl_xr_stickclick_action = XR_NULL_HANDLE; // boolean right stick click
XrAction    sdl_xr_menu_action  = XR_NULL_HANDLE; // boolean menu btn
XrSpace     sdl_xr_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
XrPath      sdl_xr_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
bool        sdl_xr_actions_attached = false;

// XrHandState lives in gui/xrinput.h now (shared with HUD/submit).
XrHandState sdl_xr_hand[2] = {};
// Right-hand role, selected by grip chords (grip alone = off,
// grip+A = keyboard, grip+B = second joystick, grip+trigger = mouse).
// 0=keyboard, 1=mouse, 2=off, 3=stick2; -1 means "set outside chords,
// re-apply the next chord absolutely". (The former grip+stick-click
// CyberPuck role is retired: VFX-1 motion is numpad-only.)
int sdl_xr_right_chord = 2;
static bool sdl_xr_stick2     = false; // second DOS joystick selected
// VFX-1 numpad range-sweep holds (mapper hotkeys, momentary): KP7/8/9
// hold sweeps the HMD yaw/pitch/roll across the wire range, KP2/KP3
// hold sweeps the puck X/Y axes, KP4/5/6 hold the puck Top/Middle/
// Bottom buttons. Resolved into hold flags below; the VFX-1 synthesis
// sweeps an axis while its flag is set and centers it otherwise. No
// real-hardware motion feeds VFX-1.
static bool sdl_xr_test_hold_hmd[3]     = {}; // KP7/8/9: yaw/pitch/roll
static bool sdl_xr_test_hold_btn[3]     = {}; // KP4/5/6: top/middle/bottom
static bool sdl_xr_test_hold_puckaxis[2] = {}; // KP2/KP3: puck X/Y sweep
float        sdl_xr_test_hmd[3]  = {};
float        sdl_xr_test_puck[2] = {};
unsigned int sdl_xr_test_puckbtn = 0;

static void xr_test_resolve()
{
	for (int i = 0; i < 3; ++i) {
		sdl_xr_test_hmd[i] =
		        sdl_xr_test_hold_hmd[i] ? 1.0f : 0.0f;
	}
	sdl_xr_test_puck[0] = sdl_xr_test_hold_puckaxis[0] ? 1.0f : 0.0f;
	sdl_xr_test_puck[1] = sdl_xr_test_hold_puckaxis[1] ? 1.0f : 0.0f;
	sdl_xr_test_puckbtn = (sdl_xr_test_hold_btn[0] ? 1u : 0u) |
	                      (sdl_xr_test_hold_btn[1] ? 2u : 0u) |
	                      (sdl_xr_test_hold_btn[2] ? 4u : 0u);
	// DIAG: prove mapper hotkeys register (edge-logged).
	static float last_h[3] = {};
	static float last_p[2] = {};
	static unsigned int last_b = 0;
	static bool first = true;
	bool changed = first;
	for (int i = 0; i < 3 && !changed; ++i) {
		changed = (sdl_xr_test_hmd[i] != last_h[i]);
	}
	for (int i = 0; i < 2 && !changed; ++i) {
		changed = (sdl_xr_test_puck[i] != last_p[i]);
	}
	changed = changed || (sdl_xr_test_puckbtn != last_b);
	if (changed) {
		LOG_MSG("OpenXR: testkeys hmd=%.0f,%.0f,%.0f puck=%.0f,%.0f btn=%u",
		        sdl_xr_test_hmd[0],
		        sdl_xr_test_hmd[1],
		        sdl_xr_test_hmd[2],
		        sdl_xr_test_puck[0],
		        sdl_xr_test_puck[1],
		        sdl_xr_test_puckbtn);
		for (int i = 0; i < 3; ++i) {
			last_h[i] = sdl_xr_test_hmd[i];
		}
		for (int i = 0; i < 2; ++i) {
			last_p[i] = sdl_xr_test_puck[i];
		}
		last_b = sdl_xr_test_puckbtn;
		first  = false;
	}
}

void xr_test_hmd_yaw_sweep_handler(bool pressed)
{
	sdl_xr_test_hold_hmd[0] = pressed;
	xr_test_resolve();
}
void xr_test_hmd_pitch_sweep_handler(bool pressed)
{
	sdl_xr_test_hold_hmd[1] = pressed;
	xr_test_resolve();
}
void xr_test_hmd_roll_sweep_handler(bool pressed)
{
	sdl_xr_test_hold_hmd[2] = pressed;
	xr_test_resolve();
}
void xr_test_puck_x_sweep_handler(bool pressed)
{
	sdl_xr_test_hold_puckaxis[0] = pressed;
	xr_test_resolve();
}
void xr_test_puck_y_sweep_handler(bool pressed)
{
	sdl_xr_test_hold_puckaxis[1] = pressed;
	xr_test_resolve();
}
void xr_test_puck_top_handler(bool pressed)
{
	sdl_xr_test_hold_btn[0] = pressed;
	xr_test_resolve();
}
void xr_test_puck_middle_handler(bool pressed)
{
	sdl_xr_test_hold_btn[1] = pressed;
	xr_test_resolve();
}
void xr_test_puck_bottom_handler(bool pressed)
{
	sdl_xr_test_hold_btn[2] = pressed;
	xr_test_resolve();
}

// Emulated DOS joystick (stick 1) via the left controller, selected
// from the left mode wheel: 0=analog, 1=hat (8-way digital), 2=WASD
// keys, 3=arrow keys, 4=off.
int  sdl_xr_joy_mode       = 4;
float sdl_xr_joy_x         = 0.0f;
float sdl_xr_joy_y         = 0.0f;
float sdl_xr_rjoy_x        = 0.0f; // right thumbstick (menu nav)
float sdl_xr_rjoy_y        = 0.0f;
bool  sdl_xr_joy_btn[2]    = {false, false}; // right A, B
bool  sdl_xr_joy_btn_pressed[2] = {false, false}; // right A, B edge
bool  sdl_xr_joy_xy[2]     = {false, false}; // left X, Y held
bool  sdl_xr_joy_xy_pressed[2] = {false, false}; // left X, Y edge

// Mirror-window demo: show the decoded stereo pair side-by-side
// instead of the normal DOS picture (verification/demo tool).
bool sdl_xr_mirror_stereo = false;

// Emulated-mouse control (toggle/cycle): 0 = off (host mouse),
// 1 = right controller aims, 2 = headset aims (relative gyro).
// Triggers click in both modes (right = left button, left = right
// button).
int   sdl_xr_mouse_mode           = 0;
// Head-mouse reference orientation + virtual cursor (relative mode).
static XrQuaternionf sdl_xr_head_ref       = {0.0f, 0.0f, 0.0f, 1.0f};
static bool          sdl_xr_head_ref_valid = false;
static float         sdl_xr_head_cur_x     = 0.0f;
static float         sdl_xr_head_cur_y     = 0.0f;
static bool  sdl_xr_mouse_has_last       = false;
static float sdl_xr_mouse_last_x         = 0.0f;
static float sdl_xr_mouse_last_y         = 0.0f;
static bool  sdl_xr_mouse_button_held[2] = {false, false}; // L, R
// Calibration offset for the controller-aim mouse, in plane UV. RGrip
// captures the current aim so it maps to the screen centre.
static float sdl_xr_mouse_off_u           = 0.0f;
static float sdl_xr_mouse_off_v           = 0.0f;

// XrVec3 lives in gui/xrinput.h now (shared with HUD ray ribbons).

XrVec3 xr_quat_rotate(const XrQuaternionf& q, const XrVec3& v)
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

XrQuaternionf xr_quat_conj(const XrQuaternionf& q)
{
	return {-q.x, -q.y, -q.z, q.w};
}

XrQuaternionf xr_quat_mul(const XrQuaternionf& a,
                                 const XrQuaternionf& b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

float xr_dot(const XrVec3& a, const XrVec3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

XrVec3 xr_sub(const XrVec3& a, const XrVec3& b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

XrVec3 xr_cross(const XrVec3& a, const XrVec3& b)
{
	return {a.y * b.z - a.z * b.y,
	        a.z * b.x - a.x * b.z,
	        a.x * b.y - a.y * b.x};
}

float xr_len(const XrVec3& v)
{
	return sqrtf(xr_dot(v, v));
}

XrVec3 xr_norm(const XrVec3& v)
{
	const float len = xr_len(v);
	if (len < 1e-9f) {
		return {0.0f, 0.0f, 0.0f};
	}
	return {v.x / len, v.y / len, v.z / len};
}

// Column-major 3x3 rotation to quaternion (Shoemake).
XrQuaternionf xr_mat3_to_quat(const float m[9])
{
	const float trace = m[0] + m[4] + m[8];
	XrQuaternionf q = {0.0f, 0.0f, 0.0f, 1.0f};
	// Storage is S(r,c) = m[c*3+r]; antisymmetric differences follow
	// the standard formulation, e.g. qx = (S21-S12)/s = (m[5]-m[7])/s.
	if (trace > 0.0f) {
		const float s = sqrtf(trace + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (m[5] - m[7]) / s;
		q.y = (m[6] - m[2]) / s;
		q.z = (m[1] - m[3]) / s;
	} else if (m[0] > m[4] && m[0] > m[8]) {
		const float s = sqrtf(1.0f + m[0] - m[4] - m[8]) * 2.0f;
		q.w = (m[5] - m[7]) / s;
		q.x = 0.25f * s;
		q.y = (m[1] + m[3]) / s;
		q.z = (m[2] + m[6]) / s;
	} else if (m[4] > m[8]) {
		const float s = sqrtf(1.0f + m[4] - m[0] - m[8]) * 2.0f;
		q.w = (m[6] - m[2]) / s;
		q.x = (m[1] + m[3]) / s;
		q.y = 0.25f * s;
		q.z = (m[5] + m[7]) / s;
	} else {
		const float s = sqrtf(1.0f + m[8] - m[0] - m[4]) * 2.0f;
		q.w = (m[1] - m[3]) / s;
		q.x = (m[2] + m[6]) / s;
		q.y = (m[5] + m[7]) / s;
		q.z = 0.25f * s;
	}
	return q;
}

// Suggest one (action, path) binding as its own call and keep it on
// success. Rationale: a bad path in a multi-binding call kills the
// whole call (e.g. WMR has no boolean trigger), while some runtimes
// (observed: Oculus 1.208) only honor the LAST suggest call per
// profile. So probe every binding individually, then apply each
// profile's survivors in ONE final call: correct under both
// behaviors, and a rejected path can never take down the rest.

static bool openxr_probe_binding(const char* profile_name,
                                 const XrPath profile, XrAction action,
                                 const char* action_name, const int hand,
                                 const char* path,
                                 std::vector<XrActionSuggestedBinding>& kept)
{
	if (action == XR_NULL_HANDLE || !path) {
		return false;
	}
	XrPath bound = XR_NULL_PATH;
	if (xrStringToPath(sdl_xr_instance, path, &bound) != XR_SUCCESS) {
		return false;
	}
	XrActionSuggestedBinding single = {action, bound};
	XrInteractionProfileSuggestedBinding suggested = {};
	suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	suggested.interactionProfile     = profile;
	suggested.countSuggestedBindings = 1;
	suggested.suggestedBindings      = &single;
	if (xrSuggestInteractionProfileBindings(sdl_xr_instance,
	                                        &suggested) != XR_SUCCESS) {
		LOG_MSG("OpenXR: %s: no binding for %s (%s hand)",
		        profile_name,
		        action_name,
		        hand == 0 ? "left" : "right");
		return false;
	}
	kept.push_back(single);
	return true;
}

// Apply a profile's probed bindings in a single call (see above).
static void openxr_apply_bindings(const char* profile_name,
                                  const XrPath profile,
                                  const std::vector<XrActionSuggestedBinding>& kept)
{
	if (kept.empty()) {
		return;
	}
	XrInteractionProfileSuggestedBinding suggested = {};
	suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	suggested.interactionProfile      = profile;
	suggested.countSuggestedBindings  = static_cast<uint32_t>(
	        kept.size());
	suggested.suggestedBindings       = kept.data();
	if (xrSuggestInteractionProfileBindings(sdl_xr_instance,
	                                        &suggested) != XR_SUCCESS) {
		LOG_MSG("OpenXR: %s: failed to apply %d bindings",
		        profile_name,
		        (int)kept.size());
	} else {
		LOG_MSG("OpenXR: %s: applied %d bindings",
		        profile_name,
		        (int)kept.size());
	}
}

bool openxr_create_input_actions()
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

	// Analog trigger pull (both hands): lets xr_trigger_threshold tune
	// how far the trigger travels before it counts as pressed.
	// Non-fatal; falls back to the boolean click action.
	XrActionCreateInfo trigvalue_info = {};
	trigvalue_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	trigvalue_info.actionType          = XR_ACTION_TYPE_FLOAT_INPUT;
	trigvalue_info.countSubactionPaths = 2;
	trigvalue_info.subactionPaths      = sdl_xr_hand_path;
	snprintf(trigvalue_info.actionName, sizeof(trigvalue_info.actionName), "trigval");
	snprintf(trigvalue_info.localizedActionName,
	         sizeof(trigvalue_info.localizedActionName),
	         "Trigger pull");
	if (xrCreateAction(sdl_xr_action_set, &trigvalue_info, &sdl_xr_trigvalue_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create trigger-value action");
		sdl_xr_trigvalue_action = XR_NULL_HANDLE;
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

	// Right thumbstick (menu navigation alongside the left one).
	// Non-fatal like the left stick.
	XrActionCreateInfo rstick_info = {};
	rstick_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	rstick_info.actionType          = XR_ACTION_TYPE_VECTOR2F_INPUT;
	rstick_info.countSubactionPaths = 1;
	rstick_info.subactionPaths      = &sdl_xr_hand_path[1];
	snprintf(rstick_info.actionName, sizeof(rstick_info.actionName), "rstick");
	snprintf(rstick_info.localizedActionName,
	         sizeof(rstick_info.localizedActionName),
	         "Right thumbstick");
	if (xrCreateAction(sdl_xr_action_set, &rstick_info, &sdl_xr_rstick_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create right-stick action");
		sdl_xr_rstick_action = XR_NULL_HANDLE;
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
	// Left X/Y buttons (joystick mode chords with left grip). Non-fatal.
	XrActionCreateInfo joyx_info = {};
	joyx_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	joyx_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
	joyx_info.countSubactionPaths = 1;
	joyx_info.subactionPaths      = &sdl_xr_hand_path[0];
	snprintf(joyx_info.actionName, sizeof(joyx_info.actionName), "joyx");
	snprintf(joyx_info.localizedActionName,
	         sizeof(joyx_info.localizedActionName),
	         "Left X button");
	if (xrCreateAction(sdl_xr_action_set, &joyx_info, &sdl_xr_joyx_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create joy-X action");
		sdl_xr_joyx_action = XR_NULL_HANDLE;
	}
	XrActionCreateInfo joyy_info = {};
	joyy_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	joyy_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
	joyy_info.countSubactionPaths = 1;
	joyy_info.subactionPaths      = &sdl_xr_hand_path[0];
	snprintf(joyy_info.actionName, sizeof(joyy_info.actionName), "joyy");
	snprintf(joyy_info.localizedActionName,
	         sizeof(joyy_info.localizedActionName),
	         "Left Y button");
	if (xrCreateAction(sdl_xr_action_set, &joyy_info, &sdl_xr_joyy_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create joy-Y action");
		sdl_xr_joyy_action = XR_NULL_HANDLE;
	}
	// Thumbstick click (either hand): opens that hand's input-mode
	// wheel. Non-fatal; without it the wheel cannot open.
	XrActionCreateInfo stickclick_info = {};
	stickclick_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	stickclick_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
	stickclick_info.countSubactionPaths = 2;
	stickclick_info.subactionPaths      = sdl_xr_hand_path;
	snprintf(stickclick_info.actionName,
	         sizeof(stickclick_info.actionName),
	         "stickclick");
	snprintf(stickclick_info.localizedActionName,
	         sizeof(stickclick_info.localizedActionName),
	         "Thumbstick click");
	if (xrCreateAction(sdl_xr_action_set,
	                   &stickclick_info,
	                   &sdl_xr_stickclick_action) != XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create stick-click action");
		sdl_xr_stickclick_action = XR_NULL_HANDLE;
	}
	// Menu button (in-headset VR menu). Non-fatal; without it the menu
	// stays closed.
	XrActionCreateInfo menu_info = {};
	menu_info.type                = XR_TYPE_ACTION_CREATE_INFO;
	menu_info.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
	menu_info.countSubactionPaths = 2;
	menu_info.subactionPaths      = sdl_xr_hand_path;
	snprintf(menu_info.actionName, sizeof(menu_info.actionName), "menu");
	snprintf(menu_info.localizedActionName,
	         sizeof(menu_info.localizedActionName),
	         "Menu button");
	if (xrCreateAction(sdl_xr_action_set, &menu_info, &sdl_xr_menu_action) !=
	    XR_SUCCESS) {
		LOG_ERR("OpenXR: Failed to create menu action");
		sdl_xr_menu_action = XR_NULL_HANDLE;
	}

	// Suggested bindings per interaction profile (best effort: a
	// runtime without the profile simply ignores it).
	struct ProfileBindings {
		const char* profile;
		const char* click_path[2];
		const char* squeeze_path[2]; // nullptr where the profile has none
		const char* trigvalue_path[2]; // analog pull, nullptr if none
		const char* stick_path;   // left vec2, nullptr where none
		const char* rstick_path;  // right vec2, nullptr where none
		const char* buttona_path; // right A, nullptr where none
		const char* buttonb_path; // right B, nullptr where none
		const char* buttonx_path; // left X, nullptr where none
		const char* buttony_path; // left Y, nullptr where none
		const char* stickclick_path[2]; // stick click per hand, nullptr if none
		const char* menu_path[2]; // menu button, nullptr where none
	};
	static const ProfileBindings profiles[] = {
	        {"/interaction_profiles/khr/simple_controller",
	         {"/user/hand/left/input/select/click",
	          "/user/hand/right/input/select/click"},
	         {nullptr, nullptr}, // no grip input on simple controllers
	         {nullptr, nullptr},
	         nullptr,
	         nullptr,
	         nullptr,
	         nullptr,
	         nullptr,
	         nullptr,
	         {nullptr, nullptr}, // no stick click on simple controllers
	         {nullptr, nullptr}}, // no menu input on simple controllers
	        {"/interaction_profiles/oculus/touch_controller",
	         {"/user/hand/left/input/trigger/touch",
	          "/user/hand/right/input/trigger/touch"},
	         {"/user/hand/left/input/squeeze/value",
	          "/user/hand/right/input/squeeze/value"},
	         {"/user/hand/left/input/trigger/value",
	          "/user/hand/right/input/trigger/value"},
	         "/user/hand/left/input/thumbstick",
	         "/user/hand/right/input/thumbstick",
	         "/user/hand/right/input/a/click",
	         "/user/hand/right/input/b/click",
	         "/user/hand/left/input/x/click",
	         "/user/hand/left/input/y/click",
	         {"/user/hand/left/input/thumbstick/click",
	          "/user/hand/right/input/thumbstick/click"},
	         {"/user/hand/left/input/menu/click",
	          nullptr}}, // right button is system-reserved
	        {"/interaction_profiles/valve/index_controller",
	         {"/user/hand/left/input/trigger/click",
	          "/user/hand/right/input/trigger/click"},
	         {"/user/hand/left/input/squeeze/value",
	          "/user/hand/right/input/squeeze/value"},
	         {"/user/hand/left/input/trigger/value",
	          "/user/hand/right/input/trigger/value"},
	         "/user/hand/left/input/thumbstick",
	         "/user/hand/right/input/thumbstick",
	         "/user/hand/right/input/a/click",
	         "/user/hand/right/input/b/click",
	         "/user/hand/left/input/x/click",
	         "/user/hand/left/input/y/click",
	         {"/user/hand/left/input/thumbstick/click",
	          "/user/hand/right/input/thumbstick/click"},
	         {nullptr, nullptr}}, // system button reserved for the overlay
	        {"/interaction_profiles/microsoft/motion_controller",
	         {nullptr, nullptr}, // no boolean trigger on WMR (trigger is
	                             // value-only); clicks come from the analog
	                             // xr_trigger_threshold override instead
	         {"/user/hand/left/input/squeeze/value",
	          "/user/hand/right/input/squeeze/value"},
	         {"/user/hand/left/input/trigger/value",
	          "/user/hand/right/input/trigger/value"},
	         "/user/hand/left/input/thumbstick",
	         "/user/hand/right/input/thumbstick",
	         nullptr, // no A button; trackpad click is button 2
	         "/user/hand/right/input/trackpad/click",
	         nullptr, // no X/Y buttons on motion controllers
	         nullptr,
	         {"/user/hand/left/input/thumbstick/click",
	          "/user/hand/right/input/thumbstick/click"},
	         {"/user/hand/left/input/menu/click",
	          "/user/hand/right/input/menu/click"}},
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
		std::vector<XrActionSuggestedBinding> kept;
		for (int hand = 0; hand < 2; ++hand) {
			openxr_probe_binding(p.profile,
			                     profile,
			                     sdl_xr_aim_action,
			                     "aim",
			                     hand,
			                     aim_path[hand],
			                     kept);
			openxr_probe_binding(p.profile,
			                     profile,
			                     sdl_xr_click_action,
			                     "click",
			                     hand,
			                     p.click_path[hand],
			                     kept);
			openxr_probe_binding(p.profile,
			                     profile,
			                     sdl_xr_grip_action,
			                     "grip",
			                     hand,
			                     p.squeeze_path[hand],
			                     kept);
			openxr_probe_binding(p.profile,
			                     profile,
			                     sdl_xr_trigvalue_action,
			                     "trigger-value",
			                     hand,
			                     p.trigvalue_path[hand],
			                     kept);
			openxr_probe_binding(p.profile,
			                     profile,
			                     sdl_xr_menu_action,
			                     "menu",
			                     hand,
			                     p.menu_path[hand],
			                     kept);
		}
		openxr_probe_binding(p.profile,
		                     profile,
		                     sdl_xr_stick_action,
		                     "stick",
		                     0,
		                     p.stick_path,
		                     kept);
		openxr_probe_binding(p.profile,
		                     profile,
		                     sdl_xr_rstick_action,
		                     "right-stick",
		                     1,
		                     p.rstick_path,
		                     kept);
		openxr_probe_binding(p.profile,
		                     profile,
		                     sdl_xr_joya_action,
		                     "joy-A",
		                     1,
		                     p.buttona_path,
		                     kept);
		openxr_probe_binding(p.profile,
		                     profile,
		                     sdl_xr_joyb_action,
		                     "joy-B",
		                     1,
		                     p.buttonb_path,
		                     kept);
		openxr_probe_binding(p.profile,
		                     profile,
		                     sdl_xr_joyx_action,
		                     "joy-X",
		                     0,
		                     p.buttonx_path,
		                     kept);
		openxr_probe_binding(p.profile,
		                     profile,
		                     sdl_xr_joyy_action,
		                     "joy-Y",
		                     0,
		                     p.buttony_path,
		                     kept);
		for (int hand = 0; hand < 2; ++hand) {
			openxr_probe_binding(p.profile,
			                     profile,
			                     sdl_xr_stickclick_action,
			                     "stick-click",
			                     hand,
			                     p.stickclick_path[hand],
			                     kept);
		}
		openxr_apply_bindings(p.profile, profile, kept);
	}
	return true;
}

bool openxr_attach_input()
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

void openxr_update_input(const XrTime display_time)
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
			const XrResult loc_result = xrLocateSpace(
			        sdl_xr_aim_space[hand],
			        sdl_xr_app_space,
			        display_time,
			        &loc);
			dst.aim_result = loc_result;
			dst.aim_flags  = (loc_result == XR_SUCCESS)
			                         ? loc.locationFlags
			                         : 0;
			if (loc_result == XR_SUCCESS) {
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
		} else {
			dst.aim_result  = XR_ERROR_HANDLE_INVALID;
			dst.aim_flags   = 0;
			dst.pose_valid  = false;
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
		// Analog trigger pull overrides the boolean click when bound:
		// xr_trigger_threshold (percent of pull) decides the press.
		// Menu and keyboard selection use the firmer
		// xr_ui_trigger_threshold so resting a finger on the trigger
		// does not spray UI clicks; the DOS mouse keeps the main
		// threshold. Both use a small release hysteresis so hover
		// near the threshold cannot flutter the button.
		static constexpr int TrigHysteresis = 3;
		bool ui_overridden = false;
		if (sdl_xr_trigvalue_action != XR_NULL_HANDLE) {
			XrActionStateGetInfo trig_get = {};
			trig_get.type          = XR_TYPE_ACTION_STATE_GET_INFO;
			trig_get.action        = sdl_xr_trigvalue_action;
			trig_get.subactionPath = sdl_xr_hand_path[hand];
			XrActionStateFloat trig_state = {};
			trig_state.type = XR_TYPE_ACTION_STATE_FLOAT;
			if (xrGetActionStateFloat(sdl_xr_session,
			                          &trig_get,
			                          &trig_state) == XR_SUCCESS &&
			    trig_state.isActive) {
				int threshold = get_xr_section()->GetInt(
				        "xr_trigger_threshold");
				if (threshold > 0) {
					if (threshold > 100) {
						threshold = 100;
					}
					const int release_at =
					        (threshold > TrigHysteresis)
					                ? threshold - TrigHysteresis
					                : threshold;
					const float v =
					        trig_state.currentState * 100.0f;
					const bool pulled =
					        dst.click ? (v >= release_at)
					                  : (v >= threshold);
					dst.click_changed = (pulled != dst.click);
					dst.click         = pulled;
				}
				int ui_threshold = get_xr_section()->GetInt(
				        "xr_ui_trigger_threshold");
				if (ui_threshold < 10) {
					ui_threshold = 10;
				}
				if (ui_threshold > 100) {
					ui_threshold = 100;
				}
				const int ui_release_at =
				        (ui_threshold > TrigHysteresis)
				                ? ui_threshold - TrigHysteresis
				                : ui_threshold;
				const float uv = trig_state.currentState * 100.0f;
				const bool ui_pulled =
				        dst.click_ui ? (uv >= ui_release_at)
				                     : (uv >= ui_threshold);
				dst.click_ui_changed = (ui_pulled != dst.click_ui);
				dst.click_ui         = ui_pulled;
				ui_overridden        = true;
			}
		}
		if (!ui_overridden) {
			// No analog trigger (e.g. simple controllers): the UI
			// press follows the boolean click.
			dst.click_ui_changed = (dst.click != dst.click_ui);
			dst.click_ui         = dst.click;
		}
		// Menu button per hand, with a rising edge for the toggle.
		if (sdl_xr_menu_action != XR_NULL_HANDLE) {
			XrActionStateGetInfo menu_get = {};
			menu_get.type          = XR_TYPE_ACTION_STATE_GET_INFO;
			menu_get.action        = sdl_xr_menu_action;
			menu_get.subactionPath = sdl_xr_hand_path[hand];
			XrActionStateBoolean menu_state = {};
			menu_state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
			if (xrGetActionStateBoolean(sdl_xr_session,
			                            &menu_get,
			                            &menu_state) == XR_SUCCESS &&
			    menu_state.isActive) {
				const bool was = dst.menu;
				dst.menu         = menu_state.currentState;
				dst.menu_changed = (menu_state.currentState &&
				                    !was);
			} else {
				dst.menu_changed = false;
			}
		}
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
		} else {
			sdl_xr_joy_x = 0.0f;
			sdl_xr_joy_y = 0.0f;
		}
	}
	// Right thumbstick (menu navigation and the second DOS joystick).
	if (sdl_xr_rstick_action != XR_NULL_HANDLE) {
		XrActionStateGetInfo rstick_get = {};
		rstick_get.type          = XR_TYPE_ACTION_STATE_GET_INFO;
		rstick_get.action        = sdl_xr_rstick_action;
		rstick_get.subactionPath = sdl_xr_hand_path[1];
		XrActionStateVector2f rstick_state = {};
		rstick_state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
		if (xrGetActionStateVector2f(sdl_xr_session,
		                            &rstick_get,
		                            &rstick_state) == XR_SUCCESS &&
		    rstick_state.isActive) {
			sdl_xr_rjoy_x = rstick_state.currentState.x;
			sdl_xr_rjoy_y = rstick_state.currentState.y;
		} else {
			sdl_xr_rjoy_x = 0.0f;
			sdl_xr_rjoy_y = 0.0f;
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
			const bool was = sdl_xr_joy_btn[b];
			sdl_xr_joy_btn[b]         = btn_state.currentState;
			sdl_xr_joy_btn_pressed[b] = (btn_state.currentState &&
			                             !was);
		} else {
			sdl_xr_joy_btn[b]         = false;
			sdl_xr_joy_btn_pressed[b] = false;
		}
	}
	// Left X/Y (joystick mode chords with left grip), with edges.
	for (int b = 0; b < 2; ++b) {
		const XrAction btn_action =
		        (b == 0) ? sdl_xr_joyx_action : sdl_xr_joyy_action;
		if (btn_action == XR_NULL_HANDLE) {
			sdl_xr_joy_xy_pressed[b] = false;
			continue;
		}
		XrActionStateGetInfo btn_get = {};
		btn_get.type          = XR_TYPE_ACTION_STATE_GET_INFO;
		btn_get.action        = btn_action;
		btn_get.subactionPath = sdl_xr_hand_path[0];
		XrActionStateBoolean btn_state = {};
		btn_state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
		if (xrGetActionStateBoolean(sdl_xr_session,
		                            &btn_get,
		                            &btn_state) == XR_SUCCESS &&
		    btn_state.isActive) {
			const bool was = sdl_xr_joy_xy[b];
			sdl_xr_joy_xy[b]         = btn_state.currentState;
			sdl_xr_joy_xy_pressed[b] = (btn_state.currentState &&
			                             !was);
		} else {
			sdl_xr_joy_xy[b]         = false;
			sdl_xr_joy_xy_pressed[b] = false;
		}
	}
	// Thumbstick click state (each hand), with a rising edge.
	for (int hand = 0; hand < 2; ++hand) {
		auto& dst = sdl_xr_hand[hand];
		if (sdl_xr_stickclick_action == XR_NULL_HANDLE) {
			dst.stick_click_changed = false;
			continue;
		}
		XrActionStateGetInfo sc_get = {};
		sc_get.type          = XR_TYPE_ACTION_STATE_GET_INFO;
		sc_get.action        = sdl_xr_stickclick_action;
		sc_get.subactionPath = sdl_xr_hand_path[hand];
		XrActionStateBoolean sc_state = {};
		sc_state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
		if (xrGetActionStateBoolean(sdl_xr_session,
		                            &sc_get,
		                            &sc_state) == XR_SUCCESS &&
		    sc_state.isActive) {
			const bool was          = dst.stick_click;
			dst.stick_click         = sc_state.currentState;
			dst.stick_click_changed = (sc_state.currentState &&
			                           !was);
		} else {
			dst.stick_click_changed = false;
		}
	}

	// Publish the controller buttons as bindable mapper sources. The
	// mapper owns edge detection, so this is just the current level.
	uint32_t xr_mapper_masks[2] = {};

	if (sdl_xr_hand[0].click) {
		xr_mapper_masks[0] |= XrMapperInputTrigger;
	}
	if (sdl_xr_hand[0].grip > 0.5f) {
		xr_mapper_masks[0] |= XrMapperInputGrip;
	}
	if (sdl_xr_hand[0].stick_click) {
		xr_mapper_masks[0] |= XrMapperInputStickClick;
	}
	if (sdl_xr_hand[0].menu) {
		xr_mapper_masks[0] |= XrMapperInputMenu;
	}
	if (sdl_xr_joy_xy[0]) {
		xr_mapper_masks[0] |= XrMapperInputX;
	}
	if (sdl_xr_joy_xy[1]) {
		xr_mapper_masks[0] |= XrMapperInputY;
	}

	if (sdl_xr_hand[1].click) {
		xr_mapper_masks[1] |= XrMapperInputTrigger;
	}
	if (sdl_xr_hand[1].grip > 0.5f) {
		xr_mapper_masks[1] |= XrMapperInputGrip;
	}
	if (sdl_xr_hand[1].stick_click) {
		xr_mapper_masks[1] |= XrMapperInputStickClick;
	}
	if (sdl_xr_hand[1].menu) {
		xr_mapper_masks[1] |= XrMapperInputMenu;
	}
	if (sdl_xr_joy_btn[0]) {
		xr_mapper_masks[1] |= XrMapperInputA;
	}
	if (sdl_xr_joy_btn[1]) {
		xr_mapper_masks[1] |= XrMapperInputB;
	}

	MAPPER_UpdateXrInput(xr_mapper_masks[0], xr_mapper_masks[1]);
}

// Ray/plane intersection core shared by hand and head tracking: origin
// o along dir hits the screen plane (centre, basis, half extents).
// Returns origin/direction, hit point, and plane UV, or false when the
// ray is parallel or the plane is behind the origin.
bool openxr_ray_plane_hit(const XrVec3& o, const XrVec3& dir,
                                 const XrVec3& center, const XrVec3& fwd,
                                 const XrVec3& right, const XrVec3& up,
                                 const float half_w, const float half_h,
                                 XrVec3& from_out, XrVec3& dir_out,
                                 XrVec3& hit_out, float& u_out,
                                 float& v_out)
{
	const XrVec3 n = {-fwd.x, -fwd.y, -fwd.z};

	const float denom = xr_dot(dir, n);
	if (denom > -1e-6f && denom < 1e-6f) {
		return false; // ray parallel to the plane
	}
	const XrVec3 diff = {center.x - o.x, center.y - o.y, center.z - o.z};
	const float t     = xr_dot(diff, n) / denom;
	if (t <= 0.0f) {
		return false; // plane is behind the origin
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

// Intersect a hand's aim ray with the virtual screen plane. Returns
// ray origin/direction, hit point, and plane UV (u right, v up, 0..1),
// or false when unusable.
bool openxr_hand_ray(const int hand, XrVec3& from_out, XrVec3& dir_out,
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
	return openxr_ray_plane_hit(o,
	                            dir,
	                            center,
	                            fwd,
	                            right,
	                            up,
	                            half_w,
	                            half_h,
	                            from_out,
	                            dir_out,
	                            hit_out,
	                            u_out,
	                            v_out);
}

bool openxr_hand_plane_uv(const int hand, float& u_out, float& v_out)
{
	XrVec3 from = {}, dir = {}, hit = {};
	return openxr_hand_ray(hand, from, dir, hit, u_out, v_out);
}

// Controller-aim mouse calibration: treat the current aim point as the
// screen centre, absorbing any grip-vs-aim misalignment into the UV
// offset, and put the cursor back at the centre.
static void openxr_recenter_controller_mouse()
{
	float u = 0.0f, v = 0.0f;
	if (!openxr_hand_plane_uv(1, u, v)) {
		return;
	}
	sdl_xr_mouse_off_u = 0.5f - u;
	sdl_xr_mouse_off_v = 0.5f - v;

	const auto& r = sdl.draw.draw_rect_px;
	sdl_xr_mouse_last_x = r.x + r.w * 0.5f;
	sdl_xr_mouse_last_y = r.y + r.h * 0.5f;
	sdl_xr_mouse_has_last = true;
	MOUSE_EventMoved(0.0f,
	                 0.0f,
	                 sdl_xr_mouse_last_x,
	                 sdl_xr_mouse_last_y);
	OPENXR_Notify("Mouse recentered");
}

// Grip grab/move/resize: the left grip moves the element under its
// aim ray, the right grip resizes the element under its aim ray.
// Element ids match the layout panels: 0 screen, 1 keyboard, 2 menu.
static void openxr_panel_geometry(const int element, XrPosef& pose_out,
                                  float& width_out, float& height_out,
                                  float& ref_w_out, float& ref_h_out)
{
	XrPosef anchor = {};
	float half_w = 0.0f, half_h = 0.0f, dist_m = 0.0f;
	if (!openxr_screen_plane(anchor, half_w, half_h, dist_m)) {
		return;
	}
	const XrVec3 fwd   = xr_quat_rotate(anchor.orientation, {0.0f, 0.0f, -1.0f});
	const XrVec3 up    = xr_quat_rotate(anchor.orientation, {0.0f, 1.0f, 0.0f});
	const XrVec3 right = xr_quat_rotate(anchor.orientation, {1.0f, 0.0f, 0.0f});

	if (element == 0) {
		XrPosef panel = {};
		float pw = 0.0f, ph = 0.0f;
		if (openxr_screen_panel(panel, pw, ph)) {
			pose_out = panel;
		} else {
			pose_out.orientation = anchor.orientation;
			pose_out.position    = {anchor.position.x + fwd.x * dist_m,
			                        anchor.position.y + fwd.y * dist_m,
			                        anchor.position.z + fwd.z * dist_m};
			pw = 2.0f * half_w;
			ph = 2.0f * half_h;
		}
		width_out  = pw;
		height_out = ph;
		const float s = std::max(0.25f, sdl_xr_layout_screen.scale);
		ref_w_out     = half_w / s;
		ref_h_out     = half_h / s;
		return;
	}
	if (element == 1) {
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
		pose_out.position    = {anchor.position.x +
		                                fwd.x * kd - up.x * (0.55f * half_h) +
		                                right.x * ox + up.x * oy,
		                        anchor.position.y +
		                                fwd.y * kd - up.y * (0.55f * half_h) +
		                                right.y * ox + up.y * oy,
		                        anchor.position.z +
		                                fwd.z * kd - up.z * (0.55f * half_h) +
		                                right.z * ox + up.z * oy};
		const float s = std::clamp(sdl_xr_layout_keyboard.scale,
		                           0.25f,
		                           2.0f) *
		                openxr_keyboard_key_scale();
		width_out  = 2.0f * half_w * s;
		height_out = 0.9f * half_h * s;
		ref_w_out  = half_w;
		ref_h_out  = half_h;
		return;
	}
	// Menu.
	float pw = 0.0f, ph = 0.0f, pu = 0.0f, pvc = 0.0f, pvh = 0.0f;
	openxr_menu_layout(half_h, pw, ph, pu, pvc, pvh);
	const float ox = std::clamp(sdl_xr_layout_menu.offset_x, -1.0f, 1.0f);
	const float md = openxr_layer_dist("xr_menu_distance") + 0.03f;
	pose_out.orientation = anchor.orientation;
	pose_out.position    = {anchor.position.x + fwd.x * md + up.x * pu +
	                                right.x * (ox * half_w),
	                        anchor.position.y + fwd.y * md + up.y * pu +
	                                right.y * (ox * half_w),
	                        anchor.position.z + fwd.z * md + up.z * pu +
	                                right.z * (ox * half_w)};
	width_out  = pw;
	height_out = ph;
	ref_w_out  = half_w;
	ref_h_out  = half_h;
}

// Element under a hand's aim ray, or -1 when the ray misses the
// screen plane. Menu and keyboard sit in front of the screen, so they
// win when their footprint contains the hit.
static int openxr_panel_target(const int hand)
{
	float u = 0.0f, v = 0.0f;
	if (!openxr_hand_plane_uv(hand, u, v)) {
		return -1;
	}
	XrPosef anchor = {};
	float half_w = 0.0f, half_h = 0.0f, dist_m = 0.0f;
	if (!openxr_screen_plane(anchor, half_w, half_h, dist_m)) {
		return -1;
	}
	if (half_w <= 0.0f || half_h <= 0.0f) {
		return -1;
	}
	if (sdl_xr_menu_visible) {
		float pw = 0.0f, ph = 0.0f, pu = 0.0f, pvc = 0.0f, pvh = 0.0f;
		openxr_menu_layout(half_h, pw, ph, pu, pvc, pvh);
		const float ox = std::clamp(sdl_xr_layout_menu.offset_x,
		                            -1.0f,
		                            1.0f);
		const float uc = 0.5f + ox * 0.5f;
		const float hu = pw / (4.0f * std::max(0.01f, half_w));
		if (std::fabs(u - uc) <= hu && std::fabs(v - pvc) <= pvh) {
			return 2;
		}
	}
	if (sdl_xr_keyboard_visible) {
		const float s = std::clamp(sdl_xr_layout_keyboard.scale,
		                           0.25f,
		                           2.0f) *
		                openxr_keyboard_key_scale();
		const float ox = std::clamp(sdl_xr_layout_keyboard.offset_x,
		                            -1.0f,
		                            1.0f);
		const float oy = std::clamp(sdl_xr_layout_keyboard.offset_y,
		                            -1.0f,
		                            1.0f);
		const float ku = (u - (0.5f + ox * 0.5f)) / s + 0.5f;
		const float kv = (v - (0.225f + oy * 0.5f)) / s + 0.225f;
		if (kv >= 0.0f && kv <= 0.45f && ku >= 0.0f && ku <= 1.0f) {
			return 1;
		}
	}
	return 0;
}

static XrPanelLayout* openxr_panel_layout(const int element)
{
	switch (element) {
	case 0: return &sdl_xr_layout_screen;
	case 1: return &sdl_xr_layout_keyboard;
	case 2: return &sdl_xr_layout_menu;
	default: return nullptr;
	}
}

struct XrPanelGrab {
	int     element  = -1;
	bool    resizing = false;
	XrVec3  hand_start = {};
	XrVec3  center     = {};
	XrVec3  right      = {};
	XrVec3  up         = {};
	float   ref_w      = 1.0f;
	float   ref_h      = 1.0f;
	float   start_ox   = 0.0f;
	float   start_oy   = 0.0f;
	float   start_scale = 1.0f;
	float   start_dist  = 1.0f;
};
static XrPanelGrab sdl_xr_panel_grab[2];

void openxr_drive_panels()
{
	openxr_layout_load();
	// With the controller-aim mouse active the right grip recenters
	// the mouse instead of resizing: a pointer has no use for resize.
	// Any in-progress right-hand resize is cancelled so it cannot
	// stick. Head-aim mode (2) still uses the grip to resize.
	const bool mouse_mode = (sdl_xr_mouse_mode == 1);
	if (mouse_mode) {
		const auto& right = sdl_xr_hand[1];
		if (right.grip_pressed && right.pose_valid) {
			openxr_recenter_controller_mouse();
		}
		if (sdl_xr_panel_grab[1].element >= 0) {
			openxr_layout_save(sdl_xr_panel_grab[1].element);
			sdl_xr_panel_grab[1].element = -1;
		}
	}
	if (!get_xr_section()->GetBool("xr_panel_grab")) {
		for (auto& grab : sdl_xr_panel_grab) {
			grab.element = -1;
		}
		return;
	}
	for (int hand = 0; hand < 2; ++hand) {
		if (mouse_mode && hand == 1) {
			continue;
		}
		auto& state = sdl_xr_hand[hand];
		auto& grab  = sdl_xr_panel_grab[hand];

		if (grab.element >= 0 && !state.pose_valid) {
			openxr_layout_save(grab.element);
			grab.element = -1;
			continue;
		}
		if (state.grip_pressed && grab.element < 0 && state.pose_valid) {
			const int target = openxr_panel_target(hand);
			XrPosef pose     = {};
			float w = 0.0f, h = 0.0f, ref_w = 0.0f, ref_h = 0.0f;
			openxr_panel_geometry(target >= 0 ? target : 0,
			                      pose,
			                      w,
			                      h,
			                      ref_w,
			                      ref_h);
			if (target >= 0 && w > 0.0f && h > 0.0f) {
				grab.element     = target;
				grab.resizing    = (hand == 1);
				grab.hand_start  = {state.pose.position.x,
				                    state.pose.position.y,
				                    state.pose.position.z};
				grab.center      = {pose.position.x,
				                    pose.position.y,
				                    pose.position.z};
				grab.right       = xr_quat_rotate(pose.orientation,
				                                  {1.0f, 0.0f, 0.0f});
				grab.up          = xr_quat_rotate(pose.orientation,
				                                  {0.0f, 1.0f, 0.0f});
				grab.ref_w       = std::max(0.05f, ref_w);
				grab.ref_h       = std::max(0.05f, ref_h);
				grab.start_dist  = std::max(0.15f,
				                            xr_len(xr_sub(grab.hand_start,
				                                          grab.center)));
				auto* layout = openxr_panel_layout(target);
				if (layout) {
					grab.start_ox    = layout->offset_x;
					grab.start_oy    = layout->offset_y;
					grab.start_scale = layout->scale;
				}
			}
		}
		if (grab.element < 0) {
			continue;
		}
		if (state.grip > 0.5f) {
			auto* layout = openxr_panel_layout(grab.element);
			if (!layout) {
				grab.element = -1;
				continue;
			}
			const XrVec3 hand_pos = {state.pose.position.x,
			                         state.pose.position.y,
			                         state.pose.position.z};
			if (grab.resizing) {
				const float d = xr_len(xr_sub(hand_pos, grab.center));
				const float f = d / grab.start_dist;
				const float lo = 0.25f;
				const float hi = (grab.element == 0) ? 3.0f : 2.0f;
				layout->scale = std::clamp(grab.start_scale * f, lo, hi);
			} else {
				const XrVec3 delta = xr_sub(hand_pos, grab.hand_start);
				layout->offset_x = std::clamp(grab.start_ox +
				                                      xr_dot(delta, grab.right) /
				                                              grab.ref_w,
				                              -1.0f,
				                              1.0f);
				layout->offset_y = std::clamp(grab.start_oy +
				                                      xr_dot(delta, grab.up) /
				                                              grab.ref_h,
				                              -1.0f,
				                              1.0f);
			}
		} else {
			openxr_layout_save(grab.element);
			grab.element = -1;
		}
	}
}

// Emulated mouse buttons follow the triggers directly: holding the
// trigger holds the button down, releasing raises it. Shared by both
// mouse modes.
void openxr_release_mouse_buttons()
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

// One emulated button, held for as long as its hand's trigger is held.
// Shared by both mouse modes.
static void openxr_mouse_button_drive(const int hand,
                                      const MouseButtonId button,
                                      const int slot,
                                      const bool menu_eats_click)
{
	// The VR menu owns the trigger (navigation / confirm / typing):
	// never press, and drop any button we were holding so it cannot
	// stick down while the menu is up.
	const bool want = sdl_xr_hand[hand].click && !menu_eats_click;
	if (want == sdl_xr_mouse_button_held[slot]) {
		return;
	}
	MOUSE_EventButton(button, want);
	sdl_xr_mouse_button_held[slot] = want;
}

// Mouse sensitivity multiplier from an xr_*_sense_* option (percent,
// 100 = raw). Scales relative motion; absolute position is unaffected
// so screen edges stay reachable.
static float openxr_mouse_sense(const char* opt_name)
{
	return std::clamp(get_xr_section()->GetInt(opt_name) / 100.0f,
	                  0.1f,
	                  4.0f);
}

void openxr_drive_mouse()
{
	if (sdl_xr_mouse_mode != 1) {
		return;
	}
	// Buttons follow trigger edges even when the ray misses the screen,
	// except edges the VR menu consumed or that type a preset name.
	const bool menu_eats_click =
	        sdl_xr_menu_claim_click ||
	        (sdl_xr_menu_visible && sdl_xr_menu_field_focus);
	openxr_mouse_button_drive(1, MouseButtonId::Left, 0, menu_eats_click);
	openxr_mouse_button_drive(0, MouseButtonId::Right, 1, menu_eats_click);

	float u = 0.0f, v = 0.0f;
	if (!openxr_hand_plane_uv(1, u, v)) {
		sdl_xr_mouse_has_last = false;
		return;
	}
	u += sdl_xr_mouse_off_u;
	v += sdl_xr_mouse_off_v;
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
		const float sx = openxr_mouse_sense(
		        "xr_controller_mouse_sense_x");
		const float sy = openxr_mouse_sense(
		        "xr_controller_mouse_sense_y");
		MOUSE_EventMoved((ax - sdl_xr_mouse_last_x) * sx,
		                 (ay - sdl_xr_mouse_last_y) * sy,
		                 ax,
		                 ay);
	}
	sdl_xr_mouse_last_x   = ax;
	sdl_xr_mouse_last_y   = ay;
	sdl_xr_mouse_has_last = true;
}

// Head-aimed mouse (relative gyro): headset yaw/pitch deltas drive
// a virtual cursor, so it works whether the picture is glued to the
// headset or pinned in the room. Controller triggers still click
// (same mapping as controller mode).
void openxr_drive_head_mouse()
{
	if (sdl_xr_mouse_mode != 2) {
		return;
	}
	const bool menu_eats_click =
	        sdl_xr_menu_claim_click ||
	        (sdl_xr_menu_visible && sdl_xr_menu_field_focus);
	openxr_mouse_button_drive(1, MouseButtonId::Left, 0, menu_eats_click);
	openxr_mouse_button_drive(0, MouseButtonId::Right, 1, menu_eats_click);

	if (!sdl_xr_head_pose_valid) {
		sdl_xr_head_ref_valid = false;
		return;
	}
	const auto& r = sdl.draw.draw_rect_px;
	const auto& hq = sdl_xr_head_pose.orientation;
	if (!sdl_xr_head_ref_valid) {
		// (Re)capture baseline: no jump, cursor to the centre.
		sdl_xr_head_ref       = hq;
		sdl_xr_head_ref_valid = true;
		sdl_xr_head_cur_x     = r.x + r.w * 0.5f;
		sdl_xr_head_cur_y     = r.y + r.h * 0.5f;
		return;
	}
	// Inter-frame rotation, yaw/pitch out (roll ignored).
	const XrQuaternionf qd = xr_quat_mul(xr_quat_conj(sdl_xr_head_ref),
	                                     hq);
	sdl_xr_head_ref = hq;
	float yaw = atan2f(2.0f * (qd.w * qd.y + qd.x * qd.z),
	                   1.0f - 2.0f * (qd.y * qd.y + qd.z * qd.z));
	float sp  = 2.0f * (qd.w * qd.x - qd.y * qd.z);
	if (sp > 1.0f) {
		sp = 1.0f;
	}
	if (sp < -1.0f) {
		sp = -1.0f;
	}
	float pitch = asinf(sp);
	// Tremor deadzone (~0.05 deg) so a held-still head sits still.
	if (std::fabs(yaw) < 0.0009f) {
		yaw = 0.0f;
	}
	if (std::fabs(pitch) < 0.0009f) {
		pitch = 0.0f;
	}
	// ~60 deg head turn spans the draw area, times sensitivity.
	const float sx =
	        openxr_mouse_sense("xr_head_mouse_sense_x");
	const float sy =
	        openxr_mouse_sense("xr_head_mouse_sense_y");
	float dx = 0.0f, dy = 0.0f;
	if (!(yaw == 0.0f && pitch == 0.0f)) {
		dx = -yaw * (r.w / 1.0472f) * sx;
		dy = -pitch * (r.h / 1.0472f) * sy;
	}
	// Right thumbstick is a secondary, head-independent mouse input
	// (fine aiming without moving the head). OpenXR +y is up, screen
	// +y is down, so invert it.
	const float rx = sdl_xr_rjoy_x;
	const float ry = sdl_xr_rjoy_y;
	if (std::fabs(rx) > 0.15f) {
		dx += rx * (r.w * 0.014f) * sx;
	}
	if (std::fabs(ry) > 0.15f) {
		dy += -ry * (r.h * 0.014f) * sy;
	}
	if (dx == 0.0f && dy == 0.0f) {
		return;
	}
	sdl_xr_head_cur_x += dx;
	sdl_xr_head_cur_y += dy;
	if (sdl_xr_head_cur_x < r.x) {
		sdl_xr_head_cur_x = r.x;
	}
	if (sdl_xr_head_cur_x > r.x + r.w) {
		sdl_xr_head_cur_x = r.x + r.w;
	}
	if (sdl_xr_head_cur_y < r.y) {
		sdl_xr_head_cur_y = r.y;
	}
	if (sdl_xr_head_cur_y > r.y + r.h) {
		sdl_xr_head_cur_y = r.y + r.h;
	}
	MOUSE_EventMoved(dx,
	                 dy,
	                 sdl_xr_head_cur_x,
	                 sdl_xr_head_cur_y);
}

// Applies a right-controller role picked from the mode wheel:
// role 0=keyboard, 1=mouse, 2=off, 3=second joystick. mouse_mode
// (1=controller, 2=head) picks the mouse flavour when role==1.
static void openxr_apply_right_role(const int role, const int mouse_mode)
{
	sdl_xr_right_chord = role;
	// Tear down the previous role first (all idempotent).
	if (sdl_xr_keyboard_pressed >= 0) {
		const auto& rects = openxr_keyboard_rects();
		if (sdl_xr_keyboard_pressed < static_cast<int>(rects.size())) {
			KEYBOARD_AddKey(rects[sdl_xr_keyboard_pressed].def->code,
			                false);
		}
		sdl_xr_keyboard_pressed = -1;
	}
	openxr_release_mouse_buttons();
	MAPPER_FeedJoystickAxis(0, 2, 0);
	MAPPER_FeedJoystickAxis(0, 3, 0);
	MAPPER_FeedJoystickButton(0, 2, false);
	MAPPER_FeedJoystickButton(0, 3, false);
	// Roles are exclusive.
	sdl_xr_keyboard_visible = (role == 0);
	if (role == 1) {
		sdl_xr_mouse_mode     = (mouse_mode == 2) ? 2 : 1;
		sdl_xr_mouse_has_last = false;
		// Recapture the head baseline when (re)entering head mode.
		sdl_xr_head_ref_valid = false;
	} else {
		sdl_xr_mouse_mode = 0;
	}
	sdl_xr_stick2 = (role == 3);
	if (role == 3) {
		JOYSTICK_Enable(1, true);
		if (!JOYSTICK_IsAccessible(1)) {
			LOG_MSG("OpenXR: second joystick on but stick 2 is not visible to DOS; set joysticktype=2axis (or 4axis)");
			OPENXR_Notify(
			        "XR second joystick needs joysticktype");
		}
	}
	if (role == 0) {
		OPENXR_Notify("Input mode: keyboard");
	} else if (role == 1) {
		OPENXR_Notify(format_str(
		        "Input mode: %s mouse",
		        sdl_xr_mouse_mode == 2 ? "head" : "controller"));
	} else if (role == 3) {
		OPENXR_Notify("Input mode: second joystick");
	} else {
		OPENXR_Notify("Input mode: off");
	}
}

// Keyboard keys currently held by the left-stick WASD / arrow presets,
// so a direction change (or centred stick) can release the old ones.
static int sdl_xr_left_key_x = 0;
static int sdl_xr_left_key_y = 0;

static void openxr_release_left_keys()
{
	if (sdl_xr_left_key_x != 0) {
		KEYBOARD_AddKey(static_cast<KBD_KEYS>(sdl_xr_left_key_x), false);
		sdl_xr_left_key_x = 0;
	}
	if (sdl_xr_left_key_y != 0) {
		KEYBOARD_AddKey(static_cast<KBD_KEYS>(sdl_xr_left_key_y), false);
		sdl_xr_left_key_y = 0;
	}
}

// Applies a left-controller role picked from the mode wheel:
// 0 analog joystick, 1 hat (8-way digital), 2 WASD keys, 3 arrow
// keys, 4 off. In analog/hat modes the left thumbstick drives the axes
// and right A/B are DOS buttons 1/2 (while the left grip is held); in
// the key modes the stick presses W/A/S/D or the arrow keys. DOS can
// only see stick 1 at port 0x201 when [joystick] joysticktype exposes
// it.
static void openxr_apply_left_role(const int role)
{
	sdl_xr_joy_mode = std::clamp(role, 0, 4);
	static const char* const joy_names[] = {
	        "analog", "hat", "WASD keys", "arrow keys", "off"};
	OPENXR_Notify(
	        format_str("XR left stick: %s", joy_names[sdl_xr_joy_mode]));
	// (Re)centre on every transition so no direction sticks.
	MAPPER_FeedJoystickAxis(0, 0, 0);
	MAPPER_FeedJoystickAxis(0, 1, 0);
	MAPPER_FeedJoystickButton(0, 0, false);
	MAPPER_FeedJoystickButton(0, 1, false);
	openxr_release_left_keys();
	if (sdl_xr_joy_mode == 0 || sdl_xr_joy_mode == 1) {
		JOYSTICK_Enable(0, true);
		if (!JOYSTICK_IsAccessible(0)) {
			LOG_MSG("OpenXR: XR joystick on but stick 1 is not visible to DOS; set joysticktype=2axis (or 4axis)");
			OPENXR_Notify(
			        "XR joystick needs joysticktype=2axis");
		}
	}
}

// Radial input-mode wheel (single instance): a thumbstick click opens
// it for that hand, tilting that stick highlights a wedge, A (right) /
// X (left) applies, and B/Y or a second click cancels.
static bool sdl_xr_wheel_is_open   = false;
static int  sdl_xr_wheel_hand      = 1; // active hand (0 left, 1 right)
static int  sdl_xr_wheel_highlight = 0;

static const char* const sdl_xr_wheel_right[] = {
        "Keyboard", "Mouse", "Head", "Joy 2", "Off"};
static const char* const sdl_xr_wheel_left[] = {
        "Analog", "Hat", "WASD", "Arrows", "Off"};

bool openxr_wheel_open()
{
	return sdl_xr_wheel_is_open;
}

int openxr_wheel_active_hand()
{
	return sdl_xr_wheel_hand;
}

int openxr_wheel_highlight()
{
	return sdl_xr_wheel_highlight;
}

// Master switch for feeding the left stick into DOS (analog/hat) and the
// WASD/arrow presets; off leaves the stick inert. Exposed as a VR menu
// toggle.
bool openxr_motion_input()
{
	return get_xr_section()->GetBool("xr_motion_input");
}

const char* const* openxr_wheel_labels(const int hand, int& count_out)
{
	if (hand == 0) {
		count_out = static_cast<int>(
		        sizeof(sdl_xr_wheel_left) /
		        sizeof(sdl_xr_wheel_left[0]));
		return sdl_xr_wheel_left;
	}
	count_out = static_cast<int>(sizeof(sdl_xr_wheel_right) /
	                             sizeof(sdl_xr_wheel_right[0]));
	return sdl_xr_wheel_right;
}

// The wedge matching the controller's current role, so the wheel opens
// on the active selection.
static int openxr_wheel_current_index(const int hand)
{
	if (hand == 0) {
		return std::clamp(sdl_xr_joy_mode, 0, 4);
	}
	if (sdl_xr_keyboard_visible) {
		return 0;
	}
	if (sdl_xr_mouse_mode == 1) {
		return 1;
	}
	if (sdl_xr_mouse_mode == 2) {
		return 2;
	}
	if (sdl_xr_stick2) {
		return 3;
	}
	return 4;
}

static void openxr_wheel_apply(const int hand, const int sel)
{
	if (hand == 0) {
		openxr_apply_left_role(sel);
		return;
	}
	switch (sel) {
	case 0: openxr_apply_right_role(0, 0); break;
	case 1: openxr_apply_right_role(1, 1); break;
	case 2: openxr_apply_right_role(1, 2); break;
	case 3: openxr_apply_right_role(3, 0); break;
	default: openxr_apply_right_role(2, 0); break;
	}
}

void openxr_drive_wheel()
{
	for (int hand = 0; hand < 2; ++hand) {
		auto& h = sdl_xr_hand[hand];
		if (!(h.stick_click_changed && h.stick_click)) {
			continue;
		}
		if (sdl_xr_wheel_is_open && sdl_xr_wheel_hand == hand) {
			sdl_xr_wheel_is_open = false; // second click cancels
		} else if (!sdl_xr_menu_visible) {
			sdl_xr_wheel_is_open   = true;
			sdl_xr_wheel_hand      = hand;
			sdl_xr_wheel_highlight = openxr_wheel_current_index(hand);
		}
	}
	if (!sdl_xr_wheel_is_open) {
		return;
	}
	const int hand = sdl_xr_wheel_hand;

	const float sx = (hand == 0) ? sdl_xr_joy_x : sdl_xr_rjoy_x;
	const float sy = (hand == 0) ? sdl_xr_joy_y : sdl_xr_rjoy_y;

	int count = 0;
	(void)openxr_wheel_labels(hand, count);
	const float mag = std::sqrt(sx * sx + sy * sy);
	if (count > 0 && mag > 0.4f) {
		// OpenXR thumbsticks report +y as up, but the wheel texture
		// is laid out y-down (top wedge at -pi/2), so negate y to
		// keep stick-up selecting the top wedge.
		constexpr float Pi = 3.14159265358979f;
		const float ang    = std::atan2(-sy, sx);
		const float step   = (2.0f * Pi) / static_cast<float>(count);
		const float start  = -Pi * 0.5f;
		int idx = static_cast<int>(
		        std::lround((ang - start) / step));
		idx = ((idx % count) + count) % count;
		sdl_xr_wheel_highlight = idx;
	}

	// A (right) / X (left) applies; B (right) / Y (left) cancels.
	const bool apply  = (hand == 1) ? sdl_xr_joy_btn_pressed[0]
	                                : sdl_xr_joy_xy_pressed[0];
	const bool cancel = (hand == 1) ? sdl_xr_joy_btn_pressed[1]
	                                : sdl_xr_joy_xy_pressed[1];
	if (apply) {
		openxr_wheel_apply(hand, sdl_xr_wheel_highlight);
		sdl_xr_wheel_is_open = false;
	} else if (cancel || sdl_xr_menu_visible) {
		sdl_xr_wheel_is_open = false;
	}
}

// Centres the primary emulated joystick (VR stick 1) and hands the second
// joystick (axes/buttons 2/3) back to the mapper, releasing anything it
// was driving. Used when the stick output is frozen (menu, wheel, presets).
static void openxr_center_joysticks()
{
	MAPPER_FeedJoystickAxis(0, 0, 0);
	MAPPER_FeedJoystickAxis(0, 1, 0);
	MAPPER_FeedJoystickButton(0, 0, false);
	MAPPER_FeedJoystickButton(0, 1, false);
	MAPPER_ReleaseJoystickAxis(0, 2);
	MAPPER_ReleaseJoystickAxis(0, 3);
	MAPPER_ReleaseJoystickButton(0, 2);
	MAPPER_ReleaseJoystickButton(0, 3);
}

// Left thumbstick drives the emulated DOS joystick (stick 1) or presses
// the WASD / arrow keys, once a role is chosen from the wheel. Output
// follows the stick/buttons directly (held = on, released = off) with no
// grip required. Right A/B are DOS buttons 1/2 in the analog/hat roles;
// the right thumbstick and left X/Y drive the second joystick in Joy 2.
void openxr_drive_joystick()
{
	const bool left_wheel =
	        sdl_xr_wheel_is_open && sdl_xr_wheel_hand == 0;

	// Motion input switched off: hand the emulated joysticks back to the
	// mapper so any bound keys/mouse own them again.
	if (!openxr_motion_input()) {
		MAPPER_ReleaseVirtualJoystick(0);
		openxr_release_left_keys();
		return;
	}

	// Frozen while the menu owns both sticks and A/B (navigation,
	// confirm, back) and while the left wheel's stick is selecting a
	// wedge. Centre the main stick and hand the second one back.
	if (sdl_xr_menu_visible || left_wheel) {
		openxr_center_joysticks();
		openxr_release_left_keys();
		return;
	}

	const int mode = sdl_xr_joy_mode;

	if (mode == 2 || mode == 3) {
		// Preset keys: W/A/S/D or the arrow keys, one key per axis.
		openxr_center_joysticks();
		const float sx = sdl_xr_joy_x;
		const float sy = sdl_xr_joy_y;
		int key_x = 0, key_y = 0;
		if (mode == 2) { // WASD
			if (sx >  0.5f) key_x = KBD_d;
			if (sx < -0.5f) key_x = KBD_a;
			if (sy >  0.5f) key_y = KBD_w;
			if (sy < -0.5f) key_y = KBD_s;
		} else { // arrow keys
			if (sx >  0.5f) key_x = KBD_right;
			if (sx < -0.5f) key_x = KBD_left;
			if (sy >  0.5f) key_y = KBD_up;
			if (sy < -0.5f) key_y = KBD_down;
		}
		if (key_x != sdl_xr_left_key_x) {
			if (sdl_xr_left_key_x != 0) {
				KEYBOARD_AddKey(
				        static_cast<KBD_KEYS>(sdl_xr_left_key_x),
				        false);
			}
			if (key_x != 0) {
				KEYBOARD_AddKey(
				        static_cast<KBD_KEYS>(key_x), true);
			}
			sdl_xr_left_key_x = key_x;
		}
		if (key_y != sdl_xr_left_key_y) {
			if (sdl_xr_left_key_y != 0) {
				KEYBOARD_AddKey(
				        static_cast<KBD_KEYS>(sdl_xr_left_key_y),
				        false);
			}
			if (key_y != 0) {
				KEYBOARD_AddKey(
				        static_cast<KBD_KEYS>(key_y), true);
			}
			sdl_xr_left_key_y = key_y;
		}
		return;
	}

	openxr_release_left_keys();

	// Analog (0) / hat (1): drive DOS stick 1 while the stick is
	// deflected. Off (4) centres stick 1 but leaves the second stick
	// active below.
	float x = 0.0f, y = 0.0f;
	if (mode != 4) {
		x = sdl_xr_joy_x;
		y = -sdl_xr_joy_y; // thumbstick up = DOS up
		if (mode == 1) {
			// Hat: snap each axis to full deflection past the
			// threshold.
			x = (x > 0.5f) ? 1.0f : ((x < -0.5f) ? -1.0f : 0.0f);
			y = (y > 0.5f) ? 1.0f : ((y < -0.5f) ? -1.0f : 0.0f);
		}
	}
	MAPPER_FeedJoystickAxis(0, 0, static_cast<int16_t>(x * 32767.0f));
	MAPPER_FeedJoystickAxis(0, 1, static_cast<int16_t>(y * 32767.0f));
	MAPPER_FeedJoystickButton(0, 0, mode != 4 && sdl_xr_joy_btn[0]);
	MAPPER_FeedJoystickButton(0, 1, mode != 4 && sdl_xr_joy_btn[1]);

	if (sdl_xr_stick2) {
		// Second DOS joystick follows the right thumbstick; the left
		// X/Y buttons are its buttons (DOS buttons 3/4 overall). Both
		// hold while pressed, like the main output.
		MAPPER_FeedJoystickAxis(
		        0, 2, static_cast<int16_t>(sdl_xr_rjoy_x * 32767.0f));
		MAPPER_FeedJoystickAxis(
		        0, 3, static_cast<int16_t>(-sdl_xr_rjoy_y * 32767.0f));
		MAPPER_FeedJoystickButton(0, 2, sdl_xr_joy_xy[0]);
		MAPPER_FeedJoystickButton(0, 3, sdl_xr_joy_xy[1]);
	} else {
		// Not in second-joystick mode: hand axes/buttons 2/3 back to
		// the mapper (centres them once on the transition).
		MAPPER_ReleaseJoystickAxis(0, 2);
		MAPPER_ReleaseJoystickAxis(0, 3);
		MAPPER_ReleaseJoystickButton(0, 2);
		MAPPER_ReleaseJoystickButton(0, 3);
	}
}

// Retired CyberPuck controller role (VFX-1 motion is numpad-only):
// kept as a no-op so the per-frame call chain is untouched.
void openxr_drive_puck()
{
}

void xr_controller_mouse_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	// Cycle off -> controller -> head -> off.
	sdl_xr_mouse_mode = (sdl_xr_mouse_mode + 1) % 3;
	if (sdl_xr_mouse_mode == 0) {
		openxr_release_mouse_buttons();
	} else {
		sdl_xr_mouse_has_last = false;
	}
	sdl_xr_head_ref_valid = false; // recapture on head-mode entry
	// Set outside the chord system: the next chord re-applies absolutely.
	sdl_xr_right_chord = -1;
	static const char* const mouse_names[] = {
	        "off (host mouse)",
	        "on: controller aims, triggers click",
	        "on: headset aims, triggers click"};
	OPENXR_Notify(format_str("Mouse control %s",
	                         mouse_names[sdl_xr_mouse_mode]));
}

void xr_ray_toggle_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool shown = !get_xr_section()->GetBool("xr_ray");
	set_section_property_value("xr", "xr_ray", shown ? "on" : "off");
	OPENXR_Notify(format_str("Pointer ray %s", shown ? "on" : "off"));
}

void xr_mirror_stereo_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	sdl_xr_mirror_stereo = !sdl_xr_mirror_stereo;
	OPENXR_Notify(format_str("Mirror: %s",
	                         sdl_xr_mirror_stereo ? "stereo 2D"
	                                              : "normal"));
}

void xr_enabled_toggle_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool on = !get_xr_section()->GetBool("xr_enabled");
	openxr_set_enabled(on);
	OPENXR_Notify(format_str("XR headset %s", on ? "enabled" : "disabled"));
}

void xr_keyboard_toggle_handler(bool pressed)
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
	// Set outside the chord system: the next chord re-applies absolutely.
	sdl_xr_right_chord = -1;
	OPENXR_Notify(format_str("Overlay keyboard %s",
	                         sdl_xr_keyboard_visible ? "shown" : "hidden"));
}

// Host mouse events defer to XR while either emulated mouse drives it.
bool openxr_controller_mouse_active()
{
	return sdl_xr_mouse_mode != 0 && sdl_xr_session_ready;
}

void xr_image_scale_adjust(const bool zoom_in, const bool pressed)
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

void xr_zoom_in_handler(bool pressed)
{
	xr_image_scale_adjust(true, pressed);
}

void xr_zoom_out_handler(bool pressed)
{
	xr_image_scale_adjust(false, pressed);
}

void xr_capture_lock_pose()
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

void xr_recenter_handler(bool pressed)
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

void xr_lock_toggle_handler(bool pressed)
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

void xr_blank_test_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	sdl_xr_submit_blank = !sdl_xr_submit_blank;
	OPENXR_Notify(format_str("Blank-frame submit test %s",
	                         sdl_xr_submit_blank ? "on (HMD goes dark)"
	                                             : "off"));
}

void xr_stereo_mode_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	static const char* const modes[] = {"mono",
	                                          "anaglyph",
	                                          "alternating",
	                                          "cybershades",
	                                          "interleaved",
	                                          "top-bottom",
	                                          "sbs",
	                                          "depth",
	                                          "autodepth",
	                                          "cybermaxx",
	                                          "iglasses"};
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
void xr_stereo_toggle_handler(bool pressed)
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

void xr_autodepth_method_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	static const char* const methods[] = {"blend",
	                                      "gradient",
	                                      "luma"
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

void xr_disparity_adjust(const bool increase, const bool pressed)
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

void xr_disparity_down_handler(bool pressed)
{
	xr_disparity_adjust(false, pressed);
}

void xr_disparity_up_handler(bool pressed)
{
	xr_disparity_adjust(true, pressed);
}

void xr_depth_strength_adjust(const bool increase, const bool pressed)
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

void xr_depth_strength_down_handler(bool pressed)
{
	xr_depth_strength_adjust(false, pressed);
}

void xr_depth_strength_up_handler(bool pressed)
{
	xr_depth_strength_adjust(true, pressed);
}

void xr_divisor_adjust(const bool increase, const bool pressed)
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

void xr_divisor_down_handler(bool pressed)
{
	xr_divisor_adjust(false, pressed);
}

void xr_divisor_up_handler(bool pressed)
{
	xr_divisor_adjust(true, pressed);
}

// Probe: is a boolean action bound to a live source on one hand?
bool openxr_probe_active(XrAction action, const int hand)
{
	if (action == XR_NULL_HANDLE ||
	    sdl_xr_session == XR_NULL_HANDLE || hand < 0 || hand > 1) {
		return false;
	}
	XrActionStateGetInfo get = {};
	get.type           = XR_TYPE_ACTION_STATE_GET_INFO;
	get.action         = action;
	get.subactionPath  = sdl_xr_hand_path[hand];
	XrActionStateBoolean state = {};
	state.type                = XR_TYPE_ACTION_STATE_BOOLEAN;
	return xrGetActionStateBoolean(sdl_xr_session, &get, &state) ==
	        XR_SUCCESS &&
	       state.isActive;
}

void xr_shutter_sync_handler(bool pressed)
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

#endif // C_OPENXR
void xr_dubois_toggle_handler(bool pressed)
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

void xr_eye_swap_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool swapped = !get_xr_section()->GetBool("xr_eye_swap");
	set_section_property_value("xr", "xr_eye_swap", swapped ? "on" : "off");
	OPENXR_Notify(format_str("Eye swap %s", swapped ? "on" : "off"));
}

void xr_flip_vertical_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool flipped = !get_xr_section()->GetBool("xr_flip_vertical");
	set_section_property_value("xr", "xr_flip_vertical", flipped ? "on" : "off");
	OPENXR_Notify(format_str("Vertical flip %s", flipped ? "on" : "off"));
}

void xr_depth_view_handler(bool pressed)
{
	if (!pressed) {
		return;
	}
	const bool shown = !get_xr_section()->GetBool("xr_show_depth");
	set_section_property_value("xr", "xr_show_depth", shown ? "on" : "off");
	OPENXR_Notify(format_str("Depth buffer view %s", shown ? "on" : "off"));
}
