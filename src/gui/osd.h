// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// In-headset HUD: VR menu, on-screen display (OSD) toasts, virtual
// keyboard, controller ray ribbons, desktop-OSD compositing, and XR
// preset (xrcfg) save/load. Driven by xrinput state, submitted by the
// frame loop in xr.cpp.

#ifndef DOSBOX_OSD_H
#define DOSBOX_OSD_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "config/setup.h"
#include "hardware/input/keyboard.h"

#if C_OPENXR
#include <openxr/openxr.h>

#include "gui/render/opengl_renderer.h"
#include "gui/xr.h"

// [osd] section accessor (the section always exists; values are live).
SectionProp* get_osd_section();

// [osd] config registration (called from GFX_AddConfigSection).
void init_osd_config_settings(SectionProp& section);

// HUD presentation constants (texture sizes, geometry, timeouts).
static constexpr int     OpenXrKeyboardTexW = 1000;
static constexpr int     OpenXrKeyboardTexH = 500;
static constexpr int     OpenXrOsdTexW = 1024;
static constexpr int     OpenXrOsdTexH = 256;
static constexpr int64_t OpenXrOsdTimeoutUs = 4'000'000; // 4 s
static constexpr int     OpenXrMenuTexW = 1536;
static constexpr int     OpenXrMenuTexH = 1024;
static constexpr float   OpenXrMenuAspect = 1.5f; // width / height
static constexpr int     OpenXrRayTexW = 128;
static constexpr int     OpenXrRayTexH = 16;
static constexpr int     OpenXrWheelTexW = 512;
static constexpr int     OpenXrWheelTexH = 512;
static constexpr float   OpenXrWheelSizeM = 0.40f; // quad edge, metres
static constexpr float   OpenXrWheelDistM = 0.22f; // in front of the palm
static constexpr float   OpenXrRayWidthM  = 0.01f; // ribbon width
static constexpr float   OpenXrRayMaxLenM = 8.0f; // clamp + blind length
static constexpr float   OpenXrMenuUpHalfH   = 0.55f;
static constexpr float   OpenXrMenuSizeHalfH = 1.1f; // full width/height
static constexpr float   OpenXrKeyboardBandTop = 0.45f;
static constexpr int     OpenXrMenuMaxNameLen   = 24;
static constexpr float   OpenXrMenuButtonZone = 0.7f; // right part = SAVE/LOAD

// Notification overlay line (owned here, read by submission).
struct XrOsdLine {
	std::string text;
	int64_t     time_us = 0;
};
extern std::deque<XrOsdLine> sdl_xr_osd;

// Overlay keyboard geometry (owned here, queried by input mouse drive).
struct XrKeyDef {
	const char* label;
	KBD_KEYS    code;
	uint8_t     row;
	uint8_t     units;
};
struct XrKeyRect {
	const XrKeyDef* def;
	float           x0, x1, v0, v1;
};
const std::vector<XrKeyRect>& openxr_keyboard_rects();

// HUD-owned state read across modules.
extern bool sdl_xr_menu_visible;
extern bool sdl_xr_keyboard_visible;
extern int  sdl_xr_keyboard_pressed;
extern bool sdl_xr_menu_claim_click;
extern bool sdl_xr_menu_field_focus;

// Layer swapchains/images/epochs (owned here, designated + torn down
// here, read by frame submission).
extern XrSwapchain sdl_xr_kbd_swapchain;
extern std::vector<XrGfxImage> sdl_xr_kbd_images;
extern bool        sdl_xr_kbd_filled;
extern GLuint      sdl_xr_kbd_tex;
extern uint64_t    sdl_xr_kbd_tex_epoch;
extern std::vector<uint64_t> sdl_xr_kbd_img_epoch;
extern int         sdl_xr_kbd_hover_built;
extern int         sdl_xr_kbd_pressed_built;
extern float       sdl_xr_kbd_opacity_built;
extern int         sdl_xr_kbd_tex_w;
extern int         sdl_xr_kbd_tex_h;
extern XrSwapchain sdl_xr_osd_swapchain;
extern std::vector<XrGfxImage> sdl_xr_osd_images;
extern GLuint      sdl_xr_osd_tex;
extern uint64_t    sdl_xr_osd_tex_epoch;
extern std::vector<uint64_t> sdl_xr_osd_img_epoch;
extern float       sdl_xr_osd_opacity_built;
extern std::string sdl_xr_osd_desk_sig;
extern bool        sdl_xr_osd_desk_touched;
extern bool        sdl_xr_osd_dirty;
extern XrSwapchain sdl_xr_menu_swapchain;
extern std::vector<XrGfxImage> sdl_xr_menu_images;
extern bool        sdl_xr_menu_filled;
extern GLuint      sdl_xr_menu_tex;
extern uint64_t    sdl_xr_menu_tex_epoch;
extern std::vector<uint64_t> sdl_xr_menu_img_epoch;
extern std::string sdl_xr_menu_built_sig;
extern XrSwapchain sdl_xr_ray_swapchain;
extern std::vector<XrGfxImage> sdl_xr_ray_images;
extern bool        sdl_xr_ray_filled;
extern GLuint      sdl_xr_ray_tex;
extern uint64_t    sdl_xr_ray_tex_epoch;
extern std::vector<uint64_t> sdl_xr_ray_img_epoch;
extern XrSwapchain sdl_xr_wheel_swapchain;
extern std::vector<XrGfxImage> sdl_xr_wheel_images;
extern GLuint      sdl_xr_wheel_tex;
extern uint64_t    sdl_xr_wheel_tex_epoch;
extern std::vector<uint64_t> sdl_xr_wheel_img_epoch;
extern std::string sdl_xr_wheel_built_sig;

