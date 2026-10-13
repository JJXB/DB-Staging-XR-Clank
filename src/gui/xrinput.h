// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// Motion-controller and headset input: OpenXR action sets, bindings,
// per-frame polling, controller/trigger mouse, head-gyro mouse,
// thumbstick joystick + menu navigation, chord mapper handlers, and
// shared ray math. Feeds HUD (osd) and the DOS inputs.

#ifndef DOSBOX_XRINPUT_H
#define DOSBOX_XRINPUT_H

#if C_OPENXR
#include <openxr/openxr.h>

// Per-hand controller tracking state (owned here, read by HUD hover
// and frame submission).
struct XrHandState {
	bool     pose_valid       = false;
	XrPosef  pose             = {};
	XrResult aim_result       = XR_SUCCESS; // last xrLocateSpace result
	XrSpaceLocationFlags aim_flags = 0; // last location flags
	bool     click            = false;
	bool     click_changed    = false;
	bool     click_ui         = false; // firmer UI-grade press (menu/keys)
	bool     click_ui_changed = false;
	bool     stick_click      = false; // right thumbstick click held
	bool     stick_click_changed = false; // right stick click rising edge
	bool     menu             = false; // menu button held
	bool     menu_changed     = false; // menu button rising edge
	float    grip             = 0.0f;
	bool     grip_pressed     = false;
	bool     grip_was_pressed = false;
};
extern XrHandState sdl_xr_hand[2];

// 3D math shared with HUD ray ribbons.
struct XrVec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};
XrVec3         xr_quat_rotate(const XrQuaternionf& q, const XrVec3& v);
XrQuaternionf  xr_quat_conj(const XrQuaternionf& q);
XrQuaternionf  xr_quat_mul(const XrQuaternionf& a, const XrQuaternionf& b);
float          xr_dot(const XrVec3& a, const XrVec3& b);
XrVec3         xr_sub(const XrVec3& a, const XrVec3& b);
XrVec3         xr_cross(const XrVec3& a, const XrVec3& b);
float          xr_len(const XrVec3& v);
XrVec3         xr_norm(const XrVec3& v);
XrQuaternionf  xr_mat3_to_quat(const float m[9]);

// Controller ray queries (used by HUD hover/hit-testing).
bool openxr_ray_plane_hit(const XrVec3& o, const XrVec3& dir,
                          const XrVec3& center, const XrVec3& fwd,
                          const XrVec3& right, const XrVec3& up,
                          const float half_w, const float half_h,
                          XrVec3& from_out, XrVec3& dir_out,
                          XrVec3& hit_out, float& u_out, float& v_out);
bool openxr_hand_ray(const int hand, XrVec3& from_out, XrVec3& dir_out,
                     XrVec3& hit_out, float& u_out, float& v_out);
bool openxr_hand_plane_uv(const int hand, float& u_out, float& v_out);

// Probe helper (used by HUD visibility logic).
bool openxr_probe_active(XrAction action, const int hand);

// Per-frame input drivers (called from openxr_frame_begin).
bool openxr_create_input_actions();
bool openxr_attach_input();
void openxr_update_input(const XrTime display_time);
void openxr_drive_input_mode();
void openxr_drive_joystick();
void openxr_drive_mouse();
void openxr_drive_head_mouse();
void openxr_drive_puck();
void openxr_release_mouse_buttons();
bool openxr_controller_mouse_active();