// Toast/message overlay.
void OPENXR_Notify(const std::string& message);

// Per-frame HUD drivers (called from openxr_frame_begin).
void openxr_drive_menu();
void openxr_drive_keyboard();

// Layer setup/refresh (called from openxr_submit_frame).
bool   openxr_setup_menu_layer();
bool   openxr_setup_keyboard_layer();
bool   openxr_setup_osd_layer();
bool   openxr_setup_ray_layer();
bool   openxr_setup_wheel_layer();
GLuint openxr_refresh_menu_texture();
GLuint openxr_refresh_keyboard_texture(const int hover_index,
                                       const int pressed_index);
GLuint openxr_refresh_osd_texture(const float opacity);
GLuint openxr_refresh_ray_texture();
GLuint openxr_refresh_wheel_texture(const int hand, const int highlight);
// Selection signature of the wheel texture (hand + highlighted wedge);
// changes trigger a rebuild in the submit path.
std::string openxr_wheel_signature(const int hand, const int highlight);
// Retained CPU rasters for the Vulkan upload path (rebuilt by the
// matching refresh call above when content changes; empty when
// nothing was built yet). Same bytes the GL path uploads as textures.
const std::vector<uint32_t>& openxr_kbd_cpu_raster(int& w_out, int& h_out);
const std::vector<uint32_t>& openxr_osd_cpu_raster(int& w_out, int& h_out);
const std::vector<uint32_t>& openxr_menu_cpu_raster(int& w_out, int& h_out);
const std::vector<uint32_t>& openxr_ray_cpu_raster(int& w_out, int& h_out);
const std::vector<uint32_t>& openxr_wheel_cpu_raster(int& w_out, int& h_out);
bool   openxr_ray_ribbon(const int hand, XrPosef& pose_out,
                          float& len_out);

// OSD helpers (called from frame submission and the mirror path).
void  openxr_osd_prune();
float openxr_osd_opacity();
bool  openxr_desk_osd_pending();
bool  openxr_composite_desk_osd();

// Keyboard display parameters (called from submission).
float openxr_keyboard_key_scale();
void  openxr_keyboard_tex_size(int& w_out, int& h_out);
float openxr_keyboard_opacity();

// Menu state + row actions (called from input handlers and sticks).
void openxr_menu_set_visible(const bool show);
void openxr_menu_build_rows();

// Desktop mirror-window toasts (osd_desk) are composited into the
// window backbuffer after the XR blit; their drivers live in osd.cpp.
int  openxr_keyboard_hover();
std::string openxr_menu_signature();
// Requested section-grid column count for the menu (0 = default, i.e.
// 2 columns unless a single section; 1..3 forces that count). Read live
// from [xr] xr_menu_columns so the raster and hit-test always agree.
int openxr_menu_columns();
void openxr_menu_layout(const float half_h, float& width_out,
                        float& height_out, float& up_out, float& vc_out,
                        float& vhalf_out);
void openxr_menu_type_key();
void openxr_menu_cycle_mode(const int dir);
void openxr_menu_cycle_autodepth(const int dir);
void openxr_menu_adjust_distance(const char* sec_name,
                                   const char* opt_name,
                                   const int delta);
void openxr_menu_adjust_picture(const int delta);
void openxr_menu_toggle_lock();

// HUD layer teardown (swapchains/images/flags, called from
// openxr_teardown_session).
void osd_teardown_layers();

#endif // C_OPENXR

#endif