// Mapper chord handlers (bound in add_default_sdl_section_mapper_bindings).
void xr_zoom_in_handler(bool pressed);
void xr_zoom_out_handler(bool pressed);
void xr_recenter_handler(bool pressed);
void xr_lock_toggle_handler(bool pressed);
void xr_blank_test_handler(bool pressed);
void xr_stereo_mode_handler(bool pressed);
void xr_stereo_toggle_handler(bool pressed);
void xr_disparity_down_handler(bool pressed);
void xr_disparity_up_handler(bool pressed);
void xr_depth_strength_down_handler(bool pressed);
void xr_depth_strength_up_handler(bool pressed);
void xr_dubois_toggle_handler(bool pressed);
void xr_shutter_sync_handler(bool pressed);
void xr_eye_swap_handler(bool pressed);
void xr_flip_vertical_handler(bool pressed);
void xr_depth_view_handler(bool pressed);
void xr_autodepth_method_handler(bool pressed);
void xr_neural_model_handler(bool pressed);
void xr_divisor_down_handler(bool pressed);
void xr_divisor_up_handler(bool pressed);
void xr_controller_mouse_handler(bool pressed);
void xr_keyboard_toggle_handler(bool pressed);
void xr_ray_toggle_handler(bool pressed);
void xr_mirror_stereo_handler(bool pressed);
// VFX-1 keyboard testing inputs (momentary mapper hotkeys; see the
// sdl_xr_test_* state above).
void xr_test_hmd_yaw_minus_handler(bool pressed);
void xr_test_hmd_yaw_plus_handler(bool pressed);
void xr_test_hmd_pitch_minus_handler(bool pressed);
void xr_test_hmd_pitch_plus_handler(bool pressed);
void xr_test_hmd_roll_minus_handler(bool pressed);
void xr_test_hmd_roll_plus_handler(bool pressed);
void xr_test_puck_x_minus_handler(bool pressed);
void xr_test_puck_x_plus_handler(bool pressed);
void xr_test_puck_y_minus_handler(bool pressed);
void xr_test_puck_y_plus_handler(bool pressed);
void xr_test_puck_top_handler(bool pressed);
void xr_test_puck_middle_handler(bool pressed);
void xr_test_puck_bottom_handler(bool pressed);

// Small setting/mode adjusters shared with the VR menu rows.
void xr_image_scale_adjust(const bool zoom_in, const bool pressed);
void xr_disparity_adjust(const bool increase, const bool pressed);
void xr_depth_strength_adjust(const bool increase, const bool pressed);
void xr_divisor_adjust(const bool increase, const bool pressed);
void xr_capture_lock_pose();

// Input-owned state read across modules.
extern int sdl_xr_right_chord;
extern float sdl_xr_joy_x;
extern float sdl_xr_joy_y;
extern XrActionSet sdl_xr_action_set;
extern XrAction    sdl_xr_menu_action;
extern XrAction    sdl_xr_aim_action;
extern XrAction    sdl_xr_click_action;
extern XrAction    sdl_xr_grip_action;
extern XrAction    sdl_xr_trigvalue_action;
extern XrAction    sdl_xr_stick_action;
extern XrAction    sdl_xr_rstick_action;
extern XrAction    sdl_xr_joya_action;
extern XrAction    sdl_xr_joyb_action;
extern XrAction    sdl_xr_joyx_action;
extern XrAction    sdl_xr_joyy_action;
extern XrAction    sdl_xr_stickclick_action;
extern XrSpace     sdl_xr_aim_space[2];
extern XrPath      sdl_xr_hand_path[2];
extern bool        sdl_xr_actions_attached;
extern bool        sdl_xr_mirror_stereo;
extern float       sdl_xr_rjoy_x;
extern float       sdl_xr_rjoy_y;
extern bool        sdl_xr_joy_btn[2];
extern bool        sdl_xr_joy_btn_pressed[2];
extern bool        sdl_xr_joy_xy[2];
extern bool        sdl_xr_joy_xy_pressed[2];
extern int         sdl_xr_joy_mode;
extern int         sdl_xr_mouse_mode;
// CyberPuck live state (right-hand cyberpuck role feeds the VFX1 bus):
// RTrigger = top, B = middle, A = bottom; right stick drives the axes.
extern bool        sdl_xr_puck_active;
extern float       sdl_xr_puck_x; // -1..1 (right stick X)
extern float       sdl_xr_puck_y; // -1..1 (right stick Y)
extern unsigned int sdl_xr_puck_btn; // bit0=top, bit1=middle, bit2=bottom
// Keyboard testing inputs (mapper hotkeys, momentary): per-axis
// overrides (-1/0/+1) for the HMD yaw/pitch/roll and puck axes, plus
// puck Top/Middle/Bottom bits. Combine in the VFX1 synthesis; work
// with or without the cyberpuck role.
extern float       sdl_xr_test_hmd[3]; // yaw, pitch, roll
extern float       sdl_xr_test_puck[2]; // puck X, Y
extern unsigned int sdl_xr_test_puckbtn; // bit0=top, bit1=middle, bit2=bottom

#endif // C_OPENXR

#endif
